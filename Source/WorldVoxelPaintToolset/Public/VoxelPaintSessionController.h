// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Delegates/IDelegateInstance.h"
#include "Misc/Optional.h"
#include "Tasks/Task.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "UObject/ObjectKey.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtr.h"
#include "VoxelPaintCanonicalSource.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintResidencyTypes.h"
#include "VoxelPaintSessionBudget.h"
#include "VoxelPaintSessionCpuState.h"
#include "VoxelPaintSessionMachine.h"
#include "VoxelPaintSessionRenderTypes.h"
#include "VoxelPaintStrokeJournal.h"
#include "VoxelPaintTopologyLock.h"
#include "VoxelPaintTypes.h"

class FObjectPostSaveContext;
class FVoxelPaintSessionResources;
class FVoxelPaintSessionTransferEngine;
class FVoxelPaintStrokePipeline;
class UPackage;
class UVoxelPaintVolumeComponent;
class UVoxelPaintVolumeData;
class UWorld;
struct FVoxelPaintCanonicalPayload;

// Per-op admission payload for BeginMutation: conservative reservations plus the
// write-intent classification the stroke pipeline computes.
struct FVoxelPaintMutationDesc
{
	int64 JournalReserveBytes = 0;
	int64 GpuStagingBytes = 0;
	bool bNonZeroWriteIntent = false;
};

struct FVoxelPaintTestSessionState
{
	TWeakObjectPtr<UVoxelPaintVolumeData> Asset;
	WorldVoxelPaint::EVoxelPaintSessionStage Stage = WorldVoxelPaint::EVoxelPaintSessionStage::Closed;
	// When set, CanMutateNow reads this fabricated coordinator status (with SessionGeneration
	// below as the controller-side generation) in place of the live coordinator/master/grid/
	// hydration terms, so tests can drive the per-placement stroke gate headlessly.
	TOptional<FVoxelPaintSessionRenderStatus> RenderStatus;
	uint64 SessionGeneration = 0;
};

class WORLDVOXELPAINTTOOLSET_API FVoxelPaintSessionController
{
public:
	static FVoxelPaintSessionController& Get();
	static FVoxelPaintSessionController* GetIfCreated();
	static void Shutdown();

	WorldVoxelPaint::EVoxelPaintSessionAdmissionVerdict OpenSession(
		UVoxelPaintVolumeComponent* Placement, FText& OutDenialText);

	bool IsSessionOpen() const;
	UVoxelPaintVolumeData* GetActiveAsset() const;
	const UWorld* GetOriginWorld() const;
	WorldVoxelPaint::EVoxelPaintSessionStage GetStage() const;
	WorldVoxelPaint::EVoxelPaintSessionStage GetStageForAsset(const UVoxelPaintVolumeData* Asset) const;
	bool CanMutateNow(FText& OutWhyNot) const;
	// Per-placement stroke gate: the conditions above ANDed with evidence that the paint
	// target's record published the session selector for the current session. Applies only
	// to frontier-admitted targets resolving the active asset; never waits on frontier
	// admission, residency integration, or brick uploads.
	bool CanMutateNow(const FVoxelPaintFrontierIdentity& PaintTargetIdentity, FText& OutWhyNot) const;
	bool IsAssetFrozen(const UVoxelPaintVolumeData* Asset) const;

	// The authority-metadata freeze gate: false with Finish-or-Discard remedy text while the
	// asset's session sits in Active..Committed; true otherwise (Deriving onward is unfrozen
	// for authority transitions; topology stays gated separately).
	bool GateAuthorityTransition(const UVoxelPaintVolumeData* Asset, FText& OutWhyNot) const;

	int64 GetJournalRetainedBytes() const;    // 0 when no session
	int64 GetJournalCapacityBytes() const;    // configured capacity (session-open value)
	int32 GetJournalEntryCount() const;
	float GetDerivingWatchdogSeconds() const; // accumulated serviced-derive seconds; 0 outside Deriving
	// Wall-clock seconds since the coordinator's publication ordinal last advanced while the
	// session's origin world held mastership; 0 outside Deriving/Publication/Retirement.
	// Nonzero-and-growing means derived work exists but the origin world's derive lane is not
	// being serviced (viewport not rendering, or another world holds mastership).
	float GetSecondsSinceDerivedServiced() const;

	// Inert when unset (production behavior exactly unchanged). While set, ONLY the query
	// surface reads it (GetStage, GetStageForAsset, IsSessionOpen, IsAssetFrozen, CanMutateNow,
	// GateAuthorityTransition, the open-session asset query); lifecycle verbs and GPU paths
	// ignore it. Tests clear it on every exit path.
	void SetTestSessionStateOverride(const TOptional<FVoxelPaintTestSessionState>& Override);

	void RequestFinish();
	// Ordinary-exit auto-Finish (mode exit and the controller's own exit hooks): preserve by
	// default; arms the save-again notice.
	void RequestAutoFinish(const TCHAR* Reason);
	void CancelSealing();
	void RequestDiscard();
	void RequestRetry();
	void RequestEndAnyway();
	void NotifyEditorTick(float DeltaSeconds);

	bool BeginMutation(const FVoxelPaintMutationDesc& Desc, FText& OutWhyNot);
	bool GrowOpenStrokeReservation(int64 AdditionalBytes);
	void CommitClosedStroke(WorldVoxelPaint::FVoxelPaintStrokeDelta&& Delta);
	void CancelOpenStroke();
	bool UndoInSession();
	bool RedoInSession();

	// Fail-closed repair of an authoritative GPU upload (undo/redo/cancel restore): while any
	// failed upload is unresolved the mutation gate stays shut and the controller re-issues the
	// affected bricks from exact CPU state; exhausted retries force teardown.
	void NotifyAuthoritativeUploadFailed(TArray<uint32>&& Bricks);

	// The bound pipeline is the only holder of the open stroke's dirty-brick state; the
	// controller needs it to distinguish an open drag (cancel with GPU restore) from a pending
	// close readback (must be allowed to deliver CommitClosedStroke) at Finish/Discard/exit.
	void NotifyStrokePipelineBound(FVoxelPaintStrokePipeline* Pipeline);
	void NotifyStrokePipelineUnbound(FVoxelPaintStrokePipeline* Pipeline);

	// Session surfaces for the stroke pipeline (6.14): the pipeline mutates the CPU state on
	// close-readback merge and drives per-stamp GPU work through the transfer engine.
	WorldVoxelPaint::FVoxelPaintSessionCpuState* GetSessionCpuState();
	FVoxelPaintSessionResources* GetSessionResources();
	FVoxelPaintSessionTransferEngine* GetTransferEngine();
	WorldVoxelPaint::FVoxelPaintSessionBudgetLedger* GetBudgetLedger();
	uint64 GetSessionGeneration() const;

private:
	struct FCanonicalizeResult
	{
		WorldVoxelPaint::FVoxelPaintCanonicalBuild Build;
		FVoxelPaintCanonicalIdentity Identity;
		bool bSucceeded = false;
	};

	FVoxelPaintSessionController();
	~FVoxelPaintSessionController();
	FVoxelPaintSessionController(const FVoxelPaintSessionController&) = delete;
	FVoxelPaintSessionController& operator=(const FVoxelPaintSessionController&) = delete;

	// Lifecycle/GPU paths read the live machine, never the test override.
	bool IsLiveSessionOpen() const;

	void OnRHIRefsResolved(uint64 ForSessionGeneration, bool bOk);
	void PushRenderBinding(EVoxelPaintSessionBindingPhase Phase);
	void ClearRenderBinding();
	void StartCanonicalizing();
	void PollSealing();
	void PollCanonicalizing();
	void PollDeriving(float DeltaSeconds);
	void UpdateDerivedServiceClock(WorldVoxelPaint::EVoxelPaintSessionStage Stage, float DeltaSeconds);
	void PollRetirement();
	void PollDeferredRelease(float DeltaSeconds);
	bool PreflightCanonicalResult(const FCanonicalizeResult& Result) const;
	void CommitFinish(FCanonicalizeResult&& Result);
	bool BuildAfterPayload(FCanonicalizeResult&& Result, FVoxelPaintCanonicalPayload& OutAfter) const;
	void SynchronousCommitCpuClosedWork();
	void AutoFinish(const TCHAR* Reason);
	void ForceTeardownNow(bool bPreserveCpuClosedWork);
	void ResolveOpenStrokeForFinish(const TCHAR* Reason);
	void TryCompleteDiscard();
	void TryRepairAuthoritativeUpload();
	void OnAuthoritativeUploadRepaired(const TArray<uint32>& Bricks);
	void DropOpenStroke(const TCHAR* Reason);
	void DrainTransfersForTeardown();
	void ReleaseSessionResourcesNow();
	void CloseoutSession();
	void UpdateStats() const;
	bool UploadDeltaBricksFromCpuState(const WorldVoxelPaint::FVoxelPaintStrokeDelta& Delta);
	void RecordAccumulatorFirstTouch(const WorldVoxelPaint::FVoxelPaintStrokeDelta& Delta);

	void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	void HandleMapChange(uint32 MapChangeFlags);
	void HandleEnginePreExit();
	void HandlePackageSaved(const FString& PackageFilename, UPackage* Package,
		FObjectPostSaveContext ObjectSaveContext);

	WorldVoxelPaint::FVoxelPaintSessionMachine Machine;
	WorldVoxelPaint::FVoxelPaintTopologyLockMachine TopologyMachine;
	WorldVoxelPaint::FVoxelPaintStrokeJournal Journal;
	WorldVoxelPaint::FVoxelPaintBaselineAccumulator Accumulator;
	WorldVoxelPaint::FVoxelPaintSessionCpuState CpuState;
	WorldVoxelPaint::FVoxelPaintSessionBudgetLedger Ledger;
	TUniquePtr<FVoxelPaintSessionResources> Resources;
	TUniquePtr<FVoxelPaintSessionTransferEngine> TransferEngine;

	TStrongObjectPtr<UVoxelPaintVolumeData> ActiveAsset;
	TWeakObjectPtr<const UWorld> OriginWorld;
	FObjectKey OriginWorldKey;
	FIntVector SessionResolution = FIntVector::ZeroValue;
	FVoxelPaintCanonicalIdentity CommittedIdentity;
	uint64 SessionGeneration = 0;
	uint64 NextSessionGeneration = 1;
	bool bAssetWasInitializedAtOpen = false;
	EVoxelPaintAssetSharingMode FrozenSharingMode = EVoxelPaintAssetSharingMode::Private;
	FGuid FrozenOwnerActorGuid;

	FVoxelPaintStrokePipeline* BoundStrokePipeline = nullptr;
	bool bStrokeOpen = false;
	int32 OpenStrokeJournalReservationId = INDEX_NONE;
	int32 OpenStrokeLedgerHandle = INDEX_NONE;
	bool bHydrationIssued = false;
	bool bHydrationComplete = false;
	bool bDiscardPending = false;
	bool bReleasePending = false;
	TArray<uint32> UnresolvedAuthoritativeUploadBricks;
	int32 AuthoritativeUploadRetriesRemaining = 0;
	bool bAssetSavedDuringSession = false;
	bool bCurrentFinishIsAuto = false;
	bool bInForcedTeardown = false;
	float DerivingServicedSeconds = 0.0f;
	float SecondsSinceDerivedServiced = 0.0f;
	float SecondsAwaitingDeferredRelease = 0.0f;
	bool bDeferredReleaseStallLogged = false;
	uint64 DerivedServiceClockOrdinal = 0;
	uint64 LastObservedPublicationOrdinal = 0;
	uint64 CommitPublicationOrdinal = 0;
	TOptional<FVoxelPaintTestSessionState> TestSessionStateOverride;

	UE::Tasks::FTask CanonicalizeTask;
	TSharedPtr<FCanonicalizeResult, ESPMode::ThreadSafe> CanonicalizeResult;

	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle WorldCleanupHandle;
	FDelegateHandle MapChangeHandle;
	FDelegateHandle PreExitHandle;
	FDelegateHandle PackageSavedHandle;
};
