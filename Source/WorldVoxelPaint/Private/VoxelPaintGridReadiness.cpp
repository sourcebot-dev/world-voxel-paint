// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintGridReadiness.h"

#if WITH_EDITOR

namespace WorldVoxelPaint
{

void FVoxelPaintGridReadinessTracker::Initialize(int32 NumSlots)
{
	Slots.Reset();
	Slots.SetNum(FMath::Max(NumSlots, 0));
	CurrentToken = FVoxelPaintGridReadinessToken();
	Readiness = EVoxelPaintGridReadiness::Pending;
	bCurrentBuildUnobserved = false;
	bRecoveryWanted = false;
	bRecoveryRequested = false;
}

void FVoxelPaintGridReadinessTracker::OnExpectedGridChanged(const FVoxelPaintGridReadinessToken& NewCurrent)
{
	if (NewCurrent == CurrentToken)
	{
		return;
	}
	CurrentToken = NewCurrent;
	Readiness = EVoxelPaintGridReadiness::Pending;
	bCurrentBuildUnobserved = false;
	bRecoveryWanted = false;
	bRecoveryRequested = false;
}

int32 FVoxelPaintGridReadinessTracker::AcquireSlot(const FVoxelPaintGridReadinessToken& Token)
{
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		if (!Slots[Index].bPending)
		{
			Slots[Index].Token = Token;
			Slots[Index].bPending = true;
			if (Token == CurrentToken)
			{
				bCurrentBuildUnobserved = false;
			}
			return Index;
		}
	}
	if (Token == CurrentToken)
	{
		bCurrentBuildUnobserved = true;
	}
	return INDEX_NONE;
}

void FVoxelPaintGridReadinessTracker::OnReadbackResolved(int32 Slot, const FVoxelPaintGridReadbackResult& Result)
{
	if (!Slots.IsValidIndex(Slot) || !Slots[Slot].bPending)
	{
		return;
	}
	Slots[Slot].bPending = false;
	if (Result.Token == CurrentToken)
	{
		Readiness = (Result.bExplicitSuccess && !Result.bMalformed)
			? EVoxelPaintGridReadiness::Ready
			: EVoxelPaintGridReadiness::Failed;
	}
	if (bCurrentBuildUnobserved && Readiness == EVoxelPaintGridReadiness::Pending && !bRecoveryRequested)
	{
		bRecoveryWanted = true;
	}
}

EVoxelPaintGridReadiness FVoxelPaintGridReadinessTracker::GetReadiness() const
{
	return Readiness;
}

bool FVoxelPaintGridReadinessTracker::WantsRecoveryRebuild() const
{
	return bRecoveryWanted;
}

void FVoxelPaintGridReadinessTracker::AcknowledgeRecoveryRequested()
{
	if (bRecoveryWanted)
	{
		bRecoveryWanted = false;
		bRecoveryRequested = true;
	}
}

int32 FVoxelPaintGridReadinessTracker::GetNumPendingSlots() const
{
	int32 Count = 0;
	for (const FSlot& Slot : Slots)
	{
		if (Slot.bPending)
		{
			++Count;
		}
	}
	return Count;
}

}

#endif // WITH_EDITOR
