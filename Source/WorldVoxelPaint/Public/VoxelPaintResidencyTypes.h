// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/SecureHash.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "UObject/ObjectKey.h"

using FVoxelPaintAssetKey = FObjectKey;

struct FVoxelPaintSourceManifest
{
	FIntVector BrickCounts = FIntVector::ZeroValue;
	TArray<uint32> PopulatedBricks;
};

struct FVoxelPaintCanonicalIdentity
{
	FSHAHash Hash;

	bool IsValid() const
	{
		return Hash != FSHAHash();
	}

	bool operator==(const FVoxelPaintCanonicalIdentity& Other) const
	{
		return Hash == Other.Hash;
	}

	bool operator!=(const FVoxelPaintCanonicalIdentity& Other) const
	{
		return !(*this == Other);
	}
};

// AcquirePayload runs INSIDE the preparation task: the editor source captures its block-copied
// BC7 blob at snapshot creation; the cooked source performs the bulk-data read in the task.
struct FVoxelPaintPreparedSourceSnapshot
{
	FVoxelPaintAssetKey AssetKey;
	uint64 SourceGeneration = 0;
	FVoxelPaintCanonicalIdentity Identity;
	TSharedPtr<const FVoxelPaintSourceManifest> Manifest;
	TFunction<bool(TArray<uint8>& OutBrickPayload)> AcquirePayload;
	FString DebugName;
};

struct FVoxelPaintPreparedPayload
{
	FVoxelPaintAssetKey AssetKey;
	uint64 SourceGeneration = 0;
	uint64 LaunchOwnerEpoch = 0;
	FVoxelPaintCanonicalIdentity Identity;
	TSharedPtr<const FVoxelPaintSourceManifest> Manifest;
	TSharedPtr<const TArray<uint8>> BrickPayload;
	bool bPayloadValid = false;
};

enum class EVoxelPaintBindingKind : uint8 { Invalid, NoTable, NeutralProxy, Data };

enum class EVoxelPaintAssetResidencyState : uint8
{
	NotResident,
	Preparing,
	Integrating,
	Published,
	Blocked,
	ConfigError
};

struct FVoxelPaintDesiredAsset
{
	FVoxelPaintAssetKey Key;
	uint64 SourceGeneration = 0;
	FVoxelPaintCanonicalIdentity Identity;
	TSharedPtr<const FVoxelPaintSourceManifest> Manifest;
	FString DebugName;
};

struct FVoxelPaintIntegrationBudget
{
	int64 MaxUploadBytes = 0;
	int32 MaxPageEntries = 0;
};

struct FVoxelPaintBrickUploadOp
{
	int32 PoolSlot = INDEX_NONE;
	int32 PayloadBrickIndex = INDEX_NONE;
	TSharedPtr<const TArray<uint8>> Payload;
};

struct FVoxelPaintPageWriteOp
{
	uint32 EntryIndex = 0;
	uint32 Value = 0;
};

struct FVoxelPaintIntegrationOps
{
	TArray<FVoxelPaintBrickUploadOp> BrickUploads;
	TArray<FVoxelPaintPageWriteOp> PageWrites;
	bool bAnyRangeActivated = false;
};
