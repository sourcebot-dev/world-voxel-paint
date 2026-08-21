// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintSessionController.h"

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GameFramework/Actor.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ITransaction.h"
#include "RHIGlobals.h"
#include "RenderingThread.h"
#include "ScopedTransaction.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "VoxelPaintAuthority.h"
#include "VoxelPaintCoordinator.h"
#include "VoxelPaintDeveloperSettings.h"
#include "VoxelPaintDiagnostics.h"
#include "VoxelPaintFinishChange.h"
#include "VoxelPaintGridReadiness.h"
#include "VoxelPaintSessionRenderTypes.h"
#include "VoxelPaintSessionResources.h"
#include "VoxelPaintSessionTransfers.h"
#include "VoxelPaintStats.h"
#include "VoxelPaintStrokePipeline.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "VoxelPaintWorldSubsystem.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "WorldVoxelPaintConstants.h"

#define LOCTEXT_NAMESPACE "WorldVoxelPaint"

namespace
{

FVoxelPaintSessionController* GVoxelPaintSessionController = nullptr;

constexpr float GDerivedWedgeTimeoutSeconds = 120.0f;

// Caps what one editor tick can add to the wall-clock idle counters, so a single stalled
// game-thread frame (modal dialog, blocking load) cannot cross a warn threshold before the
// render thread gets a chance to advance the publication ordinal.
constexpr float GServiceClockMaxTickStepSeconds = 0.25f;

constexpr float GDeferredReleaseStallWarnSeconds = 3.0f;

void ShowVoxelPaintSessionNotification(const FString& Message)
{
	if (FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(FText::FromString(Message));
		Info.ExpireDuration = 6.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
	else
	{
		UE_LOG(LogWorldVoxelPaint, Warning, TEXT("%s"), *Message);
	}
}

bool IsBrickTopologyAdmissible(const FIntVector& Counts)
{
	const int32 MaxPerAxis = FMath::Max(1,
		(int32)(GMaxVolumeTextureDimensions / WorldVoxelPaint::UniqueBrickSize));
	return Counts.X >= 1 && Counts.Y >= 1 && Counts.Z >= 1
		&& Counts.X <= MaxPerAxis && Counts.Y <= MaxPerAxis && Counts.Z <= MaxPerAxis
		&& (int64)Counts.X * Counts.Y * Counts.Z <= WorldVoxelPaint::AssetAdmissionCap;
}

} // namespace

FVoxelPaintSessionController& FVoxelPaintSessionController::Get()
{
	check(IsInGameThread());
	if (!GVoxelPaintSessionController)
	{
		GVoxelPaintSessionController = new FVoxelPaintSessionController();
	}
	return *GVoxelPaintSessionController;
}

FVoxelPaintSessionController* FVoxelPaintSessionController::GetIfCreated()
{
	return GVoxelPaintSessionController;
}

void FVoxelPaintSessionController::Shutdown()
{
	if (GVoxelPaintSessionController)
	{
		GVoxelPaintSessionController->ForceTeardownNow(true);
		delete GVoxelPaintSessionController;
		GVoxelPaintSessionController = nullptr;
	}
}

FVoxelPaintSessionController::FVoxelPaintSessionController()
{
	Ledger.Initialize(WorldVoxelPaint::SessionGpuBudgetBytes);

	WorldVoxelPaint::SetOpenSessionAssetQuery([](const UVoxelPaintVolumeData* Asset)
	{
		const FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
		return Controller
			&& Controller->GetStageForAsset(Asset) != WorldVoxelPaint::EVoxelPaintSessionStage::Closed;
	});

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("VoxelPaintSessionController"), 0.0f,
		[this](float DeltaSeconds)
		{
			NotifyEditorTick(DeltaSeconds);
			return true;
		});

	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(
		this, &FVoxelPaintSessionController::HandleWorldCleanup);
	MapChangeHandle = FEditorDelegates::MapChange.AddRaw(
		this, &FVoxelPaintSessionController::HandleMapChange);
	PreExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(
		this, &FVoxelPaintSessionController::HandleEnginePreExit);
	PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddRaw(
		this, &FVoxelPaintSessionController::HandlePackageSaved);
}

FVoxelPaintSessionController::~FVoxelPaintSessionController()
{
	UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
	FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle);
	FEditorDelegates::MapChange.Remove(MapChangeHandle);
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	WorldVoxelPaint::SetOpenSessionAssetQuery(nullptr);
}

WorldVoxelPaint::EVoxelPaintSessionAdmissionVerdict FVoxelPaintSessionController::OpenSession(
	UVoxelPaintVolumeComponent* Placement, FText& OutDenialText)
{
	using namespace WorldVoxelPaint;
	check(IsInGameThread());

	UVoxelPaintVolumeData* Asset = Placement ? Placement->ResolveActiveVariant() : nullptr;
	const UWorld* World = Placement ? Placement->GetWorld() : nullptr;
	const AActor* Initiator = Placement ? Placement->GetOwner() : nullptr;
	FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated();

	FVoxelPaintAuthorityDenial AuthorityDenial;
	FVoxelPaintSessionAdmissionInputs Inputs;
	Inputs.bAnotherSessionOpen = Machine.GetStage() != EVoxelPaintSessionStage::Closed || bReleasePending;
	Inputs.bWorldIsMaster = Coordinator && World && Coordinator->GetCurrentMasterWorld() == World;
	Inputs.bEligiblePrimary = Coordinator && World && Coordinator->IsMasterWorldWithEligiblePrimary(World);
	Inputs.GridReadiness = Coordinator
		? (uint8)Coordinator->GetGridReadiness() : (uint8)EVoxelPaintGridReadiness::Pending;
	Inputs.bPIEActiveOrQueued = GEditor
		&& (GEditor->PlayWorld != nullptr || GEditor->IsPlaySessionInProgress());
	Inputs.AuthorityVerdict = (uint8)CheckEditingAuthority(Asset, Initiator, &AuthorityDenial);
	Inputs.bAssetValid = Asset && IsBrickTopologyAdmissible(Asset->GetBrickCounts());

	const EVoxelPaintSessionAdmissionVerdict Verdict = FVoxelPaintSessionMachine::JudgeAdmission(Inputs);
	if (Verdict != EVoxelPaintSessionAdmissionVerdict::Admitted)
	{
		switch (Verdict)
		{
		case EVoxelPaintSessionAdmissionVerdict::DeniedSessionBusy:
			OutDenialText = LOCTEXT("AdmitDeniedBusy",
				"Another paint session is open (one session at a time). Finish, Discard, or End Anyway first.");
			break;
		case EVoxelPaintSessionAdmissionVerdict::DeniedNotMaster:
			OutDenialText = LOCTEXT("AdmitDeniedNotMaster",
				"This world is not the current master world; focus one of its viewports first.");
			break;
		case EVoxelPaintSessionAdmissionVerdict::DeniedNoEligiblePrimary:
			OutDenialText = LOCTEXT("AdmitDeniedNoPrimary",
				"No eligible primary viewport is available for this world.");
			break;
		case EVoxelPaintSessionAdmissionVerdict::DeniedGridNotReady:
			OutDenialText = LOCTEXT("AdmitDeniedGridNotReady",
				"The voxel paint grid is not Ready yet; wait a moment or move the camera to trigger a rebuild.");
			break;
		case EVoxelPaintSessionAdmissionVerdict::DeniedPIEActiveOrQueued:
			OutDenialText = LOCTEXT("AdmitDeniedPIE",
				"PIE/SIE is active or queued; paint sessions and PIE are mutually exclusive.");
			break;
		case EVoxelPaintSessionAdmissionVerdict::DeniedAuthority:
			OutDenialText = FText::Format(LOCTEXT("AdmitDeniedAuthority",
				"Editing denied - {0}. Remedies: Make Unique, Take Ownership, or mark Shared."),
				FText::FromString(AuthorityDenial.OwnerDescription));
			break;
		case EVoxelPaintSessionAdmissionVerdict::DeniedInvalidAsset:
		default:
			OutDenialText = LOCTEXT("AdmitDeniedInvalidAsset",
				"No valid voxel paint asset (assign an asset within the 32768-brick product cap and per-axis platform limits).");
			break;
		}
		return Verdict;
	}

	bAssetWasInitializedAtOpen = Asset->bInitialized;

	FVoxelPaintCanonicalPayload Hydration;
	Asset->CaptureCanonicalPayload(Hydration);
	if (bAssetWasInitializedAtOpen)
	{
		if (!CpuState.InitializeFromCanonical(Asset->GetBrickCounts(), Hydration.ManifestBricks,
			Hydration.PackedSourceBGRA8))
		{
			CpuState = WorldVoxelPaint::FVoxelPaintSessionCpuState();
			OutDenialText = LOCTEXT("AdmitDeniedCanonicalUnreadable",
				"The asset's canonical source failed validation; session not opened.");
			return EVoxelPaintSessionAdmissionVerdict::DeniedInvalidAsset;
		}
	}
	else
	{
		CpuState.Initialize(Asset->GetBrickCounts());
	}

	Resources = MakeUnique<FVoxelPaintSessionResources>();
	FString AllocError;
	if (!Resources->Allocate(Asset->GetResolution(), Ledger, AllocError))
	{
		Resources.Reset();
		CpuState = WorldVoxelPaint::FVoxelPaintSessionCpuState();
		OutDenialText = FText::Format(LOCTEXT("AdmitDeniedGpuBudget",
			"Session GPU resources rejected: {0}"), FText::FromString(AllocError));
		return EVoxelPaintSessionAdmissionVerdict::DeniedInvalidAsset;
	}
	TransferEngine = MakeUnique<FVoxelPaintSessionTransferEngine>();

	Machine = FVoxelPaintSessionMachine();
	if (!Machine.Open(Inputs, bAssetWasInitializedAtOpen))
	{
		ReleaseSessionResourcesNow();
		CpuState = WorldVoxelPaint::FVoxelPaintSessionCpuState();
		OutDenialText = LOCTEXT("AdmitDeniedInternal", "Session machine rejected admission.");
		return EVoxelPaintSessionAdmissionVerdict::DeniedInvalidAsset;
	}

	ActiveAsset = TStrongObjectPtr<UVoxelPaintVolumeData>(Asset);
	OriginWorld = World;
	OriginWorldKey = FObjectKey(World);
	SessionResolution = Asset->GetResolution();
	CommittedIdentity = FVoxelPaintCanonicalIdentity();
	SessionGeneration = NextSessionGeneration++;
	FrozenSharingMode = Asset->SharingMode;
	FrozenOwnerActorGuid = Asset->OwnerActorGuid;
	bAssetSavedDuringSession = false;
	bCurrentFinishIsAuto = false;
	bHydrationIssued = false;
	bHydrationComplete = false;
	bDiscardPending = false;
	UnresolvedAuthoritativeUploadBricks.Reset();
	AuthoritativeUploadRetriesRemaining = 0;
	DerivingServicedSeconds = 0.0f;
	LastObservedPublicationOrdinal = 0;
	CommitPublicationOrdinal = 0;

	Journal.Initialize(GetDefault<UVoxelPaintDeveloperSettings>()->GetJournalCapacityBytes());
	Accumulator = FVoxelPaintBaselineAccumulator();
	TopologyMachine.Reset(bAssetWasInitializedAtOpen);

	Resources->ResolveRHIRefs([Gen = SessionGeneration](bool bOk)
	{
		auto Deliver = [Gen, bOk]()
		{
			if (FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated())
			{
				Controller->OnRHIRefsResolved(Gen, bOk);
			}
		};
		if (IsInGameThread())
		{
			Deliver();
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread, MoveTemp(Deliver));
		}
	});

	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Voxel paint session %llu opened on '%s' (origin world '%s', %s asset)."),
		SessionGeneration, *Asset->GetPathName(), *GetNameSafe(World),
		bAssetWasInitializedAtOpen ? TEXT("initialized") : TEXT("uninitialized"));
	return EVoxelPaintSessionAdmissionVerdict::Admitted;
}

void FVoxelPaintSessionController::OnRHIRefsResolved(uint64 ForSessionGeneration, bool bOk)
{
	using namespace WorldVoxelPaint;
	check(IsInGameThread());
	if (ForSessionGeneration != SessionGeneration
		|| Machine.GetStage() == EVoxelPaintSessionStage::Closed || !Resources || !TransferEngine)
	{
		return;
	}
	if (!bOk)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("Voxel paint session %llu: session texture RHI resolution failed - session torn down."),
			SessionGeneration);
		ForceTeardownNow(false);
		return;
	}

	UVoxelPaintVolumeData* Asset = ActiveAsset.Get();
	TArray<uint32> HydrationBricks;
	if (Asset)
	{
		if (TSharedPtr<const FVoxelPaintSourceManifest> Manifest = Asset->GetManifestSnapshot())
		{
			HydrationBricks = Manifest->PopulatedBricks;
		}
	}
	FVoxelPaintPackedBrickTransfer Transfer;
	CpuState.BuildPackedTransfer(HydrationBricks, Transfer);

	bHydrationIssued = true;
	TransferEngine->Hydrate(*Resources, MoveTemp(Transfer), Ledger,
		[Gen = SessionGeneration](bool bHydrated)
		{
			auto Deliver = [Gen, bHydrated]()
			{
				FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
				if (!Controller || Gen != Controller->SessionGeneration)
				{
					return;
				}
				if (bHydrated)
				{
					Controller->bHydrationComplete = true;
				}
				else
				{
					UE_LOG(LogWorldVoxelPaint, Error,
						TEXT("Voxel paint session %llu: hydration failed - session torn down."), Gen);
					Controller->ForceTeardownNow(false);
				}
			};
			if (IsInGameThread())
			{
				Deliver();
			}
			else
			{
				AsyncTask(ENamedThreads::GameThread, MoveTemp(Deliver));
			}
		});

	PushRenderBinding(EVoxelPaintSessionBindingPhase::PreCommit);
}

bool FVoxelPaintSessionController::IsLiveSessionOpen() const
{
	return Machine.GetStage() != WorldVoxelPaint::EVoxelPaintSessionStage::Closed;
}

bool FVoxelPaintSessionController::IsSessionOpen() const
{
	return GetStage() != WorldVoxelPaint::EVoxelPaintSessionStage::Closed;
}

UVoxelPaintVolumeData* FVoxelPaintSessionController::GetActiveAsset() const
{
	return IsLiveSessionOpen() ? ActiveAsset.Get() : nullptr;
}

const UWorld* FVoxelPaintSessionController::GetOriginWorld() const
{
	return IsLiveSessionOpen() ? OriginWorld.Get() : nullptr;
}

WorldVoxelPaint::EVoxelPaintSessionStage FVoxelPaintSessionController::GetStage() const
{
	return TestSessionStateOverride.IsSet() ? TestSessionStateOverride->Stage : Machine.GetStage();
}

WorldVoxelPaint::EVoxelPaintSessionStage FVoxelPaintSessionController::GetStageForAsset(
	const UVoxelPaintVolumeData* Asset) const
{
	using namespace WorldVoxelPaint;
	if (TestSessionStateOverride.IsSet())
	{
		return Asset && Asset == TestSessionStateOverride->Asset.Get()
			? TestSessionStateOverride->Stage : EVoxelPaintSessionStage::Closed;
	}
	if (Asset && IsLiveSessionOpen() && ActiveAsset.Get() == Asset)
	{
		return Machine.GetStage();
	}
	return EVoxelPaintSessionStage::Closed;
}

void FVoxelPaintSessionController::SetTestSessionStateOverride(
	const TOptional<FVoxelPaintTestSessionState>& Override)
{
	check(IsInGameThread());
	TestSessionStateOverride = Override;
}

bool FVoxelPaintSessionController::CanMutateNow(FText& OutWhyNot) const
{
	using namespace WorldVoxelPaint;

	bool bOriginMasterPrimary = false;
	uint8 GridReadiness = (uint8)EVoxelPaintGridReadiness::Pending;
	if (const FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
	{
		const UWorld* Origin = OriginWorld.Get();
		bOriginMasterPrimary = Origin && Coordinator->GetCurrentMasterWorld() == Origin
			&& Coordinator->IsMasterWorldWithEligiblePrimary(Origin);
		GridReadiness = (uint8)Coordinator->GetGridReadiness();
	}
	const bool bCloseResolved = !bStrokeOpen
		&& (!TransferEngine || !TransferEngine->IsTransferInFlight())
		&& bHydrationComplete
		&& !bDiscardPending
		&& UnresolvedAuthoritativeUploadBricks.IsEmpty();

	const EVoxelPaintSessionStage Stage = GetStage();
	const bool bCanMutate = TestSessionStateOverride.IsSet()
		? Stage == EVoxelPaintSessionStage::Active
			&& (TestSessionStateOverride->RenderStatus.IsSet()
				|| (bOriginMasterPrimary
					&& GridReadiness == (uint8)EVoxelPaintGridReadiness::Ready && bCloseResolved))
		: Machine.CanMutate(bOriginMasterPrimary, GridReadiness, bCloseResolved);
	if (bCanMutate)
	{
		return true;
	}

	if (Stage != EVoxelPaintSessionStage::Active)
	{
		OutWhyNot = FText::Format(LOCTEXT("CanMutateNotActive",
			"No active paint session (stage: {0})."),
			FText::FromString(LexToString(Stage)));
	}
	else if (!bOriginMasterPrimary)
	{
		OutWhyNot = LOCTEXT("CanMutateNotOriginMaster",
			"The session's originating world is not the current master with an eligible primary viewport.");
	}
	else if (GridReadiness != (uint8)EVoxelPaintGridReadiness::Ready)
	{
		OutWhyNot = LOCTEXT("CanMutateGridNotReady",
			"The voxel paint grid is not Ready; painting resumes when the rebuild is observed.");
	}
	else if (!bHydrationComplete)
	{
		OutWhyNot = LOCTEXT("CanMutateHydrating",
			"The session is still hydrating its GPU textures; retry in a moment.");
	}
	else if (bDiscardPending)
	{
		OutWhyNot = LOCTEXT("CanMutateDiscardPending",
			"The session is being discarded.");
	}
	else if (!UnresolvedAuthoritativeUploadBricks.IsEmpty())
	{
		OutWhyNot = LOCTEXT("CanMutateUploadUnresolved",
			"A failed session GPU upload is being repaired from exact CPU state.");
	}
	else
	{
		OutWhyNot = LOCTEXT("CanMutateCloseUnresolved",
			"The previous stroke's close/commit has not resolved yet.");
	}
	return false;
}

bool FVoxelPaintSessionController::CanMutateNow(const FVoxelPaintFrontierIdentity& PaintTargetIdentity,
	FText& OutWhyNot) const
{
	if (!CanMutateNow(OutWhyNot))
	{
		return false;
	}
	FVoxelPaintSessionRenderStatus Status;
	uint64 GateSessionGeneration = SessionGeneration;
	if (TestSessionStateOverride.IsSet() && TestSessionStateOverride->RenderStatus.IsSet())
	{
		Status = TestSessionStateOverride->RenderStatus.GetValue();
		GateSessionGeneration = TestSessionStateOverride->SessionGeneration;
	}
	else if (const FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
	{
		Status = Coordinator->GetSessionRenderStatus();
	}
	if (!IsPlacementSelectorPublished(Status, GateSessionGeneration, PaintTargetIdentity))
	{
		OutWhyNot = LOCTEXT("CanMutatePlacementUnpublished",
			"Waiting for this placement's paint binding to publish.");
		return false;
	}
	return true;
}

bool FVoxelPaintSessionController::IsAssetFrozen(const UVoxelPaintVolumeData* Asset) const
{
	using namespace WorldVoxelPaint;
	const EVoxelPaintSessionStage Stage = GetStageForAsset(Asset);
	return Stage == EVoxelPaintSessionStage::Active
		|| Stage == EVoxelPaintSessionStage::Sealing
		|| Stage == EVoxelPaintSessionStage::Canonicalizing
		|| Stage == EVoxelPaintSessionStage::Committed;
}

bool FVoxelPaintSessionController::GateAuthorityTransition(const UVoxelPaintVolumeData* Asset,
	FText& OutWhyNot) const
{
	using namespace WorldVoxelPaint;
	if (IsAssetFrozen(Asset))
	{
		OutWhyNot = FText::Format(LOCTEXT("GateAuthorityFrozen",
			"Finish or Discard the paint session on '{0}' first (stage: {1})."),
			FText::FromString(GetNameSafe(Asset)),
			FText::FromString(LexToString(GetStageForAsset(Asset))));
		return false;
	}
	return true;
}

int64 FVoxelPaintSessionController::GetJournalRetainedBytes() const
{
	return Journal.GetRetainedBytes();
}

int64 FVoxelPaintSessionController::GetJournalCapacityBytes() const
{
	return Journal.GetCapacityBytes();
}

int32 FVoxelPaintSessionController::GetJournalEntryCount() const
{
	return Journal.GetNumEntries();
}

float FVoxelPaintSessionController::GetDerivingWatchdogSeconds() const
{
	return Machine.GetStage() == WorldVoxelPaint::EVoxelPaintSessionStage::Deriving
		? DerivingServicedSeconds : 0.0f;
}

float FVoxelPaintSessionController::GetSecondsSinceDerivedServiced() const
{
	using namespace WorldVoxelPaint;
	const EVoxelPaintSessionStage Stage = Machine.GetStage();
	const bool bWaitsOnPublication = Stage == EVoxelPaintSessionStage::Deriving
		|| Stage == EVoxelPaintSessionStage::Publication
		|| Stage == EVoxelPaintSessionStage::Retirement;
	return bWaitsOnPublication ? SecondsSinceDerivedServiced : 0.0f;
}

void FVoxelPaintSessionController::RequestFinish()
{
	check(IsInGameThread());
	if (Machine.GetStage() != WorldVoxelPaint::EVoxelPaintSessionStage::Active || bDiscardPending)
	{
		return;
	}
	ResolveOpenStrokeForFinish(TEXT("Finish requested"));
	bCurrentFinishIsAuto = false;
	Machine.RequestFinish();
}

void FVoxelPaintSessionController::RequestAutoFinish(const TCHAR* Reason)
{
	check(IsInGameThread());
	AutoFinish(Reason);
}

void FVoxelPaintSessionController::CancelSealing()
{
	check(IsInGameThread());
	Machine.CancelSealing();
}

void FVoxelPaintSessionController::RequestDiscard()
{
	using namespace WorldVoxelPaint;
	check(IsInGameThread());
	if (Machine.GetStage() != EVoxelPaintSessionStage::Active || bDiscardPending)
	{
		return;
	}
	if (bStrokeOpen)
	{
		if (BoundStrokePipeline && BoundStrokePipeline->IsStrokeOpen())
		{
			BoundStrokePipeline->CancelStroke();
		}
		else if (!BoundStrokePipeline)
		{
			DropOpenStroke(TEXT("Discard"));
		}
		// else: a close readback is pending; TryCompleteDiscard waits for it so its merge and
		// journal commit resolve before CpuState is torn down.
	}
	bDiscardPending = true;
	TryCompleteDiscard();
}

void FVoxelPaintSessionController::TryCompleteDiscard()
{
	using namespace WorldVoxelPaint;
	if (Machine.GetStage() != EVoxelPaintSessionStage::Active)
	{
		bDiscardPending = false;
		return;
	}
	if (bStrokeOpen || (TransferEngine && TransferEngine->IsTransferInFlight()))
	{
		return;
	}
	bDiscardPending = false;

	UVoxelPaintVolumeData* Asset = ActiveAsset.Get();
	FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated();
	const UWorld* Origin = OriginWorld.Get();
	const bool bOriginRenders = Coordinator && Origin
		&& Coordinator->GetCurrentMasterWorld() == Origin
		&& Coordinator->IsMasterWorldWithEligiblePrimary(Origin);
	if (bOriginRenders && Asset && Resources && TransferEngine && Accumulator.GetBaseline().Num() > 0)
	{
		FVoxelPaintPackedBrickTransfer Restore;
		Restore.BrickCounts = Asset->GetBrickCounts();
		Accumulator.GetBaseline().GetKeys(Restore.BrickIndices);
		Restore.BrickIndices.Sort();
		Restore.Texels.Reserve(Restore.BrickIndices.Num() * TexelsPerUniqueBrick);
		for (uint32 BrickIndex : Restore.BrickIndices)
		{
			Restore.Texels.Append(Accumulator.GetBaseline()[BrickIndex]);
		}
		TransferEngine->UploadAndPublish(*Resources, MoveTemp(Restore), Ledger, [](bool) {});
	}

	TopologyMachine.OnDiscardConfirmed();
	Machine.RequestDiscardConfirmed();
	ClearRenderBinding();

	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Voxel paint session %llu discarded on '%s' (baseline restored%s)."),
		SessionGeneration, *GetNameSafe(Asset),
		Machine.WasFirstSession() ? TEXT("; asset stays uninitialized and resizable") : TEXT(""));

	CloseoutSession();
	bReleasePending = true;
	SecondsAwaitingDeferredRelease = 0.0f;
	bDeferredReleaseStallLogged = false;
}

void FVoxelPaintSessionController::RequestRetry()
{
	using namespace WorldVoxelPaint;
	check(IsInGameThread());
	if (Machine.GetStage() != EVoxelPaintSessionStage::DerivedFailed)
	{
		return;
	}
	Machine.RequestRetry();
	DerivingServicedSeconds = 0.0f;
	if (FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
	{
		// Re-baseline so only mirrors built after this retry can re-trigger the ConfigError
		// fast path; the stale pre-retry error row must not instantly re-fail the session.
		CommitPublicationOrdinal = FMath::Max(CommitPublicationOrdinal,
			FMath::Max(Coordinator->GetSessionRenderStatus().CurrentPublicationOrdinal,
				Coordinator->GetResidencyDiagnostics().PublicationOrdinal));
		if (UVoxelPaintVolumeData* Asset = ActiveAsset.Get())
		{
			Coordinator->RequestAssetRepreparation(FVoxelPaintAssetKey(Asset));
		}
	}
	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Voxel paint session %llu: Retry requested - derived chain re-running."), SessionGeneration);
}

void FVoxelPaintSessionController::RequestEndAnyway()
{
	using namespace WorldVoxelPaint;
	check(IsInGameThread());
	if (Machine.GetStage() != EVoxelPaintSessionStage::DerivedFailed)
	{
		return;
	}
	Machine.RequestEndAnyway();
	ClearRenderBinding();
	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Voxel paint session %llu: End Anyway - session binding cleared; retiring."), SessionGeneration);
}

void FVoxelPaintSessionController::NotifyEditorTick(float DeltaSeconds)
{
	using namespace WorldVoxelPaint;
	check(IsInGameThread());

	const EVoxelPaintSessionStage Stage = Machine.GetStage();
	UpdateDerivedServiceClock(Stage, DeltaSeconds);
	switch (Stage)
	{
	case EVoxelPaintSessionStage::Sealing:
		PollSealing();
		break;
	case EVoxelPaintSessionStage::Canonicalizing:
		PollCanonicalizing();
		break;
	case EVoxelPaintSessionStage::Deriving:
		PollDeriving(DeltaSeconds);
		break;
	case EVoxelPaintSessionStage::Publication:
	case EVoxelPaintSessionStage::Retirement:
		PollRetirement();
		break;
	default:
		break;
	}

	if (!UnresolvedAuthoritativeUploadBricks.IsEmpty())
	{
		TryRepairAuthoritativeUpload();
	}
	if (bDiscardPending)
	{
		TryCompleteDiscard();
	}
	if (bReleasePending)
	{
		PollDeferredRelease(DeltaSeconds);
	}
	UpdateStats();
}

void FVoxelPaintSessionController::PollSealing()
{
	if (!bStrokeOpen && (!TransferEngine || !TransferEngine->IsTransferInFlight()))
	{
		Machine.OnSealed();
		StartCanonicalizing();
	}
}

void FVoxelPaintSessionController::StartCanonicalizing()
{
	using namespace WorldVoxelPaint;
	UVoxelPaintVolumeData* Asset = ActiveAsset.Get();
	if (!Asset)
	{
		Machine.OnCanonicalizeFailed();
		return;
	}

	TSharedPtr<FCanonicalizeResult, ESPMode::ThreadSafe> Result =
		MakeShared<FCanonicalizeResult, ESPMode::ThreadSafe>();
	CanonicalizeResult = Result;

	const FIntVector BrickCounts = Asset->GetBrickCounts();
	const FVector StorageOrigin = Asset->StorageOrigin;
	const FVector LocalHalfExtent = Asset->LocalHalfExtent;
	TArray<FColor> DenseSnapshot(CpuState.GetDense());

	CanonicalizeTask = UE::Tasks::Launch(UE_SOURCE_LOCATION,
		[Result, BrickCounts, StorageOrigin, LocalHalfExtent, Dense = MoveTemp(DenseSnapshot)]()
		{
			BuildCanonicalSourceFromDense(BrickCounts, Dense, Result->Build);
			Result->Build.Identity =
				ComputeCanonicalIdentity(Result->Build, StorageOrigin, LocalHalfExtent);
			Result->Identity = Result->Build.Identity;
			Result->bSucceeded = true;
		});
}

void FVoxelPaintSessionController::PollCanonicalizing()
{
	if (!CanonicalizeTask.IsValid() || !CanonicalizeTask.IsCompleted())
	{
		return;
	}
	// An interactive transaction (gizmo/slider drag) may span frames; opening the Finish
	// transaction now would append it into that unrelated undo group. Retry next tick.
	if (GEditor && GEditor->IsTransactionActive())
	{
		return;
	}
	TSharedPtr<FCanonicalizeResult, ESPMode::ThreadSafe> Result = MoveTemp(CanonicalizeResult);
	CanonicalizeResult.Reset();
	CanonicalizeTask = UE::Tasks::FTask();

	if (!Result || !Result->bSucceeded)
	{
		Machine.OnCanonicalizeFailed();
		ShowVoxelPaintSessionNotification(
			TEXT("Voxel paint Finish: canonicalization failed; the prior canonical state stays authoritative and the session returned to Active."));
		return;
	}
	if (!PreflightCanonicalResult(*Result))
	{
		Machine.OnCommitPreflightFailed();
		ShowVoxelPaintSessionNotification(
			TEXT("Voxel paint Finish: commit preflight failed; no transaction was opened and the session returned to Active."));
		return;
	}
	CommitFinish(MoveTemp(*Result));
}

bool FVoxelPaintSessionController::PreflightCanonicalResult(const FCanonicalizeResult& Result) const
{
	using namespace WorldVoxelPaint;
	UVoxelPaintVolumeData* Asset = ActiveAsset.Get();
	if (!Asset || !Result.Identity.IsValid())
	{
		return false;
	}
	if (Result.Build.Manifest.BrickCounts != Asset->GetBrickCounts())
	{
		return false;
	}
	const FIntVector DimsInBricks =
		ComputePackedSourceDimsInBricks(Result.Build.Manifest.PopulatedBricks.Num());
	const FIntVector PhysTexels = DimsInBricks * BrickSize;
	const int64 ExpectedBytes = (int64)PhysTexels.X * PhysTexels.Y * PhysTexels.Z * 4;
	return (int64)Result.Build.PackedSourceBGRA8.Num() == ExpectedBytes;
}

bool FVoxelPaintSessionController::BuildAfterPayload(FCanonicalizeResult&& Result,
	FVoxelPaintCanonicalPayload& OutAfter) const
{
	using namespace WorldVoxelPaint;
	UVoxelPaintVolumeData* Asset = ActiveAsset.Get();
	if (!Asset)
	{
		return false;
	}
	const bool bAfterInitialized = bAssetWasInitializedAtOpen
		|| TopologyMachine.GetState() != EVoxelPaintTopologyState::Uninitialized;

	OutAfter = FVoxelPaintCanonicalPayload();
	OutAfter.BrickCounts = Asset->GetBrickCounts();
	OutAfter.LocalHalfExtent = Asset->LocalHalfExtent;
	OutAfter.StorageOrigin = Asset->StorageOrigin;
	OutAfter.bInitialized = bAfterInitialized;
	if (bAfterInitialized)
	{
		OutAfter.PackedSourceBGRA8 = MoveTemp(Result.Build.PackedSourceBGRA8);
		OutAfter.ManifestBricks = MoveTemp(Result.Build.Manifest.PopulatedBricks);
		OutAfter.IdentityBytes.SetNumUninitialized(sizeof(Result.Identity.Hash.Hash));
		FMemory::Memcpy(OutAfter.IdentityBytes.GetData(), Result.Identity.Hash.Hash,
			sizeof(Result.Identity.Hash.Hash));
	}
	return bAfterInitialized;
}

void FVoxelPaintSessionController::CommitFinish(FCanonicalizeResult&& Result)
{
	using namespace WorldVoxelPaint;
	UVoxelPaintVolumeData* Asset = ActiveAsset.Get();
	if (!Asset)
	{
		ForceTeardownNow(false);
		return;
	}

	const FVoxelPaintCanonicalIdentity NewIdentity = Result.Identity;
	FVoxelPaintCanonicalPayload Baseline;
	Asset->CaptureCanonicalPayload(Baseline);
	FVoxelPaintCanonicalPayload After;
	const bool bAfterInitialized = BuildAfterPayload(MoveTemp(Result), After);
	CommittedIdentity = bAfterInitialized ? NewIdentity : FVoxelPaintCanonicalIdentity();

	TUniquePtr<FVoxelPaintFinishChange> Change = MakeUnique<FVoxelPaintFinishChange>(
		MoveTemp(Baseline), MoveTemp(After), Asset->GetName());
	FVoxelPaintFinishChange* RawChange = Change.Get();
	{
		FScopedTransaction Transaction(
			LOCTEXT("FinishSessionTransaction", "Finish Voxel Paint Session"));
		if (GUndo)
		{
			GUndo->StoreUndo(Asset, MoveTemp(Change));
		}
		Asset->InstallCanonicalState(RawChange->GetAfterState());
	}

	Machine.OnCommitInstalled();
	TopologyMachine.OnFinishInstalled();
	Machine.OnDerivingStarted();
	DerivingServicedSeconds = 0.0f;
	LastObservedPublicationOrdinal = 0;
	CommitPublicationOrdinal = 0;
	if (const FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
	{
		CommitPublicationOrdinal = Coordinator->GetSessionRenderStatus().CurrentPublicationOrdinal;
	}
	PushRenderBinding(EVoxelPaintSessionBindingPhase::PostCommit);

	if (bCurrentFinishIsAuto && bAssetSavedDuringSession)
	{
		ShowVoxelPaintSessionNotification(FString::Printf(
			TEXT("'%s' was saved before its paint session auto-finished; the committed paint is not in the saved file. Save the asset again."),
			*Asset->GetName()));
	}

	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Voxel paint session %llu: canonical state committed on '%s' (identity %s, initialized %d)."),
		SessionGeneration, *Asset->GetPathName(),
		bAfterInitialized ? *CommittedIdentity.Hash.ToString() : TEXT("<uninitialized>"),
		bAfterInitialized ? 1 : 0);
}

void FVoxelPaintSessionController::UpdateDerivedServiceClock(
	WorldVoxelPaint::EVoxelPaintSessionStage Stage, float DeltaSeconds)
{
	using namespace WorldVoxelPaint;
	const bool bWaitsOnPublication = Stage == EVoxelPaintSessionStage::Deriving
		|| Stage == EVoxelPaintSessionStage::Publication
		|| Stage == EVoxelPaintSessionStage::Retirement;
	if (!bWaitsOnPublication)
	{
		SecondsSinceDerivedServiced = 0.0f;
		DerivedServiceClockOrdinal = 0;
		return;
	}
	if (const FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
	{
		const uint64 Ordinal = Coordinator->GetSessionRenderStatus().CurrentPublicationOrdinal;
		const bool bOrdinalAdvanced = Ordinal != DerivedServiceClockOrdinal;
		DerivedServiceClockOrdinal = Ordinal;
		// An ordinal advanced by a DIFFERENT master world does not service this session's
		// derive lane (its origin lost mastership; the lane is stalled until it returns).
		if (bOrdinalAdvanced && Coordinator->GetCurrentMasterWorld() == OriginWorld.Get())
		{
			SecondsSinceDerivedServiced = 0.0f;
			return;
		}
	}
	SecondsSinceDerivedServiced += FMath::Min(DeltaSeconds, GServiceClockMaxTickStepSeconds);
}

void FVoxelPaintSessionController::PollDeriving(float DeltaSeconds)
{
	using namespace WorldVoxelPaint;
	FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated();
	if (!Coordinator)
	{
		return;
	}
	const FVoxelPaintSessionRenderStatus Status = Coordinator->GetSessionRenderStatus();
	if (Status.SessionGeneration != SessionGeneration)
	{
		return;
	}
	if (Status.bMatchingDataPublished && !Status.bSelectorPublishedLastFrame)
	{
		Machine.OnMatchingGenerationPublished();
		ClearRenderBinding();
		Machine.OnPublicationRetiring();
		UE_LOG(LogWorldVoxelPaint, Display,
			TEXT("Voxel paint session %llu: matching generation published - selector flipped off; retiring."),
			SessionGeneration);
		return;
	}
	const FVoxelPaintResidencyDiagnostics Diagnostics = Coordinator->GetResidencyDiagnostics();
	if (Diagnostics.PublicationOrdinal > CommitPublicationOrdinal)
	{
		if (const UVoxelPaintVolumeData* Asset = ActiveAsset.Get())
		{
			const FVoxelPaintAssetKey ActiveKey(Asset);
			for (const FVoxelPaintAssetDiagnosticsRow& Row : Diagnostics.PerAsset)
			{
				if (Row.Key != ActiveKey)
				{
					continue;
				}
				if (Row.bConfigError || Row.bOversized)
				{
					Machine.OnDerivedFailed();
					ShowVoxelPaintSessionNotification(
						TEXT("Voxel paint: the derived build did not complete (see log for build errors). The committed source is safe. Choose Retry or End Anyway."));
					UE_LOG(LogWorldVoxelPaint, Error,
						TEXT("Voxel paint session %llu: residency reports a configuration error on the committed asset - DerivedFailed (Retry / End Anyway)."),
						SessionGeneration);
					return;
				}
				break;
			}
		}
	}
	if (Status.CurrentPublicationOrdinal != LastObservedPublicationOrdinal)
	{
		LastObservedPublicationOrdinal = Status.CurrentPublicationOrdinal;
		DerivingServicedSeconds += DeltaSeconds;
		if (DerivingServicedSeconds > GDerivedWedgeTimeoutSeconds)
		{
			Machine.OnDerivedFailed();
			ShowVoxelPaintSessionNotification(
				TEXT("Voxel paint: the derived build did not complete (see log for build errors). The committed source is safe. Choose Retry or End Anyway."));
			UE_LOG(LogWorldVoxelPaint, Error,
				TEXT("Voxel paint session %llu: derived chain unresolved after %.0f serviced seconds - DerivedFailed (Retry / End Anyway)."),
				SessionGeneration, GDerivedWedgeTimeoutSeconds);
		}
	}
}

void FVoxelPaintSessionController::PollRetirement()
{
	FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated();
	if (!Coordinator)
	{
		return;
	}
	const FVoxelPaintSessionRenderStatus Status = Coordinator->GetSessionRenderStatus();
	if (Status.CurrentPublicationOrdinal <= Status.LastSelectorPublicationOrdinal)
	{
		return;
	}
	if (TransferEngine && TransferEngine->IsTransferInFlight())
	{
		return;
	}
	ReleaseSessionResourcesNow();
	Machine.OnRetirementComplete();
	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Voxel paint session %llu retired and closed."), SessionGeneration);
	CloseoutSession();
}

void FVoxelPaintSessionController::PollDeferredRelease(float DeltaSeconds)
{
	FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated();
	if (Coordinator)
	{
		const FVoxelPaintSessionRenderStatus Status = Coordinator->GetSessionRenderStatus();
		if (Status.CurrentPublicationOrdinal <= Status.LastSelectorPublicationOrdinal)
		{
			SecondsAwaitingDeferredRelease +=
				FMath::Min(DeltaSeconds, GServiceClockMaxTickStepSeconds);
			if (!bDeferredReleaseStallLogged
				&& SecondsAwaitingDeferredRelease >= GDeferredReleaseStallWarnSeconds)
			{
				bDeferredReleaseStallLogged = true;
				UE_LOG(LogWorldVoxelPaint, Warning,
					TEXT("Voxel paint: the ended session's GPU resources are still reserved after %.0f seconds waiting for one serviced frame (new sessions stay denied until then) - is the master world's viewport rendering (Realtime)?"),
					SecondsAwaitingDeferredRelease);
			}
			return;
		}
	}
	if (TransferEngine && TransferEngine->IsTransferInFlight())
	{
		return;
	}
	ReleaseSessionResourcesNow();
	bReleasePending = false;
	SecondsAwaitingDeferredRelease = 0.0f;
	bDeferredReleaseStallLogged = false;
}

bool FVoxelPaintSessionController::BeginMutation(const FVoxelPaintMutationDesc& Desc, FText& OutWhyNot)
{
	using namespace WorldVoxelPaint;
	check(IsInGameThread());
	if (!CanMutateNow(OutWhyNot))
	{
		return false;
	}

	int32 ReservationId = INDEX_NONE;
	if (!Journal.Reserve(Desc.JournalReserveBytes, ReservationId))
	{
		OutWhyNot = LOCTEXT("MutationJournalReserveFailed",
			"The stroke journal cannot reserve capacity for this operation; it was rejected unchanged.");
		return false;
	}
	int32 LedgerHandle = INDEX_NONE;
	if (Desc.GpuStagingBytes > 0
		&& !Ledger.Reserve(EVoxelPaintSessionGpuCategory::TransferStaging, Desc.GpuStagingBytes, LedgerHandle))
	{
		Journal.CancelReservation(ReservationId);
		OutWhyNot = LOCTEXT("MutationGpuReserveFailed",
			"The session GPU budget cannot reserve staging for this operation; it was rejected unchanged.");
		return false;
	}

	TopologyMachine.OnOpAdmitted(Desc.bNonZeroWriteIntent);
	bStrokeOpen = true;
	OpenStrokeJournalReservationId = ReservationId;
	OpenStrokeLedgerHandle = LedgerHandle;
	return true;
}

bool FVoxelPaintSessionController::GrowOpenStrokeReservation(int64 AdditionalBytes)
{
	check(IsInGameThread());
	if (!bStrokeOpen || OpenStrokeJournalReservationId == INDEX_NONE || AdditionalBytes < 0)
	{
		return false;
	}
	return Journal.GrowReservation(OpenStrokeJournalReservationId, AdditionalBytes);
}

void FVoxelPaintSessionController::CommitClosedStroke(WorldVoxelPaint::FVoxelPaintStrokeDelta&& Delta)
{
	check(IsInGameThread());
	if (!bStrokeOpen)
	{
		UE_LOG(LogWorldVoxelPaint, Warning,
			TEXT("CommitClosedStroke called with no open stroke - ignored."));
		return;
	}
	RecordAccumulatorFirstTouch(Delta);
	Journal.Commit(OpenStrokeJournalReservationId, MoveTemp(Delta));
	if (OpenStrokeLedgerHandle != INDEX_NONE)
	{
		Ledger.Release(OpenStrokeLedgerHandle);
	}
	bStrokeOpen = false;
	OpenStrokeJournalReservationId = INDEX_NONE;
	OpenStrokeLedgerHandle = INDEX_NONE;
}

void FVoxelPaintSessionController::CancelOpenStroke()
{
	check(IsInGameThread());
	if (!bStrokeOpen)
	{
		return;
	}
	Journal.CancelReservation(OpenStrokeJournalReservationId);
	if (OpenStrokeLedgerHandle != INDEX_NONE)
	{
		Ledger.Release(OpenStrokeLedgerHandle);
	}
	bStrokeOpen = false;
	OpenStrokeJournalReservationId = INDEX_NONE;
	OpenStrokeLedgerHandle = INDEX_NONE;
}

void FVoxelPaintSessionController::RecordAccumulatorFirstTouch(
	const WorldVoxelPaint::FVoxelPaintStrokeDelta& Delta)
{
	using namespace WorldVoxelPaint;
	FVoxelPaintPackedBrickTransfer Page;
	for (const FVoxelPaintBrickDelta& Brick : Delta.Bricks)
	{
		if (Accumulator.HasBrick(Brick.BrickIndex))
		{
			continue;
		}
		const uint32 BrickIndex = Brick.BrickIndex;
		CpuState.BuildPackedTransfer(TConstArrayView<uint32>(&BrickIndex, 1), Page);
		if (Page.Texels.Num() != TexelsPerUniqueBrick)
		{
			continue;
		}
		int32 ValueCursor = 0;
		for (int32 Bit = 0; Bit < TexelsPerUniqueBrick; ++Bit)
		{
			const int32 Word = Bit / 64;
			const uint64 Mask = 1ull << (Bit % 64);
			if (Brick.TouchedMask.IsValidIndex(Word) && (Brick.TouchedMask[Word] & Mask))
			{
				if (Brick.Before.IsValidIndex(ValueCursor))
				{
					Page.Texels[Bit] = Brick.Before[ValueCursor];
				}
				++ValueCursor;
			}
		}
		Accumulator.RecordFirstTouch(BrickIndex, Page.Texels);
	}
}

bool FVoxelPaintSessionController::UploadDeltaBricksFromCpuState(
	const WorldVoxelPaint::FVoxelPaintStrokeDelta& Delta)
{
	using namespace WorldVoxelPaint;
	if (!Resources || !TransferEngine)
	{
		return false;
	}
	TArray<uint32> Bricks;
	Bricks.Reserve(Delta.Bricks.Num());
	for (const FVoxelPaintBrickDelta& Brick : Delta.Bricks)
	{
		Bricks.Add(Brick.BrickIndex);
	}
	if (Bricks.Num() == 0)
	{
		return true;
	}
	FVoxelPaintPackedBrickTransfer Transfer;
	CpuState.BuildPackedTransfer(Bricks, Transfer);
	TransferEngine->UploadAndPublish(*Resources, MoveTemp(Transfer), Ledger,
		[Gen = SessionGeneration, Bricks = MoveTemp(Bricks)](bool bOk) mutable
		{
			if (bOk)
			{
				return;
			}
			FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
			if (Controller && Gen == Controller->SessionGeneration)
			{
				Controller->NotifyAuthoritativeUploadFailed(MoveTemp(Bricks));
			}
		});
	return true;
}

void FVoxelPaintSessionController::NotifyAuthoritativeUploadFailed(TArray<uint32>&& Bricks)
{
	check(IsInGameThread());
	if (!IsLiveSessionOpen())
	{
		return;
	}
	for (uint32 Brick : Bricks)
	{
		UnresolvedAuthoritativeUploadBricks.AddUnique(Brick);
	}
	if (AuthoritativeUploadRetriesRemaining <= 0)
	{
		AuthoritativeUploadRetriesRemaining = 3;
	}
	UE_LOG(LogWorldVoxelPaint, Error,
		TEXT("Voxel paint session %llu: authoritative GPU upload failed over %d bricks; mutations blocked until the re-upload from exact CPU state succeeds."),
		SessionGeneration, UnresolvedAuthoritativeUploadBricks.Num());
}

void FVoxelPaintSessionController::TryRepairAuthoritativeUpload()
{
	using namespace WorldVoxelPaint;
	if (!IsLiveSessionOpen())
	{
		UnresolvedAuthoritativeUploadBricks.Reset();
		AuthoritativeUploadRetriesRemaining = 0;
		return;
	}
	if (!Resources || !TransferEngine || TransferEngine->IsTransferInFlight() || bStrokeOpen)
	{
		return;
	}
	if (AuthoritativeUploadRetriesRemaining <= 0)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("Voxel paint session %llu: authoritative GPU re-upload retries exhausted; forcing teardown (CPU state remains exact)."),
			SessionGeneration);
		ForceTeardownNow(true);
		return;
	}
	--AuthoritativeUploadRetriesRemaining;
	TArray<uint32> Bricks = UnresolvedAuthoritativeUploadBricks;
	Bricks.Sort();
	FVoxelPaintPackedBrickTransfer Transfer;
	CpuState.BuildPackedTransfer(Bricks, Transfer);
	TransferEngine->UploadAndPublish(*Resources, MoveTemp(Transfer), Ledger,
		[Gen = SessionGeneration, Bricks = MoveTemp(Bricks)](bool bOk)
		{
			FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
			if (bOk && Controller && Gen == Controller->SessionGeneration)
			{
				Controller->OnAuthoritativeUploadRepaired(Bricks);
			}
		});
}

void FVoxelPaintSessionController::OnAuthoritativeUploadRepaired(const TArray<uint32>& Bricks)
{
	for (uint32 Brick : Bricks)
	{
		UnresolvedAuthoritativeUploadBricks.Remove(Brick);
	}
	if (UnresolvedAuthoritativeUploadBricks.IsEmpty())
	{
		AuthoritativeUploadRetriesRemaining = 0;
		UE_LOG(LogWorldVoxelPaint, Display,
			TEXT("Voxel paint session %llu: authoritative GPU state repaired from exact CPU state."),
			SessionGeneration);
	}
}

bool FVoxelPaintSessionController::UndoInSession()
{
	using namespace WorldVoxelPaint;
	check(IsInGameThread());
	if (Machine.GetStage() != EVoxelPaintSessionStage::Active || bStrokeOpen)
	{
		return false;
	}
	if (TransferEngine && TransferEngine->IsTransferInFlight())
	{
		return false;
	}
	if (!bHydrationComplete || bDiscardPending || !UnresolvedAuthoritativeUploadBricks.IsEmpty())
	{
		return false;
	}
	const FVoxelPaintStrokeDelta* Delta = Journal.PeekUndo();
	if (!Delta)
	{
		return false;
	}
	CpuState.ApplyDelta(*Delta, false);
	UploadDeltaBricksFromCpuState(*Delta);
	Journal.AcknowledgeUndo();
	return true;
}

bool FVoxelPaintSessionController::RedoInSession()
{
	using namespace WorldVoxelPaint;
	check(IsInGameThread());
	if (Machine.GetStage() != EVoxelPaintSessionStage::Active || bStrokeOpen)
	{
		return false;
	}
	if (TransferEngine && TransferEngine->IsTransferInFlight())
	{
		return false;
	}
	if (!bHydrationComplete || bDiscardPending || !UnresolvedAuthoritativeUploadBricks.IsEmpty())
	{
		return false;
	}
	const FVoxelPaintStrokeDelta* Delta = Journal.PeekRedo();
	if (!Delta)
	{
		return false;
	}
	CpuState.ApplyDelta(*Delta, true);
	UploadDeltaBricksFromCpuState(*Delta);
	Journal.AcknowledgeRedo();
	return true;
}

void FVoxelPaintSessionController::NotifyStrokePipelineBound(FVoxelPaintStrokePipeline* Pipeline)
{
	check(IsInGameThread());
	BoundStrokePipeline = Pipeline;
}

void FVoxelPaintSessionController::NotifyStrokePipelineUnbound(FVoxelPaintStrokePipeline* Pipeline)
{
	check(IsInGameThread());
	if (BoundStrokePipeline == Pipeline)
	{
		BoundStrokePipeline = nullptr;
	}
}

WorldVoxelPaint::FVoxelPaintSessionCpuState* FVoxelPaintSessionController::GetSessionCpuState()
{
	return IsLiveSessionOpen() ? &CpuState : nullptr;
}

FVoxelPaintSessionResources* FVoxelPaintSessionController::GetSessionResources()
{
	return IsLiveSessionOpen() ? Resources.Get() : nullptr;
}

FVoxelPaintSessionTransferEngine* FVoxelPaintSessionController::GetTransferEngine()
{
	return IsLiveSessionOpen() ? TransferEngine.Get() : nullptr;
}

WorldVoxelPaint::FVoxelPaintSessionBudgetLedger* FVoxelPaintSessionController::GetBudgetLedger()
{
	return &Ledger;
}

uint64 FVoxelPaintSessionController::GetSessionGeneration() const
{
	return SessionGeneration;
}

void FVoxelPaintSessionController::PushRenderBinding(EVoxelPaintSessionBindingPhase Phase)
{
	FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated();
	UVoxelPaintVolumeData* Asset = ActiveAsset.Get();
	if (!Coordinator || !Asset || !Resources)
	{
		return;
	}
	FVoxelPaintSessionRenderBinding Binding;
	Binding.AssetKey = FVoxelPaintAssetKey(Asset);
	Binding.OriginWorldKey = OriginWorldKey;
	Binding.SessionGeneration = SessionGeneration;
	Binding.Phase = Phase;
	Binding.CommittedIdentity = CommittedIdentity;
	Binding.PublishedTexture = Resources->GetPublishedRHI();
	Binding.Resolution = SessionResolution;
	Coordinator->SetSessionRenderBinding(Binding);
}

void FVoxelPaintSessionController::ClearRenderBinding()
{
	if (FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
	{
		if (SessionGeneration != 0)
		{
			Coordinator->ClearSessionRenderBinding(SessionGeneration);
		}
	}
}

void FVoxelPaintSessionController::SynchronousCommitCpuClosedWork()
{
	using namespace WorldVoxelPaint;
	UVoxelPaintVolumeData* Asset = ActiveAsset.Get();
	if (!Asset)
	{
		return;
	}

	FCanonicalizeResult Result;
	BuildCanonicalSourceFromDense(Asset->GetBrickCounts(), CpuState.GetDense(), Result.Build);
	Result.Build.Identity =
		ComputeCanonicalIdentity(Result.Build, Asset->StorageOrigin, Asset->LocalHalfExtent);
	Result.Identity = Result.Build.Identity;
	Result.bSucceeded = true;

	if (!PreflightCanonicalResult(Result))
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("Voxel paint session %llu: forced-teardown commit preflight failed - the prior canonical state stays authoritative."),
			SessionGeneration);
		return;
	}

	FVoxelPaintCanonicalPayload After;
	BuildAfterPayload(MoveTemp(Result), After);
	Asset->InstallCanonicalState(After);
	TopologyMachine.OnFinishInstalled();
	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Voxel paint session %llu: forced teardown committed CPU-closed work on '%s' (no undo entry)."),
		SessionGeneration, *Asset->GetPathName());
}

void FVoxelPaintSessionController::AutoFinish(const TCHAR* Reason)
{
	using namespace WorldVoxelPaint;
	if (Machine.GetStage() != EVoxelPaintSessionStage::Active || bDiscardPending)
	{
		return;
	}
	ResolveOpenStrokeForFinish(Reason);
	Machine.RequestFinish();
	bCurrentFinishIsAuto = true;
	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Voxel paint session %llu: auto-Finish started (%s)."), SessionGeneration, Reason);
}

void FVoxelPaintSessionController::ResolveOpenStrokeForFinish(const TCHAR* Reason)
{
	if (!bStrokeOpen)
	{
		return;
	}
	if (!BoundStrokePipeline)
	{
		DropOpenStroke(Reason);
		return;
	}
	if (BoundStrokePipeline->IsStrokeOpen())
	{
		UE_LOG(LogWorldVoxelPaint, Warning,
			TEXT("Voxel paint session %llu: open stroke cancelled (%s); Published/Working restored from exact CPU state."),
			SessionGeneration, Reason);
		BoundStrokePipeline->CancelStroke();
	}
	// else: the stroke is already closed with its readback pending; the reservation stays and
	// Sealing's transfer drain delivers CommitClosedStroke before OnSealed.
}

void FVoxelPaintSessionController::ForceTeardownNow(bool bPreserveCpuClosedWork)
{
	using namespace WorldVoxelPaint;
	if (bInForcedTeardown)
	{
		return;
	}
	TGuardValue<bool> ReentrancyGuard(bInForcedTeardown, true);

	if (Machine.GetStage() == EVoxelPaintSessionStage::Closed)
	{
		if (bReleasePending)
		{
			DrainTransfersForTeardown();
			ReleaseSessionResourcesNow();
			bReleasePending = false;
			SecondsAwaitingDeferredRelease = 0.0f;
			bDeferredReleaseStallLogged = false;
		}
		return;
	}

	if (bStrokeOpen)
	{
		DropOpenStroke(TEXT("forced teardown"));
	}

	bool bCarveOut = false;
	if (Machine.IsPreCommit())
	{
		bCarveOut = Machine.WasFirstSession();
		if (bCarveOut)
		{
			TopologyMachine.OnForcedTeardownNeverCommitted();
			UE_LOG(LogWorldVoxelPaint, Warning,
				TEXT("Voxel paint session %llu: never-committed first session ended by forced teardown WITHOUT commit (accepted carve-out); closed strokes were discarded."),
				SessionGeneration);
		}
		else if (bPreserveCpuClosedWork && !bDiscardPending)
		{
			SynchronousCommitCpuClosedWork();
		}
	}

	Machine.ForceTeardown(bCarveOut);
	ClearRenderBinding();
	DrainTransfersForTeardown();
	ReleaseSessionResourcesNow();
	bReleasePending = false;
	SecondsAwaitingDeferredRelease = 0.0f;
	bDeferredReleaseStallLogged = false;
	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Voxel paint session %llu: forced teardown complete."), SessionGeneration);
	CloseoutSession();
}

void FVoxelPaintSessionController::DrainTransfersForTeardown()
{
	if (!TransferEngine)
	{
		return;
	}
	// Enqueued session render commands (scatter/gather/apply/region copies, RHI-ref resolves)
	// capture raw Resources/engine/ledger pointers; they must EXECUTE before those objects are
	// freed. Their game-thread finalizations then ride queued tasks, which must also run before
	// the drain can resolve. Teardown-only stall, accepted.
	FlushRenderingCommands();
	TSharedRef<bool> Drained = MakeShared<bool>(false);
	TransferEngine->CancelAndDrain([Drained]()
	{
		*Drained = true;
	});
	if (!*Drained)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}
	ensureMsgf(*Drained && !TransferEngine->IsTransferInFlight(),
		TEXT("Forced teardown could not drain session transfers; releasing anyway."));
}

void FVoxelPaintSessionController::DropOpenStroke(const TCHAR* Reason)
{
	if (!bStrokeOpen)
	{
		return;
	}
	UE_LOG(LogWorldVoxelPaint, Warning,
		TEXT("Voxel paint session %llu: open stroke dropped (%s); its unclosed stamps never reached CPU state."),
		SessionGeneration, Reason);
	CancelOpenStroke();
}

void FVoxelPaintSessionController::ReleaseSessionResourcesNow()
{
	TransferEngine.Reset();
	if (Resources)
	{
		Resources->Release(Ledger);
		Resources.Reset();
	}
}

void FVoxelPaintSessionController::CloseoutSession()
{
	using namespace WorldVoxelPaint;
	if (FVoxelPaintStrokePipeline* Pipeline = BoundStrokePipeline)
	{
		BoundStrokePipeline = nullptr;
		Pipeline->UnbindSession();
	}
	if (UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(OriginWorld.Get()))
	{
		Subsystem->ClearAllForcedVariants();
	}
	ActiveAsset.Reset();
	OriginWorld.Reset();
	OriginWorldKey = FObjectKey();
	SessionResolution = FIntVector::ZeroValue;
	CommittedIdentity = FVoxelPaintCanonicalIdentity();
	bAssetWasInitializedAtOpen = false;
	FrozenSharingMode = EVoxelPaintAssetSharingMode::Private;
	FrozenOwnerActorGuid.Invalidate();
	bStrokeOpen = false;
	OpenStrokeJournalReservationId = INDEX_NONE;
	OpenStrokeLedgerHandle = INDEX_NONE;
	bHydrationIssued = false;
	bHydrationComplete = false;
	bDiscardPending = false;
	UnresolvedAuthoritativeUploadBricks.Reset();
	AuthoritativeUploadRetriesRemaining = 0;
	bAssetSavedDuringSession = false;
	bCurrentFinishIsAuto = false;
	DerivingServicedSeconds = 0.0f;
	LastObservedPublicationOrdinal = 0;
	CommitPublicationOrdinal = 0;
	Journal = FVoxelPaintStrokeJournal();
	Accumulator = FVoxelPaintBaselineAccumulator();
	CpuState = FVoxelPaintSessionCpuState();
	TopologyMachine = FVoxelPaintTopologyLockMachine();
	CanonicalizeResult.Reset();
	CanonicalizeTask = UE::Tasks::FTask();
	SessionGeneration = 0;
}

void FVoxelPaintSessionController::UpdateStats() const
{
	SET_DWORD_STAT(STAT_VoxelPaint_SessionStage, (int32)Machine.GetStage());
	SET_DWORD_STAT(STAT_VoxelPaint_SessionJournalBytes, Journal.GetRetainedBytes());
	SET_DWORD_STAT(STAT_VoxelPaint_SessionJournalCapacityBytes, Journal.GetCapacityBytes());
	SET_DWORD_STAT(STAT_VoxelPaint_SessionGpuBytes, Ledger.GetUsedBytes());
	SET_DWORD_STAT(STAT_VoxelPaint_SessionGpuBudgetBytes, Ledger.GetBudgetBytes());
}

void FVoxelPaintSessionController::HandleWorldCleanup(UWorld* World, bool bSessionEnded,
	bool bCleanupResources)
{
	if (!IsLiveSessionOpen() || FObjectKey(World) != OriginWorldKey)
	{
		return;
	}
	AutoFinish(TEXT("origin world cleanup"));
}

void FVoxelPaintSessionController::HandleMapChange(uint32 MapChangeFlags)
{
	if (IsLiveSessionOpen())
	{
		AutoFinish(TEXT("map change"));
	}
}

void FVoxelPaintSessionController::HandleEnginePreExit()
{
	ForceTeardownNow(true);
}

void FVoxelPaintSessionController::HandlePackageSaved(const FString& PackageFilename,
	UPackage* Package, FObjectPostSaveContext ObjectSaveContext)
{
	if (!IsLiveSessionOpen() || Machine.HasEverCommitted())
	{
		return;
	}
	const UVoxelPaintVolumeData* Asset = ActiveAsset.Get();
	if (Asset && Package && Asset->GetPackage() == Package)
	{
		bAssetSavedDuringSession = true;
	}
}

#undef LOCTEXT_NAMESPACE
