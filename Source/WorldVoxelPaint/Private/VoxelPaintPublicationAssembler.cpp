// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintPublicationAssembler.h"

#include "VoxelPaintResidency.h"
#include "VoxelPaintSessionRenderTypes.h"
#include "VoxelPaintStats.h"
#include "WorldVoxelPaintConstants.h"

bool IsPlacementSelectorPublished(
	const FVoxelPaintSessionRenderStatus& Status, uint64 SessionGeneration,
	const FVoxelPaintFrontierIdentity& Identity)
{
	return Status.SessionGeneration == SessionGeneration
		&& Status.SelectorPublishedIdentities.Contains(Identity);
}

namespace WorldVoxelPaint
{

// Assembly runs on the render thread in production (tests drive it single-threaded), so the
// change-detection statics below are single-thread-owned.
static void PublishResidencyDiagnostics(const FVoxelPaintResidencyManager& Residency,
	const FVoxelPaintAssemblyStats& Stats)
{
	SET_DWORD_STAT(STAT_VoxelPaint_AllocatedBricks, Residency.GetAllocatedBrickCount());
	SET_DWORD_STAT(STAT_VoxelPaint_PoolCapacityBricks, Residency.GetPoolSlotCapacity());
	SET_DWORD_STAT(STAT_VoxelPaint_PinnedBricks, Residency.GetPinnedBrickCount());
	SET_DWORD_STAT(STAT_VoxelPaint_WarmCacheBricks, Residency.GetWarmCacheBrickCount());
	SET_DWORD_STAT(STAT_VoxelPaint_NeutralProxyPlacements, Stats.NumNeutralProxy);
	SET_DWORD_STAT(STAT_VoxelPaint_NoTablePlacements, Stats.NumNoTable);
	SET_DWORD_STAT(STAT_VoxelPaint_InvalidPlacements, Stats.NumInvalid);
	SET_DWORD_STAT(STAT_VoxelPaint_SessionSelectorPlacements, Stats.NumSessionSelector);
	SET_DWORD_STAT(STAT_VoxelPaint_RetryQueueDepth, Residency.GetRetryQueueDepth());
	SET_DWORD_STAT(STAT_VoxelPaint_RetryBlockedBehindHead, Residency.GetRetryBlockedBehindHeadCount());
	SET_DWORD_STAT(STAT_VoxelPaint_OversizedConfigErrors, Residency.GetOversizedConfigErrorCount());
	SET_DWORD_STAT(STAT_VoxelPaint_PageEntriesActive, Residency.GetPageEntries(EVoxelPaintRangeState::Active));
	SET_DWORD_STAT(STAT_VoxelPaint_PageEntriesReserved,
		Residency.GetPageEntries(EVoxelPaintRangeState::Reserved)
		+ Residency.GetPageEntries(EVoxelPaintRangeState::Uploading));
	SET_DWORD_STAT(STAT_VoxelPaint_PageEntriesRetiring, Residency.GetPageEntries(EVoxelPaintRangeState::Retiring));
	SET_DWORD_STAT(STAT_VoxelPaint_PageEntriesFree, Residency.GetPageFreeEntries());
	SET_DWORD_STAT(STAT_VoxelPaint_PageWasteEntries, Residency.GetPageWasteEntries());
	SET_DWORD_STAT(STAT_VoxelPaint_PageHeadroomEntries, Residency.GetPageHeadroomEntries());

	static FString LastRetryQueueHead;
	FString RetryQueueHead = Residency.GetRetryQueueHeadDebugName();
	if (RetryQueueHead != LastRetryQueueHead)
	{
		if (RetryQueueHead.IsEmpty())
		{
			UE_LOG(LogWorldVoxelPaint, Log, TEXT("Retry queue drained."));
		}
		else
		{
			UE_LOG(LogWorldVoxelPaint, Log, TEXT("Retry queue head: %s (%d blocked behind it)."),
				*RetryQueueHead, Residency.GetRetryBlockedBehindHeadCount());
		}
		LastRetryQueueHead = MoveTemp(RetryQueueHead);
	}
}

EVoxelPaintStagedOutcome ResolveSessionStagedBinding(
	const FVoxelPaintSessionRenderBinding& Binding, bool bMasterIsOrigin,
	EVoxelPaintBindingKind ResidencyKind, const FVoxelPaintCanonicalIdentity& DesiredIdentity)
{
	switch (Binding.Phase)
	{
	case EVoxelPaintSessionBindingPhase::PreCommit:
		return bMasterIsOrigin
			? EVoxelPaintStagedOutcome::UseSessionTexture
			: EVoxelPaintStagedOutcome::ForceNeutralProxy;
	case EVoxelPaintSessionBindingPhase::PostCommit:
	{
		const bool bMatchingComplete = DesiredIdentity == Binding.CommittedIdentity
			&& (ResidencyKind == EVoxelPaintBindingKind::Data
				|| ResidencyKind == EVoxelPaintBindingKind::NoTable);
		if (bMatchingComplete)
		{
			return EVoxelPaintStagedOutcome::UseResidency;
		}
		return bMasterIsOrigin
			? EVoxelPaintStagedOutcome::UseSessionTexture
			: EVoxelPaintStagedOutcome::ForceNeutralProxy;
	}
	default:
		return EVoxelPaintStagedOutcome::UseResidency;
	}
}

void AssembleRecords(TConstArrayView<FVoxelPaintPlacementState> Placements,
	const FVoxelPaintResidencyManager& Residency, const FVector& EvaluationOrigin,
	TArray<FVoxelPaintVolumeRecord>& OutRecords, FVoxelPaintAssemblyStats& OutStats,
	TConstArrayView<FVoxelPaintDesiredAsset> DesiredAssets,
	const FVoxelPaintSessionRenderBinding* SessionBinding, bool bMasterIsOrigin,
	bool& bOutAnySelectorPublished,
	TArray<FVoxelPaintFrontierIdentity>* OutSelectorPublishedIdentities)
{
	OutStats = FVoxelPaintAssemblyStats();
	bOutAnySelectorPublished = false;
	if (OutSelectorPublishedIdentities)
	{
		OutSelectorPublishedIdentities->Reset();
	}
	OutRecords.Reset(Placements.Num());

	TMap<FVoxelPaintAssetKey, const FVoxelPaintDesiredAsset*> DesiredByKey;
	DesiredByKey.Reserve(DesiredAssets.Num());
	for (const FVoxelPaintDesiredAsset& Desired : DesiredAssets)
	{
		DesiredByKey.Add(Desired.Key, &Desired);
	}

	for (const FVoxelPaintPlacementState& State : Placements)
	{
		FVoxelPaintResolvedBinding Binding;
		Binding.SessionSelector = 0u;
		if ((State.Flags & RecordFlag_Valid) == 0u)
		{
			++OutStats.NumInvalid;
		}
		else
		{
			const FVoxelPaintDesiredAsset* const* DesiredEntry = DesiredByKey.Find(State.AssetKey);
			const FVoxelPaintCanonicalIdentity Identity =
				DesiredEntry ? (*DesiredEntry)->Identity : FVoxelPaintCanonicalIdentity();

			uint32 PageTableBaseIndex = NoPageTableSentinel;
			const EVoxelPaintBindingKind Kind = Residency.ResolveBinding(
				State.AssetKey, State.SourceGeneration, Identity, PageTableBaseIndex);

			EVoxelPaintStagedOutcome Outcome = EVoxelPaintStagedOutcome::UseResidency;
			if (SessionBinding && SessionBinding->IsActive() && SessionBinding->AssetKey == State.AssetKey)
			{
				Outcome = ResolveSessionStagedBinding(*SessionBinding, bMasterIsOrigin, Kind, Identity);
			}

			switch (Outcome)
			{
			case EVoxelPaintStagedOutcome::UseSessionTexture:
				Binding.SessionSelector = SessionSelectorActive;
				++OutStats.NumSessionSelector;
				bOutAnySelectorPublished = true;
				if (OutSelectorPublishedIdentities)
				{
					OutSelectorPublishedIdentities->Add(State.Identity);
				}
				break;
			case EVoxelPaintStagedOutcome::ForceNeutralProxy:
				++OutStats.NumNeutralProxy;
				break;
			default:
				switch (Kind)
				{
				case EVoxelPaintBindingKind::Data:
					Binding.PageTableBaseIndex = PageTableBaseIndex;
					++OutStats.NumData;
					break;
				case EVoxelPaintBindingKind::NoTable:
					++OutStats.NumNoTable;
					break;
				default:
					++OutStats.NumNeutralProxy;
					break;
				}
				break;
			}
		}
		OutRecords.Add(PackVolumeRecord(State, Binding, EvaluationOrigin));
	}

	PublishResidencyDiagnostics(Residency, OutStats);
}

void AssembleRecords(TConstArrayView<FVoxelPaintPlacementState> Placements,
	const FVoxelPaintResidencyManager& Residency, const FVector& EvaluationOrigin,
	TArray<FVoxelPaintVolumeRecord>& OutRecords, FVoxelPaintAssemblyStats& OutStats,
	TConstArrayView<FVoxelPaintDesiredAsset> DesiredAssets)
{
	bool bUnusedSelectorPublished = false;
	AssembleRecords(Placements, Residency, EvaluationOrigin, OutRecords, OutStats, DesiredAssets,
		nullptr, false, bUnusedSelectorPublished);
}

void AssembleRecords(TConstArrayView<FVoxelPaintPlacementState> Placements,
	const FVoxelPaintResidencyManager& Residency, const FVector& EvaluationOrigin,
	TArray<FVoxelPaintVolumeRecord>& OutRecords, FVoxelPaintAssemblyStats& OutStats)
{
	static bool bWarnedMissingSideTable = false;
	if (!bWarnedMissingSideTable && Placements.Num() > 0)
	{
		bWarnedMissingSideTable = true;
		UE_LOG(LogWorldVoxelPaint, Warning,
			TEXT("AssembleRecords called without the desired-asset side table: no canonical identity can match, every asset-bearing placement resolves fail-neutral. Pass the pack's FVoxelPaintDesiredAsset array."));
	}
	AssembleRecords(Placements, Residency, EvaluationOrigin, OutRecords, OutStats,
		TConstArrayView<FVoxelPaintDesiredAsset>());
}

}
