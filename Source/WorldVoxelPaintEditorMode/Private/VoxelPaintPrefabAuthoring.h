// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UVoxelPaintVolumeComponent;

namespace WorldVoxelPaint
{
	enum class EVoxelPaintPoolAuthoringVenue : uint8
	{
		Disabled,
		Template,
		PlacementOwned,
		RoutedInstance
	};

	// Resolved authoring context for one customized component. Pool-shape actions write to
	// WriteTarget: the component itself for Template/PlacementOwned, or the introducing class's
	// SCS node template for an SCS placement of that class (RoutedInstance). Blueprint is set
	// for template-backed targets. DisabledReason carries the venue-specific gray-out text.
	struct FVoxelPaintPoolAuthoringContext
	{
		UVoxelPaintVolumeComponent* Component = nullptr;
		UVoxelPaintVolumeComponent* WriteTarget = nullptr;
		UBlueprint* Blueprint = nullptr;
		EVoxelPaintPoolAuthoringVenue Venue = EVoxelPaintPoolAuthoringVenue::Disabled;
		bool bPoolActionsEnabled = false;
		FText DisabledReason;
		// Non-empty while any paint session is open: Create/Add commit a topology edit, which
		// the session gate refuses; Duplicate/Remove stay legal mid-session.
		FText FreshVariantDisabledReason;
	};

	FVoxelPaintPoolAuthoringContext ResolvePoolAuthoringContext(UVoxelPaintVolumeComponent* Component);

	// Fresh-pool auto-size capture: mesh-geometry bounds measured in a live instance's rigid
	// placement frame. The paint volume is always centered on the component transform, so the
	// extent alone cannot represent off-center geometry - the capture carries the centering
	// move alongside it (the Create Volume tool's spawn-at-Bounds.GetCenter(), expressed as a
	// relative location for a component that already exists). When the component cannot move
	// without dragging geometry along (it is the actor root or has attached children), the
	// extent is instead expanded around the component's current position so the volume still
	// covers every mesh.
	struct FVoxelPaintAutoSizeCapture
	{
		FVector HalfExtent = FVector(500.0);
		FVector NewRelativeLocation = FVector::ZeroVector;
		bool bMoveWriteTarget = false;
		bool bBoundsValid = false;
	};

	FVoxelPaintAutoSizeCapture ComputeAutoSizeCapture(const UVoxelPaintVolumeComponent* Instance);

	// Applies the capture's centering move to the routed write target inside the caller's
	// transaction: a live placement moves directly; a template rewrites its RelativeLocation
	// default and propagates to archetype instances still holding the old default.
	void ApplyAutoCenterMove(UVoxelPaintVolumeComponent* WriteTarget,
		const FVector& NewRelativeLocation);

	// Creation flows generalized from the Create Volume tool and the placement authority
	// helpers. Provenance branch: template-backed targets get a <BPDir>/<BPName>_VPD[_VariantN]
	// package, auto Shared with OwnerDisplayName = <BPName>; placement-owned targets keep the
	// existing map-relative Private path. Fresh-asset flows size from mesh-geometry bounds when
	// the expectation is unset, center the routed target's placement on those bounds (or expand
	// to cover them when the component cannot move), record the expectation on the routed
	// target in the same transaction, and propagate every template write to matching archetype
	// instances.
	bool CreatePoolPaintData(const FVoxelPaintPoolAuthoringContext& Context, FText& OutError);
	bool AddPoolVariant(const FVoxelPaintPoolAuthoringContext& Context, FText& OutError);
	bool DuplicatePoolVariant(const FVoxelPaintPoolAuthoringContext& Context, int32 SourceSlot,
		FText& OutError);
	bool RemovePoolVariant(const FVoxelPaintPoolAuthoringContext& Context, int32 Slot,
		FText& OutError);
}
