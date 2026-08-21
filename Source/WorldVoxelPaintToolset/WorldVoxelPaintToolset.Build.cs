// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WorldVoxelPaintToolset : ModuleRules
{
	public WorldVoxelPaintToolset(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"InteractiveToolsFramework",
				"EditorInteractiveToolsFramework",
				"WorldVoxelPaint"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"RenderCore",
				"Renderer",
				"RHI",
				"UnrealEd",
				"TypedElementFramework",
				"TypedElementRuntime",
				"Slate",
				"SlateCore"
			}
		);
	}
}
