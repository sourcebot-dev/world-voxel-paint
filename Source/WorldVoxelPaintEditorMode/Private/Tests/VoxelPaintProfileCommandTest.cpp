// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintCoordinator.h"
#include "VoxelPaintWorldSubsystem.h"

namespace VoxelPaintProfileCommandTestPrivate
{

struct FVoxelPaintTestWorld
{
	UWorld* World = nullptr;
	FVoxelPaintTestWorld()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelPaintProfileCommandTest"));
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

} // namespace VoxelPaintProfileCommandTestPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintProfileCommand_ForcesRepack,
	"Game.WorldVoxelPaint.Unit.Toolset.ProfileRebuildCommand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintProfileCommand_ForcesRepack::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintProfileCommandTestPrivate;

	if (!TestNotNull(TEXT("wvp.Grid.ProfileRebuild is registered"),
		IConsoleManager::Get().FindConsoleObject(TEXT("wvp.Grid.ProfileRebuild"))))
	{
		return false;
	}

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(World);
	if (!TestNotNull(TEXT("Subsystem exists in the test world"), Subsystem))
	{
		return false;
	}

	// The command warns when the exec world is not the coordinator's master world - true for
	// this throwaway world whenever a master is resolved (real-RHI editor sessions), but under
	// -nullrhi no master may exist at all, so expect the warning only when the command's own
	// condition will hold.
	if (const FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
	{
		const UWorld* MasterWorld = Coordinator->GetCurrentMasterWorld();
		if (MasterWorld && MasterWorld != World)
		{
			AddExpectedErrorPlain(TEXT("only the master world's rebuild renders"),
				EAutomationExpectedErrorFlags::Contains, 0);
		}
	}

	// Burst mechanics: NumFrames > 1 re-forces the repack once per subsequent FRAME (so an
	// asynchronously chosen GPU capture window contains a rebuild), and mutating calls within
	// one frame must not drain the burst early.
	{
		const uint64 BurstStart = Subsystem->GetLayoutGeneration();
		const uint64 SavedFrameCounter = GFrameCounter;
		Subsystem->ForceLayoutRepack(3);
		TestEqual(TEXT("Burst bumps the generation immediately"),
			Subsystem->GetLayoutGeneration(), BurstStart + 1u);
		Subsystem->ExpireStaleReservations();
		TestEqual(TEXT("Same-frame service does not drain the burst"),
			Subsystem->GetLayoutGeneration(), BurstStart + 1u);
		GFrameCounter++;
		Subsystem->ExpireStaleReservations();
		TestEqual(TEXT("The next frame re-forces the repack"),
			Subsystem->GetLayoutGeneration(), BurstStart + 2u);
		GFrameCounter++;
		Subsystem->ExpireStaleReservations();
		TestEqual(TEXT("The burst length is honored"),
			Subsystem->GetLayoutGeneration(), BurstStart + 3u);
		GFrameCounter++;
		Subsystem->ExpireStaleReservations();
		TestEqual(TEXT("The burst terminates"),
			Subsystem->GetLayoutGeneration(), BurstStart + 3u);
		GFrameCounter = SavedFrameCounter;
	}

	// Suite runs never arm a capture (the command skips it under NullRHI and in
	// unattended/automation sessions), so the exec below has no fire-later side effects on
	// other tests.
	const uint64 GenerationBefore = Subsystem->GetLayoutGeneration();
	GEngine->Exec(World, TEXT("wvp.Grid.ProfileRebuild"));
	TestEqual(TEXT("The command advances the layout generation immediately"),
		Subsystem->GetLayoutGeneration(), GenerationBefore + 1u);

	return true;
}
