// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintVolume.h"
#include "VoxelPaintVolumeComponent.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintHostTagTestPrivate
{

struct FVoxelPaintTestWorld
{
	UWorld* World = nullptr;

	FVoxelPaintTestWorld()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelPaintHostTagTest"));
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

static int32 CountTag(const AActor* Actor, const FName& Tag)
{
	int32 Count = 0;
	for (const FName& Existing : Actor->Tags)
	{
		Count += (Existing == Tag) ? 1 : 0;
	}
	return Count;
}

} // namespace VoxelPaintHostTagTestPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPCG_HostAutoTag,
	"Game.WorldVoxelPaint.Unit.PCG.HostAutoTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPCG_HostAutoTag::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintHostTagTestPrivate;

	const FName Tag = WorldVoxelPaint::PaintVolumeHostTag;
	TestTrue(TEXT("Tag literal is the frozen M6 contract"), Tag == FName(TEXT("WVP.PaintVolume")));

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// Leg 1 - arbitrary host actor (the majority prefab shape): register adds the tag once.
	AActor* Host = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
	if (!TestNotNull(TEXT("Host actor spawned"), Host)) { return false; }
	TestEqual(TEXT("No tag before any volume component"), CountTag(Host, Tag), 0);

	UVoxelPaintVolumeComponent* First = NewObject<UVoxelPaintVolumeComponent>(Host);
	First->RegisterComponent();
	TestEqual(TEXT("Register adds the tag exactly once"), CountTag(Host, Tag), 1);

	// Leg 2 - idempotent re-add: a second component must not duplicate the tag.
	UVoxelPaintVolumeComponent* Second = NewObject<UVoxelPaintVolumeComponent>(Host);
	Second->RegisterComponent();
	TestEqual(TEXT("Second component does not duplicate the tag"), CountTag(Host, Tag), 1);

	// Leg 3 - multi-component removal rule: tag survives while any volume component remains,
	// removed only with the last one.
	Second->DestroyComponent();
	TestEqual(TEXT("Tag survives while another volume component remains"), CountTag(Host, Tag), 1);
	First->DestroyComponent();
	TestEqual(TEXT("Tag removed with the last volume component"), CountTag(Host, Tag), 0);

	// Leg 4 - serialized-tag idempotency: a tag already on the actor (saved by a previous
	// session or copied with a duplicated prefab instance) is not duplicated on register and
	// is still removed by the last unregister; a re-register restores it.
	AActor* SeededHost = World->SpawnActor<AActor>(
		FVector(200.0, 0.0, 0.0), FRotator::ZeroRotator, SpawnParameters);
	if (!TestNotNull(TEXT("Seeded host actor spawned"), SeededHost)) { return false; }
	SeededHost->Tags.Add(Tag);
	UVoxelPaintVolumeComponent* Seeded = NewObject<UVoxelPaintVolumeComponent>(SeededHost);
	Seeded->RegisterComponent();
	TestEqual(TEXT("Pre-serialized tag is not duplicated"), CountTag(SeededHost, Tag), 1);
	Seeded->UnregisterComponent();
	TestEqual(TEXT("Unregister of the only volume component removes the tag"), CountTag(SeededHost, Tag), 0);
	Seeded->RegisterComponent();
	TestEqual(TEXT("Re-register restores the tag"), CountTag(SeededHost, Tag), 1);

	// Leg 5 - the dedicated actor is covered by the exact same component path.
	AVoxelPaintVolume* Dedicated = World->SpawnActor<AVoxelPaintVolume>(
		FVector(400.0, 0.0, 0.0), FRotator::ZeroRotator, SpawnParameters);
	if (!TestNotNull(TEXT("Dedicated volume actor spawned"), Dedicated)) { return false; }
	TestEqual(TEXT("Dedicated actor is tagged through its component"), CountTag(Dedicated, Tag), 1);

	return true;
	// Teardown implicit: ~FVoxelPaintTestWorld destroys the world and all actors.
}
