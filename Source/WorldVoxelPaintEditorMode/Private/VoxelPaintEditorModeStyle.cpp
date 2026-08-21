// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintEditorModeStyle.h"

#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleMacros.h"
#include "Styling/SlateStyleRegistry.h"

#define RootToContentDir StyleSet->RootToContentDir

const FName FVoxelPaintEditorModeStyle::StyleName("VoxelPaintStyle");

TSharedPtr<FSlateStyleSet> FVoxelPaintEditorModeStyle::StyleSet;

void FVoxelPaintEditorModeStyle::Initialize()
{
	if (StyleSet.IsValid())
	{
		return;
	}

	const FVector2D Icon16x16(16.0f, 16.0f);
	const FVector2D Icon20x20(20.0f, 20.0f);

	StyleSet = MakeShareable(new FSlateStyleSet(StyleName));
	StyleSet->SetParentStyleName(FAppStyle::GetAppStyleSetName());
	// Root at the engine's editor slate content so the palette reuses the exact icons the
	// built-in paint tools use (Mesh Paint, Landscape, Foliage) without shipping copies.
	StyleSet->SetContentRoot(FPaths::EngineContentDir() / TEXT("Editor/Slate"));
	StyleSet->SetCoreContentRoot(FPaths::EngineContentDir() / TEXT("Slate"));

	StyleSet->Set("VoxelPaintMode.SelectTool", new IMAGE_BRUSH("Icons/GeneralTools/Select_40x", Icon20x20));
	StyleSet->Set("VoxelPaintMode.SelectTool.Small", new IMAGE_BRUSH("Icons/GeneralTools/Select_40x", Icon20x20));
	StyleSet->Set("VoxelPaintMode.PaintTool", new IMAGE_BRUSH("Icons/GeneralTools/Paint_40x", Icon20x20));
	StyleSet->Set("VoxelPaintMode.PaintTool.Small", new IMAGE_BRUSH("Icons/GeneralTools/Paint_40x", Icon20x20));
	StyleSet->Set("VoxelPaintMode.EraseTool", new IMAGE_BRUSH("Icons/GeneralTools/Erase_40x", Icon20x20));
	StyleSet->Set("VoxelPaintMode.EraseTool.Small", new IMAGE_BRUSH("Icons/GeneralTools/Erase_40x", Icon20x20));
	StyleSet->Set("VoxelPaintMode.FloodTool", new IMAGE_BRUSH("Icons/GeneralTools/PaintBucket_40x", Icon20x20));
	StyleSet->Set("VoxelPaintMode.FloodTool.Small", new IMAGE_BRUSH("Icons/GeneralTools/PaintBucket_40x", Icon20x20));
	StyleSet->Set("VoxelPaintMode.InvertTool", new IMAGE_BRUSH_SVG("Starship/Common/SelectInvert_16", Icon20x20));
	StyleSet->Set("VoxelPaintMode.InvertTool.Small", new IMAGE_BRUSH_SVG("Starship/Common/SelectInvert_16", Icon20x20));
	StyleSet->Set("VoxelPaintMode.SwapTool", new IMAGE_BRUSH("Icons/Paint/Paint_SwapColors_40x", Icon20x20));
	StyleSet->Set("VoxelPaintMode.SwapTool.Small", new IMAGE_BRUSH("Icons/Paint/Paint_SwapColors_40x", Icon20x20));
	StyleSet->Set("VoxelPaintMode.CreateVolumeTool", new IMAGE_BRUSH_SVG("Starship/Common/Volume", Icon20x20));
	StyleSet->Set("VoxelPaintMode.CreateVolumeTool.Small", new IMAGE_BRUSH_SVG("Starship/Common/Volume", Icon20x20));
	StyleSet->Set("VoxelPaintMode.SizingTool", new IMAGE_BRUSH("Icons/Landscape/Landscape_Resize_x40", Icon20x20));
	StyleSet->Set("VoxelPaintMode.SizingTool.Small", new IMAGE_BRUSH("Icons/Landscape/Landscape_Resize_x40", Icon20x20));

	StyleSet->Set("VoxelPaintMode.ModeIcon", new IMAGE_BRUSH_SVG("Starship/Common/Paintbrush", Icon20x20));
	StyleSet->Set("VoxelPaintMode.ModeIcon.Small", new IMAGE_BRUSH_SVG("Starship/Common/Paintbrush", Icon16x16));

	FSlateStyleRegistry::RegisterSlateStyle(*StyleSet.Get());
}

void FVoxelPaintEditorModeStyle::Shutdown()
{
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet.Get());
		ensure(StyleSet.IsUnique());
		StyleSet.Reset();
	}
}

#undef RootToContentDir
