// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Tools/LegacyEdModeInterfaces.h"
#include "Tools/UEdMode.h"
#include "VoxelPaintEditorModeStyle.h"
#include "VoxelPaintEditorMode.generated.h"

class FEditorViewportClient;
class FViewport;

class FVoxelPaintEditorModeCommands : public TCommands<FVoxelPaintEditorModeCommands>
{
public:
	FVoxelPaintEditorModeCommands()
		: TCommands<FVoxelPaintEditorModeCommands>(
			"VoxelPaintMode",
			NSLOCTEXT("VoxelPaintEditorMode", "VoxelPaintModeCommands", "Voxel Paint Mode"),
			NAME_None,
			FVoxelPaintEditorModeStyle::StyleName)
	{
	}

	virtual void RegisterCommands() override;

	static TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> GetCommands();

	TSharedPtr<FUICommandInfo> SelectTool;
	TSharedPtr<FUICommandInfo> PaintTool;
	TSharedPtr<FUICommandInfo> EraseTool;
	TSharedPtr<FUICommandInfo> FloodTool;
	TSharedPtr<FUICommandInfo> InvertTool;
	TSharedPtr<FUICommandInfo> SwapTool;
	TSharedPtr<FUICommandInfo> CreateVolumeTool;
	TSharedPtr<FUICommandInfo> SizingTool;

	TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> Commands;
};

UCLASS()
class UVoxelPaintMode : public UEdMode, public ILegacyEdModeViewportInterface
{
	GENERATED_BODY()
public:
	static const FName ModeName;
	static const FName VoxelPaintPalette;

	static FString SelectToolName;
	static FString PaintToolName;
	static FString EraseToolName;
	static FString FloodToolName;
	static FString InvertToolName;
	static FString SwapToolName;
	static FString CreateVolumeToolName;
	static FString SizingToolName;

	UVoxelPaintMode();

	virtual void Enter() override;
	virtual void Exit() override;
	virtual void ModeTick(float DeltaTime) override;
	virtual TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> GetModeCommands() const override;

	virtual bool InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport,
		FKey Key, EInputEvent Event) override;

protected:
	virtual void CreateToolkit() override;
	virtual void ActivateDefaultTool() override;
};
