// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "Input/Reply.h"
#include "UObject/WeakObjectPtr.h"
#include "VoxelPaintPrefabAuthoring.h"

class IPropertyHandle;
class IPropertyUtilities;
class SWidget;
class UVoxelPaintVolumeData;
class UVoxelPaintVolumeComponent;
struct FAssetData;

// "Voxel Paint - Pool" section on UVoxelPaintVolumeComponent: Create Paint Data / Add Variant
// pool buttons with venue-aware gray-out and reason text, per-variant rows (asset picker,
// painted state + approximate source size, Duplicate/Remove), the variant pin dropdown with
// Reroll on placed instances, and a Reset to Template fix-up for diverged SCS instances.
class FVoxelPaintVolumeComponentDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	WorldVoxelPaint::FVoxelPaintPoolAuthoringContext GetContext() const;
	void RequestRefresh();
	void NotifyAndRefresh(bool bSucceeded, const FText& Error);

	FText GetPoolStatusText() const;
	FText GetVariantInfoText(int32 SlotIndex) const;
	FText GetPinValueText() const;
	TSharedRef<SWidget> MakePinMenu();
	void SetPin(const UVoxelPaintVolumeData* Asset);
	FReply OnRerollClicked();
	FReply OnResetToTemplateClicked();
	bool ShouldFilterPoolAsset(const FAssetData& AssetData, int32 SlotIndex) const;
	bool IsSlotAssetEditable() const;
	bool IsPoolDivergedFromArchetype() const;

	TWeakObjectPtr<UVoxelPaintVolumeComponent> CustomizedComponent;
	TSharedPtr<IPropertyUtilities> PropertyUtilities;
	TSharedPtr<IPropertyHandle> PinPropertyHandle;
};
