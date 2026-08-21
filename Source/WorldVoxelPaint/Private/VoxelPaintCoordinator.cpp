// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintCoordinator.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GlobalRenderResources.h"
#include "HAL/IConsoleManager.h"
#include "Math/DoubleFloat.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SceneInterface.h"
#include "SceneRendererInterface.h"
#include "SceneView.h"
#include "UnrealClient.h"
#include "VoxelPaintDeveloperSettings.h"
#include "VoxelPaintExternalSampling.h"
#include "VoxelPaintGridRenderer.h"
#include "VoxelPaintOwnershipTicket.h"
#include "VoxelPaintPoolRenderer.h"
#include "VoxelPaintPrimaryViewport.h"
#include "VoxelPaintPublicationAssembler.h"
#include "VoxelPaintSceneUniformBuffer.h"
#include "VoxelPaintStats.h"
#include "VoxelPaintVolumeData.h"
#include "VoxelPaintWorldSubsystem.h"
#include "WorldVoxelPaintConstants.h"
#if WITH_EDITOR
#include "Misc/ScopeLock.h"
#include "RHIGPUReadback.h"
#endif
#if !UE_BUILD_SHIPPING
#include "FXRenderingUtils.h"
#include "GlobalShader.h"
#include "PixelShaderUtils.h"
#include "RHIStaticStates.h"
#include "SceneRenderTargetParameters.h"
#include "ShowFlags.h"
#include "VoxelPaintHeatmapShader.h"
#endif

static TAutoConsoleVariable<int32> CVarVoxelPaintEnable(
	TEXT("r.VoxelPaint.Enable"),
	1,
	TEXT("Enable World Voxel Paint evaluation. 0 publishes neutral Scene UB parameters (node passes through)."),
	ECVF_RenderThreadSafe);

#if !UE_BUILD_SHIPPING
static TAutoConsoleVariable<int32> CVarVoxelPaintHeatmap(
	TEXT("r.VoxelPaint.Heatmap"),
	0,
	TEXT("Grid-cost heatmap overlay; also requires the WorldVoxelPaint show flag. 0 = off, 1 = per-cell volume count, 2 = per-cell estimated evaluation cost."),
	ECVF_RenderThreadSafe);
#endif

DECLARE_GPU_STAT_NAMED(VoxelPaintPublish, TEXT("VoxelPaint Publish"));
#if !UE_BUILD_SHIPPING
DECLARE_GPU_STAT_NAMED(VoxelPaintHeatmap, TEXT("VoxelPaint Heatmap"));
#endif

static FVoxelPaintCoordinator* GVoxelPaintCoordinator = nullptr;

namespace WorldVoxelPaint
{
	bool ShouldRenderAsMaster(const FVoxelPaintOwnershipTicket* Ticket, uint64 RenderThreadCurrentEpoch)
	{
		return Ticket != nullptr
			&& Ticket->Verdict == EVoxelPaintFamilyVerdict::MasterPrimary
			&& Ticket->OwnerEpoch == RenderThreadCurrentEpoch;
	}
}

TSharedPtr<FVoxelPaintCoordinator, ESPMode::ThreadSafe> FVoxelPaintCoordinator::CreateAndRegister()
{
	check(GVoxelPaintCoordinator == nullptr);
	return FSceneViewExtensions::NewExtension<FVoxelPaintCoordinator>();
}

FVoxelPaintCoordinator* FVoxelPaintCoordinator::GetIfCreated()
{
	return GVoxelPaintCoordinator;
}

void FVoxelPaintCoordinator::Deactivate()
{
	if (GVoxelPaintCoordinator)
	{
		if (GVoxelPaintCoordinator->SettingsChangedHandle.IsValid())
		{
			WorldVoxelPaint::OnDeveloperSettingsChanged().Remove(GVoxelPaintCoordinator->SettingsChangedHandle);
			GVoxelPaintCoordinator->SettingsChangedHandle.Reset();
		}
		GVoxelPaintCoordinator = nullptr;
	}
}

FVoxelPaintCoordinator::FVoxelPaintCoordinator(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
	const UVoxelPaintDeveloperSettings* Settings = GetDefault<UVoxelPaintDeveloperSettings>();
	PoolCapacityBytes = Settings->GetBrickPoolCapacityBytes();
	PageTableCapacityBytes = Settings->GetPageTableCapacityBytes();
	RefreshDerivedSettings();
	SettingsChangedHandle = WorldVoxelPaint::OnDeveloperSettingsChanged().AddRaw(
		this, &FVoxelPaintCoordinator::OnSettingsChanged);
	GameIdentity.OnEpochAdvance(OwnerEpoch);
#if WITH_EDITOR
	RT.GridReadiness.Initialize(WorldVoxelPaint::GridReadinessReadbackSlots);
	RT.GridReadbackSlots.SetNum(WorldVoxelPaint::GridReadinessReadbackSlots);
#endif
	SET_DWORD_STAT(STAT_VoxelPaint_OwnerEpoch, static_cast<int64>(OwnerEpoch));
	SET_DWORD_STAT(STAT_VoxelPaint_MasterHandoffState, 0);
	GVoxelPaintCoordinator = this;
}

FVoxelPaintCoordinator::~FVoxelPaintCoordinator()
{
	// Backstop only: the last shared reference can be released off the game thread; the module
	// clears the global via Deactivate() on the game thread before releasing ownership.
	if (GVoxelPaintCoordinator == this)
	{
		if (SettingsChangedHandle.IsValid())
		{
			WorldVoxelPaint::OnDeveloperSettingsChanged().Remove(SettingsChangedHandle);
			SettingsChangedHandle.Reset();
		}
		GVoxelPaintCoordinator = nullptr;
	}
}

void FVoxelPaintCoordinator::OnSettingsChanged()
{
	bSettingsDirty = true;
}

void FVoxelPaintCoordinator::RefreshDerivedSettings()
{
	const UVoxelPaintDeveloperSettings* Settings = GetDefault<UVoxelPaintDeveloperSettings>();
	GridCapacity = WorldVoxelPaint::DeriveGridCapacity(*Settings);
	CellSizeXY = Settings->GridCellSizeXY;
	CellSizeZ = Settings->GridCellSizeZ;
	EvaluationRadius = Settings->EvaluationRadius;
	GuardBandWidth = Settings->GuardBandWidth;
	if (!GridCapacity.bValid)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("Derived grid capacity invalid (WindowCellsCap=%dx%dx%d CCap=%u ceiling=%u; GridCellSizeXY=%f GridCellSizeZ=%f EvaluationRadius=%f GuardBandWidth=%f DefaultGridZ=[%f,%f]); voxel paint renders neutral until the settings change."),
			GridCapacity.WindowCellsCap.X, GridCapacity.WindowCellsCap.Y, GridCapacity.WindowCellsCap.Z,
			GridCapacity.CCap, WorldVoxelPaint::GridCandidateCapCeiling,
			Settings->GridCellSizeXY, Settings->GridCellSizeZ, Settings->EvaluationRadius,
			Settings->GuardBandWidth, Settings->DefaultGridZMin, Settings->DefaultGridZMax);
		SET_DWORD_STAT(STAT_VoxelPaint_GridCandidateCap, 0);
		SET_DWORD_STAT(STAT_VoxelPaint_GridLiveCells, 0);
		SET_DWORD_STAT(STAT_VoxelPaint_GridEmitUpperBound, 0);
		SET_DWORD_STAT(STAT_VoxelPaint_GridSteadyBytes, 0);
		SET_DWORD_STAT(STAT_VoxelPaint_GridRebuildScratchBytes, 0);
	}
}

FVoxelPaintCoordinator::FResolvedOwnership FVoxelPaintCoordinator::ResolveOwnership()
{
	FResolvedOwnership Resolved;
	if (!GEngine)
	{
		return Resolved;
	}

	bool bAnyPIE = false;
	const FWorldContext* PrimaryPIEContext = nullptr;
	const UWorld* EditorWorld = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE)
		{
			bAnyPIE = true;
			if (Context.bIsPrimaryPIEInstance && PrimaryPIEContext == nullptr && Context.World())
			{
				PrimaryPIEContext = &Context;
			}
		}
		else if (Context.WorldType == EWorldType::Editor && EditorWorld == nullptr)
		{
			EditorWorld = Context.World();
		}
	}

	if (bAnyPIE)
	{
		if (PrimaryPIEContext)
		{
			Resolved.MasterWorld = PrimaryPIEContext->World();
			if (PrimaryPIEContext->GameViewport)
			{
				Resolved.PrimaryViewport = PrimaryPIEContext->GameViewport->Viewport;
			}
			else
			{
				Resolved.PrimaryViewport = WorldVoxelPaint::GetActivePrimaryViewport();
			}
		}
		return Resolved;
	}
	if (GIsEditor)
	{
		Resolved.MasterWorld = EditorWorld;
		Resolved.PrimaryViewport = EditorWorld ? WorldVoxelPaint::GetActivePrimaryViewport() : nullptr;
		return Resolved;
	}
	if (GEngine->GameViewport)
	{
		Resolved.MasterWorld = GEngine->GameViewport->GetWorld();
		Resolved.PrimaryViewport = GEngine->GameViewport->Viewport;
	}
	return Resolved;
}

void FVoxelPaintCoordinator::AdvanceEpoch()
{
	++OwnerEpoch;
	const uint64 NewEpoch = OwnerEpoch;
	ENQUEUE_RENDER_COMMAND(VoxelPaintEpochFence)(
		[this, NewEpoch](FRHICommandListImmediate&)
		{
			RT.CurrentEpoch = NewEpoch;
			RT.ResidencyBindingEvents = 0;
			RT.bActivatedThisEpoch = false;
			RT.Identity.OnEpochAdvance(NewEpoch);
			if (RT.bResidencyInitialized)
			{
				RT.Residency->NotifyEpochAdvance(NewEpoch);
			}
#if WITH_EDITOR
			RT.GridReadiness.OnExpectedGridChanged(
				WorldVoxelPaint::FVoxelPaintGridReadinessToken{NewEpoch, 1});
#endif
		});
}

void FVoxelPaintCoordinator::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	if (bSettingsDirty)
	{
		bSettingsDirty = false;
		RefreshDerivedSettings();
		bZEnvelopeDirty = true;
		bCapacityShipPending = true;
		ExpectedWindow.bValid = false;
		GameIdentity.OnZEnvelopeOrSettingsChange();
	}

	const FResolvedOwnership Resolved = ResolveOwnership();
	const UWorld* CurrentMaster = bHasMaster ? CurrentMasterWorld.GetEvenIfUnreachable() : nullptr;
	if (CurrentMaster != Resolved.MasterWorld)
	{
		AdvanceEpoch();
		CurrentMasterWorld = Resolved.MasterWorld;
		bHasMaster = Resolved.MasterWorld != nullptr;
		CurrentPrimaryViewport = Resolved.PrimaryViewport;
		ExpectedWindow = WorldVoxelPaint::FVoxelPaintExpectedWindow();
		bZEnvelopeDirty = true;
		GameIdentity.OnEpochAdvance(OwnerEpoch);
		LastShippedIdentity = FVoxelPaintPublicationIdentity();
		LastQueuedWorld.Reset();
		LastQueuedLayoutGeneration = 0;
		LastQueuedBindingGeneration = 0;
		CaptureBook.Empty();
#if WITH_EDITOR
		LastMasterPrimaryWorld.Reset();
#endif
		SET_DWORD_STAT(STAT_VoxelPaint_OwnerEpoch, static_cast<int64>(OwnerEpoch));
		SET_DWORD_STAT(STAT_VoxelPaint_MasterHandoffState, bHasMaster ? 1 : 0);
		UE_LOG(LogWorldVoxelPaint, Log, TEXT("Master world: %s (epoch %llu)"),
			Resolved.MasterWorld ? *Resolved.MasterWorld->GetName() : TEXT("<none>"), OwnerEpoch);
	}
	else if (bHasMaster && Resolved.PrimaryViewport != CurrentPrimaryViewport)
	{
		CurrentPrimaryViewport = Resolved.PrimaryViewport;
		ExpectedWindow.bValid = false;
		GameIdentity.OnGuardConsumed();
	}

	EVoxelPaintFamilyVerdict Verdict = EVoxelPaintFamilyVerdict::Neutral;
	const UWorld* MasterWorld = bHasMaster ? CurrentMasterWorld.Get() : nullptr;
	if (MasterWorld && CurrentPrimaryViewport && GridCapacity.bValid
		&& InViewFamily.Scene && InViewFamily.Scene->GetWorld() == MasterWorld
		&& InViewFamily.RenderTarget == static_cast<const FRenderTarget*>(CurrentPrimaryViewport)
		&& InViewFamily.Views.Num() > 0)
	{
		bool bEligible = true;
		for (const FSceneView* View : InViewFamily.Views)
		{
			if (View && (View->bIsSceneCapture || View->bIsReflectionCapture || View->bIsPlanarReflection))
			{
				bEligible = false;
				break;
			}
		}
		if (bEligible)
		{
			if (UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(MasterWorld))
			{
				Verdict = EVoxelPaintFamilyVerdict::MasterPrimary;
				ServiceMasterPrimary(InViewFamily, Subsystem);
			}
		}
	}

	FVoxelPaintOwnershipTicket* Ticket = InViewFamily.GetOrCreateExtentionData<FVoxelPaintOwnershipTicket>();
	Ticket->OwnerEpoch = OwnerEpoch;
	Ticket->Verdict = Verdict;
}

void FVoxelPaintCoordinator::ServiceMasterPrimary(FSceneViewFamily& InViewFamily, UVoxelPaintWorldSubsystem* Subsystem)
{
	const UWorld* MasterWorld = CurrentMasterWorld.Get();
#if WITH_EDITOR
	LastMasterPrimaryWorld = MasterWorld;
	{
		const FVoxelPaintSessionRenderStatus Status = GetSessionRenderStatus();
		SET_DWORD_STAT(STAT_VoxelPaint_GridReadiness, Status.GridReadiness);
		if (Status.GridRecoveryRequestCount > LastConsumedGridRecoveryRequests)
		{
			LastConsumedGridRecoveryRequests = Status.GridRecoveryRequestCount;
			RequestGridRecoveryRebuild();
		}
	}
#endif
	// Force past the same-frame sweep gate: this is the contractual pre-snapshot sweep, so
	// dead-component demotion must land here even when a mutating call already swept this frame.
	Subsystem->ExpireStaleReservations(/*bForceSweep*/ true);

	const FVector CameraLWC = InViewFamily.Views[0]->ViewLocation;
	const UVoxelPaintDeveloperSettings* Settings = GetDefault<UVoxelPaintDeveloperSettings>();

	const WorldVoxelPaint::FVoxelPaintZEnvelope NewEnvelope =
		WorldVoxelPaint::ResolveZEnvelope(MasterWorld, *Settings);
	if (bZEnvelopeDirty)
	{
		bZEnvelopeDirty = false;
		ZEnvelope = NewEnvelope;
		ExpectedWindow.bValid = false;
	}
	else if (NewEnvelope.ZMin != ZEnvelope.ZMin || NewEnvelope.ZMax != ZEnvelope.ZMax)
	{
		ZEnvelope = NewEnvelope;
		ExpectedWindow.bValid = false;
		GameIdentity.OnZEnvelopeOrSettingsChange();
	}

	bool bWindowChanged = false;
	if (!ExpectedWindow.bValid)
	{
		ExpectedWindow = WorldVoxelPaint::MakeExpectedWindow(
			CameraLWC, ZEnvelope, GridCapacity, CellSizeXY, CellSizeZ);
		bWindowChanged = true;
	}
	else if (WorldVoxelPaint::IsGuardConsumed(ExpectedWindow, CameraLWC, GuardBandWidth))
	{
		ExpectedWindow = WorldVoxelPaint::MakeExpectedWindow(
			CameraLWC, ZEnvelope, GridCapacity, CellSizeXY, CellSizeZ);
		GameIdentity.OnGuardConsumed();
		bWindowChanged = true;
	}

	const uint64 LayoutGeneration = Subsystem->GetLayoutGeneration();
	const uint64 BindingGeneration = Subsystem->GetBindingGeneration();
	bool bPack = false;
	if (LastQueuedWorld.Get() != MasterWorld
		|| LastQueuedLayoutGeneration != LayoutGeneration
		|| LastQueuedBindingGeneration != BindingGeneration)
	{
		if (LastQueuedWorld.Get() == MasterWorld)
		{
			if (LastQueuedLayoutGeneration != LayoutGeneration)
			{
				GameIdentity.OnRegistryLayoutChange();
			}
			if (LastQueuedBindingGeneration != BindingGeneration)
			{
				GameIdentity.OnRegistryBindingChange();
			}
		}
		bPack = true;
	}
	if (!bPack && CaptureBook.HasPendingCaptures())
	{
		bPack = true;
	}

	FStateShipment Shipment;
	bool bShip = false;
	if (bPack)
	{
		INC_DWORD_STAT(STAT_VoxelPaint_SnapshotPacks);
		Subsystem->BuildDesiredSnapshot(Shipment.Placements);
		BuildDesiredAssets(Shipment);
		Shipment.bHasSnapshot = true;
		LastQueuedWorld = MasterWorld;
		LastQueuedLayoutGeneration = LayoutGeneration;
		LastQueuedBindingGeneration = BindingGeneration;
		bShip = true;
	}
	else
	{
		INC_DWORD_STAT(STAT_VoxelPaint_SnapshotDedupSkips);
	}

	if (bWindowChanged || bCapacityShipPending || GameIdentity != LastShippedIdentity)
	{
		bShip = true;
	}

	if (bShip)
	{
		Shipment.Identity = GameIdentity;
		Shipment.Window = ExpectedWindow;
		Shipment.Capacity = GridCapacity;
		Shipment.CellSizeXY = CellSizeXY;
		Shipment.CellSizeZ = CellSizeZ;
		Shipment.EvaluationRadius = EvaluationRadius;
		Shipment.bCapacityChanged = bCapacityShipPending;
#if WITH_EDITOR
		Shipment.MasterWorldKey = FObjectKey(MasterWorld);
#endif
		bCapacityShipPending = false;
		LastShippedIdentity = GameIdentity;
		ENQUEUE_RENDER_COMMAND(VoxelPaintCoordinatorState)(
			[this, MovedShipment = MoveTemp(Shipment)](FRHICommandListImmediate&) mutable
			{
				ApplyShipment_RenderThread(MoveTemp(MovedShipment));
			});
	}

	const FVector3f CameraCenter = FVector3f(CameraLWC - ExpectedWindow.EvaluationOrigin);
	ENQUEUE_RENDER_COMMAND(VoxelPaintCameraCenter)(
		[this, CameraCenter](FRHICommandListImmediate&)
		{
			RT.CameraCenter = CameraCenter;
		});
}

void FVoxelPaintCoordinator::BuildDesiredAssets(FStateShipment& Shipment)
{
	CaptureBook.BeginPack();
	TSet<FVoxelPaintAssetKey> Seen;
	for (const FVoxelPaintPlacementState& Placement : Shipment.Placements)
	{
		if ((Placement.Flags & WorldVoxelPaint::RecordFlag_Valid) == 0)
		{
			continue;
		}
		bool bAlreadySeen = false;
		Seen.Add(Placement.AssetKey, &bAlreadySeen);
		if (bAlreadySeen)
		{
			continue;
		}
		UVoxelPaintVolumeData* VolumeData = Cast<UVoxelPaintVolumeData>(Placement.AssetKey.ResolveObjectPtr());
		if (!VolumeData)
		{
			continue;
		}
		FVoxelPaintDesiredAsset& Desired = Shipment.DesiredAssets.AddDefaulted_GetRef();
		Desired.Key = Placement.AssetKey;
		Desired.SourceGeneration = VolumeData->GetSourceGeneration();
		Desired.Identity = VolumeData->GetCanonicalIdentity();
		Desired.Manifest = VolumeData->GetManifestSnapshot();
		Desired.DebugName = VolumeData->GetPathName();

		if (CaptureBook.IsCaptured(Placement.AssetKey, Desired.SourceGeneration))
		{
			CaptureBook.PackCaptured(Placement.AssetKey, Desired.SourceGeneration);
			continue;
		}
		bool bCapturePending = false;
		if (TSharedPtr<FVoxelPaintPreparedSourceSnapshot> Snapshot =
			VolumeData->CreatePreparationSnapshot(&bCapturePending))
		{
			CaptureBook.PackCaptured(Placement.AssetKey, Desired.SourceGeneration);
			Shipment.PrepSnapshots.Add(MoveTemp(Snapshot));
		}
		else if (bCapturePending)
		{
			CaptureBook.PackPending(Placement.AssetKey);
		}
	}
	CaptureBook.EndPack();
}

void FVoxelPaintCoordinator::ApplyShipment_RenderThread(FStateShipment&& Shipment)
{
	RT.Window = Shipment.Window;
	RT.Capacity = Shipment.Capacity;
	RT.CellSizeXY = Shipment.CellSizeXY;
	RT.CellSizeZ = Shipment.CellSizeZ;
	RT.EvaluationRadius = Shipment.EvaluationRadius;
#if WITH_EDITOR
	RT.MasterWorldKey = Shipment.MasterWorldKey;
#endif
	if (Shipment.bCapacityChanged)
	{
		RT.bGridReinitializePending = true;
	}

	RT.Identity.OwnerEpoch = Shipment.Identity.OwnerEpoch;
	RT.Identity.LayoutGeneration = Shipment.Identity.LayoutGeneration;
	RT.Identity.BindingGeneration = Shipment.Identity.BindingGeneration + RT.ResidencyBindingEvents;

	if (Shipment.bHasSnapshot)
	{
		RT.Placements = MoveTemp(Shipment.Placements);
		RT.DesiredAssets = MoveTemp(Shipment.DesiredAssets);
		RT.bDesiredAssetsDirty = true;
		for (TSharedPtr<FVoxelPaintPreparedSourceSnapshot>& Snapshot : Shipment.PrepSnapshots)
		{
			const FVoxelPaintAssetKey Key = Snapshot->AssetKey;
			RT.PrepSnapshots.Add(Key, MoveTemp(Snapshot));
		}
		TSet<FVoxelPaintAssetKey> DesiredKeys;
		for (const FVoxelPaintDesiredAsset& Desired : RT.DesiredAssets)
		{
			DesiredKeys.Add(Desired.Key);
		}
		for (auto It = RT.PrepSnapshots.CreateIterator(); It; ++It)
		{
			if (!DesiredKeys.Contains(It->Key))
			{
				It.RemoveCurrent();
			}
		}
		if (RT.PoolRenderer)
		{
			RT.PoolRenderer->PruneRetainedSnapshots(DesiredKeys);
		}
	}
}

void FVoxelPaintCoordinator::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	FVoxelPaintSceneParameters Parameters;
	WorldVoxelPaint::BuildNeutralSceneParameters(Parameters, GraphBuilder);

	const FVoxelPaintOwnershipTicket* Ticket = InViewFamily.GetExtentionData<FVoxelPaintOwnershipTicket>();
	if (WorldVoxelPaint::ShouldRenderAsMaster(Ticket, RT.CurrentEpoch)
		&& CVarVoxelPaintEnable.GetValueOnRenderThread() != 0)
	{
		ServiceAndPublish_RenderThread(GraphBuilder, Parameters);
	}

	InViewFamily.GetSceneRenderer()->GetSceneUniforms().Set(SceneUB::VoxelPaint, Parameters);
}

void FVoxelPaintCoordinator::ServiceAndPublish_RenderThread(FRDGBuilder& GraphBuilder, FVoxelPaintSceneParameters& Parameters)
{
	RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelPaintPublish, "WorldVoxelPaint.ServiceAndPublish");

	if (!RT.Window.bValid || !RT.Capacity.bValid)
	{
		return;
	}

	if (!RT.PoolRenderer)
	{
		RT.PoolRenderer = MakeUnique<FVoxelPaintPoolRenderer>();
	}
	if (!RT.GridRenderer)
	{
		RT.GridRenderer = MakeUnique<FVoxelPaintGridRenderer>();
	}
	if (!RT.PoolRenderer->IsInitialized())
	{
		RT.PoolRenderer->Initialize(GraphBuilder, PoolCapacityBytes, PageTableCapacityBytes);
		RT.Residency->Initialize(RT.PoolRenderer->GetPoolSlotCapacity(), RT.PoolRenderer->GetPageEntryCapacity());
		RT.bResidencyInitialized = true;
		RT.GridRenderer->Initialize(GraphBuilder, RT.Capacity);
		RT.bGridReinitializePending = false;
	}
	else if (RT.bGridReinitializePending)
	{
		RT.bGridReinitializePending = false;
		RT.GridRenderer->Reinitialize(GraphBuilder, RT.Capacity);
	}

	++RT.PublicationOrdinal;

	if (RT.bDesiredAssetsDirty)
	{
		RT.bDesiredAssetsDirty = false;
		RT.Residency->SetDesiredAssets(RT.CurrentEpoch, RT.DesiredAssets);
	}

	for (auto It = RT.PrepSnapshots.CreateIterator(); It; ++It)
	{
		uint64 NeededGeneration = 0;
		if (RT.Residency->NeedsPreparation(It->Key, NeededGeneration)
			&& It->Value.IsValid() && It->Value->SourceGeneration == NeededGeneration)
		{
			RT.PoolRenderer->LaunchPreparation(It->Value, RT.CurrentEpoch, RT.Residency);
			It.RemoveCurrent();
			continue;
		}
		const EVoxelPaintAssetResidencyState AssetState = RT.Residency->GetAssetState(It->Key);
		if (AssetState == EVoxelPaintAssetResidencyState::Published
			|| AssetState == EVoxelPaintAssetResidencyState::ConfigError
			|| !It->Value.IsValid()
			|| !It->Value->Manifest.IsValid()
			|| It->Value->Manifest->PopulatedBricks.IsEmpty())
		{
			It.RemoveCurrent();
		}
	}

	FVoxelPaintIntegrationBudget Budget;
	Budget.MaxUploadBytes = WorldVoxelPaint::IntegrationBudgetUploadBytes;
	Budget.MaxPageEntries = WorldVoxelPaint::IntegrationBudgetPageEntries;
	FVoxelPaintIntegrationOps Ops;
	bool bBindingsChanged = false;
	RT.Residency->IntegrateStep(RT.CurrentEpoch, RT.PublicationOrdinal, Budget, Ops, bBindingsChanged);
	RT.PoolRenderer->ExecuteOps(GraphBuilder, Ops);
	if (bBindingsChanged)
	{
		++RT.ResidencyBindingEvents;
		RT.Identity.OnResidencyBindingEvent();
	}

#if WITH_EDITOR
	const WorldVoxelPaint::FVoxelPaintGridReadinessToken GridToken{
		RT.Identity.OwnerEpoch, RT.Identity.LayoutGeneration};
	RT.GridReadiness.OnExpectedGridChanged(GridToken);
	for (int32 SlotIndex = 0; SlotIndex < RT.GridReadbackSlots.Num(); ++SlotIndex)
	{
		FRenderThreadState::FGridReadbackSlot& Slot = RT.GridReadbackSlots[SlotIndex];
		if (!Slot.bInFlight || !Slot.Readback.IsValid() || !Slot.Readback->IsReady())
		{
			continue;
		}
		WorldVoxelPaint::FVoxelPaintGridReadbackResult Result;
		Result.Token = Slot.Token;
		if (Slot.Readback->GetGPUSizeBytes() < 4u)
		{
			Result.bMalformed = true;
		}
		else if (const void* Flag = Slot.Readback->Lock(4u))
		{
			Result.bExplicitSuccess = *static_cast<const uint32*>(Flag) == 0u;
			Slot.Readback->Unlock();
		}
		else
		{
			Result.bMalformed = true;
		}
		Slot.bInFlight = false;
		RT.GridReadiness.OnReadbackResolved(SlotIndex, Result);
	}
#endif

	const bool bIdentityChanged = RT.Identity != RT.LastPublishedIdentity;
	const bool bLayoutAdvanced = RT.Identity.OwnerEpoch != RT.LastPublishedIdentity.OwnerEpoch
		|| RT.Identity.LayoutGeneration != RT.LastPublishedIdentity.LayoutGeneration;

	FRDGBufferRef VolumesBuffer = nullptr;
	if (bIdentityChanged || !RT.VolumesBuffer.IsValid())
	{
		WorldVoxelPaint::FVoxelPaintAssemblyStats AssemblyStats;
#if WITH_EDITOR
		const FVoxelPaintSessionRenderBinding* SessionBinding =
			RT.SessionBinding.IsActive() ? &RT.SessionBinding : nullptr;
		const bool bMasterIsOrigin =
			SessionBinding != nullptr && SessionBinding->OriginWorldKey == RT.MasterWorldKey;
		bool bAnySelectorPublished = false;
		WorldVoxelPaint::AssembleRecords(
			RT.Placements, *RT.Residency, RT.Window.EvaluationOrigin, RT.Records, AssemblyStats,
			RT.DesiredAssets, SessionBinding, bMasterIsOrigin, bAnySelectorPublished,
			&RT.SelectorPublishedIdentities);
		RT.bSessionSelectorPublished = bAnySelectorPublished;
#else
		WorldVoxelPaint::AssembleRecords(
			RT.Placements, *RT.Residency, RT.Window.EvaluationOrigin, RT.Records, AssemblyStats,
			RT.DesiredAssets);
#endif
		RT.NumRecords = RT.Records.Num();

		const int32 NumLanes = FMath::Max(RT.NumRecords, 1) * WorldVoxelPaint::RecordNumFloat4Lanes;
		FRDGUploadData<FVector4f> Lanes(GraphBuilder, NumLanes);
		if (RT.NumRecords > 0)
		{
			FMemory::Memcpy(Lanes.GetData(), RT.Records.GetData(), NumLanes * sizeof(FVector4f));
		}
		else
		{
			FMemory::Memzero(Lanes.GetData(), NumLanes * sizeof(FVector4f));
		}
		VolumesBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("WorldVoxelPaint.Volumes"), Lanes);
		RT.VolumesBuffer = GraphBuilder.ConvertToExternalBuffer(VolumesBuffer);
	}
	else
	{
		VolumesBuffer = GraphBuilder.RegisterExternalBuffer(RT.VolumesBuffer);
	}
	FRDGBufferSRVRef VolumesSRV = GraphBuilder.CreateSRV(VolumesBuffer);

	if (bLayoutAdvanced)
	{
		WorldVoxelPaint::FVoxelPaintGridBuildInput GridInput;
		WorldVoxelPaint::BuildGridInput(
			RT.Records, RT.Window, RT.Capacity, RT.CellSizeXY, RT.CellSizeZ, GridInput);
		if (GridInput.bValid)
		{
#if WITH_EDITOR
			FRDGBufferRef GridErrorFlagBuffer = nullptr;
			RT.GridRenderer->Rebuild(GraphBuilder, GridInput, VolumesSRV, &GridErrorFlagBuffer);
			if (GridErrorFlagBuffer)
			{
				const int32 SlotIndex = RT.GridReadiness.AcquireSlot(GridToken);
				if (SlotIndex != INDEX_NONE)
				{
					FRenderThreadState::FGridReadbackSlot& Slot = RT.GridReadbackSlots[SlotIndex];
					if (!Slot.Readback.IsValid())
					{
						Slot.Readback = MakeUnique<FRHIGPUBufferReadback>(
							TEXT("WorldVoxelPaint.GridReadinessReadback"));
					}
					AddEnqueueCopyPass(GraphBuilder, Slot.Readback.Get(), GridErrorFlagBuffer, 4u);
					Slot.Token = GridToken;
					Slot.bInFlight = true;
				}
			}
#else
			RT.GridRenderer->Rebuild(GraphBuilder, GridInput, VolumesSRV);
#endif
		}
		else
		{
			RT.GridRenderer->FailNeutral(GraphBuilder,
				GridInput.KCap != 0u && GridInput.TotalThreads > GridInput.KCap);
		}
	}

	Parameters.Volumes = VolumesSRV;
	Parameters.NumVolumes = static_cast<uint32>(RT.NumRecords);
	RT.GridRenderer->Register(GraphBuilder, Parameters);
	RT.PoolRenderer->Register(GraphBuilder, Parameters);
#if WITH_EDITOR
	if (RT.bSessionSelectorPublished && RT.SessionBinding.PublishedTexture.IsValid())
	{
		Parameters.SessionTexture = RT.SessionBinding.PublishedTexture;
	}
	if (RT.bSessionSelectorPublished)
	{
		RT.LastSelectorPublicationOrdinal = RT.PublicationOrdinal;
	}
#endif
	const FDFVector3 EvaluationOrigin(RT.Window.EvaluationOrigin);
	Parameters.EvaluationOriginHigh = EvaluationOrigin.High;
	Parameters.EvaluationOriginLow = EvaluationOrigin.Low;
	Parameters.EvaluationRadiusSq = RT.EvaluationRadius * RT.EvaluationRadius;
	Parameters.CameraCenter = RT.CameraCenter;
	Parameters.EnableFlag = 1;

	RT.LastPublishedIdentity = RT.Identity;
	if (!RT.bActivatedThisEpoch)
	{
		RT.bActivatedThisEpoch = true;
		SET_DWORD_STAT(STAT_VoxelPaint_MasterHandoffState, 2);
	}

#if WITH_EDITOR
	if (RT.GridReadiness.WantsRecoveryRebuild())
	{
		RT.GridReadiness.AcknowledgeRecoveryRequested();
		++RT.GridRecoveryRequestCount;
	}
	FVoxelPaintSessionRenderStatus Status;
	Status.SessionGeneration = RT.SessionBinding.SessionGeneration;
	Status.bSelectorPublishedLastFrame = RT.bSessionSelectorPublished;
	Status.SelectorPublishedIdentities = RT.SelectorPublishedIdentities;
	if (RT.SessionBinding.Phase == EVoxelPaintSessionBindingPhase::PostCommit)
	{
		uint32 UnusedBaseIndex = 0;
		const EVoxelPaintBindingKind Kind = RT.Residency->ResolveBinding(
			RT.SessionBinding.AssetKey, 0, RT.SessionBinding.CommittedIdentity, UnusedBaseIndex);
		Status.bMatchingDataPublished = Kind == EVoxelPaintBindingKind::Data
			|| Kind == EVoxelPaintBindingKind::NoTable;
	}
	Status.LastSelectorPublicationOrdinal = RT.LastSelectorPublicationOrdinal;
	Status.CurrentPublicationOrdinal = RT.PublicationOrdinal;
	Status.GridReadiness = static_cast<uint8>(RT.GridReadiness.GetReadiness());
	Status.GridRecoveryRequestCount = RT.GridRecoveryRequestCount;
	{
		FScopeLock StatusWriteLock(&SessionStatusLock);
		SessionStatusMirror = Status;
	}

	const double NowSeconds = FPlatformTime::Seconds();
	if (NowSeconds - RT.LastDiagnosticsBuildSeconds >= WorldVoxelPaint::ResidencyDiagnosticsRefreshSeconds)
	{
		RT.LastDiagnosticsBuildSeconds = NowSeconds;
		FVoxelPaintResidencyDiagnostics Diagnostics;
		RT.Residency->BuildDiagnostics(Diagnostics);
		FScopeLock DiagnosticsWriteLock(&SessionStatusLock);
		ResidencyDiagnosticsMirror = MoveTemp(Diagnostics);
	}
#endif
}

bool FVoxelPaintCoordinator::GetExpectedWindowBounds(FBox& OutWorldBounds) const
{
	check(IsInGameThread());
	if (!bHasMaster || !ExpectedWindow.bValid)
	{
		return false;
	}
	const double HalfX = 0.5 * ExpectedWindow.LiveCells.X * CellSizeXY;
	const double HalfY = 0.5 * ExpectedWindow.LiveCells.Y * CellSizeXY;
	OutWorldBounds = FBox(
		FVector(ExpectedWindow.SnappedCenterXY.X - HalfX, ExpectedWindow.SnappedCenterXY.Y - HalfY,
			ExpectedWindow.ZEnvelope.ZMin),
		FVector(ExpectedWindow.SnappedCenterXY.X + HalfX, ExpectedWindow.SnappedCenterXY.Y + HalfY,
			ExpectedWindow.ZEnvelope.ZMax));
	return true;
}

bool FVoxelPaintCoordinator::RegisterForExternalSampling(FRDGBuilder& GraphBuilder,
	FVoxelPaintExternalSamplingParams& Out)
{
	check(IsInRenderingThread());

	// Nothing-published / disabled: the same predicate family that keeps
	// ServiceAndPublish_RenderThread neutral, plus the enable cvar (the pooled resources
	// outlive a cvar flip; skipping the check would happily register stale data). Note
	// RT.NumRecords == 0 is deliberately NOT a false case: the volumes buffer then validly
	// holds one zeroed record and NumVolumes publishes as 0 (published-empty passthrough).
	if (CVarVoxelPaintEnable.GetValueOnRenderThread() == 0
		|| !RT.Window.bValid || !RT.Capacity.bValid
		|| !RT.PoolRenderer || !RT.PoolRenderer->IsInitialized()
		|| !RT.VolumesBuffer.IsValid())
	{
		return false;
	}

	Out = FVoxelPaintExternalSamplingParams();
	Out.Volumes = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(RT.VolumesBuffer));
	Out.NumVolumes = static_cast<uint32>(RT.NumRecords);
	Out.Records = MakeArrayView(RT.Records);
	RT.PoolRenderer->Register(GraphBuilder, Out);

	Out.SessionTexture = GBlackVolumeTexture->TextureRHI;
#if WITH_EDITOR
	if (RT.bSessionSelectorPublished && RT.SessionBinding.PublishedTexture.IsValid())
	{
		Out.SessionTexture = RT.SessionBinding.PublishedTexture;
	}
	Out.MasterWorldKey = RT.MasterWorldKey;
#endif

	// Origin split from the SAME RT.Window the records were assembled against - any other
	// origin silently shifts every volume (records store origin-relative centers).
	const FDFVector3 EvaluationOrigin(RT.Window.EvaluationOrigin);
	Out.EvaluationOriginHigh = EvaluationOrigin.High;
	Out.EvaluationOriginLow = EvaluationOrigin.Low;
	Out.CameraCenter = RT.CameraCenter;
	Out.EvaluationRadiusSq = RT.EvaluationRadius * RT.EvaluationRadius;
	return true;
}

void FVoxelPaintCoordinator::NotifyWorldTeardown(const UWorld* World)
{
	if (!World)
	{
		return;
	}

	if (bHasMaster && CurrentMasterWorld.GetEvenIfUnreachable() == World)
	{
		AdvanceEpoch();
		CurrentMasterWorld.Reset();
		bHasMaster = false;
		CurrentPrimaryViewport = nullptr;
		ExpectedWindow = WorldVoxelPaint::FVoxelPaintExpectedWindow();
		bZEnvelopeDirty = true;
		GameIdentity.OnEpochAdvance(OwnerEpoch);
		LastShippedIdentity = FVoxelPaintPublicationIdentity();
#if WITH_EDITOR
		LastMasterPrimaryWorld.Reset();
#endif
		SET_DWORD_STAT(STAT_VoxelPaint_OwnerEpoch, static_cast<int64>(OwnerEpoch));
		SET_DWORD_STAT(STAT_VoxelPaint_MasterHandoffState, 0);
	}

	if (LastQueuedWorld.GetEvenIfUnreachable() != World)
	{
		return;
	}

	LastQueuedWorld.Reset();
	LastQueuedLayoutGeneration = 0;
	LastQueuedBindingGeneration = 0;
	CaptureBook.Empty();

	ENQUEUE_RENDER_COMMAND(VoxelPaintWorldTeardown)(
		[this](FRHICommandListImmediate&)
		{
			RT.Placements.Empty();
			RT.DesiredAssets.Empty();
			RT.bDesiredAssetsDirty = false;
			RT.PrepSnapshots.Empty();
			RT.Records.Empty();
			RT.NumRecords = 0;
			RT.VolumesBuffer = nullptr;
			if (RT.PoolRenderer)
			{
				RT.PoolRenderer->ClearRetainedSnapshots();
			}
#if WITH_EDITOR
			for (FRenderThreadState::FGridReadbackSlot& Slot : RT.GridReadbackSlots)
			{
				Slot.Readback.Reset();
				Slot.bInFlight = false;
			}
			RT.GridReadiness.Initialize(WorldVoxelPaint::GridReadinessReadbackSlots);
#endif
		});
}

#if WITH_EDITOR

const UWorld* FVoxelPaintCoordinator::GetCurrentMasterWorld() const
{
	check(IsInGameThread());
	return bHasMaster ? CurrentMasterWorld.Get() : nullptr;
}

bool FVoxelPaintCoordinator::IsMasterWorldWithEligiblePrimary(const UWorld* World) const
{
	check(IsInGameThread());
	return World != nullptr
		&& bHasMaster
		&& CurrentMasterWorld.Get() == World
		&& LastMasterPrimaryWorld.Get() == World;
}

void FVoxelPaintCoordinator::SetSessionRenderBinding(const FVoxelPaintSessionRenderBinding& Binding)
{
	check(IsInGameThread());
	ENQUEUE_RENDER_COMMAND(VoxelPaintSessionBindingSet)(
		[this, Binding](FRHICommandListImmediate&)
		{
			RT.SessionBinding = Binding;
			++RT.ResidencyBindingEvents;
			RT.Identity.OnSessionBindingEvent();
		});
}

void FVoxelPaintCoordinator::ClearSessionRenderBinding(uint64 SessionGeneration)
{
	check(IsInGameThread());
	ENQUEUE_RENDER_COMMAND(VoxelPaintSessionBindingClear)(
		[this, SessionGeneration](FRHICommandListImmediate&)
		{
			if (RT.SessionBinding.IsActive() && RT.SessionBinding.SessionGeneration == SessionGeneration)
			{
				RT.SessionBinding = FVoxelPaintSessionRenderBinding();
				++RT.ResidencyBindingEvents;
				RT.Identity.OnSessionBindingEvent();
			}
		});
}

FVoxelPaintSessionRenderStatus FVoxelPaintCoordinator::GetSessionRenderStatus() const
{
	check(IsInGameThread());
	FScopeLock StatusReadLock(&SessionStatusLock);
	return SessionStatusMirror;
}

void FVoxelPaintCoordinator::RequestAssetRepreparation(const FVoxelPaintAssetKey& Key)
{
	check(IsInGameThread());
	CaptureBook.Remove(Key);
	// Forces a full re-pack next serviced frame so a fresh preparation snapshot ships for the
	// unchanged source generation.
	LastQueuedWorld.Reset();
	ENQUEUE_RENDER_COMMAND(VoxelPaintAssetRepreparation)(
		[this, Key](FRHICommandListImmediate&)
		{
			if (RT.bResidencyInitialized)
			{
				RT.Residency->ResetForRetry(Key);
			}
			if (RT.PoolRenderer)
			{
				// Drops the retained snapshot and its per-(generation, epoch) launch dedupe so
				// the re-shipped snapshot relaunches at the same source generation.
				TSet<FVoxelPaintAssetKey> KeepKeys;
				for (const FVoxelPaintDesiredAsset& Desired : RT.DesiredAssets)
				{
					if (Desired.Key != Key)
					{
						KeepKeys.Add(Desired.Key);
					}
				}
				RT.PoolRenderer->PruneRetainedSnapshots(KeepKeys);
			}
			RT.PrepSnapshots.Remove(Key);
		});
}

void FVoxelPaintCoordinator::RequestGridRecoveryRebuild()
{
	check(IsInGameThread());
	ExpectedWindow.bValid = false;
	GameIdentity.OnZEnvelopeOrSettingsChange();
	INC_DWORD_STAT(STAT_VoxelPaint_GridReadinessRecoveryRebuilds);
}

WorldVoxelPaint::EVoxelPaintGridReadiness FVoxelPaintCoordinator::GetGridReadiness() const
{
	check(IsInGameThread());
	FScopeLock StatusReadLock(&SessionStatusLock);
	return static_cast<WorldVoxelPaint::EVoxelPaintGridReadiness>(SessionStatusMirror.GridReadiness);
}

FVoxelPaintResidencyDiagnostics FVoxelPaintCoordinator::GetResidencyDiagnostics() const
{
	check(IsInGameThread());
	FScopeLock StatusReadLock(&SessionStatusLock);
	return ResidencyDiagnosticsMirror;
}

void FVoxelPaintCoordinator::SetPickPassDelegate(FVoxelPaintPickPassFunction Fn)
{
	check(IsInGameThread());
	ENQUEUE_RENDER_COMMAND(VoxelPaintPickPassSet)(
		[this, MovedFn = MoveTemp(Fn)](FRHICommandListImmediate&) mutable
		{
			RT.PickPassDelegate = MoveTemp(MovedFn);
		});
}

void FVoxelPaintCoordinator::ClearPickPassDelegate(TFunction<void()> OnCleared)
{
	check(IsInGameThread());
	ENQUEUE_RENDER_COMMAND(VoxelPaintPickPassClear)(
		[this, MovedOnCleared = MoveTemp(OnCleared)](FRHICommandListImmediate&) mutable
		{
			RT.PickPassDelegate = nullptr;
			if (MovedOnCleared)
			{
				MovedOnCleared();
			}
		});
}

void FVoxelPaintCoordinator::PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder,
	FSceneView& InView, const FRenderTargetBindingSlots& RenderTargets,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	if (!RT.PickPassDelegate)
	{
		return;
	}
	const FSceneViewFamily* Family = InView.Family;
	if (!Family || Family->Views.Num() == 0 || Family->Views[0] != &InView)
	{
		return;
	}
	if (InView.bIsSceneCapture || InView.bIsReflectionCapture || InView.bIsPlanarReflection)
	{
		return;
	}
	const FVoxelPaintOwnershipTicket* Ticket = Family->GetExtentionData<FVoxelPaintOwnershipTicket>();
	if (!WorldVoxelPaint::ShouldRenderAsMaster(Ticket, RT.CurrentEpoch))
	{
		return;
	}
	RT.PickPassDelegate(GraphBuilder, InView, SceneTextures);
}

#endif // WITH_EDITOR

#if !UE_BUILD_SHIPPING

void FVoxelPaintCoordinator::PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder,
	const FSceneView& InView, const FPostProcessingInputs& Inputs)
{
	const int32 HeatmapMode = CVarVoxelPaintHeatmap.GetValueOnRenderThread();
	if (HeatmapMode == 0 || CVarVoxelPaintEnable.GetValueOnRenderThread() == 0)
	{
		return;
	}
	const FSceneViewFamily* Family = InView.Family;
	if (!Family || Family->Views.Num() == 0 || Family->Views[0] != &InView)
	{
		return;
	}
	static const int32 ShowFlagIndex = FEngineShowFlags::FindIndexByName(TEXT("WorldVoxelPaint"));
	if (ShowFlagIndex == INDEX_NONE
		|| !Family->EngineShowFlags.GetSingleFlag(static_cast<uint32>(ShowFlagIndex)))
	{
		return;
	}
	const FVoxelPaintOwnershipTicket* Ticket = Family->GetExtentionData<FVoxelPaintOwnershipTicket>();
	if (!WorldVoxelPaint::ShouldRenderAsMaster(Ticket, RT.CurrentEpoch))
	{
		return;
	}
	if (!RT.Window.bValid || !RT.GridRenderer || !RT.GridRenderer->IsInitialized()
		|| InView.GetFeatureLevel() < ERHIFeatureLevel::SM5)
	{
		return;
	}

	FVoxelPaintSceneParameters GridParameters;
	RT.GridRenderer->Register(GraphBuilder, GridParameters);
	if (GridParameters.GridHeaders == nullptr
		|| GridParameters.GridDims.X <= 0 || GridParameters.GridDims.Y <= 0 || GridParameters.GridDims.Z <= 0)
	{
		return;
	}

	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformBuffer =
		CreateSceneTextureUniformBuffer(GraphBuilder, InView,
			ESceneTextureSetupMode::SceneColor | ESceneTextureSetupMode::SceneDepth);
	if (!SceneTexturesUniformBuffer)
	{
		return;
	}
	const FSceneTextureUniformParameters* SceneTextures = SceneTexturesUniformBuffer->GetContents();
	FRDGTextureRef SceneColorTexture = SceneTextures->SceneColorTexture;
	FRDGTextureRef SceneDepthTexture = SceneTextures->SceneDepthTexture;
	if (!SceneColorTexture || !SceneDepthTexture)
	{
		return;
	}

	const FVector TranslatedWorldToRelative =
		-(InView.ViewMatrices.GetPreViewTranslation() + RT.Window.EvaluationOrigin);

	RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelPaintHeatmap, "WorldVoxelPaint.Heatmap");

	FGlobalShaderMap* GlobalShaderMap = GetGlobalShaderMap(InView.GetFeatureLevel());
	TShaderMapRef<FVoxelPaintHeatmapPS> PixelShader(GlobalShaderMap);

	FVoxelPaintHeatmapPS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FVoxelPaintHeatmapPS::FParameters>();
	PassParameters->View = InView.ViewUniformBuffer;
	PassParameters->SceneDepthTexture = SceneDepthTexture;
	PassParameters->GridHeaders = GridParameters.GridHeaders;
	PassParameters->GridDims = GridParameters.GridDims;
	PassParameters->GridOrigin = GridParameters.GridOrigin;
	PassParameters->GridCellSize = GridParameters.GridCellSize;
	PassParameters->TranslatedWorldToRelative = FVector3f(TranslatedWorldToRelative);
	PassParameters->HeatmapMode = static_cast<uint32>(HeatmapMode);
	PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture, ERenderTargetLoadAction::ELoad);

	const FIntRect ViewRect = UE::FXRenderingUtils::GetRawViewRectUnsafe(InView);
	FPixelShaderUtils::AddFullscreenPass(GraphBuilder, GlobalShaderMap,
		RDG_EVENT_NAME("VoxelPaintHeatmap"), PixelShader, PassParameters, ViewRect,
		TStaticBlendState<CW_RGB, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha>::GetRHI());
}

#endif // !UE_BUILD_SHIPPING
