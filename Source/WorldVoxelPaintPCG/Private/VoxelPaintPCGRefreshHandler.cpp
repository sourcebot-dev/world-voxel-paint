// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintPCGRefreshHandler.h"

#if WITH_EDITOR

#include "Elements/PCGActorSelector.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "PCGComponent.h"
#include "Subsystems/PCGSubsystem.h"
#include "UObject/UObjectIterator.h"
#include "VoxelPaintPCGRefreshGrouping.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "WorldVoxelPaintConstants.h"

FDelegateHandle FVoxelPaintPCGRefreshHandler::CommitHandle;

namespace VoxelPaintPCGRefreshHandlerPrivate
{

// Level-Instance classification adapter (plan 7.2; deliberately thin - review+manual covered,
// the grouping itself is the tested pure function). Uses the exact calls PCG's own final
// refresh gate runs against the key-resolved actor (GetParentLevelInstance /
// IsInEditLevelInstanceHierarchy), so classification here matches the gate's by construction.
void ClassifyHost(ULevelInstanceSubsystem* LevelInstances,
	WorldVoxelPaintPCG::FVoxelPaintPCGConsumingHost& Descriptor)
{
	AActor* Host = Descriptor.Host;

	ILevelInstanceInterface* ParentLI =
		LevelInstances ? LevelInstances->GetParentLevelInstance(Host) : nullptr;
	// Interface -> actor identity, the engine's own pattern (LevelInstanceSubsystem.cpp uses
	// CastChecked<AActor> on these interfaces throughout). Null subsystem (worlds without it)
	// classifies every host top-level - the refresh gate skips all LI checks there too.
	Descriptor.ParentLevelInstance = ParentLI ? CastChecked<AActor>(ParentLI) : nullptr;
	Descriptor.bInEditingLIHierarchy = Host->IsInEditLevelInstanceHierarchy();

	// bPaintedThroughHost stays false: the commit delegate is deliberately asset-only (plan
	// 5.3) and no core/Toolset API identifies the painted-through placement at commit time; the
	// grouping rule's stable choice selects the key host. The preference is implemented in the
	// pure function and activates if a signal is ever plumbed (DECISION_LOG [M6-REFRESH]).

	// bOnlyTrackItself detection scan (plan 7.2 carve-out). The member is WITH_EDITORONLY_DATA;
	// this whole translation unit is WITH_EDITOR.
	TInlineComponentArray<UPCGComponent*> PCGComponents;
	Host->GetComponents(PCGComponents);
	for (const UPCGComponent* PCGComponent : PCGComponents)
	{
		if (PCGComponent && PCGComponent->bOnlyTrackItself)
		{
			Descriptor.bHasSelfOnlyPCGComponent = true;
			break;
		}
	}
}

void OnVolumeDataCommitted(UVoxelPaintVolumeData* Asset)
{
	check(IsInGameThread());
	if (!Asset)
	{
		return;
	}

	// 1) Enumerate every placement consuming the asset - the NotifyConsumingComponents scan
	//    shape (VoxelPaintVolumeData.cpp): TObjectIterator + !IsTemplate + PoolContains. Pool
	//    membership (any variant-pool entry referencing the asset) is the correct semantic for
	//    "every placement of that asset". Additionally require a registered, world-resident
	//    component with an owner: only those carry live bounds and a world to route to. Fires
	//    for reset (paint clear) too - a cleared region must regenerate; never "optimize" this
	//    to skip uninitialized assets.
	TMap<UWorld*, TMap<AActor*, WorldVoxelPaintPCG::FVoxelPaintPCGConsumingHost>> WorldHosts;
	for (TObjectIterator<UVoxelPaintVolumeComponent> It; It; ++It)
	{
		UVoxelPaintVolumeComponent* Component = *It;
		if (!Component || Component->IsTemplate() || !Component->IsRegistered()
			|| !Component->PoolContains(Asset))
		{
			continue;
		}
		UWorld* World = Component->GetWorld();
		AActor* Host = Component->GetOwner();
		if (!World || !Host)
		{
			continue;
		}

		WorldVoxelPaintPCG::FVoxelPaintPCGConsumingHost& Descriptor =
			WorldHosts.FindOrAdd(World).FindOrAdd(Host);
		Descriptor.Host = Host;
		const FBox Bounds = Component->CalcBounds(Component->GetComponentTransform()).GetBox();
		if (Bounds.IsValid)
		{
			Descriptor.Boxes.Add(Bounds);
		}
	}

	// 2-4) Per affected world (the tracking maps/octrees are per-world; editor + PIE are both
	//      live during PIE): classify hosts, group, and issue one NotifySelectionKeyChanged per
	//      grouped call - key = a tagged host actor (a bare ByTag key resolves no object and
	//      matches nothing; keyed on a tagged host, the registered culled ByTag key matches via
	//      live actor Tags), originating-change object = the asset on EVERY call.
	for (TPair<UWorld*, TMap<AActor*, WorldVoxelPaintPCG::FVoxelPaintPCGConsumingHost>>& WorldPair : WorldHosts)
	{
		UWorld* World = WorldPair.Key;
		UPCGSubsystem* Subsystem = UPCGSubsystem::GetInstance(World);
		if (!Subsystem)
		{
			continue;
		}

		ULevelInstanceSubsystem* LevelInstances = UWorld::GetSubsystem<ULevelInstanceSubsystem>(World);
		TArray<WorldVoxelPaintPCG::FVoxelPaintPCGConsumingHost> Descriptors;
		Descriptors.Reserve(WorldPair.Value.Num());
		for (TPair<AActor*, WorldVoxelPaintPCG::FVoxelPaintPCGConsumingHost>& HostPair : WorldPair.Value)
		{
			ClassifyHost(LevelInstances, HostPair.Value);
			Descriptors.Add(MoveTemp(HostPair.Value));
		}

		TArray<WorldVoxelPaintPCG::FVoxelPaintPCGRefreshCall> Calls;
		WorldVoxelPaintPCG::GroupConsumingHostsForRefresh(Descriptors, Calls);

		for (const WorldVoxelPaintPCG::FVoxelPaintPCGRefreshCall& Call : Calls)
		{
			Subsystem->NotifySelectionKeyChanged(
				FPCGSelectionKey::CreateFromObjectPtr(Call.KeyHost), Asset, Call.Boxes);
		}

		UE_LOG(LogWorldVoxelPaint, Verbose,
			TEXT("VoxelPaintPCG: commit of '%s' issued %d scoped refresh call(s) over %d consuming host(s) in world '%s'."),
			*Asset->GetName(), Calls.Num(), Descriptors.Num(), *World->GetName());
	}
}

} // namespace VoxelPaintPCGRefreshHandlerPrivate

void FVoxelPaintPCGRefreshHandler::Startup()
{
	if (!CommitHandle.IsValid())
	{
		CommitHandle = WorldVoxelPaint::OnVolumeDataCommitted().AddStatic(
			&VoxelPaintPCGRefreshHandlerPrivate::OnVolumeDataCommitted);
	}
}

void FVoxelPaintPCGRefreshHandler::Shutdown()
{
	if (CommitHandle.IsValid())
	{
		WorldVoxelPaint::OnVolumeDataCommitted().Remove(CommitHandle);
		CommitHandle.Reset();
	}
}

#endif // WITH_EDITOR
