// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintCanonicalSource.h"

#include "Containers/BitArray.h"
#include "Misc/SecureHash.h"
#include "WorldVoxelPaintConstants.h"

namespace WorldVoxelPaint
{

namespace CanonicalSourcePrivate
{
	static bool IsZeroColor(const FColor& Value)
	{
		return Value.R == 0 && Value.G == 0 && Value.B == 0 && Value.A == 0;
	}

	static FIntVector UnflattenBrickIndex(uint32 Flat, const FIntVector& BrickCounts)
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

	static void HashDouble(FSHA1& Sha, double Value)
	{
		Sha.Update(reinterpret_cast<const uint8*>(&Value), sizeof(double));
	}

	static FIntVector ClampedLogicalTexel(const FIntVector& Resolution,
		const FIntVector& BrickCoord, const FIntVector& PhysicalTexel)
	{
		return FIntVector(
			FMath::Clamp(BrickCoord.X * UniqueBrickSize + PhysicalTexel.X, 0, Resolution.X - 1),
			FMath::Clamp(BrickCoord.Y * UniqueBrickSize + PhysicalTexel.Y, 0, Resolution.Y - 1),
			FMath::Clamp(BrickCoord.Z * UniqueBrickSize + PhysicalTexel.Z, 0, Resolution.Z - 1));
	}

	static void EmitManifest(const TBitArray<>& Populated, FVoxelPaintSourceManifest& OutManifest)
	{
		for (TConstSetBitIterator<> It(Populated); It; ++It)
		{
			OutManifest.PopulatedBricks.Add((uint32)It.GetIndex());
		}
	}

	static void PackCanonicalSource(const FIntVector& BrickCounts,
		TFunctionRef<FColor(const FIntVector& BrickCoord, const FIntVector& PhysicalTexel)> ResolveTexel,
		FVoxelPaintCanonicalBuild& OutBuild)
	{
		const int32 NumPackedRecords = OutBuild.Manifest.PopulatedBricks.Num();
		OutBuild.PhysicalDimsInBricks = ComputePackedSourceDimsInBricks(NumPackedRecords);

		const FIntVector PhysTexels = OutBuild.PhysicalDimsInBricks * BrickSize;
		const int64 TotalBytes = (int64)PhysTexels.X * PhysTexels.Y * PhysTexels.Z * 4;
		checkf(TotalBytes <= MAX_int32, TEXT("Packed source too large: %lld bytes"), TotalBytes);

		OutBuild.PackedSourceBGRA8.Reset();
		OutBuild.PackedSourceBGRA8.SetNumZeroed((int32)TotalBytes);

		uint8* Bytes = OutBuild.PackedSourceBGRA8.GetData();
		for (int32 Rank = 0; Rank < NumPackedRecords; ++Rank)
		{
			const FIntVector Physical = RankToPhysicalBrick(Rank);
			const FIntVector Brick = UnflattenBrickIndex(
				OutBuild.Manifest.PopulatedBricks[Rank], BrickCounts);
			for (int32 TexelZ = 0; TexelZ < BrickSize; ++TexelZ)
			{
				for (int32 TexelY = 0; TexelY < BrickSize; ++TexelY)
				{
					for (int32 TexelX = 0; TexelX < BrickSize; ++TexelX)
					{
						const FColor Value = ResolveTexel(Brick, FIntVector(TexelX, TexelY, TexelZ));
						const int64 TexelIndex =
							(int64)(Physical.Z * BrickSize + TexelZ) * PhysTexels.X * PhysTexels.Y
							+ (int64)(Physical.Y * BrickSize + TexelY) * PhysTexels.X
							+ (Physical.X * BrickSize + TexelX);
						uint8* Texel = Bytes + TexelIndex * 4;
						Texel[0] = Value.B;
						Texel[1] = Value.G;
						Texel[2] = Value.R;
						Texel[3] = Value.A;
					}
				}
			}
		}
	}
}

void FVoxelPaintLogicalState::SetTexel(const FIntVector& Coord, FColor Value)
{
	const FIntVector Resolution = BrickCounts * UniqueBrickSize;
	checkf(Coord.X >= 0 && Coord.X < Resolution.X
		&& Coord.Y >= 0 && Coord.Y < Resolution.Y
		&& Coord.Z >= 0 && Coord.Z < Resolution.Z,
		TEXT("SetTexel out of range: (%d,%d,%d) resolution (%d,%d,%d)"),
		Coord.X, Coord.Y, Coord.Z, Resolution.X, Resolution.Y, Resolution.Z);

	if (CanonicalSourcePrivate::IsZeroColor(Value))
	{
		Texels.Remove(Coord);
	}
	else
	{
		Texels.Add(Coord, Value);
	}
}

void BuildBrickManifest(const FVoxelPaintLogicalState& State, FVoxelPaintSourceManifest& OutManifest)
{
	OutManifest.BrickCounts = State.BrickCounts;
	OutManifest.PopulatedBricks.Reset();

	const FIntVector BrickCounts = State.BrickCounts;
	const int64 TotalBricks = (int64)BrickCounts.X * BrickCounts.Y * BrickCounts.Z;
	if (TotalBricks <= 0)
	{
		return;
	}
	checkf(TotalBricks <= MAX_int32, TEXT("Brick grid too large: %lld"), TotalBricks);

	TBitArray<> Populated(false, (int32)TotalBricks);

	for (const TPair<FIntVector, FColor>& Pair : State.Texels)
	{
		if (CanonicalSourcePrivate::IsZeroColor(Pair.Value))
		{
			continue;
		}
		const FIntVector Coord = Pair.Key;
		int32 AxisBricks[3][2];
		int32 AxisNum[3];
		const int32 Coords[3] = { Coord.X, Coord.Y, Coord.Z };
		const int32 Counts[3] = { BrickCounts.X, BrickCounts.Y, BrickCounts.Z };
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const int32 Owner = Coords[Axis] / UniqueBrickSize;
			AxisNum[Axis] = 0;
			if (Owner < Counts[Axis])
			{
				AxisBricks[Axis][AxisNum[Axis]++] = Owner;
			}
			if ((Coords[Axis] % UniqueBrickSize) == 0 && Owner - 1 >= 0)
			{
				AxisBricks[Axis][AxisNum[Axis]++] = Owner - 1;
			}
		}
		for (int32 IndexX = 0; IndexX < AxisNum[0]; ++IndexX)
		{
			for (int32 IndexY = 0; IndexY < AxisNum[1]; ++IndexY)
			{
				for (int32 IndexZ = 0; IndexZ < AxisNum[2]; ++IndexZ)
				{
					const int32 Flat = AxisBricks[0][IndexX]
						+ BrickCounts.X * (AxisBricks[1][IndexY] + BrickCounts.Y * AxisBricks[2][IndexZ]);
					Populated[Flat] = true;
				}
			}
		}
	}

	CanonicalSourcePrivate::EmitManifest(Populated, OutManifest);
}

FColor ResolveBrickTexel(const FVoxelPaintLogicalState& State,
	const FIntVector& BrickCoord, const FIntVector& PhysicalTexel)
{
	checkf(PhysicalTexel.X >= 0 && PhysicalTexel.X < BrickSize
		&& PhysicalTexel.Y >= 0 && PhysicalTexel.Y < BrickSize
		&& PhysicalTexel.Z >= 0 && PhysicalTexel.Z < BrickSize,
		TEXT("Physical texel out of brick range: (%d,%d,%d)"),
		PhysicalTexel.X, PhysicalTexel.Y, PhysicalTexel.Z);

	const FIntVector Resolution = State.BrickCounts * UniqueBrickSize;
	const FIntVector Logical = CanonicalSourcePrivate::ClampedLogicalTexel(Resolution, BrickCoord, PhysicalTexel);

	if (const FColor* Found = State.Texels.Find(Logical))
	{
		return *Found;
	}
	return FColor(0, 0, 0, 0);
}

FIntVector ComputePackedSourceDimsInBricks(int32 NumPackedRecords)
{
	checkf(NumPackedRecords >= 0, TEXT("Negative packed record count: %d"), NumPackedRecords);
	const int32 BricksPerLayer = SourceBricksXY * SourceBricksXY;
	FIntVector Dims(
		FMath::Min(NumPackedRecords, SourceBricksXY),
		FMath::Min(FMath::DivideAndRoundUp(NumPackedRecords, SourceBricksXY), SourceBricksXY),
		FMath::DivideAndRoundUp(NumPackedRecords, BricksPerLayer));
	Dims.X = FMath::Max(Dims.X, 1);
	Dims.Y = FMath::Max(Dims.Y, 1);
	Dims.Z = FMath::Max(Dims.Z, 1);
	return Dims;
}

FIntVector PackedSourceRankToPhysicalBrick(int32 Rank)
{
	checkf(Rank >= 0, TEXT("Negative packed record rank: %d"), Rank);
	return CanonicalSourcePrivate::RankToPhysicalBrick(Rank);
}

void BuildCanonicalSource(const FVoxelPaintLogicalState& State, FVoxelPaintCanonicalBuild& OutBuild)
{
	OutBuild.Identity = FVoxelPaintCanonicalIdentity();
	BuildBrickManifest(State, OutBuild.Manifest);

	CanonicalSourcePrivate::PackCanonicalSource(State.BrickCounts,
		[&State](const FIntVector& BrickCoord, const FIntVector& PhysicalTexel)
		{
			return ResolveBrickTexel(State, BrickCoord, PhysicalTexel);
		},
		OutBuild);
}

void BuildBrickManifestFromDense(const FIntVector& BrickCounts,
	TConstArrayView<FColor> DenseTexels, FVoxelPaintSourceManifest& OutManifest)
{
	OutManifest.BrickCounts = BrickCounts;
	OutManifest.PopulatedBricks.Reset();

	const int64 TotalBricks = (int64)BrickCounts.X * BrickCounts.Y * BrickCounts.Z;
	if (TotalBricks <= 0)
	{
		return;
	}
	checkf(TotalBricks <= MAX_int32, TEXT("Brick grid too large: %lld"), TotalBricks);

	const FIntVector Resolution = BrickCounts * UniqueBrickSize;
	checkf((int64)DenseTexels.Num() == (int64)Resolution.X * Resolution.Y * Resolution.Z,
		TEXT("Dense view size mismatch: %d texels for resolution (%d,%d,%d)"),
		DenseTexels.Num(), Resolution.X, Resolution.Y, Resolution.Z);

	TBitArray<> Populated(false, (int32)TotalBricks);

	int32 Flat = 0;
	for (int32 BrickZ = 0; BrickZ < BrickCounts.Z; ++BrickZ)
	{
		for (int32 BrickY = 0; BrickY < BrickCounts.Y; ++BrickY)
		{
			for (int32 BrickX = 0; BrickX < BrickCounts.X; ++BrickX, ++Flat)
			{
				const FIntVector FootprintMin(
					BrickX * UniqueBrickSize, BrickY * UniqueBrickSize, BrickZ * UniqueBrickSize);
				const FIntVector FootprintMax(
					FMath::Min(FootprintMin.X + UniqueBrickSize, Resolution.X - 1),
					FMath::Min(FootprintMin.Y + UniqueBrickSize, Resolution.Y - 1),
					FMath::Min(FootprintMin.Z + UniqueBrickSize, Resolution.Z - 1));

				bool bPopulated = false;
				for (int32 TexelZ = FootprintMin.Z; TexelZ <= FootprintMax.Z && !bPopulated; ++TexelZ)
				{
					for (int32 TexelY = FootprintMin.Y; TexelY <= FootprintMax.Y && !bPopulated; ++TexelY)
					{
						const int64 Row = (int64)TexelZ * Resolution.X * Resolution.Y
							+ (int64)TexelY * Resolution.X;
						for (int32 TexelX = FootprintMin.X; TexelX <= FootprintMax.X; ++TexelX)
						{
							if (!CanonicalSourcePrivate::IsZeroColor(DenseTexels[(int32)(Row + TexelX)]))
							{
								bPopulated = true;
								break;
							}
						}
					}
				}
				if (bPopulated)
				{
					Populated[Flat] = true;
				}
			}
		}
	}

	CanonicalSourcePrivate::EmitManifest(Populated, OutManifest);
}

void BuildCanonicalSourceFromDense(const FIntVector& BrickCounts,
	TConstArrayView<FColor> DenseTexels, FVoxelPaintCanonicalBuild& OutBuild)
{
	OutBuild.Identity = FVoxelPaintCanonicalIdentity();
	BuildBrickManifestFromDense(BrickCounts, DenseTexels, OutBuild.Manifest);

	const FIntVector Resolution = BrickCounts * UniqueBrickSize;
	CanonicalSourcePrivate::PackCanonicalSource(BrickCounts,
		[&DenseTexels, &Resolution](const FIntVector& BrickCoord, const FIntVector& PhysicalTexel)
		{
			const FIntVector Logical = CanonicalSourcePrivate::ClampedLogicalTexel(
				Resolution, BrickCoord, PhysicalTexel);
			const int64 TexelIndex = (int64)Logical.X
				+ (int64)Resolution.X * ((int64)Logical.Y + (int64)Resolution.Y * Logical.Z);
			return DenseTexels[(int32)TexelIndex];
		},
		OutBuild);
}

FVoxelPaintCanonicalIdentity ComputeCanonicalIdentity(
	const FVoxelPaintCanonicalBuild& Build, const FVector& StorageOrigin, const FVector& LocalHalfExtent)
{
	FSHA1 Sha;

	const uint32 LayoutVersion = CanonicalLayoutVersion;
	Sha.Update(reinterpret_cast<const uint8*>(&LayoutVersion), sizeof(LayoutVersion));

	const int32 Counts[3] = { Build.Manifest.BrickCounts.X, Build.Manifest.BrickCounts.Y, Build.Manifest.BrickCounts.Z };
	Sha.Update(reinterpret_cast<const uint8*>(Counts), sizeof(Counts));

	CanonicalSourcePrivate::HashDouble(Sha, StorageOrigin.X);
	CanonicalSourcePrivate::HashDouble(Sha, StorageOrigin.Y);
	CanonicalSourcePrivate::HashDouble(Sha, StorageOrigin.Z);
	CanonicalSourcePrivate::HashDouble(Sha, LocalHalfExtent.X);
	CanonicalSourcePrivate::HashDouble(Sha, LocalHalfExtent.Y);
	CanonicalSourcePrivate::HashDouble(Sha, LocalHalfExtent.Z);

	if (Build.Manifest.PopulatedBricks.Num() > 0)
	{
		Sha.Update(reinterpret_cast<const uint8*>(Build.Manifest.PopulatedBricks.GetData()),
			(uint64)Build.Manifest.PopulatedBricks.Num() * sizeof(uint32));
	}
	if (Build.PackedSourceBGRA8.Num() > 0)
	{
		Sha.Update(Build.PackedSourceBGRA8.GetData(), (uint64)Build.PackedSourceBGRA8.Num());
	}

	Sha.Final();

	FVoxelPaintCanonicalIdentity Identity;
	Sha.GetHash(Identity.Hash.Hash);
	return Identity;
}

EVoxelPaintPayloadVerdict ClassifyBuiltPayload(
	EPixelFormat Format, const FIntVector& TexelDims, int32 ExpectedPackedRecords, int64 PayloadBytes)
{
	if (Format != PF_BC7)
	{
		return EVoxelPaintPayloadVerdict::WrongFormat;
	}
	if (ExpectedPackedRecords < 0)
	{
		return EVoxelPaintPayloadVerdict::WrongGeometry;
	}
	const FIntVector DimsInBricks = ComputePackedSourceDimsInBricks(ExpectedPackedRecords);
	if (TexelDims != DimsInBricks * BrickSize)
	{
		return EVoxelPaintPayloadVerdict::WrongGeometry;
	}
	const int64 ExpectedBytes = (int64)DimsInBricks.X * DimsInBricks.Y * DimsInBricks.Z * BrickPayloadBytes;
	if (PayloadBytes != ExpectedBytes)
	{
		return EVoxelPaintPayloadVerdict::WrongSize;
	}
	return EVoxelPaintPayloadVerdict::Valid;
}

const TCHAR* LexToString(EVoxelPaintPayloadVerdict Verdict)
{
	switch (Verdict)
	{
	case EVoxelPaintPayloadVerdict::Valid: return TEXT("Valid");
	case EVoxelPaintPayloadVerdict::WrongFormat: return TEXT("WrongFormat");
	case EVoxelPaintPayloadVerdict::WrongGeometry: return TEXT("WrongGeometry");
	case EVoxelPaintPayloadVerdict::WrongSize: return TEXT("WrongSize");
	default: return TEXT("Unknown");
	}
}

bool ExtractPackedBrickPayloads(TConstArrayView<uint8> PackedMipBytes,
	const FIntVector& DimsInBricks, int32 NumPackedRecords, TArray<uint8>& OutBrickPayload)
{
	OutBrickPayload.Reset();

	if (NumPackedRecords < 0 || DimsInBricks.X <= 0 || DimsInBricks.Y <= 0 || DimsInBricks.Z <= 0)
	{
		return false;
	}
	const int64 TotalBricks = (int64)DimsInBricks.X * DimsInBricks.Y * DimsInBricks.Z;
	if (NumPackedRecords > TotalBricks)
	{
		return false;
	}
	if ((int64)PackedMipBytes.Num() != TotalBricks * BrickPayloadBytes)
	{
		return false;
	}

	const int32 BlockBytes = 16;
	const int32 BrickBlocksXY = BrickSize / 4;
	const int32 RowPitchBytes = DimsInBricks.X * BrickBlocksXY * BlockBytes;
	const int64 SlicePitchBytes = (int64)RowPitchBytes * DimsInBricks.Y * BrickBlocksXY;
	const int32 BrickRowBytes = BrickBlocksXY * BlockBytes;

	OutBrickPayload.SetNumUninitialized(NumPackedRecords * BrickPayloadBytes);
	const uint8* Src = PackedMipBytes.GetData();
	uint8* Dst = OutBrickPayload.GetData();

	for (int32 Rank = 0; Rank < NumPackedRecords; ++Rank)
	{
		const FIntVector Physical = CanonicalSourcePrivate::RankToPhysicalBrick(Rank);
		uint8* DstBrick = Dst + (int64)Rank * BrickPayloadBytes;
		for (int32 Slice = 0; Slice < BrickSize; ++Slice)
		{
			for (int32 BlockRow = 0; BlockRow < BrickBlocksXY; ++BlockRow)
			{
				const int64 SrcOffset =
					(int64)(Physical.Z * BrickSize + Slice) * SlicePitchBytes
					+ (int64)(Physical.Y * BrickBlocksXY + BlockRow) * RowPitchBytes
					+ (int64)Physical.X * BrickBlocksXY * BlockBytes;
				FMemory::Memcpy(
					DstBrick + (Slice * BrickBlocksXY + BlockRow) * BrickRowBytes,
					Src + SrcOffset,
					BrickRowBytes);
			}
		}
	}
	return true;
}

namespace CanonicalSourcePrivate
{
	static void BuildGradientState(const FIntVector& BrickCounts, FVoxelPaintLogicalState& OutState)
	{
		const FIntVector Resolution = BrickCounts * UniqueBrickSize;
		for (int32 TexelZ = 0; TexelZ < Resolution.Z; ++TexelZ)
		{
			for (int32 TexelY = 0; TexelY < Resolution.Y; ++TexelY)
			{
				for (int32 TexelX = 0; TexelX < Resolution.X; ++TexelX)
				{
					const uint8 Red = Resolution.X > 1 ? (uint8)((TexelX * 255) / (Resolution.X - 1)) : 255;
					const uint8 Green = Resolution.Y > 1 ? (uint8)((TexelY * 255) / (Resolution.Y - 1)) : 255;
					const uint8 Blue = Resolution.Z > 1 ? (uint8)((TexelZ * 255) / (Resolution.Z - 1)) : 255;
					OutState.SetTexel(FIntVector(TexelX, TexelY, TexelZ), FColor(Red, Green, Blue, 255));
				}
			}
		}
	}

	static void BuildCheckerState(const FIntVector& BrickCounts, FVoxelPaintLogicalState& OutState)
	{
		const FColor PaintedColor(255, 255, 255, 255);
		for (int32 BrickZ = 0; BrickZ < BrickCounts.Z; ++BrickZ)
		{
			for (int32 BrickY = 0; BrickY < BrickCounts.Y; ++BrickY)
			{
				for (int32 BrickX = 0; BrickX < BrickCounts.X; ++BrickX)
				{
					if (((BrickX + BrickY + BrickZ) & 1) != 0)
					{
						continue;
					}
					const FIntVector PaintedMin(
						BrickX * UniqueBrickSize + 1,
						BrickY * UniqueBrickSize + 1,
						BrickZ * UniqueBrickSize + 1);
					for (int32 TexelZ = 0; TexelZ < UniqueBrickSize - 1; ++TexelZ)
					{
						for (int32 TexelY = 0; TexelY < UniqueBrickSize - 1; ++TexelY)
						{
							for (int32 TexelX = 0; TexelX < UniqueBrickSize - 1; ++TexelX)
							{
								OutState.SetTexel(PaintedMin + FIntVector(TexelX, TexelY, TexelZ), PaintedColor);
							}
						}
					}
				}
			}
		}
	}

	static void BuildSparseBlobState(const FIntVector& BrickCounts, FVoxelPaintLogicalState& OutState)
	{
		const FIntVector Resolution = BrickCounts * UniqueBrickSize;
		const FColor BlobColor(0, 128, 255, 255);
		const FColor IsolatedColor(255, 32, 32, 255);

		const FVector Center(Resolution.X * 0.5, Resolution.Y * 0.5, Resolution.Z * 0.5);
		const double Radius = FMath::Max(2.0, Resolution.GetMin() / 6.0);
		const double RadiusSq = Radius * Radius;

		const FIntVector BlobMin(
			FMath::Max(0, (int32)(Center.X - Radius) - 1),
			FMath::Max(0, (int32)(Center.Y - Radius) - 1),
			FMath::Max(0, (int32)(Center.Z - Radius) - 1));
		const FIntVector BlobMax(
			FMath::Min(Resolution.X - 1, (int32)(Center.X + Radius) + 1),
			FMath::Min(Resolution.Y - 1, (int32)(Center.Y + Radius) + 1),
			FMath::Min(Resolution.Z - 1, (int32)(Center.Z + Radius) + 1));

		for (int32 TexelZ = BlobMin.Z; TexelZ <= BlobMax.Z; ++TexelZ)
		{
			for (int32 TexelY = BlobMin.Y; TexelY <= BlobMax.Y; ++TexelY)
			{
				for (int32 TexelX = BlobMin.X; TexelX <= BlobMax.X; ++TexelX)
				{
					const FVector Delta = FVector(TexelX + 0.5, TexelY + 0.5, TexelZ + 0.5) - Center;
					if (Delta.SizeSquared() <= RadiusSq)
					{
						OutState.SetTexel(FIntVector(TexelX, TexelY, TexelZ), BlobColor);
					}
				}
			}
		}

		const int32 DiagonalBricks = BrickCounts.GetMin();
		for (int32 Diagonal = 0; Diagonal < DiagonalBricks; Diagonal += 2)
		{
			const FIntVector MinCorner(
				Diagonal * UniqueBrickSize,
				Diagonal * UniqueBrickSize,
				Diagonal * UniqueBrickSize);
			OutState.SetTexel(MinCorner, IsolatedColor);
		}
	}

	static void BuildBorderProbeState(const FIntVector& BrickCounts, FVoxelPaintLogicalState& OutState)
	{
		const FIntVector Resolution = BrickCounts * UniqueBrickSize;
		const FColor InteriorColor(40, 40, 40, 255);
		const FColor NeighbourPlaneColor(255, 0, 0, 255);
		const FColor DomainFaceColor(0, 255, 0, 255);
		const FColor EdgeDiagonalColor(0, 0, 255, 255);
		const FColor CornerColor(255, 255, 0, 255);

		for (int32 TexelZ = 0; TexelZ < UniqueBrickSize; ++TexelZ)
		{
			for (int32 TexelY = 0; TexelY < UniqueBrickSize; ++TexelY)
			{
				for (int32 TexelX = 0; TexelX < UniqueBrickSize; ++TexelX)
				{
					OutState.SetTexel(FIntVector(TexelX, TexelY, TexelZ), InteriorColor);
				}
			}
		}

		if (BrickCounts.X >= 2)
		{
			OutState.SetTexel(FIntVector(UniqueBrickSize, 3, 3), NeighbourPlaneColor);
		}

		OutState.SetTexel(FIntVector(Resolution.X - 1, 2, 2), DomainFaceColor);

		if (BrickCounts.Y >= 2)
		{
			OutState.SetTexel(FIntVector(Resolution.X - 1, UniqueBrickSize, 2), EdgeDiagonalColor);
		}

		OutState.SetTexel(Resolution - FIntVector(1, 1, 1), CornerColor);
	}
}

void BuildFixtureState(EVoxelPaintFixturePreset Preset, const FIntVector& BrickCounts,
	FVoxelPaintLogicalState& OutState)
{
	checkf(BrickCounts.X > 0 && BrickCounts.Y > 0 && BrickCounts.Z > 0,
		TEXT("Fixture brick counts must be positive: (%d,%d,%d)"),
		BrickCounts.X, BrickCounts.Y, BrickCounts.Z);

	OutState.BrickCounts = BrickCounts;
	OutState.Texels.Reset();

	switch (Preset)
	{
	case EVoxelPaintFixturePreset::Gradient:
		CanonicalSourcePrivate::BuildGradientState(BrickCounts, OutState);
		break;
	case EVoxelPaintFixturePreset::Checker:
		CanonicalSourcePrivate::BuildCheckerState(BrickCounts, OutState);
		break;
	case EVoxelPaintFixturePreset::SparseBlob:
		CanonicalSourcePrivate::BuildSparseBlobState(BrickCounts, OutState);
		break;
	case EVoxelPaintFixturePreset::BorderProbe:
		CanonicalSourcePrivate::BuildBorderProbeState(BrickCounts, OutState);
		break;
	default:
		checkNoEntry();
		break;
	}
}

}
