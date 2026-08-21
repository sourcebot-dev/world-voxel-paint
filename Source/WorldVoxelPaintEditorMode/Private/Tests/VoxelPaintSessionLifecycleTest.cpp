// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintAuthority.h"
#include "VoxelPaintGridReadiness.h"
#include "VoxelPaintSessionMachine.h"
#include "VoxelPaintTopologyLock.h"

namespace VoxelPaintSessionLifecycleTestPrivate
{
	using namespace WorldVoxelPaint;

	static FVoxelPaintSessionAdmissionInputs MakeAdmittedInputs()
	{
		FVoxelPaintSessionAdmissionInputs Inputs;
		Inputs.bAnotherSessionOpen = false;
		Inputs.bWorldIsMaster = true;
		Inputs.bEligiblePrimary = true;
		Inputs.GridReadiness = (uint8)EVoxelPaintGridReadiness::Ready;
		Inputs.bPIEActiveOrQueued = false;
		Inputs.AuthorityVerdict = (uint8)EVoxelPaintAuthorityVerdict::Allowed;
		Inputs.bAssetValid = true;
		return Inputs;
	}

	static FVoxelPaintSessionMachine DriveTo(EVoxelPaintSessionStage Stage, bool bAssetWasInitialized = false)
	{
		FVoxelPaintSessionMachine Machine;
		if (Stage == EVoxelPaintSessionStage::Closed)
		{
			return Machine;
		}
		Machine.Open(MakeAdmittedInputs(), bAssetWasInitialized);
		if (Stage == EVoxelPaintSessionStage::Active)
		{
			return Machine;
		}
		Machine.RequestFinish();
		if (Stage == EVoxelPaintSessionStage::Sealing)
		{
			return Machine;
		}
		Machine.OnSealed();
		if (Stage == EVoxelPaintSessionStage::Canonicalizing)
		{
			return Machine;
		}
		Machine.OnCommitInstalled();
		if (Stage == EVoxelPaintSessionStage::Committed)
		{
			return Machine;
		}
		Machine.OnDerivingStarted();
		if (Stage == EVoxelPaintSessionStage::Deriving)
		{
			return Machine;
		}
		if (Stage == EVoxelPaintSessionStage::DerivedFailed)
		{
			Machine.OnDerivedFailed();
			return Machine;
		}
		Machine.OnMatchingGenerationPublished();
		if (Stage == EVoxelPaintSessionStage::Publication)
		{
			return Machine;
		}
		Machine.OnPublicationRetiring();
		return Machine;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintSession_LifecycleMachine,
	"Game.WorldVoxelPaint.Unit.Session.LifecycleMachine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintSession_LifecycleMachine::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;
	using namespace VoxelPaintSessionLifecycleTestPrivate;

	{
		TestTrue(TEXT("All-good inputs admit"),
			FVoxelPaintSessionMachine::JudgeAdmission(MakeAdmittedInputs())
				== EVoxelPaintSessionAdmissionVerdict::Admitted);

		FVoxelPaintSessionAdmissionInputs Inputs = MakeAdmittedInputs();
		Inputs.bAnotherSessionOpen = true;
		TestTrue(TEXT("Busy session denies"),
			FVoxelPaintSessionMachine::JudgeAdmission(Inputs) == EVoxelPaintSessionAdmissionVerdict::DeniedSessionBusy);

		Inputs = MakeAdmittedInputs();
		Inputs.bWorldIsMaster = false;
		TestTrue(TEXT("Non-master world denies"),
			FVoxelPaintSessionMachine::JudgeAdmission(Inputs) == EVoxelPaintSessionAdmissionVerdict::DeniedNotMaster);

		Inputs = MakeAdmittedInputs();
		Inputs.bEligiblePrimary = false;
		TestTrue(TEXT("No eligible primary denies"),
			FVoxelPaintSessionMachine::JudgeAdmission(Inputs)
				== EVoxelPaintSessionAdmissionVerdict::DeniedNoEligiblePrimary);

		Inputs = MakeAdmittedInputs();
		Inputs.GridReadiness = (uint8)EVoxelPaintGridReadiness::Pending;
		TestTrue(TEXT("Pending grid denies"),
			FVoxelPaintSessionMachine::JudgeAdmission(Inputs) == EVoxelPaintSessionAdmissionVerdict::DeniedGridNotReady);
		Inputs.GridReadiness = (uint8)EVoxelPaintGridReadiness::Failed;
		TestTrue(TEXT("Failed grid denies"),
			FVoxelPaintSessionMachine::JudgeAdmission(Inputs) == EVoxelPaintSessionAdmissionVerdict::DeniedGridNotReady);

		Inputs = MakeAdmittedInputs();
		Inputs.bPIEActiveOrQueued = true;
		TestTrue(TEXT("PIE active or queued denies"),
			FVoxelPaintSessionMachine::JudgeAdmission(Inputs)
				== EVoxelPaintSessionAdmissionVerdict::DeniedPIEActiveOrQueued);

		Inputs = MakeAdmittedInputs();
		Inputs.AuthorityVerdict = (uint8)EVoxelPaintAuthorityVerdict::DeniedNotOwner;
		TestTrue(TEXT("Not-owner authority denies"),
			FVoxelPaintSessionMachine::JudgeAdmission(Inputs) == EVoxelPaintSessionAdmissionVerdict::DeniedAuthority);
		Inputs.AuthorityVerdict = (uint8)EVoxelPaintAuthorityVerdict::DeniedOwnerless;
		TestTrue(TEXT("Ownerless authority denies"),
			FVoxelPaintSessionMachine::JudgeAdmission(Inputs) == EVoxelPaintSessionAdmissionVerdict::DeniedAuthority);

		Inputs = MakeAdmittedInputs();
		Inputs.bAssetValid = false;
		TestTrue(TEXT("Invalid asset denies"),
			FVoxelPaintSessionMachine::JudgeAdmission(Inputs) == EVoxelPaintSessionAdmissionVerdict::DeniedInvalidAsset);
	}

	{
		FVoxelPaintSessionMachine Machine;
		TestTrue(TEXT("Fresh machine is Closed and PIE-admissible"),
			Machine.GetStage() == EVoxelPaintSessionStage::Closed && Machine.IsClosedForPIE());

		FVoxelPaintSessionAdmissionInputs Denied = MakeAdmittedInputs();
		Denied.bWorldIsMaster = false;
		TestFalse(TEXT("Open on denied inputs fails"), Machine.Open(Denied, false));
		TestTrue(TEXT("Denied Open leaves Closed"), Machine.GetStage() == EVoxelPaintSessionStage::Closed);

		TestTrue(TEXT("Open on admitted inputs succeeds"), Machine.Open(MakeAdmittedInputs(), false));
		TestTrue(TEXT("Open lands in Active"), Machine.GetStage() == EVoxelPaintSessionStage::Active);
		TestTrue(TEXT("Active is pre-commit"), Machine.IsPreCommit());
		TestFalse(TEXT("Active is not PIE-admissible"), Machine.IsClosedForPIE());
		TestTrue(TEXT("Uninitialized asset makes a first session"), Machine.WasFirstSession());
		TestFalse(TEXT("Fresh session has never committed"), Machine.HasEverCommitted());
		TestFalse(TEXT("Open while not Closed fails"), Machine.Open(MakeAdmittedInputs(), false));

		const uint8 Ready = (uint8)EVoxelPaintGridReadiness::Ready;
		TestTrue(TEXT("Mutation gate passes with all three true in Active"), Machine.CanMutate(true, Ready, true));
		TestFalse(TEXT("Mutation gate fails off-origin-master"), Machine.CanMutate(false, Ready, true));
		TestFalse(TEXT("Mutation gate fails on Pending grid"),
			Machine.CanMutate(true, (uint8)EVoxelPaintGridReadiness::Pending, true));
		TestFalse(TEXT("Mutation gate fails on Failed grid"),
			Machine.CanMutate(true, (uint8)EVoxelPaintGridReadiness::Failed, true));
		TestFalse(TEXT("Mutation gate fails with unresolved prior close"), Machine.CanMutate(true, Ready, false));

		TestTrue(TEXT("Finish requests Sealing"), Machine.RequestFinish());
		TestTrue(TEXT("Sealing stage"), Machine.GetStage() == EVoxelPaintSessionStage::Sealing);
		TestTrue(TEXT("Sealing is pre-commit"), Machine.IsPreCommit());
		TestFalse(TEXT("Mutation gate fails in Sealing"), Machine.CanMutate(true, Ready, true));

		TestTrue(TEXT("Sealing cancels back to Active"), Machine.CancelSealing());
		TestTrue(TEXT("Cancel lands in Active"), Machine.GetStage() == EVoxelPaintSessionStage::Active);

		Machine.RequestFinish();
		TestTrue(TEXT("Sealed advances to Canonicalizing"), Machine.OnSealed());
		TestTrue(TEXT("Canonicalizing stage"), Machine.GetStage() == EVoxelPaintSessionStage::Canonicalizing);
		TestTrue(TEXT("Canonicalizing is pre-commit"), Machine.IsPreCommit());
		TestFalse(TEXT("Mutation gate fails in Canonicalizing"), Machine.CanMutate(true, Ready, true));

		TestTrue(TEXT("Canonicalize failure returns to Active"), Machine.OnCanonicalizeFailed());
		TestTrue(TEXT("Failure lands in Active"), Machine.GetStage() == EVoxelPaintSessionStage::Active);
		TestFalse(TEXT("Canonicalize failure commits nothing"), Machine.HasEverCommitted());
		TestTrue(TEXT("Canonicalize failure keeps first-session flag"), Machine.WasFirstSession());

		Machine.RequestFinish();
		Machine.OnSealed();
		TestTrue(TEXT("Commit preflight failure returns to Active"), Machine.OnCommitPreflightFailed());
		TestTrue(TEXT("Preflight failure lands in Active"), Machine.GetStage() == EVoxelPaintSessionStage::Active);
		TestFalse(TEXT("Preflight failure commits nothing"), Machine.HasEverCommitted());

		Machine.RequestFinish();
		Machine.OnSealed();
		TestTrue(TEXT("Commit installs"), Machine.OnCommitInstalled());
		TestTrue(TEXT("Committed stage"), Machine.GetStage() == EVoxelPaintSessionStage::Committed);
		TestFalse(TEXT("Committed is not pre-commit"), Machine.IsPreCommit());
		TestTrue(TEXT("Commit sets HasEverCommitted"), Machine.HasEverCommitted());

		TestTrue(TEXT("Deriving starts"), Machine.OnDerivingStarted());
		TestTrue(TEXT("Deriving stage"), Machine.GetStage() == EVoxelPaintSessionStage::Deriving);
		TestTrue(TEXT("Matching generation publishes"), Machine.OnMatchingGenerationPublished());
		TestTrue(TEXT("Publication stage"), Machine.GetStage() == EVoxelPaintSessionStage::Publication);
		TestTrue(TEXT("Publication retires"), Machine.OnPublicationRetiring());
		TestTrue(TEXT("Retirement stage"), Machine.GetStage() == EVoxelPaintSessionStage::Retirement);
		TestTrue(TEXT("Retirement completes"), Machine.OnRetirementComplete());
		TestTrue(TEXT("Walk ends Closed"), Machine.GetStage() == EVoxelPaintSessionStage::Closed);
		TestTrue(TEXT("Closed is PIE-admissible again"), Machine.IsClosedForPIE());
	}

	{
		FVoxelPaintSessionMachine Machine = DriveTo(EVoxelPaintSessionStage::Publication);
		TestTrue(TEXT("Retirement completes straight from Publication"), Machine.OnRetirementComplete());
		TestTrue(TEXT("Direct completion lands Closed"), Machine.GetStage() == EVoxelPaintSessionStage::Closed);
	}

	{
		FVoxelPaintSessionMachine Machine = DriveTo(EVoxelPaintSessionStage::Publication);
		TestTrue(TEXT("Publication can fail derived"), Machine.OnDerivedFailed());
		TestTrue(TEXT("Publication failure lands DerivedFailed"),
			Machine.GetStage() == EVoxelPaintSessionStage::DerivedFailed);
	}

	{
		FVoxelPaintSessionMachine Machine = DriveTo(EVoxelPaintSessionStage::DerivedFailed);
		TestTrue(TEXT("Deriving failure lands DerivedFailed"),
			Machine.GetStage() == EVoxelPaintSessionStage::DerivedFailed);

		TestFalse(TEXT("DerivedFailed rejects Open"), Machine.Open(MakeAdmittedInputs(), false));
		TestFalse(TEXT("DerivedFailed rejects RequestFinish"), Machine.RequestFinish());
		TestFalse(TEXT("DerivedFailed rejects CancelSealing"), Machine.CancelSealing());
		TestFalse(TEXT("DerivedFailed rejects OnSealed"), Machine.OnSealed());
		TestFalse(TEXT("DerivedFailed rejects OnCanonicalizeFailed"), Machine.OnCanonicalizeFailed());
		TestFalse(TEXT("DerivedFailed rejects OnCommitInstalled"), Machine.OnCommitInstalled());
		TestFalse(TEXT("DerivedFailed rejects OnCommitPreflightFailed"), Machine.OnCommitPreflightFailed());
		TestFalse(TEXT("DerivedFailed rejects OnDerivingStarted"), Machine.OnDerivingStarted());
		TestFalse(TEXT("DerivedFailed rejects OnMatchingGenerationPublished"), Machine.OnMatchingGenerationPublished());
		TestFalse(TEXT("DerivedFailed rejects OnPublicationRetiring"), Machine.OnPublicationRetiring());
		TestFalse(TEXT("DerivedFailed rejects OnRetirementComplete"), Machine.OnRetirementComplete());
		TestFalse(TEXT("DerivedFailed rejects OnDerivedFailed"), Machine.OnDerivedFailed());
		TestFalse(TEXT("DerivedFailed rejects RequestDiscardConfirmed"), Machine.RequestDiscardConfirmed());
		TestTrue(TEXT("Illegal verbs leave DerivedFailed untouched"),
			Machine.GetStage() == EVoxelPaintSessionStage::DerivedFailed);

		TestTrue(TEXT("Retry re-enters Deriving"), Machine.RequestRetry());
		TestTrue(TEXT("Retry lands in Deriving"), Machine.GetStage() == EVoxelPaintSessionStage::Deriving);
		TestTrue(TEXT("Deriving can fail again"), Machine.OnDerivedFailed());
		TestTrue(TEXT("End Anyway enters Retirement"), Machine.RequestEndAnyway());
		TestTrue(TEXT("End Anyway lands in Retirement"), Machine.GetStage() == EVoxelPaintSessionStage::Retirement);
		TestTrue(TEXT("End Anyway retirement completes to Closed"), Machine.OnRetirementComplete());
		TestTrue(TEXT("End Anyway ends Closed"), Machine.GetStage() == EVoxelPaintSessionStage::Closed);
	}

	{
		const EVoxelPaintSessionStage PostCommitStages[] =
		{
			EVoxelPaintSessionStage::Committed,
			EVoxelPaintSessionStage::Deriving,
			EVoxelPaintSessionStage::Publication,
			EVoxelPaintSessionStage::Retirement,
			EVoxelPaintSessionStage::DerivedFailed
		};
		for (EVoxelPaintSessionStage Stage : PostCommitStages)
		{
			FVoxelPaintSessionMachine Machine = DriveTo(Stage);
			TestTrue(TEXT("DriveTo reached the post-commit stage"), Machine.GetStage() == Stage);
			TestFalse(TEXT("One-way commit: CancelSealing cannot reach Active"), Machine.CancelSealing());
			TestFalse(TEXT("One-way commit: OnCanonicalizeFailed cannot reach Active"), Machine.OnCanonicalizeFailed());
			TestFalse(TEXT("One-way commit: OnCommitPreflightFailed cannot reach Active"),
				Machine.OnCommitPreflightFailed());
			TestFalse(TEXT("One-way commit: RequestDiscardConfirmed cannot reach Active"),
				Machine.RequestDiscardConfirmed());
			TestFalse(TEXT("One-way commit: RequestFinish is rejected"), Machine.RequestFinish());
			TestTrue(TEXT("One-way commit: stage unchanged"), Machine.GetStage() == Stage);
			TestFalse(TEXT("One-way commit: stage never reads Active"),
				Machine.GetStage() == EVoxelPaintSessionStage::Active);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintSession_LifecycleExits,
	"Game.WorldVoxelPaint.Unit.Session.LifecycleExits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintSession_LifecycleExits::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;
	using namespace VoxelPaintSessionLifecycleTestPrivate;

	{
		FVoxelPaintSessionMachine Machine;
		FVoxelPaintTopologyLockMachine Topology;
		Topology.Reset(false);

		TestTrue(TEXT("Auto-Finish: first session opens"), Machine.Open(MakeAdmittedInputs(), false));
		Topology.OnOpAdmitted(true);
		TestTrue(TEXT("Auto-Finish: admitted write intent initializes session-locally"),
			Topology.GetState() == EVoxelPaintTopologyState::SessionLocalInitialized);

		TestTrue(TEXT("Auto-Finish: seal ordering"), Machine.RequestFinish() && Machine.OnSealed());
		TestTrue(TEXT("Auto-Finish: commit installs"), Machine.OnCommitInstalled());
		Topology.OnFinishInstalled();
		TestTrue(TEXT("Auto-Finish: Finish installs durable topology"),
			Topology.GetState() == EVoxelPaintTopologyState::DurablyInitialized);
		TestTrue(TEXT("Auto-Finish: derived chain runs to Closed"),
			Machine.OnDerivingStarted() && Machine.OnMatchingGenerationPublished() && Machine.OnRetirementComplete());
		TestTrue(TEXT("Auto-Finish: ends Closed"), Machine.GetStage() == EVoxelPaintSessionStage::Closed);
		TestTrue(TEXT("Auto-Finish: was the asset's first session"), Machine.WasFirstSession());
		TestTrue(TEXT("Auto-Finish: committed"), Machine.HasEverCommitted());
	}

	{
		FVoxelPaintSessionMachine Machine;
		FVoxelPaintTopologyLockMachine Topology;
		Topology.Reset(false);

		Machine.Open(MakeAdmittedInputs(), false);
		Topology.OnOpAdmitted(true);
		TestTrue(TEXT("Discard: confirmed Discard closes from Active"), Machine.RequestDiscardConfirmed());
		TestTrue(TEXT("Discard: ends Closed"), Machine.GetStage() == EVoxelPaintSessionStage::Closed);
		TestFalse(TEXT("Discard: never commits"), Machine.HasEverCommitted());
		Topology.OnDiscardConfirmed();
		TestTrue(TEXT("Discard: restores uninitialized topology"),
			Topology.GetState() == EVoxelPaintTopologyState::Uninitialized);
		TestTrue(TEXT("Discard: topology editable again outside sessions"), Topology.CanEditTopology(false));
	}

	{
		FVoxelPaintSessionMachine Machine = DriveTo(EVoxelPaintSessionStage::Sealing);
		TestFalse(TEXT("Discard: rejected while Sealing"), Machine.RequestDiscardConfirmed());
		TestTrue(TEXT("Discard: Sealing unchanged by the rejection"),
			Machine.GetStage() == EVoxelPaintSessionStage::Sealing);
	}

	{
		FVoxelPaintSessionMachine Machine;
		FVoxelPaintTopologyLockMachine Topology;
		Topology.Reset(false);

		Machine.Open(MakeAdmittedInputs(), false);
		Topology.OnOpAdmitted(true);
		TestTrue(TEXT("Carve-out: never-committed first session qualifies"),
			Machine.WasFirstSession() && !Machine.HasEverCommitted());
		TestTrue(TEXT("Carve-out: forced teardown ends without commit"), Machine.ForceTeardown(true));
		TestTrue(TEXT("Carve-out: ends Closed"), Machine.GetStage() == EVoxelPaintSessionStage::Closed);
		TestFalse(TEXT("Carve-out: still never committed"), Machine.HasEverCommitted());
		Topology.OnForcedTeardownNeverCommitted();
		TestTrue(TEXT("Carve-out: restores uninitialized topology"),
			Topology.GetState() == EVoxelPaintTopologyState::Uninitialized);
	}

	{
		FVoxelPaintSessionMachine Machine = DriveTo(EVoxelPaintSessionStage::Deriving);
		TestTrue(TEXT("Teardown: forced teardown works post-commit"), Machine.ForceTeardown(false));
		TestTrue(TEXT("Teardown: ends Closed"), Machine.GetStage() == EVoxelPaintSessionStage::Closed);
		TestTrue(TEXT("Teardown: commit survives as history"), Machine.HasEverCommitted());
	}

	{
		FVoxelPaintSessionMachine Machine = DriveTo(EVoxelPaintSessionStage::Canonicalizing);
		TestTrue(TEXT("Teardown: forced teardown works pre-commit"), Machine.ForceTeardown(false));
		TestTrue(TEXT("Teardown: pre-commit teardown ends Closed"),
			Machine.GetStage() == EVoxelPaintSessionStage::Closed);
	}

	{
		FVoxelPaintSessionMachine Machine;
		TestFalse(TEXT("Teardown: rejected while Closed"), Machine.ForceTeardown(false));
	}

	{
		FVoxelPaintSessionMachine Machine;
		Machine.Open(MakeAdmittedInputs(), true);
		TestFalse(TEXT("Second session on an initialized asset is not a first session"), Machine.WasFirstSession());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintSession_TopologyLock,
	"Game.WorldVoxelPaint.Unit.Session.TopologyLock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintSession_TopologyLock::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	FVoxelPaintTopologyLockMachine Machine;

	Machine.Reset(false);
	TestTrue(TEXT("Reset on an uninitialized asset lands Uninitialized"),
		Machine.GetState() == EVoxelPaintTopologyState::Uninitialized);
	TestTrue(TEXT("Uninitialized topology editable outside sessions"), Machine.CanEditTopology(false));
	TestFalse(TEXT("Open session blocks topology edits even while Uninitialized"), Machine.CanEditTopology(true));

	Machine.OnOpAdmitted(false);
	TestTrue(TEXT("Zero-write-intent op does not initialize (M3-D14 consumption side)"),
		Machine.GetState() == EVoxelPaintTopologyState::Uninitialized);

	Machine.OnOpAdmitted(true);
	TestTrue(TEXT("Admitted non-zero write intent initializes session-locally"),
		Machine.GetState() == EVoxelPaintTopologyState::SessionLocalInitialized);
	TestTrue(TEXT("Cancelled-after-admission stays initialized (no reverse event exists)"),
		Machine.GetState() == EVoxelPaintTopologyState::SessionLocalInitialized);
	Machine.OnOpAdmitted(false);
	TestTrue(TEXT("Later zero-intent ops never demote"),
		Machine.GetState() == EVoxelPaintTopologyState::SessionLocalInitialized);
	TestFalse(TEXT("Session-local initialization blocks topology edits"), Machine.CanEditTopology(false));
	Machine.OnResetDataAndUnlock();
	TestTrue(TEXT("Reset Data does not apply to session-local state"),
		Machine.GetState() == EVoxelPaintTopologyState::SessionLocalInitialized);

	Machine.OnFinishInstalled();
	TestTrue(TEXT("Finish installs durable initialization (unconditional, zero net paint included)"),
		Machine.GetState() == EVoxelPaintTopologyState::DurablyInitialized);
	TestFalse(TEXT("Durable initialization blocks topology edits outside sessions"), Machine.CanEditTopology(false));
	TestFalse(TEXT("Durable initialization blocks topology edits inside sessions"), Machine.CanEditTopology(true));
	Machine.OnOpAdmitted(true);
	TestTrue(TEXT("Write intent on an initialized asset changes nothing"),
		Machine.GetState() == EVoxelPaintTopologyState::DurablyInitialized);
	TestTrue(TEXT("Clearing paint to zero fires no event: state deliberately unchanged"),
		Machine.GetState() == EVoxelPaintTopologyState::DurablyInitialized);

	Machine.OnResetDataAndUnlock();
	TestTrue(TEXT("Reset Data and Unlock Size restores Uninitialized from durable"),
		Machine.GetState() == EVoxelPaintTopologyState::Uninitialized);
	TestTrue(TEXT("Reset makes topology editable again"), Machine.CanEditTopology(false));

	Machine.Reset(true);
	TestTrue(TEXT("Reset on a durably initialized asset lands DurablyInitialized"),
		Machine.GetState() == EVoxelPaintTopologyState::DurablyInitialized);
	Machine.OnUndoOfFirstFinish();
	TestTrue(TEXT("Undo of a first Finish restores Uninitialized"),
		Machine.GetState() == EVoxelPaintTopologyState::Uninitialized);
	Machine.OnUndoOfFirstFinish();
	TestTrue(TEXT("Undo event on an uninitialized asset is a no-op"),
		Machine.GetState() == EVoxelPaintTopologyState::Uninitialized);

	Machine.Reset(false);
	Machine.OnOpAdmitted(true);
	Machine.OnDiscardConfirmed();
	TestTrue(TEXT("Discard restores Uninitialized from session-local"),
		Machine.GetState() == EVoxelPaintTopologyState::Uninitialized);

	Machine.Reset(false);
	Machine.OnOpAdmitted(true);
	Machine.OnForcedTeardownNeverCommitted();
	TestTrue(TEXT("Never-committed forced teardown restores Uninitialized from session-local"),
		Machine.GetState() == EVoxelPaintTopologyState::Uninitialized);

	Machine.Reset(true);
	Machine.OnDiscardConfirmed();
	Machine.OnForcedTeardownNeverCommitted();
	TestTrue(TEXT("Session-local restore events never demote durable initialization"),
		Machine.GetState() == EVoxelPaintTopologyState::DurablyInitialized);

	return true;
}
