// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "VoxelPaintTypes.h"
#include "VoxelPaintWorldSettings.generated.h"

UCLASS()
class WORLDVOXELPAINT_API UVoxelPaintWorldSettings : public UAssetUserData
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Voxel Paint", meta = (ClampMin = "0.01"))
	float MinVoxelSize = 0.5f;

	UPROPERTY(EditAnywhere, Category = "Voxel Paint")
	bool bOverrideGridZRange = false;

	UPROPERTY(EditAnywhere, Category = "Voxel Paint", meta = (EditCondition = "bOverrideGridZRange"))
	float GridZMin = -10000.0f;

	UPROPERTY(EditAnywhere, Category = "Voxel Paint", meta = (EditCondition = "bOverrideGridZRange"))
	float GridZMax = 30000.0f;

	UPROPERTY(EditAnywhere, Category = "Voxel Paint")
	EVoxelPaintWorldEnableOverride EnableOverride = EVoxelPaintWorldEnableOverride::UseProjectDefault;
};
