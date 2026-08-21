// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Selection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "VoxelPaintAuthority.h"
#include "VoxelPaintCanonicalSource.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintVolume.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "WorldVoxelPaintConstants.h"

namespace
{

struct FVoxelPaintPresetEntry
{
	const TCHAR* Name;
	WorldVoxelPaint::EVoxelPaintFixturePreset Preset;
};

constexpr int32 NumFixturePresets = 4;

const FVoxelPaintPresetEntry GFixturePresets[NumFixturePresets] =
{
	{ TEXT("Gradient"), WorldVoxelPaint::EVoxelPaintFixturePreset::Gradient },
	{ TEXT("Checker"), WorldVoxelPaint::EVoxelPaintFixturePreset::Checker },
	{ TEXT("SparseBlob"), WorldVoxelPaint::EVoxelPaintFixturePreset::SparseBlob },
	{ TEXT("BorderProbe"), WorldVoxelPaint::EVoxelPaintFixturePreset::BorderProbe }
};

bool ParseFixturePreset(const FString& Token, WorldVoxelPaint::EVoxelPaintFixturePreset& OutPreset,
	const TCHAR*& OutName)
{
	for (const FVoxelPaintPresetEntry& Entry : GFixturePresets)
	{
		if (Token.Equals(Entry.Name, ESearchCase::IgnoreCase))
		{
			OutPreset = Entry.Preset;
			OutName = Entry.Name;
			return true;
		}
	}
	return false;
}

bool ParseBrickCounts(const TArray<FString>& Args, FIntVector& OutCounts)
{
	if (Args.Num() == 1)
	{
		OutCounts = FIntVector(4, 4, 4);
		return true;
	}
	if (Args.Num() != 4)
	{
		return false;
	}
	int64 Parsed[3];
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const FString& Token = Args[Index + 1];
		if (!Token.IsNumeric())
		{
			return false;
		}
		Parsed[Index] = FMath::Clamp<int64>(FCString::Atoi64(*Token), 1,
			WorldVoxelPaint::AssetAdmissionCap);
	}
	OutCounts = FIntVector((int32)Parsed[0], (int32)Parsed[1], (int32)Parsed[2]);
	return true;
}

FIntVector ClampBrickCountProduct(const FIntVector& Counts, bool& bOutClamped)
{
	FIntVector Result = Counts;
	bOutClamped = false;
	while ((int64)Result.X * Result.Y * Result.Z > WorldVoxelPaint::AssetAdmissionCap)
	{
		bOutClamped = true;
		int32& Largest = (Result.X >= Result.Y && Result.X >= Result.Z) ? Result.X
			: (Result.Y >= Result.Z ? Result.Y : Result.Z);
		Largest = FMath::Max(1, Largest / 2);
	}
	return Result;
}

UVoxelPaintVolumeData* FindOrCreateFixtureAsset(const AVoxelPaintVolume* Actor, bool& bOutCreated)
{
	bOutCreated = false;

	FString SanitizedLabel = ObjectTools::SanitizeObjectName(Actor->GetActorLabel());
	if (SanitizedLabel.IsEmpty())
	{
		SanitizedLabel = ObjectTools::SanitizeObjectName(Actor->GetName());
	}
	const FString AssetName = SanitizedLabel + TEXT("_VPD");
	const FString PackageName = FString(TEXT("/Game/VoxelPaintFixtures/")) + AssetName;
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;

	if (UVoxelPaintVolumeData* Existing = FindObject<UVoxelPaintVolumeData>(nullptr, *ObjectPath))
	{
		return Existing;
	}
	if (FPackageName::DoesPackageExist(PackageName))
	{
		UVoxelPaintVolumeData* Loaded = LoadObject<UVoxelPaintVolumeData>(nullptr, *ObjectPath);
		if (!Loaded)
		{
			UE_LOG(LogWorldVoxelPaint, Error,
				TEXT("wvp.Fixture.Seed: package '%s' exists but holds no %s asset named '%s'."),
				*PackageName, *UVoxelPaintVolumeData::StaticClass()->GetName(), *AssetName);
		}
		return Loaded;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UE_LOG(LogWorldVoxelPaint, Error, TEXT("wvp.Fixture.Seed: failed to create package '%s'."),
			*PackageName);
		return nullptr;
	}
	bOutCreated = true;
	return NewObject<UVoxelPaintVolumeData>(Package, FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional);
}

void SeedFixture(const TArray<FString>& Args)
{
	if (!GEditor)
	{
		UE_LOG(LogWorldVoxelPaint, Error, TEXT("wvp.Fixture.Seed: editor is not available."));
		return;
	}

	WorldVoxelPaint::EVoxelPaintFixturePreset Preset = WorldVoxelPaint::EVoxelPaintFixturePreset::Gradient;
	const TCHAR* PresetName = nullptr;
	FIntVector RequestedCounts(4, 4, 4);
	if (Args.Num() < 1 || !ParseFixturePreset(Args[0], Preset, PresetName)
		|| !ParseBrickCounts(Args, RequestedCounts))
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("Usage: wvp.Fixture.Seed <Gradient|Checker|SparseBlob|BorderProbe> [BrickCountX BrickCountY BrickCountZ]"));
		return;
	}

	bool bClamped = false;
	const FIntVector BrickCounts = ClampBrickCountProduct(RequestedCounts, bClamped);
	if (bClamped)
	{
		UE_LOG(LogWorldVoxelPaint, Warning,
			TEXT("wvp.Fixture.Seed: brick counts (%d,%d,%d) exceed the admission cap of %d bricks; clamped to (%d,%d,%d)."),
			RequestedCounts.X, RequestedCounts.Y, RequestedCounts.Z, WorldVoxelPaint::AssetAdmissionCap,
			BrickCounts.X, BrickCounts.Y, BrickCounts.Z);
	}

	TArray<AVoxelPaintVolume*> SelectedVolumes;
	GEditor->GetSelectedActors()->GetSelectedObjects<AVoxelPaintVolume>(SelectedVolumes);
	if (SelectedVolumes.Num() == 0)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("wvp.Fixture.Seed: no AVoxelPaintVolume actors selected."));
		return;
	}

	WorldVoxelPaint::FVoxelPaintLogicalState LogicalState;
	WorldVoxelPaint::BuildFixtureState(Preset, BrickCounts, LogicalState);
	WorldVoxelPaint::FVoxelPaintCanonicalBuild Build;
	WorldVoxelPaint::BuildCanonicalSource(LogicalState, Build);

	int32 NumSeeded = 0;
	for (AVoxelPaintVolume* Actor : SelectedVolumes)
	{
		UVoxelPaintVolumeComponent* Component = Actor->GetVolumeComponent();
		if (!Component)
		{
			UE_LOG(LogWorldVoxelPaint, Warning, TEXT("wvp.Fixture.Seed: '%s' has no volume component; skipped."),
				*Actor->GetActorLabel());
			continue;
		}

		bool bCreated = false;
		UVoxelPaintVolumeData* Asset = Component->ResolveActiveVariant();
		if (!Asset)
		{
			Asset = FindOrCreateFixtureAsset(Actor, bCreated);
		}
		if (!Asset)
		{
			continue;
		}

		if (const FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated())
		{
			if (Controller->GetStageForAsset(Asset) != WorldVoxelPaint::EVoxelPaintSessionStage::Closed)
			{
				UE_LOG(LogWorldVoxelPaint, Error,
					TEXT("wvp.Fixture.Seed: a paint session is open on '%s'; Finish or Discard it first - skipped."),
					*Asset->GetPathName());
				continue;
			}
		}
		if (bCreated)
		{
			WorldVoxelPaint::AssignOwner(Asset, Actor);
		}
		if (!bCreated && Asset->bInitialized && Asset->BrickCounts != BrickCounts)
		{
			UE_LOG(LogWorldVoxelPaint, Warning,
				TEXT("wvp.Fixture.Seed: re-seeding initialized asset '%s' with changed brick counts (%d,%d,%d) -> (%d,%d,%d) is an implicit Reset Data and Unlock Size (test-fixture mechanics)."),
				*Asset->GetPathName(), Asset->BrickCounts.X, Asset->BrickCounts.Y, Asset->BrickCounts.Z,
				BrickCounts.X, BrickCounts.Y, BrickCounts.Z);
		}

		Asset->LocalHalfExtent = Component->GetEffectiveLocalHalfExtent();
		Asset->BrickCounts = BrickCounts;
		Build.Identity = WorldVoxelPaint::ComputeCanonicalIdentity(Build, Asset->StorageOrigin,
			Asset->LocalHalfExtent);
		Asset->CommitCanonicalSource(Build);

		Component->CapturePreEditPoolState();
		Component->Modify();
		if (!Component->PoolContains(Asset))
		{
			if (Component->VolumeVariants.Num() == 0)
			{
				Component->VolumeVariants.AddDefaulted();
			}
			Component->VolumeVariants[0].Variant = Asset;
		}
		Component->ExpectedTopology.LocalHalfExtent = Asset->LocalHalfExtent;
		Component->ExpectedTopology.BrickCounts = Asset->BrickCounts;
		Component->ExpectedTopology.StorageOrigin = Asset->StorageOrigin;
		Component->ExpectedTopology.bIsSet = true;
		FProperty* VolumeVariantsProperty = FindFieldChecked<FProperty>(
			UVoxelPaintVolumeComponent::StaticClass(),
			GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VolumeVariants));
		FPropertyChangedEvent ChangedEvent(VolumeVariantsProperty);
		Component->PostEditChangeProperty(ChangedEvent);

		++NumSeeded;
		UE_LOG(LogWorldVoxelPaint, Display,
			TEXT("wvp.Fixture.Seed: '%s' <- %s (%d,%d,%d) on %s asset '%s' (%d populated bricks, identity %s)."),
			*Actor->GetActorLabel(), PresetName, BrickCounts.X, BrickCounts.Y, BrickCounts.Z,
			bCreated ? TEXT("NEW UNSAVED") : TEXT("existing"), *Asset->GetPathName(),
			Build.Manifest.PopulatedBricks.Num(), *Build.Identity.Hash.ToString());
	}

	if (NumSeeded > 0)
	{
		UE_LOG(LogWorldVoxelPaint, Display,
			TEXT("wvp.Fixture.Seed: %d volume(s) seeded. Fixture assets are dirty in /Game/VoxelPaintFixtures; saving is left to you (File > Save All). Test-fixture mechanics only."),
			NumSeeded);
	}
}

FAutoConsoleCommand GVoxelPaintFixtureSeedCommand(
	TEXT("wvp.Fixture.Seed"),
	TEXT("wvp.Fixture.Seed <Gradient|Checker|SparseBlob|BorderProbe> [BrickCountX BrickCountY BrickCountZ]\n")
	TEXT("Builds a preset canonical paint state (default counts 4 4 4, product clamped to the admission cap), commits it to a savable /Game/VoxelPaintFixtures asset, and assigns it to every selected VoxelPaintVolume actor.\n")
	TEXT("Newly created fixture assets are Private, owned by the seeded actor. Re-seeding an initialized asset with changed brick counts is an implicit Reset Data and Unlock Size. Refused while a paint session is open on the asset.\n")
	TEXT("TEST FIXTURE mechanics for automation and the verification checklist - NOT a product import path."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&SeedFixture));

} // namespace
