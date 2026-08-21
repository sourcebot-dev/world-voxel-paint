// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Math/Box.h"

#if WITH_EDITOR

class AActor;

namespace WorldVoxelPaintPCG
{
	// One consuming host actor of a committed paint asset, pre-classified by the editor-only
	// Level-Instance adapter (VoxelPaintPCGRefreshHandler.cpp). Pure data: the grouping
	// function below reads engine state only through AActor::GetPathName(), which is what makes
	// it permanently testable over synthetic descriptors (M6 plan 7.2 test scope).
	struct FVoxelPaintPCGConsumingHost
	{
		// The host actor (carries the WVP.PaintVolume auto-tag at call time, so a key built
		// from it matches the registered ByTag tracked key). Precondition: at most one
		// descriptor per host per grouping call; null-host descriptors are skipped.
		AActor* Host = nullptr;

		// World-space bounds of every consuming volume component on Host. Invalid boxes are
		// dropped at emission (PCG treats an invalid FBox as a degenerate zero-extent octree
		// probe at the origin, not a skip), and a host whose boxes all drop still participates
		// as a group member and key candidate.
		TArray<FBox> Boxes;

		// Immediate parent Level Instance ACTOR (CastChecked<AActor> of the
		// ILevelInstanceInterface the subsystem returns), null when the host is top-level.
		AActor* ParentLevelInstance = nullptr;

		// AActor::IsInEditLevelInstanceHierarchy(): the host sits inside the currently edited
		// Level-Instance hierarchy. Meaningful only with a non-null ParentLevelInstance; a
		// descriptor setting it with a null parent is normalized to top-level (defensive - the
		// engine cannot produce that combination).
		bool bInEditingLIHierarchy = false;

		// The placement the paint session was performed through. Preference input only: when
		// set on a TOP-LEVEL host, that host becomes the coalesced call's key host (plan 7.2);
		// ignored everywhere else (the coalesced key must be top-level; LI groups always use
		// the stable choice). The current adapter never sets it - no commit-time signal
		// identifies the painted-through placement under the asset-only commit delegate - so
		// the stable choice is operative today (DECISION_LOG [M6-REFRESH]).
		bool bPaintedThroughHost = false;

		// Host owns at least one UPCGComponent with bOnlyTrackItself: drives the plan-7.2
		// dedicated-call carve-out (such a component refreshes only when its owner IS the
		// key-resolved actor, so a grouped call keyed elsewhere would dirty it but never
		// refresh it - silent stale state).
		bool bHasSelfOnlyPCGComponent = false;
	};

	// One refresh call the commit handler must issue:
	//   Subsystem->NotifySelectionKeyChanged(FPCGSelectionKey::CreateFromObjectPtr(KeyHost),
	//       CommittedAsset, Boxes)
	// with the committed asset as the originating-change object on EVERY call (plan 7.2 - it
	// feeds PCG's managed-resource and ignore-origin safeguards and, not being a UPCGComponent,
	// keeps all PCG-component special cases inert).
	struct FVoxelPaintPCGRefreshCall
	{
		AActor* KeyHost = nullptr;
		TArray<FBox> Boxes;
	};

	// Plan 7.2's grouping rule as a pure, deterministic function.
	//
	// Rule (implemented EXACTLY; residuals of grouped keying are accepted by the plan, do not
	// "fix" here):
	//  - When at least one host is top-level (null ParentLevelInstance), every host NOT inside
	//    an editing-LI hierarchy - top-level and non-editing-LI hosts alike - coalesces into
	//    ONE call keyed on a top-level host: the painted-through host when it is top-level,
	//    otherwise the stable choice among the top-level hosts. Hosts inside an editing-LI
	//    hierarchy group by immediate parent Level Instance, one call per group keyed on the
	//    stable choice within the group.
	//  - When no top-level host exists, ALL hosts group by immediate parent Level Instance
	//    (the only grouping that preserves native per-host refresh results there).
	//  - Carve-out: every host with bHasSelfOnlyPCGComponent that is NOT its group's key host
	//    additionally gets one dedicated call keyed on that host, carrying that host's own
	//    boxes; the boxes also STAY in the group's call. No dedicated call when the host is
	//    already its group's key.
	//
	// Stable choice / determinism contract: hosts are ordered by case-sensitive lexicographic
	// AActor::GetPathName(); output order is [coalesced call, LI-group calls by parent-LI
	// stable order, carve-out calls (coalesced group's first, then per LI group, hosts in
	// stable order)]; boxes concatenate in member stable order with invalid boxes dropped. The
	// output is a pure function of the descriptor SET - input array order never matters.
	WORLDVOXELPAINTPCG_API void GroupConsumingHostsForRefresh(
		TConstArrayView<FVoxelPaintPCGConsumingHost> Hosts,
		TArray<FVoxelPaintPCGRefreshCall>& OutCalls);

} // namespace WorldVoxelPaintPCG

#endif // WITH_EDITOR
