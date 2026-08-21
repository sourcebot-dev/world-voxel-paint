// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectKey.h"
#include "WorldVoxelPaintConstants.h"

#if WITH_EDITOR
using FVoxelPaintFrontierIdentity = FGuid;
#else
using FVoxelPaintFrontierIdentity = FObjectKey;
#endif

// Packed volume record contract: 7 float4 lanes, 112 bytes. Single writer: the publication
// assembler (PackVolumeRecord is its packing primitive and the desired-state-only test
// helper's). Consumers: the grid-build pass and the evaluation library.
//
// | Lane | .xyz / components | .w | Encoding |
// |---|---|---|---|
// | 0 | WorldToVolumeRotation quaternion (x,y,z,w) - maps world-frame vectors into the
// |   | volume-local frame (inverse of the canonical rigid placement rotation) | (quat w)
// |   | numeric float4 |
// | 1 | VolumeCenter - placement translation MINUS evaluation origin | Flags
// |   | xyz numeric; w bit-preserved uint32 (0x1 = RecordFlag_Valid; all other bits
// |   | reserved 0) |
// | 2 | LocalHalfExtent - volume-local half extent, cm | PageTableBaseIndex
// |   | xyz numeric; w bit-preserved uint32 (0xFFFFFFFF = NoPageTableSentinel = no-table /
// |   | zero-contribution neutral-proxy binding; otherwise base entry index into the shared
// |   | PageTables buffer) |
// | 3 | BlendCoefficients (R,G,B,A) - +1.0 Add / -1.0 Subtract | (A coeff) | numeric float4 |
// | 4 | TexelSize - volume-local cm per texel per axis | SessionSelector
// |   | xyz numeric; w bit-preserved uint32 (SessionSelectorActive (1) = live session-paint
// |   | placement: evaluation samples the dense SessionTexture and skips the page table; such
// |   | records always pack PageTableBaseIndex = NoPageTableSentinel. 0 = page-table path.) |
// | 5 | StorageMin - volume-local position of the storage lattice's minimum box corner
// |   | (= asset StorageOrigin - LocalHalfExtent), cm | BrickCountX
// |   | xyz numeric; w bit-preserved uint32 (per-axis brick count, X) |
// | 6 | TexelSizeRcp - texels per cm per axis (precomputed reciprocal of lane 4.xyz)
// |   | BrickCountYZ | xyz numeric; w bit-preserved uint32 (Y in bits 0-15, Z in bits 16-31) |
//
// Storage-texel mapping (the one world-to-texel transform, center-registered): with local the
// volume-local position, the continuous texel coordinate per axis is
// T = (local - StorageMin) * TexelSizeRcp - 0.5; integer T is a texel center; box faces sit
// half a texel outside the outermost centers. Every consumer clamps T per axis to
// [0, Resolution-1] (Resolution = 7 * BrickCount per axis) after containment and before brick
// decomposition. Brick coordinate B = floor(T / 7); in-brick L = T - B*7 in [0,7); flattened
// logical brick index X-fastest: Flat = Bx + BrickCountX * (By + BrickCountY * Bz). Page entry
// index = PageTableBaseIndex + Flat; entry value 0 = absent (exact-zero contribution, NO
// fetch), else PoolSlot + 1. Pool slot unflatten X-fastest over PoolDimsInBricks; physical
// sample UVW = (SlotCoord * 8 + L + 0.5) * PoolTexelExtentRcp, SampleLevel mip 0.
//
// Integer lanes are stored bit-preserved (typed uint32 POD fields at float4 lane offsets,
// uploaded by memcpy - bit-identical to the FMath::AsFloat idiom) and decoded with asuint()
// shader-side; never numerically converted.

struct FVoxelPaintResolvedBinding
{
	uint32 PageTableBaseIndex = WorldVoxelPaint::NoPageTableSentinel;
	uint32 SessionSelector = 0;
};

struct FVoxelPaintPlacementState
{
	FQuat Rotation = FQuat::Identity;
	FVector Translation = FVector::ZeroVector;
	FVector3f LocalHalfExtent = FVector3f(500.0f);
	FVector3f TexelSize = FVector3f(1.0f);
	FVector3f StorageMin = FVector3f(-500.0f);
	FIntVector BrickCounts = FIntVector(4, 4, 4);
	FVector4f BlendCoefficients = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	uint32 Flags = 0;
	FObjectKey AssetKey;
	uint64 SourceGeneration = 0;
	FVoxelPaintFrontierIdentity Identity;
};

struct FVoxelPaintVolumeRecord
{
	FQuat4f WorldToVolumeRotation;      // lane 0
	FVector3f VolumeCenter;             // lane 1.xyz (evaluation-origin-relative)
	uint32 Flags;                       // lane 1.w
	FVector3f LocalHalfExtent;          // lane 2.xyz
	uint32 PageTableBaseIndex;          // lane 2.w
	FVector4f BlendCoefficients;        // lane 3
	FVector3f TexelSize;                // lane 4.xyz
	uint32 SessionSelector;             // lane 4.w
	FVector3f StorageMin;               // lane 5.xyz
	uint32 BrickCountX;                 // lane 5.w
	FVector3f TexelSizeRcp;             // lane 6.xyz
	uint32 BrickCountYZ;                // lane 6.w
};
static_assert(sizeof(FVoxelPaintVolumeRecord) == WorldVoxelPaint::RecordNumFloat4Lanes * 16);
static_assert(offsetof(FVoxelPaintVolumeRecord, Flags) == 28);
static_assert(offsetof(FVoxelPaintVolumeRecord, PageTableBaseIndex) == 44);
static_assert(offsetof(FVoxelPaintVolumeRecord, SessionSelector) == 76);
static_assert(offsetof(FVoxelPaintVolumeRecord, BrickCountX) == 92);
static_assert(offsetof(FVoxelPaintVolumeRecord, BrickCountYZ) == 108);

WORLDVOXELPAINT_API FVoxelPaintVolumeRecord PackVolumeRecord(
	const FVoxelPaintPlacementState& State, const FVoxelPaintResolvedBinding& Binding,
	const FVector& EvaluationOrigin);
