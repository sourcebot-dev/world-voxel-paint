// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneUniformBuffer.h"
#include "ShaderParameterMacros.h"

class FRDGBuilder;

BEGIN_SHADER_PARAMETER_STRUCT(FVoxelPaintSceneParameters, WORLDVOXELPAINT_API)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Volumes)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GridHeaders)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GridIndexList)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, PageTables)
	SHADER_PARAMETER_TEXTURE(Texture3D, BrickPool)
	SHADER_PARAMETER_TEXTURE(Texture3D, SessionTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, PoolSampler)
	SHADER_PARAMETER(FVector3f, EvaluationOriginHigh)
	SHADER_PARAMETER(float, EvaluationRadiusSq)
	SHADER_PARAMETER(FVector3f, EvaluationOriginLow)
	SHADER_PARAMETER(uint32, NumVolumes)
	SHADER_PARAMETER(FVector3f, GridOrigin)
	SHADER_PARAMETER(uint32, EnableFlag)
	SHADER_PARAMETER(FVector3f, CameraCenter)
	SHADER_PARAMETER(uint32, BrickSize)
	SHADER_PARAMETER(FVector3f, PoolTexelExtentRcp)
	SHADER_PARAMETER(uint32, UniqueBrickSize)
	SHADER_PARAMETER(FIntVector, GridDims)
	SHADER_PARAMETER(FVector2f, GridCellSize)
	SHADER_PARAMETER(FIntVector, PoolDimsInBricks)
END_SHADER_PARAMETER_STRUCT()

DECLARE_SCENE_UB_STRUCT(FVoxelPaintSceneParameters, VoxelPaint, WORLDVOXELPAINT_API)

namespace WorldVoxelPaint
{
	WORLDVOXELPAINT_API void BuildNeutralSceneParameters(
		FVoxelPaintSceneParameters& OutParameters, FRDGBuilder& GraphBuilder);
}
