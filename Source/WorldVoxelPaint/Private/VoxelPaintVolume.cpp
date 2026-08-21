// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintVolume.h"

#include "Components/BoxComponent.h"
#include "VoxelPaintVolumeComponent.h"

#if WITH_EDITOR
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GameFramework/WorldSettings.h"
#include "RHIGlobals.h"
#include "VoxelPaintAuthority.h"
#include "VoxelPaintDetailLadder.h"
#include "VoxelPaintVolumeData.h"
#include "VoxelPaintWorldSettings.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "WorldVoxelPaintConstants.h"
#endif

AVoxelPaintVolume::AVoxelPaintVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	VolumeComponent = CreateDefaultSubobject<UVoxelPaintVolumeComponent>(TEXT("VolumeComponent"));
	VolumeComponent->SetMobility(EComponentMobility::Static);
	RootComponent = VolumeComponent;

#if WITH_EDITORONLY_DATA
	EditorBoxComponent = CreateEditorOnlyDefaultSubobject<UBoxComponent>(TEXT("EditorBoxComponent"));
	if (EditorBoxComponent)
	{
		EditorBoxComponent->SetupAttachment(VolumeComponent);
		EditorBoxComponent->SetBoxExtent(VolumeComponent->GetEffectiveLocalHalfExtent(), false);
		EditorBoxComponent->bDrawOnlyIfSelected = true;
		EditorBoxComponent->SetIsVisualizationComponent(true);
		EditorBoxComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		EditorBoxComponent->SetCanEverAffectNavigation(false);
	}
#endif
}

UVoxelPaintVolumeComponent* AVoxelPaintVolume::GetVolumeComponent() const
{
	return VolumeComponent;
}

void AVoxelPaintVolume::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
#if WITH_EDITOR
	RefreshEditorBoxExtent();
#endif
}

#if WITH_EDITOR

namespace
{

void ShowVoxelPaintVolumeNotification(const FString& Message)
{
	if (FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(FText::FromString(Message));
		Info.ExpireDuration = 6.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
	else
	{
		UE_LOG(LogWorldVoxelPaint, Warning, TEXT("%s"), *Message);
	}
}

double ResolveMinVoxelSize(const UWorld* World)
{
	if (World)
	{
		if (AWorldSettings* WorldSettings = World->GetWorldSettings(false, false))
		{
			if (const UVoxelPaintWorldSettings* Override =
				WorldSettings->GetAssetUserData<UVoxelPaintWorldSettings>())
			{
				return Override->MinVoxelSize;
			}
		}
	}
	return UVoxelPaintWorldSettings::StaticClass()
		->GetDefaultObject<UVoxelPaintWorldSettings>()->MinVoxelSize;
}

} // namespace

void AVoxelPaintVolume::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	RefreshEditorBoxExtent();
}

void AVoxelPaintVolume::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	if (bFinished)
	{
		CommitGizmoScaleResize();
	}
	RefreshEditorBoxExtent();
}

void AVoxelPaintVolume::CommitGizmoScaleResize()
{
	if (!VolumeComponent)
	{
		return;
	}
	const FVector CommittedScale = VolumeComponent->GetRelativeScale3D();
	if (CommittedScale.Equals(FVector::OneVector))
	{
		return;
	}
	UVoxelPaintVolumeData* Asset = VolumeComponent->ResolveActiveVariant();
	if (!Asset)
	{
		return;
	}

	const auto RevertScale = [this]()
	{
		VolumeComponent->Modify();
		VolumeComponent->SetRelativeScale3D(FVector::OneVector);
	};

	WorldVoxelPaint::FVoxelPaintAuthorityDenial Denial;
	if (WorldVoxelPaint::CheckEditingAuthority(Asset, this, &Denial)
		!= WorldVoxelPaint::EVoxelPaintAuthorityVerdict::Allowed)
	{
		RevertScale();
		ShowVoxelPaintVolumeNotification(FString::Printf(
			TEXT("%s: resize rejected - %s. Remedies: Make Unique, Take Ownership, or mark Shared."),
			*GetActorLabel(), *Denial.OwnerDescription));
		return;
	}
	if (WorldVoxelPaint::IsAssetInOpenSession(Asset))
	{
		RevertScale();
		ShowVoxelPaintVolumeNotification(FString::Printf(
			TEXT("%s: resize rejected - a paint session is open on '%s'; Finish or Discard it first."),
			*GetActorLabel(), *Asset->GetName()));
		return;
	}
	if (Asset->bInitialized)
	{
		RevertScale();
		ShowVoxelPaintVolumeNotification(FString::Printf(
			TEXT("%s: resize rejected - '%s' is initialized and its topology is frozen. Use Reset Data and Unlock Size to resize."),
			*GetActorLabel(), *Asset->GetName()));
		return;
	}

	WorldVoxelPaint::FVoxelPaintLadderClamps Clamps;
	Clamps.MinVoxelSize = ResolveMinVoxelSize(GetWorld());
	Clamps.MaxBrickProduct = WorldVoxelPaint::AssetAdmissionCap;
	Clamps.MaxBricksPerAxis =
		FMath::Max(1, (int32)(GMaxVolumeTextureDimensions / WorldVoxelPaint::UniqueBrickSize));

	WorldVoxelPaint::FVoxelPaintLadderState LadderState;
	LadderState.HalfExtent = Asset->LocalHalfExtent * CommittedScale.GetAbs();
	LadderState.BrickCounts = Asset->BrickCounts;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const double MinHalfExtent = 0.5 * Clamps.MinVoxelSize
			* (double)(WorldVoxelPaint::UniqueBrickSize * LadderState.BrickCounts[Axis]);
		LadderState.HalfExtent[Axis] = FMath::Max3(LadderState.HalfExtent[Axis], MinHalfExtent, 1.0);
	}

	FString LadderError;
	if (!WorldVoxelPaint::ValidateLadderState(LadderState, Clamps, LadderError))
	{
		RevertScale();
		ShowVoxelPaintVolumeNotification(FString::Printf(
			TEXT("%s: resize rejected - %s"), *GetActorLabel(), *LadderError));
		return;
	}

	Asset->PreEditChange(UVoxelPaintVolumeData::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeData, LocalHalfExtent)));
	Asset->Modify();
	Asset->LocalHalfExtent = LadderState.HalfExtent;
	VolumeComponent->Modify();
	VolumeComponent->ExpectedTopology.LocalHalfExtent = Asset->LocalHalfExtent;
	VolumeComponent->ExpectedTopology.BrickCounts = Asset->BrickCounts;
	VolumeComponent->ExpectedTopology.StorageOrigin = Asset->StorageOrigin;
	VolumeComponent->ExpectedTopology.bIsSet = true;
	FPropertyChangedEvent ChangedEvent(UVoxelPaintVolumeData::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeData, LocalHalfExtent)));
	Asset->PostEditChangeProperty(ChangedEvent);
	RevertScale();

	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("%s: committed gizmo scale converted to asset extent (%g, %g, %g) on '%s'; placement returned to unit scale."),
		*GetActorLabel(), LadderState.HalfExtent.X, LadderState.HalfExtent.Y, LadderState.HalfExtent.Z,
		*Asset->GetPathName());
}

void AVoxelPaintVolume::RefreshEditorBoxExtent()
{
	if (EditorBoxComponent && VolumeComponent)
	{
		EditorBoxComponent->SetBoxExtent(VolumeComponent->GetEffectiveLocalHalfExtent(), false);
	}
}
#endif
