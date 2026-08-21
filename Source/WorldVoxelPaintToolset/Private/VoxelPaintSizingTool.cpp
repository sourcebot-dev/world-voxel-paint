// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintSizingTool.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "InteractiveToolManager.h"
#include "Misc/MessageDialog.h"
#include "ToolContextInterfaces.h"
#include "UObject/UnrealType.h"
#include "VoxelPaintResidencyTypes.h"
#include "VoxelPaintTemplateRouting.h"
#include "VoxelPaintTools.h"
#include "VoxelPaintTopologyOps.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "WorldVoxelPaintConstants.h"

#define LOCTEXT_NAMESPACE "WorldVoxelPaint"

namespace
{
	int32 GetPopulatedBrickCount(const UVoxelPaintVolumeData* Asset)
	{
		const TSharedPtr<const FVoxelPaintSourceManifest> Manifest = Asset->GetManifestSnapshot();
		return Manifest.IsValid() ? Manifest->PopulatedBricks.Num() : 0;
	}
}

void UVoxelPaintSizingToolProperties::SnapToDesiredTexelSize()
{
	if (UVoxelPaintSizingTool* Tool = OwnerTool.Get())
	{
		Tool->SnapToDesiredTexelSize();
	}
}

void UVoxelPaintSizingToolProperties::ResetDataAndUnlockSize()
{
	if (UVoxelPaintSizingTool* Tool = OwnerTool.Get())
	{
		Tool->ResetDataAndUnlockSize();
	}
}

bool UVoxelPaintSizingToolBuilder::CanBuildTool(const FToolBuilderState& SceneState) const
{
	for (AActor* Actor : SceneState.SelectedActors)
	{
		if (Actor)
		{
			if (const UVoxelPaintVolumeComponent* Component = Actor->FindComponentByClass<UVoxelPaintVolumeComponent>())
			{
				if (Component->HasValidVolumeData())
				{
					return true;
				}
			}
		}
	}
	return false;
}

UInteractiveTool* UVoxelPaintSizingToolBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	UVoxelPaintSizingTool* Tool = NewObject<UVoxelPaintSizingTool>(SceneState.ToolManager);
	for (AActor* Actor : SceneState.SelectedActors)
	{
		if (Actor)
		{
			if (UVoxelPaintVolumeComponent* Component = Actor->FindComponentByClass<UVoxelPaintVolumeComponent>())
			{
				if (Component->HasValidVolumeData())
				{
					Tool->SetTargetPlacement(Component);
					break;
				}
			}
		}
	}
	return Tool;
}

void UVoxelPaintSizingTool::SetTargetPlacement(UVoxelPaintVolumeComponent* InPlacement)
{
	TargetPlacement = InPlacement;
}

void UVoxelPaintSizingTool::Setup()
{
	Super::Setup();
	Properties = NewObject<UVoxelPaintSizingToolProperties>(this, TEXT("Sizing"));
	Properties->OwnerTool = this;
	SyncFromAsset();
	AddToolPropertySource(Properties);
}

void UVoxelPaintSizingTool::OnTick(float DeltaTime)
{
	Super::OnTick(DeltaTime);
	const UVoxelPaintVolumeData* Asset = GetAsset();
	if (!Asset)
	{
		return;
	}
	if (Asset->LocalHalfExtent != LastSyncedHalfExtent
		|| Asset->GetBrickCounts() != LastSyncedBrickCounts
		|| Asset->bInitialized != bLastSyncedInitialized
		|| GetPopulatedBrickCount(Asset) != LastSyncedPopulatedBricks)
	{
		SyncFromAsset();
	}
}

void UVoxelPaintSizingTool::OnPropertyModified(UObject* PropertySet, FProperty* Property)
{
	Super::OnPropertyModified(PropertySet, Property);
	if (PropertySet != Properties)
	{
		return;
	}
	const WorldVoxelPaint::FVoxelPaintLadderState NewState = BuildStateFromProperties();
	if (NewState.HalfExtent != LastSyncedHalfExtent || NewState.BrickCounts != LastSyncedBrickCounts)
	{
		CommitState(NewState);
	}
}

void UVoxelPaintSizingTool::SnapToDesiredTexelSize()
{
	using namespace WorldVoxelPaint;
	WorldVoxelPaint::FVoxelPaintLadderState State = BuildStateFromProperties();
	const FVoxelPaintLadderClamps Clamps = GetClamps();
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		SelectRungForTexelSize(State, Axis, Properties->DesiredTexelSize, Clamps);
	}
	CommitState(State);
}

void UVoxelPaintSizingTool::ResetDataAndUnlockSize()
{
	UVoxelPaintVolumeData* Asset = GetAsset();
	if (!Asset)
	{
		return;
	}
	if (FMessageDialog::Open(EAppMsgType::YesNo,
		WorldVoxelPaint::GetResetDataConfirmationText(Asset)) != EAppReturnType::Yes)
	{
		return;
	}
	const AActor* Initiator = TargetPlacement.IsValid() ? TargetPlacement->GetOwner() : nullptr;
	FText Error;
	if (!WorldVoxelPaint::ResetDataAndUnlockSize(Asset, Initiator, Error))
	{
		FVoxelPaintToolsContext::NotifyUser(Error);
	}
	SyncFromAsset();
}

UVoxelPaintVolumeData* UVoxelPaintSizingTool::GetAsset() const
{
	const UVoxelPaintVolumeComponent* Placement = TargetPlacement.Get();
	return Placement ? Placement->ResolveActiveVariant() : nullptr;
}

WorldVoxelPaint::FVoxelPaintLadderClamps UVoxelPaintSizingTool::GetClamps() const
{
	const UVoxelPaintVolumeComponent* Placement = TargetPlacement.Get();
	return WorldVoxelPaint::BuildLadderClamps(Placement ? Placement->GetWorld() : nullptr);
}

WorldVoxelPaint::FVoxelPaintLadderState UVoxelPaintSizingTool::BuildStateFromProperties() const
{
	WorldVoxelPaint::FVoxelPaintLadderState State;
	State.HalfExtent = Properties->HalfExtent;
	State.BrickCounts = FIntVector(
		FMath::Max(1, Properties->DetailRungs.X),
		FMath::Max(1, Properties->DetailRungs.Y),
		FMath::Max(1, Properties->DetailRungs.Z));
	return State;
}

bool UVoxelPaintSizingTool::CommitState(const WorldVoxelPaint::FVoxelPaintLadderState& NewState)
{
	using namespace WorldVoxelPaint;

	UVoxelPaintVolumeData* Asset = GetAsset();
	if (!Asset)
	{
		SyncFromAsset();
		return false;
	}
	if (Asset->bInitialized)
	{
		SyncFromAsset();
		const FText Frozen = FText::Format(LOCTEXT("SizingFrozen",
			"'{0}' is initialized and its topology is frozen. Use Reset Data and Unlock Size for a destructive change."),
			FText::FromString(Asset->GetName()));
		Properties->Status = Frozen.ToString();
		Properties->StatusLevel = EVoxelPaintSizingStatusLevel::Frozen;
		FVoxelPaintToolsContext::NotifyUser(Frozen);
		return false;
	}

	const FVoxelPaintLadderClamps Clamps = GetClamps();
	FString LadderError;
	if (!ValidateLadderState(NewState, Clamps, LadderError))
	{
		SyncFromAsset();
		Properties->Status = LadderError;
		Properties->StatusLevel = EVoxelPaintSizingStatusLevel::Error;
		FVoxelPaintToolsContext::NotifyUser(FText::FromString(LadderError));
		return false;
	}

	const AActor* Initiator = TargetPlacement.IsValid() ? TargetPlacement->GetOwner() : nullptr;
	FText Error;
	UVoxelPaintVolumeComponent* ExpectationTarget =
		ResolveExpectationWriteTarget(TargetPlacement.Get(), Error);
	if (!ExpectationTarget)
	{
		SyncFromAsset();
		Properties->Status = Error.ToString();
		Properties->StatusLevel = EVoxelPaintSizingStatusLevel::Error;
		FVoxelPaintToolsContext::NotifyUser(Error);
		return false;
	}
	if (!CommitPreInitTopologyEdit(Asset, Initiator, ExpectationTarget, NewState, Error))
	{
		SyncFromAsset();
		Properties->Status = Error.ToString();
		Properties->StatusLevel = EVoxelPaintSizingStatusLevel::Error;
		FVoxelPaintToolsContext::NotifyUser(Error);
		return false;
	}

	SyncFromAsset();
	return true;
}

void UVoxelPaintSizingTool::SyncFromAsset()
{
	using namespace WorldVoxelPaint;

	const UVoxelPaintVolumeData* Asset = GetAsset();
	if (!Asset)
	{
		Properties->AssetName = TEXT("<none>");
		Properties->bAssetInitialized = false;
		Properties->Status = TEXT("Select a voxel paint volume with an assigned asset.");
		Properties->StatusLevel = EVoxelPaintSizingStatusLevel::NoAsset;
		Properties->WorstCaseBricks = 0;
		Properties->WorstCasePoolMemory.Reset();
		Properties->PopulatedBricks = 0;
		Properties->CurrentPoolMemory.Reset();
		LastSyncedHalfExtent = FVector::ZeroVector;
		LastSyncedBrickCounts = FIntVector::ZeroValue;
		bLastSyncedInitialized = false;
		LastSyncedPopulatedBricks = INDEX_NONE;
		return;
	}

	FVoxelPaintLadderState State;
	State.HalfExtent = Asset->LocalHalfExtent;
	State.BrickCounts = Asset->GetBrickCounts();

	Properties->AssetName = Asset->GetName();
	Properties->bAssetInitialized = Asset->bInitialized;
	Properties->HalfExtent = State.HalfExtent;
	Properties->DetailRungs = State.BrickCounts;
	Properties->ResolvedTexelSize = State.GetTexelSize();
	Properties->Resolution = State.GetResolution();
	Properties->WorstCaseBricks = State.GetWorstCaseBricks();
	Properties->WorstCasePoolMemory = FString::Printf(TEXT("%.1f MiB"),
		(double)State.GetWorstCasePoolBytes() / (1024.0 * 1024.0));

	const int32 PopulatedBrickCount = GetPopulatedBrickCount(Asset);
	Properties->PopulatedBricks = PopulatedBrickCount;
	Properties->CurrentPoolMemory = FString::Printf(TEXT("%.1f MiB"),
		(double)((int64)PopulatedBrickCount * BrickPayloadBytes) / (1024.0 * 1024.0));

	if (Asset->bInitialized)
	{
		Properties->Status = TEXT("Topology frozen: the first paint stroke initialized this asset. "
			"Size and detail are read-only; Reset Data and Unlock Size below is the only unlock.");
		Properties->StatusLevel = EVoxelPaintSizingStatusLevel::Frozen;
	}
	else
	{
		FString LadderError;
		const bool bValid = ValidateLadderState(State, GetClamps(), LadderError);
		Properties->Status = bValid
			? TEXT("Editable: size and detail stay unlocked until the first paint stroke initializes the asset.")
			: LadderError;
		Properties->StatusLevel = bValid
			? EVoxelPaintSizingStatusLevel::Editable : EVoxelPaintSizingStatusLevel::Error;
	}

	LastSyncedHalfExtent = State.HalfExtent;
	LastSyncedBrickCounts = State.BrickCounts;
	bLastSyncedInitialized = Asset->bInitialized;
	LastSyncedPopulatedBricks = PopulatedBrickCount;
}

#undef LOCTEXT_NAMESPACE
