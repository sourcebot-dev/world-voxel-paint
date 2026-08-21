// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace WorldVoxelPaint
{
	enum class EVoxelPaintSessionStage : uint8
	{
		Closed,
		Active,
		Sealing,
		Canonicalizing,
		Committed,
		Deriving,
		Publication,
		Retirement,
		DerivedFailed
	};

	WORLDVOXELPAINTTOOLSET_API const TCHAR* LexToString(EVoxelPaintSessionStage Stage);

	enum class EVoxelPaintSessionAdmissionVerdict : uint8
	{
		Admitted,
		DeniedSessionBusy,
		DeniedNotMaster,
		DeniedNoEligiblePrimary,
		DeniedGridNotReady,
		DeniedPIEActiveOrQueued,
		DeniedAuthority,
		DeniedInvalidAsset
	};

	struct FVoxelPaintSessionAdmissionInputs
	{
		bool bAnotherSessionOpen = false;
		bool bWorldIsMaster = false;
		bool bEligiblePrimary = false;
		uint8 GridReadiness = 0;
		bool bPIEActiveOrQueued = false;
		uint8 AuthorityVerdict = 0;
		bool bAssetValid = false;
	};

	class WORLDVOXELPAINTTOOLSET_API FVoxelPaintSessionMachine
	{
	public:
		EVoxelPaintSessionStage GetStage() const;
		bool IsPreCommit() const;
		bool IsClosedForPIE() const;
		bool WasFirstSession() const;
		bool HasEverCommitted() const;

		static EVoxelPaintSessionAdmissionVerdict JudgeAdmission(const FVoxelPaintSessionAdmissionInputs& Inputs);
		bool Open(const FVoxelPaintSessionAdmissionInputs& Inputs, bool bAssetWasInitialized);

		bool CanMutate(bool bOriginIsCurrentMasterWithPrimary, uint8 GridReadiness,
			bool bPriorCloseCommitResolved) const;

		bool RequestFinish();
		bool CancelSealing();
		bool OnSealed();
		bool OnCanonicalizeFailed();
		bool OnCommitInstalled();
		bool OnCommitPreflightFailed();
		bool OnDerivingStarted();
		bool OnMatchingGenerationPublished();
		bool OnRetirementComplete();
		bool OnPublicationRetiring();
		bool OnDerivedFailed();
		bool RequestRetry();
		bool RequestEndAnyway();

		bool RequestDiscardConfirmed();
		bool ForceTeardown(bool bNeverCommittedFirstSession);

	private:
		EVoxelPaintSessionStage Stage = EVoxelPaintSessionStage::Closed;
		bool bWasFirstSession = false;
		bool bHasEverCommitted = false;
	};

	WORLDVOXELPAINTTOOLSET_API bool ResolvePIEVerdict(EVoxelPaintSessionStage Stage, FString& OutReason);
}
