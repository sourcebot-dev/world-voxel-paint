// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintSessionTransfers.h"

#include "Async/Async.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "VoxelPaintSessionBudget.h"
#include "VoxelPaintSessionResources.h"
#include "VoxelPaintSessionTransferShaders.h"
#include "WorldVoxelPaintConstants.h"

#include <atomic>

DECLARE_GPU_STAT_NAMED(VoxelPaintSession, TEXT("VoxelPaint Session"));

static TAutoConsoleVariable<int32> CVarVoxelPaintEditorMidStrokePublication(
	TEXT("r.VoxelPaint.Editor.MidStrokePublication"),
	1,
	TEXT("1 (default): mid-stroke dirty-region copies publish in-progress paint to the session texture each frame. 0: live preview drops to stroke-close granularity."),
	ECVF_Default);

namespace SessionTransfersPrivate
{
	static void ComputeBrickUnionBox(TConstArrayView<uint32> BrickIndices,
		const FIntVector& BrickCounts, FIntVector& OutTexelMin, FIntVector& OutTexelMax)
	{
		OutTexelMin = FIntVector(MAX_int32, MAX_int32, MAX_int32);
		OutTexelMax = FIntVector(MIN_int32, MIN_int32, MIN_int32);
		const uint32 CountX = static_cast<uint32>(BrickCounts.X);
		const uint32 CountY = static_cast<uint32>(BrickCounts.Y);
		for (const uint32 BrickIndex : BrickIndices)
		{
			const int32 BrickX = static_cast<int32>(BrickIndex % CountX);
			const int32 BrickY = static_cast<int32>((BrickIndex / CountX) % CountY);
			const int32 BrickZ = static_cast<int32>(BrickIndex / (CountX * CountY));
			const FIntVector TexelMin(
				BrickX * WorldVoxelPaint::UniqueBrickSize,
				BrickY * WorldVoxelPaint::UniqueBrickSize,
				BrickZ * WorldVoxelPaint::UniqueBrickSize);
			OutTexelMin.X = FMath::Min(OutTexelMin.X, TexelMin.X);
			OutTexelMin.Y = FMath::Min(OutTexelMin.Y, TexelMin.Y);
			OutTexelMin.Z = FMath::Min(OutTexelMin.Z, TexelMin.Z);
			OutTexelMax.X = FMath::Max(OutTexelMax.X, TexelMin.X + WorldVoxelPaint::UniqueBrickSize - 1);
			OutTexelMax.Y = FMath::Max(OutTexelMax.Y, TexelMin.Y + WorldVoxelPaint::UniqueBrickSize - 1);
			OutTexelMax.Z = FMath::Max(OutTexelMax.Z, TexelMin.Z + WorldVoxelPaint::UniqueBrickSize - 1);
		}
	}

	static bool ExecuteScatterOp_RenderThread(FRHICommandListImmediate& RHICmdList,
		const FVoxelPaintSessionResources& Resources,
		const WorldVoxelPaint::FVoxelPaintPackedBrickTransfer& Transfer, bool bHydrate,
		const FIntVector& CopyMin, const FIntVector& CopyMax)
	{
		const FTextureRHIRef WorkingRHI = Resources.GetWorkingRHI();
		const FTextureRHIRef PublishedRHI = Resources.GetPublishedRHI();
		if (!WorkingRHI.IsValid() || !PublishedRHI.IsValid())
		{
			return false;
		}

		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("WorldVoxelPaint.SessionScatterOp"));
		{
			RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelPaintSession, "WorldVoxelPaint.SessionScatter");

			FRDGTextureRef WorkingRDG =
				RegisterExternalTexture(GraphBuilder, WorkingRHI, TEXT("WorldVoxelPaint.SessionWorking"));
			FRDGTextureRef PublishedRDG =
				RegisterExternalTexture(GraphBuilder, PublishedRHI, TEXT("WorldVoxelPaint.SessionPublished"));

			if (bHydrate)
			{
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGTextureUAVDesc(WorkingRDG)),
					FLinearColor::Transparent);
			}

			const int32 NumBricks = Transfer.BrickIndices.Num();
			if (NumBricks > 0)
			{
				const int32 NumTexels = NumBricks * WorldVoxelPaint::TexelsPerUniqueBrick;
				FRDGBufferRef IndicesBuffer = CreateStructuredBuffer(GraphBuilder,
					TEXT("WorldVoxelPaint.SessionScatterBricks"), sizeof(uint32), NumBricks,
					Transfer.BrickIndices.GetData(),
					static_cast<uint64>(NumBricks) * sizeof(uint32));
				FRDGBufferRef TexelsBuffer = CreateStructuredBuffer(GraphBuilder,
					TEXT("WorldVoxelPaint.SessionScatterTexels"), sizeof(uint32), NumTexels,
					Transfer.Texels.GetData(),
					static_cast<uint64>(NumTexels) * sizeof(uint32));

				FVoxelPaintSessionScatterCS::FParameters* PassParameters =
					GraphBuilder.AllocParameters<FVoxelPaintSessionScatterCS::FParameters>();
				PassParameters->BrickIndices = GraphBuilder.CreateSRV(IndicesBuffer);
				PassParameters->SourceTexels = GraphBuilder.CreateSRV(TexelsBuffer);
				PassParameters->RWWorkingTexture = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(WorkingRDG));
				PassParameters->BrickCounts = Transfer.BrickCounts;
				PassParameters->NumBricks = static_cast<uint32>(NumBricks);

				TShaderMapRef<FVoxelPaintSessionScatterCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder,
					RDG_EVENT_NAME("WorldVoxelPaint.SessionScatter(%d)", NumBricks),
					ComputeShader, PassParameters,
					FComputeShaderUtils::GetGroupCountWrapped(NumTexels,
						FVoxelPaintSessionScatterCS::ThreadGroupSize));
			}

			FRHICopyTextureInfo CopyInfo;
			if (!bHydrate)
			{
				CopyInfo.SourcePosition = CopyMin;
				CopyInfo.DestPosition = CopyMin;
				CopyInfo.Size = CopyMax - CopyMin + FIntVector(1, 1, 1);
			}
			AddCopyTexturePass(GraphBuilder, WorkingRDG, PublishedRDG, CopyInfo);
		}

		GraphBuilder.Execute();
		return true;
	}
}

struct FVoxelPaintSessionTransferEngine::FReadbackRequest
{
	TUniquePtr<FRHIGPUBufferReadback> Readback;
	TArray<uint32> BrickIndices;
	TArray<uint32> Texels;
	FIntVector BrickCounts = FIntVector::ZeroValue;
	uint32 NumBytes = 0;
	WorldVoxelPaint::FVoxelPaintSessionBudgetLedger* Ledger = nullptr;
	int32 StagingHandle = INDEX_NONE;
	TFunction<void(bool, WorldVoxelPaint::FVoxelPaintPackedBrickTransfer&&)> OnComplete;
	std::atomic<bool> bCopyEnqueued{ false };
	std::atomic<bool> bPollInFlight{ false };
	std::atomic<bool> bDataReady{ false };
	std::atomic<bool> bFailed{ false };
};

FVoxelPaintSessionTransferEngine::~FVoxelPaintSessionTransferEngine()
{
	ensureMsgf(PendingOps == 0 && !ActiveReadback.IsValid(),
		TEXT("FVoxelPaintSessionTransferEngine destroyed with transfers in flight; CancelAndDrain first."));
	if (ReadbackTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ReadbackTickerHandle);
	}
}

void FVoxelPaintSessionTransferEngine::Hydrate(FVoxelPaintSessionResources& Resources,
	WorldVoxelPaint::FVoxelPaintPackedBrickTransfer&& Transfer,
	WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger, TFunction<void(bool)> OnComplete)
{
	StartScatterOp(Resources, MoveTemp(Transfer), Ledger, MoveTemp(OnComplete), true);
}

void FVoxelPaintSessionTransferEngine::UploadAndPublish(FVoxelPaintSessionResources& Resources,
	WorldVoxelPaint::FVoxelPaintPackedBrickTransfer&& Transfer,
	WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger, TFunction<void(bool)> OnComplete)
{
	StartScatterOp(Resources, MoveTemp(Transfer), Ledger, MoveTemp(OnComplete), false);
}

void FVoxelPaintSessionTransferEngine::StartScatterOp(FVoxelPaintSessionResources& Resources,
	WorldVoxelPaint::FVoxelPaintPackedBrickTransfer&& Transfer,
	WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger, TFunction<void(bool)> OnComplete,
	bool bHydrate)
{
	using namespace WorldVoxelPaint;

	checkf(IsInGameThread(), TEXT("Session transfers are game-thread entry points"));

	if (bCancelDraining || !Resources.IsAllocated() || !Transfer.IsValid()
		|| Transfer.BrickCounts * UniqueBrickSize != Resources.GetResolution())
	{
		if (OnComplete)
		{
			OnComplete(false);
		}
		return;
	}
	if (!bHydrate && Transfer.BrickIndices.IsEmpty())
	{
		if (OnComplete)
		{
			OnComplete(true);
		}
		return;
	}
	if (!Ledger.BeginTransferGeneration())
	{
		if (OnComplete)
		{
			OnComplete(false);
		}
		return;
	}

	const int32 NumBricks = Transfer.BrickIndices.Num();
	const int64 StagingBytes = static_cast<int64>(NumBricks) * sizeof(uint32)
		+ static_cast<int64>(NumBricks) * TexelsPerUniqueBrick * sizeof(uint32);
	int32 StagingHandle = INDEX_NONE;
	if (!Ledger.Reserve(EVoxelPaintSessionGpuCategory::TransferStaging, StagingBytes, StagingHandle))
	{
		Ledger.EndTransferGeneration();
		if (OnComplete)
		{
			OnComplete(false);
		}
		return;
	}

	FIntVector CopyMin = FIntVector::ZeroValue;
	FIntVector CopyMax = Resources.GetResolution() - FIntVector(1, 1, 1);
	if (!bHydrate)
	{
		SessionTransfersPrivate::ComputeBrickUnionBox(Transfer.BrickIndices, Transfer.BrickCounts,
			CopyMin, CopyMax);
	}

	++PendingOps;
	const FVoxelPaintSessionResources* ResourcesPtr = &Resources;
	WorldVoxelPaint::FVoxelPaintSessionBudgetLedger* LedgerPtr = &Ledger;
	ENQUEUE_RENDER_COMMAND(WorldVoxelPaintSessionScatterOp)(
		[this, ResourcesPtr, LedgerPtr, StagingHandle, bHydrate, CopyMin, CopyMax,
			Transfer = MoveTemp(Transfer),
			OnComplete = MoveTemp(OnComplete)](FRHICommandListImmediate& RHICmdList) mutable
		{
			const bool bOk = SessionTransfersPrivate::ExecuteScatterOp_RenderThread(
				RHICmdList, *ResourcesPtr, Transfer, bHydrate, CopyMin, CopyMax);
			AsyncTask(ENamedThreads::GameThread,
				[this, bOk, LedgerPtr, StagingHandle, OnComplete = MoveTemp(OnComplete)]() mutable
				{
					FinalizeStreamOp(bOk, *LedgerPtr, StagingHandle, OnComplete);
				});
		});
}

void FVoxelPaintSessionTransferEngine::FinalizeStreamOp(bool bOk,
	WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger, int32 StagingHandle,
	TFunction<void(bool)>& OnComplete)
{
	Ledger.Release(StagingHandle);
	Ledger.EndTransferGeneration();
	--PendingOps;
	if (bCancelDraining)
	{
		MaybeFireDrain();
		return;
	}
	if (OnComplete)
	{
		OnComplete(bOk);
	}
}

void FVoxelPaintSessionTransferEngine::ReadbackDirtyBricks(FVoxelPaintSessionResources& Resources,
	TArray<uint32>&& DirtyBricks, WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger,
	TFunction<void(bool, WorldVoxelPaint::FVoxelPaintPackedBrickTransfer&&)> OnComplete)
{
	using namespace WorldVoxelPaint;

	checkf(IsInGameThread(), TEXT("Session transfers are game-thread entry points"));

	FVoxelPaintPackedBrickTransfer Immediate;
	Immediate.BrickCounts = Resources.GetResolution() / UniqueBrickSize;

	if (bCancelDraining || !Resources.IsAllocated() || ActiveReadback.IsValid())
	{
		if (OnComplete)
		{
			OnComplete(false, MoveTemp(Immediate));
		}
		return;
	}
	if (DirtyBricks.IsEmpty())
	{
		if (OnComplete)
		{
			OnComplete(true, MoveTemp(Immediate));
		}
		return;
	}
	if (!Ledger.BeginTransferGeneration())
	{
		if (OnComplete)
		{
			OnComplete(false, MoveTemp(Immediate));
		}
		return;
	}

	const int32 NumBricks = DirtyBricks.Num();
	const int64 TexelBytes = static_cast<int64>(NumBricks) * TexelsPerUniqueBrick * sizeof(uint32);
	const int64 StagingBytes = static_cast<int64>(NumBricks) * sizeof(uint32) + 2 * TexelBytes;
	int32 StagingHandle = INDEX_NONE;
	if (!Ledger.Reserve(EVoxelPaintSessionGpuCategory::TransferStaging, StagingBytes, StagingHandle))
	{
		Ledger.EndTransferGeneration();
		if (OnComplete)
		{
			OnComplete(false, MoveTemp(Immediate));
		}
		return;
	}

	TSharedPtr<FReadbackRequest, ESPMode::ThreadSafe> Request =
		MakeShared<FReadbackRequest, ESPMode::ThreadSafe>();
	Request->Readback = MakeUnique<FRHIGPUBufferReadback>(
		FName(TEXT("WorldVoxelPaint.SessionStrokeReadback")));
	Request->BrickIndices = MoveTemp(DirtyBricks);
	Request->BrickCounts = Immediate.BrickCounts;
	Request->NumBytes = static_cast<uint32>(TexelBytes);
	Request->Ledger = &Ledger;
	Request->StagingHandle = StagingHandle;
	Request->OnComplete = MoveTemp(OnComplete);
	ActiveReadback = Request;
	++PendingOps;

	const FVoxelPaintSessionResources* ResourcesPtr = &Resources;
	ENQUEUE_RENDER_COMMAND(WorldVoxelPaintSessionGatherOp)(
		[ResourcesPtr, Request](FRHICommandListImmediate& RHICmdList)
		{
			const FTextureRHIRef WorkingRHI = ResourcesPtr->GetWorkingRHI();
			if (!WorkingRHI.IsValid())
			{
				Request->bFailed = true;
				return;
			}

			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("WorldVoxelPaint.SessionGatherOp"));
			{
				RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelPaintSession, "WorldVoxelPaint.SessionGather");

				FRDGTextureRef WorkingRDG =
					RegisterExternalTexture(GraphBuilder, WorkingRHI, TEXT("WorldVoxelPaint.SessionWorking"));

				const int32 NumRequestBricks = Request->BrickIndices.Num();
				const int32 NumTexels = NumRequestBricks * WorldVoxelPaint::TexelsPerUniqueBrick;
				FRDGBufferRef IndicesBuffer = CreateStructuredBuffer(GraphBuilder,
					TEXT("WorldVoxelPaint.SessionGatherBricks"), sizeof(uint32), NumRequestBricks,
					Request->BrickIndices.GetData(),
					static_cast<uint64>(NumRequestBricks) * sizeof(uint32));
				FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumTexels),
					TEXT("WorldVoxelPaint.SessionGatherTexels"));

				FVoxelPaintSessionGatherCS::FParameters* PassParameters =
					GraphBuilder.AllocParameters<FVoxelPaintSessionGatherCS::FParameters>();
				PassParameters->BrickIndices = GraphBuilder.CreateSRV(IndicesBuffer);
				PassParameters->WorkingTexture = WorkingRDG;
				PassParameters->RWGatherTexels = GraphBuilder.CreateUAV(OutputBuffer);
				PassParameters->BrickCounts = Request->BrickCounts;
				PassParameters->NumBricks = static_cast<uint32>(NumRequestBricks);

				TShaderMapRef<FVoxelPaintSessionGatherCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder,
					RDG_EVENT_NAME("WorldVoxelPaint.SessionGather(%d)", NumRequestBricks),
					ComputeShader, PassParameters,
					FComputeShaderUtils::GetGroupCountWrapped(NumTexels,
						FVoxelPaintSessionGatherCS::ThreadGroupSize));

				AddEnqueueCopyPass(GraphBuilder, Request->Readback.Get(), OutputBuffer, Request->NumBytes);
			}
			GraphBuilder.Execute();
			Request->bCopyEnqueued = true;
		});

	ReadbackTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FVoxelPaintSessionTransferEngine::TickReadback), 0.0f);
}

bool FVoxelPaintSessionTransferEngine::TickReadback(float)
{
	if (!ActiveReadback.IsValid())
	{
		ReadbackTickerHandle.Reset();
		return false;
	}

	FReadbackRequest& Request = *ActiveReadback;
	if (Request.bDataReady.load())
	{
		FinalizeReadback(true);
		return false;
	}
	if (Request.bFailed.load())
	{
		FinalizeReadback(false);
		return false;
	}

	if (Request.bCopyEnqueued.load() && !Request.bPollInFlight.exchange(true))
	{
		TSharedPtr<FReadbackRequest, ESPMode::ThreadSafe> Shared = ActiveReadback;
		ENQUEUE_RENDER_COMMAND(WorldVoxelPaintSessionReadbackPoll)(
			[Shared](FRHICommandListImmediate&)
			{
				if (Shared->Readback->IsReady())
				{
					const uint32 NumTexels = Shared->NumBytes / sizeof(uint32);
					if (void* Data = Shared->Readback->Lock(Shared->NumBytes))
					{
						Shared->Texels.SetNumUninitialized(static_cast<int32>(NumTexels));
						FMemory::Memcpy(Shared->Texels.GetData(), Data, Shared->NumBytes);
						Shared->Readback->Unlock();
						Shared->bDataReady = true;
					}
					else
					{
						Shared->bFailed = true;
					}
				}
				Shared->bPollInFlight = false;
			});
	}
	return true;
}

void FVoxelPaintSessionTransferEngine::FinalizeReadback(bool bOk)
{
	TSharedPtr<FReadbackRequest, ESPMode::ThreadSafe> Request = ActiveReadback;
	ActiveReadback.Reset();
	ReadbackTickerHandle.Reset();

	Request->Ledger->Release(Request->StagingHandle);
	Request->Ledger->EndTransferGeneration();
	--PendingOps;

	// Any in-flight poll command was enqueued earlier and holds its own shared ref; the
	// readback object is deleted render-side, after it, in submission order.
	ENQUEUE_RENDER_COMMAND(WorldVoxelPaintSessionReadbackRelease)(
		[Request](FRHICommandListImmediate&)
		{
			Request->Readback.Reset();
		});

	if (bCancelDraining)
	{
		MaybeFireDrain();
		return;
	}

	if (Request->OnComplete)
	{
		WorldVoxelPaint::FVoxelPaintPackedBrickTransfer Transfer;
		Transfer.BrickCounts = Request->BrickCounts;
		if (bOk)
		{
			Transfer.BrickIndices = MoveTemp(Request->BrickIndices);
			Transfer.Texels = MoveTemp(Request->Texels);
		}
		Request->OnComplete(bOk, MoveTemp(Transfer));
	}
}

void FVoxelPaintSessionTransferEngine::CopyRegionWorkingToPublished(
	FVoxelPaintSessionResources& Resources, const FIntVector& TexelMin, const FIntVector& TexelMax)
{
	checkf(IsInGameThread(), TEXT("Session transfers are game-thread entry points"));

	if (!Resources.IsAllocated())
	{
		return;
	}

	const FVoxelPaintSessionResources* ResourcesPtr = &Resources;
	ENQUEUE_RENDER_COMMAND(WorldVoxelPaintSessionRegionCopy)(
		[ResourcesPtr, TexelMin, TexelMax](FRHICommandListImmediate& RHICmdList)
		{
			const FTextureRHIRef WorkingRHI = ResourcesPtr->GetWorkingRHI();
			const FTextureRHIRef PublishedRHI = ResourcesPtr->GetPublishedRHI();
			if (!WorkingRHI.IsValid() || !PublishedRHI.IsValid())
			{
				return;
			}

			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("WorldVoxelPaint.SessionRegionCopy"));
			{
				RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelPaintSession, "WorldVoxelPaint.SessionRegionCopy");

				FRDGTextureRef WorkingRDG =
					RegisterExternalTexture(GraphBuilder, WorkingRHI, TEXT("WorldVoxelPaint.SessionWorking"));
				FRDGTextureRef PublishedRDG =
					RegisterExternalTexture(GraphBuilder, PublishedRHI, TEXT("WorldVoxelPaint.SessionPublished"));

				FRHICopyTextureInfo CopyInfo;
				CopyInfo.SourcePosition = TexelMin;
				CopyInfo.DestPosition = TexelMin;
				CopyInfo.Size = TexelMax - TexelMin + FIntVector(1, 1, 1);
				AddCopyTexturePass(GraphBuilder, WorkingRDG, PublishedRDG, CopyInfo);
			}
			GraphBuilder.Execute();
		});
}

bool FVoxelPaintSessionTransferEngine::IsTransferInFlight() const
{
	return PendingOps > 0;
}

void FVoxelPaintSessionTransferEngine::CancelAndDrain(TFunction<void()> OnDrained)
{
	checkf(IsInGameThread(), TEXT("Session transfers are game-thread entry points"));
	ensureMsgf(!bCancelDraining, TEXT("CancelAndDrain re-entered while already draining"));

	bCancelDraining = true;
	PendingDrain = MoveTemp(OnDrained);

	if (ActiveReadback.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ReadbackTickerHandle);
		FinalizeReadback(false);
	}

	MaybeFireDrain();
}

void FVoxelPaintSessionTransferEngine::MaybeFireDrain()
{
	if (bCancelDraining && PendingOps == 0)
	{
		bCancelDraining = false;
		TFunction<void()> Drain = MoveTemp(PendingDrain);
		PendingDrain = nullptr;
		if (Drain)
		{
			Drain();
		}
	}
}
