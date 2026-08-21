// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BaseTools/BaseBrushTool.h"
#include "BaseTools/SingleClickTool.h"
#include "InteractiveToolBuilder.h"
#include "Templates/UniquePtr.h"
#include "UObject/WeakObjectPtr.h"
#include "VoxelPaintGpuPickProvider.h"
#include "VoxelPaintPickProvider.h"
#include "VoxelPaintStrokePipeline.h"
#include "VoxelPaintTools.generated.h"

class UTexture2D;
class UVoxelPaintVolumeComponent;
class UVoxelPaintWholeVolumeTool;
class UVoxelPaintCreateVolumeTool;

// Process-lifetime holder for the one stroke pipeline + pick providers the editor tools share.
// The pipeline is never destroyed while the process lives, so pending transfer callbacks stay
// safe; stale session bindings are dropped by generation before any use.
class WORLDVOXELPAINTTOOLSET_API FVoxelPaintToolsContext
{
public:
	static FVoxelPaintToolsContext& Get();

	FVoxelPaintStrokePipeline& GetPipeline() { return Pipeline; }
	IVoxelPaintPickProvider& GetPickProvider();
	FName GetLastPickSource() const;
	bool HasInFlightPickRequests() const;
	// Drains and destroys the GPU provider; module shutdown calls it BEFORE the session
	// controller is torn down so no pending pick delivery outlives the session surfaces.
	void ShutdownGpuPickProvider();

	// Unbinds the pipeline when the controller's session closed or changed generation.
	void SyncPipelineToSession();
	// Opens a session on the placement when none is open, then (re)binds the pipeline to the
	// controller's session surfaces. False = denial/remedy text in OutError.
	bool EnsureSessionFor(UVoxelPaintVolumeComponent* Placement, FText& OutError);

	static UVoxelPaintVolumeComponent* FindSelectedPlacement();
	static void NotifyUser(const FText& Message);

private:
	enum class EPickRoute : uint8
	{
		None,
		Cpu,
		Gpu
	};

	FVoxelPaintStrokePipeline Pipeline;
	FVoxelPaintCpuPickProvider PickProvider;
	TUniquePtr<FVoxelPaintGpuPickProvider> GpuPickProvider;
	EPickRoute LastPickRoute = EPickRoute::None;
	uint64 BoundSessionGeneration = 0;
	bool bBound = false;
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintBrushToolProperties : public UInteractiveToolPropertySet
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Voxel Brush")
	FLinearColor Color = FLinearColor::White;

	UPROPERTY(EditAnywhere, Category = "Voxel Brush", DisplayName = "Erase")
	bool bErase = false;

	UPROPERTY(EditAnywhere, Category = "Voxel Brush", meta = (UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0"))
	float FlattenPct = 0.0f;

	UPROPERTY(EditAnywhere, Category = "Voxel Brush", meta = (UIMin = "0.0", UIMax = "180.0", ClampMin = "0.0", ClampMax = "180.0"))
	float AngleToleranceDeg = 90.0f;

	UPROPERTY(EditAnywhere, Category = "Channel Mask", DisplayName = "Red")
	bool bWriteRed = true;

	UPROPERTY(EditAnywhere, Category = "Channel Mask", DisplayName = "Green")
	bool bWriteGreen = true;

	UPROPERTY(EditAnywhere, Category = "Channel Mask", DisplayName = "Blue")
	bool bWriteBlue = true;

	UPROPERTY(EditAnywhere, Category = "Channel Mask", DisplayName = "Alpha")
	bool bWriteAlpha = true;

	uint32 GetChannelMask() const;
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintWholeVolumeToolProperties : public UInteractiveToolPropertySet
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Whole Volume", meta = (EditCondition = "bShowColor", HideEditConditionToggle, EditConditionHides))
	FLinearColor Color = FLinearColor::White;

	UPROPERTY(EditAnywhere, Category = "Whole Volume", meta = (UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bShowColor", HideEditConditionToggle, EditConditionHides))
	float Strength = 1.0f;

	UPROPERTY(EditAnywhere, Category = "Channel Mask", DisplayName = "Red")
	bool bWriteRed = true;

	UPROPERTY(EditAnywhere, Category = "Channel Mask", DisplayName = "Green")
	bool bWriteGreen = true;

	UPROPERTY(EditAnywhere, Category = "Channel Mask", DisplayName = "Blue")
	bool bWriteBlue = true;

	UPROPERTY(EditAnywhere, Category = "Channel Mask", DisplayName = "Alpha")
	bool bWriteAlpha = true;

	UPROPERTY(EditAnywhere, Category = "Swap", meta = (ClampMin = "0", ClampMax = "3", EditCondition = "bShowSwapChannels", HideEditConditionToggle, EditConditionHides))
	int32 SwapSourceChannel = 0;

	UPROPERTY(EditAnywhere, Category = "Swap", meta = (ClampMin = "0", ClampMax = "3", EditCondition = "bShowSwapChannels", HideEditConditionToggle, EditConditionHides))
	int32 SwapTargetChannel = 1;

	UPROPERTY(meta = (TransientToolProperty))
	bool bShowColor = true;

	UPROPERTY(meta = (TransientToolProperty))
	bool bShowSwapChannels = false;

	UFUNCTION(CallInEditor, Category = "Whole Volume")
	void Apply();

	uint32 GetChannelMask() const;

	TWeakObjectPtr<UVoxelPaintWholeVolumeTool> OwnerTool;
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintCreateVolumeToolProperties : public UInteractiveToolPropertySet
{
	GENERATED_BODY()
public:
	UPROPERTY(VisibleAnywhere, Category = "Selection Bounds")
	FVector BoundsCenter = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, Category = "Selection Bounds")
	FVector BoundsExtent = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, Category = "Selection Bounds")
	FString Status;

	UFUNCTION(CallInEditor, Category = "Create")
	void CreateVolume();

	TWeakObjectPtr<UVoxelPaintCreateVolumeTool> OwnerTool;
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintSelectToolBuilder : public USingleClickToolBuilder
{
	GENERATED_BODY()
public:
	virtual UInteractiveTool* BuildTool(const FToolBuilderState& SceneState) const override;
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintSelectTool : public USingleClickTool
{
	GENERATED_BODY()
public:
	void SetTargetWorld(UWorld* InWorld) { TargetWorld = InWorld; }

	virtual void Setup() override;
	virtual void Render(IToolsContextRenderAPI* RenderAPI) override;
	virtual FInputRayHit IsHitByClick(const FInputDeviceRay& ClickPos) override;
	virtual void OnClicked(const FInputDeviceRay& ClickPos) override;

private:
	AActor* FindVolumeActorAtClick(const FInputDeviceRay& ClickPos) const;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> VolumeIconTexture;

	TWeakObjectPtr<UWorld> TargetWorld;
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintBrushToolBuilder : public UInteractiveToolBuilder
{
	GENERATED_BODY()
public:
	UPROPERTY()
	bool bEraseMode = false;

	virtual bool CanBuildTool(const FToolBuilderState& SceneState) const override;
	virtual UInteractiveTool* BuildTool(const FToolBuilderState& SceneState) const override;
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintBrushTool : public UBaseBrushTool
{
	GENERATED_BODY()
public:
	UVoxelPaintBrushTool();

	void SetStartInEraseMode(bool bInErase) { bStartInEraseMode = bInErase; }

	virtual void Setup() override;
	virtual void Shutdown(EToolShutdownType ShutdownType) override;
	virtual void OnTick(float DeltaTime) override;
	virtual bool HitTest(const FRay& Ray, FHitResult& OutHit) override;
	virtual void OnBeginDrag(const FRay& Ray) override;
	virtual void OnUpdateDrag(const FRay& Ray) override;
	virtual void OnEndDrag(const FRay& Ray) override;
	virtual void OnCancelDrag() override;
	virtual bool HasCancel() const override { return false; }
	virtual bool HasAccept() const override { return false; }
	virtual bool CanAccept() const override { return false; }

protected:
	virtual double EstimateMaximumTargetDimension() override;

private:
	void ApplyPendingStamp();
	void DenyStrokeOnce(const FText& Message);
	void CloseStrokeWhenDrained();
	FVoxelPaintBrushParams MakeBrushParams() const;

	UPROPERTY(Transient)
	TObjectPtr<UVoxelPaintBrushToolProperties> VoxelProperties;

	FRay PendingStampRay;
	uint32 StampGeneration = 0;
	bool bStampPending = false;
	bool bInDrag = false;
	bool bStrokeDeniedThisDrag = false;
	bool bStartInEraseMode = false;
	bool bCloseStrokePending = false;
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintWholeVolumeToolBuilder : public UInteractiveToolBuilder
{
	GENERATED_BODY()
public:
	EVoxelPaintStrokeOp Op = EVoxelPaintStrokeOp::Flood;

	virtual bool CanBuildTool(const FToolBuilderState& SceneState) const override;
	virtual UInteractiveTool* BuildTool(const FToolBuilderState& SceneState) const override;
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintWholeVolumeTool : public UInteractiveTool
{
	GENERATED_BODY()
public:
	void SetOp(EVoxelPaintStrokeOp InOp) { Op = InOp; }

	virtual void Setup() override;
	virtual void Shutdown(EToolShutdownType ShutdownType) override;
	virtual bool HasCancel() const override { return false; }
	virtual bool HasAccept() const override { return false; }
	virtual bool CanAccept() const override { return false; }

	void ExecuteOp();

private:
	UPROPERTY(Transient)
	TObjectPtr<UVoxelPaintWholeVolumeToolProperties> Properties;

	EVoxelPaintStrokeOp Op = EVoxelPaintStrokeOp::Flood;
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintCreateVolumeToolBuilder : public UInteractiveToolBuilder
{
	GENERATED_BODY()
public:
	virtual bool CanBuildTool(const FToolBuilderState& SceneState) const override;
	virtual UInteractiveTool* BuildTool(const FToolBuilderState& SceneState) const override;
};

UCLASS()
class WORLDVOXELPAINTTOOLSET_API UVoxelPaintCreateVolumeTool : public UInteractiveTool
{
	GENERATED_BODY()
public:
	void SetTargetWorld(UWorld* InWorld) { TargetWorld = InWorld; }

	virtual void Setup() override;
	virtual bool HasCancel() const override { return false; }
	virtual bool HasAccept() const override { return false; }
	virtual bool CanAccept() const override { return false; }

	void CreateVolumeFromSelection();

private:
	FBox ComputeSelectionBounds() const;

	UPROPERTY(Transient)
	TObjectPtr<UVoxelPaintCreateVolumeToolProperties> Properties;

	TWeakObjectPtr<UWorld> TargetWorld;
};
