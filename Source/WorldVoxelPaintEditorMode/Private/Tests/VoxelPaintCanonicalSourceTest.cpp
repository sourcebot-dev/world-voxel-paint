// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "PixelFormat.h"
#include "VoxelPaintCanonicalSource.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintCanonicalSourceTestPrivate
{
	using namespace WorldVoxelPaint;

	static FIntVector BrickCoordFromFlat(uint32 Flat, const FIntVector& BrickCounts)
	{
		const uint32 CountX = (uint32)BrickCounts.X;
		const uint32 CountY = (uint32)BrickCounts.Y;
		return FIntVector(
			(int32)(Flat % CountX),
			(int32)((Flat / CountX) % CountY),
			(int32)(Flat / (CountX * CountY)));
	}

	static FIntVector RankToPhysicalBrick(int32 Rank)
	{
		return FIntVector(
			Rank % SourceBricksXY,
			(Rank / SourceBricksXY) % SourceBricksXY,
			Rank / (SourceBricksXY * SourceBricksXY));
	}

	static int64 TexelByteOffset(const FIntVector& PhysBrick, const FIntVector& Texel, const FIntVector& PhysTexels)
	{
		return ((int64)(PhysBrick.Z * BrickSize + Texel.Z) * PhysTexels.X * PhysTexels.Y
			+ (int64)(PhysBrick.Y * BrickSize + Texel.Y) * PhysTexels.X
			+ (PhysBrick.X * BrickSize + Texel.X)) * 4;
	}

	static FColor ReadPackedTexel(const FVoxelPaintCanonicalBuild& Build, int32 Rank, const FIntVector& Texel)
	{
		const FIntVector PhysTexels = Build.PhysicalDimsInBricks * BrickSize;
		const uint8* Bytes = Build.PackedSourceBGRA8.GetData() + TexelByteOffset(RankToPhysicalBrick(Rank), Texel, PhysTexels);
		return FColor(Bytes[2], Bytes[1], Bytes[0], Bytes[3]);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintCanonicalSource_PackingDeterminism,
	"Game.WorldVoxelPaint.Unit.CanonicalSource.PackingDeterminism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintCanonicalSource_PackingDeterminism::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	TArray<TPair<FIntVector, FColor>> Texels;
	Texels.Emplace(FIntVector(1, 1, 1), FColor(200, 10, 10, 255));
	Texels.Emplace(FIntVector(8, 9, 10), FColor(10, 200, 10, 255));
	Texels.Emplace(FIntVector(13, 13, 13), FColor(10, 10, 200, 255));
	Texels.Emplace(FIntVector(0, 0, 0), FColor(10, 200, 200, 255));
	Texels.Emplace(FIntVector(7, 7, 7), FColor(200, 10, 200, 255));

	FVoxelPaintLogicalState Forward;
	Forward.BrickCounts = FIntVector(2, 2, 2);
	for (int32 Index = 0; Index < Texels.Num(); ++Index)
	{
		Forward.SetTexel(Texels[Index].Key, Texels[Index].Value);
	}

	FVoxelPaintLogicalState Reverse;
	Reverse.BrickCounts = FIntVector(2, 2, 2);
	for (int32 Index = Texels.Num() - 1; Index >= 0; --Index)
	{
		Reverse.SetTexel(Texels[Index].Key, Texels[Index].Value);
	}

	FVoxelPaintCanonicalBuild ForwardBuild;
	FVoxelPaintCanonicalBuild ReverseBuild;
	BuildCanonicalSource(Forward, ForwardBuild);
	BuildCanonicalSource(Reverse, ReverseBuild);

	const FVector StorageOrigin(1.0, 2.0, 3.0);
	const FVector HalfExtent(500.0, 500.0, 500.0);
	const FVoxelPaintCanonicalIdentity ForwardIdentity = ComputeCanonicalIdentity(ForwardBuild, StorageOrigin, HalfExtent);
	const FVoxelPaintCanonicalIdentity ReverseIdentity = ComputeCanonicalIdentity(ReverseBuild, StorageOrigin, HalfExtent);

	TestTrue(TEXT("Manifests identical regardless of insert order"),
		ForwardBuild.Manifest.PopulatedBricks == ReverseBuild.Manifest.PopulatedBricks);
	TestTrue(TEXT("Packed bytes byte-identical regardless of insert order"),
		ForwardBuild.PackedSourceBGRA8 == ReverseBuild.PackedSourceBGRA8);
	TestTrue(TEXT("Identity identical regardless of insert order"), ForwardIdentity == ReverseIdentity);
	TestTrue(TEXT("Identity is valid (nonzero hash)"), ForwardIdentity.IsValid());

	FVoxelPaintLogicalState BrickZero;
	BrickZero.BrickCounts = FIntVector(2, 2, 2);
	BrickZero.SetTexel(FIntVector(1, 2, 3), FColor(255, 128, 64, 255));

	FVoxelPaintLogicalState BrickSeven;
	BrickSeven.BrickCounts = FIntVector(2, 2, 2);
	BrickSeven.SetTexel(FIntVector(8, 9, 10), FColor(255, 128, 64, 255));

	FVoxelPaintCanonicalBuild ZeroBuild;
	FVoxelPaintCanonicalBuild SevenBuild;
	BuildCanonicalSource(BrickZero, ZeroBuild);
	BuildCanonicalSource(BrickSeven, SevenBuild);

	TestTrue(TEXT("Single-brick manifest at flat 0"),
		ZeroBuild.Manifest.PopulatedBricks.Num() == 1 && ZeroBuild.Manifest.PopulatedBricks[0] == 0u);
	TestTrue(TEXT("Single-brick manifest at flat 7"),
		SevenBuild.Manifest.PopulatedBricks.Num() == 1 && SevenBuild.Manifest.PopulatedBricks[0] == 7u);
	TestTrue(TEXT("Identical per-brick content packs byte-identical regardless of which logical brick holds it"),
		ZeroBuild.PackedSourceBGRA8 == SevenBuild.PackedSourceBGRA8);
	TestTrue(TEXT("Identity differs when the same record sits in a different logical brick"),
		ComputeCanonicalIdentity(ZeroBuild, StorageOrigin, HalfExtent)
			!= ComputeCanonicalIdentity(SevenBuild, StorageOrigin, HalfExtent));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintCanonicalSource_ManifestOrder,
	"Game.WorldVoxelPaint.Unit.CanonicalSource.ManifestOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintCanonicalSource_ManifestOrder::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;
	using namespace VoxelPaintCanonicalSourceTestPrivate;

	TestTrue(TEXT("Dims for 0 records is (1,1,1)"), ComputePackedSourceDimsInBricks(0) == FIntVector(1, 1, 1));
	TestTrue(TEXT("Dims for 1 record is (1,1,1)"), ComputePackedSourceDimsInBricks(1) == FIntVector(1, 1, 1));
	TestTrue(TEXT("Dims for 16 records is (16,1,1)"), ComputePackedSourceDimsInBricks(16) == FIntVector(16, 1, 1));
	TestTrue(TEXT("Dims for 17 records is (16,2,1)"), ComputePackedSourceDimsInBricks(17) == FIntVector(16, 2, 1));
	TestTrue(TEXT("Dims for 256 records is (16,16,1)"), ComputePackedSourceDimsInBricks(256) == FIntVector(16, 16, 1));
	TestTrue(TEXT("Dims for 257 records is (16,16,2)"), ComputePackedSourceDimsInBricks(257) == FIntVector(16, 16, 2));

	const FIntVector BrickCounts(4, 4, 2);
	const uint32 ChosenFlats[17] = { 0, 2, 3, 5, 7, 8, 10, 12, 14, 16, 18, 20, 22, 25, 27, 29, 31 };
	const int32 ScrambledOrder[17] = { 9, 3, 16, 0, 12, 6, 14, 1, 10, 4, 15, 7, 2, 11, 5, 13, 8 };

	FVoxelPaintLogicalState State;
	State.BrickCounts = BrickCounts;
	for (int32 Index = 0; Index < 17; ++Index)
	{
		const uint32 Flat = ChosenFlats[ScrambledOrder[Index]];
		const FIntVector Brick = BrickCoordFromFlat(Flat, BrickCounts);
		State.SetTexel(Brick * UniqueBrickSize + FIntVector(3, 3, 3),
			FColor((uint8)(Flat * 7 + 1), (uint8)Flat, (uint8)(255 - Flat), 255));
	}

	const FIntVector ZeroedBrick = BrickCoordFromFlat(1, BrickCounts);
	State.SetTexel(ZeroedBrick * UniqueBrickSize + FIntVector(3, 3, 3), FColor(9, 9, 9, 255));
	State.SetTexel(ZeroedBrick * UniqueBrickSize + FIntVector(3, 3, 3), FColor(0, 0, 0, 0));

	FVoxelPaintCanonicalBuild Build;
	BuildCanonicalSource(State, Build);

	TestTrue(TEXT("17 populated bricks"), Build.Manifest.PopulatedBricks.Num() == 17);
	for (int32 Index = 0; Index < Build.Manifest.PopulatedBricks.Num(); ++Index)
	{
		TestTrue(TEXT("Manifest entry matches the expected ascending flat index"),
			Build.Manifest.PopulatedBricks[Index] == ChosenFlats[Index]);
		if (Index > 0)
		{
			TestTrue(TEXT("Manifest strictly ascending"),
				Build.Manifest.PopulatedBricks[Index] > Build.Manifest.PopulatedBricks[Index - 1]);
		}
	}
	TestFalse(TEXT("Brick zeroed back to all-zero is omitted"), Build.Manifest.PopulatedBricks.Contains(1u));

	TestTrue(TEXT("Physical dims match ComputePackedSourceDimsInBricks"),
		Build.PhysicalDimsInBricks == ComputePackedSourceDimsInBricks(17));
	const FIntVector PhysTexels = Build.PhysicalDimsInBricks * BrickSize;
	TestTrue(TEXT("Packed byte count matches physical dims"),
		Build.PackedSourceBGRA8.Num() == PhysTexels.X * PhysTexels.Y * PhysTexels.Z * 4);

	for (int32 Rank = 0; Rank < 17; ++Rank)
	{
		const FIntVector Brick = BrickCoordFromFlat(Build.Manifest.PopulatedBricks[Rank], BrickCounts);
		const FColor Expected = ResolveBrickTexel(State, Brick, FIntVector(3, 3, 3));
		TestTrue(TEXT("Rank slot holds its manifest brick's content at the derived physical position"),
			ReadPackedTexel(Build, Rank, FIntVector(3, 3, 3)) == Expected);
	}

	const int32 TotalBrickSlots = Build.PhysicalDimsInBricks.X * Build.PhysicalDimsInBricks.Y * Build.PhysicalDimsInBricks.Z;
	bool bTailZero = true;
	for (int32 Rank = 17; Rank < TotalBrickSlots; ++Rank)
	{
		const FIntVector PhysBrick = RankToPhysicalBrick(Rank);
		for (int32 TexelZ = 0; TexelZ < BrickSize && bTailZero; ++TexelZ)
		{
			for (int32 TexelY = 0; TexelY < BrickSize && bTailZero; ++TexelY)
			{
				const int64 RowOffset = TexelByteOffset(PhysBrick, FIntVector(0, TexelY, TexelZ), PhysTexels);
				for (int32 Byte = 0; Byte < BrickSize * 4; ++Byte)
				{
					if (Build.PackedSourceBGRA8[RowOffset + Byte] != 0)
					{
						bTailZero = false;
						break;
					}
				}
			}
		}
	}
	TestTrue(TEXT("Every unowned tail byte is zero"), bTailZero);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintCanonicalSource_BorderRules,
	"Game.WorldVoxelPaint.Unit.CanonicalSource.BorderRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintCanonicalSource_BorderRules::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	{
		FVoxelPaintLogicalState Corner;
		Corner.BrickCounts = FIntVector(2, 2, 2);
		Corner.SetTexel(FIntVector(7, 7, 7), FColor(255, 255, 255, 255));
		FVoxelPaintSourceManifest Manifest;
		BuildBrickManifest(Corner, Manifest);
		TestTrue(TEXT("Min-corner texel populates all 8 lower-direction neighbours"), Manifest.PopulatedBricks.Num() == 8);
		for (int32 Index = 0; Index < Manifest.PopulatedBricks.Num(); ++Index)
		{
			TestTrue(TEXT("Full-footprint corner manifest is the 8 surrounding bricks"),
				Manifest.PopulatedBricks[Index] == (uint32)Index);
		}
	}

	{
		FVoxelPaintLogicalState Face;
		Face.BrickCounts = FIntVector(2, 2, 2);
		Face.SetTexel(FIntVector(7, 3, 3), FColor(255, 255, 255, 255));
		FVoxelPaintSourceManifest Manifest;
		BuildBrickManifest(Face, Manifest);
		TestTrue(TEXT("Min-plane texel populates the owner and the one lower X neighbour"),
			Manifest.PopulatedBricks.Num() == 2
			&& Manifest.PopulatedBricks[0] == 0u && Manifest.PopulatedBricks[1] == 1u);
	}

	{
		const FColor Shared(77, 88, 99, 255);
		FVoxelPaintLogicalState Pair;
		Pair.BrickCounts = FIntVector(2, 1, 1);
		Pair.SetTexel(FIntVector(7, 2, 3), Shared);
		TestTrue(TEXT("Owner brick reads the shared border texel at its min plane"),
			ResolveBrickTexel(Pair, FIntVector(1, 0, 0), FIntVector(0, 2, 3)) == Shared);
		TestTrue(TEXT("Lower neighbour duplicates the shared border texel at its max plane"),
			ResolveBrickTexel(Pair, FIntVector(0, 0, 0), FIntVector(7, 2, 3)) == Shared);
		TestTrue(TEXT("Unpainted border texel reads exact zero"),
			ResolveBrickTexel(Pair, FIntVector(0, 0, 0), FIntVector(7, 4, 4)) == FColor(0, 0, 0, 0));
	}

	{
		const FColor Outermost(11, 22, 33, 255);
		FVoxelPaintLogicalState Domain;
		Domain.BrickCounts = FIntVector(1, 1, 1);
		Domain.SetTexel(FIntVector(6, 2, 2), Outermost);
		TestTrue(TEXT("Domain-face plane duplicates the outermost unique texel via clamp"),
			ResolveBrickTexel(Domain, FIntVector(0, 0, 0), FIntVector(7, 2, 2)) == Outermost);
	}

	{
		FVoxelPaintLogicalState Probe;
		BuildFixtureState(EVoxelPaintFixturePreset::BorderProbe, FIntVector(2, 2, 1), Probe);
		FVoxelPaintSourceManifest Manifest;
		BuildBrickManifest(Probe, Manifest);
		TestTrue(TEXT("BorderProbe(2,2,1) populates bricks 0,1,3 and leaves (0,1,0) absent"),
			Manifest.PopulatedBricks.Num() == 3
			&& Manifest.PopulatedBricks[0] == 0u
			&& Manifest.PopulatedBricks[1] == 1u
			&& Manifest.PopulatedBricks[2] == 3u);

		TestTrue(TEXT("BorderProbe interior color"),
			ResolveBrickTexel(Probe, FIntVector(0, 0, 0), FIntVector(3, 3, 3)) == FColor(40, 40, 40, 255));
		TestTrue(TEXT("BorderProbe neighbour-plane color on the owner side"),
			ResolveBrickTexel(Probe, FIntVector(1, 0, 0), FIntVector(0, 3, 3)) == FColor(255, 0, 0, 255));
		TestTrue(TEXT("BorderProbe neighbour-plane color duplicated on the lower side"),
			ResolveBrickTexel(Probe, FIntVector(0, 0, 0), FIntVector(7, 3, 3)) == FColor(255, 0, 0, 255));
		TestTrue(TEXT("BorderProbe absent-neighbour plane reads exact zero"),
			ResolveBrickTexel(Probe, FIntVector(0, 0, 0), FIntVector(3, 7, 3)) == FColor(0, 0, 0, 0));
		TestTrue(TEXT("BorderProbe domain-face color duplicated by clamp"),
			ResolveBrickTexel(Probe, FIntVector(1, 0, 0), FIntVector(7, 2, 2)) == FColor(0, 255, 0, 255));
		TestTrue(TEXT("BorderProbe mixed edge color (domain clamp X, border cross Y)"),
			ResolveBrickTexel(Probe, FIntVector(1, 1, 0), FIntVector(7, 0, 2)) == FColor(0, 0, 255, 255));
		TestTrue(TEXT("BorderProbe corner color (clamp X, clamp Y within brick, clamp Z domain)"),
			ResolveBrickTexel(Probe, FIntVector(1, 1, 0), FIntVector(7, 7, 7)) == FColor(255, 255, 0, 255));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintCanonicalSource_IdentityHash,
	"Game.WorldVoxelPaint.Unit.CanonicalSource.IdentityHash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintCanonicalSource_IdentityHash::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	const FVector Origin(0.0, 0.0, 0.0);
	const FVector HalfExtent(500.0, 500.0, 500.0);

	FVoxelPaintLogicalState SmallState;
	SmallState.BrickCounts = FIntVector(1, 1, 1);
	SmallState.SetTexel(FIntVector(1, 1, 1), FColor(255, 255, 255, 255));

	FVoxelPaintLogicalState WideState;
	WideState.BrickCounts = FIntVector(2, 1, 1);
	WideState.SetTexel(FIntVector(1, 1, 1), FColor(255, 255, 255, 255));

	FVoxelPaintCanonicalBuild SmallBuild;
	FVoxelPaintCanonicalBuild WideBuild;
	BuildCanonicalSource(SmallState, SmallBuild);
	BuildCanonicalSource(WideState, WideBuild);

	const FVoxelPaintCanonicalIdentity SmallIdentity = ComputeCanonicalIdentity(SmallBuild, Origin, HalfExtent);
	TestTrue(TEXT("Identity recomputation is deterministic"),
		SmallIdentity == ComputeCanonicalIdentity(SmallBuild, Origin, HalfExtent));
	TestTrue(TEXT("Same paint under different BrickCounts hashes differently"),
		SmallIdentity != ComputeCanonicalIdentity(WideBuild, Origin, HalfExtent));
	TestTrue(TEXT("Different StorageOrigin hashes differently"),
		SmallIdentity != ComputeCanonicalIdentity(SmallBuild, FVector(1.0, 0.0, 0.0), HalfExtent));
	TestTrue(TEXT("Different LocalHalfExtent hashes differently"),
		SmallIdentity != ComputeCanonicalIdentity(SmallBuild, Origin, FVector(400.0, 500.0, 500.0)));

	FVoxelPaintLogicalState EmptyState;
	EmptyState.BrickCounts = FIntVector(1, 1, 1);
	FVoxelPaintCanonicalBuild EmptyBuild;
	BuildCanonicalSource(EmptyState, EmptyBuild);
	TestTrue(TEXT("Empty state builds an empty manifest"), EmptyBuild.Manifest.PopulatedBricks.Num() == 0);
	const FVoxelPaintCanonicalIdentity EmptyIdentity = ComputeCanonicalIdentity(EmptyBuild, Origin, HalfExtent);
	TestTrue(TEXT("Empty state has a valid hash"), EmptyIdentity.IsValid());
	TestTrue(TEXT("Empty-state hash is deterministic"),
		EmptyIdentity == ComputeCanonicalIdentity(EmptyBuild, Origin, HalfExtent));
	TestTrue(TEXT("Empty-state hash differs from a painted state"), EmptyIdentity != SmallIdentity);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintBC7_PayloadClassifier,
	"Game.WorldVoxelPaint.Unit.BC7.PayloadClassifier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintBC7_PayloadClassifier::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	const int32 NumRecords = 3;
	const FIntVector GoodDims = ComputePackedSourceDimsInBricks(NumRecords) * BrickSize;
	const int64 GoodBytes = (int64)NumRecords * BrickPayloadBytes;

	TestTrue(TEXT("Exact PF_BC7 + expected dims + expected size is Valid"),
		ClassifyBuiltPayload(PF_BC7, GoodDims, NumRecords, GoodBytes) == EVoxelPaintPayloadVerdict::Valid);
	TestTrue(TEXT("PF_B8G8R8A8 is WrongFormat"),
		ClassifyBuiltPayload(PF_B8G8R8A8, GoodDims, NumRecords, GoodBytes) == EVoxelPaintPayloadVerdict::WrongFormat);
	TestTrue(TEXT("Off-by-one-brick dims is WrongGeometry"),
		ClassifyBuiltPayload(PF_BC7, GoodDims + FIntVector(BrickSize, 0, 0), NumRecords, GoodBytes)
			== EVoxelPaintPayloadVerdict::WrongGeometry);
	TestTrue(TEXT("Negative record count is WrongGeometry"),
		ClassifyBuiltPayload(PF_BC7, GoodDims, -1, GoodBytes) == EVoxelPaintPayloadVerdict::WrongGeometry);
	TestTrue(TEXT("Truncated payload is WrongSize"),
		ClassifyBuiltPayload(PF_BC7, GoodDims, NumRecords, GoodBytes - BrickPayloadBytes)
			== EVoxelPaintPayloadVerdict::WrongSize);
	TestTrue(TEXT("One-byte-short payload is WrongSize"),
		ClassifyBuiltPayload(PF_BC7, GoodDims, NumRecords, GoodBytes - 1) == EVoxelPaintPayloadVerdict::WrongSize);

	const FIntVector BigDims = ComputePackedSourceDimsInBricks(17);
	TestTrue(TEXT("17-record dims validate against the padded slot count, not the record count"),
		ClassifyBuiltPayload(PF_BC7, BigDims * BrickSize, 17,
			(int64)BigDims.X * BigDims.Y * BigDims.Z * BrickPayloadBytes) == EVoxelPaintPayloadVerdict::Valid);

	return true;
}
