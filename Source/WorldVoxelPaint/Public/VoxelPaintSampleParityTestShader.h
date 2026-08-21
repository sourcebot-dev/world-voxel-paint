// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// Editor-only automation-test shader for the grid-free parameterized sampler
// (WVP_SampleVoxelPaintRecords in VoxelPaintSample.ush). Lives in this PostConfigInit runtime
// module for the same reason as VoxelPaintSessionTransferShaders.h: FShaderTypeRegistration
// asserts on any type registered after CommitAll, which runs before Default-phase editor
// modules load. The HasEditorOnlyData permutation gate keeps it out of every cooked build.
// Dispatched by WorldVoxelPaintEditorMode/Private/Tests/VoxelPaintPCGSampleParityTest.cpp.
class FVoxelPaintSampleParityCS : public FGlobalShader
{
public:
	DECLARE_EXPORTED_GLOBAL_SHADER(FVoxelPaintSampleParityCS, WORLDVOXELPAINT_API);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPaintSampleParityCS, FGlobalShader);

	static constexpr int32 ThreadGroupSize = 64;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Volumes)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, PageTables)
		SHADER_PARAMETER_TEXTURE(Texture3D, BrickPool)
		SHADER_PARAMETER_TEXTURE(Texture3D, SessionTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, PoolSampler)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RecordIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ProbePositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutWeights)
		SHADER_PARAMETER(uint32, NumRecordIndices)
		SHADER_PARAMETER(uint32, NumVolumes)
		SHADER_PARAMETER(uint32, NumProbes)
		SHADER_PARAMETER(uint32, BrickSize)
		SHADER_PARAMETER(uint32, UniqueBrickSize)
		SHADER_PARAMETER(FIntVector, PoolDimsInBricks)
		SHADER_PARAMETER(FVector3f, PoolTexelExtentRcp)
		SHADER_PARAMETER(FVector3f, CameraCenter)
		SHADER_PARAMETER(float, EvaluationRadiusSq)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		FShaderCompilerEnvironment& OutEnvironment);
};

#endif
