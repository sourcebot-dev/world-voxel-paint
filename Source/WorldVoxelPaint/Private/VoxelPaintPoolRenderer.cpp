// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintPoolRenderer.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "DynamicRHI.h"
#include "GlobalShader.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "RHIStaticStates.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"
#include "Tasks/Task.h"
#include "VoxelPaintExternalSampling.h"
#include "VoxelPaintResidency.h"
#include "VoxelPaintSceneUniformBuffer.h"
#include "VoxelPaintStats.h"
#include "WorldVoxelPaintConstants.h"

DECLARE_GPU_STAT_NAMED(VoxelPaintPoolOps, TEXT("VoxelPaint PoolOps"));

class FVoxelPaintPageScatterCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelPaintPageScatterCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPaintPageScatterCS, FGlobalShader);

	static constexpr int32 ThreadGroupSize = 64;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, PageWriteOps)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWPageTables)
		SHADER_PARAMETER(uint32, NumWrites)
		SHADER_PARAMETER(uint32, PageEntryCapacity)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVoxelPaintPageScatterCS, "/Plugin/WorldVoxelPaint/VoxelPaintPoolUpload.usf", "PageScatterCS", SF_Compute);

FVoxelPaintPoolRenderer::FVoxelPaintPoolRenderer() = default;
FVoxelPaintPoolRenderer::~FVoxelPaintPoolRenderer()
{
	if (bInitialized)
	{
		SET_MEMORY_STAT(STAT_VoxelPaint_BrickPoolTextureMemory, 0);
		SET_MEMORY_STAT(STAT_VoxelPaint_PageTableMemory, 0);
	}
}

void FVoxelPaintPoolRenderer::Initialize(FRDGBuilder& GraphBuilder, int64 PoolCapacityBytes, int64 PageTableCapacityBytes)
{
	if (bInitialized)
	{
		return;
	}

	const int64 SlotsPerLayer =
		static_cast<int64>(WorldVoxelPaint::PoolBricksXY) * WorldVoxelPaint::PoolBricksXY;
	const int64 RequestedSlots =
		FMath::Max<int64>(PoolCapacityBytes / WorldVoxelPaint::BrickPayloadBytes, 1);
	int64 NumLayers = FMath::DivideAndRoundUp(RequestedSlots, SlotsPerLayer);
	const int64 MaxLayers =
		FMath::Max<int64>(static_cast<int64>(GMaxVolumeTextureDimensions) / WorldVoxelPaint::BrickSize, 1);
	if (NumLayers > MaxLayers)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("Brick pool budget %lld bytes requires %lld 128x128-brick layers; clamped to %lld (GMaxVolumeTextureDimensions %d)."),
			PoolCapacityBytes, NumLayers, MaxLayers, static_cast<int32>(GMaxVolumeTextureDimensions));
		NumLayers = MaxLayers;
	}

	PoolDimsInBricks = FIntVector(
		WorldVoxelPaint::PoolBricksXY, WorldVoxelPaint::PoolBricksXY, static_cast<int32>(NumLayers));
	PoolSlotCapacity = static_cast<int32>(NumLayers * SlotsPerLayer);
	PageEntryCapacity = static_cast<int32>(FMath::Clamp<int64>(
		PageTableCapacityBytes / static_cast<int64>(sizeof(uint32)), 1, MAX_int32));

	const FIntVector TexelDims = PoolDimsInBricks * WorldVoxelPaint::BrickSize;
	const FRHITextureCreateDesc PoolDesc =
		FRHITextureCreateDesc::Create3D(TEXT("WorldVoxelPaint.BrickPool"), TexelDims, PF_BC7)
		.SetFlags(ETextureCreateFlags::ShaderResource)
		.SetInitialState(ERHIAccess::SRVMask);
	PoolTexture = GraphBuilder.RHICmdList.CreateTexture(PoolDesc);

	FRDGBufferRef PageTableRDG = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), PageEntryCapacity),
		TEXT("WorldVoxelPaint.PageTables"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(PageTableRDG), 0u);
	PageTableBuffer = GraphBuilder.ConvertToExternalBuffer(PageTableRDG);

	SET_MEMORY_STAT(STAT_VoxelPaint_BrickPoolTextureMemory,
		static_cast<int64>(RHICalcTexturePlatformSize(PoolDesc).Size));
	SET_MEMORY_STAT(STAT_VoxelPaint_PageTableMemory,
		static_cast<int64>(PageEntryCapacity) * static_cast<int64>(sizeof(uint32)));

	bInitialized = true;
}

bool FVoxelPaintPoolRenderer::IsInitialized() const
{
	return bInitialized;
}

int32 FVoxelPaintPoolRenderer::GetPoolSlotCapacity() const
{
	return PoolSlotCapacity;
}

int32 FVoxelPaintPoolRenderer::GetPageEntryCapacity() const
{
	return PageEntryCapacity;
}

FIntVector FVoxelPaintPoolRenderer::GetPoolDimsInBricks() const
{
	return PoolDimsInBricks;
}

void FVoxelPaintPoolRenderer::LaunchPreparation(TSharedPtr<FVoxelPaintPreparedSourceSnapshot> Snapshot,
	uint64 OwnerEpoch, const TSharedRef<FVoxelPaintResidencyManager, ESPMode::ThreadSafe>& Residency)
{
	if (Snapshot.IsValid() && Snapshot->Manifest.IsValid() && Snapshot->AssetKey != FVoxelPaintAssetKey())
	{
		FRetainedSnapshot& Retained = RetainedSnapshots.FindOrAdd(Snapshot->AssetKey);
		if (!Retained.Snapshot.IsValid() || Retained.Snapshot->SourceGeneration <= Snapshot->SourceGeneration)
		{
			Retained.Snapshot = Snapshot;
		}
	}
	LaunchPendingPreparations(OwnerEpoch, Residency);
}

void FVoxelPaintPoolRenderer::PruneRetainedSnapshots(const TSet<FVoxelPaintAssetKey>& DesiredKeys)
{
	for (TMap<FVoxelPaintAssetKey, FRetainedSnapshot>::TIterator It = RetainedSnapshots.CreateIterator(); It; ++It)
	{
		if (!DesiredKeys.Contains(It->Key))
		{
			It.RemoveCurrent();
		}
	}
}

void FVoxelPaintPoolRenderer::ClearRetainedSnapshots()
{
	RetainedSnapshots.Empty();
}

void FVoxelPaintPoolRenderer::LaunchPendingPreparations(uint64 OwnerEpoch,
	const TSharedRef<FVoxelPaintResidencyManager, ESPMode::ThreadSafe>& Residency)
{
	for (TMap<FVoxelPaintAssetKey, FRetainedSnapshot>::TIterator It = RetainedSnapshots.CreateIterator(); It; ++It)
	{
		FRetainedSnapshot& Retained = It.Value();
		if (!Retained.Snapshot.IsValid() || !Retained.Snapshot->Manifest.IsValid())
		{
			It.RemoveCurrent();
			continue;
		}

		const TSharedPtr<FVoxelPaintPreparedSourceSnapshot>& Snapshot = Retained.Snapshot;
		const bool bAlreadyLaunched = Retained.LaunchedGeneration == Snapshot->SourceGeneration
			&& Retained.LaunchedEpoch == OwnerEpoch;

		uint64 NeededGeneration = 0;
		if (Residency->NeedsPreparation(Snapshot->AssetKey, NeededGeneration))
		{
			if (NeededGeneration == Snapshot->SourceGeneration && !bAlreadyLaunched)
			{
				LaunchTask(Snapshot, OwnerEpoch, Residency);
				Retained.LaunchedGeneration = Snapshot->SourceGeneration;
				Retained.LaunchedEpoch = OwnerEpoch;
			}
			continue;
		}

		const EVoxelPaintAssetResidencyState State = Residency->GetAssetState(Snapshot->AssetKey);
		if (State == EVoxelPaintAssetResidencyState::NotResident && !bAlreadyLaunched
			&& Snapshot->Manifest->PopulatedBricks.Num() > 0)
		{
			// Snapshots can land before the same frame's SetDesiredAssets; the completion render
			// command executes after that application, so an early launch is stale-filter safe.
			LaunchTask(Snapshot, OwnerEpoch, Residency);
			Retained.LaunchedGeneration = Snapshot->SourceGeneration;
			Retained.LaunchedEpoch = OwnerEpoch;
			continue;
		}

		if (State == EVoxelPaintAssetResidencyState::Published
			|| State == EVoxelPaintAssetResidencyState::ConfigError
			|| Snapshot->Manifest->PopulatedBricks.IsEmpty())
		{
			It.RemoveCurrent();
		}
	}
}

void FVoxelPaintPoolRenderer::LaunchTask(const TSharedPtr<FVoxelPaintPreparedSourceSnapshot>& Snapshot,
	uint64 OwnerEpoch, const TSharedRef<FVoxelPaintResidencyManager, ESPMode::ThreadSafe>& Residency)
{
	Residency->OnPreparationLaunched(Snapshot->AssetKey, Snapshot->SourceGeneration, OwnerEpoch);

	TWeakPtr<FVoxelPaintResidencyManager, ESPMode::ThreadSafe> WeakResidency(Residency);
	UE::Tasks::Launch(TEXT("WorldVoxelPaint.Preparation"),
		[Snapshot, OwnerEpoch, WeakResidency]()
		{
			FVoxelPaintPreparedPayload Payload;
			Payload.AssetKey = Snapshot->AssetKey;
			Payload.SourceGeneration = Snapshot->SourceGeneration;
			Payload.LaunchOwnerEpoch = OwnerEpoch;
			Payload.Identity = Snapshot->Identity;
			Payload.Manifest = Snapshot->Manifest;

			TSharedPtr<TArray<uint8>> Bytes = MakeShared<TArray<uint8>>();
			const bool bAcquired = Snapshot->AcquirePayload && Snapshot->AcquirePayload(*Bytes);
			const int64 ExpectedBytes = static_cast<int64>(Snapshot->Manifest->PopulatedBricks.Num())
				* WorldVoxelPaint::BrickPayloadBytes;
			Payload.bPayloadValid = bAcquired && static_cast<int64>(Bytes->Num()) == ExpectedBytes;
			Payload.BrickPayload = Bytes;

			ENQUEUE_RENDER_COMMAND(WorldVoxelPaintPreparationCompleted)(
				[WeakResidency, Payload = MoveTemp(Payload)](FRHICommandListImmediate&) mutable
				{
					if (TSharedPtr<FVoxelPaintResidencyManager, ESPMode::ThreadSafe> Pinned = WeakResidency.Pin())
					{
						Pinned->OnPreparationCompleted(MoveTemp(Payload));
					}
				});
		},
		UE::Tasks::ETaskPriority::BackgroundNormal);
}

void FVoxelPaintPoolRenderer::ExecuteOps(FRDGBuilder& GraphBuilder, const FVoxelPaintIntegrationOps& Ops)
{
	if (!bInitialized)
	{
		return;
	}

	RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelPaintPoolOps, "WorldVoxelPaint.PoolOps");

	if (Ops.BrickUploads.Num() > 0)
	{
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("WorldVoxelPaint.BrickUploads(%d)", Ops.BrickUploads.Num()),
			ERDGPassFlags::None,
			[Texture = PoolTexture, Uploads = Ops.BrickUploads, Dims = PoolDimsInBricks](FRHICommandList& RHICmdList)
			{
				constexpr uint32 BlockBytes = 16;
				constexpr uint32 NumBlocksPerAxis = WorldVoxelPaint::BrickSize / 4;
				constexpr uint32 SourceRowPitch = NumBlocksPerAxis * BlockBytes;
				constexpr uint32 SourceDepthPitch = NumBlocksPerAxis * NumBlocksPerAxis * BlockBytes;

				for (const FVoxelPaintBrickUploadOp& Op : Uploads)
				{
					if (!Op.Payload.IsValid() || Op.PoolSlot < 0 || Op.PayloadBrickIndex < 0)
					{
						continue;
					}
					const int64 SourceOffset =
						static_cast<int64>(Op.PayloadBrickIndex) * WorldVoxelPaint::BrickPayloadBytes;
					if (SourceOffset + WorldVoxelPaint::BrickPayloadBytes > Op.Payload->Num())
					{
						continue;
					}
					const int32 SlotX = Op.PoolSlot % Dims.X;
					const int32 SlotY = (Op.PoolSlot / Dims.X) % Dims.Y;
					const int32 SlotZ = Op.PoolSlot / (Dims.X * Dims.Y);
					if (SlotZ >= Dims.Z)
					{
						continue;
					}
					const FUpdateTextureRegion3D Region(
						static_cast<uint32>(SlotX * WorldVoxelPaint::BrickSize),
						static_cast<uint32>(SlotY * WorldVoxelPaint::BrickSize),
						static_cast<uint32>(SlotZ * WorldVoxelPaint::BrickSize),
						0, 0, 0,
						WorldVoxelPaint::BrickSize, WorldVoxelPaint::BrickSize, WorldVoxelPaint::BrickSize);
					RHICmdList.UpdateTexture3D(Texture, 0, Region, SourceRowPitch, SourceDepthPitch,
						Op.Payload->GetData() + SourceOffset);
				}
			});
	}

	if (Ops.PageWrites.Num() > 0)
	{
		static_assert(sizeof(FVoxelPaintPageWriteOp) == 8, "Page write ops upload as uint2 pairs");

		FRDGBufferRef WriteOpsBuffer = CreateStructuredBuffer(GraphBuilder,
			TEXT("WorldVoxelPaint.PageWriteOps"), sizeof(FVoxelPaintPageWriteOp), Ops.PageWrites.Num(),
			Ops.PageWrites.GetData(),
			static_cast<uint64>(Ops.PageWrites.Num()) * sizeof(FVoxelPaintPageWriteOp));
		FRDGBufferRef PageTableRDG = GraphBuilder.RegisterExternalBuffer(PageTableBuffer);

		FVoxelPaintPageScatterCS::FParameters* PassParameters =
			GraphBuilder.AllocParameters<FVoxelPaintPageScatterCS::FParameters>();
		PassParameters->PageWriteOps = GraphBuilder.CreateSRV(WriteOpsBuffer);
		PassParameters->RWPageTables = GraphBuilder.CreateUAV(PageTableRDG);
		PassParameters->NumWrites = static_cast<uint32>(Ops.PageWrites.Num());
		PassParameters->PageEntryCapacity = static_cast<uint32>(PageEntryCapacity);

		TShaderMapRef<FVoxelPaintPageScatterCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("WorldVoxelPaint.PageScatter(%d)", Ops.PageWrites.Num()),
			ComputeShader, PassParameters,
			FComputeShaderUtils::GetGroupCount(Ops.PageWrites.Num(), FVoxelPaintPageScatterCS::ThreadGroupSize));
	}
}

void FVoxelPaintPoolRenderer::Register(FRDGBuilder& GraphBuilder, FVoxelPaintSceneParameters& Params) const
{
	if (!bInitialized)
	{
		return;
	}

	Params.BrickPool = PoolTexture;
	Params.PageTables = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(PageTableBuffer));
	Params.PoolSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Params.PoolDimsInBricks = PoolDimsInBricks;
	const FIntVector TexelDims = PoolDimsInBricks * WorldVoxelPaint::BrickSize;
	Params.PoolTexelExtentRcp = FVector3f(
		1.0f / static_cast<float>(TexelDims.X),
		1.0f / static_cast<float>(TexelDims.Y),
		1.0f / static_cast<float>(TexelDims.Z));
	Params.BrickSize = static_cast<uint32>(WorldVoxelPaint::BrickSize);
	Params.UniqueBrickSize = static_cast<uint32>(WorldVoxelPaint::UniqueBrickSize);
}

void FVoxelPaintPoolRenderer::Register(FRDGBuilder& GraphBuilder, FVoxelPaintExternalSamplingParams& Params) const
{
	if (!bInitialized)
	{
		return;
	}
	Params.BrickPool = PoolTexture;
	Params.PageTables = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(PageTableBuffer));
	Params.PoolSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Params.PoolDimsInBricks = PoolDimsInBricks;
	const FIntVector TexelDims = PoolDimsInBricks * WorldVoxelPaint::BrickSize;
	Params.PoolTexelExtentRcp = FVector3f(
		1.0f / static_cast<float>(TexelDims.X),
		1.0f / static_cast<float>(TexelDims.Y),
		1.0f / static_cast<float>(TexelDims.Z));
	Params.BrickSize = static_cast<uint32>(WorldVoxelPaint::BrickSize);
	Params.UniqueBrickSize = static_cast<uint32>(WorldVoxelPaint::UniqueBrickSize);
}
