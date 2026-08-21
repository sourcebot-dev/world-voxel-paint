// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintSizingDetails.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "VoxelPaintSizingTool.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "VoxelPaintEditorMode"

TSharedRef<IDetailCustomization> FVoxelPaintSizingDetails::MakeInstance()
{
	return MakeShareable(new FVoxelPaintSizingDetails);
}

void FVoxelPaintSizingDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	TWeakObjectPtr<UVoxelPaintSizingToolProperties> Properties;
	for (const TWeakObjectPtr<UObject>& Object : Objects)
	{
		if (UVoxelPaintSizingToolProperties* Candidate = Cast<UVoxelPaintSizingToolProperties>(Object.Get()))
		{
			Properties = Candidate;
			break;
		}
	}
	if (!Properties.IsValid())
	{
		return;
	}

	// Categories read top-down in dependency order: identity, inputs, derived, cost, status.
	DetailBuilder.EditCategory("Asset", FText::GetEmpty(), ECategoryPriority::Important);
	IDetailCategoryBuilder& InputCategory = DetailBuilder.EditCategory("Size and Detail",
		FText::GetEmpty(), ECategoryPriority::Important);
	DetailBuilder.EditCategory("Derived",
		LOCTEXT("DerivedCategoryDisplay", "Derived (Read-Only)"), ECategoryPriority::Important);
	DetailBuilder.EditCategory("Cost Preview", FText::GetEmpty(), ECategoryPriority::Important);
	IDetailCategoryBuilder& StatusCategory = DetailBuilder.EditCategory("Status",
		FText::GetEmpty(), ECategoryPriority::Important);

	// Inputs in edit order, with Snap directly under the Desired Texel Size it consumes.
	// Enablement is a live attribute instead of an EditCondition: the tool flips
	// bAssetInitialized directly in C++ (Reset, undo), and FPropertyNode caches metadata
	// edit-condition results until the panel is rebuilt, leaving the rows stale.
	const TAttribute<bool> InputsEnabled = TAttribute<bool>::CreateLambda([Properties]()
	{
		return Properties.IsValid() && !Properties->bAssetInitialized;
	});
	InputCategory.AddProperty(DetailBuilder.GetProperty(
		GET_MEMBER_NAME_CHECKED(UVoxelPaintSizingToolProperties, HalfExtent)))
		.IsEnabled(InputsEnabled);
	InputCategory.AddProperty(DetailBuilder.GetProperty(
		GET_MEMBER_NAME_CHECKED(UVoxelPaintSizingToolProperties, DetailRungs)))
		.IsEnabled(InputsEnabled);
	InputCategory.AddProperty(DetailBuilder.GetProperty(
		GET_MEMBER_NAME_CHECKED(UVoxelPaintSizingToolProperties, DesiredTexelSize)))
		.IsEnabled(InputsEnabled);
	InputCategory.AddCustomRow(LOCTEXT("SnapRowFilter", "Snap to Desired Texel Size"))
	.WholeRowContent()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 2.0f, 0.0f, 4.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("SnapButton", "Snap to Desired Texel Size"))
			.ToolTipText(LOCTEXT("SnapButtonToolTip",
				"Pick, per axis, the detail rung whose resolved texel size is closest to Desired Texel Size."))
			.IsEnabled_Lambda([Properties]()
			{
				return Properties.IsValid() && !Properties->bAssetInitialized
					&& Properties->StatusLevel != EVoxelPaintSizingStatusLevel::NoAsset;
			})
			.OnClicked_Lambda([Properties]()
			{
				if (Properties.IsValid())
				{
					Properties->SnapToDesiredTexelSize();
				}
				return FReply::Handled();
			})
		]
	];

	// Status renders as a wrapped, color-coded whole row instead of a truncated text box.
	DetailBuilder.HideProperty(DetailBuilder.GetProperty(
		GET_MEMBER_NAME_CHECKED(UVoxelPaintSizingToolProperties, Status)));
	StatusCategory.AddCustomRow(LOCTEXT("StatusRowFilter", "Status"))
	.WholeRowContent()
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.Margin(FMargin(2.0f, 4.0f))
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.Text_Lambda([Properties]()
		{
			return Properties.IsValid() ? FText::FromString(Properties->Status) : FText::GetEmpty();
		})
		.ColorAndOpacity_Lambda([Properties]() -> FSlateColor
		{
			if (!Properties.IsValid())
			{
				return FSlateColor::UseForeground();
			}
			switch (Properties->StatusLevel)
			{
			case EVoxelPaintSizingStatusLevel::Editable:
				return FStyleColors::AccentGreen;
			case EVoxelPaintSizingStatusLevel::Error:
				return FStyleColors::Error;
			case EVoxelPaintSizingStatusLevel::NoAsset:
				return FStyleColors::Warning;
			case EVoxelPaintSizingStatusLevel::Frozen:
			default:
				return FSlateColor::UseForeground();
			}
		})
	];

	// The destructive unlock sits right under the status text that references it.
	StatusCategory.AddCustomRow(LOCTEXT("ResetRowFilter", "Reset Data and Unlock Size"))
	.WholeRowContent()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 2.0f, 0.0f, 4.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("ResetButton", "Reset Data and Unlock Size"))
			.ToolTipText_Lambda([Properties]()
			{
				if (Properties.IsValid() && !Properties->bAssetInitialized)
				{
					return LOCTEXT("ResetButtonDisabledToolTip",
						"The asset is not initialized; size and detail are already editable.");
				}
				return LOCTEXT("ResetButtonToolTip",
					"Destructively clear all painted data and unlock Size and Detail. Asks for confirmation first.");
			})
			.IsEnabled_Lambda([Properties]()
			{
				return Properties.IsValid() && Properties->bAssetInitialized;
			})
			.OnClicked_Lambda([Properties]()
			{
				if (Properties.IsValid())
				{
					Properties->ResetDataAndUnlockSize();
				}
				return FReply::Handled();
			})
		]
	];
}

#undef LOCTEXT_NAMESPACE
