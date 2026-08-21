// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintSessionTransferShaders.h"

#if WITH_EDITOR

#include "DataDrivenShaderPlatformInfo.h"
#include "ShaderPermutation.h"

namespace SessionTransferShadersPrivate
{
	static bool ShouldCompileSessionTransferShader(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5)
			&& EnumHasAllFlags(Parameters.Flags, EShaderPermutationFlags::HasEditorOnlyData);
	}

	static void ModifySessionTransferShaderEnvironment(FShaderCompilerEnvironment& OutEnvironment,
		int32 ThreadGroupSize)
	{
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), ThreadGroupSize);
	}
}

bool FVoxelPaintSessionScatterCS::ShouldCompilePermutation(
	const FGlobalShaderPermutationParameters& Parameters)
{
	return SessionTransferShadersPrivate::ShouldCompileSessionTransferShader(Parameters);
}

void FVoxelPaintSessionScatterCS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	SessionTransferShadersPrivate::ModifySessionTransferShaderEnvironment(OutEnvironment, ThreadGroupSize);
}

bool FVoxelPaintSessionGatherCS::ShouldCompilePermutation(
	const FGlobalShaderPermutationParameters& Parameters)
{
	return SessionTransferShadersPrivate::ShouldCompileSessionTransferShader(Parameters);
}

void FVoxelPaintSessionGatherCS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	SessionTransferShadersPrivate::ModifySessionTransferShaderEnvironment(OutEnvironment, ThreadGroupSize);
}

IMPLEMENT_GLOBAL_SHADER(FVoxelPaintSessionScatterCS,
	"/Plugin/WorldVoxelPaint/VoxelPaintSessionTransfer.usf", "ScatterCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelPaintSessionGatherCS,
	"/Plugin/WorldVoxelPaint/VoxelPaintSessionTransfer.usf", "GatherCS", SF_Compute);

#endif
