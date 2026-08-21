// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintAuthority.h"

#if WITH_EDITOR

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "UObject/NameTypes.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "VoxelPaintVolumeData.h"
#include "WorldVoxelPaintConstants.h"

namespace WorldVoxelPaint
{

namespace
{

FVoxelPaintOpenSessionAssetQuery GOpenSessionAssetQuery;

FString SanitizeObjectNameLocal(const FString& InName)
{
	FString Sanitized = InName;
	const TCHAR* InvalidChar = INVALID_OBJECTNAME_CHARACTERS;
	while (*InvalidChar)
	{
		Sanitized.ReplaceCharInline(*InvalidChar, TCHAR('_'), ESearchCase::CaseSensitive);
		++InvalidChar;
	}
	return Sanitized;
}

FString MakeOwnerDisplayName(const AActor* OwnerActor)
{
	if (!OwnerActor)
	{
		return FString();
	}
	FString ContextName;
	if (const UWorld* World = OwnerActor->GetWorld())
	{
		ContextName = FPackageName::GetShortName(World->GetPackage()->GetName());
	}
	else
	{
		ContextName = FPackageName::GetShortName(OwnerActor->GetOutermost()->GetName());
	}
	return FString::Printf(TEXT("%s in %s"), *OwnerActor->GetActorLabel(), *ContextName);
}

FString ResolvePlacementFolder(const AActor* OwnerActor)
{
	const FString FallbackFolder = TEXT("/Game/VoxelPaintVolumes");
	const UWorld* World = OwnerActor->GetWorld();
	if (!World)
	{
		return FallbackFolder;
	}
	const FString MapPackageName = World->GetPackage()->GetName();
	if (FPackageName::IsTempPackage(MapPackageName)
		|| !FPackageName::IsValidLongPackageName(MapPackageName))
	{
		return FallbackFolder;
	}
	return FPackageName::GetLongPackagePath(MapPackageName) + TEXT("/VoxelPaintVolumes");
}

UPackage* CreateUniquePlacementPackage(const AActor* OwnerActor, FString& OutAssetName)
{
	FString SanitizedLabel = SanitizeObjectNameLocal(OwnerActor->GetActorLabel());
	if (SanitizedLabel.IsEmpty())
	{
		SanitizedLabel = SanitizeObjectNameLocal(OwnerActor->GetName());
	}
	const FString Folder = ResolvePlacementFolder(OwnerActor);
	const FString BaseName = SanitizedLabel + TEXT("_VPD");

	FString AssetName = BaseName;
	FString PackageName = Folder + TEXT("/") + AssetName;
	int32 Suffix = 0;
	while (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName))
	{
		++Suffix;
		AssetName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix);
		PackageName = Folder + TEXT("/") + AssetName;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("VoxelPaintAuthority: failed to create package '%s'."), *PackageName);
		return nullptr;
	}
	OutAssetName = AssetName;
	return Package;
}

} // namespace

EVoxelPaintAuthorityVerdict CheckEditingAuthority(const UVoxelPaintVolumeData* Asset,
	const AActor* InitiatingActor, FVoxelPaintAuthorityDenial* OutDenial)
{
	const auto Deny = [OutDenial](EVoxelPaintAuthorityVerdict Verdict, FString&& Description)
	{
		if (OutDenial)
		{
			OutDenial->Verdict = Verdict;
			OutDenial->OwnerDescription = MoveTemp(Description);
		}
		return Verdict;
	};

	if (!Asset)
	{
		return Deny(EVoxelPaintAuthorityVerdict::DeniedOwnerless, TEXT("no asset is assigned"));
	}

	if (Asset->SharingMode == EVoxelPaintAssetSharingMode::Shared)
	{
		return Deny(EVoxelPaintAuthorityVerdict::Allowed, FString());
	}

	if (!Asset->OwnerActorGuid.IsValid())
	{
		return Deny(EVoxelPaintAuthorityVerdict::DeniedOwnerless,
			TEXT("this asset has no owner placement"));
	}

	if (InitiatingActor && InitiatingActor->GetActorGuid() == Asset->OwnerActorGuid)
	{
		return Deny(EVoxelPaintAuthorityVerdict::Allowed, FString());
	}

	return Deny(EVoxelPaintAuthorityVerdict::DeniedNotOwner,
		Asset->OwnerDisplayName.IsEmpty()
			? FString(TEXT("another placement owns this asset"))
			: FString::Printf(TEXT("owned by %s"), *Asset->OwnerDisplayName));
}

void AssignOwner(UVoxelPaintVolumeData* Asset, const AActor* OwnerActor)
{
	if (!Asset)
	{
		return;
	}
	if (!OwnerActor)
	{
		Asset->OwnerActorGuid.Invalidate();
		Asset->OwnerDisplayName.Reset();
		return;
	}
	Asset->OwnerActorGuid = OwnerActor->GetActorGuid();
	Asset->OwnerDisplayName = MakeOwnerDisplayName(OwnerActor);
}

UVoxelPaintVolumeData* CreateVolumeDataForPlacement(const AActor* OwnerActor)
{
	if (!OwnerActor)
	{
		UE_LOG(LogWorldVoxelPaint, Warning,
			TEXT("CreateVolumeDataForPlacement: no owner actor - no asset created."));
		return nullptr;
	}

	FString AssetName;
	UPackage* Package = CreateUniquePlacementPackage(OwnerActor, AssetName);
	if (!Package)
	{
		return nullptr;
	}

	UVoxelPaintVolumeData* Asset = NewObject<UVoxelPaintVolumeData>(Package, FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional);
	Asset->SharingMode = EVoxelPaintAssetSharingMode::Private;
	AssignOwner(Asset, OwnerActor);
	Asset->MarkPackageDirty();

	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Created voxel paint asset '%s' for placement '%s' (unsaved; save the package to keep it)."),
		*Asset->GetPathName(), *OwnerActor->GetActorLabel());
	return Asset;
}

UVoxelPaintVolumeData* DeepCopyVolumeDataForPlacement(const UVoxelPaintVolumeData* Source,
	const AActor* OwnerActor)
{
	if (!Source || !OwnerActor)
	{
		UE_LOG(LogWorldVoxelPaint, Warning,
			TEXT("DeepCopyVolumeDataForPlacement: missing source asset or owner actor - no copy made."));
		return nullptr;
	}

	FString AssetName;
	UPackage* Package = CreateUniquePlacementPackage(OwnerActor, AssetName);
	if (!Package)
	{
		return nullptr;
	}

	UVoxelPaintVolumeData* Copy = DuplicateObject<UVoxelPaintVolumeData>(Source, Package,
		FName(*AssetName));
	if (!Copy)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("DeepCopyVolumeDataForPlacement: duplication of '%s' failed."), *Source->GetPathName());
		return nullptr;
	}
	Copy->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	Copy->SharingMode = EVoxelPaintAssetSharingMode::Private;
	AssignOwner(Copy, OwnerActor);
	Copy->MarkPackageDirty();

	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Deep-copied voxel paint asset '%s' -> '%s' for placement '%s' (unsaved; save the package to keep it)."),
		*Source->GetPathName(), *Copy->GetPathName(), *OwnerActor->GetActorLabel());
	return Copy;
}

void TakeOwnership(UVoxelPaintVolumeData* Asset, const AActor* NewOwner)
{
	if (!Asset || !NewOwner)
	{
		return;
	}
	Asset->Modify();
	Asset->SharingMode = EVoxelPaintAssetSharingMode::Private;
	AssignOwner(Asset, NewOwner);
	UE_LOG(LogWorldVoxelPaint, Display, TEXT("'%s' is now Private, owned by %s."),
		*Asset->GetPathName(), *Asset->OwnerDisplayName);
}

void MarkShared(UVoxelPaintVolumeData* Asset)
{
	if (!Asset)
	{
		return;
	}
	Asset->Modify();
	Asset->SharingMode = EVoxelPaintAssetSharingMode::Shared;
	Asset->OwnerActorGuid.Invalidate();
	Asset->OwnerDisplayName.Reset();
	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("'%s' is now Shared; any referencing placement may edit it."), *Asset->GetPathName());
}

void MarkPrivate(UVoxelPaintVolumeData* Asset, const AActor* NewOwner)
{
	TakeOwnership(Asset, NewOwner);
}

void SetOpenSessionAssetQuery(FVoxelPaintOpenSessionAssetQuery Query)
{
	GOpenSessionAssetQuery = MoveTemp(Query);
}

bool IsAssetInOpenSession(const UVoxelPaintVolumeData* Asset)
{
	return Asset && GOpenSessionAssetQuery && GOpenSessionAssetQuery(Asset);
}

} // namespace WorldVoxelPaint

#endif
