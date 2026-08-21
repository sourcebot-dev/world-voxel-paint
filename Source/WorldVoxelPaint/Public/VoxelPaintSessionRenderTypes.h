// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "UObject/ObjectKey.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintResidencyTypes.h"

enum class EVoxelPaintSessionBindingPhase : uint8 { None, PreCommit, PostCommit };

struct FVoxelPaintSessionRenderBinding
{
	FVoxelPaintAssetKey AssetKey;
	FObjectKey OriginWorldKey;                 // the master at session open
	uint64 SessionGeneration = 0;              // monotonic per controller-opened session
	EVoxelPaintSessionBindingPhase Phase = EVoxelPaintSessionBindingPhase::None;
	FVoxelPaintCanonicalIdentity CommittedIdentity;   // valid from PostCommit on
	FTextureRHIRef PublishedTexture;           // material-visible; NEVER carries UAV
	FIntVector Resolution = FIntVector::ZeroValue;

	bool IsActive() const { return Phase != EVoxelPaintSessionBindingPhase::None; }
};

namespace WorldVoxelPaint
{
	enum class EVoxelPaintStagedOutcome : uint8 { UseResidency, UseSessionTexture, ForceNeutralProxy };

	// THE session-staged match-or-neutral rule, evaluated per placement of the
	// ACTIVE asset BEFORE the residency binding is honored; pure and headless-tested:
	//  PreCommit  + origin master     -> UseSessionTexture
	//  PreCommit  + non-origin master -> ForceNeutralProxy   (baseline BC7 explicitly ineligible)
	//  PostCommit + origin master     -> UseSessionTexture UNTIL a published complete
	//                                    representation matches CommittedIdentity -> UseResidency
	//                                    (the atomic flip)
	//  PostCommit + non-origin master -> UseResidency once the desired identity matches
	//                                    CommittedIdentity; ForceNeutralProxy while it is stale
	//                                    (never baseline or stale BC7 post-commit)
	//  Phase None / other assets      -> UseResidency
	WORLDVOXELPAINT_API EVoxelPaintStagedOutcome ResolveSessionStagedBinding(
		const FVoxelPaintSessionRenderBinding& Binding, bool bMasterIsOrigin,
		EVoxelPaintBindingKind ResidencyKind, const FVoxelPaintCanonicalIdentity& DesiredIdentity);
}

struct FVoxelPaintSessionRenderStatus          // RT-written mirror; game-thread observation only
{
	uint64 SessionGeneration = 0;
	bool bSelectorPublishedLastFrame = false;  // any record published SessionSelectorActive
	bool bMatchingDataPublished = false;       // active asset published a complete representation
	                                           // matching CommittedIdentity
	uint64 LastSelectorPublicationOrdinal = 0;
	uint64 CurrentPublicationOrdinal = 0;
	uint8 GridReadiness = 0;                   // EVoxelPaintGridReadiness mirror
	uint64 GridRecoveryRequestCount = 0;       // monotonic; game thread fires one recovery
	                                           // rebuild per increment
	TArray<FVoxelPaintFrontierIdentity> SelectorPublishedIdentities;   // placements whose record
	                                           // published SessionSelectorActive last assembly
};

WORLDVOXELPAINT_API bool IsPlacementSelectorPublished(
	const FVoxelPaintSessionRenderStatus& Status, uint64 SessionGeneration,
	const FVoxelPaintFrontierIdentity& Identity);
