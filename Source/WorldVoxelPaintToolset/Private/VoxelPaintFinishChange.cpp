// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintFinishChange.h"

#include "Templates/Casts.h"
#include "WorldVoxelPaintConstants.h"

FVoxelPaintFinishChange::FVoxelPaintFinishChange(FVoxelPaintCanonicalPayload&& InBaseline,
	FVoxelPaintCanonicalPayload&& InAfter, FString InAssetName)
	: Baseline(MoveTemp(InBaseline))
	, After(MoveTemp(InAfter))
	, AssetName(MoveTemp(InAssetName))
{
}

void FVoxelPaintFinishChange::Apply(UObject* Object)
{
	Install(Object, After);
}

void FVoxelPaintFinishChange::Revert(UObject* Object)
{
	Install(Object, Baseline);
}

void FVoxelPaintFinishChange::Install(UObject* Object, const FVoxelPaintCanonicalPayload& Payload)
{
	UVoxelPaintVolumeData* Asset = Cast<UVoxelPaintVolumeData>(Object);
	if (!Asset)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("FVoxelPaintFinishChange: transaction object '%s' is not a UVoxelPaintVolumeData - install skipped."),
			*GetNameSafe(Object));
		return;
	}
	Asset->InstallCanonicalState(Payload);
}

FString FVoxelPaintFinishChange::ToString() const
{
	return FString::Printf(TEXT("Voxel paint Finish on %s"), *AssetName);
}

SIZE_T FVoxelPaintFinishChange::GetSize() const
{
	return sizeof(*this) + (SIZE_T)Baseline.GetBytes() + (SIZE_T)After.GetBytes();
}
