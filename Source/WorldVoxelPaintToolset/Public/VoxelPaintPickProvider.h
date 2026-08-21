// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtr.h"

class UVoxelPaintVolumeComponent;
class UWorld;

enum class EVoxelPaintStrokeOp : uint8
{
	Paint,
	Erase,
	Flood,
	Invert,
	Swap
};

struct FVoxelPaintBrushParams
{
	float RadiusWorld = 100.0f;
	float Strength = 1.0f;
	float FalloffHotspot = 0.5f;
	float FlattenPct = 0.0f;
	float AngleToleranceDeg = 90.0f;
	FLinearColor Color = FLinearColor::White;
	uint32 ChannelMask = 0xF;
	uint8 SwapSrcChannel = 0;
	uint8 SwapDstChannel = 1;
};

struct FVoxelPaintStampRequest
{
	TWeakObjectPtr<UWorld> World;
	FVector RayOrigin = FVector::ZeroVector;
	FVector RayDirection = FVector::ForwardVector;
	FVoxelPaintBrushParams Brush;
	TWeakObjectPtr<UVoxelPaintVolumeComponent> Target;
};

// The ONE stage-2 work-list format: M4's GPU picker must produce exactly this.
struct FVoxelPaintGatherList
{
	TArray<uint32> TexelIndices;
	FIntVector DirtyMin = FIntVector::ZeroValue;
	FIntVector DirtyMax = FIntVector::ZeroValue;
	TArray<uint32> DirtyBricks;
	FVector3f BrushCenterTexel = FVector3f::ZeroVector;
	float BrushRadiusTexels = 0.0f;

	bool IsEmpty() const
	{
		return TexelIndices.IsEmpty();
	}
};

class WORLDVOXELPAINTTOOLSET_API IVoxelPaintPickProvider
{
public:
	virtual ~IVoxelPaintPickProvider() = default;

	// Async-shaped: the CPU provider invokes OnReady synchronously; a GPU provider may deliver
	// frames later. A request that hits nothing delivers an empty list.
	virtual void RequestStampSelection(const FVoxelPaintStampRequest& Request,
		TFunction<void(FVoxelPaintGatherList&&)> OnReady) = 0;
};

class WORLDVOXELPAINTTOOLSET_API FVoxelPaintCpuPickProvider : public IVoxelPaintPickProvider
{
public:
	static constexpr int32 MaxVisibilityCandidates = 32768;

	virtual void RequestStampSelection(const FVoxelPaintStampRequest& Request,
		TFunction<void(FVoxelPaintGatherList&&)> OnReady) override;
};
