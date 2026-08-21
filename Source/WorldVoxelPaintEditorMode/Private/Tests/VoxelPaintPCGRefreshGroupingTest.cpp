// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintPCGRefreshGrouping.h"

namespace VoxelPaintPCGRefreshGroupingTestPrivate
{

struct FVoxelPaintTestWorld
{
	UWorld* World = nullptr;

	FVoxelPaintTestWorld()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelPaintGroupingTest"));
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

// Explicitly named spawns: the grouping contract's stable choice is case-sensitive
// lexicographic GetPathName() order, and all actors share the world/level path prefix, so
// name choice pins the expected order exactly.
AActor* SpawnHost(UWorld* World, const TCHAR* Name)
{
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParameters.Name = FName(Name);
	return World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
}

FBox BoxAt(double CenterX)
{
	const FVector Center(CenterX, 0.0, 0.0);
	return FBox(Center - FVector(100.0), Center + FVector(100.0));
}

WorldVoxelPaintPCG::FVoxelPaintPCGConsumingHost MakeHost(AActor* Host, const TArray<FBox>& Boxes,
	AActor* ParentLevelInstance = nullptr, bool bInEditingLIHierarchy = false,
	bool bHasSelfOnlyPCGComponent = false, bool bPaintedThroughHost = false)
{
	WorldVoxelPaintPCG::FVoxelPaintPCGConsumingHost Descriptor;
	Descriptor.Host = Host;
	Descriptor.Boxes = Boxes;
	Descriptor.ParentLevelInstance = ParentLevelInstance;
	Descriptor.bInEditingLIHierarchy = bInEditingLIHierarchy;
	Descriptor.bHasSelfOnlyPCGComponent = bHasSelfOnlyPCGComponent;
	Descriptor.bPaintedThroughHost = bPaintedThroughHost;
	return Descriptor;
}

bool CallsEqual(const TArray<WorldVoxelPaintPCG::FVoxelPaintPCGRefreshCall>& A,
	const TArray<WorldVoxelPaintPCG::FVoxelPaintPCGRefreshCall>& B)
{
	if (A.Num() != B.Num())
	{
		return false;
	}
	for (int32 CallIndex = 0; CallIndex < A.Num(); ++CallIndex)
	{
		if (A[CallIndex].KeyHost != B[CallIndex].KeyHost
			|| A[CallIndex].Boxes.Num() != B[CallIndex].Boxes.Num())
		{
			return false;
		}
		for (int32 BoxIndex = 0; BoxIndex < A[CallIndex].Boxes.Num(); ++BoxIndex)
		{
			if (!(A[CallIndex].Boxes[BoxIndex] == B[CallIndex].Boxes[BoxIndex]))
			{
				return false;
			}
		}
	}
	return true;
}

} // namespace VoxelPaintPCGRefreshGroupingTestPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPCG_RefreshGroupingTopLevel,
	"Game.WorldVoxelPaint.Unit.PCG.RefreshGroupingTopLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPCG_RefreshGroupingTopLevel::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintPCGRefreshGroupingTestPrivate;
	using namespace WorldVoxelPaintPCG;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	AActor* TopA = SpawnHost(World, TEXT("WVPHostA"));
	AActor* TopB = SpawnHost(World, TEXT("WVPHostB"));
	AActor* Interior = SpawnHost(World, TEXT("WVPHostC"));
	AActor* ParentLI = SpawnHost(World, TEXT("WVPParentA"));
	if (!TestNotNull(TEXT("TopA spawned"), TopA) || !TestNotNull(TEXT("TopB spawned"), TopB)
		|| !TestNotNull(TEXT("Interior spawned"), Interior)
		|| !TestNotNull(TEXT("ParentLI spawned"), ParentLI))
	{
		return false;
	}

	const FBox BoxA = BoxAt(0.0);
	const FBox BoxB = BoxAt(1000.0);
	const FBox BoxC1 = BoxAt(2000.0);
	const FBox BoxC2 = BoxAt(3000.0);

	// Plan 7.2 typical commit: two top-level hosts plus a NON-editing LI-interior host all
	// coalesce into exactly one call keyed on a top-level host. Input deliberately unsorted.
	TArray<FVoxelPaintPCGConsumingHost> Hosts;
	Hosts.Add(MakeHost(Interior, { BoxC1, BoxC2 }, ParentLI, /*bInEditingLIHierarchy=*/false));
	Hosts.Add(MakeHost(TopB, { BoxB }));
	Hosts.Add(MakeHost(TopA, { BoxA }));

	TArray<FVoxelPaintPCGRefreshCall> Calls;
	GroupConsumingHostsForRefresh(Hosts, Calls);

	if (!TestEqual(TEXT("Typical commit produces exactly one call"), Calls.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("Key host is the stably-first top-level host"), Calls[0].KeyHost == TopA);
	if (!TestEqual(TEXT("Call carries every consuming box"), Calls[0].Boxes.Num(), 4))
	{
		return false;
	}
	// Box integrity: members in stable host order (WVPHostA < WVPHostB < WVPHostC), each
	// host's boxes in descriptor order.
	TestTrue(TEXT("Box 0 is TopA's"), Calls[0].Boxes[0] == BoxA);
	TestTrue(TEXT("Box 1 is TopB's"), Calls[0].Boxes[1] == BoxB);
	TestTrue(TEXT("Box 2 is Interior's first"), Calls[0].Boxes[2] == BoxC1);
	TestTrue(TEXT("Box 3 is Interior's second"), Calls[0].Boxes[3] == BoxC2);

	// Painted-through preference: a flagged TOP-LEVEL host takes the key; box set unchanged.
	for (FVoxelPaintPCGConsumingHost& Host : Hosts)
	{
		Host.bPaintedThroughHost = (Host.Host == TopB);
	}
	GroupConsumingHostsForRefresh(Hosts, Calls);
	TestTrue(TEXT("Painted-through top-level host takes the key"),
		Calls.Num() == 1 && Calls[0].KeyHost == TopB && Calls[0].Boxes.Num() == 4);

	// Painted-through on an LI-interior host is ignored: the coalesced key must be top-level.
	for (FVoxelPaintPCGConsumingHost& Host : Hosts)
	{
		Host.bPaintedThroughHost = (Host.Host == Interior);
	}
	GroupConsumingHostsForRefresh(Hosts, Calls);
	TestTrue(TEXT("LI-interior painted-through flag cannot take the key"),
		Calls.Num() == 1 && Calls[0].KeyHost == TopA);

	// Invalid boxes are dropped; the host still coalesces (and stays key-eligible).
	const FBox Invalid(EForceInit::ForceInit);
	for (FVoxelPaintPCGConsumingHost& Host : Hosts)
	{
		Host.bPaintedThroughHost = false;
		if (Host.Host == TopB)
		{
			Host.Boxes = { Invalid };
		}
	}
	GroupConsumingHostsForRefresh(Hosts, Calls);
	TestTrue(TEXT("Invalid boxes are dropped, host still grouped"),
		Calls.Num() == 1 && Calls[0].KeyHost == TopA && Calls[0].Boxes.Num() == 3);

	// Empty input -> no calls.
	GroupConsumingHostsForRefresh(TConstArrayView<FVoxelPaintPCGConsumingHost>(), Calls);
	TestEqual(TEXT("Empty input produces no calls"), Calls.Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPCG_RefreshGroupingLevelInstance,
	"Game.WorldVoxelPaint.Unit.PCG.RefreshGroupingLevelInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPCG_RefreshGroupingLevelInstance::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintPCGRefreshGroupingTestPrivate;
	using namespace WorldVoxelPaintPCG;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	AActor* EditA = SpawnHost(World, TEXT("WVPHostA"));
	AActor* EditB = SpawnHost(World, TEXT("WVPHostB"));
	AActor* EditC = SpawnHost(World, TEXT("WVPHostC"));
	AActor* Top = SpawnHost(World, TEXT("WVPHostT"));
	AActor* ParentA = SpawnHost(World, TEXT("WVPParentA"));
	AActor* ParentB = SpawnHost(World, TEXT("WVPParentB"));
	if (!TestNotNull(TEXT("EditA spawned"), EditA) || !TestNotNull(TEXT("EditB spawned"), EditB)
		|| !TestNotNull(TEXT("EditC spawned"), EditC) || !TestNotNull(TEXT("Top spawned"), Top)
		|| !TestNotNull(TEXT("ParentA spawned"), ParentA)
		|| !TestNotNull(TEXT("ParentB spawned"), ParentB))
	{
		return false;
	}

	const FBox BoxA = BoxAt(0.0);
	const FBox BoxB = BoxAt(1000.0);
	const FBox BoxC = BoxAt(2000.0);
	const FBox BoxT = BoxAt(3000.0);

	// Leg 1 - editing-LI hosts group by immediate parent LI while the top-level host keeps its
	// own coalesced call. Expected order: coalesced, then LI groups by parent stable order.
	{
		TArray<FVoxelPaintPCGConsumingHost> Hosts;
		Hosts.Add(MakeHost(EditC, { BoxC }, ParentB, /*bInEditingLIHierarchy=*/true));
		Hosts.Add(MakeHost(Top, { BoxT }));
		Hosts.Add(MakeHost(EditB, { BoxB }, ParentA, /*bInEditingLIHierarchy=*/true));
		Hosts.Add(MakeHost(EditA, { BoxA }, ParentA, /*bInEditingLIHierarchy=*/true));

		TArray<FVoxelPaintPCGRefreshCall> Calls;
		GroupConsumingHostsForRefresh(Hosts, Calls);

		if (!TestEqual(TEXT("Editing-LI commit: one coalesced + one call per parent LI"),
			Calls.Num(), 3))
		{
			return false;
		}
		TestTrue(TEXT("Call 0 is the coalesced top-level call"),
			Calls[0].KeyHost == Top && Calls[0].Boxes.Num() == 1 && Calls[0].Boxes[0] == BoxT);
		TestTrue(TEXT("Call 1 is ParentA's group keyed on its stably-first host"),
			Calls[1].KeyHost == EditA && Calls[1].Boxes.Num() == 2
				&& Calls[1].Boxes[0] == BoxA && Calls[1].Boxes[1] == BoxB);
		TestTrue(TEXT("Call 2 is ParentB's group"),
			Calls[2].KeyHost == EditC && Calls[2].Boxes.Num() == 1 && Calls[2].Boxes[0] == BoxC);
	}

	// Leg 2 - no-top-level fallback: with zero top-level hosts, ALL hosts (editing and
	// non-editing alike) group by immediate parent LI.
	{
		TArray<FVoxelPaintPCGConsumingHost> Hosts;
		Hosts.Add(MakeHost(EditA, { BoxA }, ParentA, /*bInEditingLIHierarchy=*/false));
		Hosts.Add(MakeHost(EditB, { BoxB }, ParentA, /*bInEditingLIHierarchy=*/true));
		Hosts.Add(MakeHost(EditC, { BoxC }, ParentB, /*bInEditingLIHierarchy=*/false));

		TArray<FVoxelPaintPCGRefreshCall> Calls;
		GroupConsumingHostsForRefresh(Hosts, Calls);

		if (!TestEqual(TEXT("No-top-level commit: one call per parent LI"), Calls.Num(), 2))
		{
			return false;
		}
		TestTrue(TEXT("Call 0 is ParentA's group (editing + non-editing members)"),
			Calls[0].KeyHost == EditA && Calls[0].Boxes.Num() == 2
				&& Calls[0].Boxes[0] == BoxA && Calls[0].Boxes[1] == BoxB);
		TestTrue(TEXT("Call 1 is ParentB's group"),
			Calls[1].KeyHost == EditC && Calls[1].Boxes.Num() == 1 && Calls[1].Boxes[0] == BoxC);
	}

	// Leg 3 - defensive normalization: an editing flag with no parent LI is treated top-level
	// (the engine cannot produce this combination; the function must not crash or mis-group).
	{
		TArray<FVoxelPaintPCGConsumingHost> Hosts;
		Hosts.Add(MakeHost(EditA, { BoxA }, /*ParentLevelInstance=*/nullptr,
			/*bInEditingLIHierarchy=*/true));

		TArray<FVoxelPaintPCGRefreshCall> Calls;
		GroupConsumingHostsForRefresh(Hosts, Calls);
		TestTrue(TEXT("Editing flag without a parent LI normalizes to top-level"),
			Calls.Num() == 1 && Calls[0].KeyHost == EditA && Calls[0].Boxes.Num() == 1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPCG_RefreshGroupingCarveOut,
	"Game.WorldVoxelPaint.Unit.PCG.RefreshGroupingCarveOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPCG_RefreshGroupingCarveOut::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintPCGRefreshGroupingTestPrivate;
	using namespace WorldVoxelPaintPCG;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	AActor* TopA = SpawnHost(World, TEXT("WVPHostA"));
	AActor* TopB = SpawnHost(World, TEXT("WVPHostB"));
	AActor* TopC = SpawnHost(World, TEXT("WVPHostC"));
	AActor* ParentA = SpawnHost(World, TEXT("WVPParentA"));
	if (!TestNotNull(TEXT("TopA spawned"), TopA) || !TestNotNull(TEXT("TopB spawned"), TopB)
		|| !TestNotNull(TEXT("TopC spawned"), TopC)
		|| !TestNotNull(TEXT("ParentA spawned"), ParentA))
	{
		return false;
	}

	const FBox BoxA = BoxAt(0.0);
	const FBox BoxB = BoxAt(1000.0);
	const FBox BoxC = BoxAt(2000.0);

	// Leg 1 - a self-only NON-key host gets one dedicated call carrying its own boxes; those
	// boxes also STAY in the group's call.
	{
		TArray<FVoxelPaintPCGConsumingHost> Hosts;
		Hosts.Add(MakeHost(TopA, { BoxA }));
		Hosts.Add(MakeHost(TopB, { BoxB }, nullptr, false, /*bHasSelfOnlyPCGComponent=*/true));

		TArray<FVoxelPaintPCGRefreshCall> Calls;
		GroupConsumingHostsForRefresh(Hosts, Calls);

		if (!TestEqual(TEXT("Group call plus one carve-out"), Calls.Num(), 2))
		{
			return false;
		}
		TestTrue(TEXT("Group call keeps every box incl. the carved host's"),
			Calls[0].KeyHost == TopA && Calls[0].Boxes.Num() == 2
				&& Calls[0].Boxes[0] == BoxA && Calls[0].Boxes[1] == BoxB);
		TestTrue(TEXT("Carve-out keyed on the self-only host with its own boxes"),
			Calls[1].KeyHost == TopB && Calls[1].Boxes.Num() == 1 && Calls[1].Boxes[0] == BoxB);
	}

	// Leg 2 - already-key no-op: when the self-only host IS the group's key, no dedicated call.
	{
		TArray<FVoxelPaintPCGConsumingHost> Hosts;
		Hosts.Add(MakeHost(TopB, { BoxB }, nullptr, false, /*bHasSelfOnlyPCGComponent=*/true));

		TArray<FVoxelPaintPCGRefreshCall> Calls;
		GroupConsumingHostsForRefresh(Hosts, Calls);
		TestTrue(TEXT("Self-only key host produces exactly one call, no carve-out"),
			Calls.Num() == 1 && Calls[0].KeyHost == TopB && Calls[0].Boxes.Num() == 1);
	}

	// Leg 3 - the carve-out applies inside LI groups too, with the same already-key no-op.
	{
		TArray<FVoxelPaintPCGConsumingHost> Hosts;
		Hosts.Add(MakeHost(TopA, { BoxA }, ParentA, /*bInEditingLIHierarchy=*/true));
		Hosts.Add(MakeHost(TopB, { BoxB }, ParentA, /*bInEditingLIHierarchy=*/true,
			/*bHasSelfOnlyPCGComponent=*/true));

		TArray<FVoxelPaintPCGRefreshCall> Calls;
		GroupConsumingHostsForRefresh(Hosts, Calls);
		if (!TestEqual(TEXT("LI group call plus one carve-out"), Calls.Num(), 2))
		{
			return false;
		}
		TestTrue(TEXT("LI group call keyed on the stably-first member"),
			Calls[0].KeyHost == TopA && Calls[0].Boxes.Num() == 2);
		TestTrue(TEXT("LI-group carve-out keyed on the self-only member"),
			Calls[1].KeyHost == TopB && Calls[1].Boxes.Num() == 1 && Calls[1].Boxes[0] == BoxB);

		// Flip the flag onto the key member instead: no carve-out at all.
		Hosts[0].bHasSelfOnlyPCGComponent = true;
		Hosts[1].bHasSelfOnlyPCGComponent = false;
		GroupConsumingHostsForRefresh(Hosts, Calls);
		TestTrue(TEXT("Self-only on the group key member is a no-op"),
			Calls.Num() == 1 && Calls[0].KeyHost == TopA);
	}

	// Leg 4 - multiple carve-outs emit after the group call, hosts in stable order.
	{
		TArray<FVoxelPaintPCGConsumingHost> Hosts;
		Hosts.Add(MakeHost(TopC, { BoxC }, nullptr, false, /*bHasSelfOnlyPCGComponent=*/true));
		Hosts.Add(MakeHost(TopA, { BoxA }));
		Hosts.Add(MakeHost(TopB, { BoxB }, nullptr, false, /*bHasSelfOnlyPCGComponent=*/true));

		TArray<FVoxelPaintPCGRefreshCall> Calls;
		GroupConsumingHostsForRefresh(Hosts, Calls);
		if (!TestEqual(TEXT("One group call plus two carve-outs"), Calls.Num(), 3))
		{
			return false;
		}
		TestTrue(TEXT("Group call first"), Calls[0].KeyHost == TopA && Calls[0].Boxes.Num() == 3);
		TestTrue(TEXT("Carve-outs in stable host order"),
			Calls[1].KeyHost == TopB && Calls[2].KeyHost == TopC);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPCG_RefreshGroupingDeterminism,
	"Game.WorldVoxelPaint.Unit.PCG.RefreshGroupingDeterminism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPCG_RefreshGroupingDeterminism::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintPCGRefreshGroupingTestPrivate;
	using namespace WorldVoxelPaintPCG;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	AActor* TopA = SpawnHost(World, TEXT("WVPHostA"));
	AActor* TopB = SpawnHost(World, TEXT("WVPHostB"));
	AActor* EditD = SpawnHost(World, TEXT("WVPHostD"));
	AActor* EditE = SpawnHost(World, TEXT("WVPHostE"));
	AActor* ParentA = SpawnHost(World, TEXT("WVPParentA"));
	if (!TestNotNull(TEXT("TopA spawned"), TopA) || !TestNotNull(TEXT("TopB spawned"), TopB)
		|| !TestNotNull(TEXT("EditD spawned"), EditD) || !TestNotNull(TEXT("EditE spawned"), EditE)
		|| !TestNotNull(TEXT("ParentA spawned"), ParentA))
	{
		return false;
	}

	const FBox BoxA = BoxAt(0.0);
	const FBox BoxB = BoxAt(1000.0);
	const FBox BoxD = BoxAt(2000.0);
	const FBox BoxE = BoxAt(3000.0);

	const FVoxelPaintPCGConsumingHost DescA = MakeHost(TopA, { BoxA });
	const FVoxelPaintPCGConsumingHost DescB = MakeHost(TopB, { BoxB });
	const FVoxelPaintPCGConsumingHost DescD = MakeHost(EditD, { BoxD }, ParentA, true);
	const FVoxelPaintPCGConsumingHost DescE = MakeHost(EditE, { BoxE }, ParentA, true,
		/*bHasSelfOnlyPCGComponent=*/true);

	const TArray<FVoxelPaintPCGConsumingHost> Forward = { DescB, DescE, DescA, DescD };
	const TArray<FVoxelPaintPCGConsumingHost> Reversed = { DescD, DescA, DescE, DescB };

	TArray<FVoxelPaintPCGRefreshCall> ForwardCalls;
	TArray<FVoxelPaintPCGRefreshCall> ReversedCalls;
	GroupConsumingHostsForRefresh(Forward, ForwardCalls);
	GroupConsumingHostsForRefresh(Reversed, ReversedCalls);

	TestTrue(TEXT("Output is invariant under input permutation"),
		CallsEqual(ForwardCalls, ReversedCalls));

	// Pin the full expected shape once: coalesced (key = stably-first top-level), LI group
	// (key = stably-first member), then the carve-out.
	if (!TestEqual(TEXT("Three calls total"), ForwardCalls.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("Coalesced call keyed on the stably-first top-level host"),
		ForwardCalls[0].KeyHost == TopA && ForwardCalls[0].Boxes.Num() == 2
			&& ForwardCalls[0].Boxes[0] == BoxA && ForwardCalls[0].Boxes[1] == BoxB);
	TestTrue(TEXT("LI group call keyed on its stably-first member"),
		ForwardCalls[1].KeyHost == EditD && ForwardCalls[1].Boxes.Num() == 2
			&& ForwardCalls[1].Boxes[0] == BoxD && ForwardCalls[1].Boxes[1] == BoxE);
	TestTrue(TEXT("Carve-out last, keyed on the self-only non-key host"),
		ForwardCalls[2].KeyHost == EditE && ForwardCalls[2].Boxes.Num() == 1
			&& ForwardCalls[2].Boxes[0] == BoxE);

	return true;
	// Teardown implicit: ~FVoxelPaintTestWorld destroys the world and all actors.
}
