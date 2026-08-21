// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

// Editor-only commit -> scoped-refresh bridge (M6 plan 7.2). Subscribes to the core module's
// WorldVoxelPaint::OnVolumeDataCommitted() delegate at module startup. On each commit (or
// reset - both content changes route through the same funnel), it enumerates every placement
// consuming the committed asset, classifies the host actors (Level-Instance adapter), groups
// them with the pure grouping function (VoxelPaintPCGRefreshGrouping.h), and issues one
// UPCGSubsystem::NotifySelectionKeyChanged per grouped call per affected world, with the asset
// as the originating-change object on every call. PCG then octree-intersects each box
// independently and refreshes each matching component at most once per call - runtime-managed
// components are routed to the runtime-gen scheduler by UPCGComponent::Refresh itself.
// Static-only: no instances, one process-wide subscription.
class FVoxelPaintPCGRefreshHandler
{
public:
	static void Startup();    // idempotent; call from FWorldVoxelPaintPCGModule::StartupModule
	static void Shutdown();   // removes the subscription; call from ShutdownModule

private:
	static FDelegateHandle CommitHandle;
};

#endif // WITH_EDITOR
