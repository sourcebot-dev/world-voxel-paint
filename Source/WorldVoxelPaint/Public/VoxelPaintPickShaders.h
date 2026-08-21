// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "GlobalShader.h"
#include "SceneTexturesConfig.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"

// These editor-only shader types must live in this PostConfigInit module:
// FShaderTypeRegistration asserts on any type registered after CommitAll
// (LaunchEngineLoop.cpp:3094), which runs before Default-phase editor modules load.

class FVoxelPaintPickSeedCS : public FGlobalShader
{
public:
	DECLARE_EXPORTED_GLOBAL_SHADER(FVoxelPaintPickSeedCS, WORLDVOXELPAINT_API);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPaintPickSeedCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RWSeedParams)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWOutput)
		SHADER_PARAMETER(FVector2f, CursorNdc)
		SHADER_PARAMETER(FMatrix44f, TranslatedWorldToTexel)
		SHADER_PARAMETER(uint32, IndexCapacity)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment);
};

class FVoxelPaintPickCS : public FGlobalShader
{
public:
	DECLARE_EXPORTED_GLOBAL_SHADER(FVoxelPaintPickCS, WORLDVOXELPAINT_API);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPaintPickCS, FGlobalShader);

	static constexpr int32 ThreadGroupDim = 8;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, SeedParams)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWOutput)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWDedupeMask)
		SHADER_PARAMETER(FMatrix44f, TranslatedWorldToTexel)
		SHADER_PARAMETER(FIntVector, Resolution)
		SHADER_PARAMETER(uint32, IndexCapacity)
		SHADER_PARAMETER(float, BrushRadiusWorld)
		SHADER_PARAMETER(float, FlattenPct)
		SHADER_PARAMETER(float, MinFlattenDepthWorld)
		SHADER_PARAMETER(float, AngleToleranceDeg)
	END_SHADER_PARAMETER_STRUCT()

	class FWaveOps : SHADER_PERMUTATION_BOOL("WAVE_OPS");
	using FPermutationDomain = TShaderPermutationDomain<FWaveOps>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment);
};

#endif
