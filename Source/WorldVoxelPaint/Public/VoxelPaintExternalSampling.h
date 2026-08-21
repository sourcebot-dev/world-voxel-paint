// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "RHIResources.h"
#include "RenderGraphFwd.h"
#include "UObject/ObjectKey.h"
#include "VoxelPaintRecord.h"
#include "WorldVoxelPaintConstants.h"

class FRDGBuilder;

// Out-parameter of FVoxelPaintCoordinator::RegisterForExternalSampling (M6 plan 5.2): the exact
// resource/scalar subset of FVoxelPaintSceneParameters an external (non-material) compute
// consumer needs to run the grid-free sampler WVP_SampleVoxelPaintRecords
// (Shaders/VoxelPaintSample.ush), plus the CPU-side record mirror external callers cull
// against. Deliberately a plain struct, NOT a shader-parameter struct: it carries non-shader
// members (record mirror, world identity), and consumers copy fields into their own parameter
// structs (a ComputeFramework data-interface parameter struct, a test shader's FParameters).
//
// Lifetime contract: every member is valid ONLY for the current render-thread callback and the
// RDG builder passed to the accessor. Never cache anything across frames - Records aliases
// coordinator render-thread state the next render command may reassemble or wipe, and
// SessionTexture is re-evaluated per call by design (selector publication is per-frame state).
struct FVoxelPaintExternalSamplingParams
{
	// GPU resources. Volumes/PageTables are SRVs registered into the caller's builder.
	// BrickPool/SessionTexture are plain RHI texture refs (created raw, never RDG-registered -
	// bind them through SHADER_PARAMETER_TEXTURE(Texture3D, ...) slots). One shared
	// trilinear-clamp sampler serves BOTH textures; binding any other sampler changes edge
	// blending versus the material path.
	FRDGBufferSRVRef Volumes = nullptr;             // StructuredBuffer<float4>, 7 lanes/record
	FRDGBufferSRVRef PageTables = nullptr;          // StructuredBuffer<uint>; entry 0 = absent
	FTextureRHIRef BrickPool;                       // BC7 Texture3D atlas
	FTextureRHIRef SessionTexture;                  // dense session Texture3D, or neutral black
	FRHISamplerState* PoolSampler = nullptr;        // TStaticSamplerState<SF_Trilinear, AM_Clamp x3>

	// Scalars; semantics identical to the FVoxelPaintSceneParameters members of the same names.
	uint32 NumVolumes = 0;                          // record count; 0 = valid published-empty
	FVector3f EvaluationOriginHigh = FVector3f::ZeroVector;   // double-float split of the
	FVector3f EvaluationOriginLow = FVector3f::ZeroVector;    //   evaluation origin (LWC)
	uint32 BrickSize = static_cast<uint32>(WorldVoxelPaint::BrickSize);
	uint32 UniqueBrickSize = static_cast<uint32>(WorldVoxelPaint::UniqueBrickSize);
	FIntVector PoolDimsInBricks = FIntVector(1, 1, 1);
	FVector3f PoolTexelExtentRcp = FVector3f::OneVector;
	FVector3f CameraCenter = FVector3f::ZeroVector; // evaluation-origin-relative; 2D XY cull
	float EvaluationRadiusSq = 0.0f;

	// CPU-side mirror of the packed records the Volumes buffer holds: same count and order as
	// the GPU buffer, VolumeCenter evaluation-origin-relative. External callers build their
	// culled record index lists from this; any list handed to the shader must preserve
	// ASCENDING record-index order (accumulation-order parity with the grid path's
	// canonicalized ascending order).
	TConstArrayView<FVoxelPaintVolumeRecord> Records;

#if WITH_EDITOR
	// Identity of the master world the published records came from (FObjectKey of the UWorld).
	// Editor-only on purpose: PIE-vs-editor is the only reachable multi-world case for external
	// GPU sampling (PCG creates GPU graph tasks only in editor builds). A caller whose
	// execution world differs must discard these params and fall back to neutral bindings -
	// the records are another world's placements.
	FObjectKey MasterWorldKey;
#endif
};

namespace WorldVoxelPaint
{
	// Neutral bindings for the false-return path of RegisterForExternalSampling (the external
	// twin of BuildNeutralSceneParameters): zero-filled default structured buffers, the black
	// volume texture for BOTH BrickPool and SessionTexture, the shared trilinear-clamp sampler,
	// zero counts/origin/radius, unit pool dims. The grid-free sampler then yields exact zero
	// for every position.
	WORLDVOXELPAINT_API void BuildNeutralExternalSamplingParams(
		FVoxelPaintExternalSamplingParams& OutParams, FRDGBuilder& GraphBuilder);
}
