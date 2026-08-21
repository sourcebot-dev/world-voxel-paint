// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Change.h"
#include "VoxelPaintVolumeData.h"

class WORLDVOXELPAINTTOOLSET_API FVoxelPaintFinishChange final : public FCommandChange
{
public:
	FVoxelPaintFinishChange(FVoxelPaintCanonicalPayload&& InBaseline, FVoxelPaintCanonicalPayload&& InAfter,
		FString InAssetName);

	virtual void Apply(UObject* Object) override;
	virtual void Revert(UObject* Object) override;
	virtual FString ToString() const override;
	virtual SIZE_T GetSize() const override;

	const FVoxelPaintCanonicalPayload& GetBaselineState() const { return Baseline; }
	const FVoxelPaintCanonicalPayload& GetAfterState() const { return After; }

private:
	static void Install(UObject* Object, const FVoxelPaintCanonicalPayload& Payload);

	FVoxelPaintCanonicalPayload Baseline;
	FVoxelPaintCanonicalPayload After;
	FString AssetName;
};
