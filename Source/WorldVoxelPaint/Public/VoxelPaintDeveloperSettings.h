// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "VoxelPaintTypes.h"
#include "VoxelPaintDeveloperSettings.generated.h"

UCLASS(config = Engine, defaultconfig, meta = (DisplayName = "World Voxel Paint"))
class WORLDVOXELPAINT_API UVoxelPaintDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	UVoxelPaintDeveloperSettings();

	virtual FName GetCategoryName() const override;

	int64 GetBrickPoolCapacityBytes() const;
	int64 GetPageTableCapacityBytes() const;
#if WITH_EDITOR
	int64 GetJournalCapacityBytes() const;
#endif

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	UPROPERTY(config, EditAnywhere, Category = "Budgets")
	int32 BrickPoolCapacityMiBEditor = 256;
	UPROPERTY(config, EditAnywhere, Category = "Budgets")
	int32 BrickPoolCapacityMiBCooked = 128;
	UPROPERTY(config, EditAnywhere, Category = "Budgets")
	int32 PageTableCapacityMiBEditor = 128;
	UPROPERTY(config, EditAnywhere, Category = "Budgets")
	int32 PageTableCapacityMiBCooked = 64;

	UPROPERTY(config, EditAnywhere, Category = "Grid")
	float GridCellSizeXY = 500.0f;
	UPROPERTY(config, EditAnywhere, Category = "Grid")
	float GridCellSizeZ = 1000.0f;
	UPROPERTY(config, EditAnywhere, Category = "Grid")
	float EvaluationRadius = 50000.0f;
	UPROPERTY(config, EditAnywhere, Category = "Grid")
	float GuardBandWidth = 10000.0f;
	UPROPERTY(config, EditAnywhere, Category = "Grid", meta = (ClampMin = "0"))
	int32 GridCandidateCapOverride = 0;
	UPROPERTY(config, EditAnywhere, Category = "Grid")
	float DefaultGridZMin = -10000.0f;
	UPROPERTY(config, EditAnywhere, Category = "Grid")
	float DefaultGridZMax = 30000.0f;

	UPROPERTY(config, EditAnywhere, Category = "Channels")
	TArray<FVoxelPaintChannelContext> ChannelContexts;

#if WITH_EDITORONLY_DATA
	UPROPERTY(config, EditAnywhere, Category = "Session", meta = (ClampMin = 64, ClampMax = 4096))
	int32 JournalCapacityMiB = 1024;
#endif
};
