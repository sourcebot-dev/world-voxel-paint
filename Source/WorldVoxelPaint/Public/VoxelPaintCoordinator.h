// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "SceneViewExtension.h"
#include "Templates/RefCounting.h"
#include "Templates/UniquePtr.h"
#include "UObject/WeakObjectPtr.h"
#include "VoxelPaintCaptureBook.h"
#include "VoxelPaintDiagnostics.h"
#include "VoxelPaintGridReadiness.h"
#include "VoxelPaintGridSetup.h"
#include "VoxelPaintPublicationIdentity.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintResidency.h"
#include "VoxelPaintResidencyTypes.h"
#include "VoxelPaintSessionRenderTypes.h"

class FRDGPooledBuffer;
class FRHIGPUBufferReadback;
class FViewport;
class FVoxelPaintGridRenderer;
class FVoxelPaintPoolRenderer;
class FVoxelPaintSceneParameters;
class UVoxelPaintWorldSubsystem;
class UWorld;
struct FVoxelPaintExternalSamplingParams;

class FVoxelPaintCoordinator final : public FSceneViewExtensionBase
{
public:
	WORLDVOXELPAINT_API static TSharedPtr<FVoxelPaintCoordinator, ESPMode::ThreadSafe> CreateAndRegister();
	WORLDVOXELPAINT_API static FVoxelPaintCoordinator* GetIfCreated();
	WORLDVOXELPAINT_API static void Deactivate();

	FVoxelPaintCoordinator(const FAutoRegister& AutoRegister);
	virtual ~FVoxelPaintCoordinator() override;

	// Game thread only.
	WORLDVOXELPAINT_API void NotifyWorldTeardown(const UWorld* World);
	WORLDVOXELPAINT_API bool GetExpectedWindowBounds(FBox& OutWorldBounds) const;

	// Render thread only.

	// RDG-setup timeline: call from render-thread setup code such as a compute data-provider
	// proxy's AllocateResources, never from inside an RDG pass lambda or a parallel task.
	// Registers the last-published pooled resources into the caller's builder
	// and fills Out for the grid-free sampler; see VoxelPaintExternalSampling.h for the full
	// member and lifetime contract. Returns false when evaluation is disabled
	// (r.VoxelPaint.Enable 0) or nothing is published (never serviced as master-primary, or
	// wiped by world teardown) - the caller must then bind neutral resources
	// (WorldVoxelPaint::BuildNeutralExternalSamplingParams) so downstream sampling yields exact
	// zero. A true return with Out.NumVolumes == 0 is the valid published-empty state;
	// shader-side consumers guard on NumVolumes.
	WORLDVOXELPAINT_API bool RegisterForExternalSampling(FRDGBuilder& GraphBuilder,
		FVoxelPaintExternalSamplingParams& Out);

#if WITH_EDITOR
	// Session render path + grid readiness (game thread).
	WORLDVOXELPAINT_API const UWorld* GetCurrentMasterWorld() const;
	WORLDVOXELPAINT_API bool IsMasterWorldWithEligiblePrimary(const UWorld* World) const;   // last-resolved-frame state
	WORLDVOXELPAINT_API void SetSessionRenderBinding(const FVoxelPaintSessionRenderBinding& Binding);
	WORLDVOXELPAINT_API void ClearSessionRenderBinding(uint64 SessionGeneration);   // stale generations ignored
	WORLDVOXELPAINT_API FVoxelPaintSessionRenderStatus GetSessionRenderStatus() const;   // lock-guarded POD copy
	WORLDVOXELPAINT_API void RequestAssetRepreparation(const FVoxelPaintAssetKey& Key);
	WORLDVOXELPAINT_API void RequestGridRecoveryRebuild();   // one layout advance (settings-change idiom)
	WORLDVOXELPAINT_API WorldVoxelPaint::EVoxelPaintGridReadiness GetGridReadiness() const;

	WORLDVOXELPAINT_API FVoxelPaintResidencyDiagnostics GetResidencyDiagnostics() const;   // lock-guarded POD copy

	// Set/Clear enqueue render commands; the functor runs ONLY on the render thread, at most
	// once per family, only when the family is the master world's eligible-primary family and
	// the view is the family's first view. After ClearPickPassDelegate's render command
	// executes (OnCleared runs on the render thread), the functor is never invoked again.
	using FVoxelPaintPickPassFunction = TFunction<void(FRDGBuilder&, const FSceneView&,
		TRDGUniformBufferRef<FSceneTextureUniformParameters>)>;
	WORLDVOXELPAINT_API void SetPickPassDelegate(FVoxelPaintPickPassFunction Fn);
	WORLDVOXELPAINT_API void ClearPickPassDelegate(TFunction<void()> OnCleared);
#endif

	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
#if WITH_EDITOR
	virtual void PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView,
		const FRenderTargetBindingSlots& RenderTargets,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;
#endif
#if !UE_BUILD_SHIPPING
	virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView,
		const FPostProcessingInputs& Inputs) override;
#endif

private:
	struct FResolvedOwnership
	{
		const UWorld* MasterWorld = nullptr;
		FViewport* PrimaryViewport = nullptr;
	};

	struct FStateShipment
	{
		FVoxelPaintPublicationIdentity Identity;
		WorldVoxelPaint::FVoxelPaintExpectedWindow Window;
		WorldVoxelPaint::FVoxelPaintGridCapacity Capacity;
		float CellSizeXY = 0.0f;
		float CellSizeZ = 0.0f;
		float EvaluationRadius = 0.0f;
		bool bCapacityChanged = false;
		bool bHasSnapshot = false;
#if WITH_EDITOR
		FObjectKey MasterWorldKey;
#endif
		TArray<FVoxelPaintPlacementState> Placements;
		TArray<FVoxelPaintDesiredAsset> DesiredAssets;
		TArray<TSharedPtr<FVoxelPaintPreparedSourceSnapshot>> PrepSnapshots;
	};

	static FResolvedOwnership ResolveOwnership();

	// Game thread.
	void AdvanceEpoch();
	void RefreshDerivedSettings();
	void OnSettingsChanged();
	void ServiceMasterPrimary(FSceneViewFamily& InViewFamily, UVoxelPaintWorldSubsystem* Subsystem);
	void BuildDesiredAssets(FStateShipment& Shipment);

	// Render thread.
	void ApplyShipment_RenderThread(FStateShipment&& Shipment);
	void ServiceAndPublish_RenderThread(FRDGBuilder& GraphBuilder, FVoxelPaintSceneParameters& Parameters);

	// Game-thread state.
	uint64 OwnerEpoch = 1;
	bool bHasMaster = false;
	bool bSettingsDirty = false;
	bool bZEnvelopeDirty = true;
	bool bCapacityShipPending = false;
	TWeakObjectPtr<const UWorld> CurrentMasterWorld;
	FViewport* CurrentPrimaryViewport = nullptr;
	FVoxelPaintPublicationIdentity GameIdentity;
	FVoxelPaintPublicationIdentity LastShippedIdentity;
	WorldVoxelPaint::FVoxelPaintExpectedWindow ExpectedWindow;
	WorldVoxelPaint::FVoxelPaintZEnvelope ZEnvelope;
	WorldVoxelPaint::FVoxelPaintGridCapacity GridCapacity;
	float CellSizeXY = 0.0f;
	float CellSizeZ = 0.0f;
	float EvaluationRadius = 0.0f;
	float GuardBandWidth = 0.0f;
	int64 PoolCapacityBytes = 0;
	int64 PageTableCapacityBytes = 0;
	TWeakObjectPtr<const UWorld> LastQueuedWorld;
	uint64 LastQueuedLayoutGeneration = 0;
	uint64 LastQueuedBindingGeneration = 0;
	WorldVoxelPaint::FVoxelPaintSnapshotCaptureBook CaptureBook;
	FDelegateHandle SettingsChangedHandle;
#if WITH_EDITOR
	TWeakObjectPtr<const UWorld> LastMasterPrimaryWorld;
	uint64 LastConsumedGridRecoveryRequests = 0;
	mutable FCriticalSection SessionStatusLock;
	FVoxelPaintSessionRenderStatus SessionStatusMirror;
	FVoxelPaintResidencyDiagnostics ResidencyDiagnosticsMirror;   // guarded by SessionStatusLock
#endif

	// Touched only by render commands and _RenderThread hooks.
	struct FRenderThreadState
	{
		uint64 CurrentEpoch = 1;
		uint64 PublicationOrdinal = 0;
		uint64 ResidencyBindingEvents = 0;
		bool bActivatedThisEpoch = false;
		bool bDesiredAssetsDirty = false;
		bool bGridReinitializePending = false;
		bool bResidencyInitialized = false;
		FVoxelPaintPublicationIdentity Identity;
		FVoxelPaintPublicationIdentity LastPublishedIdentity;
		WorldVoxelPaint::FVoxelPaintExpectedWindow Window;
		WorldVoxelPaint::FVoxelPaintGridCapacity Capacity;
		float CellSizeXY = 0.0f;
		float CellSizeZ = 0.0f;
		float EvaluationRadius = 0.0f;
		FVector3f CameraCenter = FVector3f::ZeroVector;
		TArray<FVoxelPaintPlacementState> Placements;
		TArray<FVoxelPaintDesiredAsset> DesiredAssets;
		TMap<FVoxelPaintAssetKey, TSharedPtr<FVoxelPaintPreparedSourceSnapshot>> PrepSnapshots;
		TArray<FVoxelPaintVolumeRecord> Records;
		int32 NumRecords = 0;
		TRefCountPtr<FRDGPooledBuffer> VolumesBuffer;
		// Shared so detached preparation tasks capture a weak reference: a completion render
		// command enqueued after coordinator teardown must drop its payload, never dereference
		// freed coordinator memory.
		TSharedRef<FVoxelPaintResidencyManager, ESPMode::ThreadSafe> Residency =
			MakeShared<FVoxelPaintResidencyManager, ESPMode::ThreadSafe>();
		TUniquePtr<FVoxelPaintPoolRenderer> PoolRenderer;
		TUniquePtr<FVoxelPaintGridRenderer> GridRenderer;
#if WITH_EDITOR
		struct FGridReadbackSlot
		{
			TUniquePtr<FRHIGPUBufferReadback> Readback;
			WorldVoxelPaint::FVoxelPaintGridReadinessToken Token;
			bool bInFlight = false;
		};

		FObjectKey MasterWorldKey;
		FVoxelPaintSessionRenderBinding SessionBinding;
		bool bSessionSelectorPublished = false;
		TArray<FVoxelPaintFrontierIdentity> SelectorPublishedIdentities;
		uint64 LastSelectorPublicationOrdinal = 0;
		uint64 GridRecoveryRequestCount = 0;
		WorldVoxelPaint::FVoxelPaintGridReadinessTracker GridReadiness;
		TArray<FGridReadbackSlot> GridReadbackSlots;
		FVoxelPaintPickPassFunction PickPassDelegate;
		double LastDiagnosticsBuildSeconds = 0.0;
#endif
	};

	FRenderThreadState RT;
};
