// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintStrokePipeline.h"

#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "VoxelPaintSessionBudget.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintSessionCpuState.h"
#include "VoxelPaintSessionResources.h"
#include "VoxelPaintSessionTransfers.h"
#include "VoxelPaintStrokeApplyShader.h"
#include "VoxelPaintStrokeJournal.h"
#include "WorldVoxelPaintConstants.h"

#define LOCTEXT_NAMESPACE "WorldVoxelPaint"

DECLARE_GPU_STAT_NAMED(VoxelPaintStroke, TEXT("VoxelPaint Stroke"));

namespace StrokePipelinePrivate
{
	constexpr int32 MaskWords = (WorldVoxelPaint::TexelsPerUniqueBrick + 63) / 64;

	struct FStrokeApplyDispatchArgs
	{
		uint32 Op = 0;
		uint32 WorkCount = 0;
		FVector3f BrushCenterTexel = FVector3f::ZeroVector;
		float BrushRadiusTexels = 0.0f;
		float Strength = 1.0f;
		float FalloffHotspot = 0.5f;
		FLinearColor Color = FLinearColor::White;
		uint32 ChannelMask = 0xF;
		uint32 SwapChannels = 0;
		FIntVector Resolution = FIntVector::ZeroValue;
		FIntVector CopyMin = FIntVector::ZeroValue;
		FIntVector CopyMax = FIntVector::ZeroValue;
	};

	static bool ShouldPublishMidStroke()
	{
		const IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(
			TEXT("r.VoxelPaint.Editor.MidStrokePublication"));
		return !CVar || CVar->GetInt() != 0;
	}

	static bool ComputeNonZeroWriteIntent(EVoxelPaintStrokeOp Op, const FVoxelPaintBrushParams& Brush)
	{
		switch (Op)
		{
		case EVoxelPaintStrokeOp::Invert:
		case EVoxelPaintStrokeOp::Swap:
			return true;
		case EVoxelPaintStrokeOp::Erase:
			return false;
		case EVoxelPaintStrokeOp::Paint:
		case EVoxelPaintStrokeOp::Flood:
		default:
			break;
		}
		if (Brush.Strength <= 0.0f || Brush.ChannelMask == 0)
		{
			return false;
		}
		FLinearColor Color = Brush.Color;
		for (int32 Channel = 0; Channel < 4; ++Channel)
		{
			if ((Brush.ChannelMask & (1u << Channel)) != 0 && Color.Component(Channel) > 0.0f)
			{
				return true;
			}
		}
		return false;
	}

	static int64 EstimateDeltaBytes(int64 NumBricks)
	{
		const int64 PerBrick = (int64)sizeof(WorldVoxelPaint::FVoxelPaintBrickDelta)
			+ (int64)MaskWords * sizeof(uint64)
			+ 2ll * WorldVoxelPaint::TexelsPerUniqueBrick * sizeof(uint32);
		return (int64)sizeof(WorldVoxelPaint::FVoxelPaintStrokeDelta) + NumBricks * PerBrick;
	}

	static int64 EstimateStampFootprintBricks(const FVoxelPaintBrushParams& Brush,
		const FVector& TexelSize, const FIntVector& BrickCounts)
	{
		int64 Product = 1;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double BrickSpan = WorldVoxelPaint::UniqueBrickSize * TexelSize[Axis];
			const int64 SpanBricks = (int64)(2.0 * Brush.RadiusWorld / FMath::Max(BrickSpan, UE_DOUBLE_SMALL_NUMBER)) + 2;
			Product *= FMath::Clamp<int64>(SpanBricks, 1, BrickCounts[Axis]);
		}
		return Product;
	}

	static int64 EstimateStampFootprintTexels(const FVoxelPaintBrushParams& Brush,
		const FVector& TexelSize, const FIntVector& Resolution)
	{
		int64 Product = 1;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const int64 SpanTexels = (int64)(2.0 * Brush.RadiusWorld
				/ FMath::Max(TexelSize[Axis], UE_DOUBLE_SMALL_NUMBER)) + 2;
			Product *= FMath::Clamp<int64>(SpanTexels, 1, Resolution[Axis]);
		}
		return Product;
	}
}

void FVoxelPaintStrokePipeline::BuildWholeVolumeGatherList(const FIntVector& BrickCounts, FVoxelPaintGatherList& Out)
{
	Out = FVoxelPaintGatherList();
	if (BrickCounts.GetMin() <= 0)
	{
		return;
	}
	const FIntVector Resolution = BrickCounts * WorldVoxelPaint::UniqueBrickSize;
	const int64 NumTexels = (int64)Resolution.X * Resolution.Y * Resolution.Z;
	const int64 NumBricks = (int64)BrickCounts.X * BrickCounts.Y * BrickCounts.Z;
	checkf(NumTexels <= MAX_int32, TEXT("Whole-volume gather list too large: %lld texels"), NumTexels);

	Out.TexelIndices.SetNumUninitialized((int32)NumTexels);
	for (int32 Index = 0; Index < (int32)NumTexels; ++Index)
	{
		Out.TexelIndices[Index] = (uint32)Index;
	}
	Out.DirtyMin = FIntVector::ZeroValue;
	Out.DirtyMax = Resolution - FIntVector(1);
	Out.DirtyBricks.SetNumUninitialized((int32)NumBricks);
	for (int32 Index = 0; Index < (int32)NumBricks; ++Index)
	{
		Out.DirtyBricks[Index] = (uint32)Index;
	}
	Out.BrushCenterTexel = (FVector3f(Resolution) - FVector3f(1.0f)) * 0.5f;
	Out.BrushRadiusTexels = 0.0f;
}

void FVoxelPaintStrokePipeline::BindSession(const FVoxelPaintStrokePipelineSessionBindings& InBindings)
{
	if (Bindings.Controller && Bindings.Controller != InBindings.Controller)
	{
		Bindings.Controller->NotifyStrokePipelineUnbound(this);
	}
	Bindings = InBindings;
	StrokeDirtyBricks.Reset();
	ReservedStrokeBricks = 0;
	bStrokeDirtyValid = false;
	bStrokeOpen = false;
	bCloseResolved = true;
	bStrokeGrowthDeniedLogged = false;
	if (Bindings.Controller)
	{
		Bindings.Controller->NotifyStrokePipelineBound(this);
	}
}

void FVoxelPaintStrokePipeline::UnbindSession()
{
	if (Bindings.Controller)
	{
		Bindings.Controller->NotifyStrokePipelineUnbound(this);
	}
	Bindings = FVoxelPaintStrokePipelineSessionBindings();
	StrokeDirtyBricks.Reset();
	ReservedStrokeBricks = 0;
	bStrokeDirtyValid = false;
	bStrokeOpen = false;
	bCloseResolved = true;
	bStrokeGrowthDeniedLogged = false;
}

bool FVoxelPaintStrokePipeline::BeginStroke(EVoxelPaintStrokeOp Op, const FVoxelPaintBrushParams& Brush, FText& OutWhyNot)
{
	using namespace StrokePipelinePrivate;

	if (!Bindings.IsValid())
	{
		OutWhyNot = LOCTEXT("StrokeNoSession", "No paint session is bound.");
		return false;
	}
	if (bStrokeOpen)
	{
		OutWhyNot = LOCTEXT("StrokeAlreadyOpen", "A stroke is already open.");
		return false;
	}
	if (!bCloseResolved)
	{
		OutWhyNot = LOCTEXT("StrokeCloseUnresolved", "The previous stroke is still committing.");
		return false;
	}
	if (Op == EVoxelPaintStrokeOp::Swap)
	{
		const bool bChannelsValid = Brush.SwapSrcChannel < 4 && Brush.SwapDstChannel < 4
			&& (Brush.ChannelMask & (1u << Brush.SwapSrcChannel)) != 0
			&& (Brush.ChannelMask & (1u << Brush.SwapDstChannel)) != 0;
		if (!bChannelsValid)
		{
			OutWhyNot = LOCTEXT("StrokeSwapUnmasked", "Channel swap requires both swapped channels in the channel mask.");
			return false;
		}
	}

	const bool bWholeVolume = Op == EVoxelPaintStrokeOp::Flood
		|| Op == EVoxelPaintStrokeOp::Invert || Op == EVoxelPaintStrokeOp::Swap;
	const FIntVector Resolution = Bindings.BrickCounts * WorldVoxelPaint::UniqueBrickSize;
	const int64 NumBricks = (int64)Bindings.BrickCounts.X * Bindings.BrickCounts.Y * Bindings.BrickCounts.Z;
	const int64 NumTexels = (int64)Resolution.X * Resolution.Y * Resolution.Z;

	FVoxelPaintMutationDesc Desc;
	Desc.bNonZeroWriteIntent = ComputeNonZeroWriteIntent(Op, Brush);
	int64 ReserveBricks = 0;
	if (bWholeVolume)
	{
		ReserveBricks = NumBricks;
		Desc.JournalReserveBytes = EstimateDeltaBytes(NumBricks);
		Desc.GpuStagingBytes = NumTexels * (int64)sizeof(uint32);
	}
	else
	{
		ReserveBricks = EstimateStampFootprintBricks(Brush, Bindings.TexelSize, Bindings.BrickCounts);
		Desc.JournalReserveBytes = EstimateDeltaBytes(ReserveBricks);
		Desc.GpuStagingBytes = FMath::Min<int64>(
			EstimateStampFootprintTexels(Brush, Bindings.TexelSize, Resolution),
			FVoxelPaintCpuPickProvider::MaxVisibilityCandidates) * (int64)sizeof(uint32);
	}

	if (!Bindings.Controller->BeginMutation(Desc, OutWhyNot))
	{
		return false;
	}

	StrokeOp = Op;
	StrokeBrush = Brush;
	StrokeDirtyBricks.Reset();
	ReservedStrokeBricks = ReserveBricks;
	bStrokeDirtyValid = false;
	bStrokeOpen = true;
	bStrokeGrowthDeniedLogged = false;
	return true;
}

void FVoxelPaintStrokePipeline::ApplyStamp(FVoxelPaintGatherList&& Gather)
{
	using namespace StrokePipelinePrivate;

	if (!bStrokeOpen || !Bindings.IsValid() || Gather.IsEmpty())
	{
		return;
	}

	int64 NewBricks = 0;
	{
		int32 ExistingIndex = 0;
		for (const uint32 Incoming : Gather.DirtyBricks)
		{
			while (ExistingIndex < StrokeDirtyBricks.Num() && StrokeDirtyBricks[ExistingIndex] < Incoming)
			{
				++ExistingIndex;
			}
			if (ExistingIndex >= StrokeDirtyBricks.Num() || StrokeDirtyBricks[ExistingIndex] != Incoming)
			{
				++NewBricks;
			}
		}
	}
	const int64 ProspectiveBricks = (int64)StrokeDirtyBricks.Num() + NewBricks;
	if (ProspectiveBricks > ReservedStrokeBricks)
	{
		const int64 GrowBytes =
			EstimateDeltaBytes(ProspectiveBricks) - EstimateDeltaBytes(ReservedStrokeBricks);
		if (!Bindings.Controller->GrowOpenStrokeReservation(GrowBytes))
		{
			if (!bStrokeGrowthDeniedLogged)
			{
				bStrokeGrowthDeniedLogged = true;
				UE_LOG(LogWorldVoxelPaint, Warning,
					TEXT("Stroke journal reservation cannot grow for this stamp; further stamps of this stroke are rejected unchanged."));
			}
			return;
		}
		ReservedStrokeBricks = ProspectiveBricks;
	}

	AccumulateStrokeDirty(Gather);
	EnqueueApplyDispatch(MoveTemp(Gather), ShouldPublishMidStroke());
}

void FVoxelPaintStrokePipeline::RequestCloseStroke()
{
	if (!bStrokeOpen)
	{
		return;
	}
	bStrokeOpen = false;
	if (!Bindings.IsValid())
	{
		return;
	}
	if (!bStrokeDirtyValid || StrokeDirtyBricks.IsEmpty())
	{
		Bindings.Controller->CancelOpenStroke();
		return;
	}

	bCloseResolved = false;
	TArray<uint32> Bricks = StrokeDirtyBricks;
	Bindings.Transfers->ReadbackDirtyBricks(*Bindings.Resources, MoveTemp(Bricks), *Bindings.Ledger,
		[this](bool bOk, WorldVoxelPaint::FVoxelPaintPackedBrickTransfer&& Readback)
		{
			OnCloseReadback(bOk, MoveTemp(Readback));
		});
}

void FVoxelPaintStrokePipeline::CancelStroke()
{
	if (!bStrokeOpen)
	{
		return;
	}
	bStrokeOpen = false;
	if (!Bindings.IsValid())
	{
		return;
	}
	if (bStrokeDirtyValid && !StrokeDirtyBricks.IsEmpty())
	{
		RestoreStrokeRegionFromCpu();
	}
	Bindings.Controller->CancelOpenStroke();
}

bool FVoxelPaintStrokePipeline::IsCloseResolved() const
{
	return bCloseResolved;
}

bool FVoxelPaintStrokePipeline::IsStrokeOpen() const
{
	return bStrokeOpen;
}

void FVoxelPaintStrokePipeline::EnqueueApplyDispatch(FVoxelPaintGatherList&& Gather, bool bMidStrokePublication)
{
	using namespace StrokePipelinePrivate;

	FStrokeApplyDispatchArgs Args;
	Args.Op = (uint32)StrokeOp;
	Args.WorkCount = (uint32)Gather.TexelIndices.Num();
	Args.BrushCenterTexel = Gather.BrushCenterTexel;
	Args.BrushRadiusTexels = Gather.BrushRadiusTexels;
	Args.Strength = StrokeBrush.Strength;
	Args.FalloffHotspot = StrokeBrush.FalloffHotspot;
	Args.Color = StrokeBrush.Color;
	Args.ChannelMask = StrokeBrush.ChannelMask;
	Args.SwapChannels = (uint32)StrokeBrush.SwapSrcChannel | ((uint32)StrokeBrush.SwapDstChannel << 8);
	Args.Resolution = Bindings.BrickCounts * WorldVoxelPaint::UniqueBrickSize;
	Args.CopyMin = Gather.DirtyMin;
	Args.CopyMax = Gather.DirtyMax;

	FVoxelPaintSessionResources* Resources = Bindings.Resources;
	ENQUEUE_RENDER_COMMAND(WorldVoxelPaintStrokeApply)(
		[Resources, Args, TexelIndices = MoveTemp(Gather.TexelIndices), bMidStrokePublication](FRHICommandListImmediate& RHICmdList) mutable
		{
			const FTextureRHIRef WorkingRHI = Resources->GetWorkingRHI();
			const FTextureRHIRef PublishedRHI = Resources->GetPublishedRHI();
			if (!WorkingRHI.IsValid() || (bMidStrokePublication && !PublishedRHI.IsValid()))
			{
				return;
			}

			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("WorldVoxelPaint.StrokeApply"));
			{
				RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelPaintStroke, "WorldVoxelPaint.StrokeApply");

				FRDGTextureRef Working = RegisterExternalTexture(GraphBuilder, WorkingRHI,
					TEXT("WorldVoxelPaint.SessionWorking"));
				FRDGBufferRef WorkList = CreateStructuredBuffer(GraphBuilder,
					TEXT("WorldVoxelPaint.StrokeWorkList"), MoveTemp(TexelIndices));

				FVoxelPaintStrokeApplyCS::FParameters* PassParameters =
					GraphBuilder.AllocParameters<FVoxelPaintStrokeApplyCS::FParameters>();
				PassParameters->WorkList = GraphBuilder.CreateSRV(WorkList);
				PassParameters->RWWorking = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(Working));
				PassParameters->Op = Args.Op;
				PassParameters->WorkCount = Args.WorkCount;
				PassParameters->BrushCenterTexel = Args.BrushCenterTexel;
				PassParameters->BrushRadiusTexels = Args.BrushRadiusTexels;
				PassParameters->Strength = Args.Strength;
				PassParameters->FalloffHotspot = Args.FalloffHotspot;
				PassParameters->Color = Args.Color;
				PassParameters->ChannelMask = Args.ChannelMask;
				PassParameters->SwapChannels = Args.SwapChannels;
				PassParameters->Resolution = Args.Resolution;

				TShaderMapRef<FVoxelPaintStrokeApplyCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder,
					RDG_EVENT_NAME("WorldVoxelPaint.StrokeApply(%u)", Args.WorkCount),
					ComputeShader, PassParameters,
					FComputeShaderUtils::GetGroupCountWrapped((int32)Args.WorkCount, FVoxelPaintStrokeApplyCS::ThreadGroupSize));

				if (bMidStrokePublication)
				{
					FRDGTextureRef Published = RegisterExternalTexture(GraphBuilder, PublishedRHI,
						TEXT("WorldVoxelPaint.SessionPublished"));
					FRHICopyTextureInfo CopyInfo;
					CopyInfo.SourcePosition = Args.CopyMin;
					CopyInfo.DestPosition = Args.CopyMin;
					CopyInfo.Size = Args.CopyMax - Args.CopyMin + FIntVector(1);
					AddCopyTexturePass(GraphBuilder, Working, Published, CopyInfo);
				}
			}

			GraphBuilder.Execute();
		});
}

void FVoxelPaintStrokePipeline::AccumulateStrokeDirty(const FVoxelPaintGatherList& Gather)
{
	if (bStrokeDirtyValid)
	{
		StrokeDirtyMin = StrokeDirtyMin.ComponentMin(Gather.DirtyMin);
		StrokeDirtyMax = StrokeDirtyMax.ComponentMax(Gather.DirtyMax);
	}
	else
	{
		StrokeDirtyMin = Gather.DirtyMin;
		StrokeDirtyMax = Gather.DirtyMax;
		bStrokeDirtyValid = true;
	}

	TArray<uint32> Merged;
	Merged.Reserve(StrokeDirtyBricks.Num() + Gather.DirtyBricks.Num());
	int32 ExistingIndex = 0;
	int32 IncomingIndex = 0;
	while (ExistingIndex < StrokeDirtyBricks.Num() || IncomingIndex < Gather.DirtyBricks.Num())
	{
		uint32 Next;
		if (IncomingIndex >= Gather.DirtyBricks.Num())
		{
			Next = StrokeDirtyBricks[ExistingIndex++];
		}
		else if (ExistingIndex >= StrokeDirtyBricks.Num())
		{
			Next = Gather.DirtyBricks[IncomingIndex++];
		}
		else if (StrokeDirtyBricks[ExistingIndex] <= Gather.DirtyBricks[IncomingIndex])
		{
			Next = StrokeDirtyBricks[ExistingIndex++];
		}
		else
		{
			Next = Gather.DirtyBricks[IncomingIndex++];
		}
		if (Merged.IsEmpty() || Merged.Last() != Next)
		{
			Merged.Add(Next);
		}
	}
	StrokeDirtyBricks = MoveTemp(Merged);
}

void FVoxelPaintStrokePipeline::RestoreStrokeRegionFromCpu()
{
	if (!Bindings.IsValid() || !bStrokeDirtyValid || StrokeDirtyBricks.IsEmpty())
	{
		bCloseResolved = true;
		return;
	}
	bCloseResolved = false;
	WorldVoxelPaint::FVoxelPaintPackedBrickTransfer Restore;
	Bindings.CpuState->BuildPackedTransfer(StrokeDirtyBricks, Restore);
	FVoxelPaintSessionController* Controller = Bindings.Controller;
	Bindings.Transfers->UploadAndPublish(*Bindings.Resources, MoveTemp(Restore), *Bindings.Ledger,
		[this, Controller, Bricks = StrokeDirtyBricks](bool bOk) mutable
		{
			if (!bOk && Controller)
			{
				Controller->NotifyAuthoritativeUploadFailed(MoveTemp(Bricks));
			}
			bCloseResolved = true;
		});
}

void FVoxelPaintStrokePipeline::OnCloseReadback(bool bOk, WorldVoxelPaint::FVoxelPaintPackedBrickTransfer&& Readback)
{
	using namespace StrokePipelinePrivate;

	if (!Bindings.IsValid())
	{
		bCloseResolved = true;
		return;
	}
	if (!bOk || !Readback.IsValid() || Readback.BrickCounts != Bindings.BrickCounts)
	{
		RestoreStrokeRegionFromCpu();
		Bindings.Controller->CancelOpenStroke();
		return;
	}

	WorldVoxelPaint::FVoxelPaintPackedBrickTransfer Before;
	Bindings.CpuState->BuildPackedTransfer(Readback.BrickIndices, Before);
	Bindings.CpuState->MergePackedTransfer(Readback);

	WorldVoxelPaint::FVoxelPaintStrokeDelta Delta;
	for (int32 Rank = 0; Rank < Readback.BrickIndices.Num(); ++Rank)
	{
		const int32 Base = Rank * WorldVoxelPaint::TexelsPerUniqueBrick;
		WorldVoxelPaint::FVoxelPaintBrickDelta BrickDelta;
		bool bAnyTouched = false;
		for (int32 InBrick = 0; InBrick < WorldVoxelPaint::TexelsPerUniqueBrick; ++InBrick)
		{
			const uint32 BeforeTexel = Before.Texels[Base + InBrick];
			const uint32 AfterTexel = Readback.Texels[Base + InBrick];
			if (BeforeTexel == AfterTexel)
			{
				continue;
			}
			if (!bAnyTouched)
			{
				BrickDelta.BrickIndex = Readback.BrickIndices[Rank];
				BrickDelta.TouchedMask.SetNumZeroed(MaskWords);
				bAnyTouched = true;
			}
			BrickDelta.TouchedMask[InBrick / 64] |= 1ull << (InBrick % 64);
			BrickDelta.Before.Add(BeforeTexel);
			BrickDelta.After.Add(AfterTexel);
		}
		if (bAnyTouched)
		{
			Delta.Bricks.Add(MoveTemp(BrickDelta));
		}
	}

	if (Delta.Bricks.IsEmpty())
	{
		Bindings.Controller->CancelOpenStroke();
		bCloseResolved = true;
		return;
	}

	Bindings.Controller->CommitClosedStroke(MoveTemp(Delta));
	Bindings.Transfers->CopyRegionWorkingToPublished(*Bindings.Resources, StrokeDirtyMin, StrokeDirtyMax);
	bCloseResolved = true;
}

#undef LOCTEXT_NAMESPACE
