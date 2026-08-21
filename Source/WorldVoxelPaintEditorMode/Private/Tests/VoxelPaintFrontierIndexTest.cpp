// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintVolume.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintWorldSubsystem.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintFrontierIndexTestPrivate
{

struct FVoxelPaintTestWorld
{
	UWorld* World = nullptr;

	FVoxelPaintTestWorld()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelPaintFrontierIndexTest"));
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

// Cross-checks the cached O(1) reverse lookup against the independent single-pass enumeration:
// strictly ascending slot indices, member-count parity, and per member the cached lookup and
// cached placement state agree with what the enumeration walked out of the slot array.
bool VerifyFrontierConsistency(FAutomationTestBase& Test, const UVoxelPaintWorldSubsystem& Subsystem)
{
	struct FEnumeratedSlot
	{
		int32 SlotIndex = INDEX_NONE;
		const UVoxelPaintVolumeComponent* Component = nullptr;
		FVector Translation = FVector::ZeroVector;
	};
	TArray<FEnumeratedSlot> Slots;
	Subsystem.EnumerateFrontierSlots([&Slots](int32 SlotIndex,
		const UVoxelPaintVolumeComponent& Component, const FVoxelPaintPlacementState& State)
	{
		Slots.Add({ SlotIndex, &Component, State.Translation });
	});

	bool bConsistent = Test.TestEqual(TEXT("Enumeration count matches GetNumFrontierMembers"),
		Slots.Num(), Subsystem.GetNumFrontierMembers());
	int32 PreviousIndex = INDEX_NONE;
	for (const FEnumeratedSlot& Slot : Slots)
	{
		bConsistent &= Test.TestTrue(TEXT("Enumerated slot indices strictly ascend"),
			Slot.SlotIndex > PreviousIndex);
		PreviousIndex = Slot.SlotIndex;
		bConsistent &= Test.TestEqual(TEXT("Cached reverse lookup agrees with the enumerated slot"),
			Subsystem.GetFrontierPosition(Slot.Component), Slot.SlotIndex);
		FVoxelPaintPlacementState State;
		if (Test.TestTrue(TEXT("Enumerated member has a cached placement state"),
			Subsystem.GetCachedPlacementState(Slot.Component, State)))
		{
			bConsistent &= Test.TestEqual(TEXT("Cached state translation matches the enumerated state"),
				State.Translation, Slot.Translation);
		}
		else
		{
			bConsistent = false;
		}
	}
	return bConsistent;
}

} // namespace VoxelPaintFrontierIndexTestPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintFrontier_ReverseIndexConsistency,
	"Game.WorldVoxelPaint.Unit.Registry.ReverseIndexConsistency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintFrontier_ReverseIndexConsistency::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintFrontierIndexTestPrivate;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(World);
	if (!TestNotNull(TEXT("Subsystem exists in the test world"), Subsystem))
	{
		return false;
	}

	constexpr int32 NumVolumes = 8;
	TArray<AVoxelPaintVolume*> Volumes;
	Volumes.Reserve(NumVolumes);
	for (int32 Index = 0; Index < NumVolumes; ++Index)
	{
		AVoxelPaintVolume* Volume = SpawnTestVolume(World, FVector(Index * 200.0, 0.0, 0.0));
		if (Volume == nullptr)
		{
			AddError(FString::Printf(TEXT("Failed to spawn test volume %d"), Index));
			return false;
		}
		Volumes.Add(Volume);
	}

	for (int32 Index = 0; Index < NumVolumes; ++Index)
	{
		TestEqual(TEXT("Spawn order filled ascending frontier positions"),
			Subsystem->GetFrontierPosition(Volumes[Index]->GetVolumeComponent()), Index);
	}
	VerifyFrontierConsistency(*this, *Subsystem);

	// A transform refresh must land in exactly the moved component's slot (the O(1) lookup
	// located it) while every other member stays untouched.
	UVoxelPaintVolumeComponent* MovedComponent = Volumes[2]->GetVolumeComponent();
	MovedComponent->SetMobility(EComponentMobility::Movable);
	const FVector NewLocation(123.0, 456.0, 789.0);
	Volumes[2]->SetActorLocation(NewLocation);
	Subsystem->NotifyTransformChanged(MovedComponent);
	TestEqual(TEXT("Moved volume kept its frontier position"),
		Subsystem->GetFrontierPosition(MovedComponent), 2);
	FVoxelPaintPlacementState MovedState;
	if (TestTrue(TEXT("Moved volume still has a cached state"),
		Subsystem->GetCachedPlacementState(MovedComponent, MovedState)))
	{
		TestEqual(TEXT("Transform refresh landed in the moved volume's slot"),
			MovedState.Translation, NewLocation);
	}
	VerifyFrontierConsistency(*this, *Subsystem);

	// Demotion clears the cache (lookup misses); same-frame identity reclaim rebuilds it
	// through OccupyFrontierSlot.
	UVoxelPaintVolumeComponent* ReclaimComponent = Volumes[3]->GetVolumeComponent();
	Subsystem->UnregisterVolume(ReclaimComponent);
	TestEqual(TEXT("Unregistered component reports no frontier position"),
		Subsystem->GetFrontierPosition(ReclaimComponent), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("Frontier lost exactly the unregistered member"),
		Subsystem->GetNumFrontierMembers(), NumVolumes - 1);
	VerifyFrontierConsistency(*this, *Subsystem);
	Subsystem->RegisterVolume(ReclaimComponent);
	TestEqual(TEXT("Same-frame re-register reclaims the exact slot"),
		Subsystem->GetFrontierPosition(ReclaimComponent), 3);
	VerifyFrontierConsistency(*this, *Subsystem);

	// Actor destruction demotes its slot, the next frame's sweep frees it, and a fresh
	// registrant claims it as the lowest Empty slot with a correct cache.
	World->DestroyActor(Volumes[5]);
	TestEqual(TEXT("Destroyed volume left the frontier"),
		Subsystem->GetNumFrontierMembers(), NumVolumes - 1);
	VerifyFrontierConsistency(*this, *Subsystem);
	const uint64 SavedFrameCounter = GFrameCounter;
	GFrameCounter++;
	Subsystem->NotifyPropertyChanged(Volumes[0]->GetVolumeComponent());
	AVoxelPaintVolume* LateVolume = SpawnTestVolume(World, FVector(0.0, 800.0, 0.0));
	if (LateVolume == nullptr)
	{
		GFrameCounter = SavedFrameCounter;
		AddError(TEXT("Failed to spawn the late test volume"));
		return false;
	}
	TestEqual(TEXT("Late registrant claims the expired slot"),
		Subsystem->GetFrontierPosition(LateVolume->GetVolumeComponent()), 5);
	VerifyFrontierConsistency(*this, *Subsystem);
	GFrameCounter = SavedFrameCounter;

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintFrontier_SweepFrameGate,
	"Game.WorldVoxelPaint.Unit.Registry.SweepFrameGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintFrontier_SweepFrameGate::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintFrontierIndexTestPrivate;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(World);
	if (!TestNotNull(TEXT("Subsystem exists in the test world"), Subsystem))
	{
		return false;
	}

	const uint64 SavedFrameCounter = GFrameCounter;

	AVoxelPaintVolume* VolumeA = SpawnTestVolume(World, FVector::ZeroVector);
	AVoxelPaintVolume* VolumeB = SpawnTestVolume(World, FVector(200.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("Volume A spawned"), VolumeA) || !TestNotNull(TEXT("Volume B spawned"), VolumeB))
	{
		return false;
	}
	UVoxelPaintVolumeComponent* ComponentA = VolumeA->GetVolumeComponent();
	TestEqual(TEXT("Volume A holds slot 0"), Subsystem->GetFrontierPosition(ComponentA), 0);
	TestEqual(TEXT("Volume B holds slot 1"), Subsystem->GetFrontierPosition(VolumeB->GetVolumeComponent()), 1);

	// Gate observable: DestroyComponent (component-level; the actor survives) marks the
	// component garbage WITHOUT firing the actor-level pre-unregister delegate, so slot 0 stays
	// Occupied-with-dead-weak until a sweep demotes it. The spawns above already swept this
	// frame, so gated same-frame calls must defer the demotion (no LayoutGeneration bump) while
	// the coordinator's forced call must land it same-frame.
	const uint64 GenerationBeforeDestroy = Subsystem->GetLayoutGeneration();
	ComponentA->DestroyComponent();
	Subsystem->ExpireStaleReservations();
	Subsystem->ExpireStaleReservations();
	TestEqual(TEXT("Same-frame gated sweeps defer the dead-slot demotion"),
		Subsystem->GetLayoutGeneration(), GenerationBeforeDestroy);
	TestEqual(TEXT("Dead slot still occupied while the demotion is deferred"),
		Subsystem->GetNumFrontierMembers(), 2);

	// While the demotion is deferred the notify paths must be no-ops for the dead component:
	// the IsValid guard early-outs before the (still-hitting) reverse lookup could blank the
	// slot's cached state and bump LayoutGeneration via RefreshCachedState's null-component
	// branch.
	Subsystem->NotifyTransformChanged(ComponentA);
	Subsystem->NotifyPropertyChanged(ComponentA);
	TestEqual(TEXT("Notify calls on the dead component are no-ops"),
		Subsystem->GetLayoutGeneration(), GenerationBeforeDestroy);
	FVoxelPaintPlacementState DeadSlotState;
	if (TestTrue(TEXT("Dead slot still exposes its cached state while demotion is deferred"),
		Subsystem->GetCachedPlacementState(ComponentA, DeadSlotState)))
	{
		TestNotEqual(TEXT("Notify calls did not blank the dead slot's cached state"),
			DeadSlotState.TexelSize, FVoxelPaintPlacementState().TexelSize);
	}

	Subsystem->ExpireStaleReservations(/*bForceSweep*/ true);
	TestEqual(TEXT("Forced sweep demotes the dead slot same-frame"),
		Subsystem->GetLayoutGeneration(), GenerationBeforeDestroy + 1u);
	TestEqual(TEXT("One member remains after the forced demotion"),
		Subsystem->GetNumFrontierMembers(), 1);

	// A reservation stamped this frame cannot expire this frame - gated or forced - so
	// same-frame skipping is provably lossless (the ReservedFrame rule, not the gate).
	const uint64 GenerationAfterDemotion = Subsystem->GetLayoutGeneration();
	Subsystem->ExpireStaleReservations();
	Subsystem->ExpireStaleReservations(/*bForceSweep*/ true);
	TestEqual(TEXT("Same-frame reservation survives repeat and forced sweeps"),
		Subsystem->GetLayoutGeneration(), GenerationAfterDemotion);
	TestEqual(TEXT("Reserved slot is not expired same-frame"), Subsystem->GetNumFrontierMembers(), 1);

	AVoxelPaintVolume* VolumeC = SpawnTestVolume(World, FVector(400.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("Volume C spawned"), VolumeC))
	{
		return false;
	}
	TestEqual(TEXT("Same-frame registrant skips the live Reserved slot"),
		Subsystem->GetFrontierPosition(VolumeC->GetVolumeComponent()), 2);

	// The next frame's first sweep expires the reservation and the freed slot is reclaimed as
	// the lowest Empty slot.
	GFrameCounter++;
	Subsystem->ExpireStaleReservations();
	TestEqual(TEXT("Next-frame sweep expires the reservation"), Subsystem->GetNumFrontierMembers(), 2);
	AVoxelPaintVolume* VolumeD = SpawnTestVolume(World, FVector(600.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("Volume D spawned"), VolumeD))
	{
		GFrameCounter = SavedFrameCounter;
		return false;
	}
	TestEqual(TEXT("Freed slot is reclaimed as the lowest Empty slot"),
		Subsystem->GetFrontierPosition(VolumeD->GetVolumeComponent()), 0);
	TestEqual(TEXT("Three members after reclaim"), Subsystem->GetNumFrontierMembers(), 3);

	// Structural calls force the sweep past the gate: a component-level destroy (no actor
	// pre-unregister delegate, so no UnregisterVolume) leaves slot 1 Occupied-with-dead-weak
	// and the gate is closed (this frame already swept), yet a same-frame replacement on the
	// same actor must reclaim that exact slot - RegisterVolume's forced sweep demotes it to
	// Reserved ahead of the Reserved-identity search, exactly as when the sweep was ungated.
	// A gate that also blocked structural sweeps would land the replacement in slot 3 instead.
	VolumeB->GetVolumeComponent()->DestroyComponent();
	UVoxelPaintVolumeComponent* Replacement = NewObject<UVoxelPaintVolumeComponent>(VolumeB);
	Replacement->RegisterComponent();
	TestEqual(TEXT("Same-frame replacement reclaims the destroyed component's exact slot"),
		Subsystem->GetFrontierPosition(Replacement), 1);
	TestEqual(TEXT("Identity reclaim keeps the member count"),
		Subsystem->GetNumFrontierMembers(), 3);

	GFrameCounter = SavedFrameCounter;

	return true;
}
