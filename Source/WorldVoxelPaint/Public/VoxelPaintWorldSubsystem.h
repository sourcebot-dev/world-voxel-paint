// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/SparseArray.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"
#include "UObject/WeakObjectPtr.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintWorldSubsystem.generated.h"

class AActor;
class UVoxelPaintVolumeComponent;
class UVoxelPaintVolumeData;
class UWorld;

UCLASS()
class WORLDVOXELPAINT_API UVoxelPaintWorldSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static UVoxelPaintWorldSubsystem* Get(const UWorld* World);

	virtual void PostInitialize() override;
	virtual void Deinitialize() override;

	void RegisterVolume(UVoxelPaintVolumeComponent* Component);
	void UnregisterVolume(UVoxelPaintVolumeComponent* Component);

	void NotifyTransformChanged(UVoxelPaintVolumeComponent* Component);
	void NotifyPropertyChanged(UVoxelPaintVolumeComponent* Component);

	// Game thread only; runs at the top of every mutating call and once per rendered frame
	// from the coordinator. Only the notify paths use the frame-gated default: their repeat
	// same-frame calls return after the (self-gated) forced-repack block. Register/Unregister
	// and the coordinator's per-frame call pass bForceSweep and always sweep, exactly as when
	// the sweep was ungated. Dead-component demotion can therefore be deferred only across
	// same-frame notify calls, and lands no later than the next structural call, the
	// coordinator's pre-snapshot sweep (master worlds), or the next frame's first mutating
	// call, whichever comes first.
	void ExpireStaleReservations(bool bForceSweep = false);

	uint64 GetLayoutGeneration() const;
	uint64 GetBindingGeneration() const;

	// Diagnostic lever (wvp.Grid.ProfileRebuild): invalidates the packed layout so the next
	// serviced master frame repacks every record and rebuilds the grid. NumFrames > 1 re-forces
	// the repack once per subsequent frame (a burst) so a GPU capture window whose frame is
	// chosen asynchronously by the profiler is guaranteed to contain a rebuild frame.
	void ForceLayoutRepack(int32 NumFrames = 1);

	int32 CountRegisteredVolumes() const;
	int32 GetNumFrontierMembers() const;
	int32 GetFrontierPosition(const UVoxelPaintVolumeComponent* Component) const;
	bool GetCachedPlacementState(const UVoxelPaintVolumeComponent* Component,
		FVoxelPaintPlacementState& OutState) const;

	// Occupied slots return the slot cache; waiters/unregistered fall back to
	// ResolveVariantLocal().
	UVoxelPaintVolumeData* GetResolvedVariant(const UVoxelPaintVolumeComponent* Component) const;

#if WITH_EDITOR
	void SetForcedVariant(const FVoxelPaintFrontierIdentity& Identity, const UVoxelPaintVolumeData* Asset);
	void ClearForcedVariant(const FVoxelPaintFrontierIdentity& Identity);
	void ClearAllForcedVariants();
	bool GetForcedVariant(const FVoxelPaintFrontierIdentity& Identity, FObjectKey& OutAsset) const;
#endif

	// Ascending frontier order, occupied slots only (valid and invalid-flag placements alike).
	void BuildDesiredSnapshot(TArray<FVoxelPaintPlacementState>& OutPlacements) const;
	void EnumerateFrontier(TFunctionRef<void(const FVoxelPaintPlacementState&)> Fn) const;

	// Single-pass panel/tooling enumeration: ascending slot order, Occupied slots with live
	// components only (the BuildDesiredSnapshot filter). SlotIndex is the value
	// GetFrontierPosition would return for Component.
	void EnumerateFrontierSlots(TFunctionRef<void(int32 SlotIndex,
		const UVoxelPaintVolumeComponent& Component, const FVoxelPaintPlacementState& State)> Fn) const;

	// Desired-state-only packing helper: packs each occupied cached state with a default
	// FVoxelPaintResolvedBinding (no-table sentinel). Not the production record writer.
	void BuildRecordSnapshot(const FVector& EvaluationOrigin,
		TArray<FVoxelPaintVolumeRecord>& OutRecords) const;

private:
	enum class EFrontierSlotState : uint8
	{
		Empty,
		Occupied,
		Reserved
	};

	struct FFrontierSlot
	{
		EFrontierSlotState State = EFrontierSlotState::Empty;
		FVoxelPaintFrontierIdentity Identity;
		TWeakObjectPtr<UVoxelPaintVolumeComponent> Component;
		uint64 ReservedFrame = 0;
		FVoxelPaintPlacementState CachedState;
		int32 ResolvedVariantIndex = INDEX_NONE;
		TWeakObjectPtr<UVoxelPaintVolumeData> ResolvedVariant;
	};

	struct FWaiter
	{
		FVoxelPaintFrontierIdentity Identity;
		TWeakObjectPtr<UVoxelPaintVolumeComponent> Component;
	};

	void OnActorPostRegisterAllComponents(AActor* Actor);
	void OnActorPreUnregisterAllComponents(AActor* Actor);
	void EnsureFrontier();
	bool PassesRegistrationFilters(const UVoxelPaintVolumeComponent* Component) const;
	int32 FindOccupiedFrontierSlot(const UVoxelPaintVolumeComponent* Component) const;
	int32 FindWaiterIndex(const UVoxelPaintVolumeComponent* Component) const;
	void OccupyFrontierSlot(int32 FrontierIndex, UVoxelPaintVolumeComponent* Component,
		const FVoxelPaintFrontierIdentity& Identity);
	static FVoxelPaintFrontierIdentity MakeFrontierIdentity(const UVoxelPaintVolumeComponent* Component);
	void RefreshCachedState(FFrontierSlot& Slot, FVoxelPaintPlacementState& OutState);
#if WITH_EDITOR
	void ReResolveForcedIdentity(const FVoxelPaintFrontierIdentity& Identity);
	void PruneStaleForcedVariants();
#endif

	void UpdateStats() const;

	TSparseArray<TWeakObjectPtr<UVoxelPaintVolumeComponent>> Registry;
	int32 LowestFreeIndexSearchStart = 0;
	int32 RegistrySweepCursor = 0;
	TArray<FFrontierSlot> FrontierSlots;
	TArray<FWaiter> WaiterQueue;
	uint64 LayoutGeneration = 1;
	int32 PendingForcedRepackFrames = 0;
	uint64 LastForcedRepackFrame = 0;
	uint64 LastReservationSweepFrame = 0;
	uint64 BindingGeneration = 1;
	FDelegateHandle PostRegisterHandle;
	FDelegateHandle PreUnregisterHandle;
#if WITH_EDITOR
	TMap<FVoxelPaintFrontierIdentity, FObjectKey> ForcedVariantOverrides;
#endif
};
