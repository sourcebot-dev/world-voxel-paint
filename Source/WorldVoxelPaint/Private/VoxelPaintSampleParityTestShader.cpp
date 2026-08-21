// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintSampleParityTestShader.h"

#if WITH_EDITOR

#include "DataDrivenShaderPlatformInfo.h"
#include "ShaderPermutation.h"

bool FVoxelPaintSampleParityCS::ShouldCompilePermutation(
	const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5)
		&& EnumHasAllFlags(Parameters.Flags, EShaderPermutationFlags::HasEditorOnlyData);
}

void FVoxelPaintSampleParityCS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
}

IMPLEMENT_GLOBAL_SHADER(FVoxelPaintSampleParityCS,
	"/Plugin/WorldVoxelPaint/VoxelPaintSampleParityTest.usf", "MainCS", SF_Compute);

#endif
