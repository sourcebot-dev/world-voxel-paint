// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "RHI.h"
#include "VoxelPaintCoordinator.h"
#include "VoxelPaintWorldSubsystem.h"
#include "WorldVoxelPaintConstants.h"

namespace
{

// UE 5.7's GPU profiler picks its capture frame asynchronously (the trigger latches against the
// GPU-completion high-water mark, which lags the game thread by pipeline depth), so a one-shot
// rebuild can miss the captured frame. Re-forcing the repack for this many frames guarantees the
// capture window contains a rebuild frame regardless of when the profiler latches.
constexpr int32 ProfileRebuildBurstFrames = 8;

void ProfileRebuild(const TArray<FString>& Args, UWorld* World)
{
	UVoxelPaintWorldSubsystem* Subsystem = World ? UVoxelPaintWorldSubsystem::Get(World) : nullptr;
	if (!Subsystem)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("wvp.Grid.ProfileRebuild: no voxel-paint subsystem in the executing world."));
		return;
	}

	Subsystem->ForceLayoutRepack(ProfileRebuildBurstFrames);

	if (const FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
	{
		const UWorld* MasterWorld = Coordinator->GetCurrentMasterWorld();
		if (MasterWorld && MasterWorld != World)
		{
			UE_LOG(LogWorldVoxelPaint, Warning,
				TEXT("wvp.Grid.ProfileRebuild: executed in '%s' but the current master world is '%s' - only the master world's rebuild renders. Run the command there (e.g. stop PIE first)."),
				*GetNameSafe(World), *GetNameSafe(MasterWorld));
		}
	}

	const IConsoleVariable* EnableVariable =
		IConsoleManager::Get().FindConsoleVariable(TEXT("r.VoxelPaint.Enable"));
	if (EnableVariable && EnableVariable->GetInt() == 0)
	{
		UE_LOG(LogWorldVoxelPaint, Warning,
			TEXT("wvp.Grid.ProfileRebuild: r.VoxelPaint.Enable is 0 - the forced repack will not rebuild until it is re-enabled."));
	}

	if (GUsingNullRHI)
	{
		UE_LOG(LogWorldVoxelPaint, Display,
			TEXT("wvp.Grid.ProfileRebuild: repack burst forced (layout generation %llu); GPU capture skipped under NullRHI."),
			Subsystem->GetLayoutGeneration());
		return;
	}

	// The capture fires on a LATER rendered frame; in unattended/automation sessions nobody can
	// view it and its completion side effects (profile-visualizer load) would land inside
	// whatever automation test happens to be running then. Repack burst still forced.
	if (FApp::IsUnattended() || GIsAutomationTesting)
	{
		UE_LOG(LogWorldVoxelPaint, Display,
			TEXT("wvp.Grid.ProfileRebuild: repack burst forced (layout generation %llu); GPU capture skipped in an unattended/automation session."),
			Subsystem->GetLayoutGeneration());
		return;
	}

	const bool bDump = Args.ContainsByPredicate(
		[](const FString& Arg) { return Arg.Equals(TEXT("dump"), ESearchCase::IgnoreCase); });
	const TCHAR* CaptureCommand = bDump ? TEXT("DumpGPU") : TEXT("ProfileGPU");
	GEngine->Exec(World, CaptureCommand);
	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("wvp.Grid.ProfileRebuild: repack forced for this and the next %d serviced master frames (layout generation %llu); %s armed - the capture window will contain a rebuild frame. Per-pass GPU timings appear under 'WorldVoxelPaint.GridRebuild' (compare with stat voxelpaint's Grid Emit Upper Bound). The master world's primary viewport must be rendering (Realtime)."),
		ProfileRebuildBurstFrames - 1, Subsystem->GetLayoutGeneration(), CaptureCommand);
}

FAutoConsoleCommandWithWorldAndArgs GVoxelPaintProfileRebuildCommand(
	TEXT("wvp.Grid.ProfileRebuild"),
	TEXT("wvp.Grid.ProfileRebuild [dump]\n")
	TEXT("Forces a full voxel-paint record repack + grid rebuild, re-forced for a burst of the next several serviced master frames, and arms ProfileGPU - the capture frame (chosen asynchronously by the GPU profiler) is guaranteed to contain a rebuild, with per-pass GPU timings under 'WorldVoxelPaint.GridRebuild'.\n")
	TEXT("'dump' arms DumpGPU instead for a full offline capture. ProfileGPU output honors your r.ProfileGPU.* settings (r.ProfileGPU.ShowUI 0 keeps it log-only).\n")
	TEXT("Editor diagnostic only; requires a rendering (Realtime) master primary viewport and r.VoxelPaint.Enable 1; warns when executed outside the master world. Under NullRHI or in unattended/automation sessions the repack burst is still forced but the capture is skipped."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ProfileRebuild));

} // namespace
