// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Compute/PCGComputeKernel.h"

#include "VoxelPaintPCGSampleKernel.generated.h"

/**
 * Compute kernel emitted by UVoxelPaintPCGSampleSettings (M6 plan 6.2): one thread per input
 * point, pass-through point processor that adds the four channel-weight attributes and
 * optionally overwrites Density. The voxel field is bound by UVoxelPaintPCGDataInterface
 * (additional input DI); the kernel HLSL calls the DI's PLAIN function names
 * (WVPSampler_GetLayerWeights et al.) - ComputeFramework emits the mangled shims.
 *
 * WITH_EDITOR gating below is the exact PCGComputeKernel.h split (spec-inputs E): source-path/
 * entry-point/DI-creation/additional-sources are editor compile-time hooks; pin, thread-count,
 * data-description, and attribute-key queries are UNGATED because kernels are
 * UPROPERTY-serialized into cooked compute graphs.
 */
UCLASS()
class WORLDVOXELPAINTPCG_API UVoxelPaintPCGSampleKernel : public UPCGComputeKernel
{
	GENERATED_BODY()

public:
	virtual TSharedPtr<const FPCGDataCollectionDesc> ComputeOutputBindingDataDesc(FName InOutputPinLabel, UPCGDataBinding* InBinding) const override;
	virtual int ComputeThreadCount(const UPCGDataBinding* Binding) const override;
#if WITH_EDITOR
	virtual const TCHAR* GetSourceFilePath() const override { return TEXT("/Plugin/WorldVoxelPaintPCG/VoxelPaintPCGSample.usf"); }
	virtual FString GetEntryPoint() const override { return TEXT("VoxelPaintPCGSampleCS"); }
	virtual void CreateAdditionalInputDataInterfaces(FPCGGPUCompilationContext& InOutContext, UObject* InObjectOuter, TArray<TObjectPtr<UComputeDataInterface>>& OutDataInterfaces) const override;
	virtual void GatherAdditionalSources(TArray<TObjectPtr<class UComputeSource>>& OutAdditionalSources) const override;
#endif
	virtual void GetKernelAttributeKeys(TArray<FPCGKernelAttributeKey>& OutKeys) const override;
	virtual void GetInputPins(TArray<FPCGPinProperties>& OutPins) const override;
	virtual void GetOutputPins(TArray<FPCGPinPropertiesGPU>& OutPins) const override;
};
