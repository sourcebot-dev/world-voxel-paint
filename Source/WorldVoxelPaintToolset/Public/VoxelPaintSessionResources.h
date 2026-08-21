// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RHIFwd.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"

class UTextureRenderTargetVolume;

namespace WorldVoxelPaint
{
	class FVoxelPaintSessionBudgetLedger;
}

class WORLDVOXELPAINTTOOLSET_API FVoxelPaintSessionResources : public FGCObject
{
public:
	FVoxelPaintSessionResources() = default;
	virtual ~FVoxelPaintSessionResources() override;

	// Game thread. Reserves SessionPair bytes in the ledger FIRST; a rejected reservation
	// creates no objects. Working carries bSupportsUAV; Published never does.
	bool Allocate(const FIntVector& InResolution,
		WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger, FString& OutError);
	bool IsAllocated() const;
	FIntVector GetResolution() const;

	UTextureRenderTargetVolume* GetPublished() const;
	UTextureRenderTargetVolume* GetWorking() const;

	// Render thread; null until ResolveRHIRefs has completed. This object must stay alive
	// until Release (in-flight render commands read these members).
	FTextureRHIRef GetPublishedRHI() const;
	FTextureRHIRef GetWorkingRHI() const;
	// Game thread; OnResolved(bOk) delivered on the game thread after one render-command
	// round trip.
	void ResolveRHIRefs(TFunction<void(bool bOk)> OnResolved);

	// Game thread. Caller must first observe CurrentPublicationOrdinal >
	// LastSelectorPublicationOrdinal on the coordinator status mirror (one-ordinal
	// deferral); RHI deferred deletion covers still-executing GPU work after that. Frees the
	// ledger reservation.
	void Release(WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger);

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override;

private:
	TObjectPtr<UTextureRenderTargetVolume> PublishedTexture;
	TObjectPtr<UTextureRenderTargetVolume> WorkingTexture;
	FTextureRHIRef PublishedRHI;
	FTextureRHIRef WorkingRHI;
	FIntVector Resolution = FIntVector::ZeroValue;
	int32 LedgerHandle = INDEX_NONE;
};
