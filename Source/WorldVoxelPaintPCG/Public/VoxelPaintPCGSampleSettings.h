// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "PCGSettings.h"

#include "VoxelPaintPCGSampleSettings.generated.h"

/**
 * "Sample World Voxel Paint" - GPU-only PCG node (M6 plan 6.1). For every point flowing
 * through it, samples the painted voxel field (the same sparse page-table + brick-atlas data
 * the Sample World Voxel Paint material node reads per pixel) and writes the four project
 * channel weights onto the point as float attributes named from the ChannelContexts in
 * Project Settings > World Voxel Paint. Weights are raw/unclamped, identical to the material
 * node; optionally the selected channel is written to Density, saturated. A weight-field
 * sampler, not a color sampler. There is no CPU execution path.
 */
UCLASS(BlueprintType, ClassGroup = (Procedural))
class WORLDVOXELPAINTPCG_API UVoxelPaintPCGSampleSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	UVoxelPaintPCGSampleSettings();

	/**
	 * Resolves the four output attribute names from the project ChannelContexts. Shared by the
	 * kernel (static attribute table + output data description - UNGATED, runs in cooked
	 * builds where the kernel is UPROPERTY-serialized) and the data provider (game-thread
	 * attribute-id resolution). Rule per channel i (letters R,G,B,A):
	 *   DisplayName trimmed; empty or letter-equal (case-insensitive) -> generic
	 *   VoxelChannel<i>; else FPCGMetadataAttributeBase::SanitizeName; a sanitized name equal
	 *   to ANY reserved generic VoxelChannel<k> or duplicating an earlier channel's -> generic
	 *   VoxelChannel<i> (a duplicate would collapse two channels onto one attribute; reserving
	 *   the generics keeps the never-re-checked fallbacks themselves collision-free). The four
	 *   emitted names are unique by construction. Array length is config-driven - out-of-range
	 *   indices take the generic fallback.
	 */
	static void GetChannelAttributeNames(FName OutChannelNames[4]);

	//~Begin UPCGSettings interface
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual bool IsInputPinRequiredByExecution(const UPCGPin* InPin) const override { return true; }
	virtual FString GetAdditionalTitleInformation() const override;
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override;
	virtual FText GetDefaultNodeTitle() const override;
	virtual FText GetNodeTooltipText() const override;
	virtual EPCGSettingsType GetType() const override;
	virtual void CreateKernels(FPCGGPUCompilationContext& InOutContext, UObject* InObjectOuter, TArray<UPCGComputeKernel*>& OutKernels, TArray<FPCGKernelEdge>& OutEdges) const override;
	virtual void GetStaticTrackedKeys(FPCGSelectionKeyToSettingsMap& OutKeysToSettings, TArray<TObjectPtr<const UPCGGraph>>& OutVisitedGraphs) const override;
#endif

protected:
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings interface

public:
#if WITH_EDITOR
	virtual void BeginDestroy() override;
#endif

	/**
	 * Write the selected channel's weight into the point's built-in Density, saturated to
	 * [0,1]. When disabled, an upstream-computed Density passes through untouched. The four
	 * channel attributes are written either way (raw/unclamped - saturate before use as
	 * lerp-style factors).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings)
	bool bWriteDensity = true;

	/**
	 * Channel feeding Density: 0=R, 1=G, 2=B, 3=A of the project's paint channels. The node
	 * title shows the resolved channel name (Project Settings > World Voxel Paint > Channels).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Settings, meta = (ClampMin = "0", ClampMax = "3", EditCondition = "bWriteDensity"))
	int32 DensityChannel = 0;

#if WITH_EDITOR
private:
	/** Core developer-settings change hook: recompiles graphs only when the derived attribute names actually changed. */
	void OnVoxelPaintDeveloperSettingsChanged();

	/** Last names this instance derived; compared by the hook above to filter unrelated settings edits. */
	FName CachedChannelAttributeNames[4];
#endif
};
