// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintPickProvider.h"

#include "Async/ParallelFor.h"
#include "CollisionQueryParams.h"
#include "Engine/EngineTypes.h"
#include "EngineDefines.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "VoxelPaintBrushMath.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"

namespace CpuPickProviderPrivate
{
	struct FPickCandidate
	{
		FIntVector Coord = FIntVector::ZeroValue;
		FVector World = FVector::ZeroVector;
		float DistSq = 0.0f;
	};

	static int64 FlattenTexel(const FIntVector& Resolution, const FIntVector& Coord)
	{
		return (int64)Coord.X + (int64)Resolution.X * ((int64)Coord.Y + (int64)Resolution.Y * Coord.Z);
	}
}

void FVoxelPaintCpuPickProvider::RequestStampSelection(const FVoxelPaintStampRequest& Request,
	TFunction<void(FVoxelPaintGatherList&&)> OnReady)
{
	using namespace CpuPickProviderPrivate;

	FVoxelPaintGatherList Out;

	UWorld* World = Request.World.Get();
	UVoxelPaintVolumeComponent* Target = Request.Target.Get();
	if (!World || !Target || !Target->HasValidVolumeData() || Target->GetWorld() != World)
	{
		OnReady(MoveTemp(Out));
		return;
	}

	const UVoxelPaintVolumeData* Asset = Target->ResolveActiveVariant();
	const FIntVector Resolution = Asset->GetResolution();
	const FIntVector BrickCounts = Asset->GetBrickCounts();
	const FVector TexelSize = Asset->GetTexelSize();
	const FVector StorageMin = Asset->GetStorageMin();
	if (TexelSize.GetMin() <= 0.0 || Resolution.GetMin() <= 0)
	{
		OnReady(MoveTemp(Out));
		return;
	}

	const FVector RayDirection = Request.RayDirection.GetSafeNormal();
	if (RayDirection.IsNearlyZero())
	{
		OnReady(MoveTemp(Out));
		return;
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VoxelPaintCpuPick), /*bInTraceComplex=*/true);
	FHitResult CursorHit;
	if (!World->LineTraceSingleByChannel(CursorHit, Request.RayOrigin,
		Request.RayOrigin + RayDirection * HALF_WORLD_MAX, ECC_Visibility, QueryParams))
	{
		OnReady(MoveTemp(Out));
		return;
	}

	const FTransform Frame = Target->GetRigidPlacementFrame();
	const FVector LocalHit = Frame.InverseTransformPositionNoScale(CursorHit.ImpactPoint);
	const FVector LocalBrushNormal = Frame.InverseTransformVectorNoScale(CursorHit.ImpactNormal).GetSafeNormal();
	const double Radius = FMath::Max(0.0, (double)Request.Brush.RadiusWorld);
	const double TexelDiagonal = TexelSize.Size();
	const float MinFlattenDepth = (float)(0.5 * TexelDiagonal);

	const FVector BoxMin = LocalHit - Radius;
	const FVector BoxMax = LocalHit + Radius;
	const FIntVector TexelMin(
		FMath::Clamp(FMath::FloorToInt32((BoxMin.X - StorageMin.X) / TexelSize.X - 0.5), 0, Resolution.X - 1),
		FMath::Clamp(FMath::FloorToInt32((BoxMin.Y - StorageMin.Y) / TexelSize.Y - 0.5), 0, Resolution.Y - 1),
		FMath::Clamp(FMath::FloorToInt32((BoxMin.Z - StorageMin.Z) / TexelSize.Z - 0.5), 0, Resolution.Z - 1));
	const FIntVector TexelMax(
		FMath::Clamp(FMath::CeilToInt32((BoxMax.X - StorageMin.X) / TexelSize.X - 0.5), 0, Resolution.X - 1),
		FMath::Clamp(FMath::CeilToInt32((BoxMax.Y - StorageMin.Y) / TexelSize.Y - 0.5), 0, Resolution.Y - 1),
		FMath::Clamp(FMath::CeilToInt32((BoxMax.Z - StorageMin.Z) / TexelSize.Z - 0.5), 0, Resolution.Z - 1));

	TArray<FPickCandidate> Candidates;
	for (int32 Z = TexelMin.Z; Z <= TexelMax.Z; ++Z)
	{
		for (int32 Y = TexelMin.Y; Y <= TexelMax.Y; ++Y)
		{
			for (int32 X = TexelMin.X; X <= TexelMax.X; ++X)
			{
				const FVector LocalCenter = StorageMin
					+ (FVector(X, Y, Z) + 0.5) * TexelSize;
				const FVector LocalDelta = LocalCenter - LocalHit;
				const double DistSq = LocalDelta.SizeSquared();
				if (!WorldVoxelPaint::PassesRadius((float)FMath::Sqrt(DistSq), (float)Radius))
				{
					continue;
				}
				if (!WorldVoxelPaint::PassesFlatten(
					(float)FVector::DotProduct(LocalDelta, LocalBrushNormal), (float)Radius,
					Request.Brush.FlattenPct, MinFlattenDepth))
				{
					continue;
				}
				FPickCandidate& Candidate = Candidates.AddDefaulted_GetRef();
				Candidate.Coord = FIntVector(X, Y, Z);
				Candidate.World = Frame.TransformPositionNoScale(LocalCenter);
				Candidate.DistSq = (float)DistSq;
			}
		}
	}

	if (Candidates.Num() > MaxVisibilityCandidates)
	{
		Candidates.Sort([](const FPickCandidate& A, const FPickCandidate& B)
		{
			return A.DistSq < B.DistSq;
		});
		Candidates.SetNum(MaxVisibilityCandidates);
	}

	if (Candidates.IsEmpty())
	{
		OnReady(MoveTemp(Out));
		return;
	}

	const double OcclusionTolerance = 2.0 * TexelDiagonal;
	const float AngleToleranceDeg = Request.Brush.AngleToleranceDeg;
	const FVector BrushNormal = CursorHit.ImpactNormal;
	const FVector EyePosition = Request.RayOrigin;

	// Worker-thread scene reads are safe here: every UWorld trace takes FScopedSceneReadLock
	// (SceneQuery.cpp) and the game thread is parked inside this ParallelFor.
	TArray<uint8> Accepted;
	Accepted.SetNumZeroed(Candidates.Num());
	ParallelFor(Candidates.Num(), [&Candidates, &Accepted, &EyePosition, &BrushNormal,
		OcclusionTolerance, AngleToleranceDeg, World](int32 Index)
	{
		const FPickCandidate& Candidate = Candidates[Index];
		const FVector ToCandidate = Candidate.World - EyePosition;
		const double Distance = ToCandidate.Size();
		if (Distance <= UE_KINDA_SMALL_NUMBER)
		{
			Accepted[Index] = 1;
			return;
		}
		const FVector Direction = ToCandidate / Distance;

		FCollisionQueryParams EyeParams(SCENE_QUERY_STAT(VoxelPaintCpuPickVisibility), /*bInTraceComplex=*/true);
		FHitResult EyeHit;
		const bool bHit = World->LineTraceSingleByChannel(EyeHit, EyePosition,
			Candidate.World + Direction * OcclusionTolerance, ECC_Visibility, EyeParams);
		if (!bHit)
		{
			Accepted[Index] = 1;
			return;
		}
		if ((double)EyeHit.Distance < Distance - OcclusionTolerance)
		{
			return;
		}
		if (!WorldVoxelPaint::PassesAngle(FVector3f(EyeHit.ImpactNormal), FVector3f(BrushNormal),
			AngleToleranceDeg))
		{
			return;
		}
		Accepted[Index] = 1;
	});

	TArray<uint32> TexelIndices;
	TexelIndices.Reserve(Candidates.Num());
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		if (Accepted[Index])
		{
			TexelIndices.Add((uint32)FlattenTexel(Resolution, Candidates[Index].Coord));
		}
	}

	const FVector3f BrushCenterTexel(
		(float)((LocalHit.X - StorageMin.X) / TexelSize.X - 0.5),
		(float)((LocalHit.Y - StorageMin.Y) / TexelSize.Y - 0.5),
		(float)((LocalHit.Z - StorageMin.Z) / TexelSize.Z - 0.5));
	const float BrushRadiusTexels = (float)(Radius / TexelSize.GetMin());
	WorldVoxelPaint::BuildGatherListFromPick(BrickCounts, MoveTemp(TexelIndices),
		BrushCenterTexel, BrushRadiusTexels, Out);
	OnReady(MoveTemp(Out));
}
