// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintVariantSelection.h"
#include "VoxelPaintVolume.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "VoxelPaintWorldSubsystem.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintVariantResolutionTestPrivate
{

struct FVoxelPaintTestWorld
{
	UWorld* World = nullptr;

	FVoxelPaintTestWorld()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelPaintVariantResolutionTest"));
		FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
		WorldContext.SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
	}

	~FVoxelPaintTestWorld()
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}
};

AVoxelPaintVolume* SpawnTestVolume(UWorld* World, const FVector& Location)
{
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World->SpawnActor<AVoxelPaintVolume>(Location, FRotator::ZeroRotator, SpawnParameters);
}

UVoxelPaintVolumeData* MakeAsset(const FVector& HalfExtent, const FIntVector& BrickCounts,
	const FVector& StorageOrigin)
{
	UVoxelPaintVolumeData* Asset = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	Asset->LocalHalfExtent = HalfExtent;
	Asset->BrickCounts = BrickCounts;
	Asset->StorageOrigin = StorageOrigin;
	return Asset;
}

void AddPoolEntry(UVoxelPaintVolumeComponent* Component, UVoxelPaintVolumeData* Asset, float Weight)
{
	FVoxelPaintVariantPoolEntry& Entry = Component->VolumeVariants.AddDefaulted_GetRef();
	Entry.Variant = Asset;
	Entry.Weight = Weight;
}

WorldVoxelPaint::FVoxelPaintPoolView MakeViewFromPool(const UVoxelPaintVolumeComponent* Component)
{
	WorldVoxelPaint::FVoxelPaintPoolView View;
	for (const FVoxelPaintVariantPoolEntry& Entry : Component->VolumeVariants)
	{
		View.Assets.Add(Entry.Variant.Get());
		View.Weights.Add(Entry.Weight);
	}
	return View;
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintVariants_PinResolution,
	"Game.WorldVoxelPaint.Unit.Variants.PinResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintVariants_PinResolution::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintVariantResolutionTestPrivate;

	VoxelPaintVariantResolutionTestPrivate::FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(World);
	if (!TestNotNull(TEXT("Subsystem exists in the test world"), Subsystem))
	{
		return false;
	}

	AVoxelPaintVolume* Volume = VoxelPaintVariantResolutionTestPrivate::SpawnTestVolume(World, FVector(120.0, 40.0, -10.0));
	UVoxelPaintVolumeComponent* Component = Volume ? Volume->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("Volume component spawned"), Component))
	{
		return false;
	}

	UVoxelPaintVolumeData* AssetA = MakeAsset(FVector(500.0), FIntVector(4, 4, 4), FVector::ZeroVector);
	UVoxelPaintVolumeData* AssetB = MakeAsset(FVector(600.0), FIntVector(5, 5, 5), FVector::ZeroVector);
	UVoxelPaintVolumeData* OutsideAsset = MakeAsset(FVector(700.0), FIntVector(6, 6, 6), FVector::ZeroVector);

	AddPoolEntry(Component, AssetA, 1.0f);
	AddPoolEntry(Component, AssetB, 0.0f);
	Subsystem->NotifyPropertyChanged(Component);

	TestTrue(TEXT("Zero-weight sibling: the roll resolves the only rollable slot"),
		Subsystem->GetResolvedVariant(Component) == AssetA);
	TestEqual(TEXT("Baseline resolved index is the rollable slot"),
		Component->GetResolvedVariantIndex(), 0);

	Component->VariantOverride = AssetB;
	Subsystem->NotifyPropertyChanged(Component);
	TestTrue(TEXT("Pin resolves the pinned asset regardless of weights (zero-weight slot pinnable)"),
		Subsystem->GetResolvedVariant(Component) == AssetB);
	TestEqual(TEXT("Pin resolves the pinned asset's slot index"),
		Component->GetResolvedVariantIndex(), 1);

	AddPoolEntry(Component, AssetA, 100.0f);
	Component->VariantOverride = AssetA;
	Subsystem->NotifyPropertyChanged(Component);
	TestTrue(TEXT("Pin to an asset with a later duplicate slot resolves the asset"),
		Subsystem->GetResolvedVariant(Component) == AssetA);
	TestEqual(TEXT("Pin to a duplicated asset reports the FIRST occurrence's slot"),
		Component->GetResolvedVariantIndex(), 0);

	Component->VariantOverride = OutsideAsset;
	Subsystem->NotifyPropertyChanged(Component);
	TestTrue(TEXT("Dangling pin falls back to the deterministic roll"),
		Subsystem->GetResolvedVariant(Component) == AssetA);
	TestEqual(TEXT("Dangling pin: GetResolvedVariantIndex reports the rolled slot"),
		Component->GetResolvedVariantIndex(), 0);
	TestTrue(TEXT("Dangling pin is preserved, never clamped or shifted"),
		Component->VariantOverride == OutsideAsset);

	Component->VariantOverride = nullptr;
	Subsystem->NotifyPropertyChanged(Component);
	TestTrue(TEXT("Clearing the pin restores the roll"),
		Subsystem->GetResolvedVariant(Component) == AssetA);
	TestEqual(TEXT("Cleared pin restores the rolled slot index"),
		Component->GetResolvedVariantIndex(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintVariants_ForcePreviewOverride,
	"Game.WorldVoxelPaint.Unit.Variants.ForcePreviewOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintVariants_ForcePreviewOverride::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintVariantResolutionTestPrivate;

	VoxelPaintVariantResolutionTestPrivate::FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(World);
	if (!TestNotNull(TEXT("Subsystem exists in the test world"), Subsystem))
	{
		return false;
	}

	AVoxelPaintVolume* Volume = VoxelPaintVariantResolutionTestPrivate::SpawnTestVolume(World, FVector(75.0, -20.0, 5.0));
	UVoxelPaintVolumeComponent* Component = Volume ? Volume->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("Volume component spawned"), Component))
	{
		return false;
	}

	UVoxelPaintVolumeData* AssetA = MakeAsset(FVector(500.0), FIntVector(4, 4, 4), FVector::ZeroVector);
	UVoxelPaintVolumeData* AssetB = MakeAsset(FVector(600.0), FIntVector(5, 5, 5), FVector::ZeroVector);
	AddPoolEntry(Component, AssetA, 1.0f);
	AddPoolEntry(Component, AssetB, 1.0f);
	Subsystem->NotifyPropertyChanged(Component);

	FVoxelPaintPlacementState State;
	if (!TestTrue(TEXT("Placement state cached for the pooled component"),
		Subsystem->GetCachedPlacementState(Component, State)))
	{
		return false;
	}
	const FVoxelPaintFrontierIdentity Identity = State.Identity;

	Component->VariantOverride = AssetA;
	Subsystem->NotifyPropertyChanged(Component);
	TestTrue(TEXT("Pin resolves ahead of the roll"),
		Subsystem->GetResolvedVariant(Component) == AssetA);

	Subsystem->SetForcedVariant(Identity, AssetB);
	TestTrue(TEXT("Force-preview resolves ahead of pin AND roll"),
		Subsystem->GetResolvedVariant(Component) == AssetB);
	FObjectKey ForcedKey;
	TestTrue(TEXT("GetForcedVariant reports the stored override"),
		Subsystem->GetForcedVariant(Identity, ForcedKey));
	TestTrue(TEXT("Stored override key matches the forced asset"),
		ForcedKey == FObjectKey(AssetB));

	Subsystem->NotifyPropertyChanged(Component);
	TestTrue(TEXT("Override keyed by frontier identity survives a NotifyPropertyChanged cache rebuild"),
		Subsystem->GetResolvedVariant(Component) == AssetB);

	Subsystem->ClearForcedVariant(Identity);
	TestTrue(TEXT("Clear re-resolves to the pin"),
		Subsystem->GetResolvedVariant(Component) == AssetA);

	Subsystem->SetForcedVariant(Identity, AssetB);
	Subsystem->ClearAllForcedVariants();
	TestTrue(TEXT("ClearAll re-resolves to the pin"),
		Subsystem->GetResolvedVariant(Component) == AssetA);

	Component->VariantOverride = nullptr;
	Subsystem->NotifyPropertyChanged(Component);
	Subsystem->SetForcedVariant(Identity, AssetB);
	TestTrue(TEXT("Force-preview applies over the plain roll"),
		Subsystem->GetResolvedVariant(Component) == AssetB);
	Component->VolumeVariants.RemoveAt(1);
	Subsystem->NotifyPropertyChanged(Component);
	TestTrue(TEXT("Resolution drops an override whose asset left the pool"),
		Subsystem->GetResolvedVariant(Component) == AssetA);
	Subsystem->ClearAllForcedVariants();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintRegistry_ResolvedVariantCutover,
	"Game.WorldVoxelPaint.Unit.Registry.ResolvedVariantCutover",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintRegistry_ResolvedVariantCutover::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintVariantResolutionTestPrivate;

	VoxelPaintVariantResolutionTestPrivate::FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(World);
	if (!TestNotNull(TEXT("Subsystem exists in the test world"), Subsystem))
	{
		return false;
	}

	const FVector SpawnLocation(100.2, 3.7, -25.9);
	AVoxelPaintVolume* Volume = VoxelPaintVariantResolutionTestPrivate::SpawnTestVolume(World, SpawnLocation);
	UVoxelPaintVolumeComponent* Component = Volume ? Volume->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("Volume component spawned"), Component))
	{
		return false;
	}

	UVoxelPaintVolumeData* AssetA = MakeAsset(FVector(600.0, 500.0, 400.0), FIntVector(5, 4, 3),
		FVector(25.0, -10.0, 5.0));
	UVoxelPaintVolumeData* AssetB = MakeAsset(FVector(300.0, 300.0, 300.0), FIntVector(4, 4, 4),
		FVector::ZeroVector);

	Component->BlendModeG = EVoxelPaintBlendMode::Subtract;
	AddPoolEntry(Component, AssetA, 1.0f);
	Subsystem->NotifyPropertyChanged(Component);

	{
		FVoxelPaintPlacementState State;
		if (!TestTrue(TEXT("Placement state cached"), Subsystem->GetCachedPlacementState(Component, State)))
		{
			return false;
		}
		TestTrue(TEXT("Rotation matches the rigid placement frame"),
			State.Rotation == FQuat::Identity);
		TestTrue(TEXT("Translation matches the rigid placement frame"),
			State.Translation == SpawnLocation);
		TestTrue(TEXT("LocalHalfExtent matches the resolved asset"),
			State.LocalHalfExtent == FVector3f(AssetA->LocalHalfExtent));
		TestTrue(TEXT("TexelSize matches the resolved asset"),
			State.TexelSize == FVector3f(AssetA->GetTexelSize()));
		TestTrue(TEXT("StorageMin matches the resolved asset"),
			State.StorageMin == FVector3f(AssetA->GetStorageMin()));
		TestTrue(TEXT("BrickCounts match the resolved asset"),
			State.BrickCounts == AssetA->GetBrickCounts());
		TestTrue(TEXT("BlendCoefficients match the component blend modes"),
			State.BlendCoefficients == FVector4f(1.0f, -1.0f, 1.0f, 1.0f));
		TestEqual(TEXT("Flags carry the valid bit"), State.Flags, WorldVoxelPaint::RecordFlag_Valid);
		TestTrue(TEXT("AssetKey is the resolved variant's key"),
			State.AssetKey == FObjectKey(AssetA));
		TestEqual(TEXT("SourceGeneration matches the resolved asset"),
			State.SourceGeneration, AssetA->GetSourceGeneration());
		TestTrue(TEXT("Identity is the frontier identity (actor instance guid)"),
			State.Identity == Volume->GetActorInstanceGuid());
	}

	{
		const uint64 GenerationBefore = Subsystem->GetLayoutGeneration();
		Component->VolumeVariants[0].Variant = AssetB;
		Subsystem->NotifyPropertyChanged(Component);
		FVoxelPaintPlacementState State;
		Subsystem->GetCachedPlacementState(Component, State);
		TestTrue(TEXT("Pool edit flips the cached AssetKey"), State.AssetKey == FObjectKey(AssetB));
		TestTrue(TEXT("Pool edit bumps the layout generation"),
			Subsystem->GetLayoutGeneration() > GenerationBefore);
	}

	{
		Component->VolumeVariants.Empty();
		Subsystem->NotifyPropertyChanged(Component);
		FVoxelPaintPlacementState State;
		Subsystem->GetCachedPlacementState(Component, State);
		TestFalse(TEXT("Empty pool: HasValidVolumeData is false"), Component->HasValidVolumeData());
		TestEqual(TEXT("Empty pool: cached flags drop the valid bit"), State.Flags, 0u);

		AddPoolEntry(Component, nullptr, 1.0f);
		Subsystem->NotifyPropertyChanged(Component);
		Subsystem->GetCachedPlacementState(Component, State);
		TestFalse(TEXT("All-null pool: HasValidVolumeData is false"), Component->HasValidVolumeData());
		TestEqual(TEXT("All-null pool: cached flags drop the valid bit"), State.Flags, 0u);

		Component->VolumeVariants.Empty();
		AddPoolEntry(Component, AssetA, 1.0f);
		AddPoolEntry(Component, AssetA, 1.0f);
		Subsystem->NotifyPropertyChanged(Component);
		Subsystem->GetCachedPlacementState(Component, State);
		TestTrue(TEXT("Duplicate-after-first pool still resolves the first occurrence"),
			Component->HasValidVolumeData());
		TestTrue(TEXT("Duplicate-after-first pool resolves the aliased asset"),
			Subsystem->GetResolvedVariant(Component) == AssetA);
		TestEqual(TEXT("Duplicate-after-first pool keeps the valid bit"),
			State.Flags, WorldVoxelPaint::RecordFlag_Valid);
	}

	{
		Component->VolumeVariants.Empty();
		AddPoolEntry(Component, AssetA, 1.0f);
		AddPoolEntry(Component, AssetB, 1.0f);
		Subsystem->NotifyPropertyChanged(Component);
		TestTrue(TEXT("Two-valid-slot pool: HasValidVolumeData is true"), Component->HasValidVolumeData());

		FVoxelPaintPlacementState State;
		Subsystem->GetCachedPlacementState(Component, State);
		const FObjectKey KeyBeforeMove = State.AssetKey;
		const int32 SlotBeforeMove = Component->GetResolvedVariantIndex();

		Component->SetMobility(EComponentMobility::Movable);
		const uint64 GenerationBeforeSubQuantum = Subsystem->GetLayoutGeneration();
		Volume->SetActorLocation(SpawnLocation + FVector(0.2, 0.1, -0.2));
		Subsystem->NotifyTransformChanged(Component);
		Subsystem->GetCachedPlacementState(Component, State);
		TestTrue(TEXT("Sub-quantum move keeps the resolved AssetKey"), State.AssetKey == KeyBeforeMove);
		TestTrue(TEXT("Sub-quantum move still ships the transform refresh"),
			Subsystem->GetLayoutGeneration() > GenerationBeforeSubQuantum);

		const WorldVoxelPaint::FVoxelPaintPoolView View = MakeViewFromPool(Component);
		FVector FlipLocation = FVector::ZeroVector;
		int32 FlipSlot = INDEX_NONE;
		for (int32 Step = 1; Step <= 64; ++Step)
		{
			const FVector Candidate = SpawnLocation + FVector(10.0 * Step, 0.0, 0.0);
			const uint64 CandidateSeed =
				WorldVoxelPaint::MakeTransformSeed(FQuat::Identity, Candidate);
			const int32 CandidateSlot = WorldVoxelPaint::ResolveWeightedVariantSlot(CandidateSeed, View);
			if (CandidateSlot != SlotBeforeMove)
			{
				FlipLocation = Candidate;
				FlipSlot = CandidateSlot;
				break;
			}
		}
		if (!TestNotEqual(TEXT("A cross-bucket location that flips the roll exists within the search range"),
			FlipSlot, (int32)INDEX_NONE))
		{
			return false;
		}

		const uint64 GenerationBeforeFlip = Subsystem->GetLayoutGeneration();
		Volume->SetActorLocation(FlipLocation);
		Subsystem->NotifyTransformChanged(Component);
		Subsystem->GetCachedPlacementState(Component, State);
		TestTrue(TEXT("Cross-bucket move re-resolves to the flipped slot's asset"),
			State.AssetKey == FObjectKey(Component->VolumeVariants[FlipSlot].Variant.Get()));
		TestFalse(TEXT("Cross-bucket move changed the AssetKey"), State.AssetKey == KeyBeforeMove);
		TestTrue(TEXT("Cross-bucket move bumps the layout generation"),
			Subsystem->GetLayoutGeneration() > GenerationBeforeFlip);
	}

	return true;
}
