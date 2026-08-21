// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Misc/Optional.h"
#include "VoxelPaintAllocators.h"
#include "VoxelPaintResidencyTypes.h"

#if WITH_EDITOR
struct FVoxelPaintResidencyDiagnostics;
#endif

// Pure CPU state machine: no UObject access, no RHI. The single owner of the brick free list
// and the page-table allocator; all GPU work is expressed as emitted integration ops.
// Render-thread owned after coordinator init; tests may drive it single-threaded.
class WORLDVOXELPAINT_API FVoxelPaintResidencyManager
{
public:
	void Initialize(int32 PoolSlotCapacity, int32 PageTableEntryCapacity);

	void SetDesiredAssets(uint64 OwnerEpoch, TConstArrayView<FVoxelPaintDesiredAsset> Desired);
	void NotifyEpochAdvance(uint64 NewEpoch);

	bool NeedsPreparation(const FVoxelPaintAssetKey& Key, uint64& OutSourceGeneration) const;
	void OnPreparationLaunched(const FVoxelPaintAssetKey& Key, uint64 SourceGeneration, uint64 OwnerEpoch);
	void OnPreparationCompleted(FVoxelPaintPreparedPayload&& Payload);

	void IntegrateStep(uint64 OwnerEpoch, uint64 PublicationOrdinal,
		const FVoxelPaintIntegrationBudget& Budget,
		FVoxelPaintIntegrationOps& OutOps, bool& bOutBindingsChanged);

	EVoxelPaintBindingKind ResolveBinding(const FVoxelPaintAssetKey& Key, uint64 SourceGeneration,
		const FVoxelPaintCanonicalIdentity& Identity, uint32& OutPageTableBaseIndex) const;

	EVoxelPaintAssetResidencyState GetAssetState(const FVoxelPaintAssetKey& Key) const;

	// Render thread. Clears a terminal/stale entry so an unchanged source generation can
	// re-prepare: ConfigError -> NotResident (removed from queue); Published entries and warm
	// cache are untouched (the Retry path after a build/publication failure).
	void ResetForRetry(const FVoxelPaintAssetKey& Key);

	int32 GetPoolSlotCapacity() const;
	int32 GetAllocatedBrickCount() const;
	int32 GetPinnedBrickCount() const;
	int32 GetWarmCacheBrickCount() const;
	int32 GetRetryQueueDepth() const;
	int32 GetRetryBlockedBehindHeadCount() const;
	FString GetRetryQueueHeadDebugName() const;
	int32 GetOversizedConfigErrorCount() const;
	int32 GetConfigErrorAssetCount() const;
	int32 GetNeutralProxyAssetCount() const;
	int32 GetNoTableAssetCount() const;
	int32 GetPageEntryCapacity() const;
	int32 GetPageEntries(EVoxelPaintRangeState State) const;
	int32 GetPageFreeEntries() const;
	int32 GetPageWasteEntries() const;
	int32 GetPageHeadroomEntries() const;

#if WITH_EDITOR
	// Render thread. Read-only enumeration; mutates no residency state.
	void BuildDiagnostics(FVoxelPaintResidencyDiagnostics& Out) const;
#endif

private:
	struct FRepresentation
	{
		uint64 SourceGeneration = 0;
		FVoxelPaintCanonicalIdentity Identity;
		TSharedPtr<const FVoxelPaintSourceManifest> Manifest;
		TArray<int32> BrickSlots;
		int32 PageRangeHandle = INDEX_NONE;
		int64 TableEntryCount = 0;
		bool bPublished = false;
		uint64 DemotionOrdinal = 0;
		TSharedPtr<const TArray<uint8>> PendingUploadPayload;
		int32 NumBricksEmitted = 0;
		int64 NumPageEntriesEmitted = 0;
		int32 PageCursor = 0;
		bool bRangeUploading = false;
	};

	struct FLruStamp
	{
		uint64 Epoch = 0;
		uint64 Ordinal = 0;

		bool operator<(const FLruStamp& Other) const
		{
			return Epoch != Other.Epoch ? Epoch < Other.Epoch : Ordinal < Other.Ordinal;
		}
	};

	struct FEntry
	{
		FVoxelPaintAssetKey Key;
		FString DebugName;
		EVoxelPaintAssetResidencyState State = EVoxelPaintAssetResidencyState::NotResident;
		bool bDesired = false;
		bool bQueued = false;
		bool bOversized = false;
		uint64 ConfigErrorGeneration = 0;
		uint64 DesiredSourceGeneration = 0;
		FVoxelPaintCanonicalIdentity DesiredIdentity;
		TSharedPtr<const FVoxelPaintSourceManifest> DesiredManifest;
		bool bPrepInFlight = false;
		uint64 PrepGeneration = 0;
		uint64 PrepEpoch = 0;
		TOptional<FVoxelPaintPreparedPayload> PendingPayload;
		TArray<FRepresentation> Representations;
		int32 IntegratingRepIndex = INDEX_NONE;
		FLruStamp LruStamp;
	};

	struct FPendingSlotRelease
	{
		uint64 StampOrdinal = 0;
		TArray<int32> Slots;
	};

	void ApplyEpochAdvance(uint64 NewEpoch);
	void RecomputeState(FEntry& Entry) const;
	void ApplyDesiredSupersession(FEntry& Entry, const FVoxelPaintDesiredAsset& Desired);
	void ApplyUndesired(FEntry& Entry);
	bool IsEntryForgettable(const FEntry& Entry) const;
	bool ClassifyOversized(FEntry& Entry);
	void SetConfigError(FEntry& Entry, bool bOversized);
	void CancelIntegration(FEntry& Entry);
	void DemoteRepresentation(FEntry& Entry, FRepresentation& Representation);
	void PublishRepresentation(FEntry& Entry, int32 RepIndex);
	void PublishNoTable(FEntry& Entry);
	void EnqueueClaimant(FEntry& Entry);
	void RemoveFromQueue(FEntry& Entry);
	int32 FindMatchingRepresentation(const FEntry& Entry, const FVoxelPaintCanonicalIdentity& Identity) const;
	bool AttemptClaim(FEntry& Entry, uint64 Ordinal, bool& bOutBindingsChanged);
	bool EvictNextVictim(uint64 Ordinal);
	void ReleasePendingSlots(uint64 Ordinal);

	FVoxelPaintBlockAllocator BlockAllocator;
	FVoxelPaintPageTableAllocator PageAllocator;
	TMap<FVoxelPaintAssetKey, FEntry> Entries;
	TArray<FVoxelPaintAssetKey> RetryQueue;
	TArray<FVoxelPaintAssetKey> IntegratingList;
	TArray<FPendingSlotRelease> PendingSlotReleases;
	uint64 CurrentEpoch = 0;
	uint64 LastServicedOrdinal = 0;
	bool bPendingBindingsChanged = false;
	bool bInitialized = false;
};
