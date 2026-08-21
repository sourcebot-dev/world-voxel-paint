// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintPCGDataInterface.h"

#include "VoxelPaintPCGSampleSettings.h"

#include "PCGGraphExecutionStateInterface.h"
#include "Compute/PCGDataBinding.h"
#include "Compute/PCGDataDescription.h"

#include "VoxelPaintCoordinator.h"
#include "VoxelPaintExternalSampling.h"
#include "VoxelPaintRecord.h"
#include "WorldVoxelPaintConstants.h"

#include "GlobalRenderResources.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "ShaderCompilerCore.h"
#include "ShaderCore.h"
#include "ShaderParameterMetadataBuilder.h"
#include "ComputeFramework/ShaderParamTypeDefinition.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VoxelPaintPCGDataInterface)

// Every member surfaces to the DI template as {DataInterfaceName}_<MemberName>; the .ush
// declarations must match these names exactly. BrickPool/SessionTexture are plain RHI
// textures (created raw via RHICmdList.CreateTexture, never RDG-registered) -> plain
// SHADER_PARAMETER_TEXTURE slots, never SHADER_PARAMETER_RDG_TEXTURE (spec-inputs G).
BEGIN_SHADER_PARAMETER_STRUCT(FVoxelPaintPCGDataInterfaceParameters,)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Volumes)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, PageTables)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RecordIndices)
	SHADER_PARAMETER_TEXTURE(Texture3D, BrickPool)
	SHADER_PARAMETER_TEXTURE(Texture3D, SessionTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, PoolSampler)
	SHADER_PARAMETER(uint32, NumRecordIndices)
	SHADER_PARAMETER(uint32, NumVolumes)
	SHADER_PARAMETER(uint32, BrickSize)
	SHADER_PARAMETER(uint32, UniqueBrickSize)
	SHADER_PARAMETER(FIntVector, PoolDimsInBricks)
	SHADER_PARAMETER(FVector3f, PoolTexelExtentRcp)
	SHADER_PARAMETER(FVector3f, EvaluationOriginHigh)
	SHADER_PARAMETER(FVector3f, EvaluationOriginLow)
	SHADER_PARAMETER(FVector3f, CameraCenter)
	SHADER_PARAMETER(float, EvaluationRadiusSq)
	SHADER_PARAMETER(FIntVector4, ChannelAttributeIds)
	SHADER_PARAMETER(int32, DensityWriteChannel)
END_SHADER_PARAMETER_STRUCT()

void UVoxelPaintPCGDataInterface::GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const
{
	// Additional-input DIs bind with Prefix == NAME_None: the kernel calls these EXACT names
	// (no pin prefix, no mangling), so every name is self-prefixed to stay globally unique in
	// the collapsed compute graph (spawner-DI convention).
	OutFunctions.AddDefaulted_GetRef()
		.SetName(TEXT("WVPSampler_GetLayerWeights"))
		.AddReturnType(EShaderFundamentalType::Float, 4)
		.AddParam(EShaderFundamentalType::Float, 3); // InWorldPos

	OutFunctions.AddDefaulted_GetRef()
		.SetName(TEXT("WVPSampler_GetChannelAttributeId"))
		.AddReturnType(EShaderFundamentalType::Int)
		.AddParam(EShaderFundamentalType::Uint); // InChannelIndex 0..3

	OutFunctions.AddDefaulted_GetRef()
		.SetName(TEXT("WVPSampler_GetDensityWriteChannel"))
		.AddReturnType(EShaderFundamentalType::Int); // 0..3, or -1 = leave density untouched
}

void UVoxelPaintPCGDataInterface::GetShaderParameters(TCHAR const* UID, FShaderParametersMetadataBuilder& InOutBuilder, FShaderParametersMetadataAllocations& InOutAllocations) const
{
	InOutBuilder.AddNestedStruct<FVoxelPaintPCGDataInterfaceParameters>(UID);
}

TCHAR const* UVoxelPaintPCGDataInterface::TemplateFilePath = TEXT("/Plugin/WorldVoxelPaintPCG/VoxelPaintPCGDataInterface.ush");

TCHAR const* UVoxelPaintPCGDataInterface::GetShaderVirtualPath() const
{
	return TemplateFilePath;
}

void UVoxelPaintPCGDataInterface::GetShaderHash(FString& InOutKey) const
{
	// GetShaderFileHash recursively hashes scanned #includes; the core sampler library is
	// ALSO hashed explicitly (landscape-DI defensive pattern) so edits to the shared paint
	// math always retrigger PCG kernel compiles. SP_PCD3D_SM5 is a hash-key input only - keep
	// verbatim on all platforms, Metal included (engine does the same).
	GetShaderFileHash(TemplateFilePath, EShaderPlatform::SP_PCD3D_SM5).AppendString(InOutKey);
	GetShaderFileHash(TEXT("/Plugin/WorldVoxelPaint/VoxelPaintSample.ush"), EShaderPlatform::SP_PCD3D_SM5).AppendString(InOutKey);
}

void UVoxelPaintPCGDataInterface::GetHLSL(FString& OutHLSL, FString const& InDataInterfaceName) const
{
	TMap<FString, FStringFormatArg> TemplateArgs =
	{
		{ TEXT("DataInterfaceName"), InDataInterfaceName },
	};

	FString TemplateFile;
	if (ensure(LoadShaderSourceFile(TemplateFilePath, EShaderPlatform::SP_PCD3D_SM5, &TemplateFile, nullptr)))
	{
		OutHLSL += FString::Format(*TemplateFile, TemplateArgs);
	}
}

UComputeDataProvider* UVoxelPaintPCGDataInterface::CreateDataProvider() const
{
	return NewObject<UVoxelPaintPCGDataProvider>();
}

void UVoxelPaintPCGDataProvider::Initialize(const UComputeDataInterface* InDataInterface, UObject* InBinding, uint64 InInputMask, uint64 InOutputMask)
{
	Super::Initialize(InDataInterface, InBinding, InInputMask, InOutputMask);

	// Unconditional overwrite: pooled FComputeGraphInstance reuse re-runs Initialize on this
	// same object (ComputeGraph.cpp:307-331); stale capture must never survive.
	GenerationBounds = FBox(EForceInit::ForceInit);
	ChannelAttributeIds = FIntVector4(INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE);
	DensityWriteChannel = INDEX_NONE;
#if WITH_EDITOR
	CapturedWorldKey = FObjectKey();
#endif

	UPCGDataBinding* Binding = CastChecked<UPCGDataBinding>(InBinding);
	if (const IPCGGraphExecutionSource* ExecutionSource = Binding->GetExecutionSource())
	{
		GenerationBounds = ExecutionSource->GetExecutionState().GetBounds();
#if WITH_EDITOR
		CapturedWorldKey = FObjectKey(ExecutionSource->GetExecutionState().GetWorld());
#endif
	}
	// NOTE: additional-input DIs have no pins - GetDownstreamInputPinLabelAliases() is empty
	// by construction. Never assert on it (landscape's check is pin-route-only).
}

bool UVoxelPaintPCGDataProvider::PrepareForExecute_GameThread(UPCGDataBinding* InBinding)
{
	if (!Super::PrepareForExecute_GameThread(InBinding))
	{
		return false;
	}

	// Attribute ids resolve here, not in Initialize: the binding's attribute table is
	// guaranteed initialized by this hook (spawner precedent, SMSDI PrepareForExecute). The
	// names were declared statically via GetKernelAttributeKeys, so the ids are normally
	// present (exceptions handled below).
	// ENGINE ADAPTATION (do-not-call class): UPCGDataBinding::GetAttributeId is an inline
	// wrapper over the UNEXPORTED FPCGKernelAttributeTable::GetAttributeId - calling it from
	// this module fails to link. Resolve the identical table ids through the exported path
	// instead: GetCachedKernelPinDataDesc (PCG_API) + FPCGDataCollectionDesc::GetAttributeDesc
	// (exported); the desc's AttributeId IS the binding attribute-table id (it is filled from
	// InBinding->GetAttributeId inside the PCG module, PCGDataDescription.cpp).
	FName ChannelNames[4];
	UVoxelPaintPCGSampleSettings::GetChannelAttributeNames(ChannelNames);
	const TSharedPtr<const FPCGDataCollectionDesc> OutputPinDesc =
		InBinding->GetCachedKernelPinDataDesc(GetProducerKernel(), PCGPinConstants::DefaultOutputLabel, /*bIsInput=*/false);
	// A legitimately empty pin (zero data descriptions) resolves no attributes by construction
	// (GetAttributeDesc needs the attribute present on at least one data) - mute the ensure for
	// that case (spawner precedent: PCGStaticMeshSpawnerDataInterface mutes its unresolved-
	// selector error on empty data), keeping it armed for the genuine failure: static-attribute-
	// table overflow (PCG logs the error, still compiles+executes, and FPCGDataDesc::AddAttribute
	// silently drops the attribute). An unresolved -1 stays safe downstream because the kernel
	// guards every Out_SetFloat on id >= 0 (the data-collection setters do NOT no-op on -1).
	const bool bOutputHasData = OutputPinDesc.IsValid() && !OutputPinDesc->GetDataDescriptions().IsEmpty();
	for (int32 ChannelIndex = 0; ChannelIndex < 4; ++ChannelIndex)
	{
		int32 AttributeId = INDEX_NONE;
		if (OutputPinDesc.IsValid())
		{
			FPCGKernelAttributeDesc AttributeDesc;
			bool bConflictingTypesFound = false;
			bool bPresentOnAllData = false;
			if (OutputPinDesc->GetAttributeDesc(ChannelNames[ChannelIndex], AttributeDesc, bConflictingTypesFound, bPresentOnAllData)
				&& AttributeDesc.GetAttributeKey().GetType() == EPCGKernelAttributeType::Float)
			{
				AttributeId = AttributeDesc.GetAttributeId();
			}
		}
		ChannelAttributeIds[ChannelIndex] = AttributeId;
		ensure(!bOutputHasData || ChannelAttributeIds[ChannelIndex] != INDEX_NONE);
	}

	const UVoxelPaintPCGSampleSettings* Settings =
		CastChecked<UVoxelPaintPCGSampleSettings>(GetProducerKernel()->GetSettings());
	DensityWriteChannel = Settings->bWriteDensity ? FMath::Clamp(Settings->DensityChannel, 0, 3) : INDEX_NONE;

	return true;
}

FComputeDataProviderRenderProxy* UVoxelPaintPCGDataProvider::GetRenderProxy()
{
	// ALWAYS returns a proxy: a null proxy fails the compute worker's provider ensure; data
	// absence is expressed by the proxy's neutral bindings instead.
	return new FVoxelPaintPCGDataProviderProxy(GenerationBounds, ChannelAttributeIds, DensityWriteChannel
#if WITH_EDITOR
		, CapturedWorldKey
#endif
	);
}

void UVoxelPaintPCGDataProvider::Reset()
{
	GenerationBounds = FBox(EForceInit::ForceInit);
	ChannelAttributeIds = FIntVector4(INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE);
	DensityWriteChannel = INDEX_NONE;
#if WITH_EDITOR
	CapturedWorldKey = FObjectKey();
#endif

	Super::Reset();
}

FVoxelPaintPCGDataProviderProxy::FVoxelPaintPCGDataProviderProxy(
	const FBox& InGenerationBounds,
	const FIntVector4& InChannelAttributeIds,
	int32 InDensityWriteChannel
#if WITH_EDITOR
	, const FObjectKey& InCapturedWorldKey
#endif
	)
	: GenerationBounds(InGenerationBounds)
	, ChannelAttributeIds(InChannelAttributeIds)
	, DensityWriteChannel(InDensityWriteChannel)
#if WITH_EDITOR
	, CapturedWorldKey(InCapturedWorldKey)
#endif
{
}

bool FVoxelPaintPCGDataProviderProxy::IsValid(FValidationData const& InValidationData) const
{
	// Struct-size check only (landscape/spawner shape). Resource absence is neutral-bound in
	// AllocateResources, never invalidated - IsValid runs BEFORE AllocateResources and a
	// false here kills the whole graph submission.
	return InValidationData.ParameterStructSize == sizeof(FParameters);
}

void FVoxelPaintPCGDataProviderProxy::AllocateResources(FRDGBuilder& GraphBuilder, FAllocationData const& InAllocationData)
{
	// Neutral-first: every binding gets a valid default, and the live path overwrites
	// wholesale. Any bail-out below therefore leaves a TOTAL neutral set (the kernel then
	// outputs exact-zero weights) - never a null bind, never a partial set.
	FVoxelPaintExternalSamplingParams Params;
	WorldVoxelPaint::BuildNeutralExternalSamplingParams(Params, GraphBuilder);

	CulledRecordIndices.Reset();

	// Live registration. Coordinator reached ON the render thread, per submission, never
	// cached across frames; nullptr (never created / torn down) falls through neutral.
	if (FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
	{
		FVoxelPaintExternalSamplingParams LiveParams;
		if (Coordinator->RegisterForExternalSampling(GraphBuilder, LiveParams))
		{
			bool bWorldMatches = true;
#if WITH_EDITOR
			// PIE-vs-editor guard: the singleton coordinator publishes ONE master world's
			// records; a PCG task executing in any other world must sample neutral (the
			// records are another world's placements). Editor-only by contract - cooked
			// builds have no member and no reachable multi-world case (spec-core D4).
			bWorldMatches = (LiveParams.MasterWorldKey == CapturedWorldKey);
#endif
			if (bWorldMatches)
			{
				Params = LiveParams;
			}
		}
	}

	// CPU record cull against the PCG component's generation bounds (spec-inputs G): records
	// live in Params.Records (CPU mirror, same count/order as the GPU buffer, centers
	// evaluation-origin-relative). Ascending record-index order falls out of the loop order
	// and is REQUIRED (float-accumulation determinism parity with the grid path). The list is
	// capped at the shared per-point candidate constant - the grid-free sampler clamps its
	// loop to the same 32, so entries past the cap would be dead weight; the drop mirrors the
	// material path's per-cell overfull-drop semantics.
	if (Params.NumVolumes > 0)
	{
		const FVector EvaluationOrigin = FVector(Params.EvaluationOriginHigh) + FVector(Params.EvaluationOriginLow);
		const bool bCullToBounds = GenerationBounds.IsValid != 0;
		const FBox RelativeBounds = bCullToBounds ? GenerationBounds.ShiftBy(-EvaluationOrigin) : FBox(EForceInit::ForceInit);
		const int32 RecordCap = static_cast<int32>(WorldVoxelPaint::MaxEvaluatedCandidatesPerCell);

		for (int32 RecordIndex = 0; RecordIndex < Params.Records.Num() && CulledRecordIndices.Num() < RecordCap; ++RecordIndex)
		{
			const FVoxelPaintVolumeRecord& Record = Params.Records[RecordIndex];
			if ((Record.Flags & WorldVoxelPaint::RecordFlag_Valid) == 0u)
			{
				continue;
			}
			if (bCullToBounds)
			{
				// Conservative OBB -> world AABB: widen the volume-local half extent by the
				// shader's containment epsilon ((1/64) * TexelSize,
				// WORLDVOXELPAINT_CONTAINMENT_EPSILON) FIRST - the GPU test admits
				// |local| <= LocalHalfExtent + epsilon in the volume-local frame, so the
				// epsilon must be rotated with the box (adding it unrotated in world axes
				// under-widens by up to sqrt(3)x for rotated volumes) - then rotate the three
				// widened axes into world space (volume->world = inverse of the packed
				// world->volume quat) and sum absolute components, so the CPU cull never
				// rejects a record the GPU containment test would admit.
				const FQuat4f VolumeToWorld = Record.WorldToVolumeRotation.Inverse();
				const FVector3f WidenedHalfExtent = Record.LocalHalfExtent + Record.TexelSize * (1.0f / 64.0f);
				const FVector3f WorldExtent =
					VolumeToWorld.RotateVector(FVector3f(WidenedHalfExtent.X, 0.0f, 0.0f)).GetAbs()
					+ VolumeToWorld.RotateVector(FVector3f(0.0f, WidenedHalfExtent.Y, 0.0f)).GetAbs()
					+ VolumeToWorld.RotateVector(FVector3f(0.0f, 0.0f, WidenedHalfExtent.Z)).GetAbs();
				const FVector RecordCenter(Record.VolumeCenter);
				const FBox RecordBounds(RecordCenter - FVector(WorldExtent), RecordCenter + FVector(WorldExtent));
				if (!RelativeBounds.Intersect(RecordBounds))
				{
					continue;
				}
			}
			CulledRecordIndices.Add(static_cast<uint32>(RecordIndex));
		}
	}

	NumRecordIndices = static_cast<uint32>(CulledRecordIndices.Num());
	if (CulledRecordIndices.IsEmpty())
	{
		// Zero-survivor case: the binding must still be a valid SRV (TOTAL-binding rule), but
		// the buffer is never read - NumRecordIndices stays 0 and the sampler clamps its loop
		// to zero iterations. Upload one dummy 0u entry through the SAME path as the live list
		// (a zero-element structured buffer cannot be created; the parity test uses the
		// identical non-empty-buffer idiom). Deliberately NOT
		// GSystemTextures.GetDefaultStructuredBuffer: GSystemTextures is RENDERER_API (Renderer
		// module, SystemTextures.h:261), and this module does not link Renderer.
		CulledRecordIndices.Add(0u);
	}
	const FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CulledRecordIndices.Num());
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(BufferDesc, TEXT("WorldVoxelPaintPCG.CulledRecordIndices"));
	RecordIndicesSRV = GraphBuilder.CreateSRV(Buffer);
	GraphBuilder.QueueBufferUpload(Buffer, MakeArrayView(CulledRecordIndices));

	// Snapshot the remaining bindings into proxy members: Params (and in particular the
	// Records view) is valid only inside this callback.
	VolumesSRV = Params.Volumes;
	PageTablesSRV = Params.PageTables;
	BrickPool = Params.BrickPool;
	SessionTexture = Params.SessionTexture;
	PoolSampler = Params.PoolSampler;
	NumVolumes = Params.NumVolumes;
	BrickSize = Params.BrickSize;
	UniqueBrickSize = Params.UniqueBrickSize;
	PoolDimsInBricks = Params.PoolDimsInBricks;
	PoolTexelExtentRcp = Params.PoolTexelExtentRcp;
	EvaluationOriginHigh = Params.EvaluationOriginHigh;
	EvaluationOriginLow = Params.EvaluationOriginLow;
	CameraCenter = Params.CameraCenter;
	EvaluationRadiusSq = Params.EvaluationRadiusSq;
}

void FVoxelPaintPCGDataProviderProxy::GatherDispatchData(FDispatchData const& InDispatchData)
{
	// Identical parameters for every invocation (landscape shape) - the bindings are
	// per-component, not per-data-item. No RDG calls here; the builder is not available.
	const TStridedView<FParameters> ParameterArray = MakeStridedParameterView<FParameters>(InDispatchData);
	for (int32 InvocationIndex = 0; InvocationIndex < ParameterArray.Num(); ++InvocationIndex)
	{
		FParameters& Parameters = ParameterArray[InvocationIndex];
		Parameters.Volumes = VolumesSRV;
		Parameters.PageTables = PageTablesSRV;
		Parameters.RecordIndices = RecordIndicesSRV;
		Parameters.BrickPool = BrickPool;
		Parameters.SessionTexture = SessionTexture;
		Parameters.PoolSampler = PoolSampler;
		Parameters.NumRecordIndices = NumRecordIndices;
		Parameters.NumVolumes = NumVolumes;
		Parameters.BrickSize = BrickSize;
		Parameters.UniqueBrickSize = UniqueBrickSize;
		Parameters.PoolDimsInBricks = PoolDimsInBricks;
		Parameters.PoolTexelExtentRcp = PoolTexelExtentRcp;
		Parameters.EvaluationOriginHigh = EvaluationOriginHigh;
		Parameters.EvaluationOriginLow = EvaluationOriginLow;
		Parameters.CameraCenter = CameraCenter;
		Parameters.EvaluationRadiusSq = EvaluationRadiusSq;
		Parameters.ChannelAttributeIds = ChannelAttributeIds;
		Parameters.DensityWriteChannel = DensityWriteChannel;
	}
}
