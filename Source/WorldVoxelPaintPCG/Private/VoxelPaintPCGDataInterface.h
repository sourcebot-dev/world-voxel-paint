// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Compute/DataInterfaces/PCGComputeDataInterface.h"
#include "ComputeFramework/ComputeDataProvider.h"
#include "RenderGraphFwd.h"
#include "RHIResources.h"
#include "UObject/ObjectKey.h"

#include "VoxelPaintPCGDataInterface.generated.h"

class FVoxelPaintPCGDataInterfaceParameters;

/**
 * Additional-input compute data interface binding the published World Voxel Paint resources
 * into a PCG GPU kernel (M6 plan 6.3; modeled on PCGLandscapeDataInterface, created via the
 * spawner-DI additional-input route). Compile-time only object state; per-execution state
 * lives on the provider/proxy. Runtime class on purpose: the DI is serialized into cooked
 * compute graphs; GetHLSL/GetShaderHash are simply never called outside editor graph compile.
 */
UCLASS(ClassGroup = (Procedural))
class UVoxelPaintPCGDataInterface : public UPCGComputeDataInterface
{
	GENERATED_BODY()

public:
	//~ Begin UComputeDataInterface Interface
	TCHAR const* GetClassName() const override { return TEXT("WVPVoxelPaint"); }
	void GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const override;
	void GetShaderParameters(TCHAR const* UID, FShaderParametersMetadataBuilder& InOutBuilder, FShaderParametersMetadataAllocations& InOutAllocations) const override;
	TCHAR const* GetShaderVirtualPath() const override;
	void GetShaderHash(FString& InOutKey) const override;
	void GetHLSL(FString& OutHLSL, FString const& InDataInterfaceName) const override;
	UComputeDataProvider* CreateDataProvider() const override;
	//~ End UComputeDataInterface Interface

private:
	static TCHAR const* TemplateFilePath;
};

/**
 * Game-thread provider. Lifecycle facts this class is written against (spec-inputs C8):
 * Initialize runs once per graph-task execution AND re-runs on pooled FComputeGraphInstance
 * reuse - every capture is unconditionally overwritten; PrepareForExecute_GameThread runs per
 * execution after the binding's attribute table exists (attribute ids resolve there, spawner
 * precedent); GetRenderProxy runs once per EnqueueWork and NEVER returns null; Reset clears.
 */
UCLASS()
class UVoxelPaintPCGDataProvider : public UPCGComputeDataProvider
{
	GENERATED_BODY()

public:
	//~ Begin UComputeDataProvider Interface
	virtual void Initialize(const UComputeDataInterface* InDataInterface, UObject* InBinding, uint64 InInputMask, uint64 InOutputMask) override;
	virtual bool PrepareForExecute_GameThread(UPCGDataBinding* InBinding) override;
	virtual FComputeDataProviderRenderProxy* GetRenderProxy() override;
	virtual void Reset() override;
	//~ End UComputeDataProvider Interface

private:
	/** PCG component generation bounds (world space), from the execution state. */
	FBox GenerationBounds = FBox(EForceInit::ForceInit);

	/** Output attribute ids for channels 0..3, resolved on the game thread per execution. */
	FIntVector4 ChannelAttributeIds = FIntVector4(INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE);

	/** Density channel to write (0..3), or INDEX_NONE when bWriteDensity is off. */
	int32 DensityWriteChannel = INDEX_NONE;

#if WITH_EDITOR
	/** Identity of the executing world; compared against the coordinator's master world on the render thread (PIE-vs-editor guard). */
	FObjectKey CapturedWorldKey;
#endif
};

/** Render-thread proxy; one instance per EnqueueWork, deleted by ~FGraphInvocation after submission. */
class FVoxelPaintPCGDataProviderProxy : public FComputeDataProviderRenderProxy
{
public:
	FVoxelPaintPCGDataProviderProxy(
		const FBox& InGenerationBounds,
		const FIntVector4& InChannelAttributeIds,
		int32 InDensityWriteChannel
#if WITH_EDITOR
		, const FObjectKey& InCapturedWorldKey
#endif
	);

	//~ Begin FComputeDataProviderRenderProxy Interface
	bool IsValid(FValidationData const& InValidationData) const override;
	void AllocateResources(FRDGBuilder& GraphBuilder, FAllocationData const& InAllocationData) override;
	void GatherDispatchData(FDispatchData const& InDispatchData) override;
	//~ End FComputeDataProviderRenderProxy Interface

protected:
	using FParameters = FVoxelPaintPCGDataInterfaceParameters;

	// Game-thread snapshot (constructor).
	FBox GenerationBounds;
	FIntVector4 ChannelAttributeIds;
	int32 DensityWriteChannel;
#if WITH_EDITOR
	FObjectKey CapturedWorldKey;
#endif

	// Filled by AllocateResources; TOTAL in both the live and neutral paths - a null binding
	// in any path is an RDG validation error IsValid cannot catch (it runs before allocation
	// and checks struct size only).
	TArray<uint32> CulledRecordIndices;               // ascending record order; owns the upload memory
	FRDGBufferSRVRef VolumesSRV = nullptr;
	FRDGBufferSRVRef PageTablesSRV = nullptr;
	FRDGBufferSRVRef RecordIndicesSRV = nullptr;
	FTextureRHIRef BrickPool;
	FTextureRHIRef SessionTexture;
	FRHISamplerState* PoolSampler = nullptr;
	uint32 NumRecordIndices = 0;
	uint32 NumVolumes = 0;
	uint32 BrickSize = 0;
	uint32 UniqueBrickSize = 0;
	FIntVector PoolDimsInBricks = FIntVector(1, 1, 1);
	FVector3f PoolTexelExtentRcp = FVector3f::OneVector;
	FVector3f EvaluationOriginHigh = FVector3f::ZeroVector;
	FVector3f EvaluationOriginLow = FVector3f::ZeroVector;
	FVector3f CameraCenter = FVector3f::ZeroVector;
	float EvaluationRadiusSq = 0.0f;
};
