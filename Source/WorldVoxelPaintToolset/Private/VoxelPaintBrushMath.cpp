// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintBrushMath.h"

#include "VoxelPaintPickProvider.h"
#include "WorldVoxelPaintConstants.h"

namespace WorldVoxelPaint
{
	bool PassesRadius(float DistWorld, float RadiusWorld)
	{
		return DistWorld <= RadiusWorld;
	}

	bool PassesFlatten(float SignedDepthWorld, float RadiusWorld, float FlattenPct,
		float MinFlattenDepthWorld)
	{
		const float MaxDepth = FMath::Max(
			RadiusWorld * (1.0f - FMath::Clamp(FlattenPct, 0.0f, 1.0f)), MinFlattenDepthWorld);
		return FMath::Abs(SignedDepthWorld) <= MaxDepth;
	}

	bool PassesAngle(const FVector3f& SurfaceNormal, const FVector3f& BrushNormal,
		float AngleToleranceDeg)
	{
		const FVector3f Surface = SurfaceNormal.GetSafeNormal();
		const FVector3f Brush = BrushNormal.GetSafeNormal();
		if (Surface.IsNearlyZero() || Brush.IsNearlyZero())
		{
			return true;
		}
		const float CosTolerance = FMath::Cos(FMath::DegreesToRadians(
			FMath::Clamp(AngleToleranceDeg, 0.0f, 180.0f)));
		return FVector3f::DotProduct(Surface, Brush) >= CosTolerance;
	}

	float FalloffWeight(float DistTexels, float RadiusTexels, float FalloffHotspot)
	{
		const float Hotspot = FMath::Clamp(FalloffHotspot, 0.0f, 1.0f) * RadiusTexels;
		if (DistTexels <= Hotspot)
		{
			return 1.0f;
		}
		return 1.0f - FMath::Clamp(
			(DistTexels - Hotspot) / FMath::Max(RadiusTexels - Hotspot, 0.0001f), 0.0f, 1.0f);
	}

	void BuildGatherListFromPick(const FIntVector& BrickCounts, TArray<uint32>&& TexelIndices,
		const FVector3f& BrushCenterTexel, float BrushRadiusTexels, FVoxelPaintGatherList& Out)
	{
		Out = FVoxelPaintGatherList();

		const FIntVector Resolution = BrickCounts * UniqueBrickSize;
		const int64 DomainTexels = (int64)FMath::Max(Resolution.X, 0)
			* FMath::Max(Resolution.Y, 0) * FMath::Max(Resolution.Z, 0);

		const int32 NumBefore = TexelIndices.Num();
		TexelIndices.RemoveAll([DomainTexels](uint32 Index)
		{
			return (int64)Index >= DomainTexels;
		});
		if (TexelIndices.Num() != NumBefore)
		{
			UE_LOG(LogWorldVoxelPaint, Warning,
				TEXT("BuildGatherListFromPick dropped %d out-of-domain texel indices."),
				NumBefore - TexelIndices.Num());
		}
		if (TexelIndices.IsEmpty())
		{
			return;
		}

		TexelIndices.Sort();

		const uint32 ResX = (uint32)Resolution.X;
		const uint32 ResXY = (uint32)Resolution.X * (uint32)Resolution.Y;
		FIntVector DirtyMin = FIntVector(MAX_int32);
		FIntVector DirtyMax = FIntVector(MIN_int32);
		TSet<uint32> DirtyBrickSet;
		for (uint32 Index : TexelIndices)
		{
			const FIntVector Coord(
				(int32)(Index % ResX),
				(int32)((Index / ResX) % (uint32)Resolution.Y),
				(int32)(Index / ResXY));
			DirtyMin = DirtyMin.ComponentMin(Coord);
			DirtyMax = DirtyMax.ComponentMax(Coord);
			const FIntVector Brick = Coord / UniqueBrickSize;
			DirtyBrickSet.Add((uint32)(Brick.X + BrickCounts.X * (Brick.Y + BrickCounts.Y * Brick.Z)));
		}

		Out.TexelIndices = MoveTemp(TexelIndices);
		Out.DirtyMin = DirtyMin;
		Out.DirtyMax = DirtyMax;
		Out.DirtyBricks = DirtyBrickSet.Array();
		Out.DirtyBricks.Sort();
		Out.BrushCenterTexel = BrushCenterTexel;
		Out.BrushRadiusTexels = BrushRadiusTexels;
	}
}
