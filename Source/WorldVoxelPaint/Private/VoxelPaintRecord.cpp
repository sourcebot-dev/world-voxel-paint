// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintRecord.h"

FVoxelPaintVolumeRecord PackVolumeRecord(
	const FVoxelPaintPlacementState& State, const FVoxelPaintResolvedBinding& Binding,
	const FVector& EvaluationOrigin)
{
	FVoxelPaintVolumeRecord Record;
	Record.WorldToVolumeRotation = FQuat4f(State.Rotation.Inverse());
	Record.VolumeCenter = FVector3f(State.Translation - EvaluationOrigin);
	Record.Flags = State.Flags;
	Record.LocalHalfExtent = State.LocalHalfExtent;
	Record.PageTableBaseIndex = Binding.PageTableBaseIndex;
	Record.BlendCoefficients = State.BlendCoefficients;
	Record.TexelSize = State.TexelSize;
	Record.SessionSelector = Binding.SessionSelector;
	Record.StorageMin = State.StorageMin;
	Record.BrickCountX = static_cast<uint32>(State.BrickCounts.X);
	Record.TexelSizeRcp = FVector3f(
		1.0f / State.TexelSize.X, 1.0f / State.TexelSize.Y, 1.0f / State.TexelSize.Z);
	Record.BrickCountYZ = static_cast<uint32>(State.BrickCounts.Y)
		| (static_cast<uint32>(State.BrickCounts.Z) << 16);
	return Record;
}
