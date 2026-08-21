// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintPickShaders.h"

#if WITH_EDITOR

#include "DataDrivenShaderPlatformInfo.h"
#include "ShaderPermutation.h"

namespace PickShadersPrivate
{
	static bool ShouldCompilePickPermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5)
			&& EnumHasAllFlags(Parameters.Flags, EShaderPermutationFlags::HasEditorOnlyData);
	}
}

bool FVoxelPaintPickSeedCS::ShouldCompilePermutation(
	const FGlobalShaderPermutationParameters& Parameters)
{
	return PickShadersPrivate::ShouldCompilePickPermutation(Parameters);
}

void FVoxelPaintPickSeedCS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("PICK_THREADGROUP_DIM"), FVoxelPaintPickCS::ThreadGroupDim);
}

bool FVoxelPaintPickCS::ShouldCompilePermutation(
	const FGlobalShaderPermutationParameters& Parameters)
{
	const FPermutationDomain PermutationVector(Parameters.PermutationId);

	if (PermutationVector.Get<FWaveOps>() && !RHISupportsWaveOperations(Parameters.Platform))
	{
		return false;
	}

	return PickShadersPrivate::ShouldCompilePickPermutation(Parameters);
}

void FVoxelPaintPickCS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("PICK_THREADGROUP_DIM"), ThreadGroupDim);

	FPermutationDomain PermutationVector(Parameters.PermutationId);
	if (PermutationVector.Get<FWaveOps>())
	{
		OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
	}
}

IMPLEMENT_GLOBAL_SHADER(FVoxelPaintPickSeedCS,
	"/Plugin/WorldVoxelPaint/VoxelPaintPick.usf", "SeedCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelPaintPickCS,
	"/Plugin/WorldVoxelPaint/VoxelPaintPick.usf", "PickCS", SF_Compute);

#endif
