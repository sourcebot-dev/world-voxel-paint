// Copyright Epic Games, Inc. All Rights Reserved.

#include "Algo/Reverse.h"
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintBrushMath.h"
#include "VoxelPaintPickProvider.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintPickEquivalenceTestPrivate
{
	static uint32 Flatten(const FIntVector& Resolution, const FIntVector& Coord)
	{
		return (uint32)(Coord.X + Resolution.X * (Coord.Y + Resolution.Y * Coord.Z));
	}

	static uint32 BrickIndex(const FIntVector& BrickCounts, const FIntVector& Coord)
	{
		const FIntVector Brick = Coord / WorldVoxelPaint::UniqueBrickSize;
		return (uint32)(Brick.X + BrickCounts.X * (Brick.Y + BrickCounts.Y * Brick.Z));
	}

	// Fixed permutation standing in for GPU append order (deterministic, order-scrambling).
	static TArray<uint32> GpuOrder(const TArray<uint32>& Ascending)
	{
		TArray<uint32> Result = Ascending;
		Algo::Reverse(Result);
		for (int32 Index = 0; Index + 1 < Result.Num(); Index += 2)
		{
			Swap(Result[Index], Result[Index + 1]);
		}
		return Result;
	}

	static bool ListsByteIdentical(const FVoxelPaintGatherList& A, const FVoxelPaintGatherList& B)
	{
		if (A.TexelIndices.Num() != B.TexelIndices.Num() || A.DirtyBricks.Num() != B.DirtyBricks.Num())
		{
			return false;
		}
		if (A.TexelIndices.Num() > 0 && FMemory::Memcmp(A.TexelIndices.GetData(),
			B.TexelIndices.GetData(), A.TexelIndices.Num() * sizeof(uint32)) != 0)
		{
			return false;
		}
		if (A.DirtyBricks.Num() > 0 && FMemory::Memcmp(A.DirtyBricks.GetData(),
			B.DirtyBricks.GetData(), A.DirtyBricks.Num() * sizeof(uint32)) != 0)
		{
			return false;
		}
		return A.DirtyMin == B.DirtyMin && A.DirtyMax == B.DirtyMax
			&& A.BrushCenterTexel == B.BrushCenterTexel
			&& A.BrushRadiusTexels == B.BrushRadiusTexels;
	}

	static bool IsStrictlyAscending(const TArray<uint32>& Values)
	{
		for (int32 Index = 1; Index < Values.Num(); ++Index)
		{
			if (Values[Index] <= Values[Index - 1])
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPick_ProviderEquivalence,
	"Game.WorldVoxelPaint.Unit.Pick.ProviderEquivalence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPick_ProviderEquivalence::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintPickEquivalenceTestPrivate;
	using namespace WorldVoxelPaint;

	AddExpectedMessage(TEXT("out-of-domain texel indices"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 4, false);

	{
		const FIntVector BrickCounts(2, 1, 1);
		const FIntVector Resolution = BrickCounts * UniqueBrickSize;
		const FVector3f BrushCenter(7.25f, 3.0f, 3.5f);
		const float BrushRadius = 2.5f;

		TArray<uint32> CpuOrderIndices;
		for (int32 Z = 3; Z <= 3; ++Z)
		{
			for (int32 Y = 2; Y <= 4; ++Y)
			{
				for (int32 X = 5; X <= 9; ++X)
				{
					CpuOrderIndices.Add(Flatten(Resolution, FIntVector(X, Y, Z)));
				}
			}
		}
		const TArray<uint32> GpuOrderIndices = GpuOrder(CpuOrderIndices);
		TestFalse(TEXT("Blob: the fabricated GPU append order differs from CPU order"),
			IsStrictlyAscending(GpuOrderIndices));

		FVoxelPaintGatherList FromCpu;
		FVoxelPaintGatherList FromGpu;
		BuildGatherListFromPick(BrickCounts, TArray<uint32>(CpuOrderIndices), BrushCenter,
			BrushRadius, FromCpu);
		BuildGatherListFromPick(BrickCounts, TArray<uint32>(GpuOrderIndices), BrushCenter,
			BrushRadius, FromGpu);

		TestTrue(TEXT("Blob: GPU-order and CPU-order inputs normalize byte-identically"),
			ListsByteIdentical(FromCpu, FromGpu));
		TestTrue(TEXT("Blob: output texel indices are strictly ascending"),
			IsStrictlyAscending(FromGpu.TexelIndices));
		TestTrue(TEXT("Blob: output equals the sorted logical set"),
			FromGpu.TexelIndices == CpuOrderIndices);
		TestTrue(TEXT("Blob: DirtyMin/DirtyMax is the tight texel AABB"),
			FromGpu.DirtyMin == FIntVector(5, 2, 3) && FromGpu.DirtyMax == FIntVector(9, 4, 3));
		TestTrue(TEXT("Blob: dirty bricks span the brick boundary, ascending"),
			FromGpu.DirtyBricks == TArray<uint32>({ 0u, 1u }));
		TestTrue(TEXT("Blob: brush fields pass through"),
			FromGpu.BrushCenterTexel == BrushCenter && FromGpu.BrushRadiusTexels == BrushRadius);
	}

	{
		const FIntVector BrickCounts(2, 1, 1);
		FVoxelPaintGatherList FromEmpty;
		BuildGatherListFromPick(BrickCounts, TArray<uint32>(), FVector3f(1.0f, 2.0f, 3.0f), 4.0f,
			FromEmpty);
		TestTrue(TEXT("Empty: no-hit input yields IsEmpty"), FromEmpty.IsEmpty());
		TestTrue(TEXT("Empty: AABB is zero"),
			FromEmpty.DirtyMin == FIntVector::ZeroValue && FromEmpty.DirtyMax == FIntVector::ZeroValue);
		TestTrue(TEXT("Empty: list is default-constructed (no bricks, zero brush fields)"),
			FromEmpty.DirtyBricks.IsEmpty() && FromEmpty.BrushCenterTexel == FVector3f::ZeroVector
				&& FromEmpty.BrushRadiusTexels == 0.0f);
	}

	{
		const FIntVector BrickCounts(2, 1, 1);
		const FIntVector Resolution = BrickCounts * UniqueBrickSize;
		const uint32 DomainTexels = (uint32)(Resolution.X * Resolution.Y * Resolution.Z);

		TArray<uint32> ValidAscending;
		ValidAscending.Add(Flatten(Resolution, FIntVector(0, 0, 0)));
		ValidAscending.Add(Flatten(Resolution, FIntVector(6, 6, 6)));
		ValidAscending.Add(Flatten(Resolution, FIntVector(13, 6, 6)));

		TArray<uint32> CpuOrderIndices = ValidAscending;
		CpuOrderIndices.Add(DomainTexels);
		CpuOrderIndices.Add(DomainTexels + 5000u);
		CpuOrderIndices.Add(0xFFFFFFFFu);
		TArray<uint32> GpuOrderIndices = GpuOrder(CpuOrderIndices);

		FVoxelPaintGatherList FromCpu;
		FVoxelPaintGatherList FromGpu;
		BuildGatherListFromPick(BrickCounts, MoveTemp(CpuOrderIndices), FVector3f(7.0f, 3.0f, 3.0f),
			8.0f, FromCpu);
		BuildGatherListFromPick(BrickCounts, MoveTemp(GpuOrderIndices), FVector3f(7.0f, 3.0f, 3.0f),
			8.0f, FromGpu);

		TestTrue(TEXT("OutOfDomain: both orders drop the same indices and stay byte-identical"),
			ListsByteIdentical(FromCpu, FromGpu));
		TestTrue(TEXT("OutOfDomain: only the in-domain indices survive"),
			FromGpu.TexelIndices == ValidAscending);
		TestTrue(TEXT("OutOfDomain: AABB spans the surviving texels"),
			FromGpu.DirtyMin == FIntVector(0, 0, 0) && FromGpu.DirtyMax == FIntVector(13, 6, 6));

		TArray<uint32> AllInvalid({ DomainTexels, DomainTexels + 1u });
		FVoxelPaintGatherList FromInvalidCpu;
		FVoxelPaintGatherList FromInvalidGpu;
		BuildGatherListFromPick(BrickCounts, TArray<uint32>(AllInvalid), FVector3f(1.0f, 1.0f, 1.0f),
			2.0f, FromInvalidCpu);
		Algo::Reverse(AllInvalid);
		BuildGatherListFromPick(BrickCounts, MoveTemp(AllInvalid), FVector3f(1.0f, 1.0f, 1.0f),
			2.0f, FromInvalidGpu);
		TestTrue(TEXT("OutOfDomain: fully dropped input yields the default empty list"),
			FromInvalidCpu.IsEmpty() && FromInvalidCpu.DirtyMin == FIntVector::ZeroValue
				&& FromInvalidCpu.DirtyMax == FIntVector::ZeroValue
				&& FromInvalidCpu.BrushRadiusTexels == 0.0f);
		TestTrue(TEXT("OutOfDomain: fully dropped input is order-independent"),
			ListsByteIdentical(FromInvalidCpu, FromInvalidGpu));
	}

	{
		const FIntVector BrickCounts(1, 64, 1);
		const FIntVector Resolution = BrickCounts * UniqueBrickSize;
		const TArray<FIntVector> Coords({
			FIntVector(3, 0, 3), FIntVector(3, 220, 3), FIntVector(3, 447, 3) });

		TArray<uint32> CpuOrderIndices;
		for (const FIntVector& Coord : Coords)
		{
			CpuOrderIndices.Add(Flatten(Resolution, Coord));
		}
		TArray<uint32> ExpectedBricks;
		for (const FIntVector& Coord : Coords)
		{
			ExpectedBricks.AddUnique(BrickIndex(BrickCounts, Coord));
		}
		ExpectedBricks.Sort();

		FVoxelPaintGatherList FromCpu;
		FVoxelPaintGatherList FromGpu;
		BuildGatherListFromPick(BrickCounts, TArray<uint32>(CpuOrderIndices),
			FVector3f(3.0f, 220.0f, 3.0f), 230.0f, FromCpu);
		BuildGatherListFromPick(BrickCounts, GpuOrder(CpuOrderIndices),
			FVector3f(3.0f, 220.0f, 3.0f), 230.0f, FromGpu);

		TestTrue(TEXT("Elongated 1xNx1: orders normalize byte-identically"),
			ListsByteIdentical(FromCpu, FromGpu));
		TestTrue(TEXT("Elongated 1xNx1: indices ascend across the long axis"),
			FromGpu.TexelIndices == CpuOrderIndices);
		TestTrue(TEXT("Elongated 1xNx1: dirty bricks ascend along the long axis"),
			FromGpu.DirtyBricks == ExpectedBricks
				&& FromGpu.DirtyBricks == TArray<uint32>({ 0u, 31u, 63u }));
		TestTrue(TEXT("Elongated 1xNx1: AABB is tight"),
			FromGpu.DirtyMin == FIntVector(3, 0, 3) && FromGpu.DirtyMax == FIntVector(3, 447, 3));
	}

	{
		const FIntVector BrickCounts(32, 32, 32);
		TestEqual(TEXT("CapScale: the fixture sits at the admission cap"),
			BrickCounts.X * BrickCounts.Y * BrickCounts.Z, AssetAdmissionCap);
		const FIntVector Resolution = BrickCounts * UniqueBrickSize;
		const TArray<FIntVector> Coords({
			FIntVector(0, 0, 0), FIntVector(100, 150, 200), FIntVector(223, 223, 223) });

		TArray<uint32> CpuOrderIndices;
		TArray<uint32> ExpectedBricks;
		for (const FIntVector& Coord : Coords)
		{
			CpuOrderIndices.Add(Flatten(Resolution, Coord));
			ExpectedBricks.AddUnique(BrickIndex(BrickCounts, Coord));
		}
		ExpectedBricks.Sort();

		FVoxelPaintGatherList FromCpu;
		FVoxelPaintGatherList FromGpu;
		BuildGatherListFromPick(BrickCounts, TArray<uint32>(CpuOrderIndices),
			FVector3f(112.0f, 112.0f, 112.0f), 200.0f, FromCpu);
		BuildGatherListFromPick(BrickCounts, GpuOrder(CpuOrderIndices),
			FVector3f(112.0f, 112.0f, 112.0f), 200.0f, FromGpu);

		TestTrue(TEXT("CapScale: orders normalize byte-identically"),
			ListsByteIdentical(FromCpu, FromGpu));
		TestTrue(TEXT("CapScale: the last domain texel survives (boundary inclusive)"),
			FromGpu.TexelIndices.Contains(
				(uint32)(Resolution.X * Resolution.Y * Resolution.Z - 1)));
		TestTrue(TEXT("CapScale: bricks resolve up to the final brick, ascending"),
			FromGpu.DirtyBricks == ExpectedBricks
				&& FromGpu.DirtyBricks.Last() == (uint32)(AssetAdmissionCap - 1));
		TestTrue(TEXT("CapScale: AABB spans the domain corners"),
			FromGpu.DirtyMin == FIntVector(0, 0, 0)
				&& FromGpu.DirtyMax == FIntVector(223, 223, 223));
	}

	return true;
}
