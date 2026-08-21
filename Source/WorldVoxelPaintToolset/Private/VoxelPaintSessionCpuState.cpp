// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintSessionCpuState.h"

#include "VoxelPaintCanonicalSource.h"
#include "VoxelPaintStrokeJournal.h"

namespace WorldVoxelPaint
{

namespace SessionCpuStatePrivate
{
	static uint32 UNormByteRoundNearest(float Value)
	{
		if (!(Value > 0.0f))
		{
			return 0u;
		}
		if (Value >= 1.0f)
		{
			return 255u;
		}
		return (uint32)(Value * 255.0f + 0.5f);
	}

	static int64 FlattenTexel(const FIntVector& Resolution, const FIntVector& Coord)
	{
		return (int64)Coord.X + (int64)Resolution.X * ((int64)Coord.Y + (int64)Resolution.Y * Coord.Z);
	}

	static FIntVector UnflattenBrick(uint32 Flat, const FIntVector& BrickCounts)
	{
		const uint32 CountX = (uint32)BrickCounts.X;
		const uint32 CountY = (uint32)BrickCounts.Y;
		return FIntVector(
			(int32)(Flat % CountX),
			(int32)((Flat / CountX) % CountY),
			(int32)(Flat / (CountX * CountY)));
	}

	static FIntVector UnflattenInBrickTexel(int32 InBrickIndex)
	{
		return FIntVector(
			InBrickIndex % UniqueBrickSize,
			(InBrickIndex / UniqueBrickSize) % UniqueBrickSize,
			InBrickIndex / (UniqueBrickSize * UniqueBrickSize));
	}

	static bool ValidateBrickList(TConstArrayView<uint32> BrickIndices, const FIntVector& BrickCounts)
	{
		const int64 TotalBricks = (int64)BrickCounts.X * BrickCounts.Y * BrickCounts.Z;
		for (int32 Index = 0; Index < BrickIndices.Num(); ++Index)
		{
			if ((int64)BrickIndices[Index] >= TotalBricks)
			{
				return false;
			}
			if (Index > 0 && BrickIndices[Index] <= BrickIndices[Index - 1])
			{
				return false;
			}
		}
		return true;
	}
}

uint32 PackTexelBGRA(const FColor& Texel)
{
	return (uint32)Texel.B | ((uint32)Texel.G << 8) | ((uint32)Texel.R << 16) | ((uint32)Texel.A << 24);
}

FColor UnpackTexelBGRA(uint32 Packed)
{
	return FColor(
		(uint8)((Packed >> 16) & 0xFFu),
		(uint8)((Packed >> 8) & 0xFFu),
		(uint8)(Packed & 0xFFu),
		(uint8)((Packed >> 24) & 0xFFu));
}

uint32 PackUNorm4RoundNearest(const FVector4f& Value)
{
	using namespace SessionCpuStatePrivate;
	return UNormByteRoundNearest(Value.Z)
		| (UNormByteRoundNearest(Value.Y) << 8)
		| (UNormByteRoundNearest(Value.X) << 16)
		| (UNormByteRoundNearest(Value.W) << 24);
}

void FVoxelPaintSessionCpuState::Initialize(const FIntVector& InBrickCounts)
{
	checkf(InBrickCounts.X > 0 && InBrickCounts.Y > 0 && InBrickCounts.Z > 0,
		TEXT("Session brick counts must be positive: (%d,%d,%d)"),
		InBrickCounts.X, InBrickCounts.Y, InBrickCounts.Z);

	BrickCounts = InBrickCounts;
	const FIntVector Resolution = GetResolution();
	const int64 NumTexels = (int64)Resolution.X * Resolution.Y * Resolution.Z;
	checkf(NumTexels <= MAX_int32, TEXT("Session domain too large: %lld texels"), NumTexels);

	Dense.Reset();
	Dense.SetNumZeroed((int32)NumTexels);
}

bool FVoxelPaintSessionCpuState::InitializeFromCanonical(const FIntVector& InBrickCounts,
	TConstArrayView<uint32> ManifestBricks, TConstArrayView<uint8> PackedSourceBGRA8)
{
	using namespace SessionCpuStatePrivate;

	if (InBrickCounts.X <= 0 || InBrickCounts.Y <= 0 || InBrickCounts.Z <= 0)
	{
		return false;
	}
	const FIntVector Resolution = InBrickCounts * UniqueBrickSize;
	if ((int64)Resolution.X * Resolution.Y * Resolution.Z > MAX_int32)
	{
		return false;
	}
	if (!ValidateBrickList(ManifestBricks, InBrickCounts))
	{
		return false;
	}
	const FIntVector DimsInBricks = ComputePackedSourceDimsInBricks(ManifestBricks.Num());
	const FIntVector PhysTexels = DimsInBricks * BrickSize;
	const int64 ExpectedBytes = (int64)PhysTexels.X * PhysTexels.Y * PhysTexels.Z * SessionTexelBytes;
	if ((int64)PackedSourceBGRA8.Num() != ExpectedBytes)
	{
		return false;
	}

	Initialize(InBrickCounts);

	const uint8* Bytes = PackedSourceBGRA8.GetData();
	for (int32 Rank = 0; Rank < ManifestBricks.Num(); ++Rank)
	{
		const FIntVector Physical = PackedSourceRankToPhysicalBrick(Rank);
		const FIntVector Brick = UnflattenBrick(ManifestBricks[Rank], InBrickCounts);
		for (int32 TexelZ = 0; TexelZ < UniqueBrickSize; ++TexelZ)
		{
			for (int32 TexelY = 0; TexelY < UniqueBrickSize; ++TexelY)
			{
				for (int32 TexelX = 0; TexelX < UniqueBrickSize; ++TexelX)
				{
					const int64 SourceTexel =
						(int64)(Physical.Z * BrickSize + TexelZ) * PhysTexels.X * PhysTexels.Y
						+ (int64)(Physical.Y * BrickSize + TexelY) * PhysTexels.X
						+ (Physical.X * BrickSize + TexelX);
					const uint8* Texel = Bytes + SourceTexel * SessionTexelBytes;
					const FIntVector Logical = Brick * UniqueBrickSize + FIntVector(TexelX, TexelY, TexelZ);
					Dense[(int32)FlattenTexel(Resolution, Logical)] =
						FColor(Texel[2], Texel[1], Texel[0], Texel[3]);
				}
			}
		}
	}
	return true;
}

FIntVector FVoxelPaintSessionCpuState::GetResolution() const
{
	return BrickCounts * UniqueBrickSize;
}

FColor FVoxelPaintSessionCpuState::GetTexel(const FIntVector& Coord) const
{
	const FIntVector Resolution = GetResolution();
	checkf(Coord.X >= 0 && Coord.X < Resolution.X
		&& Coord.Y >= 0 && Coord.Y < Resolution.Y
		&& Coord.Z >= 0 && Coord.Z < Resolution.Z,
		TEXT("GetTexel out of range: (%d,%d,%d) resolution (%d,%d,%d)"),
		Coord.X, Coord.Y, Coord.Z, Resolution.X, Resolution.Y, Resolution.Z);
	return Dense[(int32)SessionCpuStatePrivate::FlattenTexel(Resolution, Coord)];
}

TConstArrayView<FColor> FVoxelPaintSessionCpuState::GetDense() const
{
	return Dense;
}

void FVoxelPaintSessionCpuState::BuildPackedTransfer(TConstArrayView<uint32> BrickIndices,
	FVoxelPaintPackedBrickTransfer& Out) const
{
	using namespace SessionCpuStatePrivate;

	checkf(Dense.Num() > 0, TEXT("BuildPackedTransfer on uninitialized session state"));
	checkf(ValidateBrickList(BrickIndices, BrickCounts),
		TEXT("BuildPackedTransfer brick list must be ascending and in range"));

	const FIntVector Resolution = GetResolution();
	Out.BrickCounts = BrickCounts;
	Out.BrickIndices = TArray<uint32>(BrickIndices.GetData(), BrickIndices.Num());
	Out.Texels.SetNumUninitialized(BrickIndices.Num() * TexelsPerUniqueBrick);

	uint32* OutTexel = Out.Texels.GetData();
	for (int32 Rank = 0; Rank < BrickIndices.Num(); ++Rank)
	{
		const FIntVector Brick = UnflattenBrick(BrickIndices[Rank], BrickCounts);
		for (int32 InBrick = 0; InBrick < TexelsPerUniqueBrick; ++InBrick)
		{
			const FIntVector Logical = Brick * UniqueBrickSize + UnflattenInBrickTexel(InBrick);
			*OutTexel++ = PackTexelBGRA(Dense[(int32)FlattenTexel(Resolution, Logical)]);
		}
	}
}

void FVoxelPaintSessionCpuState::MergePackedTransfer(const FVoxelPaintPackedBrickTransfer& In)
{
	using namespace SessionCpuStatePrivate;

	checkf(Dense.Num() > 0, TEXT("MergePackedTransfer on uninitialized session state"));
	checkf(In.IsValid(), TEXT("MergePackedTransfer with malformed transfer"));
	checkf(In.BrickCounts == BrickCounts,
		TEXT("MergePackedTransfer brick counts mismatch: (%d,%d,%d) vs (%d,%d,%d)"),
		In.BrickCounts.X, In.BrickCounts.Y, In.BrickCounts.Z, BrickCounts.X, BrickCounts.Y, BrickCounts.Z);
	checkf(ValidateBrickList(In.BrickIndices, BrickCounts),
		TEXT("MergePackedTransfer brick list must be ascending and in range"));

	const FIntVector Resolution = GetResolution();
	const uint32* InTexel = In.Texels.GetData();
	for (int32 Rank = 0; Rank < In.BrickIndices.Num(); ++Rank)
	{
		const FIntVector Brick = UnflattenBrick(In.BrickIndices[Rank], BrickCounts);
		for (int32 InBrick = 0; InBrick < TexelsPerUniqueBrick; ++InBrick)
		{
			const FIntVector Logical = Brick * UniqueBrickSize + UnflattenInBrickTexel(InBrick);
			Dense[(int32)FlattenTexel(Resolution, Logical)] = UnpackTexelBGRA(*InTexel++);
		}
	}
}

void FVoxelPaintSessionCpuState::ApplyDelta(const FVoxelPaintStrokeDelta& Delta, bool bUseAfter)
{
	using namespace SessionCpuStatePrivate;

	checkf(Dense.Num() > 0, TEXT("ApplyDelta on uninitialized session state"));

	const FIntVector Resolution = GetResolution();
	const int64 TotalBricks = (int64)BrickCounts.X * BrickCounts.Y * BrickCounts.Z;
	constexpr int32 MaskWords = (TexelsPerUniqueBrick + 63) / 64;

	for (const FVoxelPaintBrickDelta& BrickDelta : Delta.Bricks)
	{
		checkf((int64)BrickDelta.BrickIndex < TotalBricks,
			TEXT("Delta brick out of range: %u"), BrickDelta.BrickIndex);
		checkf(BrickDelta.TouchedMask.Num() == MaskWords,
			TEXT("Delta mask must carry %d words, got %d"), MaskWords, BrickDelta.TouchedMask.Num());
		checkf(BrickDelta.Before.Num() == BrickDelta.After.Num(),
			TEXT("Delta before/after counts differ: %d vs %d"),
			BrickDelta.Before.Num(), BrickDelta.After.Num());

		const TArray<uint32>& Values = bUseAfter ? BrickDelta.After : BrickDelta.Before;
		const FIntVector Brick = UnflattenBrick(BrickDelta.BrickIndex, BrickCounts);
		int32 ValueIndex = 0;
		for (int32 InBrick = 0; InBrick < TexelsPerUniqueBrick; ++InBrick)
		{
			if ((BrickDelta.TouchedMask[InBrick / 64] & (1ull << (InBrick % 64))) == 0)
			{
				continue;
			}
			checkf(ValueIndex < Values.Num(),
				TEXT("Delta mask names more texels than values carried: %d"), Values.Num());
			const FIntVector Logical = Brick * UniqueBrickSize + UnflattenInBrickTexel(InBrick);
			Dense[(int32)FlattenTexel(Resolution, Logical)] = UnpackTexelBGRA(Values[ValueIndex++]);
		}
		checkf(ValueIndex == Values.Num(),
			TEXT("Delta carries %d values but mask names %d texels"), Values.Num(), ValueIndex);
	}
}

bool FVoxelPaintSessionCpuState::EqualsDense(const FVoxelPaintSessionCpuState& Other) const
{
	return BrickCounts == Other.BrickCounts && Dense == Other.Dense;
}

}
