// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

namespace WorldVoxelPaint
{
	enum class EVoxelPaintGridReadiness : uint8 { Pending, Ready, Failed };

	struct FVoxelPaintGridReadinessToken
	{
		uint64 OwnerEpoch = 0;
		uint64 LayoutGeneration = 0;

		bool operator==(const FVoxelPaintGridReadinessToken& Other) const
		{
			return OwnerEpoch == Other.OwnerEpoch && LayoutGeneration == Other.LayoutGeneration;
		}
	};

	struct FVoxelPaintGridReadbackResult
	{
		FVoxelPaintGridReadinessToken Token;
		bool bExplicitSuccess = false;
		bool bMalformed = false;
	};

	// Pure CPU core -- tests drive it with FABRICATED results; the coordinator wraps it on the
	// render thread. Only a result whose token equals the CURRENT expected token changes
	// readiness; stale-generation or superseded-epoch results are discarded (slot freed,
	// readiness untouched). Slots are never reused while pending. Saturation is
	// observation-only: the build itself always ran and published.
	class WORLDVOXELPAINT_API FVoxelPaintGridReadinessTracker
	{
	public:
		void Initialize(int32 NumSlots);
		// Every epoch advance and every layout-generation advance resets readiness to Pending:
		void OnExpectedGridChanged(const FVoxelPaintGridReadinessToken& NewCurrent);
		// A rebuild happened; a readback was issued iff a slot was free. INDEX_NONE = saturated
		// -> the current generation goes unobserved.
		int32 AcquireSlot(const FVoxelPaintGridReadinessToken& Token);
		void OnReadbackResolved(int32 Slot, const FVoxelPaintGridReadbackResult& Result);
		EVoxelPaintGridReadiness GetReadiness() const;
		// True when a slot freed while the current generation was built unobserved and readiness
		// is Pending -> the editor requests ONE ordinary recovery rebuild.
		bool WantsRecoveryRebuild() const;
		void AcknowledgeRecoveryRequested();
		int32 GetNumPendingSlots() const;

	private:
		struct FSlot
		{
			FVoxelPaintGridReadinessToken Token;
			bool bPending = false;
		};

		TArray<FSlot> Slots;
		FVoxelPaintGridReadinessToken CurrentToken;
		EVoxelPaintGridReadiness Readiness = EVoxelPaintGridReadiness::Pending;
		bool bCurrentBuildUnobserved = false;
		bool bRecoveryWanted = false;
		bool bRecoveryRequested = false;
	};
}

#endif // WITH_EDITOR
