// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UClass;
class UVoxelPaintVolumeComponent;
struct FVoxelPaintExpectedTopology;
struct FVoxelPaintVariantPoolEntry;

namespace WorldVoxelPaint
{
	enum class EVoxelPaintPoolProvenance : uint8
	{
		Native,
		Instance,
		SCSOwnClass,
		SCSOverrideOwner,
		SCSAncestor,
		UCS
	};

	// Classifies a component instance against the class the user operates on. SCSOwnClass: the
	// operated-on class introduced the SCS node itself. SCSOverrideOwner: an ancestor introduced
	// the node and the operated-on class holds a class-local inherited-component override for
	// it. SCSAncestor: an ancestor's template supplies the pool with no class-local override.
	WORLDVOXELPAINTTOOLSET_API EVoxelPaintPoolProvenance ClassifyPoolProvenance(
		const UVoxelPaintVolumeComponent* Component, const UClass* OperatedClass);

	// The pool's effective supplying template: the owning class's own inherited-component
	// override, an ancestor's inherited override, or the introducing SCS node template. Never
	// creates an override. Returns null for templates and for Native/Instance/UCS provenance;
	// callers treat null as a single-member domain.
	WORLDVOXELPAINTTOOLSET_API UVoxelPaintVolumeComponent* ResolveEffectiveSupplyingTemplate(
		const UVoxelPaintVolumeComponent* Component);

	// Expectation-write-target resolution for topology ops initiated from a placement.
	// Native/Instance: the component itself. SCSOwnClass: the class's own SCS node template.
	// SCSOverrideOwner: the existing class-local override (never created from a placement).
	// SCSAncestor/UCS: null with OutError set, before any asset mutation. Template inputs
	// resolve to themselves.
	WORLDVOXELPAINTTOOLSET_API UVoxelPaintVolumeComponent* ResolveExpectationWriteTarget(
		UVoxelPaintVolumeComponent* Component, FText& OutError);

	// After any direct C++ write to a template's VolumeVariants/ExpectedTopology, inside the
	// same transaction: sweeps the template's archetype instances and applies the identical
	// change to every non-template instance whose pre-edit value of the written member matched
	// the template's pre-edit value, under per-instance Modify plus a synthesized
	// PostEditChangeProperty. Instances holding a genuinely divergent value are preserved.
	WORLDVOXELPAINTTOOLSET_API void PropagateTemplatePoolChange(UVoxelPaintVolumeComponent* Template,
		const TArray<FVoxelPaintVariantPoolEntry>& PreEditPool,
		const FVoxelPaintExpectedTopology& PreEditExpectation);
}
