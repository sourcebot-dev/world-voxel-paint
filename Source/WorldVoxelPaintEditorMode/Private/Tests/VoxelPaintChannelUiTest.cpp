// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintChannelMaskCustomization.h"
#include "VoxelPaintTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintChannelMask_UiText,
	"Game.WorldVoxelPaint.Unit.ChannelMask.UiText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintChannelMask_UiText::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	TArray<FVoxelPaintChannelContext> Contexts;
	Contexts.SetNum(4);
	Contexts[0].bInUse = true;
	Contexts[0].DisplayName = TEXT("Wetness");
	Contexts[0].DefaultValue = 0.25f;
	Contexts[1].bInUse = false;
	Contexts[1].DisplayName = TEXT("Moss");
	Contexts[1].DefaultValue = 1.0f;
	Contexts[2].bInUse = true;
	Contexts[2].DisplayName = TEXT("   ");
	Contexts[3].bInUse = false;
	Contexts[3].DisplayName = TEXT("A");

	{
		const FVoxelPaintChannelMaskUiText UiText = ResolveChannelMaskUiText(0, Contexts);
		TestEqual(TEXT("In-use named channel labels as name (letter)"),
			UiText.Label.ToString(), FString(TEXT("Wetness (R)")));
		TestTrue(TEXT("In-use tooltip states in use"),
			UiText.ToolTip.ToString().Contains(TEXT(": in use in this project")));
		TestFalse(TEXT("In-use tooltip does not state not in use"),
			UiText.ToolTip.ToString().Contains(TEXT(": not in use in this project")));
		TestTrue(TEXT("Tooltip carries the default value"),
			UiText.ToolTip.ToString().Contains(TEXT("0.25")));
		TestTrue(TEXT("Tooltip names the settings location"),
			UiText.ToolTip.ToString().Contains(TEXT("World Voxel Paint")));
	}

	{
		const FVoxelPaintChannelMaskUiText UiText = ResolveChannelMaskUiText(1, Contexts);
		TestEqual(TEXT("Not-in-use named channel labels with the suffix"),
			UiText.Label.ToString(), FString(TEXT("Moss (G) - not in use")));
		TestTrue(TEXT("Not-in-use tooltip states not in use"),
			UiText.ToolTip.ToString().Contains(TEXT(": not in use in this project")));
		TestTrue(TEXT("Not-in-use tooltip keeps the writable note"),
			UiText.ToolTip.ToString().Contains(TEXT("stays writable")));
	}

	{
		const FVoxelPaintChannelMaskUiText UiText = ResolveChannelMaskUiText(2, Contexts);
		TestEqual(TEXT("Whitespace-only display name falls back to the base label"),
			UiText.Label.ToString(), FString(TEXT("Blue")));
	}

	{
		const FVoxelPaintChannelMaskUiText UiText = ResolveChannelMaskUiText(3, Contexts);
		TestEqual(TEXT("Letter-equal display name falls back, keeping the in-use suffix"),
			UiText.Label.ToString(), FString(TEXT("Alpha - not in use")));
	}

	{
		Contexts[0].DisplayName = TEXT("red");
		const FVoxelPaintChannelMaskUiText UiText = ResolveChannelMaskUiText(0, Contexts);
		TestEqual(TEXT("Base-name-equal display name (any case) falls back"),
			UiText.Label.ToString(), FString(TEXT("Red")));
	}

	{
		TArray<FVoxelPaintChannelContext> Short;
		Short.SetNum(2);
		const FVoxelPaintChannelMaskUiText UiText = ResolveChannelMaskUiText(3, Short);
		TestEqual(TEXT("Missing entry labels with the plain base name"),
			UiText.Label.ToString(), FString(TEXT("Alpha")));
		TestTrue(TEXT("Missing entry tooltip states no context is declared"),
			UiText.ToolTip.ToString().Contains(TEXT("No channel context is declared")));
	}

	{
		const FVoxelPaintChannelMaskUiText UiText = ResolveChannelMaskUiText(4, Contexts);
		TestTrue(TEXT("Out-of-range channel index resolves to empty text"),
			UiText.Label.IsEmpty() && UiText.ToolTip.IsEmpty());
	}

	return true;
}
