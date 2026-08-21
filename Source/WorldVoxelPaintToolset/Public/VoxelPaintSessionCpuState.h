// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "WorldVoxelPaintConstants.h"

namespace WorldVoxelPaint
{
	struct FVoxelPaintStrokeDelta;

	WORLDVOXELPAINTTOOLSET_API uint32 PackTexelBGRA(const FColor& Texel);
	WORLDVOXELPAINTTOOLSET_API FColor UnpackTexelBGRA(uint32 Packed);
	WORLDVOXELPAINTTOOLSET_API uint32 PackUNorm4RoundNearest(const FVector4f& Value);

	struct FVoxelPaintPackedBrickTransfer
	{
		FIntVector BrickCounts = FIntVector::ZeroValue;
		TArray<uint32> BrickIndices;
		TArray<uint32> Texels;

		bool IsValid() const
		{
			return Texels.Num() == BrickIndices.Num() * TexelsPerUniqueBrick;
		}
	};

	class WORLDVOXELPAINTTOOLSET_API FVoxelPaintSessionCpuState
	{
	public:
		void Initialize(const FIntVector& InBrickCounts);
		bool InitializeFromCanonical(const FIntVector& InBrickCounts,
			TConstArrayView<uint32> ManifestBricks, TConstArrayView<uint8> PackedSourceBGRA8);
		FIntVector GetResolution() const;
		FColor GetTexel(const FIntVector& Coord) const;
		TConstArrayView<FColor> GetDense() const;
		void BuildPackedTransfer(TConstArrayView<uint32> BrickIndices,
			FVoxelPaintPackedBrickTransfer& Out) const;
		void MergePackedTransfer(const FVoxelPaintPackedBrickTransfer& In);
		void ApplyDelta(const FVoxelPaintStrokeDelta& Delta, bool bUseAfter);
		bool EqualsDense(const FVoxelPaintSessionCpuState& Other) const;

	private:
		FIntVector BrickCounts = FIntVector::ZeroValue;
		TArray<FColor> Dense;
	};
}
