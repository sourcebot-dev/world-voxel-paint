// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "Layout/Visibility.h"
#include "Styling/SlateColor.h"
#include "Toolkits/BaseToolkit.h"
#include "UObject/WeakObjectPtr.h"

class UVoxelPaintVolumeComponent;
class UVoxelPaintVolumeData;

class FVoxelPaintForcePreviewPolicy
{
public:
	static bool CanForceVariant(const UVoxelPaintVolumeData* Asset, FText& OutWhyNot);
	static bool ApplyForcedVariant(UVoxelPaintVolumeComponent* Placement,
		const UVoxelPaintVolumeData* Asset, bool bAllCopies, FText& OutWhyNot);
	static void ComputeAllCopiesDomain(UVoxelPaintVolumeComponent* Placement,
		TArray<UVoxelPaintVolumeComponent*>& OutDomain);
};

class FVoxelPaintModeToolkit : public FModeToolkit
{
public:
	virtual void Init(const TSharedPtr<IToolkitHost>& InitToolkitHost,
		TWeakObjectPtr<UEdMode> InOwningMode) override;

	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual void GetToolPaletteNames(TArray<FName>& PaletteNames) const override;
	virtual FText GetToolPaletteDisplayName(FName Palette) const override;
	virtual TSharedPtr<SWidget> GetInlineContent() const override;

private:
	TSharedRef<SWidget> BuildSessionPanel() const;

	FText GetAssetValueText() const;
	FSlateColor GetAssetValueColor() const;
	FText GetAssetToolTipText() const;
	FText GetSharingValueText() const;
	FText GetOwnerValueText() const;
	FSlateColor GetOwnerValueColor() const;
	FText GetOwnerToolTipText() const;
	EVisibility GetOwnerRowVisibility() const;
	FText GetLinkedPlacementsValueText() const;
	EVisibility GetAssetDetailVisibility() const;

	FText GetDanglingPinText() const;
	EVisibility GetDanglingPinVisibility() const;
	EVisibility GetScaleNoteVisibility() const;

	TSharedRef<SWidget> MakeForcePreviewMenu() const;
	FText GetForcePreviewValueText() const;
	FText GetForcePreviewToolTipText() const;
	bool CanUseForcePreview() const;
	void OnForcePreviewPicked(TWeakObjectPtr<const UVoxelPaintVolumeData> Variant) const;
	const UVoxelPaintVolumeData* GetForcedVariantForSelected() const;

	bool CanFinish() const;
	bool CanCancelSealing() const;
	bool CanDiscard() const;
	bool CanRetryOrEndAnyway() const;

	FReply OnFinishClicked() const;
	FReply OnCancelSealingClicked() const;
	FReply OnDiscardClicked() const;
	FReply OnRetryClicked() const;
	FReply OnEndAnywayClicked() const;

	FReply OnTakeOwnershipClicked() const;
	FReply OnMakeUniqueClicked() const;
	FReply OnMarkSharedClicked() const;
	FReply OnMarkPrivateClicked() const;

	bool CanTakeOwnership() const;
	bool CanMakeUnique() const;
	bool CanMarkShared() const;
	bool CanMarkPrivate() const;

	EVisibility GetTakeOwnershipVisibility() const;
	EVisibility GetMarkSharedVisibility() const;
	EVisibility GetMarkPrivateVisibility() const;

	bool PassesAuthorityGate(const UVoxelPaintVolumeData* Asset, FText& OutWhyNot) const;
	bool EnsureAuthorityGatePasses(const UVoxelPaintVolumeData* Asset) const;
	FText GetAuthorityGateToolTipText(bool bProvenanceGated) const;
	static bool IsPrefabProvenance(const UVoxelPaintVolumeComponent* Placement);
	static UVoxelPaintVolumeComponent* GetSelectedPlacement();
	static UVoxelPaintVolumeData* GetSelectedAsset();

	mutable double LinkedPlacementsCacheTime = -1.0;
	mutable int32 LinkedPlacementsCountCache = 0;
	mutable FString LinkedPlacementsNamesCache;
	mutable bool bForcePreviewAllCopies = false;
};
