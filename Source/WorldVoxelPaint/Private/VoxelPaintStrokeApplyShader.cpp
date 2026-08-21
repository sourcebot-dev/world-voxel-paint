// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintStrokeApplyShader.h"

#if WITH_EDITOR

#include "DataDrivenShaderPlatformInfo.h"
#include "ShaderPermutation.h"

bool FVoxelPaintStrokeApplyCS::ShouldCompilePermutation(
	const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5)
		&& EnumHasAllFlags(Parameters.Flags, EShaderPermutationFlags::HasEditorOnlyData);
}

void FVoxelPaintStrokeApplyCS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
}

IMPLEMENT_GLOBAL_SHADER(FVoxelPaintStrokeApplyCS,
	"/Plugin/WorldVoxelPaint/VoxelPaintStrokeApply.usf", "StrokeApplyCS", SF_Compute);

#endif
