// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VoxelPaintVariantSelection.h"
#include "VoxelPaintVolumeData.h"

#include <limits>

namespace VoxelPaintVariantSelectionTestPrivate
{

struct FPoolBuilder
{
	WorldVoxelPaint::FVoxelPaintPoolView View;

	FPoolBuilder& Add(const UVoxelPaintVolumeData* Asset, float Weight)
	{
		View.Assets.Add(Asset);
		View.Weights.Add(Weight);
		return *this;
	}
};

constexpr uint64 SeedStride = 0x9E3779B97F4A7C15ull;

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintVariants_WeightedRollDeterminism,
	"Game.WorldVoxelPaint.Unit.Variants.WeightedRollDeterminism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintVariants_WeightedRollDeterminism::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;
	using namespace VoxelPaintVariantSelectionTestPrivate;

	UVoxelPaintVolumeData* AssetA = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	UVoxelPaintVolumeData* AssetB = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	UVoxelPaintVolumeData* AssetC = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	if (!TestNotNull(TEXT("Asset A created"), AssetA)
		|| !TestNotNull(TEXT("Asset B created"), AssetB)
		|| !TestNotNull(TEXT("Asset C created"), AssetC))
	{
		return false;
	}

	{
		FPoolBuilder Pool;
		Pool.Add(AssetA, 1.0f).Add(AssetB, 1.0f).Add(AssetC, 2.0f);
		const uint64 Seeds[] = { 0ull, 42ull, SeedStride, MAX_uint64 };
		for (const uint64 Seed : Seeds)
		{
			const int32 First = ResolveWeightedVariantSlot(Seed, Pool.View);
			TestEqual(TEXT("Same (seed, pool) resolves the same raw index on a second call"),
				ResolveWeightedVariantSlot(Seed, Pool.View), First);
			TestEqual(TEXT("Same (seed, pool) resolves the same raw index on a third call"),
				ResolveWeightedVariantSlot(Seed, Pool.View), First);
		}
	}

	{
		FPoolBuilder Pool;
		Pool.Add(AssetA, 1.0f).Add(AssetB, 1.0f).Add(AssetC, 2.0f);
		int32 Counts[3] = { 0, 0, 0 };
		constexpr int32 NumSamples = 10000;
		for (int32 SampleIndex = 0; SampleIndex < NumSamples; ++SampleIndex)
		{
			const uint64 Seed = uint64(SampleIndex) * SeedStride;
			const int32 Slot = ResolveWeightedVariantSlot(Seed, Pool.View);
			if (Slot < 0 || Slot > 2)
			{
				AddError(FString::Printf(TEXT("Roll returned out-of-range slot %d"), Slot));
				return false;
			}
			++Counts[Slot];
		}
		TestTrue(FString::Printf(TEXT("Weight-1 slot 0 lands within 10 percent of 2500 (got %d)"), Counts[0]),
			FMath::Abs(Counts[0] - 2500) <= 250);
		TestTrue(FString::Printf(TEXT("Weight-1 slot 1 lands within 10 percent of 2500 (got %d)"), Counts[1]),
			FMath::Abs(Counts[1] - 2500) <= 250);
		TestTrue(FString::Printf(TEXT("Weight-2 slot 2 lands within 10 percent of 5000 (got %d)"), Counts[2]),
			FMath::Abs(Counts[2] - 5000) <= 500);
	}

	{
		FPoolBuilder Pool;
		Pool.Add(AssetA, 0.0f).Add(AssetB, 1.0f);
		bool bZeroWeightRolled = false;
		for (int32 SampleIndex = 0; SampleIndex < 256; ++SampleIndex)
		{
			if (ResolveWeightedVariantSlot(uint64(SampleIndex) * SeedStride, Pool.View) != 1)
			{
				bZeroWeightRolled = true;
				break;
			}
		}
		TestFalse(TEXT("A zero-weight slot is never rolled"), bZeroWeightRolled);
	}

	{
		FPoolBuilder Pool;
		Pool.Add(AssetA, -5.0f).Add(AssetB, std::numeric_limits<float>::quiet_NaN()).Add(AssetC, 1.0f);
		bool bDegenerateWeightRolled = false;
		for (int32 SampleIndex = 0; SampleIndex < 256; ++SampleIndex)
		{
			if (ResolveWeightedVariantSlot(uint64(SampleIndex) * SeedStride, Pool.View) != 2)
			{
				bDegenerateWeightRolled = true;
				break;
			}
		}
		TestFalse(TEXT("Negative and NaN weights clamp to zero and are never rolled"), bDegenerateWeightRolled);
	}

	{
		FPoolBuilder Pool;
		Pool.Add(AssetA, 0.0f).Add(AssetB, 0.0f);
		TestEqual(TEXT("All-zero weights: seed 0 rolls the first valid slot under the uniform fallback"),
			ResolveWeightedVariantSlot(0ull, Pool.View), 0);
		TestEqual(TEXT("All-zero weights: the maximum seed rolls the last valid slot under the uniform fallback"),
			ResolveWeightedVariantSlot(MAX_uint64, Pool.View), 1);
		int32 FallbackCounts[2] = { 0, 0 };
		for (int32 SampleIndex = 0; SampleIndex < 1000; ++SampleIndex)
		{
			const int32 Slot = ResolveWeightedVariantSlot(uint64(SampleIndex) * SeedStride, Pool.View);
			if (Slot == 0 || Slot == 1)
			{
				++FallbackCounts[Slot];
			}
		}
		TestTrue(TEXT("Uniform fallback reaches both valid slots"),
			FallbackCounts[0] > 0 && FallbackCounts[1] > 0);
	}

	{
		const FVoxelPaintPoolView EmptyView;
		TestEqual(TEXT("An empty pool resolves INDEX_NONE"),
			ResolveWeightedVariantSlot(42ull, EmptyView), (int32)INDEX_NONE);

		FPoolBuilder AllNull;
		AllNull.Add(nullptr, 1.0f).Add(nullptr, 1.0f);
		TestEqual(TEXT("An all-null pool resolves INDEX_NONE"),
			ResolveWeightedVariantSlot(42ull, AllNull.View), (int32)INDEX_NONE);
	}

	{
		FPoolBuilder Pool;
		Pool.Add(nullptr, 1.0f).Add(AssetA, 1.0f).Add(AssetA, 100.0f).Add(AssetB, 1.0f);
		TArray<int32> ValidSlots;
		ComputeValidSlots(Pool.View, ValidSlots);
		TestEqual(TEXT("Valid-slot count skips nulls and later duplicates"), ValidSlots.Num(), 2);
		if (ValidSlots.Num() == 2)
		{
			TestEqual(TEXT("First valid slot is the first non-null entry"), ValidSlots[0], 1);
			TestEqual(TEXT("Second valid slot skips the duplicate"), ValidSlots[1], 3);
		}
		bool bDuplicateRolled = false;
		for (int32 SampleIndex = 0; SampleIndex < 512; ++SampleIndex)
		{
			const int32 Slot = ResolveWeightedVariantSlot(uint64(SampleIndex) * SeedStride, Pool.View);
			if (Slot != 1 && Slot != 3)
			{
				bDuplicateRolled = true;
				break;
			}
		}
		TestFalse(TEXT("A duplicate slot is never rolled (alias of the first occurrence)"), bDuplicateRolled);
	}

	{
		FPoolBuilder Pool;
		Pool.Add(AssetA, 1.0f).Add(AssetB, 1.0f).Add(AssetC, 2.0f);
		const int32 UpperEdgeSlot = ResolveWeightedVariantSlot(MAX_uint64, Pool.View);
		TestNotEqual(TEXT("Seed MAX_uint64 never resolves INDEX_NONE on a rollable pool"),
			UpperEdgeSlot, (int32)INDEX_NONE);
		TestEqual(TEXT("Seed MAX_uint64 resolves the last rollable slot"), UpperEdgeSlot, 2);

		FPoolBuilder ZeroTail;
		ZeroTail.Add(AssetA, 1.0f).Add(AssetB, 1.0f).Add(AssetC, 0.0f);
		TestEqual(TEXT("Seed MAX_uint64 resolves the last ROLLABLE slot, not a zero-weight tail slot"),
			ResolveWeightedVariantSlot(MAX_uint64, ZeroTail.View), 1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintVariants_SeedClassification,
	"Game.WorldVoxelPaint.Unit.Variants.SeedClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintVariants_SeedClassification::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	for (int32 Mask = 0; Mask < 16; ++Mask)
	{
		FVoxelPaintSeedClassifierInputs Inputs;
		Inputs.bActorGuidValid = (Mask & 1) != 0;
		Inputs.bInstanceGuidMatchesActorGuid = (Mask & 2) != 0;
		Inputs.bWasLoaded = (Mask & 4) != 0;
		Inputs.bEditorWorld = (Mask & 8) != 0;
		const bool bExpected = Inputs.bActorGuidValid && Inputs.bInstanceGuidMatchesActorGuid
			&& (Inputs.bWasLoaded || Inputs.bEditorWorld);
		TestEqual(FString::Printf(TEXT("Classifier truth-table row %d"), Mask),
			ClassifyAsIdentitySeeded(Inputs), bExpected);
	}

	{
		FVoxelPaintSeedClassifierInputs RuntimeSpawn;
		RuntimeSpawn.bActorGuidValid = true;
		RuntimeSpawn.bInstanceGuidMatchesActorGuid = true;
		RuntimeSpawn.bWasLoaded = false;
		RuntimeSpawn.bEditorWorld = false;
		TestFalse(TEXT("A valid-guid runtime spawn classifies tier 2 (guid validity alone never suffices)"),
			ClassifyAsIdentitySeeded(RuntimeSpawn));
	}

	{
		const FGuid GuidA(0x11111111, 0x22222222, 0x33333333, 0x44444444);
		const FGuid GuidB(0x11111111, 0x22222222, 0x33333333, 0x44444445);
		TestEqual(TEXT("Identity seed is stable for the same guid"),
			MakeIdentitySeed(GuidA), MakeIdentitySeed(GuidA));
		TestNotEqual(TEXT("Identity seeds differ for distinct guids"),
			MakeIdentitySeed(GuidA), MakeIdentitySeed(GuidB));
	}

	const FVector Translation(100.2, -40.7, 3.3);
	const FQuat IdentityRotation = FQuat::Identity;

	TestEqual(TEXT("Identical frames hash identically"),
		MakeTransformSeed(IdentityRotation, Translation),
		MakeTransformSeed(IdentityRotation, Translation));

	TestEqual(TEXT("A sub-quantum move (under 0.5 cm per axis) keeps the seed"),
		MakeTransformSeed(IdentityRotation, Translation),
		MakeTransformSeed(IdentityRotation, Translation + FVector(0.2, 0.1, -0.2)));

	TestNotEqual(TEXT("A cross-bucket move (1 cm) changes the seed"),
		MakeTransformSeed(IdentityRotation, Translation),
		MakeTransformSeed(IdentityRotation, Translation + FVector(1.0, 0.0, 0.0)));

	{
		const FQuat Rotation = FRotator(10.0, 20.0, 30.0).Quaternion();
		const FQuat Negated(-Rotation.X, -Rotation.Y, -Rotation.Z, -Rotation.W);
		TestEqual(TEXT("Q and -Q hash identically (integer-tuple sign canonicalization)"),
			MakeTransformSeed(Rotation, Translation), MakeTransformSeed(Negated, Translation));
	}

	{
		const FQuat YawPositiveNoise(0.0, 0.0, 1.0, 1e-16);
		const FQuat YawNegativeNoise(0.0, 0.0, 1.0, -1e-16);
		const FQuat YawNegatedPositiveNoise(0.0, 0.0, -1.0, 1e-16);
		const FQuat YawNegatedNegativeNoise(0.0, 0.0, -1.0, -1e-16);
		const uint64 Reference = MakeTransformSeed(YawPositiveNoise, Translation);
		TestEqual(TEXT("Yaw-180 with W float noise of the opposite sign hashes identically"),
			MakeTransformSeed(YawNegativeNoise, Translation), Reference);
		TestEqual(TEXT("Negated yaw-180 with positive W noise hashes identically"),
			MakeTransformSeed(YawNegatedPositiveNoise, Translation), Reference);
		TestEqual(TEXT("Negated yaw-180 with negative W noise hashes identically"),
			MakeTransformSeed(YawNegatedNegativeNoise, Translation), Reference);
	}

	{
		const FQuat Yaw90 = FRotator(0.0, 90.0, 0.0).Quaternion();
		TestNotEqual(TEXT("A rotation-only change alters the seed"),
			MakeTransformSeed(IdentityRotation, Translation), MakeTransformSeed(Yaw90, Translation));
	}

	return true;
}
