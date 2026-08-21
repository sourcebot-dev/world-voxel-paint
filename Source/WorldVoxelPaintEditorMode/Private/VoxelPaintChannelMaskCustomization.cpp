// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintChannelMaskCustomization.h"

#include "DetailLayoutBuilder.h"
#include "PropertyHandle.h"
#include "VoxelPaintDeveloperSettings.h"
#include "VoxelPaintTools.h"
#include "VoxelPaintTypes.h"

#define LOCTEXT_NAMESPACE "VoxelPaintEditorMode"

namespace WorldVoxelPaint
{
	FVoxelPaintChannelMaskUiText ResolveChannelMaskUiText(int32 ChannelIndex,
		TConstArrayView<FVoxelPaintChannelContext> ChannelContexts)
	{
		static const TCHAR* Letters[] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };
		static const TCHAR* BaseNames[] = { TEXT("Red"), TEXT("Green"), TEXT("Blue"), TEXT("Alpha") };

		FVoxelPaintChannelMaskUiText Result;
		if (ChannelIndex < 0 || ChannelIndex >= (int32)UE_ARRAY_COUNT(Letters))
		{
			return Result;
		}

		const FString Letter = Letters[ChannelIndex];
		const FString BaseName = BaseNames[ChannelIndex];
		const FVoxelPaintChannelContext* Context = ChannelContexts.IsValidIndex(ChannelIndex)
			? &ChannelContexts[ChannelIndex] : nullptr;
		const FString ProjectName = Context ? Context->DisplayName.TrimStartAndEnd() : FString();
		const bool bHasProjectName = !ProjectName.IsEmpty()
			&& !ProjectName.Equals(Letter, ESearchCase::IgnoreCase)
			&& !ProjectName.Equals(BaseName, ESearchCase::IgnoreCase);

		FText Label = bHasProjectName
			? FText::Format(LOCTEXT("ChannelLabelNamedFormat", "{0} ({1})"),
				FText::FromString(ProjectName), FText::FromString(Letter))
			: FText::FromString(BaseName);
		if (Context && !Context->bInUse)
		{
			Label = FText::Format(LOCTEXT("ChannelLabelNotInUseFormat", "{0} - not in use"), Label);
		}
		Result.Label = Label;

		if (!Context)
		{
			Result.ToolTip = FText::Format(LOCTEXT("ChannelToolTipNoContextFormat",
				"No channel context is declared for the {0} channel in Project Settings > Plugins > "
				"World Voxel Paint. Channel semantics are a project convention only; the channel "
				"stays writable."),
				FText::FromString(Letter));
			return Result;
		}

		Result.ToolTip = FText::Format(LOCTEXT("ChannelToolTipFormat",
			"{0} channel{1}: {2} in this project. Default value: {3}. Channel semantics are a "
			"project convention declared in Project Settings > Plugins > World Voxel Paint, not "
			"enforcement; a channel not in use stays writable."),
			FText::FromString(Letter),
			bHasProjectName
				? FText::Format(LOCTEXT("ChannelToolTipNameFormat", " '{0}'"),
					FText::FromString(ProjectName))
				: FText::GetEmpty(),
			Context->bInUse ? LOCTEXT("ChannelInUse", "in use") : LOCTEXT("ChannelNotInUse", "not in use"),
			FText::AsNumber(Context->DefaultValue));
		return Result;
	}
}

TSharedRef<IDetailCustomization> FVoxelPaintChannelMaskCustomization::MakeInstance()
{
	return MakeShareable(new FVoxelPaintChannelMaskCustomization);
}

void FVoxelPaintChannelMaskCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	static const FName ChannelPropertyNames[] = {
		GET_MEMBER_NAME_CHECKED(UVoxelPaintBrushToolProperties, bWriteRed),
		GET_MEMBER_NAME_CHECKED(UVoxelPaintBrushToolProperties, bWriteGreen),
		GET_MEMBER_NAME_CHECKED(UVoxelPaintBrushToolProperties, bWriteBlue),
		GET_MEMBER_NAME_CHECKED(UVoxelPaintBrushToolProperties, bWriteAlpha)
	};

	const UVoxelPaintDeveloperSettings* Settings = GetDefault<UVoxelPaintDeveloperSettings>();
	for (int32 ChannelIndex = 0; ChannelIndex < (int32)UE_ARRAY_COUNT(ChannelPropertyNames); ++ChannelIndex)
	{
		const TSharedRef<IPropertyHandle> Handle =
			DetailBuilder.GetProperty(ChannelPropertyNames[ChannelIndex]);
		if (!Handle->IsValidHandle())
		{
			continue;
		}
		const WorldVoxelPaint::FVoxelPaintChannelMaskUiText UiText =
			WorldVoxelPaint::ResolveChannelMaskUiText(ChannelIndex, Settings->ChannelContexts);
		Handle->SetPropertyDisplayName(UiText.Label);
		Handle->SetToolTipText(UiText.ToolTip);
	}
}

#undef LOCTEXT_NAMESPACE
