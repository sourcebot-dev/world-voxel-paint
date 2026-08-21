// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintAssetValidator.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/ArrayView.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelUtils.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/DataValidation.h"
#include "Templates/Casts.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "VoxelPaintEditorWorldFilter.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"

#define LOCTEXT_NAMESPACE "WorldVoxelPaint"

namespace
{

UVoxelPaintAssetValidator::FStageQueryOverride GStageQueryOverrideForTest;
UVoxelPaintAssetValidator::FEnumerableWorldOverride GEnumerableWorldOverrideForTest;

WorldVoxelPaint::EVoxelPaintSessionStage QuerySessionStage(const UVoxelPaintVolumeData* Asset)
{
	if (GStageQueryOverrideForTest)
	{
		return GStageQueryOverrideForTest(Asset);
	}
	if (const FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated())
	{
		return Controller->GetStageForAsset(Asset);
	}
	return WorldVoxelPaint::EVoxelPaintSessionStage::Closed;
}

bool PassesEnumerableWorldFilter(const UWorld* World)
{
	if (GEnumerableWorldOverrideForTest)
	{
		return GEnumerableWorldOverrideForTest(World);
	}
	return WorldVoxelPaint::IsEnumerableWorld(World);
}

}

void GatherReferenceRecords(const UVoxelPaintVolumeData* Asset,
	TArray<FVoxelPaintReferenceRecord>& OutRecords)
{
	TSet<FName> CoveredPackages;
	for (TObjectIterator<UVoxelPaintVolumeComponent> It; It; ++It)
	{
		const UVoxelPaintVolumeComponent* Component = *It;
		if (!IsValid(Component) || Component->IsTemplate() || !Component->PoolContains(Asset))
		{
			continue;
		}
		const AActor* Owner = Component->GetOwner();
		if (!Owner || !PassesEnumerableWorldFilter(Owner->GetWorld()))
		{
			continue;
		}

		// ULevelStreaming::FindStreamingLevel is not exported in 5.7; FLevelUtils' wrapper is.
		const ULevelStreaming* StreamingLevel = FLevelUtils::FindStreamingLevel(Owner->GetLevel());
		FName SourcePackage;
		if (StreamingLevel && !StreamingLevel->PackageNameToLoad.IsNone())
		{
			SourcePackage = StreamingLevel->PackageNameToLoad;
		}
		else if (const UPackage* ActorPackage = Owner->GetPackage())
		{
			SourcePackage = ActorPackage->GetFName();
		}

		if (const UPackage* ActorPackage = Owner->GetPackage())
		{
			CoveredPackages.Add(ActorPackage->GetFName());
			CoveredPackages.Add(ActorPackage->GetLoadedPath().GetPackageFName());
		}
		CoveredPackages.Add(SourcePackage);

		FVoxelPaintReferenceRecord& Record = OutRecords.AddDefaulted_GetRef();
		Record.ActorGuid = Owner->GetActorGuid();
		Record.SourcePackage = SourcePackage;
		Record.bLoaded = true;
	}

	const IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
	const UPackage* AssetPackage = Asset->GetPackage();
	if (!AssetRegistry || !AssetPackage)
	{
		return;
	}
	TArray<FName> ReferencerPackages;
	AssetRegistry->GetReferencers(AssetPackage->GetFName(), ReferencerPackages);
	for (const FName ReferencerPackage : ReferencerPackages)
	{
		if (CoveredPackages.Contains(ReferencerPackage)
			|| FindPackage(nullptr, *ReferencerPackage.ToString()) != nullptr)
		{
			continue;
		}
		FVoxelPaintReferenceRecord& Record = OutRecords.AddDefaulted_GetRef();
		Record.SourcePackage = ReferencerPackage;
		Record.bLoaded = false;
	}
}

EVoxelPaintMultiRefVerdict ClassifyReferences(EVoxelPaintAssetSharingMode Mode,
	TConstArrayView<FVoxelPaintReferenceRecord> Records, int32& OutLogicalPlacements)
{
	TSet<TPair<FGuid, FName>> LogicalPlacements;
	for (const FVoxelPaintReferenceRecord& Record : Records)
	{
		if (!Record.bLoaded || !Record.ActorGuid.IsValid())
		{
			continue;
		}
		LogicalPlacements.Add(TPair<FGuid, FName>(Record.ActorGuid, Record.SourcePackage));
	}
	OutLogicalPlacements = LogicalPlacements.Num();

	if (Mode == EVoxelPaintAssetSharingMode::Shared)
	{
		return EVoxelPaintMultiRefVerdict::Legitimate;
	}
	return OutLogicalPlacements > 1
		? EVoxelPaintMultiRefVerdict::PrivateMultiReference
		: EVoxelPaintMultiRefVerdict::Legitimate;
}

void UVoxelPaintAssetValidator::SetStageQueryOverrideForTest(FStageQueryOverride Override)
{
	GStageQueryOverrideForTest = MoveTemp(Override);
}

void UVoxelPaintAssetValidator::SetEnumerableWorldOverrideForTest(FEnumerableWorldOverride Override)
{
	GEnumerableWorldOverrideForTest = MoveTemp(Override);
}

bool UVoxelPaintAssetValidator::CanValidateAsset_Implementation(const FAssetData& InAssetData,
	UObject* InObject, FDataValidationContext& InContext) const
{
	return Cast<UVoxelPaintVolumeData>(InObject) != nullptr;
}

EDataValidationResult UVoxelPaintAssetValidator::ValidateLoadedAsset_Implementation(
	const FAssetData& InAssetData, UObject* InAsset, FDataValidationContext& Context)
{
	UVoxelPaintVolumeData* Asset = Cast<UVoxelPaintVolumeData>(InAsset);
	if (!Asset)
	{
		return EDataValidationResult::NotValidated;
	}

	bool bFailed = false;

	const WorldVoxelPaint::EVoxelPaintSessionStage Stage = QuerySessionStage(Asset);
	if (Stage == WorldVoxelPaint::EVoxelPaintSessionStage::Active
		|| Stage == WorldVoxelPaint::EVoxelPaintSessionStage::Sealing
		|| Stage == WorldVoxelPaint::EVoxelPaintSessionStage::Canonicalizing)
	{
		AssetFails(Asset, LOCTEXT("ValidatorUncommittedSession",
			"Finish or Discard the open paint session, save, then submit again."));
		bFailed = true;
	}

	TArray<FVoxelPaintReferenceRecord> Records;
	GatherReferenceRecords(Asset, Records);

	for (const FVoxelPaintReferenceRecord& Record : Records)
	{
		if (!Record.bLoaded)
		{
			AssetMessage(EMessageSeverity::Info, FText::Format(LOCTEXT("ValidatorUnloadedReferencer",
				"Unloaded package '{0}' references this asset and was not classified for multi-reference."),
				FText::FromName(Record.SourcePackage)));
		}
	}

	int32 NumLogicalPlacements = 0;
	if (ClassifyReferences(Asset->SharingMode, Records, NumLogicalPlacements)
		== EVoxelPaintMultiRefVerdict::PrivateMultiReference)
	{
		AssetFails(Asset, FText::Format(LOCTEXT("ValidatorPrivateMultiReference",
			"{0} logical placements reference this Private asset; mark Shared or Make Unique."),
			FText::AsNumber(NumLogicalPlacements)));
		bFailed = true;
	}

	if (!bFailed)
	{
		AssetPasses(Asset);
		return EDataValidationResult::Valid;
	}
	return EDataValidationResult::Invalid;
}

#undef LOCTEXT_NAMESPACE
