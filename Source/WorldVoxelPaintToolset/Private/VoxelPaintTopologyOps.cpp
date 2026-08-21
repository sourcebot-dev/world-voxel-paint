// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintTopologyOps.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "RHIGlobals.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "VoxelPaintAuthority.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintTemplateRouting.h"
#include "VoxelPaintTypes.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "VoxelPaintWorldSettings.h"
#include "WorldVoxelPaintConstants.h"

#define LOCTEXT_NAMESPACE "WorldVoxelPaint"

namespace WorldVoxelPaint
{

namespace TopologyOpsPrivate
{

static bool GateTopologyOp(UVoxelPaintVolumeData* Asset, const AActor* InitiatingActor,
	const TCHAR* OpName, FText& OutError)
{
	if (!Asset)
	{
		OutError = LOCTEXT("TopologyOpNoAsset", "No voxel paint asset assigned.");
		return false;
	}

	// Topology ops run outside sessions only (any open session, not just one on this asset).
	if (const FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated())
	{
		if (Controller->IsSessionOpen())
		{
			OutError = FText::Format(
				LOCTEXT("TopologyOpSessionOpen",
					"{0}: a paint session is open on '{1}' ({2}); Finish or Discard the open session first."),
				FText::FromString(OpName),
				FText::FromString(GetNameSafe(Controller->GetActiveAsset())),
				FText::FromString(LexToString(Controller->GetStage())));
			return false;
		}
	}

	FVoxelPaintAuthorityDenial Denial;
	if (CheckEditingAuthority(Asset, InitiatingActor, &Denial) != EVoxelPaintAuthorityVerdict::Allowed)
	{
		OutError = FText::Format(
			LOCTEXT("TopologyOpAuthorityDenied",
				"{0}: rejected - {1}. Remedies: Make Unique, Take Ownership, or mark Shared."),
			FText::FromString(OpName), FText::FromString(Denial.OwnerDescription));
		return false;
	}

	return true;
}

static void ApplyTopologyProperty(UVoxelPaintVolumeData* Asset, FName PropertyName)
{
	FProperty* Property = UVoxelPaintVolumeData::StaticClass()->FindPropertyByName(PropertyName);
	FPropertyChangedEvent ChangedEvent(Property);
	Asset->PostEditChangeProperty(ChangedEvent);
}

} // namespace TopologyOpsPrivate

FVoxelPaintLadderClamps BuildLadderClamps(const UWorld* World)
{
	FVoxelPaintLadderClamps Clamps;
	Clamps.MinVoxelSize = UVoxelPaintWorldSettings::StaticClass()
		->GetDefaultObject<UVoxelPaintWorldSettings>()->MinVoxelSize;
	if (World)
	{
		if (AWorldSettings* WorldSettings = World->GetWorldSettings(false, false))
		{
			if (const UVoxelPaintWorldSettings* Override =
				WorldSettings->GetAssetUserData<UVoxelPaintWorldSettings>())
			{
				Clamps.MinVoxelSize = Override->MinVoxelSize;
			}
		}
	}
	Clamps.MaxBrickProduct = AssetAdmissionCap;
	Clamps.MaxBricksPerAxis =
		FMath::Max(1, (int32)(GMaxVolumeTextureDimensions / UniqueBrickSize));
	return Clamps;
}

FText GetResetDataConfirmationText(const UVoxelPaintVolumeData* Asset)
{
	const FText AssetName = FText::FromString(Asset ? Asset->GetName() : TEXT("<none>"));
	if (Asset && Asset->SharingMode == EVoxelPaintAssetSharingMode::Shared)
	{
		return FText::Format(
			LOCTEXT("ResetDataConfirmShared",
				"'{0}' is Shared: Reset Data and Unlock Size deletes ALL paint and affects every linked placement. Make Unique first for an independent change. Continue?"),
			AssetName);
	}
	return FText::Format(
		LOCTEXT("ResetDataConfirmPrivate",
			"Reset Data and Unlock Size deletes ALL paint on '{0}' and unlocks its size and detail. Continue?"),
		AssetName);
}

bool ResetDataAndUnlockSize(UVoxelPaintVolumeData* Asset, const AActor* InitiatingActor, FText& OutError)
{
	if (!TopologyOpsPrivate::GateTopologyOp(Asset, InitiatingActor,
		TEXT("Reset Data and Unlock Size"), OutError))
	{
		return false;
	}
	if (!Asset->bInitialized)
	{
		OutError = FText::Format(
			LOCTEXT("ResetDataNotInitialized",
				"Reset Data and Unlock Size: '{0}' is not initialized; size and detail are already editable."),
			FText::FromString(Asset->GetName()));
		return false;
	}

	{
		FScopedTransaction Transaction(
			LOCTEXT("ResetDataTransaction", "Reset Voxel Paint Data and Unlock Size"));
		Asset->Modify();
		Asset->ResetCanonicalData();
	}

	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Reset Data and Unlock Size: '%s' reset to the uninitialized, resizable state."),
		*Asset->GetPathName());
	return true;
}

bool CommitPreInitTopologyEdit(UVoxelPaintVolumeData* Asset, const AActor* InitiatingActor,
	UVoxelPaintVolumeComponent* ExpectationTarget, const FVoxelPaintLadderState& NewState,
	FText& OutError)
{
	if (!TopologyOpsPrivate::GateTopologyOp(Asset, InitiatingActor,
		TEXT("Topology edit"), OutError))
	{
		return false;
	}
	if (Asset->bInitialized)
	{
		OutError = FText::Format(
			LOCTEXT("TopologyEditFrozen",
				"Topology edit: '{0}' is initialized and its topology is frozen. Use Reset Data and Unlock Size first."),
			FText::FromString(Asset->GetName()));
		return false;
	}

	const FVoxelPaintLadderClamps Clamps =
		BuildLadderClamps(InitiatingActor ? InitiatingActor->GetWorld() : nullptr);
	FString LadderError;
	if (!ValidateLadderState(NewState, Clamps, LadderError))
	{
		OutError = FText::Format(LOCTEXT("TopologyEditInvalid", "Topology edit rejected: {0}"),
			FText::FromString(LadderError));
		return false;
	}

	{
		FScopedTransaction Transaction(
			LOCTEXT("TopologyEditTransaction", "Edit Voxel Paint Volume Size and Detail"));

		Asset->PreEditChange(UVoxelPaintVolumeData::StaticClass()->FindPropertyByName(
			GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeData, LocalHalfExtent)));
		Asset->Modify();
		Asset->LocalHalfExtent = NewState.HalfExtent;
		TopologyOpsPrivate::ApplyTopologyProperty(Asset,
			GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeData, LocalHalfExtent));

		Asset->PreEditChange(UVoxelPaintVolumeData::StaticClass()->FindPropertyByName(
			GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeData, BrickCounts)));
		Asset->BrickCounts = NewState.BrickCounts;
		TopologyOpsPrivate::ApplyTopologyProperty(Asset,
			GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeData, BrickCounts));

		if (ExpectationTarget)
		{
			const TArray<FVoxelPaintVariantPoolEntry> PreEditPool = ExpectationTarget->VolumeVariants;
			const FVoxelPaintExpectedTopology PreEditExpectation = ExpectationTarget->ExpectedTopology;
			ExpectationTarget->CapturePreEditPoolState();
			ExpectationTarget->Modify();
			ExpectationTarget->ExpectedTopology.LocalHalfExtent = NewState.HalfExtent;
			ExpectationTarget->ExpectedTopology.BrickCounts = NewState.BrickCounts;
			ExpectationTarget->ExpectedTopology.StorageOrigin = Asset->StorageOrigin;
			ExpectationTarget->ExpectedTopology.bIsSet = true;
			FPropertyChangedEvent ExpectationEvent(
				UVoxelPaintVolumeComponent::StaticClass()->FindPropertyByName(
					GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, ExpectedTopology)));
			ExpectationTarget->PostEditChangeProperty(ExpectationEvent);
			if (ExpectationTarget->IsTemplate())
			{
				PropagateTemplatePoolChange(ExpectationTarget, PreEditPool, PreEditExpectation);
			}
		}
	}

	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Topology edit: '%s' set to half extent (%g, %g, %g), brick counts (%d, %d, %d)."),
		*Asset->GetPathName(), NewState.HalfExtent.X, NewState.HalfExtent.Y, NewState.HalfExtent.Z,
		NewState.BrickCounts.X, NewState.BrickCounts.Y, NewState.BrickCounts.Z);
	return true;
}

} // namespace WorldVoxelPaint

#undef LOCTEXT_NAMESPACE
