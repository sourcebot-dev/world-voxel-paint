// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "VoxelPaintOwnershipTicket.h"
#include "VoxelPaintPublicationAssembler.h"
#include "VoxelPaintPublicationIdentity.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintResidency.h"
#include "WorldVoxelPaintConstants.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPublication_IdentityAdvancement,
	"Game.WorldVoxelPaint.Unit.Publication.IdentityAdvancement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPublication_IdentityAdvancement::RunTest(const FString& Parameters)
{
	FVoxelPaintPublicationIdentity Identity;
	FVoxelPaintPublicationIdentity Reference;
	TestTrue(TEXT("Fresh identities compare equal"), Identity == Reference);

	Identity.OnEpochAdvance(5);
	TestTrue(TEXT("Epoch advance seeds the triple"),
		Identity.OwnerEpoch == 5u && Identity.LayoutGeneration == 1u && Identity.BindingGeneration == 1u);
	TestTrue(TEXT("Advanced identity differs from the stale one"), Identity != Reference);

	Identity.OnRegistryLayoutChange();
	TestTrue(TEXT("Registry layout change advances layout only"),
		Identity.LayoutGeneration == 2u && Identity.BindingGeneration == 1u);
	Identity.OnGuardConsumed();
	TestTrue(TEXT("Guard consumption advances layout only"),
		Identity.LayoutGeneration == 3u && Identity.BindingGeneration == 1u);
	Identity.OnZEnvelopeOrSettingsChange();
	TestTrue(TEXT("Z-envelope/settings change advances layout only"),
		Identity.LayoutGeneration == 4u && Identity.BindingGeneration == 1u);

	Identity.OnRegistryBindingChange();
	TestTrue(TEXT("Registry binding change advances binding only"),
		Identity.LayoutGeneration == 4u && Identity.BindingGeneration == 2u);
	Identity.OnResidencyBindingEvent();
	TestTrue(TEXT("Residency binding event advances binding only"),
		Identity.LayoutGeneration == 4u && Identity.BindingGeneration == 3u);
	TestTrue(TEXT("Epoch untouched by layout/binding advances"), Identity.OwnerEpoch == 5u);

	Identity.OnEpochAdvance(6);
	TestTrue(TEXT("Epoch advance resets layout and binding to 1"),
		Identity.OwnerEpoch == 6u && Identity.LayoutGeneration == 1u && Identity.BindingGeneration == 1u);

	Reference.OnEpochAdvance(6);
	TestTrue(TEXT("Equal triples compare equal (dedup comparison)"), Identity == Reference);
	Reference.OnResidencyBindingEvent();
	TestTrue(TEXT("A single component difference breaks equality"), Identity != Reference);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintOwnership_EpochTicketStaleness,
	"Game.WorldVoxelPaint.Unit.Ownership.EpochTicketStaleness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintOwnership_EpochTicketStaleness::RunTest(const FString& Parameters)
{
	FVoxelPaintOwnershipTicket MasterTicket;
	MasterTicket.OwnerEpoch = 7;
	MasterTicket.Verdict = EVoxelPaintFamilyVerdict::MasterPrimary;

	TestTrue(TEXT("Ticket epoch matching the fenced epoch renders as master"),
		WorldVoxelPaint::ShouldRenderAsMaster(&MasterTicket, 7));
	TestFalse(TEXT("Ticket older than the fenced epoch is stale-rejected"),
		WorldVoxelPaint::ShouldRenderAsMaster(&MasterTicket, 8));
	TestFalse(TEXT("Missing ticket is neutral"),
		WorldVoxelPaint::ShouldRenderAsMaster(nullptr, 7));

	FVoxelPaintOwnershipTicket NeutralTicket;
	NeutralTicket.OwnerEpoch = 8;
	NeutralTicket.Verdict = EVoxelPaintFamilyVerdict::Neutral;
	TestFalse(TEXT("Neutral verdict is neutral even at the current epoch"),
		WorldVoxelPaint::ShouldRenderAsMaster(&NeutralTicket, 8));

	FVoxelPaintOwnershipTicket ZeroTicket;
	TestFalse(TEXT("Default ticket (epoch 0, Neutral) is neutral"),
		WorldVoxelPaint::ShouldRenderAsMaster(&ZeroTicket, 0));

	TestTrue(TEXT("Subclass identifier resolves by pointer identity"),
		MasterTicket.GetSubclassIdentifier() == FVoxelPaintOwnershipTicket::GSubclassIdentifier);

	return true;
}

namespace VoxelPaintAssembleBindingsTestPrivate
{
	static FVoxelPaintCanonicalIdentity MakeIdentity(uint8 Seed)
	{
		FVoxelPaintCanonicalIdentity Identity;
		FMemory::Memset(Identity.Hash.Hash, Seed, sizeof(Identity.Hash.Hash));
		return Identity;
	}

	static FVoxelPaintDesiredAsset MakeDesired(const FVoxelPaintAssetKey& Key, uint8 IdentitySeed,
		const FIntVector& BrickCounts, const FString& DebugName)
	{
		TSharedPtr<FVoxelPaintSourceManifest> Manifest = MakeShared<FVoxelPaintSourceManifest>();
		Manifest->BrickCounts = BrickCounts;
		const int32 Total = BrickCounts.X * BrickCounts.Y * BrickCounts.Z;
		for (int32 Index = 0; Index < Total; ++Index)
		{
			Manifest->PopulatedBricks.Add((uint32)Index);
		}
		FVoxelPaintDesiredAsset Desired;
		Desired.Key = Key;
		Desired.SourceGeneration = 1;
		Desired.Identity = MakeIdentity(IdentitySeed);
		Desired.Manifest = Manifest;
		Desired.DebugName = DebugName;
		return Desired;
	}

	static void PrepareAndComplete(FVoxelPaintResidencyManager& Manager, const FVoxelPaintDesiredAsset& Desired)
	{
		Manager.OnPreparationLaunched(Desired.Key, Desired.SourceGeneration, 1);
		FVoxelPaintPreparedPayload Payload;
		Payload.AssetKey = Desired.Key;
		Payload.SourceGeneration = Desired.SourceGeneration;
		Payload.LaunchOwnerEpoch = 1;
		Payload.Identity = Desired.Identity;
		Payload.Manifest = Desired.Manifest;
		TSharedPtr<TArray<uint8>> Bytes = MakeShared<TArray<uint8>>();
		Bytes->SetNumZeroed(Desired.Manifest->PopulatedBricks.Num() * WorldVoxelPaint::BrickPayloadBytes);
		Payload.BrickPayload = Bytes;
		Payload.bPayloadValid = true;
		Manager.OnPreparationCompleted(MoveTemp(Payload));
	}

	static FVoxelPaintPlacementState MakePlacement(const FVoxelPaintDesiredAsset& Desired)
	{
		FVoxelPaintPlacementState State;
		State.Flags = WorldVoxelPaint::RecordFlag_Valid;
		State.AssetKey = Desired.Key;
		State.SourceGeneration = Desired.SourceGeneration;
		State.BrickCounts = Desired.Manifest->BrickCounts;
		return State;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPublication_AssembleBindings,
	"Game.WorldVoxelPaint.Unit.Publication.AssembleBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPublication_AssembleBindings::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintAssembleBindingsTestPrivate;

	const FVoxelPaintAssetKey KeyA(UObject::StaticClass());
	const FVoxelPaintAssetKey KeyB(UClass::StaticClass());
	const FVoxelPaintDesiredAsset AssetA = MakeDesired(KeyA, 1, FIntVector(4, 1, 1), TEXT("/WVP/Fixtures/A"));
	const FVoxelPaintDesiredAsset AssetB = MakeDesired(KeyB, 2, FIntVector(5, 1, 1), TEXT("/WVP/Fixtures/B"));

	FVoxelPaintResidencyManager Manager;
	Manager.Initialize(16, 64);

	TArray<FVoxelPaintDesiredAsset> Desired;
	Desired.Add(AssetA);
	Desired.Add(AssetB);
	Manager.SetDesiredAssets(1, Desired);
	PrepareAndComplete(Manager, AssetA);
	PrepareAndComplete(Manager, AssetB);

	FVoxelPaintIntegrationBudget Budget;
	Budget.MaxUploadBytes = int64(1) << 40;
	Budget.MaxPageEntries = MAX_int32;
	FVoxelPaintIntegrationOps Ops;
	bool bBindingsChanged = false;
	Manager.IntegrateStep(1, 1, Budget, Ops, bBindingsChanged);
	TestTrue(TEXT("Asset A published"), Manager.GetAssetState(KeyA) == EVoxelPaintAssetResidencyState::Published);
	TestTrue(TEXT("Asset B published"), Manager.GetAssetState(KeyB) == EVoxelPaintAssetResidencyState::Published);

	TArray<FVoxelPaintPlacementState> Placements;
	Placements.Add(MakePlacement(AssetA));
	Placements.Add(MakePlacement(AssetB));
	FVoxelPaintPlacementState& Invalid = Placements.AddDefaulted_GetRef();
	Invalid.Flags = 0;

	TArray<FVoxelPaintVolumeRecord> Records;
	WorldVoxelPaint::FVoxelPaintAssemblyStats Stats;
	WorldVoxelPaint::AssembleRecords(Placements, Manager, FVector::ZeroVector, Records, Stats, Desired);

	TestTrue(TEXT("One record per placement"), Records.Num() == 3);
	TestTrue(TEXT("Side-table identity resolves A to a Data binding at the first-fit range offset"),
		Records[0].PageTableBaseIndex == 0u);
	TestTrue(TEXT("B's Data binding packs the real range offset behind A's table"),
		Records[1].PageTableBaseIndex == 4u);
	TestTrue(TEXT("Invalid placement packs the sentinel"),
		Records[2].PageTableBaseIndex == WorldVoxelPaint::NoPageTableSentinel);
	TestTrue(TEXT("Assembly stats: two Data, one invalid, no neutral proxies"),
		Stats.NumData == 2 && Stats.NumInvalid == 1 && Stats.NumNeutralProxy == 0 && Stats.NumNoTable == 0);

	AddExpectedMessage(TEXT("AssembleRecords called without the desired-asset side table"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1, false);
	WorldVoxelPaint::AssembleRecords(Placements, Manager, FVector::ZeroVector, Records, Stats);
	TestTrue(TEXT("Omitted side table resolves every asset-bearing placement to the sentinel"),
		Records[0].PageTableBaseIndex == WorldVoxelPaint::NoPageTableSentinel
		&& Records[1].PageTableBaseIndex == WorldVoxelPaint::NoPageTableSentinel);
	TestTrue(TEXT("Omitted side table counts neutral proxies, never Data"),
		Stats.NumData == 0 && Stats.NumNeutralProxy == 2 && Stats.NumInvalid == 1);

	return true;
}
