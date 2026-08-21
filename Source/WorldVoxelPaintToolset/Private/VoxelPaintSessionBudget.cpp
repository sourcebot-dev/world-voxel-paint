// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintSessionBudget.h"

namespace WorldVoxelPaint
{

void FVoxelPaintSessionBudgetLedger::Initialize(int64 InBudgetBytes)
{
	Reservations.Reset();
	BudgetBytes = FMath::Max<int64>(0, InBudgetBytes);
	UsedBytes = 0;
	for (int64& CategoryBytes : UsedBytesByCategory)
	{
		CategoryBytes = 0;
	}
	NextHandle = 1;
	bTransferLive = false;
}

bool FVoxelPaintSessionBudgetLedger::Reserve(EVoxelPaintSessionGpuCategory Category, int64 Bytes, int32& OutHandle)
{
	OutHandle = INDEX_NONE;

	if (Bytes < 0)
	{
		return false;
	}
	if (UsedBytes + Bytes > BudgetBytes)
	{
		return false;
	}

	FReservation Reservation;
	Reservation.Handle = NextHandle++;
	Reservation.Category = Category;
	Reservation.Bytes = Bytes;
	Reservations.Add(Reservation);

	UsedBytes += Bytes;
	UsedBytesByCategory[(uint8)Category] += Bytes;
	OutHandle = Reservation.Handle;
	return true;
}

void FVoxelPaintSessionBudgetLedger::Release(int32 Handle)
{
	const int32 Index = Reservations.IndexOfByPredicate(
		[Handle](const FReservation& Reservation) { return Reservation.Handle == Handle; });
	checkf(Index != INDEX_NONE, TEXT("Release of unknown budget handle: %d"), Handle);

	const FReservation& Reservation = Reservations[Index];
	UsedBytes -= Reservation.Bytes;
	UsedBytesByCategory[(uint8)Reservation.Category] -= Reservation.Bytes;
	Reservations.RemoveAtSwap(Index);
}

bool FVoxelPaintSessionBudgetLedger::BeginTransferGeneration()
{
	if (bTransferLive)
	{
		return false;
	}
	bTransferLive = true;
	return true;
}

void FVoxelPaintSessionBudgetLedger::EndTransferGeneration()
{
	checkf(bTransferLive, TEXT("EndTransferGeneration without a live transfer generation"));
	bTransferLive = false;
}

bool FVoxelPaintSessionBudgetLedger::IsTransferLive() const
{
	return bTransferLive;
}

int64 FVoxelPaintSessionBudgetLedger::GetUsedBytes() const
{
	return UsedBytes;
}

int64 FVoxelPaintSessionBudgetLedger::GetUsedBytes(EVoxelPaintSessionGpuCategory Category) const
{
	return UsedBytesByCategory[(uint8)Category];
}

int64 FVoxelPaintSessionBudgetLedger::GetBudgetBytes() const
{
	return BudgetBytes;
}

}
