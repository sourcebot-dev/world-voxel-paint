// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace WorldVoxelPaint
{
	enum class EVoxelPaintSessionGpuCategory : uint8
	{
		SessionPair,
		Scratch,
		TransferStaging,
		RetirementOverlap
	};

	class WORLDVOXELPAINTTOOLSET_API FVoxelPaintSessionBudgetLedger
	{
	public:
		void Initialize(int64 InBudgetBytes);
		bool Reserve(EVoxelPaintSessionGpuCategory Category, int64 Bytes, int32& OutHandle);
		void Release(int32 Handle);
		bool BeginTransferGeneration();
		void EndTransferGeneration();
		bool IsTransferLive() const;
		int64 GetUsedBytes() const;
		int64 GetUsedBytes(EVoxelPaintSessionGpuCategory Category) const;
		int64 GetBudgetBytes() const;

	private:
		struct FReservation
		{
			int32 Handle = INDEX_NONE;
			EVoxelPaintSessionGpuCategory Category = EVoxelPaintSessionGpuCategory::SessionPair;
			int64 Bytes = 0;
		};

		TArray<FReservation> Reservations;
		int64 BudgetBytes = 0;
		int64 UsedBytes = 0;
		int64 UsedBytesByCategory[4] = { 0, 0, 0, 0 };
		int32 NextHandle = 1;
		bool bTransferLive = false;
	};
}
