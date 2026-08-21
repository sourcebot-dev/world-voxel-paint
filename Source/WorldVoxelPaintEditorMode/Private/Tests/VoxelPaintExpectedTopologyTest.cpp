// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Selection.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "VoxelPaintAuthority.h"
#include "VoxelPaintDetailLadder.h"
#include "VoxelPaintTemplateRouting.h"
#include "VoxelPaintTopologyOps.h"
#include "VoxelPaintVolume.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "VoxelPaintWorldSubsystem.h"

namespace VoxelPaintExpectedTopologyTestPrivate
{

struct FVoxelPaintTestWorld
{
	UWorld* World = nullptr;

	FVoxelPaintTestWorld()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelPaintExpectedTopologyTest"));
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

FProperty* GetComponentPoolProperty()
{
	return FindFieldChecked<FProperty>(UVoxelPaintVolumeComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VolumeVariants));
}

FProperty* GetComponentExpectationProperty()
{
	return FindFieldChecked<FProperty>(UVoxelPaintVolumeComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, ExpectedTopology));
}

void AddPoolEntryWithSynthesizedEvent(UVoxelPaintVolumeComponent* Component, UVoxelPaintVolumeData* Asset)
{
	FProperty* PoolProperty = GetComponentPoolProperty();
	Component->PreEditChange(PoolProperty);
	FVoxelPaintVariantPoolEntry& Entry = Component->VolumeVariants.AddDefaulted_GetRef();
	Entry.Variant = Asset;
	FPropertyChangedEvent ChangedEvent(PoolProperty);
	Component->PostEditChangeProperty(ChangedEvent);
}

void EmptyPoolWithSynthesizedEvent(UVoxelPaintVolumeComponent* Component)
{
	FProperty* PoolProperty = GetComponentPoolProperty();
	Component->PreEditChange(PoolProperty);
	Component->VolumeVariants.Empty();
	FPropertyChangedEvent ChangedEvent(PoolProperty);
	Component->PostEditChangeProperty(ChangedEvent);
}

void EditAssetExtentThroughDetailsPath(UVoxelPaintVolumeData* Asset, const FVector& NewExtent)
{
	FProperty* ExtentProperty = FindFieldChecked<FProperty>(UVoxelPaintVolumeData::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeData, LocalHalfExtent));
	Asset->PreEditChange(ExtentProperty);
	Asset->LocalHalfExtent = NewExtent;
	FPropertyChangedEvent ChangedEvent(ExtentProperty);
	Asset->PostEditChangeProperty(ChangedEvent);
}

bool ExpectationEquals(const FVoxelPaintExpectedTopology& Expectation, const FVector& HalfExtent,
	const FIntVector& BrickCounts, const FVector& StorageOrigin, bool bIsSet)
{
	return Expectation.LocalHalfExtent == HalfExtent
		&& Expectation.BrickCounts == BrickCounts
		&& Expectation.StorageOrigin == StorageOrigin
		&& Expectation.bIsSet == bIsSet;
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintVariants_TopologyExpectation,
	"Game.WorldVoxelPaint.Unit.Variants.TopologyExpectation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintVariants_TopologyExpectation::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintExpectedTopologyTestPrivate;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;

	AVoxelPaintVolume* Volume = SpawnTestVolume(World, FVector(10.0, 20.0, 30.0));
	UVoxelPaintVolumeComponent* Component = Volume ? Volume->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("Volume component spawned"), Component))
	{
		return false;
	}

	const FVector ExtentA(600.0, 500.0, 400.0);
	const FIntVector BricksA(5, 4, 3);
	const FVector OriginA(10.0, 20.0, 30.0);
	UVoxelPaintVolumeData* AssetA = MakeAsset(ExtentA, BricksA, OriginA);

	const FVector ExtentB(320.0, 320.0, 320.0);
	const FIntVector BricksB(6, 6, 6);
	UVoxelPaintVolumeData* AssetB = MakeAsset(ExtentB, BricksB, FVector::ZeroVector);

	TestFalse(TEXT("A fresh component's expectation is unset"), Component->ExpectedTopology.bIsSet);

	AddPoolEntryWithSynthesizedEvent(Component, AssetA);
	TestTrue(TEXT("First valid entry adopts its triplet and sets bIsSet"),
		ExpectationEquals(Component->ExpectedTopology, ExtentA, BricksA, OriginA, true));
	TestTrue(TEXT("Adopted expectation matches the first entry"),
		Component->ExpectedTopology.Matches(*AssetA));

	AddPoolEntryWithSynthesizedEvent(Component, AssetB);
	TestTrue(TEXT("A second differing entry does NOT re-adopt"),
		ExpectationEquals(Component->ExpectedTopology, ExtentA, BricksA, OriginA, true));
	TestFalse(TEXT("The differing entry stands as a mismatch"),
		Component->ExpectedTopology.Matches(*AssetB));

	EmptyPoolWithSynthesizedEvent(Component);
	TestTrue(TEXT("An emptied pool keeps its expectation"),
		ExpectationEquals(Component->ExpectedTopology, ExtentA, BricksA, OriginA, true));

	AddPoolEntryWithSynthesizedEvent(Component, AssetB);
	TestTrue(TEXT("Re-population with foreign topology never re-adopts"),
		ExpectationEquals(Component->ExpectedTopology, ExtentA, BricksA, OriginA, true));
	TestFalse(TEXT("Re-populated foreign topology is a standing mismatch"),
		Component->ExpectedTopology.Matches(*AssetB));

	{
		AVoxelPaintVolume* AuthorVolume = SpawnTestVolume(World, FVector(200.0, 0.0, 0.0));
		UVoxelPaintVolumeComponent* AuthorComponent =
			AuthorVolume ? AuthorVolume->GetVolumeComponent() : nullptr;
		if (!TestNotNull(TEXT("Author-edit volume component spawned"), AuthorComponent))
		{
			return false;
		}
		TestFalse(TEXT("Author-edit component starts unset"), AuthorComponent->ExpectedTopology.bIsSet);
		FProperty* ExpectationProperty = GetComponentExpectationProperty();
		AuthorComponent->PreEditChange(ExpectationProperty);
		AuthorComponent->ExpectedTopology.LocalHalfExtent = FVector(700.0);
		FPropertyChangedEvent ChangedEvent(ExpectationProperty);
		AuthorComponent->PostEditChangeProperty(ChangedEvent);
		TestTrue(TEXT("Any author edit under the ExpectedTopology chain sets bIsSet"),
			AuthorComponent->ExpectedTopology.bIsSet);
	}

	{
		FVoxelPaintExpectedTopology Expectation;
		Expectation.LocalHalfExtent = ExtentA;
		Expectation.BrickCounts = BricksA;
		Expectation.StorageOrigin = OriginA;
		Expectation.bIsSet = true;

		UVoxelPaintVolumeData* Exact = MakeAsset(ExtentA, BricksA, OriginA);
		TestTrue(TEXT("Matches accepts an exactly equal triplet"), Expectation.Matches(*Exact));

		UVoxelPaintVolumeData* ExtentOff = MakeAsset(ExtentA + FVector(0.0001, 0.0, 0.0), BricksA, OriginA);
		TestFalse(TEXT("Matches rejects an epsilon extent difference (exact equality)"),
			Expectation.Matches(*ExtentOff));

		UVoxelPaintVolumeData* BricksOff = MakeAsset(ExtentA, BricksA + FIntVector(1, 0, 0), OriginA);
		TestFalse(TEXT("Matches rejects a brick-count difference"), Expectation.Matches(*BricksOff));

		UVoxelPaintVolumeData* OriginOff = MakeAsset(ExtentA, BricksA, OriginA + FVector(0.0, 0.5, 0.0));
		TestFalse(TEXT("Matches rejects a storage-origin difference"), Expectation.Matches(*OriginOff));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintVariants_TopologySyncPaths,
	"Game.WorldVoxelPaint.Unit.Variants.TopologySyncPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintVariants_TopologySyncPaths::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintExpectedTopologyTestPrivate;
	using namespace WorldVoxelPaint;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(World);
	if (!TestNotNull(TEXT("Subsystem exists in the test world"), Subsystem))
	{
		return false;
	}

	{
		AVoxelPaintVolume* Volume = SpawnTestVolume(World, FVector::ZeroVector);
		UVoxelPaintVolumeComponent* Component = Volume ? Volume->GetVolumeComponent() : nullptr;
		if (!TestNotNull(TEXT("Commit-leg volume component spawned"), Component))
		{
			return false;
		}
		UVoxelPaintVolumeData* Asset = MakeAsset(FVector(500.0), FIntVector(4, 4, 4), FVector::ZeroVector);
		MarkShared(Asset);
		Component->VolumeVariants.AddDefaulted_GetRef().Variant = Asset;
		Subsystem->NotifyPropertyChanged(Component);

		FText RouteError;
		UVoxelPaintVolumeComponent* Target = ResolveExpectationWriteTarget(Component, RouteError);
		TestTrue(TEXT("Native placement routes the expectation write to the component itself"),
			Target == Component);

		FVoxelPaintLadderState NewState;
		NewState.HalfExtent = FVector(640.0, 480.0, 320.0);
		NewState.BrickCounts = FIntVector(5, 4, 3);
		FText CommitError;
		const bool bCommitted = CommitPreInitTopologyEdit(Asset, Volume, Target, NewState, CommitError);
		TestTrue(FString::Printf(TEXT("CommitPreInitTopologyEdit succeeds (%s)"),
			*CommitError.ToString()), bCommitted);
		TestTrue(TEXT("Asset half extent written"), Asset->LocalHalfExtent == NewState.HalfExtent);
		TestTrue(TEXT("Asset brick counts written"), Asset->BrickCounts == NewState.BrickCounts);
		TestTrue(TEXT("Expectation target written atomically with the asset"),
			ExpectationEquals(Component->ExpectedTopology, NewState.HalfExtent, NewState.BrickCounts,
				Asset->StorageOrigin, true));
		TestTrue(TEXT("Expectation matches the resized asset"),
			Component->ExpectedTopology.Matches(*Asset));
	}

	{
		AActor* PlainOwner = World->SpawnActor<AActor>();
		if (!TestNotNull(TEXT("Plain owner actor spawned"), PlainOwner))
		{
			return false;
		}
		UVoxelPaintVolumeData* UntouchedAsset =
			MakeAsset(FVector(500.0), FIntVector(4, 4, 4), FVector::ZeroVector);
		const FVector ExtentBefore = UntouchedAsset->LocalHalfExtent;
		const FIntVector BricksBefore = UntouchedAsset->BrickCounts;

		UVoxelPaintVolumeComponent* UcsComponent =
			NewObject<UVoxelPaintVolumeComponent>(PlainOwner, TEXT("VPUcsComponent"));
		UcsComponent->CreationMethod = EComponentCreationMethod::UserConstructionScript;
		FText UcsError;
		UVoxelPaintVolumeComponent* UcsTarget = ResolveExpectationWriteTarget(UcsComponent, UcsError);
		TestNull(TEXT("UCS provenance refuses the expectation write target"), UcsTarget);
		TestFalse(TEXT("UCS refusal carries an error"), UcsError.IsEmpty());

		UVoxelPaintVolumeComponent* AncestorComponent =
			NewObject<UVoxelPaintVolumeComponent>(PlainOwner, TEXT("VPAncestorComponent"));
		AncestorComponent->CreationMethod = EComponentCreationMethod::SimpleConstructionScript;
		FText AncestorError;
		UVoxelPaintVolumeComponent* AncestorTarget =
			ResolveExpectationWriteTarget(AncestorComponent, AncestorError);
		TestNull(TEXT("SCSAncestor provenance refuses the expectation write target"), AncestorTarget);
		TestFalse(TEXT("SCSAncestor refusal carries an error"), AncestorError.IsEmpty());

		TestTrue(TEXT("Refusal fires before any asset mutation (extent unchanged)"),
			UntouchedAsset->LocalHalfExtent == ExtentBefore);
		TestTrue(TEXT("Refusal fires before any asset mutation (brick counts unchanged)"),
			UntouchedAsset->BrickCounts == BricksBefore);
	}

	const FVector OldExtent(500.0);
	const FIntVector OldBricks(4, 4, 4);
	const FVector OldOrigin = FVector::ZeroVector;
	const FVector NewExtent(650.0, 500.0, 500.0);

	{
		UVoxelPaintVolumeData* Asset = MakeAsset(OldExtent, OldBricks, OldOrigin);
		AVoxelPaintVolume* Volume = SpawnTestVolume(World, FVector(300.0, 0.0, 0.0));
		UVoxelPaintVolumeComponent* Component = Volume->GetVolumeComponent();
		Component->VolumeVariants.AddDefaulted_GetRef().Variant = Asset;
		Component->ExpectedTopology.LocalHalfExtent = OldExtent;
		Component->ExpectedTopology.BrickCounts = OldBricks;
		Component->ExpectedTopology.StorageOrigin = OldOrigin;
		Component->ExpectedTopology.bIsSet = true;
		Subsystem->NotifyPropertyChanged(Component);

		EditAssetExtentThroughDetailsPath(Asset, NewExtent);
		TestTrue(TEXT("Placement-owned sole-consumer single-entry pool syncs the expectation"),
			ExpectationEquals(Component->ExpectedTopology, NewExtent, OldBricks, OldOrigin, true));
		TestTrue(TEXT("Synced expectation matches the resized asset"),
			Component->ExpectedTopology.Matches(*Asset));
	}

	{
		UVoxelPaintVolumeData* Asset = MakeAsset(OldExtent, OldBricks, OldOrigin);
		AActor* ScsOwner = World->SpawnActor<AActor>();
		UVoxelPaintVolumeComponent* ScsComponent =
			NewObject<UVoxelPaintVolumeComponent>(ScsOwner, TEXT("VPScsSyncComponent"));
		ScsComponent->CreationMethod = EComponentCreationMethod::SimpleConstructionScript;
		ScsComponent->VolumeVariants.AddDefaulted_GetRef().Variant = Asset;
		ScsComponent->ExpectedTopology.LocalHalfExtent = OldExtent;
		ScsComponent->ExpectedTopology.BrickCounts = OldBricks;
		ScsComponent->ExpectedTopology.StorageOrigin = OldOrigin;
		ScsComponent->ExpectedTopology.bIsSet = true;

		EditAssetExtentThroughDetailsPath(Asset, NewExtent);
		TestTrue(TEXT("An SCS-created instance NEVER syncs from the asset fan-out"),
			ExpectationEquals(ScsComponent->ExpectedTopology, OldExtent, OldBricks, OldOrigin, true));
		TestFalse(TEXT("The SCS instance keeps its standing mismatch"),
			ScsComponent->ExpectedTopology.Matches(*Asset));
	}

	{
		UVoxelPaintVolumeData* Asset = MakeAsset(OldExtent, OldBricks, OldOrigin);
		AVoxelPaintVolume* VolumeOne = SpawnTestVolume(World, FVector(400.0, 0.0, 0.0));
		AVoxelPaintVolume* VolumeTwo = SpawnTestVolume(World, FVector(500.0, 0.0, 0.0));
		UVoxelPaintVolumeComponent* ComponentOne = VolumeOne->GetVolumeComponent();
		UVoxelPaintVolumeComponent* ComponentTwo = VolumeTwo->GetVolumeComponent();
		UVoxelPaintVolumeComponent* BothComponents[2] = { ComponentOne, ComponentTwo };
		for (UVoxelPaintVolumeComponent* Component : BothComponents)
		{
			Component->VolumeVariants.AddDefaulted_GetRef().Variant = Asset;
			Component->ExpectedTopology.LocalHalfExtent = OldExtent;
			Component->ExpectedTopology.BrickCounts = OldBricks;
			Component->ExpectedTopology.StorageOrigin = OldOrigin;
			Component->ExpectedTopology.bIsSet = true;
			Subsystem->NotifyPropertyChanged(Component);
		}

		EditAssetExtentThroughDetailsPath(Asset, NewExtent);
		TestTrue(TEXT("A second loaded consumer suppresses the sync for the first pool"),
			ExpectationEquals(ComponentOne->ExpectedTopology, OldExtent, OldBricks, OldOrigin, true));
		TestTrue(TEXT("A second loaded consumer suppresses the sync for the second pool"),
			ExpectationEquals(ComponentTwo->ExpectedTopology, OldExtent, OldBricks, OldOrigin, true));
		TestFalse(TEXT("The first pool keeps its mismatch"),
			ComponentOne->ExpectedTopology.Matches(*Asset));
		TestFalse(TEXT("The second pool keeps its mismatch"),
			ComponentTwo->ExpectedTopology.Matches(*Asset));
	}

	{
		UVoxelPaintVolumeData* Asset = MakeAsset(OldExtent, OldBricks, OldOrigin);
		UVoxelPaintVolumeData* OtherEntry =
			MakeAsset(FVector(200.0), FIntVector(3, 3, 3), FVector::ZeroVector);
		AVoxelPaintVolume* Volume = SpawnTestVolume(World, FVector(600.0, 0.0, 0.0));
		UVoxelPaintVolumeComponent* Component = Volume->GetVolumeComponent();
		Component->VolumeVariants.AddDefaulted_GetRef().Variant = Asset;
		Component->VolumeVariants.AddDefaulted_GetRef().Variant = OtherEntry;
		Component->ExpectedTopology.LocalHalfExtent = OldExtent;
		Component->ExpectedTopology.BrickCounts = OldBricks;
		Component->ExpectedTopology.StorageOrigin = OldOrigin;
		Component->ExpectedTopology.bIsSet = true;
		Subsystem->NotifyPropertyChanged(Component);

		EditAssetExtentThroughDetailsPath(Asset, NewExtent);
		TestTrue(TEXT("A multi-distinct-entry pool does not sync"),
			ExpectationEquals(Component->ExpectedTopology, OldExtent, OldBricks, OldOrigin, true));
	}

	{
		if (!TestNotNull(TEXT("GEditor is available for the fixture leg"), GEditor))
		{
			return false;
		}
		AVoxelPaintVolume* FixtureVolume = SpawnTestVolume(World, FVector(700.0, 0.0, 0.0));
		UVoxelPaintVolumeComponent* FixtureComponent = FixtureVolume->GetVolumeComponent();

		USelection* SelectedActors = GEditor->GetSelectedActors();
		SelectedActors->DeselectAll();
		SelectedActors->Select(FixtureVolume);
		GEngine->Exec(World, TEXT("wvp.Fixture.Seed Gradient"));
		SelectedActors->DeselectAll();

		if (!TestTrue(TEXT("Fixture seed populated pool slot 0"),
			FixtureComponent->VolumeVariants.Num() > 0
				&& FixtureComponent->VolumeVariants[0].Variant != nullptr))
		{
			return false;
		}
		UVoxelPaintVolumeData* FixtureAsset = FixtureComponent->VolumeVariants[0].Variant;
		TestTrue(TEXT("Fixture asset committed an initialized canonical source"),
			FixtureAsset->bInitialized);
		TestTrue(TEXT("Fixture seed synced the expectation in the same step"),
			FixtureComponent->ExpectedTopology.bIsSet
				&& FixtureComponent->ExpectedTopology.Matches(*FixtureAsset));
	}

	return true;
}
