// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintSessionResources.h"

#include "Async/Async.h"
#include "Engine/TextureRenderTargetVolume.h"
#include "PixelFormat.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "VoxelPaintSessionBudget.h"
#include "WorldVoxelPaintConstants.h"

namespace SessionResourcesPrivate
{
	static UTextureRenderTargetVolume* CreateSessionTexture(const FIntVector& Resolution, bool bWithUAV)
	{
		UTextureRenderTargetVolume* Texture =
			NewObject<UTextureRenderTargetVolume>(GetTransientPackage(), NAME_None, RF_Transient);
		Texture->ClearColor = FLinearColor::Transparent;
		Texture->SRGB = false;
		Texture->bForceLinearGamma = true;
		Texture->bHDR = false;
		// bSupportsUAV is copied to bCanCreateUAV only inside CreateResource
		// (TextureRenderTargetVolume.cpp:125-133) -- it must be set before Init.
		Texture->bSupportsUAV = bWithUAV;
		Texture->Init(Resolution.X, Resolution.Y, Resolution.Z, PF_R8G8B8A8);
		// InitRHI queues a deferred once-clear that would run inside the NEXT scene render --
		// after hydration; resolving it immediately keeps later transfer commands authoritative.
		Texture->UpdateResourceImmediate(true);
		return Texture;
	}
}

FVoxelPaintSessionResources::~FVoxelPaintSessionResources()
{
	ensureMsgf(LedgerHandle == INDEX_NONE,
		TEXT("FVoxelPaintSessionResources destroyed without Release; ledger reservation leaked."));
}

bool FVoxelPaintSessionResources::Allocate(const FIntVector& InResolution,
	WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger, FString& OutError)
{
	checkf(IsInGameThread(), TEXT("FVoxelPaintSessionResources::Allocate is game-thread only"));
	OutError.Reset();

	if (IsAllocated())
	{
		OutError = TEXT("Session texture pair is already allocated.");
		return false;
	}

	const int64 MaxAxis = static_cast<int64>(GMaxVolumeTextureDimensions);
	if (InResolution.X <= 0 || InResolution.Y <= 0 || InResolution.Z <= 0
		|| InResolution.X > MaxAxis || InResolution.Y > MaxAxis || InResolution.Z > MaxAxis)
	{
		OutError = FString::Printf(
			TEXT("Session resolution (%d,%d,%d) is outside the platform volume-texture limits."),
			InResolution.X, InResolution.Y, InResolution.Z);
		return false;
	}

	const int64 PairBytes = 2ll * InResolution.X * InResolution.Y * InResolution.Z
		* WorldVoxelPaint::SessionTexelBytes;
	if (!Ledger.Reserve(WorldVoxelPaint::EVoxelPaintSessionGpuCategory::SessionPair, PairBytes,
		LedgerHandle))
	{
		OutError = FString::Printf(
			TEXT("Session texture pair (%lld bytes) does not fit the editor GPU budget (%lld of %lld bytes in use)."),
			PairBytes, Ledger.GetUsedBytes(), Ledger.GetBudgetBytes());
		return false;
	}

	PublishedTexture = SessionResourcesPrivate::CreateSessionTexture(InResolution, false);
	WorkingTexture = SessionResourcesPrivate::CreateSessionTexture(InResolution, true);
	Resolution = InResolution;

	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Session texture pair allocated: %dx%dx%d PF_R8G8B8A8, %lld bytes."),
		InResolution.X, InResolution.Y, InResolution.Z, PairBytes);
	return true;
}

bool FVoxelPaintSessionResources::IsAllocated() const
{
	return PublishedTexture != nullptr && WorkingTexture != nullptr;
}

FIntVector FVoxelPaintSessionResources::GetResolution() const
{
	return Resolution;
}

UTextureRenderTargetVolume* FVoxelPaintSessionResources::GetPublished() const
{
	return PublishedTexture;
}

UTextureRenderTargetVolume* FVoxelPaintSessionResources::GetWorking() const
{
	return WorkingTexture;
}

FTextureRHIRef FVoxelPaintSessionResources::GetPublishedRHI() const
{
	return PublishedRHI;
}

FTextureRHIRef FVoxelPaintSessionResources::GetWorkingRHI() const
{
	return WorkingRHI;
}

void FVoxelPaintSessionResources::ResolveRHIRefs(TFunction<void(bool bOk)> OnResolved)
{
	checkf(IsInGameThread(), TEXT("FVoxelPaintSessionResources::ResolveRHIRefs is game-thread only"));

	FTextureRenderTargetResource* PublishedResource =
		PublishedTexture ? PublishedTexture->GameThread_GetRenderTargetResource() : nullptr;
	FTextureRenderTargetResource* WorkingResource =
		WorkingTexture ? WorkingTexture->GameThread_GetRenderTargetResource() : nullptr;

	ENQUEUE_RENDER_COMMAND(WorldVoxelPaintSessionResolveRHIRefs)(
		[this, PublishedResource, WorkingResource, OnResolved = MoveTemp(OnResolved)](FRHICommandListImmediate&) mutable
		{
			PublishedRHI = PublishedResource ? PublishedResource->GetRenderTargetTexture() : nullptr;
			WorkingRHI = WorkingResource ? WorkingResource->GetRenderTargetTexture() : nullptr;
			const bool bOk = PublishedRHI.IsValid() && WorkingRHI.IsValid();
			if (OnResolved)
			{
				AsyncTask(ENamedThreads::GameThread,
					[OnResolved = MoveTemp(OnResolved), bOk]()
					{
						OnResolved(bOk);
					});
			}
		});
}

void FVoxelPaintSessionResources::Release(WorldVoxelPaint::FVoxelPaintSessionBudgetLedger& Ledger)
{
	checkf(IsInGameThread(), TEXT("FVoxelPaintSessionResources::Release is game-thread only"));

	// Transfers are drained and the render binding cleared before Release (controller
	// contract), so no render command reads these members anymore; the refs ride one last
	// command so their final release happens render-side, in submission order.
	ENQUEUE_RENDER_COMMAND(WorldVoxelPaintSessionReleaseRHIRefs)(
		[Published = MoveTemp(PublishedRHI), Working = MoveTemp(WorkingRHI)](FRHICommandListImmediate&) mutable
		{
			Published.SafeRelease();
			Working.SafeRelease();
		});

	if (PublishedTexture)
	{
		PublishedTexture->ReleaseResource();
		PublishedTexture = nullptr;
	}
	if (WorkingTexture)
	{
		WorkingTexture->ReleaseResource();
		WorkingTexture = nullptr;
	}
	Resolution = FIntVector::ZeroValue;

	if (LedgerHandle != INDEX_NONE)
	{
		Ledger.Release(LedgerHandle);
		LedgerHandle = INDEX_NONE;
	}
}

void FVoxelPaintSessionResources::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(PublishedTexture);
	Collector.AddReferencedObject(WorkingTexture);
}

FString FVoxelPaintSessionResources::GetReferencerName() const
{
	return TEXT("FVoxelPaintSessionResources");
}
