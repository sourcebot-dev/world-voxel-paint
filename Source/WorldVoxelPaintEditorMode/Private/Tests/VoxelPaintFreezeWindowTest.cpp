// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "VoxelPaintAuthority.h"
#include "VoxelPaintDetailLadder.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintSessionMachine.h"
#include "VoxelPaintTopologyOps.h"
#include "VoxelPaintVolumeData.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintSession_FreezeWindow,
	"Game.WorldVoxelPaint.Unit.Session.FreezeWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintSession_FreezeWindow::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	FVoxelPaintSessionController& Controller = FVoxelPaintSessionController::Get();
	ON_SCOPE_EXIT
	{
		Controller.SetTestSessionStateOverride(TOptional<FVoxelPaintTestSessionState>());
	};

	UVoxelPaintVolumeData* Asset = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	UVoxelPaintVolumeData* OtherAsset = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	if (!TestNotNull(TEXT("Transient assets created"), Asset)
		|| !TestNotNull(TEXT("Second transient asset created"), OtherAsset))
	{
		return false;
	}
	MarkShared(Asset);
	TestFalse(TEXT("Precondition: no live session is open in the headless run"),
		Controller.IsSessionOpen());

	FVoxelPaintLadderState BadLadder;
	BadLadder.HalfExtent = FVector(-1.0);

	const EVoxelPaintSessionStage AllStages[] =
	{
		EVoxelPaintSessionStage::Closed,
		EVoxelPaintSessionStage::Active,
		EVoxelPaintSessionStage::Sealing,
		EVoxelPaintSessionStage::Canonicalizing,
		EVoxelPaintSessionStage::Committed,
		EVoxelPaintSessionStage::Deriving,
		EVoxelPaintSessionStage::Publication,
		EVoxelPaintSessionStage::Retirement,
		EVoxelPaintSessionStage::DerivedFailed
	};
	static_assert(UE_ARRAY_COUNT(AllStages) == 9, "The freeze test must drive all nine stages");

	for (EVoxelPaintSessionStage Stage : AllStages)
	{
		const FString StageName = LexToString(Stage);
		const bool bExpectFrozen = Stage == EVoxelPaintSessionStage::Active
			|| Stage == EVoxelPaintSessionStage::Sealing
			|| Stage == EVoxelPaintSessionStage::Canonicalizing
			|| Stage == EVoxelPaintSessionStage::Committed;

		FVoxelPaintTestSessionState Injected;
		Injected.Asset = Asset;
		Injected.Stage = Stage;
		Controller.SetTestSessionStateOverride(Injected);

		TestTrue(FString::Printf(TEXT("%s: GetStage reads the injected stage"), *StageName),
			Controller.GetStage() == Stage);
		TestTrue(FString::Printf(TEXT("%s: GetStageForAsset reads the injected stage"), *StageName),
			Controller.GetStageForAsset(Asset) == Stage);
		TestTrue(FString::Printf(TEXT("%s: GetStageForAsset reads Closed for another asset"), *StageName),
			Controller.GetStageForAsset(OtherAsset) == EVoxelPaintSessionStage::Closed);
		TestEqual(FString::Printf(TEXT("%s: IsSessionOpen tracks the injected stage"), *StageName),
			Controller.IsSessionOpen(), Stage != EVoxelPaintSessionStage::Closed);
		TestEqual(FString::Printf(TEXT("%s: the open-session asset query tracks the override"), *StageName),
			IsAssetInOpenSession(Asset), Stage != EVoxelPaintSessionStage::Closed);
		TestFalse(FString::Printf(TEXT("%s: the open-session asset query stays false for another asset"), *StageName),
			IsAssetInOpenSession(OtherAsset));

		TestEqual(FString::Printf(TEXT("%s: IsAssetFrozen matches the Active..Committed window"), *StageName),
			Controller.IsAssetFrozen(Asset), bExpectFrozen);
		TestFalse(FString::Printf(TEXT("%s: another asset is never frozen"), *StageName),
			Controller.IsAssetFrozen(OtherAsset));

		FText GateWhyNot;
		TestEqual(FString::Printf(TEXT("%s: GateAuthorityTransition is false only in-window"), *StageName),
			Controller.GateAuthorityTransition(Asset, GateWhyNot), !bExpectFrozen);
		if (bExpectFrozen)
		{
			const FString GateText = GateWhyNot.ToString();
			TestTrue(FString::Printf(TEXT("%s: the gate remedy text says Finish or Discard"), *StageName),
				GateText.Contains(TEXT("Finish or Discard")));
			TestTrue(FString::Printf(TEXT("%s: the gate remedy text names the asset"), *StageName),
				GateText.Contains(Asset->GetName()));
			TestTrue(FString::Printf(TEXT("%s: the gate remedy text names the stage"), *StageName),
				GateText.Contains(StageName));
		}
		FText OtherWhyNot;
		TestTrue(FString::Printf(TEXT("%s: GateAuthorityTransition passes for another asset"), *StageName),
			Controller.GateAuthorityTransition(OtherAsset, OtherWhyNot));

		FText MutateWhyNot;
		TestFalse(FString::Printf(TEXT("%s: CanMutateNow is false headlessly"), *StageName),
			Controller.CanMutateNow(MutateWhyNot));
		if (Stage != EVoxelPaintSessionStage::Active)
		{
			const FString MutateText = MutateWhyNot.ToString();
			TestTrue(FString::Printf(TEXT("%s: the mutate denial names the injected stage"), *StageName),
				MutateText.Contains(TEXT("No active paint session"))
					&& MutateText.Contains(StageName));
		}

		FText ResetError;
		TestFalse(FString::Printf(TEXT("%s: ResetDataAndUnlockSize is rejected"), *StageName),
			ResetDataAndUnlockSize(Asset, nullptr, ResetError));
		FText TopologyError;
		TestFalse(FString::Printf(TEXT("%s: CommitPreInitTopologyEdit is rejected"), *StageName),
			CommitPreInitTopologyEdit(Asset, nullptr, nullptr, BadLadder, TopologyError));
		if (Stage != EVoxelPaintSessionStage::Closed)
		{
			TestTrue(FString::Printf(TEXT("%s: Reset rejects on the open session"), *StageName),
				ResetError.ToString().Contains(TEXT("a paint session is open")));
			TestTrue(FString::Printf(TEXT("%s: topology edit rejects on the open session"), *StageName),
				TopologyError.ToString().Contains(TEXT("a paint session is open")));
		}
		else
		{
			TestTrue(TEXT("Closed: Reset passes the session gate and rejects downstream (uninitialized)"),
				ResetError.ToString().Contains(TEXT("is not initialized"))
					&& !ResetError.ToString().Contains(TEXT("a paint session is open")));
			TestTrue(TEXT("Closed: topology edit passes the session gate and rejects downstream (ladder)"),
				TopologyError.ToString().Contains(TEXT("Topology edit rejected"))
					&& !TopologyError.ToString().Contains(TEXT("a paint session is open")));
		}
	}

	Controller.SetTestSessionStateOverride(TOptional<FVoxelPaintTestSessionState>());

	TestTrue(TEXT("Cleared: GetStage reverts to the live machine (Closed)"),
		Controller.GetStage() == EVoxelPaintSessionStage::Closed);
	TestFalse(TEXT("Cleared: IsSessionOpen reverts to the live machine"), Controller.IsSessionOpen());
	TestTrue(TEXT("Cleared: GetStageForAsset reverts to Closed"),
		Controller.GetStageForAsset(Asset) == EVoxelPaintSessionStage::Closed);
	TestFalse(TEXT("Cleared: the freeze window is inactive"), Controller.IsAssetFrozen(Asset));
	TestFalse(TEXT("Cleared: the open-session asset query reverts"), IsAssetInOpenSession(Asset));
	FText ClearedWhyNot;
	TestTrue(TEXT("Cleared: GateAuthorityTransition passes"),
		Controller.GateAuthorityTransition(Asset, ClearedWhyNot));
	FText ClearedResetError;
	TestFalse(TEXT("Cleared: Reset still rejects downstream, not on a session"),
		ResetDataAndUnlockSize(Asset, nullptr, ClearedResetError));
	TestTrue(TEXT("Cleared: the Reset rejection is the uninitialized one"),
		ClearedResetError.ToString().Contains(TEXT("is not initialized")));

	return true;
}
