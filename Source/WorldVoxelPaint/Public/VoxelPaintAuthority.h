// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

class AActor;
class UVoxelPaintVolumeData;

namespace WorldVoxelPaint
{
	enum class EVoxelPaintAuthorityVerdict : uint8
	{
		Allowed,
		DeniedNotOwner,
		DeniedOwnerless
	};

	struct FVoxelPaintAuthorityDenial
	{
		EVoxelPaintAuthorityVerdict Verdict = EVoxelPaintAuthorityVerdict::Allowed;
		FString OwnerDescription;
	};

	WORLDVOXELPAINT_API EVoxelPaintAuthorityVerdict CheckEditingAuthority(
		const UVoxelPaintVolumeData* Asset, const AActor* InitiatingActor,
		FVoxelPaintAuthorityDenial* OutDenial = nullptr);

	WORLDVOXELPAINT_API void AssignOwner(UVoxelPaintVolumeData* Asset, const AActor* OwnerActor);

	WORLDVOXELPAINT_API UVoxelPaintVolumeData* CreateVolumeDataForPlacement(const AActor* OwnerActor);
	WORLDVOXELPAINT_API UVoxelPaintVolumeData* DeepCopyVolumeDataForPlacement(
		const UVoxelPaintVolumeData* Source, const AActor* OwnerActor);

	WORLDVOXELPAINT_API void TakeOwnership(UVoxelPaintVolumeData* Asset, const AActor* NewOwner);
	WORLDVOXELPAINT_API void MarkShared(UVoxelPaintVolumeData* Asset);
	WORLDVOXELPAINT_API void MarkPrivate(UVoxelPaintVolumeData* Asset, const AActor* NewOwner);

	using FVoxelPaintOpenSessionAssetQuery = TFunction<bool(const UVoxelPaintVolumeData*)>;
	WORLDVOXELPAINT_API void SetOpenSessionAssetQuery(FVoxelPaintOpenSessionAssetQuery Query);
	WORLDVOXELPAINT_API bool IsAssetInOpenSession(const UVoxelPaintVolumeData* Asset);
}

#endif
