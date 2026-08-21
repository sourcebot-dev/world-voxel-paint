// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintVariantSelection.h"

#include "Hash/CityHash.h"

namespace WorldVoxelPaint
{

uint64 MakeIdentitySeed(const FGuid& ActorGuid)
{
	const uint32 Buffer[4] = { ActorGuid.A, ActorGuid.B, ActorGuid.C, ActorGuid.D };
	return CityHash64(reinterpret_cast<const char*>(Buffer), sizeof(Buffer));
}

uint64 MakeTransformSeed(const FQuat& Rotation, const FVector& Translation)
{
	int64 QuantizedTranslation[3];
	QuantizedTranslation[0] = FMath::RoundToInt64(Translation.X);
	QuantizedTranslation[1] = FMath::RoundToInt64(Translation.Y);
	QuantizedTranslation[2] = FMath::RoundToInt64(Translation.Z);

	int32 QuantizedRotation[4];
	QuantizedRotation[0] = FMath::RoundToInt32(Rotation.X * 128.0);
	QuantizedRotation[1] = FMath::RoundToInt32(Rotation.Y * 128.0);
	QuantizedRotation[2] = FMath::RoundToInt32(Rotation.Z * 128.0);
	QuantizedRotation[3] = FMath::RoundToInt32(Rotation.W * 128.0);

	bool bNegate = QuantizedRotation[3] < 0;
	if (QuantizedRotation[3] == 0)
	{
		for (int32 ComponentIndex = 0; ComponentIndex < 3; ++ComponentIndex)
		{
			if (QuantizedRotation[ComponentIndex] != 0)
			{
				bNegate = QuantizedRotation[ComponentIndex] < 0;
				break;
			}
		}
	}
	if (bNegate)
	{
		for (int32 ComponentIndex = 0; ComponentIndex < 4; ++ComponentIndex)
		{
			QuantizedRotation[ComponentIndex] = -QuantizedRotation[ComponentIndex];
		}
	}

	uint8 Buffer[40];
	static_assert(sizeof(QuantizedTranslation) + sizeof(QuantizedRotation) == sizeof(Buffer),
		"Transform seed buffer must be exactly 40 bytes");
	FMemory::Memcpy(Buffer, QuantizedTranslation, sizeof(QuantizedTranslation));
	FMemory::Memcpy(Buffer + sizeof(QuantizedTranslation), QuantizedRotation, sizeof(QuantizedRotation));
	return CityHash64(reinterpret_cast<const char*>(Buffer), sizeof(Buffer));
}

bool ClassifyAsIdentitySeeded(const FVoxelPaintSeedClassifierInputs& In)
{
	return In.bActorGuidValid && In.bInstanceGuidMatchesActorGuid && (In.bWasLoaded || In.bEditorWorld);
}

void ComputeValidSlots(const FVoxelPaintPoolView& Pool, TArray<int32>& OutValidSlots)
{
	OutValidSlots.Reset();
	for (int32 SlotIndex = 0; SlotIndex < Pool.Assets.Num(); ++SlotIndex)
	{
		const UVoxelPaintVolumeData* Asset = Pool.Assets[SlotIndex];
		if (!Asset)
		{
			continue;
		}
		bool bDuplicateOfEarlier = false;
		for (int32 EarlierIndex = 0; EarlierIndex < SlotIndex; ++EarlierIndex)
		{
			if (Pool.Assets[EarlierIndex] == Asset)
			{
				bDuplicateOfEarlier = true;
				break;
			}
		}
		if (!bDuplicateOfEarlier)
		{
			OutValidSlots.Add(SlotIndex);
		}
	}
}

int32 ResolveWeightedVariantSlot(uint64 Seed, const FVoxelPaintPoolView& Pool)
{
	TArray<int32> ValidSlots;
	ComputeValidSlots(Pool, ValidSlots);
	if (ValidSlots.Num() == 0)
	{
		return INDEX_NONE;
	}

	const auto EffectiveWeight = [&Pool](int32 Slot) -> double
	{
		const float Weight = Pool.Weights.IsValidIndex(Slot) ? Pool.Weights[Slot] : 0.0f;
		return (FMath::IsFinite(Weight) && Weight > 0.0f) ? double(Weight) : 0.0;
	};

	bool bAllZero = true;
	for (int32 Slot : ValidSlots)
	{
		if (EffectiveWeight(Slot) > 0.0)
		{
			bAllZero = false;
			break;
		}
	}

	TArray<int32> RollableSlots;
	TArray<double> CumulativeWeights;
	RollableSlots.Reserve(ValidSlots.Num());
	CumulativeWeights.Reserve(ValidSlots.Num());
	double Cumulative = 0.0;
	for (int32 Slot : ValidSlots)
	{
		const double Weight = bAllZero ? 1.0 : EffectiveWeight(Slot);
		if (Weight <= 0.0)
		{
			continue;
		}
		Cumulative += Weight;
		RollableSlots.Add(Slot);
		CumulativeWeights.Add(Cumulative);
	}

	const double Uniform = double(Seed >> 11) * (1.0 / 9007199254740992.0);
	const double Threshold = Uniform * Cumulative;
	for (int32 RollIndex = 0; RollIndex < RollableSlots.Num(); ++RollIndex)
	{
		if (CumulativeWeights[RollIndex] > Threshold)
		{
			return RollableSlots[RollIndex];
		}
	}
	return RollableSlots.Last();
}

} // namespace WorldVoxelPaint
