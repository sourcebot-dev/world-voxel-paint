// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// This editor-only shader type must live in this PostConfigInit module:
// FShaderTypeRegistration asserts on any type registered after CommitAll
// (LaunchEngineLoop.cpp:3094), which runs before Default-phase editor modules load.

class FVoxelPaintStrokeApplyCS : public FGlobalShader
{
public:
	DECLARE_EXPORTED_GLOBAL_SHADER(FVoxelPaintStrokeApplyCS, WORLDVOXELPAINT_API);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPaintStrokeApplyCS, FGlobalShader);

	static constexpr int32 ThreadGroupSize = 64;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, WorkList)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, RWWorking)
		SHADER_PARAMETER(uint32, Op)
		SHADER_PARAMETER(uint32, WorkCount)
		SHADER_PARAMETER(FVector3f, BrushCenterTexel)
		SHADER_PARAMETER(float, BrushRadiusTexels)
		SHADER_PARAMETER(float, Strength)
		SHADER_PARAMETER(float, FalloffHotspot)
		SHADER_PARAMETER(FLinearColor, Color)
		SHADER_PARAMETER(uint32, ChannelMask)
		SHADER_PARAMETER(uint32, SwapChannels)
		SHADER_PARAMETER(FIntVector, Resolution)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment);
};

#endif
