// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "VoxelPaintTypes.generated.h"

UENUM()
enum class EVoxelPaintBlendMode : uint8
{
	Add,
	Subtract
};

UENUM()
enum class EVoxelPaintAssetSharingMode : uint8
{
	Private,
	Shared
};

UENUM()
enum class EVoxelPaintWorldEnableOverride : uint8
{
	UseProjectDefault,
	ForceEnabled,
	ForceDisabled
};

USTRUCT()
struct FVoxelPaintChannelContext
{
	GENERATED_BODY()

	UPROPERTY(config, EditAnywhere, Category = "Voxel Paint")
	bool bInUse = false;

	UPROPERTY(config, EditAnywhere, Category = "Voxel Paint")
	FString DisplayName;

	UPROPERTY(config, EditAnywhere, Category = "Voxel Paint")
	float DefaultValue = 0.0f;
};
