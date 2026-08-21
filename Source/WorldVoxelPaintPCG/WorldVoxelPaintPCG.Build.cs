// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WorldVoxelPaintPCG : ModuleRules
{
	public WorldVoxelPaintPCG(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"PCG"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"ComputeFramework",
				"Projects",
				"RenderCore",
				"RHI",
				"WorldVoxelPaint"
			}
		);
	}
}
