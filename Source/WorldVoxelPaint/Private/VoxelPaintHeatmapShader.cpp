// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintHeatmapShader.h"

#include "DataDrivenShaderPlatformInfo.h"

bool FVoxelPaintHeatmapPS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5)
		&& IsPCPlatform(Parameters.Platform);
}

IMPLEMENT_GLOBAL_SHADER(FVoxelPaintHeatmapPS,
	"/Plugin/WorldVoxelPaint/VoxelPaintHeatmap.usf", "MainPS", SF_Pixel);
