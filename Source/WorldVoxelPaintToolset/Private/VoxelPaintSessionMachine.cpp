// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintSessionMachine.h"

namespace WorldVoxelPaint
{

namespace SessionMachinePrivate
{
	static constexpr uint8 GridReadinessReady = 1;
	static constexpr uint8 AuthorityAllowed = 0;
}

const TCHAR* LexToString(EVoxelPaintSessionStage Stage)
{
	switch (Stage)
	{
	case EVoxelPaintSessionStage::Closed: return TEXT("Closed");
	case EVoxelPaintSessionStage::Active: return TEXT("Active");
	case EVoxelPaintSessionStage::Sealing: return TEXT("Sealing");
	case EVoxelPaintSessionStage::Canonicalizing: return TEXT("Canonicalizing");
	case EVoxelPaintSessionStage::Committed: return TEXT("Committed");
	case EVoxelPaintSessionStage::Deriving: return TEXT("Deriving");
	case EVoxelPaintSessionStage::Publication: return TEXT("Publication");
	case EVoxelPaintSessionStage::Retirement: return TEXT("Retirement");
	case EVoxelPaintSessionStage::DerivedFailed: return TEXT("DerivedFailed");
	default: return TEXT("Unknown");
	}
}

EVoxelPaintSessionStage FVoxelPaintSessionMachine::GetStage() const
{
	return Stage;
}

bool FVoxelPaintSessionMachine::IsPreCommit() const
{
	return Stage == EVoxelPaintSessionStage::Active
		|| Stage == EVoxelPaintSessionStage::Sealing
		|| Stage == EVoxelPaintSessionStage::Canonicalizing;
}

bool FVoxelPaintSessionMachine::IsClosedForPIE() const
{
	return Stage == EVoxelPaintSessionStage::Closed;
}

bool FVoxelPaintSessionMachine::WasFirstSession() const
{
	return bWasFirstSession;
}

bool FVoxelPaintSessionMachine::HasEverCommitted() const
{
	return bHasEverCommitted;
}

EVoxelPaintSessionAdmissionVerdict FVoxelPaintSessionMachine::JudgeAdmission(
	const FVoxelPaintSessionAdmissionInputs& Inputs)
{
	if (Inputs.bAnotherSessionOpen)
	{
		return EVoxelPaintSessionAdmissionVerdict::DeniedSessionBusy;
	}
	if (!Inputs.bWorldIsMaster)
	{
		return EVoxelPaintSessionAdmissionVerdict::DeniedNotMaster;
	}
	if (!Inputs.bEligiblePrimary)
	{
		return EVoxelPaintSessionAdmissionVerdict::DeniedNoEligiblePrimary;
	}
	if (Inputs.GridReadiness != SessionMachinePrivate::GridReadinessReady)
	{
		return EVoxelPaintSessionAdmissionVerdict::DeniedGridNotReady;
	}
	if (Inputs.bPIEActiveOrQueued)
	{
		return EVoxelPaintSessionAdmissionVerdict::DeniedPIEActiveOrQueued;
	}
	if (Inputs.AuthorityVerdict != SessionMachinePrivate::AuthorityAllowed)
	{
		return EVoxelPaintSessionAdmissionVerdict::DeniedAuthority;
	}
	if (!Inputs.bAssetValid)
	{
		return EVoxelPaintSessionAdmissionVerdict::DeniedInvalidAsset;
	}
	return EVoxelPaintSessionAdmissionVerdict::Admitted;
}

bool FVoxelPaintSessionMachine::Open(const FVoxelPaintSessionAdmissionInputs& Inputs, bool bAssetWasInitialized)
{
	if (Stage != EVoxelPaintSessionStage::Closed)
	{
		return false;
	}
	if (JudgeAdmission(Inputs) != EVoxelPaintSessionAdmissionVerdict::Admitted)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Active;
	bWasFirstSession = !bAssetWasInitialized;
	bHasEverCommitted = false;
	return true;
}

bool FVoxelPaintSessionMachine::CanMutate(bool bOriginIsCurrentMasterWithPrimary, uint8 GridReadiness,
	bool bPriorCloseCommitResolved) const
{
	return Stage == EVoxelPaintSessionStage::Active
		&& bOriginIsCurrentMasterWithPrimary
		&& GridReadiness == SessionMachinePrivate::GridReadinessReady
		&& bPriorCloseCommitResolved;
}

bool FVoxelPaintSessionMachine::RequestFinish()
{
	if (Stage != EVoxelPaintSessionStage::Active)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Sealing;
	return true;
}

bool FVoxelPaintSessionMachine::CancelSealing()
{
	if (Stage != EVoxelPaintSessionStage::Sealing)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Active;
	return true;
}

bool FVoxelPaintSessionMachine::OnSealed()
{
	if (Stage != EVoxelPaintSessionStage::Sealing)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Canonicalizing;
	return true;
}

bool FVoxelPaintSessionMachine::OnCanonicalizeFailed()
{
	if (Stage != EVoxelPaintSessionStage::Canonicalizing)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Active;
	return true;
}

bool FVoxelPaintSessionMachine::OnCommitInstalled()
{
	if (Stage != EVoxelPaintSessionStage::Canonicalizing)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Committed;
	bHasEverCommitted = true;
	return true;
}

bool FVoxelPaintSessionMachine::OnCommitPreflightFailed()
{
	if (Stage != EVoxelPaintSessionStage::Canonicalizing)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Active;
	return true;
}

bool FVoxelPaintSessionMachine::OnDerivingStarted()
{
	if (Stage != EVoxelPaintSessionStage::Committed)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Deriving;
	return true;
}

bool FVoxelPaintSessionMachine::OnMatchingGenerationPublished()
{
	if (Stage != EVoxelPaintSessionStage::Deriving)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Publication;
	return true;
}

bool FVoxelPaintSessionMachine::OnRetirementComplete()
{
	if (Stage != EVoxelPaintSessionStage::Publication && Stage != EVoxelPaintSessionStage::Retirement)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Closed;
	return true;
}

bool FVoxelPaintSessionMachine::OnPublicationRetiring()
{
	if (Stage != EVoxelPaintSessionStage::Publication)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Retirement;
	return true;
}

bool FVoxelPaintSessionMachine::OnDerivedFailed()
{
	if (Stage != EVoxelPaintSessionStage::Deriving && Stage != EVoxelPaintSessionStage::Publication)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::DerivedFailed;
	return true;
}

bool FVoxelPaintSessionMachine::RequestRetry()
{
	if (Stage != EVoxelPaintSessionStage::DerivedFailed)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Deriving;
	return true;
}

bool FVoxelPaintSessionMachine::RequestEndAnyway()
{
	if (Stage != EVoxelPaintSessionStage::DerivedFailed)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Retirement;
	return true;
}

bool FVoxelPaintSessionMachine::RequestDiscardConfirmed()
{
	if (Stage != EVoxelPaintSessionStage::Active)
	{
		return false;
	}
	Stage = EVoxelPaintSessionStage::Closed;
	return true;
}

bool FVoxelPaintSessionMachine::ForceTeardown(bool bNeverCommittedFirstSession)
{
	if (Stage == EVoxelPaintSessionStage::Closed)
	{
		return false;
	}
	checkSlow(!bNeverCommittedFirstSession || (bWasFirstSession && !bHasEverCommitted));
	Stage = EVoxelPaintSessionStage::Closed;
	return true;
}

bool ResolvePIEVerdict(EVoxelPaintSessionStage Stage, FString& OutReason)
{
	if (Stage == EVoxelPaintSessionStage::Closed)
	{
		OutReason.Reset();
		return true;
	}
	OutReason = FString::Printf(
		TEXT("A World Voxel Paint session is %s. Finish or Discard the session (or End Anyway after a derived failure) before starting PIE."),
		LexToString(Stage));
	return false;
}

}
