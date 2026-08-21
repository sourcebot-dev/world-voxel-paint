// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorValidatorBase.h"
#include "Templates/Function.h"
#include "VoxelPaintSessionMachine.h"
#include "VoxelPaintTypes.h"

#include "VoxelPaintAssetValidator.generated.h"

class UVoxelPaintVolumeData;
class UWorld;

struct FVoxelPaintReferenceRecord
{
	FGuid ActorGuid;
	FName SourcePackage;
	bool bLoaded = false;
};

enum class EVoxelPaintMultiRefVerdict : uint8
{
	Legitimate,
	PrivateMultiReference
};

EVoxelPaintMultiRefVerdict ClassifyReferences(EVoxelPaintAssetSharingMode Mode,
	TConstArrayView<FVoxelPaintReferenceRecord> Records, int32& OutLogicalPlacements);

void GatherReferenceRecords(const UVoxelPaintVolumeData* Asset,
	TArray<FVoxelPaintReferenceRecord>& OutRecords);

UCLASS()
class UVoxelPaintAssetValidator : public UEditorValidatorBase
{
	GENERATED_BODY()

public:
	using FStageQueryOverride =
		TFunction<WorldVoxelPaint::EVoxelPaintSessionStage(const UVoxelPaintVolumeData*)>;
	static void SetStageQueryOverrideForTest(FStageQueryOverride Override);

	using FEnumerableWorldOverride = TFunction<bool(const UWorld*)>;
	static void SetEnumerableWorldOverrideForTest(FEnumerableWorldOverride Override);

protected:
	virtual bool CanValidateAsset_Implementation(const FAssetData& InAssetData, UObject* InObject,
		FDataValidationContext& InContext) const override;
	virtual EDataValidationResult ValidateLoadedAsset_Implementation(const FAssetData& InAssetData,
		UObject* InAsset, FDataValidationContext& Context) override;
};
