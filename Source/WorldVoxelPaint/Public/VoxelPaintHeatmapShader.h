// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"

class FVoxelPaintHeatmapPS : public FGlobalShader
{
public:
	DECLARE_EXPORTED_GLOBAL_SHADER(FVoxelPaintHeatmapPS, WORLDVOXELPAINT_API);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPaintHeatmapPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, SceneDepthTexture)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GridHeaders)
		SHADER_PARAMETER(FIntVector, GridDims)
		SHADER_PARAMETER(FVector3f, GridOrigin)
		SHADER_PARAMETER(FVector2f, GridCellSize)
		SHADER_PARAMETER(FVector3f, TranslatedWorldToRelative)
		SHADER_PARAMETER(uint32, HeatmapMode)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
};
