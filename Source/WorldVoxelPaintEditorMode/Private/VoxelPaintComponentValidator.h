// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "EditorValidatorBase.h"
#include "VoxelPaintTypes.h"
#include "VoxelPaintVariantSelection.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"

#include "VoxelPaintComponentValidator.generated.h"

namespace WorldVoxelPaint
{
	enum class EVoxelPaintComponentRule : uint8
	{
		TopologyMismatch,
		DuplicateEntry,
		NullEntry,
		DanglingPin,
		ScsPoolDivergence,
		PrivateEntryInScsPool,
		ForeignPrivateEntry,
		UcsProvenance,
		NativeBlueprintCarrier,
		NonUnitEffectiveScale,
		AllZeroWeights
	};

	enum class EVoxelPaintComponentFindingSeverity : uint8
	{
		Error,
		Warning,
		Note
	};

	struct FVoxelPaintComponentRuleEntry
	{
		bool bHasAsset = false;
		FString AssetName;
		FVoxelPaintTopologyTriplet Topology;
		EVoxelPaintAssetSharingMode SharingMode = EVoxelPaintAssetSharingMode::Private;
		bool bOwnedByThisPlacement = false;
		FString OwnerDisplayName;
	};

	struct FVoxelPaintComponentRuleInputs
	{
		FVoxelPaintPoolView Pool;

		// Parallel to Pool.Assets; entry fields are meaningful only when bHasAsset.
		TArray<FVoxelPaintComponentRuleEntry> Entries;

		FVoxelPaintExpectedTopology Expectation;

		bool bPinSet = false;
		bool bPinInPool = false;
		FString PinAssetName;

		EComponentCreationMethod CreationMethod = EComponentCreationMethod::Native;
		bool bIsTemplate = false;
		bool bOwnerClassIsBlueprintGeneratedClass = false;

		bool bHasArchetype = false;
		TArray<const UVoxelPaintVolumeData*> ArchetypeAssets;
		TArray<float> ArchetypeWeights;
		FVoxelPaintExpectedTopology ArchetypeExpectation;

		float MaxScaleDeviation = 0.0f;
	};

	struct FVoxelPaintComponentFinding
	{
		EVoxelPaintComponentRule Rule = EVoxelPaintComponentRule::TopologyMismatch;
		EVoxelPaintComponentFindingSeverity Severity = EVoxelPaintComponentFindingSeverity::Error;
		FText Message;
	};

	void BuildComponentRuleInputs(const UVoxelPaintVolumeComponent& Component,
		FVoxelPaintComponentRuleInputs& OutInputs);

	void ValidateComponentPoolState(const FVoxelPaintComponentRuleInputs& Inputs,
		TArray<FVoxelPaintComponentFinding>& OutFindings);
}

UCLASS()
class UVoxelPaintComponentValidator : public UEditorValidatorBase
{
	GENERATED_BODY()

protected:
	virtual bool CanValidateAsset_Implementation(const FAssetData& InAssetData, UObject* InObject,
		FDataValidationContext& InContext) const override;
	virtual EDataValidationResult ValidateLoadedAsset_Implementation(const FAssetData& InAssetData,
		UObject* InAsset, FDataValidationContext& Context) override;
};
