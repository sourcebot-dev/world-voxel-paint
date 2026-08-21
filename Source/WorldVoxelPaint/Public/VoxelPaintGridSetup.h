// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VoxelPaintRecord.h"

class UVoxelPaintDeveloperSettings;
class UWorld;

namespace WorldVoxelPaint
{
	WORLDVOXELPAINT_API FSimpleMulticastDelegate& OnDeveloperSettingsChanged();

	struct FVoxelPaintGridCapacity
	{
		FIntVector WindowCellsCap = FIntVector::ZeroValue;
		uint32 CCap = 0;
		uint32 KCap = 0;
		bool bValid = false;
	};
	WORLDVOXELPAINT_API FVoxelPaintGridCapacity DeriveGridCapacity(const UVoxelPaintDeveloperSettings& Settings);

	struct FVoxelPaintZEnvelope
	{
		double ZMin = 0;
		double ZMax = 0;
	};
	// Game thread, UObject-touching.
	WORLDVOXELPAINT_API FVoxelPaintZEnvelope ResolveZEnvelope(const UWorld* MasterWorld,
		const UVoxelPaintDeveloperSettings& Settings);

	struct FVoxelPaintExpectedWindow
	{
		FVector2D SnappedCenterXY = FVector2D::ZeroVector;
		FVoxelPaintZEnvelope ZEnvelope;
		FVector EvaluationOrigin = FVector::ZeroVector;
		FIntVector LiveCells = FIntVector::ZeroValue;
		bool bValid = false;
	};
	WORLDVOXELPAINT_API FVoxelPaintExpectedWindow MakeExpectedWindow(const FVector& CameraLWC,
		const FVoxelPaintZEnvelope& ZEnvelope, const FVoxelPaintGridCapacity& Capacity,
		float CellSizeXY, float CellSizeZ);
	// Tested against the expected window's snapped center, never the published origin.
	WORLDVOXELPAINT_API bool IsGuardConsumed(const FVoxelPaintExpectedWindow& Window,
		const FVector& CameraLWC, float GuardBandWidth);

	struct FVoxelPaintGridVolumeRange
	{
		uint32 RecordIndex = 0;
		FIntVector CellMin = FIntVector::ZeroValue;
		FIntVector CellDims = FIntVector::ZeroValue;
		uint32 FirstThread = 0;
		uint32 CellCount = 0;
	};
	struct FVoxelPaintGridBuildInput
	{
		FIntVector GridDims = FIntVector::ZeroValue;
		FVector3f GridOriginRel = FVector3f::ZeroVector;
		FVector2f CellSize = FVector2f::ZeroVector;
		uint32 KCap = 0;
		uint32 TotalThreads = 0;
		TArray<FVoxelPaintGridVolumeRange> Ranges;
		uint32 NumOutsideZEnvelope = 0;
		bool bValid = false;
	};
	WORLDVOXELPAINT_API void BuildGridInput(TConstArrayView<FVoxelPaintVolumeRecord> Records,
		const FVoxelPaintExpectedWindow& Window, const FVoxelPaintGridCapacity& Capacity,
		float CellSizeXY, float CellSizeZ, FVoxelPaintGridBuildInput& OutInput);

	// Mirrored bit-exactly by the grid-build .usf.
	WORLDVOXELPAINT_API uint32 PackGridHeader(uint32 Offset, uint32 Count);
	WORLDVOXELPAINT_API void UnpackGridHeader(uint32 Header, uint32& OutOffset, uint32& OutCount);
}
