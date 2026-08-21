// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "InteractiveTool.h"
#include "InteractiveToolBuilder.h"
#include "UObject/WeakObjectPtr.h"
#include "VoxelPaintDetailLadder.h"
#include "VoxelPaintSizingTool.generated.h"

class UVoxelPaintSizingTool;
class UVoxelPaintVolumeComponent;
class UVoxelPaintVolumeData;

UENUM()
enum class EVoxelPaintSizingStatusLevel : uint8
{
	NoAsset,
	Editable,
	Frozen,
	Error
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintSizingToolProperties : public UInteractiveToolPropertySet
{
	GENERATED_BODY()
public:
	UPROPERTY(VisibleAnywhere, Category = "Asset",
		meta = (ToolTip = "The volume data asset whose size and detail this tool edits."))
	FString AssetName;

	UPROPERTY(VisibleAnywhere, Category = "Asset", DisplayName = "Initialized (Topology Frozen)",
		meta = (ToolTip = "Set by the first paint stroke. While set, Size and Detail are read-only; Reset Data and Unlock Size is the only way to edit them again."))
	bool bAssetInitialized = false;

	// The three inputs below are locked while bAssetInitialized is set. That enablement is
	// bound as a live attribute by FVoxelPaintSizingDetails rather than an EditCondition:
	// the tool flips bAssetInitialized directly in C++ (Reset, undo, external edits), and
	// FPropertyNode caches metadata edit-condition results until the panel rebuilds, which
	// would leave the rows stale until the user switches tools.
	UPROPERTY(EditAnywhere, Category = "Size and Detail", DisplayName = "Half Extent (cm)",
		meta = (ClampMin = "1.0",
			ToolTip = "Half of the volume's size in cm along each local axis. Edits apply to the asset immediately while unlocked. Together with Detail Rungs this determines the resolved texel size."))
	FVector HalfExtent = FVector(500.0);

	UPROPERTY(EditAnywhere, Category = "Size and Detail", DisplayName = "Detail Rungs (Bricks)",
		meta = (ClampMin = "1", UIMin = "1",
			ToolTip = "Bricks per axis on the exact-fit detail ladder. Each brick contributes 7 texels on its axis, so Resolution = Rungs x 7. More rungs mean finer texels and a larger worst-case pool."))
	FIntVector DetailRungs = FIntVector(4, 4, 4);

	UPROPERTY(EditAnywhere, Category = "Size and Detail", DisplayName = "Desired Texel Size (cm)",
		meta = (ClampMin = "0.01",
			ToolTip = "Target texel size for the Snap button below. Not applied directly: Snap picks, per axis, the detail rung whose resolved texel size is closest to this value."))
	double DesiredTexelSize = 10.0;

	UPROPERTY(VisibleAnywhere, Category = "Derived", DisplayName = "Resolved Texel Size (cm)",
		meta = (ToolTip = "Derived: (2 x Half Extent) / Resolution. The actual world-space size of one texel on each axis."))
	FVector ResolvedTexelSize = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, Category = "Derived", DisplayName = "Resolution (Texels)",
		meta = (ToolTip = "Derived: Detail Rungs x 7 texels per axis."))
	FIntVector Resolution = FIntVector::ZeroValue;

	UPROPERTY(VisibleAnywhere, Category = "Cost Preview", DisplayName = "Worst-Case Bricks",
		meta = (ToolTip = "Derived: Rungs X x Y x Z. Pool bricks needed if every brick of the volume gets painted."))
	int64 WorstCaseBricks = 0;

	UPROPERTY(VisibleAnywhere, Category = "Cost Preview", DisplayName = "Worst-Case Pool Memory",
		meta = (ToolTip = "Worst-Case Bricks x 512 B of payload per brick."))
	FString WorstCasePoolMemory;

	UPROPERTY(VisibleAnywhere, Category = "Cost Preview", DisplayName = "Populated Bricks",
		meta = (ToolTip = "Bricks of this asset that currently hold painted data."))
	int32 PopulatedBricks = 0;

	UPROPERTY(VisibleAnywhere, Category = "Cost Preview", DisplayName = "Current Pool Memory",
		meta = (ToolTip = "Populated Bricks x 512 B of payload per brick."))
	FString CurrentPoolMemory;

	// Rendered as a wrapped, color-coded row by the sizing details customization.
	UPROPERTY(VisibleAnywhere, Category = "Status")
	FString Status;

	UPROPERTY(meta = (TransientToolProperty))
	EVoxelPaintSizingStatusLevel StatusLevel = EVoxelPaintSizingStatusLevel::NoAsset;

	// Invoked by the buttons the sizing details customization adds.
	void SnapToDesiredTexelSize();
	void ResetDataAndUnlockSize();

	TWeakObjectPtr<UVoxelPaintSizingTool> OwnerTool;
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintSizingToolBuilder : public UInteractiveToolBuilder
{
	GENERATED_BODY()
public:
	virtual bool CanBuildTool(const FToolBuilderState& SceneState) const override;
	virtual UInteractiveTool* BuildTool(const FToolBuilderState& SceneState) const override;
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintSizingTool : public UInteractiveTool
{
	GENERATED_BODY()
public:
	void SetTargetPlacement(UVoxelPaintVolumeComponent* InPlacement);

	virtual void Setup() override;
	virtual void OnTick(float DeltaTime) override;
	virtual void OnPropertyModified(UObject* PropertySet, FProperty* Property) override;
	virtual bool HasCancel() const override { return false; }
	virtual bool HasAccept() const override { return false; }
	virtual bool CanAccept() const override { return false; }

	void SnapToDesiredTexelSize();
	void ResetDataAndUnlockSize();

private:
	UVoxelPaintVolumeData* GetAsset() const;
	WorldVoxelPaint::FVoxelPaintLadderClamps GetClamps() const;
	WorldVoxelPaint::FVoxelPaintLadderState BuildStateFromProperties() const;
	bool CommitState(const WorldVoxelPaint::FVoxelPaintLadderState& NewState);
	void SyncFromAsset();

	UPROPERTY(Transient)
	TObjectPtr<UVoxelPaintSizingToolProperties> Properties;

	TWeakObjectPtr<UVoxelPaintVolumeComponent> TargetPlacement;
	FVector LastSyncedHalfExtent = FVector::ZeroVector;
	FIntVector LastSyncedBrickCounts = FIntVector::ZeroValue;
	bool bLastSyncedInitialized = false;
	int32 LastSyncedPopulatedBricks = INDEX_NONE;
};
