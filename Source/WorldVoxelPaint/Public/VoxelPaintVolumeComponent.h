// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "VoxelPaintTypes.h"
#include "VoxelPaintVolumeComponent.generated.h"

class UVoxelPaintVolumeData;

USTRUCT()
struct WORLDVOXELPAINT_API FVoxelPaintVariantPoolEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Voxel Paint")
	TObjectPtr<UVoxelPaintVolumeData> Variant;

	UPROPERTY(EditAnywhere, Category = "Voxel Paint", meta = (ClampMin = "0.0"))
	float Weight = 1.0f;

	bool operator==(const FVoxelPaintVariantPoolEntry& Other) const
	{
		return Variant == Other.Variant && Weight == Other.Weight;
	}
};

USTRUCT()
struct WORLDVOXELPAINT_API FVoxelPaintExpectedTopology
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Voxel Paint")
	FVector LocalHalfExtent = FVector(500.0);

	UPROPERTY(EditAnywhere, Category = "Voxel Paint")
	FIntVector BrickCounts = FIntVector(4, 4, 4);

	UPROPERTY(EditAnywhere, Category = "Voxel Paint")
	FVector StorageOrigin = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Paint")
	bool bIsSet = false;

	bool Matches(const UVoxelPaintVolumeData& Asset) const;

	bool operator==(const FVoxelPaintExpectedTopology& Other) const
	{
		return LocalHalfExtent == Other.LocalHalfExtent
			&& BrickCounts == Other.BrickCounts
			&& StorageOrigin == Other.StorageOrigin
			&& bIsSet == Other.bIsSet;
	}
};

UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent))
class WORLDVOXELPAINT_API UVoxelPaintVolumeComponent : public USceneComponent
{
	GENERATED_BODY()
public:
	UVoxelPaintVolumeComponent(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditAnywhere, Category = "Voxel Paint")
	EVoxelPaintBlendMode BlendModeR = EVoxelPaintBlendMode::Add;
	UPROPERTY(EditAnywhere, Category = "Voxel Paint")
	EVoxelPaintBlendMode BlendModeG = EVoxelPaintBlendMode::Add;
	UPROPERTY(EditAnywhere, Category = "Voxel Paint")
	EVoxelPaintBlendMode BlendModeB = EVoxelPaintBlendMode::Add;
	UPROPERTY(EditAnywhere, Category = "Voxel Paint")
	EVoxelPaintBlendMode BlendModeA = EVoxelPaintBlendMode::Add;

	UPROPERTY(EditAnywhere, Category = "Voxel Paint")
	TArray<FVoxelPaintVariantPoolEntry> VolumeVariants;

	UPROPERTY(EditAnywhere, Category = "Voxel Paint")
	FVoxelPaintExpectedTopology ExpectedTopology;

	UPROPERTY(EditInstanceOnly, Category = "Voxel Paint")
	TObjectPtr<UVoxelPaintVolumeData> VariantOverride;

	UVoxelPaintVolumeData* ResolveActiveVariant() const;
	UVoxelPaintVolumeData* ResolveVariantLocal() const;
	int32 GetResolvedVariantIndex() const;
	bool PoolContains(const UVoxelPaintVolumeData* Asset) const;

	bool HasValidVolumeData() const;
	FTransform GetRigidPlacementFrame() const;
	FVector GetEffectiveLocalHalfExtent() const;
	FIntVector GetEffectiveResolution() const;
	FVector GetEffectiveTexelSize() const;
	FVector4f GetBlendCoefficients() const;
	int32 GetRegistrySlotIndex() const;
	void SetRegistrySlotIndex(int32 InSlotIndex);
	int32 GetFrontierSlotIndex() const;
	void SetFrontierSlotIndex(int32 InFrontierSlotIndex);

#if WITH_EDITOR
	void ResetPoolToArchetype();
	void CapturePreEditPoolState();
#endif

	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual bool RequiresGameThreadEndOfFrameUpdates() const override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void SendRenderTransform_Concurrent() override;
#if WITH_EDITOR
	virtual bool CanEditChange(const FProperty* InProperty) const override;
	virtual bool CanEditChange(const FEditPropertyChain& PropertyChain) const override;
	virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostDuplicate(bool bDuplicateForPIE) override;
#endif

private:
	void CaptureSeedState();
#if WITH_EDITOR
	bool AllowsPoolEdits() const;
#endif

	// Retained for the component object's lifetime; recaptured on OnRegister only while unset,
	// so WP cell hide/show re-registration never rerolls. BP reinstancing creates a NEW
	// component whose OnRegister recaptures - safe in editor builds (actor guids re-readable
	// there); cooked builds never reinstance.
	bool bSeedCaptured = false;
	bool bTier1Seed = false;
	uint64 RetainedTier1Seed = 0;

#if WITH_EDITORONLY_DATA
	TArray<FVoxelPaintVariantPoolEntry> PreEditPoolCache;
	FVoxelPaintExpectedTopology PreEditExpectationCache;
#endif

	UPROPERTY(Transient, DuplicateTransient, NonTransactional)
	int32 SlotIndex = INDEX_NONE;

	// Subsystem-owned O(1) frontier-lookup cache; always verified against the slot's weak
	// component before trust (UVoxelPaintWorldSubsystem::FindOccupiedFrontierSlot).
	UPROPERTY(Transient, DuplicateTransient, NonTransactional)
	int32 FrontierSlotIndex = INDEX_NONE;
};
