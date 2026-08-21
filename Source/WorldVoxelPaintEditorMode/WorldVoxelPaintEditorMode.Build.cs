// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WorldVoxelPaintEditorMode : ModuleRules
{
	public WorldVoxelPaintEditorMode(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
				"EditorSubsystem"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"EditorFramework",
				"EditorWidgets",
				"UnrealEd",
				"InteractiveToolsFramework",
				"EditorInteractiveToolsFramework",
				"WorldVoxelPaintToolset",
				"WorldVoxelPaint",
				"PropertyEditor",
				"MainFrame",
				"LevelEditor",
				"PlacementMode",
				"ToolMenus",
				"ToolWidgets",
				"WorkspaceMenuStructure",
				"Projects",
				"RenderCore",
				"RHI",
				"DataValidation",
				"AssetRegistry",
				"PCG",
				"WorldVoxelPaintPCG"
			}
		);
	}
}
