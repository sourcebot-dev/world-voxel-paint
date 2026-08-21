// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// These editor-only shader types must live in this PostConfigInit module:
// FShaderTypeRegistration asserts on any type registered after CommitAll
// (LaunchEngineLoop.cpp:3094), which runs before Default-phase editor modules load.

class FVoxelPaintSessionScatterCS : public FGlobalShader
{
public:
	DECLARE_EXPORTED_GLOBAL_SHADER(FVoxelPaintSessionScatterCS, WORLDVOXELPAINT_API);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPaintSessionScatterCS, FGlobalShader);

	static constexpr int32 ThreadGroupSize = 64;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BrickIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SourceTexels)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWWorkingTexture)
		SHADER_PARAMETER(FIntVector, BrickCounts)
		SHADER_PARAMETER(uint32, NumBricks)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment);
};

class FVoxelPaintSessionGatherCS : public FGlobalShader
{
public:
	DECLARE_EXPORTED_GLOBAL_SHADER(FVoxelPaintSessionGatherCS, WORLDVOXELPAINT_API);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPaintSessionGatherCS, FGlobalShader);

	static constexpr int32 ThreadGroupSize = 64;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BrickIndices)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, WorkingTexture)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWGatherTexels)
		SHADER_PARAMETER(FIntVector, BrickCounts)
		SHADER_PARAMETER(uint32, NumBricks)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment);
};

#endif
