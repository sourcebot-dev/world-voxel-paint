// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintResidency.h"

#include "VoxelPaintDiagnostics.h"
#include "WorldVoxelPaintConstants.h"

namespace
{
	int64 ComputeTableEntryCount(const FVoxelPaintSourceManifest& Manifest)
	{
		return static_cast<int64>(Manifest.BrickCounts.X) * Manifest.BrickCounts.Y * Manifest.BrickCounts.Z;
	}
}

void FVoxelPaintResidencyManager::Initialize(int32 PoolSlotCapacity, int32 PageTableEntryCapacity)
{
	BlockAllocator.Initialize(PoolSlotCapacity);
	PageAllocator.Initialize(PageTableEntryCapacity);
	Entries.Empty();
	RetryQueue.Empty();
	IntegratingList.Empty();
	PendingSlotReleases.Empty();
	CurrentEpoch = 0;
	LastServicedOrdinal = 0;
	bPendingBindingsChanged = false;
	bInitialized = true;
}

void FVoxelPaintResidencyManager::SetDesiredAssets(uint64 OwnerEpoch, TConstArrayView<FVoxelPaintDesiredAsset> Desired)
{
	check(bInitialized);
	if (OwnerEpoch != CurrentEpoch)
	{
		ApplyEpochAdvance(OwnerEpoch);
	}

	for (TPair<FVoxelPaintAssetKey, FEntry>& Pair : Entries)
	{
		Pair.Value.bDesired = false;
	}

	for (const FVoxelPaintDesiredAsset& DesiredAsset : Desired)
	{
		if (DesiredAsset.Key == FVoxelPaintAssetKey())
		{
			continue;
		}
		FEntry& Entry = Entries.FindOrAdd(DesiredAsset.Key);
		Entry.Key = DesiredAsset.Key;
		if (!DesiredAsset.DebugName.IsEmpty())
		{
			Entry.DebugName = DesiredAsset.DebugName;
		}
		Entry.bDesired = true;
		if (Entry.DesiredSourceGeneration != DesiredAsset.SourceGeneration || Entry.DesiredIdentity != DesiredAsset.Identity)
		{
			ApplyDesiredSupersession(Entry, DesiredAsset);
		}
		else if (DesiredAsset.Manifest)
		{
			Entry.DesiredManifest = DesiredAsset.Manifest;
		}
		if (Entry.State == EVoxelPaintAssetResidencyState::ConfigError)
		{
			continue;
		}
		if (ClassifyOversized(Entry))
		{
			continue;
		}
		if (Entry.State != EVoxelPaintAssetResidencyState::Published &&
			Entry.IntegratingRepIndex == INDEX_NONE && !Entry.bQueued)
		{
			if (FindMatchingRepresentation(Entry, Entry.DesiredIdentity) != INDEX_NONE)
			{
				EnqueueClaimant(Entry);
			}
			else if (Entry.PendingPayload.IsSet() &&
				Entry.PendingPayload->SourceGeneration == Entry.DesiredSourceGeneration &&
				Entry.PendingPayload->LaunchOwnerEpoch == CurrentEpoch)
			{
				EnqueueClaimant(Entry);
			}
		}
	}

	for (TMap<FVoxelPaintAssetKey, FEntry>::TIterator It = Entries.CreateIterator(); It; ++It)
	{
		if (!It->Value.bDesired)
		{
			ApplyUndesired(It->Value);
			if (IsEntryForgettable(It->Value))
			{
				It.RemoveCurrent();
			}
		}
	}
}

void FVoxelPaintResidencyManager::NotifyEpochAdvance(uint64 NewEpoch)
{
	check(bInitialized);
	if (NewEpoch != CurrentEpoch)
	{
		ApplyEpochAdvance(NewEpoch);
	}
}

bool FVoxelPaintResidencyManager::NeedsPreparation(const FVoxelPaintAssetKey& Key, uint64& OutSourceGeneration) const
{
	OutSourceGeneration = 0;
	const FEntry* Entry = Entries.Find(Key);
	if (!Entry || !Entry->bDesired)
	{
		return false;
	}
	if (Entry->State == EVoxelPaintAssetResidencyState::ConfigError ||
		Entry->State == EVoxelPaintAssetResidencyState::Published ||
		Entry->State == EVoxelPaintAssetResidencyState::Integrating ||
		Entry->bQueued)
	{
		return false;
	}
	if (FindMatchingRepresentation(*Entry, Entry->DesiredIdentity) != INDEX_NONE)
	{
		return false;
	}
	if (Entry->DesiredManifest && Entry->DesiredManifest->PopulatedBricks.IsEmpty())
	{
		return false;
	}
	if (Entry->PendingPayload.IsSet() &&
		Entry->PendingPayload->SourceGeneration == Entry->DesiredSourceGeneration &&
		Entry->PendingPayload->LaunchOwnerEpoch == CurrentEpoch)
	{
		return false;
	}
	if (Entry->bPrepInFlight &&
		Entry->PrepGeneration == Entry->DesiredSourceGeneration &&
		Entry->PrepEpoch == CurrentEpoch)
	{
		return false;
	}
	OutSourceGeneration = Entry->DesiredSourceGeneration;
	return true;
}

void FVoxelPaintResidencyManager::OnPreparationLaunched(const FVoxelPaintAssetKey& Key, uint64 SourceGeneration, uint64 OwnerEpoch)
{
	check(bInitialized);
	FEntry& Entry = Entries.FindOrAdd(Key);
	Entry.Key = Key;
	Entry.bPrepInFlight = true;
	Entry.PrepGeneration = SourceGeneration;
	Entry.PrepEpoch = OwnerEpoch;
	if (Entry.State == EVoxelPaintAssetResidencyState::NotResident)
	{
		Entry.State = EVoxelPaintAssetResidencyState::Preparing;
	}
}

void FVoxelPaintResidencyManager::OnPreparationCompleted(FVoxelPaintPreparedPayload&& Payload)
{
	check(bInitialized);
	FEntry* EntryPtr = Entries.Find(Payload.AssetKey);
	if (!EntryPtr)
	{
		return;
	}
	FEntry& Entry = *EntryPtr;
	if (Entry.bPrepInFlight &&
		Entry.PrepGeneration == Payload.SourceGeneration &&
		Entry.PrepEpoch == Payload.LaunchOwnerEpoch)
	{
		Entry.bPrepInFlight = false;
	}
	if (Payload.LaunchOwnerEpoch != CurrentEpoch ||
		!Entry.bDesired ||
		Payload.SourceGeneration != Entry.DesiredSourceGeneration ||
		Entry.State == EVoxelPaintAssetResidencyState::ConfigError ||
		Entry.State == EVoxelPaintAssetResidencyState::Published ||
		Entry.State == EVoxelPaintAssetResidencyState::Integrating ||
		Entry.bQueued)
	{
		return;
	}

	const int32 NumPopulated = Payload.Manifest ? Payload.Manifest->PopulatedBricks.Num() : 0;
	const int64 ExpectedBytes = static_cast<int64>(NumPopulated) * WorldVoxelPaint::BrickPayloadBytes;
	const int64 ActualBytes = Payload.BrickPayload ? Payload.BrickPayload->Num() : 0;
	if (!Payload.bPayloadValid || !Payload.Manifest.IsValid() || ActualBytes != ExpectedBytes)
	{
		SetConfigError(Entry, false);
		return;
	}

	Entry.DesiredManifest = Payload.Manifest;
	if (ClassifyOversized(Entry))
	{
		return;
	}
	if (NumPopulated == 0)
	{
		return;
	}
	Entry.PendingPayload.Emplace(MoveTemp(Payload));
	EnqueueClaimant(Entry);
}

void FVoxelPaintResidencyManager::IntegrateStep(uint64 OwnerEpoch, uint64 PublicationOrdinal,
	const FVoxelPaintIntegrationBudget& Budget,
	FVoxelPaintIntegrationOps& OutOps, bool& bOutBindingsChanged)
{
	check(bInitialized);
	if (OwnerEpoch != CurrentEpoch)
	{
		ApplyEpochAdvance(OwnerEpoch);
	}
	LastServicedOrdinal = PublicationOrdinal;

	OutOps.BrickUploads.Reset();
	OutOps.PageWrites.Reset();
	OutOps.bAnyRangeActivated = false;
	bOutBindingsChanged = bPendingBindingsChanged;
	bPendingBindingsChanged = false;

	if (PublicationOrdinal > 0)
	{
		PageAllocator.ReleaseRetired(PublicationOrdinal - 1);
	}
	ReleasePendingSlots(PublicationOrdinal);

	for (TPair<FVoxelPaintAssetKey, FEntry>& Pair : Entries)
	{
		FEntry& Entry = Pair.Value;
		if (Entry.bDesired &&
			Entry.State != EVoxelPaintAssetResidencyState::Published &&
			Entry.State != EVoxelPaintAssetResidencyState::ConfigError &&
			Entry.IntegratingRepIndex == INDEX_NONE &&
			Entry.DesiredManifest && Entry.DesiredManifest->PopulatedBricks.IsEmpty())
		{
			PublishNoTable(Entry);
			bOutBindingsChanged = true;
		}
	}

	while (RetryQueue.Num() > 0)
	{
		const FVoxelPaintAssetKey HeadKey = RetryQueue[0];
		FEntry* HeadEntry = Entries.Find(HeadKey);
		if (!HeadEntry || !HeadEntry->bQueued)
		{
			RetryQueue.RemoveAt(0);
			continue;
		}
		if (!AttemptClaim(*HeadEntry, PublicationOrdinal, bOutBindingsChanged))
		{
			break;
		}
		if (HeadEntry->bQueued)
		{
			HeadEntry->bQueued = false;
			check(RetryQueue.Num() > 0 && RetryQueue[0] == HeadKey);
			RetryQueue.RemoveAt(0);
		}
	}

	int64 BytesRemaining = Budget.MaxUploadBytes;
	int32 EntriesRemaining = Budget.MaxPageEntries;
	int32 ListIndex = 0;
	while (ListIndex < IntegratingList.Num())
	{
		FEntry* EntryPtr = Entries.Find(IntegratingList[ListIndex]);
		if (!EntryPtr || EntryPtr->IntegratingRepIndex == INDEX_NONE)
		{
			IntegratingList.RemoveAt(ListIndex);
			continue;
		}
		FEntry& Entry = *EntryPtr;
		FRepresentation& Rep = Entry.Representations[Entry.IntegratingRepIndex];
		const FVoxelPaintSourceManifest& Manifest = *Rep.Manifest;
		const int32 NumPopulated = Manifest.PopulatedBricks.Num();

		while (Rep.NumBricksEmitted < NumPopulated && BytesRemaining > 0)
		{
			FVoxelPaintBrickUploadOp& Op = OutOps.BrickUploads.AddDefaulted_GetRef();
			Op.PoolSlot = Rep.BrickSlots[Rep.NumBricksEmitted];
			Op.PayloadBrickIndex = Rep.NumBricksEmitted;
			Op.Payload = Rep.PendingUploadPayload;
			BytesRemaining -= WorldVoxelPaint::BrickPayloadBytes;
			++Rep.NumBricksEmitted;
		}
		if (Rep.NumBricksEmitted < NumPopulated)
		{
			break;
		}

		if (!Rep.bRangeUploading)
		{
			PageAllocator.SetState(Rep.PageRangeHandle, EVoxelPaintRangeState::Uploading);
			Rep.bRangeUploading = true;
		}
		const uint32 RangeOffset = static_cast<uint32>(PageAllocator.GetRangeOffset(Rep.PageRangeHandle));
		while (Rep.NumPageEntriesEmitted < Rep.TableEntryCount && EntriesRemaining > 0)
		{
			const uint32 FlatIndex = static_cast<uint32>(Rep.NumPageEntriesEmitted);
			uint32 Value = WorldVoxelPaint::PageEntryAbsent;
			if (Rep.PageCursor < NumPopulated && Manifest.PopulatedBricks[Rep.PageCursor] == FlatIndex)
			{
				Value = static_cast<uint32>(Rep.BrickSlots[Rep.PageCursor]) + 1;
				++Rep.PageCursor;
			}
			FVoxelPaintPageWriteOp& Op = OutOps.PageWrites.AddDefaulted_GetRef();
			Op.EntryIndex = RangeOffset + FlatIndex;
			Op.Value = Value;
			--EntriesRemaining;
			++Rep.NumPageEntriesEmitted;
		}
		if (Rep.NumPageEntriesEmitted < Rep.TableEntryCount)
		{
			break;
		}

		PageAllocator.SetState(Rep.PageRangeHandle, EVoxelPaintRangeState::Active);
		OutOps.bAnyRangeActivated = true;
		Rep.PendingUploadPayload.Reset();
		const int32 RepIndex = Entry.IntegratingRepIndex;
		Entry.IntegratingRepIndex = INDEX_NONE;
		PublishRepresentation(Entry, RepIndex);
		bOutBindingsChanged = true;
		IntegratingList.RemoveAt(ListIndex);
	}
}

EVoxelPaintBindingKind FVoxelPaintResidencyManager::ResolveBinding(const FVoxelPaintAssetKey& Key, uint64 SourceGeneration,
	const FVoxelPaintCanonicalIdentity& Identity, uint32& OutPageTableBaseIndex) const
{
	OutPageTableBaseIndex = WorldVoxelPaint::NoPageTableSentinel;
	if (Key == FVoxelPaintAssetKey())
	{
		return EVoxelPaintBindingKind::Invalid;
	}
	const FEntry* Entry = Entries.Find(Key);
	if (!Entry)
	{
		return EVoxelPaintBindingKind::Invalid;
	}
	for (const FRepresentation& Rep : Entry->Representations)
	{
		if (Rep.bPublished && Rep.Identity == Identity)
		{
			if (!Rep.Manifest || Rep.Manifest->PopulatedBricks.IsEmpty())
			{
				return EVoxelPaintBindingKind::NoTable;
			}
			OutPageTableBaseIndex = static_cast<uint32>(PageAllocator.GetRangeOffset(Rep.PageRangeHandle));
			return EVoxelPaintBindingKind::Data;
		}
	}
	return EVoxelPaintBindingKind::NeutralProxy;
}

EVoxelPaintAssetResidencyState FVoxelPaintResidencyManager::GetAssetState(const FVoxelPaintAssetKey& Key) const
{
	const FEntry* Entry = Entries.Find(Key);
	return Entry ? Entry->State : EVoxelPaintAssetResidencyState::NotResident;
}

void FVoxelPaintResidencyManager::ResetForRetry(const FVoxelPaintAssetKey& Key)
{
	check(bInitialized);
	FEntry* EntryPtr = Entries.Find(Key);
	if (!EntryPtr)
	{
		return;
	}
	FEntry& Entry = *EntryPtr;
	Entry.bPrepInFlight = false;
	Entry.PrepGeneration = 0;
	Entry.PrepEpoch = 0;
	if (Entry.State == EVoxelPaintAssetResidencyState::ConfigError)
	{
		RemoveFromQueue(Entry);
		Entry.bOversized = false;
		Entry.ConfigErrorGeneration = 0;
		Entry.State = EVoxelPaintAssetResidencyState::NotResident;
	}
	RecomputeState(Entry);
}

int32 FVoxelPaintResidencyManager::GetPoolSlotCapacity() const
{
	return BlockAllocator.GetMaxNumBlocks();
}

int32 FVoxelPaintResidencyManager::GetAllocatedBrickCount() const
{
	return BlockAllocator.GetNumAllocated();
}

int32 FVoxelPaintResidencyManager::GetPinnedBrickCount() const
{
	int32 Count = 0;
	for (const TPair<FVoxelPaintAssetKey, FEntry>& Pair : Entries)
	{
		for (int32 Index = 0; Index < Pair.Value.Representations.Num(); ++Index)
		{
			const FRepresentation& Rep = Pair.Value.Representations[Index];
			if (Rep.bPublished || Index == Pair.Value.IntegratingRepIndex)
			{
				Count += Rep.BrickSlots.Num();
			}
		}
	}
	return Count;
}

int32 FVoxelPaintResidencyManager::GetWarmCacheBrickCount() const
{
	int32 Count = 0;
	for (const TPair<FVoxelPaintAssetKey, FEntry>& Pair : Entries)
	{
		for (int32 Index = 0; Index < Pair.Value.Representations.Num(); ++Index)
		{
			const FRepresentation& Rep = Pair.Value.Representations[Index];
			if (!Rep.bPublished && Index != Pair.Value.IntegratingRepIndex)
			{
				Count += Rep.BrickSlots.Num();
			}
		}
	}
	return Count;
}

int32 FVoxelPaintResidencyManager::GetRetryQueueDepth() const
{
	return RetryQueue.Num();
}

int32 FVoxelPaintResidencyManager::GetRetryBlockedBehindHeadCount() const
{
	return FMath::Max(0, RetryQueue.Num() - 1);
}

FString FVoxelPaintResidencyManager::GetRetryQueueHeadDebugName() const
{
	if (RetryQueue.Num() > 0)
	{
		if (const FEntry* Entry = Entries.Find(RetryQueue[0]))
		{
			return Entry->DebugName;
		}
	}
	return FString();
}

int32 FVoxelPaintResidencyManager::GetOversizedConfigErrorCount() const
{
	int32 Count = 0;
	for (const TPair<FVoxelPaintAssetKey, FEntry>& Pair : Entries)
	{
		if (Pair.Value.State == EVoxelPaintAssetResidencyState::ConfigError && Pair.Value.bOversized)
		{
			++Count;
		}
	}
	return Count;
}

int32 FVoxelPaintResidencyManager::GetConfigErrorAssetCount() const
{
	int32 Count = 0;
	for (const TPair<FVoxelPaintAssetKey, FEntry>& Pair : Entries)
	{
		if (Pair.Value.State == EVoxelPaintAssetResidencyState::ConfigError)
		{
			++Count;
		}
	}
	return Count;
}

int32 FVoxelPaintResidencyManager::GetNeutralProxyAssetCount() const
{
	int32 Count = 0;
	for (const TPair<FVoxelPaintAssetKey, FEntry>& Pair : Entries)
	{
		const FEntry& Entry = Pair.Value;
		if (!Entry.bDesired)
		{
			continue;
		}
		uint32 UnusedBaseIndex = 0;
		const EVoxelPaintBindingKind Kind = ResolveBinding(Entry.Key, Entry.DesiredSourceGeneration, Entry.DesiredIdentity, UnusedBaseIndex);
		if (Kind == EVoxelPaintBindingKind::NeutralProxy)
		{
			++Count;
		}
	}
	return Count;
}

int32 FVoxelPaintResidencyManager::GetNoTableAssetCount() const
{
	int32 Count = 0;
	for (const TPair<FVoxelPaintAssetKey, FEntry>& Pair : Entries)
	{
		const FEntry& Entry = Pair.Value;
		if (!Entry.bDesired)
		{
			continue;
		}
		uint32 UnusedBaseIndex = 0;
		const EVoxelPaintBindingKind Kind = ResolveBinding(Entry.Key, Entry.DesiredSourceGeneration, Entry.DesiredIdentity, UnusedBaseIndex);
		if (Kind == EVoxelPaintBindingKind::NoTable)
		{
			++Count;
		}
	}
	return Count;
}

int32 FVoxelPaintResidencyManager::GetPageEntryCapacity() const
{
	return PageAllocator.GetCapacityEntries();
}

int32 FVoxelPaintResidencyManager::GetPageEntries(EVoxelPaintRangeState State) const
{
	return PageAllocator.GetEntries(State);
}

int32 FVoxelPaintResidencyManager::GetPageFreeEntries() const
{
	return PageAllocator.GetFreeEntries();
}

int32 FVoxelPaintResidencyManager::GetPageWasteEntries() const
{
	return PageAllocator.GetFreeEntries() - PageAllocator.GetLargestFreeRange();
}

int32 FVoxelPaintResidencyManager::GetPageHeadroomEntries() const
{
	return PageAllocator.GetLargestFreeRange();
}

#if WITH_EDITOR

void FVoxelPaintResidencyManager::BuildDiagnostics(FVoxelPaintResidencyDiagnostics& Out) const
{
	Out = FVoxelPaintResidencyDiagnostics();
	Out.PoolSlotCapacity = GetPoolSlotCapacity();
	Out.AllocatedBricks = GetAllocatedBrickCount();
	Out.PinnedBricks = GetPinnedBrickCount();
	Out.WarmCacheBricks = GetWarmCacheBrickCount();
	Out.RetryQueueDepth = GetRetryQueueDepth();
	Out.RetryBlockedBehindHead = GetRetryBlockedBehindHeadCount();
	Out.RetryHeadDebugName = GetRetryQueueHeadDebugName();
	Out.OversizedConfigErrors = GetOversizedConfigErrorCount();
	Out.ConfigErrorAssets = GetConfigErrorAssetCount();
	Out.NeutralProxyAssets = GetNeutralProxyAssetCount();
	Out.NoTableAssets = GetNoTableAssetCount();
	Out.PageEntryCapacity = GetPageEntryCapacity();
	Out.PageEntriesActive = GetPageEntries(EVoxelPaintRangeState::Active);
	Out.PageEntriesReserved = GetPageEntries(EVoxelPaintRangeState::Reserved)
		+ GetPageEntries(EVoxelPaintRangeState::Uploading);
	Out.PageEntriesRetiring = GetPageEntries(EVoxelPaintRangeState::Retiring);
	Out.PageEntriesFree = GetPageFreeEntries();
	Out.PageWasteEntries = GetPageWasteEntries();
	Out.PageHeadroomEntries = GetPageHeadroomEntries();
	Out.PublicationOrdinal = LastServicedOrdinal;

	Out.PerAsset.Reserve(Entries.Num());
	for (const TPair<FVoxelPaintAssetKey, FEntry>& Pair : Entries)
	{
		const FEntry& Entry = Pair.Value;
		FVoxelPaintAssetDiagnosticsRow& Row = Out.PerAsset.AddDefaulted_GetRef();
		Row.Key = Entry.Key;
		Row.DebugName = Entry.DebugName;
		Row.State = static_cast<uint8>(Entry.State);
		Row.bConfigError = Entry.State == EVoxelPaintAssetResidencyState::ConfigError;
		Row.bOversized = Entry.bOversized;
		bool bAnyWarmSlots = false;
		for (int32 Index = 0; Index < Entry.Representations.Num(); ++Index)
		{
			const FRepresentation& Rep = Entry.Representations[Index];
			if (Rep.bPublished)
			{
				Row.bPublished = true;
				Row.ResidentBricks = Rep.BrickSlots.Num();
				Row.PageEntries = Rep.TableEntryCount;
				Row.bNoTable = !Rep.Manifest || Rep.Manifest->PopulatedBricks.IsEmpty();
			}
			else if (Index != Entry.IntegratingRepIndex && Rep.BrickSlots.Num() > 0)
			{
				bAnyWarmSlots = true;
			}
		}
		Row.bWarmCacheOnly = !Row.bPublished && bAnyWarmSlots;
	}
}

#endif // WITH_EDITOR

void FVoxelPaintResidencyManager::ApplyEpochAdvance(uint64 NewEpoch)
{
	CurrentEpoch = NewEpoch;
	for (TPair<FVoxelPaintAssetKey, FEntry>& Pair : Entries)
	{
		FEntry& Entry = Pair.Value;
		if (Entry.IntegratingRepIndex != INDEX_NONE)
		{
			CancelIntegration(Entry);
		}
		for (FRepresentation& Rep : Entry.Representations)
		{
			if (Rep.bPublished)
			{
				DemoteRepresentation(Entry, Rep);
			}
		}
		Entry.PendingPayload.Reset();
		Entry.bQueued = false;
		if (Entry.State != EVoxelPaintAssetResidencyState::ConfigError &&
			Entry.State != EVoxelPaintAssetResidencyState::Preparing)
		{
			Entry.State = EVoxelPaintAssetResidencyState::NotResident;
		}
	}
	RetryQueue.Reset();
	bPendingBindingsChanged = true;
	for (TMap<FVoxelPaintAssetKey, FEntry>::TIterator It = Entries.CreateIterator(); It; ++It)
	{
		if (IsEntryForgettable(It->Value))
		{
			It.RemoveCurrent();
		}
	}
}

void FVoxelPaintResidencyManager::RecomputeState(FEntry& Entry) const
{
	if (Entry.State == EVoxelPaintAssetResidencyState::ConfigError)
	{
		return;
	}
	for (int32 Index = 0; Index < Entry.Representations.Num(); ++Index)
	{
		const FRepresentation& Rep = Entry.Representations[Index];
		if (Rep.bPublished && Rep.Identity == Entry.DesiredIdentity)
		{
			Entry.State = EVoxelPaintAssetResidencyState::Published;
			return;
		}
	}
	if (Entry.IntegratingRepIndex != INDEX_NONE)
	{
		Entry.State = EVoxelPaintAssetResidencyState::Integrating;
		return;
	}
	if (Entry.bQueued)
	{
		Entry.State = EVoxelPaintAssetResidencyState::Blocked;
		return;
	}
	if (Entry.bPrepInFlight &&
		Entry.PrepGeneration == Entry.DesiredSourceGeneration &&
		Entry.PrepEpoch == CurrentEpoch)
	{
		Entry.State = EVoxelPaintAssetResidencyState::Preparing;
		return;
	}
	Entry.State = EVoxelPaintAssetResidencyState::NotResident;
}

void FVoxelPaintResidencyManager::ApplyDesiredSupersession(FEntry& Entry, const FVoxelPaintDesiredAsset& Desired)
{
	Entry.DesiredSourceGeneration = Desired.SourceGeneration;
	Entry.DesiredIdentity = Desired.Identity;
	Entry.DesiredManifest = Desired.Manifest;
	if (Entry.State == EVoxelPaintAssetResidencyState::ConfigError &&
		Entry.ConfigErrorGeneration != Desired.SourceGeneration)
	{
		Entry.State = EVoxelPaintAssetResidencyState::NotResident;
		Entry.bOversized = false;
		Entry.ConfigErrorGeneration = 0;
	}
	for (FRepresentation& Rep : Entry.Representations)
	{
		if (Rep.bPublished && Rep.Identity != Entry.DesiredIdentity)
		{
			DemoteRepresentation(Entry, Rep);
			bPendingBindingsChanged = true;
		}
	}
	if (Entry.IntegratingRepIndex != INDEX_NONE &&
		Entry.Representations[Entry.IntegratingRepIndex].Identity != Entry.DesiredIdentity)
	{
		CancelIntegration(Entry);
	}
	if (Entry.PendingPayload.IsSet() && Entry.PendingPayload->SourceGeneration != Entry.DesiredSourceGeneration)
	{
		Entry.PendingPayload.Reset();
		RemoveFromQueue(Entry);
	}
	RecomputeState(Entry);
}

bool FVoxelPaintResidencyManager::IsEntryForgettable(const FEntry& Entry) const
{
	return !Entry.bDesired
		&& Entry.State == EVoxelPaintAssetResidencyState::NotResident
		&& Entry.Representations.IsEmpty()
		&& !Entry.PendingPayload.IsSet()
		&& !Entry.bQueued
		&& !Entry.bPrepInFlight
		&& Entry.IntegratingRepIndex == INDEX_NONE;
}

void FVoxelPaintResidencyManager::ApplyUndesired(FEntry& Entry)
{
	if (Entry.IntegratingRepIndex != INDEX_NONE)
	{
		CancelIntegration(Entry);
	}
	for (FRepresentation& Rep : Entry.Representations)
	{
		if (Rep.bPublished)
		{
			DemoteRepresentation(Entry, Rep);
		}
	}
	Entry.PendingPayload.Reset();
	RemoveFromQueue(Entry);
	if (Entry.State != EVoxelPaintAssetResidencyState::ConfigError)
	{
		Entry.State = EVoxelPaintAssetResidencyState::NotResident;
	}
}

bool FVoxelPaintResidencyManager::ClassifyOversized(FEntry& Entry)
{
	if (!Entry.DesiredManifest || Entry.DesiredManifest->PopulatedBricks.IsEmpty())
	{
		return false;
	}
	const int32 NumPopulated = Entry.DesiredManifest->PopulatedBricks.Num();
	const int64 TableEntries = ComputeTableEntryCount(*Entry.DesiredManifest);
	if (NumPopulated > BlockAllocator.GetMaxNumBlocks() ||
		TableEntries > static_cast<int64>(PageAllocator.GetCapacityEntries()))
	{
		SetConfigError(Entry, true);
		return true;
	}
	return false;
}

void FVoxelPaintResidencyManager::SetConfigError(FEntry& Entry, bool bOversized)
{
	if (Entry.IntegratingRepIndex != INDEX_NONE)
	{
		CancelIntegration(Entry);
	}
	Entry.PendingPayload.Reset();
	RemoveFromQueue(Entry);
	Entry.State = EVoxelPaintAssetResidencyState::ConfigError;
	Entry.bOversized = bOversized;
	Entry.ConfigErrorGeneration = Entry.DesiredSourceGeneration;
}

void FVoxelPaintResidencyManager::CancelIntegration(FEntry& Entry)
{
	check(Entry.IntegratingRepIndex != INDEX_NONE);
	FRepresentation& Rep = Entry.Representations[Entry.IntegratingRepIndex];
	if (Rep.BrickSlots.Num() > 0)
	{
		FPendingSlotRelease& Release = PendingSlotReleases.AddDefaulted_GetRef();
		Release.StampOrdinal = LastServicedOrdinal;
		Release.Slots = MoveTemp(Rep.BrickSlots);
	}
	if (Rep.PageRangeHandle != INDEX_NONE)
	{
		PageAllocator.Retire(Rep.PageRangeHandle, LastServicedOrdinal);
	}
	Entry.Representations.RemoveAt(Entry.IntegratingRepIndex);
	Entry.IntegratingRepIndex = INDEX_NONE;
	IntegratingList.Remove(Entry.Key);
	if (Entry.State == EVoxelPaintAssetResidencyState::Integrating)
	{
		Entry.State = EVoxelPaintAssetResidencyState::NotResident;
	}
}

void FVoxelPaintResidencyManager::DemoteRepresentation(FEntry& Entry, FRepresentation& Representation)
{
	Representation.bPublished = false;
	Representation.DemotionOrdinal = LastServicedOrdinal;
	Entry.LruStamp.Epoch = CurrentEpoch;
	Entry.LruStamp.Ordinal = LastServicedOrdinal;
}

void FVoxelPaintResidencyManager::PublishRepresentation(FEntry& Entry, int32 RepIndex)
{
	for (int32 Index = 0; Index < Entry.Representations.Num(); ++Index)
	{
		if (Index != RepIndex && Entry.Representations[Index].bPublished)
		{
			DemoteRepresentation(Entry, Entry.Representations[Index]);
		}
	}
	Entry.Representations[RepIndex].bPublished = true;
	Entry.State = EVoxelPaintAssetResidencyState::Published;
}

void FVoxelPaintResidencyManager::PublishNoTable(FEntry& Entry)
{
	RemoveFromQueue(Entry);
	int32 MatchIndex = FindMatchingRepresentation(Entry, Entry.DesiredIdentity);
	if (MatchIndex == INDEX_NONE)
	{
		FRepresentation& Rep = Entry.Representations.AddDefaulted_GetRef();
		Rep.SourceGeneration = Entry.DesiredSourceGeneration;
		Rep.Identity = Entry.DesiredIdentity;
		Rep.Manifest = Entry.DesiredManifest;
		Rep.TableEntryCount = 0;
		MatchIndex = Entry.Representations.Num() - 1;
	}
	PublishRepresentation(Entry, MatchIndex);
}

void FVoxelPaintResidencyManager::EnqueueClaimant(FEntry& Entry)
{
	check(!Entry.bQueued);
	Entry.bQueued = true;
	RetryQueue.Add(Entry.Key);
	Entry.State = EVoxelPaintAssetResidencyState::Blocked;
}

void FVoxelPaintResidencyManager::RemoveFromQueue(FEntry& Entry)
{
	if (Entry.bQueued)
	{
		RetryQueue.Remove(Entry.Key);
		Entry.bQueued = false;
	}
}

int32 FVoxelPaintResidencyManager::FindMatchingRepresentation(const FEntry& Entry, const FVoxelPaintCanonicalIdentity& Identity) const
{
	for (int32 Index = 0; Index < Entry.Representations.Num(); ++Index)
	{
		if (Index != Entry.IntegratingRepIndex && Entry.Representations[Index].Identity == Identity)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool FVoxelPaintResidencyManager::AttemptClaim(FEntry& Entry, uint64 Ordinal, bool& bOutBindingsChanged)
{
	const int32 MatchIndex = FindMatchingRepresentation(Entry, Entry.DesiredIdentity);
	if (MatchIndex != INDEX_NONE)
	{
		PublishRepresentation(Entry, MatchIndex);
		bOutBindingsChanged = true;
		return true;
	}
	if (!Entry.PendingPayload.IsSet())
	{
		Entry.State = EVoxelPaintAssetResidencyState::NotResident;
		return true;
	}
	const FVoxelPaintPreparedPayload& Payload = Entry.PendingPayload.GetValue();
	const FVoxelPaintSourceManifest& Manifest = *Payload.Manifest;
	const int32 NumPopulated = Manifest.PopulatedBricks.Num();
	const int64 TableEntries = ComputeTableEntryCount(Manifest);
	if (NumPopulated > BlockAllocator.GetMaxNumBlocks() ||
		TableEntries > static_cast<int64>(PageAllocator.GetCapacityEntries()))
	{
		SetConfigError(Entry, true);
		return true;
	}

	while (BlockAllocator.GetNumFree() < NumPopulated ||
		static_cast<int64>(PageAllocator.GetLargestFreeRange()) < TableEntries)
	{
		if (!EvictNextVictim(Ordinal))
		{
			return false;
		}
	}

	FRepresentation NewRep;
	NewRep.SourceGeneration = Payload.SourceGeneration;
	NewRep.Identity = Payload.Identity;
	NewRep.Manifest = Payload.Manifest;
	NewRep.TableEntryCount = TableEntries;
	NewRep.PendingUploadPayload = Payload.BrickPayload;
	verify(BlockAllocator.Allocate(NumPopulated, NewRep.BrickSlots));
	NewRep.PageRangeHandle = PageAllocator.Reserve(static_cast<int32>(TableEntries));
	check(NewRep.PageRangeHandle != INDEX_NONE);

	Entry.PendingPayload.Reset();
	Entry.Representations.Add(MoveTemp(NewRep));
	Entry.IntegratingRepIndex = Entry.Representations.Num() - 1;
	Entry.State = EVoxelPaintAssetResidencyState::Integrating;
	IntegratingList.Add(Entry.Key);
	return true;
}

bool FVoxelPaintResidencyManager::EvictNextVictim(uint64 Ordinal)
{
	FEntry* Victim = nullptr;
	for (TPair<FVoxelPaintAssetKey, FEntry>& Pair : Entries)
	{
		FEntry& Candidate = Pair.Value;
		bool bHasEvictable = false;
		for (int32 Index = 0; Index < Candidate.Representations.Num(); ++Index)
		{
			const FRepresentation& Rep = Candidate.Representations[Index];
			if (!Rep.bPublished && Index != Candidate.IntegratingRepIndex && Rep.DemotionOrdinal < Ordinal)
			{
				bHasEvictable = true;
				break;
			}
		}
		if (!bHasEvictable)
		{
			continue;
		}
		if (!Victim ||
			Candidate.LruStamp < Victim->LruStamp ||
			(!(Victim->LruStamp < Candidate.LruStamp) &&
				FCString::Strcmp(*Candidate.DebugName, *Victim->DebugName) < 0))
		{
			Victim = &Candidate;
		}
	}
	if (!Victim)
	{
		return false;
	}
	for (int32 Index = Victim->Representations.Num() - 1; Index >= 0; --Index)
	{
		FRepresentation& Rep = Victim->Representations[Index];
		if (!Rep.bPublished && Index != Victim->IntegratingRepIndex && Rep.DemotionOrdinal < Ordinal)
		{
			if (Rep.BrickSlots.Num() > 0)
			{
				BlockAllocator.Free(Rep.BrickSlots);
			}
			if (Rep.PageRangeHandle != INDEX_NONE)
			{
				PageAllocator.Retire(Rep.PageRangeHandle, Rep.DemotionOrdinal);
				PageAllocator.ReleaseRetired(Ordinal > 0 ? Ordinal - 1 : 0);
			}
			Victim->Representations.RemoveAt(Index);
			if (Victim->IntegratingRepIndex > Index)
			{
				--Victim->IntegratingRepIndex;
			}
		}
	}
	return true;
}

void FVoxelPaintResidencyManager::ReleasePendingSlots(uint64 Ordinal)
{
	for (int32 Index = PendingSlotReleases.Num() - 1; Index >= 0; --Index)
	{
		if (PendingSlotReleases[Index].StampOrdinal < Ordinal)
		{
			BlockAllocator.Free(PendingSlotReleases[Index].Slots);
			PendingSlotReleases.RemoveAt(Index);
		}
	}
}
