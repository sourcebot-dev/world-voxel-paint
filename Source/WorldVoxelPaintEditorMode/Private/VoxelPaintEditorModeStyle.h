// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FSlateStyleSet;

// Slate style set for the Voxel Paint editor mode. Registers "VoxelPaintMode.<CommandName>"
// brushes so the mode's tool palette picks up icons automatically (the FUICommandInfo icon is
// derived from the TCommands context name plus the command member name), reusing the engine
// icons that Mesh Paint, Landscape, and Foliage use for their equivalent tools.
class FVoxelPaintEditorModeStyle
{
public:
	static void Initialize();
	static void Shutdown();

	static const FName StyleName;

private:
	static TSharedPtr<FSlateStyleSet> StyleSet;
};
