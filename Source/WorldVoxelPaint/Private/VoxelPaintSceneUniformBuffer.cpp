// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintSceneUniformBuffer.h"

#include "GlobalRenderResources.h"
#include "RHIStaticStates.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SystemTextures.h"
#include "VoxelPaintExternalSampling.h"
#include "WorldVoxelPaintConstants.h"

static void GetDefaultVoxelPaintParameters(FVoxelPaintSceneParameters& OutParameters, FRDGBuilder& GraphBuilder)
{
	WorldVoxelPaint::BuildNeutralSceneParameters(OutParameters, GraphBuilder);
}

IMPLEMENT_SCENE_UB_STRUCT(FVoxelPaintSceneParameters, VoxelPaint, GetDefaultVoxelPaintParameters);

namespace WorldVoxelPaint
{

void BuildNeutralSceneParameters(FVoxelPaintSceneParameters& OutParameters, FRDGBuilder& GraphBuilder)
{
	OutParameters.Volumes = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer<FVector4f>(GraphBuilder));
	OutParameters.GridHeaders = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer<uint32>(GraphBuilder));
	OutParameters.GridIndexList = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer<uint32>(GraphBuilder));
	OutParameters.PageTables = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer<uint32>(GraphBuilder));
	OutParameters.BrickPool = GBlackVolumeTexture->TextureRHI;
	OutParameters.SessionTexture = GBlackVolumeTexture->TextureRHI;
	OutParameters.PoolSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	OutParameters.EvaluationOriginHigh = FVector3f::ZeroVector;
	OutParameters.EvaluationRadiusSq = 0.0f;
	OutParameters.EvaluationOriginLow = FVector3f::ZeroVector;
	OutParameters.NumVolumes = 0;
	OutParameters.GridOrigin = FVector3f::ZeroVector;
	OutParameters.EnableFlag = 0;
	OutParameters.CameraCenter = FVector3f::ZeroVector;
	OutParameters.BrickSize = static_cast<uint32>(WorldVoxelPaint::BrickSize);
	OutParameters.PoolTexelExtentRcp = FVector3f::OneVector;
	OutParameters.UniqueBrickSize = static_cast<uint32>(WorldVoxelPaint::UniqueBrickSize);
	OutParameters.GridDims = FIntVector::ZeroValue;
	OutParameters.GridCellSize = FVector2f::ZeroVector;
	OutParameters.PoolDimsInBricks = FIntVector(1, 1, 1);
}

void BuildNeutralExternalSamplingParams(FVoxelPaintExternalSamplingParams& OutParams, FRDGBuilder& GraphBuilder)
{
	OutParams = FVoxelPaintExternalSamplingParams();
	OutParams.Volumes = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer<FVector4f>(GraphBuilder));
	OutParams.PageTables = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer<uint32>(GraphBuilder));
	OutParams.BrickPool = GBlackVolumeTexture->TextureRHI;
	OutParams.SessionTexture = GBlackVolumeTexture->TextureRHI;
	OutParams.PoolSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	// Struct defaults already carry the remaining neutral scalars (zero counts, zero
	// origin/camera/radius, unit pool dims, real brick-geometry constants, empty record view).
}

}
