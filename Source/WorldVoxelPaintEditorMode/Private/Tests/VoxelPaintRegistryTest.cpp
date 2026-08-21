// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/World.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintTypes.h"
#include "VoxelPaintVolume.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "VoxelPaintWorldSubsystem.h"
#include "WorldVoxelPaintConstants.h"

namespace
{

struct FVoxelPaintTestWorld
{
	UWorld* World = nullptr;

	FVoxelPaintTestWorld()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelPaintTest"));
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

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintRegistry_RegisterRoundTrip,
	"Game.WorldVoxelPaint.Unit.Registry.RegisterRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintRegistry_RegisterRoundTrip::RunTest(const FString& Parameters)
{
	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(World);
	if (!TestNotNull(TEXT("Subsystem exists in the test world"), Subsystem))
	{
		return false;
	}

	AVoxelPaintVolume* Volume = SpawnTestVolume(World, FVector::ZeroVector);
	if (!TestNotNull(TEXT("Volume spawned"), Volume))
	{
		return false;
	}
	UVoxelPaintVolumeComponent* Component = Volume->GetVolumeComponent();
	if (!TestNotNull(TEXT("Volume has a component"), Component))
	{
		return false;
	}

	Component->BlendModeG = EVoxelPaintBlendMode::Subtract;
	UVoxelPaintVolumeData* VolumeData = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	FVoxelPaintVariantPoolEntry& PoolEntry = Component->VolumeVariants.AddDefaulted_GetRef();
	PoolEntry.Variant = VolumeData;
	Subsystem->NotifyPropertyChanged(Component);

	TestTrue(TEXT("Component holds a registry slot"), Component->GetRegistrySlotIndex() != INDEX_NONE);
	TestEqual(TEXT("Component holds frontier position 0"), Subsystem->GetFrontierPosition(Component), 0);
	TestEqual(TEXT("One frontier member"), Subsystem->GetNumFrontierMembers(), 1);
	TestEqual(TEXT("One registered volume"), Subsystem->CountRegisteredVolumes(), 1);

	TArray<FVoxelPaintVolumeRecord> Records;
	Subsystem->BuildRecordSnapshot(FVector::ZeroVector, Records);
	if (TestEqual(TEXT("Snapshot packs one record"), Records.Num(), 1))
	{
		const FVoxelPaintVolumeRecord& Record = Records[0];
		TestTrue(TEXT("Record BlendCoefficients reflect BlendModeG = Subtract"), Record.BlendCoefficients == FVector4f(1.0f, -1.0f, 1.0f, 1.0f));
		TestTrue(TEXT("Record LocalHalfExtent matches the effective extent"), Record.LocalHalfExtent == FVector3f(Component->GetEffectiveLocalHalfExtent()));
		TestTrue(TEXT("Record StorageMin matches the asset storage minimum"), Record.StorageMin == FVector3f(VolumeData->GetStorageMin()));
		TestTrue(TEXT("Record Flags carry the valid bit"), Record.Flags == WorldVoxelPaint::RecordFlag_Valid);
		TestTrue(TEXT("Record PageTableBaseIndex is the no-table sentinel"), Record.PageTableBaseIndex == WorldVoxelPaint::NoPageTableSentinel);
	}

	const uint64 LayoutGenerationBefore = Subsystem->GetLayoutGeneration();
	World->DestroyActor(Volume);

	TestEqual(TEXT("Registry empty after destroy"), Subsystem->CountRegisteredVolumes(), 0);
	TestEqual(TEXT("Frontier empty after destroy"), Subsystem->GetNumFrontierMembers(), 0);
	TestTrue(TEXT("Layout generation advanced on removal"), Subsystem->GetLayoutGeneration() > LayoutGenerationBefore);

	Records.Reset();
	Subsystem->BuildRecordSnapshot(FVector::ZeroVector, Records);
	TestEqual(TEXT("Snapshot empty after destroy"), Records.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintRegistry_FrontierFifoSticky,
	"Game.WorldVoxelPaint.Unit.Registry.FrontierFifoSticky",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintRegistry_FrontierFifoSticky::RunTest(const FString& Parameters)
{
	AddExpectedErrorPlain(TEXT("Selected-placement frontier is full"), EAutomationExpectedErrorFlags::Contains, 0);

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(World);
	if (!TestNotNull(TEXT("Subsystem exists in the test world"), Subsystem))
	{
		return false;
	}

	const int32 NumVolumes = WorldVoxelPaint::MaxSelectedPlacements + 2;
	const int32 FirstWaiterIndex = WorldVoxelPaint::MaxSelectedPlacements;
	const int32 SecondWaiterIndex = WorldVoxelPaint::MaxSelectedPlacements + 1;
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

	TestEqual(TEXT("Frontier holds exactly MaxSelectedPlacements members"), Subsystem->GetNumFrontierMembers(), WorldVoxelPaint::MaxSelectedPlacements);
	TestEqual(TEXT("Registry holds every volume"), Subsystem->CountRegisteredVolumes(), NumVolumes);
	TestEqual(TEXT("First excess volume is a waiter"), Subsystem->GetFrontierPosition(Volumes[FirstWaiterIndex]->GetVolumeComponent()), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("Second excess volume is a waiter"), Subsystem->GetFrontierPosition(Volumes[SecondWaiterIndex]->GetVolumeComponent()), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("Spawn order filled ascending frontier positions"), Subsystem->GetFrontierPosition(Volumes[5]->GetVolumeComponent()), 5);

	World->DestroyActor(Volumes[5]);
	const uint64 SavedFrameCounter = GFrameCounter;
	GFrameCounter++;
	Subsystem->NotifyPropertyChanged(Volumes[0]->GetVolumeComponent());
	GFrameCounter = SavedFrameCounter;

	TestEqual(TEXT("Oldest waiter promoted into the freed slot (FIFO)"), Subsystem->GetFrontierPosition(Volumes[FirstWaiterIndex]->GetVolumeComponent()), 5);
	TestEqual(TEXT("Second waiter still waits"), Subsystem->GetFrontierPosition(Volumes[SecondWaiterIndex]->GetVolumeComponent()), static_cast<int32>(INDEX_NONE));

	bool bPositionsSticky = true;
	for (int32 Index = 0; Index < WorldVoxelPaint::MaxSelectedPlacements; ++Index)
	{
		if (Index == 5)
		{
			continue;
		}
		if (Subsystem->GetFrontierPosition(Volumes[Index]->GetVolumeComponent()) != Index)
		{
			bPositionsSticky = false;
			AddError(FString::Printf(TEXT("Volume %d lost its frontier position"), Index));
			break;
		}
	}
	TestTrue(TEXT("Every other member kept its position (sticky)"), bPositionsSticky);

	UVoxelPaintVolumeComponent* ReclaimComponent = Volumes[10]->GetVolumeComponent();
	const int32 PositionBefore = Subsystem->GetFrontierPosition(ReclaimComponent);
	TestEqual(TEXT("Reclaim candidate holds its position"), PositionBefore, 10);
	Subsystem->UnregisterVolume(ReclaimComponent);
	TestEqual(TEXT("Unregistered component left the registry"), ReclaimComponent->GetRegistrySlotIndex(), static_cast<int32>(INDEX_NONE));
	Subsystem->RegisterVolume(ReclaimComponent);
	TestEqual(TEXT("Same-frame re-register reclaims the exact slot"), Subsystem->GetFrontierPosition(ReclaimComponent), PositionBefore);
	TestEqual(TEXT("Waiter did not steal the reserved slot"), Subsystem->GetFrontierPosition(Volumes[SecondWaiterIndex]->GetVolumeComponent()), static_cast<int32>(INDEX_NONE));

	return true;
}
