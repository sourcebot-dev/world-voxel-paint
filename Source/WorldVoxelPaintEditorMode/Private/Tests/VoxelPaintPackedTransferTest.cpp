// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintCanonicalSource.h"
#include "VoxelPaintSessionCpuState.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintPackedTransferTestPrivate
{
	using namespace WorldVoxelPaint;

	static int32 FlattenTexel(const FIntVector& Resolution, const FIntVector& Coord)
	{
		return Coord.X + Resolution.X * (Coord.Y + Resolution.Y * Coord.Z);
	}

	static FIntVector UnflattenBrick(uint32 Flat, const FIntVector& BrickCounts)
	{
		return FIntVector(
			(int32)(Flat % (uint32)BrickCounts.X),
			(int32)((Flat / (uint32)BrickCounts.X) % (uint32)BrickCounts.Y),
			(int32)(Flat / ((uint32)BrickCounts.X * BrickCounts.Y)));
	}

	static FIntVector UnflattenInBrickTexel(int32 InBrick)
	{
		return FIntVector(
			InBrick % UniqueBrickSize,
			(InBrick / UniqueBrickSize) % UniqueBrickSize,
			InBrick / (UniqueBrickSize * UniqueBrickSize));
	}

	static void DensifyState(const FVoxelPaintLogicalState& State, TArray<FColor>& OutDense)
	{
		const FIntVector Resolution = State.BrickCounts * UniqueBrickSize;
		OutDense.Reset();
		OutDense.SetNumZeroed(Resolution.X * Resolution.Y * Resolution.Z);
		for (const TPair<FIntVector, FColor>& Texel : State.Texels)
		{
			OutDense[FlattenTexel(Resolution, Texel.Key)] = Texel.Value;
		}
	}

	static TArray<uint32> AllBricks(const FIntVector& BrickCounts)
	{
		TArray<uint32> Bricks;
		const int32 Total = BrickCounts.X * BrickCounts.Y * BrickCounts.Z;
		for (int32 Index = 0; Index < Total; ++Index)
		{
			Bricks.Add((uint32)Index);
		}
		return Bricks;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintSession_PackedTransfer,
	"Game.WorldVoxelPaint.Unit.Session.PackedTransfer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintSession_PackedTransfer::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;
	using namespace VoxelPaintPackedTransferTestPrivate;

	{
		const FColor Samples[] = {
			FColor(0, 0, 0, 0), FColor(255, 255, 255, 255), FColor(1, 2, 3, 4),
			FColor(254, 128, 127, 1), FColor(200, 10, 90, 128) };
		for (const FColor& Sample : Samples)
		{
			TestTrue(TEXT("Pack/unpack round-trips the color"), UnpackTexelBGRA(PackTexelBGRA(Sample)) == Sample);
			// The engine value whose little-endian MEMORY order is B,G,R,A is ToPackedARGB
			// ((A<<24)|(R<<16)|(G<<8)|B); FColor::ToPackedBGRA is the value with B in the TOP byte.
			TestTrue(TEXT("Pack matches the engine packed value with BGRA memory order (ToPackedARGB)"),
				PackTexelBGRA(Sample) == Sample.ToPackedARGB());
		}
		const uint32 PackedSamples[] = { 0u, 0xFFFFFFFFu, 0x01020304u, 0x80FF7F01u };
		for (uint32 Packed : PackedSamples)
		{
			TestTrue(TEXT("Unpack/pack round-trips the packed word"), PackTexelBGRA(UnpackTexelBGRA(Packed)) == Packed);
		}
		TestTrue(TEXT("Byte order pinned: B bits 0-7, G 8-15, R 16-23, A 24-31"),
			PackTexelBGRA(FColor(1, 2, 3, 4)) == 0x04010203u);
	}

	{
		TestTrue(TEXT("UNorm pack: zero maps to zero"),
			PackUNorm4RoundNearest(FVector4f(0.0f, 0.0f, 0.0f, 0.0f)) == 0u);
		TestTrue(TEXT("UNorm pack: one saturates every channel"),
			PackUNorm4RoundNearest(FVector4f(1.0f, 1.0f, 1.0f, 1.0f)) == 0xFFFFFFFFu);
		TestTrue(TEXT("UNorm pack: above one clamps to 255"),
			PackUNorm4RoundNearest(FVector4f(2.0f, 1.5f, 100.0f, 1.0001f)) == 0xFFFFFFFFu);
		TestTrue(TEXT("UNorm pack: negatives clamp to zero"),
			PackUNorm4RoundNearest(FVector4f(-1.0f, -0.25f, -100.0f, -0.0f)) == 0u);
		TestTrue(TEXT("UNorm pack: the 127.5 tie rounds up to 128"),
			PackUNorm4RoundNearest(FVector4f(0.5f, 0.0f, 0.0f, 0.0f)) == PackTexelBGRA(FColor(128, 0, 0, 0)));
		TestTrue(TEXT("UNorm pack: channel placement X=R"),
			PackUNorm4RoundNearest(FVector4f(1.0f, 0.0f, 0.0f, 0.0f)) == PackTexelBGRA(FColor(255, 0, 0, 0)));
		TestTrue(TEXT("UNorm pack: channel placement Y=G"),
			PackUNorm4RoundNearest(FVector4f(0.0f, 1.0f, 0.0f, 0.0f)) == PackTexelBGRA(FColor(0, 255, 0, 0)));
		TestTrue(TEXT("UNorm pack: channel placement Z=B"),
			PackUNorm4RoundNearest(FVector4f(0.0f, 0.0f, 1.0f, 0.0f)) == PackTexelBGRA(FColor(0, 0, 255, 0)));
		TestTrue(TEXT("UNorm pack: channel placement W=A"),
			PackUNorm4RoundNearest(FVector4f(0.0f, 0.0f, 0.0f, 1.0f)) == PackTexelBGRA(FColor(0, 0, 0, 255)));
		TestTrue(TEXT("UNorm pack: 1/255 quantum round-trips"),
			PackUNorm4RoundNearest(FVector4f(1.0f / 255.0f, 0.0f, 0.0f, 0.0f)) == PackTexelBGRA(FColor(1, 0, 0, 0)));
	}

	{
		const FIntVector BrickCounts(3, 2, 1);
		FVoxelPaintSessionCpuState State;
		State.Initialize(BrickCounts);

		FVoxelPaintPackedBrickTransfer In;
		In.BrickCounts = BrickCounts;
		In.BrickIndices = { 1u, 4u };
		In.Texels.SetNumUninitialized(In.BrickIndices.Num() * TexelsPerUniqueBrick);
		for (int32 Rank = 0; Rank < In.BrickIndices.Num(); ++Rank)
		{
			for (int32 InBrick = 0; InBrick < TexelsPerUniqueBrick; ++InBrick)
			{
				In.Texels[Rank * TexelsPerUniqueBrick + InBrick] = PackTexelBGRA(
					FColor((uint8)(Rank + 1), (uint8)(InBrick & 0xFF), (uint8)(InBrick >> 8), 255));
			}
		}
		TestTrue(TEXT("Constructed transfer is well-formed"), In.IsValid());

		State.MergePackedTransfer(In);
		const int32 SpotTexels[] = { 0, 1, 7, 49, 342 };
		for (int32 Rank = 0; Rank < In.BrickIndices.Num(); ++Rank)
		{
			const FIntVector Brick = UnflattenBrick(In.BrickIndices[Rank], BrickCounts);
			for (int32 InBrick : SpotTexels)
			{
				const FIntVector Coord = Brick * UniqueBrickSize + UnflattenInBrickTexel(InBrick);
				TestTrue(TEXT("Merge lands brick-major, X-fastest within the brick"),
					State.GetTexel(Coord)
						== FColor((uint8)(Rank + 1), (uint8)(InBrick & 0xFF), (uint8)(InBrick >> 8), 255));
			}
		}
		TestTrue(TEXT("Untouched brick stays exact zero"), State.GetTexel(FIntVector(0, 0, 0)) == FColor(0, 0, 0, 0));

		FVoxelPaintPackedBrickTransfer Out;
		State.BuildPackedTransfer(In.BrickIndices, Out);
		TestTrue(TEXT("Build mirrors the merged payload exactly (bijective packing)"),
			Out.BrickCounts == In.BrickCounts && Out.BrickIndices == In.BrickIndices && Out.Texels == In.Texels);

		FVoxelPaintPackedBrickTransfer WholeDomain;
		State.BuildPackedTransfer(AllBricks(BrickCounts), WholeDomain);
		FVoxelPaintSessionCpuState Rebuilt;
		Rebuilt.Initialize(BrickCounts);
		Rebuilt.MergePackedTransfer(WholeDomain);
		TestTrue(TEXT("Whole-domain transfer round-trips the dense state"), Rebuilt.EqualsDense(State));
	}

	{
		const FIntVector ElongatedCounts[] = { FIntVector(1, 8, 1), FIntVector(5, 1, 1), FIntVector(1, 1, 6) };
		for (const FIntVector& BrickCounts : ElongatedCounts)
		{
			FVoxelPaintLogicalState State;
			BuildFixtureState(EVoxelPaintFixturePreset::Gradient, BrickCounts, State);
			FVoxelPaintCanonicalBuild Build;
			BuildCanonicalSource(State, Build);

			FVoxelPaintSessionCpuState Hydrated;
			TestTrue(TEXT("Elongated domain hydrates from the canonical source"),
				Hydrated.InitializeFromCanonical(BrickCounts, Build.Manifest.PopulatedBricks, Build.PackedSourceBGRA8));

			TArray<FColor> Dense;
			DensifyState(State, Dense);
			const TConstArrayView<FColor> HydratedDense = Hydrated.GetDense();
			TestTrue(TEXT("Hydrated dense state matches the logical state"),
				HydratedDense.Num() == Dense.Num()
					&& FMemory::Memcmp(HydratedDense.GetData(), Dense.GetData(),
						Dense.Num() * sizeof(FColor)) == 0);

			FVoxelPaintPackedBrickTransfer WholeDomain;
			Hydrated.BuildPackedTransfer(AllBricks(BrickCounts), WholeDomain);
			FVoxelPaintSessionCpuState Rebuilt;
			Rebuilt.Initialize(BrickCounts);
			Rebuilt.MergePackedTransfer(WholeDomain);
			TestTrue(TEXT("Elongated whole-domain transfer round-trips"), Rebuilt.EqualsDense(Hydrated));
		}
	}

	{
		const FIntVector BrickCounts(2, 2, 2);
		FVoxelPaintLogicalState State;
		BuildFixtureState(EVoxelPaintFixturePreset::SparseBlob, BrickCounts, State);
		FVoxelPaintCanonicalBuild Build;
		BuildCanonicalSource(State, Build);

		FVoxelPaintSessionCpuState Truth;
		Truth.InitializeFromCanonical(BrickCounts, Build.Manifest.PopulatedBricks, Build.PackedSourceBGRA8);
		FVoxelPaintSessionCpuState Corrupted;
		Corrupted.InitializeFromCanonical(BrickCounts, Build.Manifest.PopulatedBricks, Build.PackedSourceBGRA8);

		FVoxelPaintPackedBrickTransfer Garbage;
		Garbage.BrickCounts = BrickCounts;
		Garbage.BrickIndices = { 3u };
		Garbage.Texels.Init(PackTexelBGRA(FColor(9, 9, 9, 9)), TexelsPerUniqueBrick);
		Corrupted.MergePackedTransfer(Garbage);
		TestFalse(TEXT("Corruption visibly diverges the states"), Corrupted.EqualsDense(Truth));

		FVoxelPaintPackedBrickTransfer Restore;
		const uint32 RestoreBricks[] = { 3u };
		Truth.BuildPackedTransfer(RestoreBricks, Restore);
		Corrupted.MergePackedTransfer(Restore);
		TestTrue(TEXT("Merging the exact packed page restores byte equality"), Corrupted.EqualsDense(Truth));
	}

	{
		const FVector StorageOrigin(1.0, 2.0, 3.0);
		const FVector HalfExtent(500.0, 400.0, 300.0);
		struct FAdapterCase
		{
			EVoxelPaintFixturePreset Preset;
			FIntVector BrickCounts;
		};
		const FAdapterCase Cases[] = {
			{ EVoxelPaintFixturePreset::Gradient, FIntVector(2, 2, 2) },
			{ EVoxelPaintFixturePreset::Checker, FIntVector(2, 2, 2) },
			{ EVoxelPaintFixturePreset::SparseBlob, FIntVector(2, 2, 2) },
			{ EVoxelPaintFixturePreset::BorderProbe, FIntVector(2, 2, 1) },
			{ EVoxelPaintFixturePreset::Gradient, FIntVector(1, 4, 1) } };

		for (const FAdapterCase& Case : Cases)
		{
			FVoxelPaintLogicalState State;
			BuildFixtureState(Case.Preset, Case.BrickCounts, State);

			FVoxelPaintCanonicalBuild MapBuild;
			BuildCanonicalSource(State, MapBuild);
			FVoxelPaintSourceManifest MapManifest;
			BuildBrickManifest(State, MapManifest);

			TArray<FColor> Dense;
			DensifyState(State, Dense);
			FVoxelPaintCanonicalBuild DenseBuild;
			BuildCanonicalSourceFromDense(Case.BrickCounts, Dense, DenseBuild);
			FVoxelPaintSourceManifest DenseManifest;
			BuildBrickManifestFromDense(Case.BrickCounts, Dense, DenseManifest);

			TestTrue(TEXT("Dense adapter: manifest identical to the TMap path"),
				DenseManifest.BrickCounts == MapManifest.BrickCounts
					&& DenseManifest.PopulatedBricks == MapManifest.PopulatedBricks);
			TestTrue(TEXT("Dense adapter: build manifest identical to the TMap path"),
				DenseBuild.Manifest.BrickCounts == MapBuild.Manifest.BrickCounts
					&& DenseBuild.Manifest.PopulatedBricks == MapBuild.Manifest.PopulatedBricks);
			TestTrue(TEXT("Dense adapter: physical dims identical to the TMap path"),
				DenseBuild.PhysicalDimsInBricks == MapBuild.PhysicalDimsInBricks);
			TestTrue(TEXT("Dense adapter: packed source BYTE-IDENTICAL to the TMap path"),
				DenseBuild.PackedSourceBGRA8 == MapBuild.PackedSourceBGRA8);
			TestTrue(TEXT("Dense adapter: canonical identity identical to the TMap path"),
				ComputeCanonicalIdentity(DenseBuild, StorageOrigin, HalfExtent)
					== ComputeCanonicalIdentity(MapBuild, StorageOrigin, HalfExtent));
		}
	}

	return true;
}
