// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Serialization/BulkData.h"
#include "Templates/SharedPointer.h"
#include "UObject/Object.h"
#include "VoxelPaintResidencyTypes.h"
#include "VoxelPaintTypes.h"

#if WITH_EDITORONLY_DATA
#include "Serialization/EditorBulkData.h"
#endif

#include "VoxelPaintVolumeData.generated.h"

class ITargetPlatform;
class UVolumeTexture;

namespace WorldVoxelPaint
{
	struct FVoxelPaintCanonicalBuild;
}

#if WITH_EDITOR
struct FVoxelPaintTopologyTriplet
{
	FVector LocalHalfExtent = FVector(500.0);
	FIntVector BrickCounts = FIntVector(4, 4, 4);
	FVector StorageOrigin = FVector::ZeroVector;
};

struct FVoxelPaintCanonicalPayload
{
	TArray<uint8> PackedSourceBGRA8;
	TArray<uint32> ManifestBricks;
	TArray<uint8> IdentityBytes;
	FIntVector BrickCounts = FIntVector(4, 4, 4);
	FVector LocalHalfExtent = FVector(500.0);
	FVector StorageOrigin = FVector::ZeroVector;
	bool bInitialized = false;

	int64 GetBytes() const
	{
		return (int64)sizeof(*this)
			+ PackedSourceBGRA8.Num()
			+ (int64)ManifestBricks.Num() * sizeof(uint32)
			+ IdentityBytes.Num();
	}
};
#endif

USTRUCT()
struct FVoxelPaintBrickManifest
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<uint32> PopulatedBricks;
};

USTRUCT()
struct FVoxelPaintCookedPayloadHeader
{
	GENERATED_BODY()

	UPROPERTY()
	uint32 LayoutVersion = 0;

	UPROPERTY()
	uint32 NumPackedRecords = 0;

	UPROPERTY()
	uint64 PayloadBytes = 0;
};

UCLASS()
class WORLDVOXELPAINT_API UVoxelPaintVolumeData : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Voxel Paint", meta = (ClampMin = "1.0"))
	FVector LocalHalfExtent = FVector(500.0);

	UPROPERTY(EditAnywhere, Category = "Voxel Paint", meta = (ClampMin = "1"))
	FIntVector BrickCounts = FIntVector(4, 4, 4);

	UPROPERTY(EditAnywhere, Category = "Voxel Paint")
	FVector StorageOrigin = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Paint")
	bool bInitialized = false;

	UPROPERTY()
	FVoxelPaintBrickManifest BrickManifest;

	UPROPERTY(VisibleAnywhere, AssetRegistrySearchable, Category = "Voxel Paint|Authority")
	EVoxelPaintAssetSharingMode SharingMode = EVoxelPaintAssetSharingMode::Private;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Paint|Authority")
	FGuid OwnerActorGuid;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Paint|Authority")
	FString OwnerDisplayName;

	FIntVector GetResolution() const;
	FVector GetTexelSize() const;

	uint64 GetSourceGeneration() const;
	FVoxelPaintCanonicalIdentity GetCanonicalIdentity() const;
	TSharedPtr<const FVoxelPaintSourceManifest> GetManifestSnapshot() const;
	FIntVector GetBrickCounts() const;
	FVector GetStorageMin() const;

#if WITH_EDITOR
	void CommitCanonicalSource(const WorldVoxelPaint::FVoxelPaintCanonicalBuild& Build);
	void InstallCanonicalState(const FVoxelPaintCanonicalPayload& Payload);
	void CaptureCanonicalPayload(FVoxelPaintCanonicalPayload& Out) const;
	void ResetCanonicalData();
#endif

	// A null return with bOutCapturePending set means the source build is still in flight and a
	// later call at the same source generation can succeed; a null return without it is final
	// for the current state (nothing to capture).
	TSharedPtr<FVoxelPaintPreparedSourceSnapshot> CreatePreparationSnapshot(bool* bOutCapturePending = nullptr);

	virtual void PostLoad() override;
	virtual void PostDuplicate(bool bDuplicateForPIE) override;
	virtual void Serialize(FArchive& Ar) override;
#if WITH_EDITOR
	virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void BeginCacheForCookedPlatformData(const ITargetPlatform* TargetPlatform) override;
	virtual bool IsCachedCookedPlatformDataLoaded(const ITargetPlatform* TargetPlatform) override;
	virtual void ClearCachedCookedPlatformData(const ITargetPlatform* TargetPlatform) override;
	virtual void ClearAllCachedCookedPlatformData() override;
#endif

private:
	void RefreshManifestSnapshot();
	TSharedPtr<FVoxelPaintPreparedSourceSnapshot> MakeSnapshotShell() const;
#if WITH_EDITOR
	bool GetPackedSourceBytes(TArray<uint8>& OutBytes) const;
	bool ValidatePackedSourceBytesAgainstManifest(int64 NumBytes, int32 NumManifestBricks,
		const TCHAR* UseSite) const;
	UVolumeTexture* CreateBuildHelperTexture(TConstArrayView<uint8> PackedBytes) const;
	void ReleaseAdmissionHelper();
	void HarvestCookedPayload();
	TSharedPtr<FVoxelPaintPreparedSourceSnapshot> CreatePreparationSnapshot_Editor(bool* bOutCapturePending);
	void RecomputeCanonicalIdentityFromStored();
	void NotifyConsumingComponents(const FVoxelPaintTopologyTriplet* OldTopology = nullptr);
#else
	TSharedPtr<FVoxelPaintPreparedSourceSnapshot> CreatePreparationSnapshot_Cooked();
#endif

#if WITH_EDITORONLY_DATA
	UE::Serialization::FEditorBulkData PackedSourceBulk;

	UPROPERTY(Transient)
	TObjectPtr<UVolumeTexture> AdmissionHelperTexture;

	uint64 AdmissionHelperGeneration = 0;

	UPROPERTY(Transient)
	TObjectPtr<UVolumeTexture> CookHelperTexture;

	FVector PreEditLocalHalfExtent = FVector(500.0);
	FIntVector PreEditBrickCounts = FIntVector(4, 4, 4);
	FVector PreEditStorageOrigin = FVector::ZeroVector;
#endif

	UPROPERTY()
	TArray<uint8> CanonicalIdentityBytes;

	UPROPERTY()
	FVoxelPaintCookedPayloadHeader CookedHeader;

	FByteBulkData CookedBrickPayload;

	uint64 SourceGeneration = 1;
	TSharedPtr<const FVoxelPaintSourceManifest> ManifestSnapshot;
};

#if WITH_EDITOR
namespace WorldVoxelPaint
{
	// Editor-only commit notification (M6 plan 5.3). Fired on the game thread from the tail of
	// UVoxelPaintVolumeData::InstallCanonicalState - the single funnel both
	// CommitCanonicalSource (paint commit) and ResetCanonicalData (paint clear) route through -
	// after the source generation advanced, the manifest snapshot refreshed, and consuming
	// components were notified; a subscriber reading GetSourceGeneration() sees the new value.
	// Validation-failure early-outs in InstallCanonicalState never broadcast. Deliberately
	// PCG-agnostic: announces "this paint asset's content changed", nothing more - subscribers
	// enumerate placements themselves (see NotifyConsumingComponents' TObjectIterator scan).
	// Property-edit refresh (PostEditChangeProperty) intentionally does NOT broadcast:
	// commit-time only, per plan sections 5.3 and 10.
	WORLDVOXELPAINT_API TMulticastDelegate<void(UVoxelPaintVolumeData*)>& OnVolumeDataCommitted();
}
#endif
