// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintPCGRefreshGrouping.h"

#if WITH_EDITOR

#include "GameFramework/Actor.h"

namespace WorldVoxelPaintPCG
{

namespace
{

// The frozen "stable choice": case-sensitive lexicographic GetPathName() order. Unique per
// world, stable across sessions for saved actors, independent of spawn/iteration order.
bool IsStablyBefore(const AActor& A, const AActor& B)
{
	return A.GetPathName().Compare(B.GetPathName(), ESearchCase::CaseSensitive) < 0;
}

void AppendValidBoxes(const FVoxelPaintPCGConsumingHost& Host, TArray<FBox>& OutBoxes)
{
	for (const FBox& Box : Host.Boxes)
	{
		if (Box.IsValid)
		{
			OutBoxes.Add(Box);
		}
	}
}

} // anonymous namespace

void GroupConsumingHostsForRefresh(TConstArrayView<FVoxelPaintPCGConsumingHost> Hosts,
	TArray<FVoxelPaintPCGRefreshCall>& OutCalls)
{
	OutCalls.Reset();

	// Deterministic processing order: stable host order, null hosts skipped defensively.
	TArray<const FVoxelPaintPCGConsumingHost*> Ordered;
	Ordered.Reserve(Hosts.Num());
	for (const FVoxelPaintPCGConsumingHost& Host : Hosts)
	{
		if (Host.Host)
		{
			Ordered.Add(&Host);
		}
	}
	Ordered.Sort([](const FVoxelPaintPCGConsumingHost& A, const FVoxelPaintPCGConsumingHost& B)
		{
			return IsStablyBefore(*A.Host, *B.Host);
		});
	if (Ordered.IsEmpty())
	{
		return;
	}

	// Classification. bInEditingLIHierarchy without a parent LI normalizes to top-level.
	auto IsTopLevel = [](const FVoxelPaintPCGConsumingHost& Host)
	{
		return Host.ParentLevelInstance == nullptr;
	};
	auto IsEditingGrouped = [&IsTopLevel](const FVoxelPaintPCGConsumingHost& Host)
	{
		return !IsTopLevel(Host) && Host.bInEditingLIHierarchy;
	};

	bool bAnyTopLevel = false;
	for (const FVoxelPaintPCGConsumingHost* Host : Ordered)
	{
		bAnyTopLevel |= IsTopLevel(*Host);
	}

	struct FGroup
	{
		AActor* KeyHost = nullptr;
		TArray<const FVoxelPaintPCGConsumingHost*> Members;   // stable order preserved
	};

	// Partition. With a top-level host present: non-editing hosts (top-level and LI-interior
	// alike) coalesce; editing-LI hosts group by immediate parent. With none: everything
	// groups by immediate parent (all hosts have one - a parentless host would be top-level).
	FGroup Coalesced;
	TArray<TPair<AActor*, FGroup>> LIGroups;   // parent-LI actor -> group

	auto FindOrAddLIGroup = [&LIGroups](AActor* ParentLI) -> FGroup&
	{
		for (TPair<AActor*, FGroup>& Pair : LIGroups)
		{
			if (Pair.Key == ParentLI)
			{
				return Pair.Value;
			}
		}
		LIGroups.Emplace(ParentLI, FGroup());
		return LIGroups.Last().Value;
	};

	for (const FVoxelPaintPCGConsumingHost* Host : Ordered)
	{
		const bool bToLIGroup = bAnyTopLevel ? IsEditingGrouped(*Host) : true;
		if (bToLIGroup)
		{
			FindOrAddLIGroup(Host->ParentLevelInstance).Members.Add(Host);
		}
		else
		{
			Coalesced.Members.Add(Host);
		}
	}

	// Coalesced key: painted-through top-level member first (stable-first if several were
	// flagged), else the stable-first top-level member. The key MUST be top-level - an
	// LI-interior key actor would suppress refreshes around other placements (PCG's final
	// refresh gate runs its Level-Instance checks against the key-resolved actor).
	if (!Coalesced.Members.IsEmpty())
	{
		for (const FVoxelPaintPCGConsumingHost* Member : Coalesced.Members)
		{
			if (Member->bPaintedThroughHost && IsTopLevel(*Member))
			{
				Coalesced.KeyHost = Member->Host;
				break;
			}
		}
		if (!Coalesced.KeyHost)
		{
			for (const FVoxelPaintPCGConsumingHost* Member : Coalesced.Members)
			{
				if (IsTopLevel(*Member))
				{
					Coalesced.KeyHost = Member->Host;
					break;
				}
			}
		}
		// The coalesced group exists only when a top-level host exists, and every top-level
		// host is a coalesced member - a key host is therefore always found.
		check(Coalesced.KeyHost);
	}

	// LI-group keys: stable-first member. Then order the group calls by parent-LI stable order.
	for (TPair<AActor*, FGroup>& Pair : LIGroups)
	{
		Pair.Value.KeyHost = Pair.Value.Members[0]->Host;
	}
	LIGroups.Sort([](const TPair<AActor*, FGroup>& A, const TPair<AActor*, FGroup>& B)
		{
			return IsStablyBefore(*A.Key, *B.Key);
		});

	// Emission: group calls first (coalesced, then LI groups), then the carve-outs.
	auto EmitGroupCall = [&OutCalls](const FGroup& Group)
	{
		FVoxelPaintPCGRefreshCall& Call = OutCalls.AddDefaulted_GetRef();
		Call.KeyHost = Group.KeyHost;
		for (const FVoxelPaintPCGConsumingHost* Member : Group.Members)
		{
			AppendValidBoxes(*Member, Call.Boxes);
		}
	};
	if (!Coalesced.Members.IsEmpty())
	{
		EmitGroupCall(Coalesced);
	}
	for (const TPair<AActor*, FGroup>& Pair : LIGroups)
	{
		EmitGroupCall(Pair.Value);
	}

	// bOnlyTrackItself carve-out (plan 7.2): one dedicated call per self-only host that is not
	// already its group's key, carrying that host's own boxes (which also stayed in the group's
	// call above). A zero-box dedicated call is still emitted: the built-in Self always-track
	// key dirties the host's own PCG components without any bounds probe, and the final gate
	// admits a bOnlyTrackItself component exactly when its owner is the key-resolved actor.
	auto EmitCarveOuts = [&OutCalls](const FGroup& Group)
	{
		for (const FVoxelPaintPCGConsumingHost* Member : Group.Members)
		{
			if (Member->bHasSelfOnlyPCGComponent && Member->Host != Group.KeyHost)
			{
				FVoxelPaintPCGRefreshCall& Call = OutCalls.AddDefaulted_GetRef();
				Call.KeyHost = Member->Host;
				AppendValidBoxes(*Member, Call.Boxes);
			}
		}
	};
	if (!Coalesced.Members.IsEmpty())
	{
		EmitCarveOuts(Coalesced);
	}
	for (const TPair<AActor*, FGroup>& Pair : LIGroups)
	{
		EmitCarveOuts(Pair.Value);
	}
}

} // namespace WorldVoxelPaintPCG

#endif // WITH_EDITOR
