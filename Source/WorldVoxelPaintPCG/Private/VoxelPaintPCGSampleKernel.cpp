// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintPCGSampleKernel.h"

#include "VoxelPaintPCGDataInterface.h"
#include "VoxelPaintPCGSampleSettings.h"

#include "PCGCommon.h"
#include "PCGPointPropertiesTraits.h"
#include "Compute/PCGComputeCommon.h"
#include "Compute/PCGDataBinding.h"
#include "Compute/PCGDataDescription.h"
#include "Compute/PCGPinPropertiesGPU.h"
#include "Graph/PCGGPUGraphCompilationContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VoxelPaintPCGSampleKernel)

TSharedPtr<const FPCGDataCollectionDesc> UVoxelPaintPCGSampleKernel::ComputeOutputBindingDataDesc(FName InOutputPinLabel, UPCGDataBinding* InBinding) const
{
	check(InBinding);

	// Code assumes the single default output pin.
	if (!ensure(InOutputPinLabel == PCGPinConstants::DefaultOutputLabel))
	{
		return nullptr;
	}

	// Forward the input description, then add the node's four channel attributes
	// (StaticMeshSpawnerKernel pattern; ComputeDataDescFromPinProperties is unexported and
	// must not be called from this module).
	const FPCGKernelPin InputKernelPin(GetKernelIndex(), PCGPinConstants::DefaultInputLabel, /*bIsInput=*/true);
	const TSharedPtr<const FPCGDataCollectionDesc> InputDesc = InBinding->ComputeKernelPinDataDesc(InputKernelPin);
	if (!ensure(InputDesc))
	{
		return nullptr;
	}

	TSharedPtr<FPCGDataCollectionDesc> OutDesc = FPCGDataCollectionDesc::MakeSharedFrom(InputDesc);

	FName ChannelNames[4];
	UVoxelPaintPCGSampleSettings::GetChannelAttributeNames(ChannelNames);
	for (int32 ChannelIndex = 0; ChannelIndex < 4; ++ChannelIndex)
	{
		OutDesc->AddAttributeToAllData(
			FPCGKernelAttributeKey(ChannelNames[ChannelIndex], EPCGKernelAttributeType::Float), InBinding);
	}

	// Density varies per point when written; allocate per-point storage wherever points vary
	// (Transform allocated) - the UPCGNormalToDensityKernel rule, verbatim.
	const UVoxelPaintPCGSampleSettings* Settings = CastChecked<UVoxelPaintPCGSampleSettings>(GetSettings());
	if (Settings->bWriteDensity)
	{
		for (FPCGDataDesc& DataDesc : OutDesc->GetDataDescriptionsMutable())
		{
			if (!!(DataDesc.GetAllocatedProperties() & EPCGPointNativeProperties::Transform))
			{
				DataDesc.AllocateProperties(EPCGPointNativeProperties::Density);
			}
		}
	}

	return OutDesc;
}

int UVoxelPaintPCGSampleKernel::ComputeThreadCount(const UPCGDataBinding* InBinding) const
{
	// One thread per INPUT point (spec-inputs C7; StaticMeshSpawnerKernel.cpp:118-122
	// precedent - for this 1:1 pass-through the output count is identical, but the input pin
	// is the locked convention).
	const TSharedPtr<const FPCGDataCollectionDesc> InputPinDesc =
		InBinding->GetCachedKernelPinDataDesc(this, PCGPinConstants::DefaultInputLabel, /*bIsInput=*/true);
	return ensure(InputPinDesc) ? InputPinDesc->ComputeTotalElementCount() : 0;
}

#if WITH_EDITOR

void UVoxelPaintPCGSampleKernel::CreateAdditionalInputDataInterfaces(FPCGGPUCompilationContext& InOutContext, UObject* InObjectOuter, TArray<TObjectPtr<UComputeDataInterface>>& OutDataInterfaces) const
{
	Super::CreateAdditionalInputDataInterfaces(InOutContext, InObjectOuter, OutDataInterfaces);

	// SetProducerKernel is the exported route to node settings (SetProducerSettings is
	// unexported - never call it); the provider reads GetProducerKernel()->GetSettings().
	TObjectPtr<UVoxelPaintPCGDataInterface> NodeDI = InOutContext.NewObject_AnyThread<UVoxelPaintPCGDataInterface>(InObjectOuter);
	NodeDI->SetProducerKernel(this);
	OutDataInterfaces.Add(NodeDI);
}

void UVoxelPaintPCGSampleKernel::GatherAdditionalSources(TArray<TObjectPtr<UComputeSource>>& OutAdditionalSources) const
{
	// Deliberately explicit: PCG's stock helper sources (PCGCS_ShaderUtils/-Internal,
	// PCGCS_Quaternion) supply PCG_COPY_ALL_ATTRIBUTES_TO_OUTPUT, ComputeSeed, FRand and the
	// quaternion helpers - skipping the Super call silently loses them (spec-inputs F).
	Super::GatherAdditionalSources(OutAdditionalSources);
}

#endif // WITH_EDITOR

void UVoxelPaintPCGSampleKernel::GetKernelAttributeKeys(TArray<FPCGKernelAttributeKey>& OutKeys) const
{
	// Registers the four channel attributes in the compute graph's static attribute table at
	// compile time (96-cap table); names must match ComputeOutputBindingDataDesc and the
	// provider's game-thread id resolution - all three derive from GetChannelAttributeNames.
	FName ChannelNames[4];
	UVoxelPaintPCGSampleSettings::GetChannelAttributeNames(ChannelNames);
	for (int32 ChannelIndex = 0; ChannelIndex < 4; ++ChannelIndex)
	{
		OutKeys.AddUnique(FPCGKernelAttributeKey(ChannelNames[ChannelIndex], EPCGKernelAttributeType::Float));
	}
}

void UVoxelPaintPCGSampleKernel::GetInputPins(TArray<FPCGPinProperties>& OutPins) const
{
	OutPins.Emplace(PCGPinConstants::DefaultInputLabel, EPCGDataType::Point);
}

void UVoxelPaintPCGSampleKernel::GetOutputPins(TArray<FPCGPinPropertiesGPU>& OutPins) const
{
	OutPins.Emplace(PCGPinConstants::DefaultOutputLabel, EPCGDataType::Point);
}
