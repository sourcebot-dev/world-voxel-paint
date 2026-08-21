// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintGpuPickProvider.h"

#include "Async/Async.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/World.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Math/ScaleMatrix.h"
#include "Math/TranslationMatrix.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RHI.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SceneInterface.h"
#include "SceneRenderTargetParameters.h"
#include "SceneView.h"
#include "ShowFlags.h"
#include "Templates/TypeCompatibleBytes.h"
#include "VoxelPaintBrushMath.h"
#include "VoxelPaintCoordinator.h"
#include "VoxelPaintPickShaders.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "WorldVoxelPaintConstants.h"

#include <atomic>

DECLARE_GPU_STAT_NAMED(VoxelPaintPick, TEXT("VoxelPaint Pick"));

static TAutoConsoleVariable<int32> CVarVoxelPaintEditorGpuPicking(
	TEXT("r.VoxelPaint.Editor.GpuPicking"),
	1,
	TEXT("1 (default): brush stamp selection uses the GPU visible-pixel picker with per-stamp CPU fallback on any unavailability. 0: the CPU pick provider is used for every stamp."),
	ECVF_Default);

namespace GpuPickProviderPrivate
{
	enum : uint8
	{
		ClaimNone = 0,
		ClaimGpu = 1,
		ClaimFallback = 2
	};

	static constexpr double ConsumeDeadlineSeconds = 0.5;
	static constexpr double CursorProjectDistance = 100000.0;
	static constexpr uint32 SeedTrailerLanes = 4;

	static int64 EstimateStampFootprintTexels(float RadiusWorld, const FVector& TexelSize,
		const FIntVector& Resolution)
	{
		int64 Product = 1;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			// 2x2x2 trilinear pick footprint bound: per-axis emitted range is [floor(Tmin), floor(Tmax)+1] with Tmax - Tmin <= 2r, so the count is < int(2r) + 4.
			const int64 SpanTexels = (int64)(2.0 * RadiusWorld
				/ FMath::Max(TexelSize[Axis], UE_DOUBLE_SMALL_NUMBER)) + 4;
			Product *= FMath::Clamp<int64>(SpanTexels, 1, Resolution[Axis]);
		}
		return Product;
	}
}

struct FVoxelPaintGpuPickProvider::FGpuPickRequest
{
	FVoxelPaintStampRequest Source;
	TFunction<void(FVoxelPaintGatherList&&)> OnReady;
	FMatrix WorldToTexel = FMatrix::Identity;
	FVector RayOrigin = FVector::ZeroVector;
	FVector RayDirection = FVector::ForwardVector;
	FIntVector Resolution = FIntVector::ZeroValue;
	FIntVector BrickCounts = FIntVector::ZeroValue;
	float BrushRadiusWorld = 0.0f;
	float FlattenPct = 0.0f;
	float AngleToleranceDeg = 90.0f;
	float MinFlattenDepthWorld = 0.0f;
	float BrushRadiusTexels = 0.0f;
	uint32 IndexCapacity = 0;
	uint32 ReadbackNumBytes = 0;
	const void* WorldKey = nullptr;
	int32 SlotIndex = INDEX_NONE;
	double IssueTimeSeconds = 0.0;

	// FailReason is written before the bFailed store and read only after its load.
	const TCHAR* FailReason = nullptr;

	std::atomic<uint8> Claim { GpuPickProviderPrivate::ClaimNone };
	std::atomic<bool> bCopyEnqueued { false };
	std::atomic<bool> bPollInFlight { false };
	std::atomic<bool> bDataReady { false };
	std::atomic<bool> bFailed { false };

	// Render-thread written before the bDataReady store, game-thread read after its load.
	TArray<uint32> ReadbackData;
};

// Game-thread-only fields, but the object outlives the provider so drain-completion tasks
// arriving after a teardown-time destruction touch no freed memory.
struct FVoxelPaintGpuPickProvider::FGpuPickDrainState
{
	int32 PendingFences = 0;
	bool bArmed = false;
	TFunction<void()> OnDrained;

	static void ResolveFence(const TSharedPtr<FGpuPickDrainState, ESPMode::ThreadSafe>& State)
	{
		AsyncTask(ENamedThreads::GameThread, [State]()
		{
			--State->PendingFences;
			MaybeFire(State);
		});
	}

	static void MaybeFire(const TSharedPtr<FGpuPickDrainState, ESPMode::ThreadSafe>& State)
	{
		if (!State->bArmed || State->PendingFences > 0 || !State->OnDrained)
		{
			return;
		}
		TFunction<void()> Drain = MoveTemp(State->OnDrained);
		State->OnDrained = nullptr;
		Drain();
	}
};

// Pending and Readbacks are touched only by render commands and the bound pick-pass functor.
struct FVoxelPaintGpuPickProvider::FGpuPickRenderState
{
	TArray<FGpuPickRequestRef> Pending;
	TArray<TUniquePtr<FRHIGPUBufferReadback>> Readbacks;

	void ServiceOnePending(FRDGBuilder& GraphBuilder, const FSceneView& View,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures);
	void ExecutePickPass(FRDGBuilder& GraphBuilder, const FSceneView& View,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures,
		FGpuPickRequest& Request);
};

void FVoxelPaintGpuPickProvider::FGpuPickRenderState::ServiceOnePending(
	FRDGBuilder& GraphBuilder, const FSceneView& View,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	using namespace GpuPickProviderPrivate;

	while (!Pending.IsEmpty())
	{
		FGpuPickRequestRef Request = Pending[0];
		Pending.RemoveAt(0);
		uint8 Expected = ClaimNone;
		if (!Request->Claim.compare_exchange_strong(Expected, ClaimGpu))
		{
			continue;
		}
		ExecutePickPass(GraphBuilder, View, SceneTextures, *Request);
		return;
	}
}

void FVoxelPaintGpuPickProvider::FGpuPickRenderState::ExecutePickPass(
	FRDGBuilder& GraphBuilder, const FSceneView& View,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures,
	FGpuPickRequest& Request)
{
	using namespace GpuPickProviderPrivate;

	RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelPaintPick, "WorldVoxelPaint.Pick");

	const auto Fail = [&Request](const TCHAR* Reason)
	{
		Request.FailReason = Reason;
		Request.bFailed = true;
	};

	if (!SceneTextures)
	{
		Fail(TEXT("NoSceneTextures"));
		return;
	}
	if (!View.Family || !View.Family->EngineShowFlags.Lighting
		|| View.Family->EngineShowFlags.Wireframe)
	{
		Fail(TEXT("NoUsableGBuffer"));
		return;
	}
	if (!View.Family->Scene || View.Family->Scene->GetWorld() != Request.WorldKey)
	{
		Fail(TEXT("WorldMismatch"));
		return;
	}
	if (!View.ViewUniformBuffer.IsValid())
	{
		Fail(TEXT("NoViewUniformBuffer"));
		return;
	}
	const ERHIFeatureLevel::Type FeatureLevel = View.GetFeatureLevel();
	if (FeatureLevel < ERHIFeatureLevel::SM5)
	{
		Fail(TEXT("FeatureLevelUnsupported"));
		return;
	}
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(FeatureLevel);
	TShaderRef<FVoxelPaintPickSeedCS> SeedShader =
		ShaderMap->GetShader<FVoxelPaintPickSeedCS>();
	FVoxelPaintPickCS::FPermutationDomain PickPermutationVector;
	PickPermutationVector.Set<FVoxelPaintPickCS::FWaveOps>(
		GRHISupportsWaveOperations && RHISupportsWaveOperations(GMaxRHIShaderPlatform));
	TShaderRef<FVoxelPaintPickCS> PickShader =
		ShaderMap->GetShader<FVoxelPaintPickCS>(PickPermutationVector);
	if (!SeedShader.IsValid() || !PickShader.IsValid())
	{
		Fail(TEXT("ShaderUnavailable"));
		return;
	}
	// The hook-passed UB is built before the base pass (SceneDepth setup only), so GBufferA
	// binds black through it; rebuild against the just-produced GBuffer instead.
	TRDGUniformBufferRef<FSceneTextureUniformParameters> PickSceneTextures =
		CreateSceneTextureUniformBuffer(GraphBuilder, View,
			ESceneTextureSetupMode::SceneDepth | ESceneTextureSetupMode::GBufferA);
	if (!PickSceneTextures)
	{
		Fail(TEXT("NoSceneTextures"));
		return;
	}
	FRDGTextureRef SceneDepthTexture = PickSceneTextures->GetParameters()->SceneDepthTexture;
	if (!SceneDepthTexture)
	{
		Fail(TEXT("NoSceneDepth"));
		return;
	}
	const FIntPoint DepthExtent = SceneDepthTexture->Desc.Extent;
	if (DepthExtent.X <= 0 || DepthExtent.Y <= 0)
	{
		Fail(TEXT("EmptyViewRect"));
		return;
	}

	const FVector PreViewTranslation = View.ViewMatrices.GetPreViewTranslation();
	const FVector4 Clip = View.ViewMatrices.GetTranslatedViewProjectionMatrix().TransformFVector4(
		FVector4(Request.RayOrigin + PreViewTranslation
			+ Request.RayDirection * CursorProjectDistance, 1.0));
	if (Clip.W <= UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		Fail(TEXT("CursorBehindView"));
		return;
	}
	const FVector2f CursorNdc((float)(Clip.X / Clip.W), (float)(Clip.Y / Clip.W));
	if (FMath::Abs(CursorNdc.X) > 1.0f || FMath::Abs(CursorNdc.Y) > 1.0f)
	{
		Fail(TEXT("CursorOffscreen"));
		return;
	}

	const FMatrix44f TranslatedWorldToTexel =
		FMatrix44f(FTranslationMatrix(-PreViewTranslation) * Request.WorldToTexel);

	const uint32 OutputLanes = 2u + Request.IndexCapacity + SeedTrailerLanes;
	FRDGBufferRef SeedParamsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), 2),
		TEXT("WorldVoxelPaint.PickSeedParams"));
	FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), (int32)OutputLanes),
		TEXT("WorldVoxelPaint.PickOutput"));
	const int64 DomainTexels = (int64)Request.Resolution.X * Request.Resolution.Y
		* Request.Resolution.Z;
	const int32 MaskWords = (int32)((DomainTexels + 31) / 32);
	FRDGBufferRef DedupeMaskBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), MaskWords),
		TEXT("WorldVoxelPaint.PickDedupeMask"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DedupeMaskBuffer), 0u);

	FVoxelPaintPickSeedCS::FParameters* SeedParameters =
		GraphBuilder.AllocParameters<FVoxelPaintPickSeedCS::FParameters>();
	SeedParameters->SceneTextures = PickSceneTextures;
	SeedParameters->View = View.ViewUniformBuffer;
	SeedParameters->RWSeedParams = GraphBuilder.CreateUAV(SeedParamsBuffer);
	SeedParameters->RWOutput = GraphBuilder.CreateUAV(OutputBuffer);
	SeedParameters->CursorNdc = CursorNdc;
	SeedParameters->TranslatedWorldToTexel = TranslatedWorldToTexel;
	SeedParameters->IndexCapacity = Request.IndexCapacity;
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("WorldVoxelPaint.PickSeed"),
		SeedShader, SeedParameters, FIntVector(1, 1, 1));

	FVoxelPaintPickCS::FParameters* PickParameters =
		GraphBuilder.AllocParameters<FVoxelPaintPickCS::FParameters>();
	PickParameters->SceneTextures = PickSceneTextures;
	PickParameters->View = View.ViewUniformBuffer;
	PickParameters->SeedParams = GraphBuilder.CreateSRV(SeedParamsBuffer);
	PickParameters->RWOutput = GraphBuilder.CreateUAV(OutputBuffer);
	PickParameters->RWDedupeMask = GraphBuilder.CreateUAV(DedupeMaskBuffer);
	PickParameters->TranslatedWorldToTexel = TranslatedWorldToTexel;
	PickParameters->Resolution = Request.Resolution;
	PickParameters->IndexCapacity = Request.IndexCapacity;
	PickParameters->BrushRadiusWorld = Request.BrushRadiusWorld;
	PickParameters->FlattenPct = Request.FlattenPct;
	PickParameters->MinFlattenDepthWorld = Request.MinFlattenDepthWorld;
	PickParameters->AngleToleranceDeg = Request.AngleToleranceDeg;
	FComputeShaderUtils::AddPass(GraphBuilder,
		RDG_EVENT_NAME("WorldVoxelPaint.Pick(%dx%d)", DepthExtent.X, DepthExtent.Y),
		PickShader, PickParameters,
		FComputeShaderUtils::GetGroupCount(DepthExtent,
			FIntPoint(FVoxelPaintPickCS::ThreadGroupDim, FVoxelPaintPickCS::ThreadGroupDim)));

	if (!Readbacks.IsValidIndex(Request.SlotIndex))
	{
		Fail(TEXT("SlotInvalid"));
		return;
	}
	TUniquePtr<FRHIGPUBufferReadback>& Slot = Readbacks[Request.SlotIndex];
	if (!Slot.IsValid())
	{
		Slot = MakeUnique<FRHIGPUBufferReadback>(FName(TEXT("WorldVoxelPaint.PickReadback")));
	}
	AddEnqueueCopyPass(GraphBuilder, Slot.Get(), OutputBuffer, Request.ReadbackNumBytes);
	Request.bCopyEnqueued = true;
}

FVoxelPaintGpuPickProvider::FVoxelPaintGpuPickProvider(IVoxelPaintPickProvider& InCpuFallback)
	: CpuFallback(InCpuFallback)
	, RenderState(MakeShared<FGpuPickRenderState, ESPMode::ThreadSafe>())
	, DrainState(MakeShared<FGpuPickDrainState, ESPMode::ThreadSafe>())
	, LastPickSource(TEXT("None"))
{
	SlotRequests.SetNum(WorldVoxelPaint::PickReadbackSlots);
	RenderState->Readbacks.SetNum(WorldVoxelPaint::PickReadbackSlots);
}

FVoxelPaintGpuPickProvider::~FVoxelPaintGpuPickProvider()
{
	ensureMsgf(!HasInFlightRequests(),
		TEXT("FVoxelPaintGpuPickProvider destroyed with picks in flight; CancelAndDrain first."));
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (bDelegateBound)
	{
		if (FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
		{
			Coordinator->ClearPickPassDelegate([]() {});
		}
		bDelegateBound = false;
	}
	TSharedPtr<FGpuPickRenderState, ESPMode::ThreadSafe> State = RenderState;
	ENQUEUE_RENDER_COMMAND(WorldVoxelPaintPickRelease)(
		[State](FRHICommandListImmediate&)
		{
			State->Pending.Empty();
			State->Readbacks.Empty();
		});
}

void FVoxelPaintGpuPickProvider::RequestStampSelection(const FVoxelPaintStampRequest& Request,
	TFunction<void(FVoxelPaintGatherList&&)> OnReady)
{
	using namespace GpuPickProviderPrivate;

	checkf(IsInGameThread(), TEXT("Pick requests are game-thread entry points"));

	if (bDraining)
	{
		FallBackToCpu(TEXT("Draining"), Request, MoveTemp(OnReady));
		return;
	}
	if (CVarVoxelPaintEditorGpuPicking.GetValueOnGameThread() == 0)
	{
		FallBackToCpu(TEXT("CVarDisabled"), Request, MoveTemp(OnReady));
		return;
	}

	UWorld* World = Request.World.Get();
	UVoxelPaintVolumeComponent* Target = Request.Target.Get();
	if (!World || !Target || !Target->HasValidVolumeData() || Target->GetWorld() != World)
	{
		FallBackToCpu(TEXT("InvalidTarget"), Request, MoveTemp(OnReady));
		return;
	}
	const UVoxelPaintVolumeData* Asset = Target->ResolveActiveVariant();
	const FIntVector Resolution = Asset->GetResolution();
	const FIntVector BrickCounts = Asset->GetBrickCounts();
	const FVector TexelSize = Asset->GetTexelSize();
	const FVector StorageMin = Asset->GetStorageMin();
	if (TexelSize.GetMin() <= 0.0 || Resolution.GetMin() <= 0)
	{
		FallBackToCpu(TEXT("InvalidTarget"), Request, MoveTemp(OnReady));
		return;
	}
	const FVector RayDirection = Request.RayDirection.GetSafeNormal();
	if (RayDirection.IsNearlyZero())
	{
		FallBackToCpu(TEXT("DegenerateRay"), Request, MoveTemp(OnReady));
		return;
	}

	if (GUsingNullRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5)
	{
		FallBackToCpu(TEXT("RHIUnsupported"), Request, MoveTemp(OnReady));
		return;
	}
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	if (!ShaderMap || !ShaderMap->HasShader(&FVoxelPaintPickSeedCS::GetStaticType(), 0)
		|| !ShaderMap->HasShader(&FVoxelPaintPickCS::GetStaticType(), 0))
	{
		FallBackToCpu(TEXT("ShaderUnavailable"), Request, MoveTemp(OnReady));
		return;
	}
	FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated();
	if (!Coordinator || !Coordinator->IsMasterWorldWithEligiblePrimary(World))
	{
		FallBackToCpu(TEXT("NoMasterPrimary"), Request, MoveTemp(OnReady));
		return;
	}

	const float RadiusWorld = FMath::Max(0.0f, Request.Brush.RadiusWorld);
	const int64 DomainTexels = (int64)Resolution.X * Resolution.Y * Resolution.Z;
	const int64 IndexCapacity = FMath::Clamp<int64>(
		FMath::Min3<int64>(
			EstimateStampFootprintTexels(RadiusWorld, TexelSize, Resolution),
			DomainTexels,
			(int64)WorldVoxelPaint::PickWorkListTexelCap),
		1, (int64)WorldVoxelPaint::PickWorkListTexelCap);

	FGpuPickRequestRef NewRequest = MakeShared<FGpuPickRequest, ESPMode::ThreadSafe>();
	NewRequest->Source = Request;
	NewRequest->OnReady = MoveTemp(OnReady);
	NewRequest->WorldToTexel = Target->GetRigidPlacementFrame().ToInverseMatrixWithScale()
		* FTranslationMatrix(-StorageMin)
		* FScaleMatrix(FVector(1.0 / TexelSize.X, 1.0 / TexelSize.Y, 1.0 / TexelSize.Z));
	NewRequest->RayOrigin = Request.RayOrigin;
	NewRequest->RayDirection = RayDirection;
	NewRequest->Resolution = Resolution;
	NewRequest->BrickCounts = BrickCounts;
	NewRequest->BrushRadiusWorld = RadiusWorld;
	NewRequest->FlattenPct = Request.Brush.FlattenPct;
	NewRequest->AngleToleranceDeg = Request.Brush.AngleToleranceDeg;
	NewRequest->MinFlattenDepthWorld = (float)(0.5 * TexelSize.Size());
	NewRequest->BrushRadiusTexels = (float)(RadiusWorld / TexelSize.GetMin());
	NewRequest->IndexCapacity = (uint32)IndexCapacity;
	NewRequest->ReadbackNumBytes =
		(2u + (uint32)IndexCapacity + SeedTrailerLanes) * sizeof(uint32);
	NewRequest->WorldKey = World;

	TryIssueCoalesced();
	const int32 FreeSlot = FindFreeSlot();
	if (FreeSlot != INDEX_NONE && !CoalescedRequest.IsValid())
	{
		IssueRequest(NewRequest, FreeSlot);
		return;
	}

	if (CoalescedRequest.IsValid())
	{
		FGpuPickRequestRef Displaced = MoveTemp(CoalescedRequest);
		CoalescedRequest.Reset();
		FallBackToCpu(TEXT("Saturated"), Displaced->Source, MoveTemp(Displaced->OnReady));
	}
	CoalescedRequest = NewRequest;
	EnsureTicker();
}

void FVoxelPaintGpuPickProvider::CancelAndDrain(TFunction<void()> OnDrained)
{
	using namespace GpuPickProviderPrivate;

	checkf(IsInGameThread(), TEXT("Pick requests are game-thread entry points"));

	bDraining = true;
	DrainState->bArmed = true;
	DrainState->OnDrained = MoveTemp(OnDrained);

	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	CoalescedRequest.Reset();
	for (FGpuPickRequestRef& Request : SlotRequests)
	{
		if (Request.IsValid())
		{
			uint8 Expected = ClaimNone;
			Request->Claim.compare_exchange_strong(Expected, ClaimFallback);
			Request.Reset();
		}
	}

	TSharedPtr<FGpuPickDrainState, ESPMode::ThreadSafe> Drain = DrainState;
	if (bDelegateBound)
	{
		if (FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
		{
			++Drain->PendingFences;
			Coordinator->ClearPickPassDelegate([Drain]()
			{
				FGpuPickDrainState::ResolveFence(Drain);
			});
		}
		bDelegateBound = false;
	}

	++Drain->PendingFences;
	TSharedPtr<FGpuPickRenderState, ESPMode::ThreadSafe> State = RenderState;
	ENQUEUE_RENDER_COMMAND(WorldVoxelPaintPickDrainFence)(
		[State, Drain](FRHICommandListImmediate&)
		{
			State->Pending.Empty();
			FGpuPickDrainState::ResolveFence(Drain);
		});

	FGpuPickDrainState::MaybeFire(Drain);
}

bool FVoxelPaintGpuPickProvider::HasInFlightRequests() const
{
	if (CoalescedRequest.IsValid())
	{
		return true;
	}
	for (const FGpuPickRequestRef& Request : SlotRequests)
	{
		if (Request.IsValid())
		{
			return true;
		}
	}
	return false;
}

FName FVoxelPaintGpuPickProvider::GetLastPickSource() const
{
	return LastPickSource;
}

void FVoxelPaintGpuPickProvider::FallBackToCpu(const TCHAR* Reason,
	const FVoxelPaintStampRequest& Request, TFunction<void(FVoxelPaintGatherList&&)>&& OnReady)
{
	if (!bFallbackLoggedSinceGpuDelivery)
	{
		UE_LOG(LogWorldVoxelPaint, Log,
			TEXT("GPU pick unavailable (%s); stamp selection fell back to the CPU provider."),
			Reason);
		bFallbackLoggedSinceGpuDelivery = true;
	}
	LastPickSource = FName(TEXT("CPU"));
	CpuFallback.RequestStampSelection(Request, MoveTemp(OnReady));
}

int32 FVoxelPaintGpuPickProvider::FindFreeSlot() const
{
	for (int32 SlotIndex = 0; SlotIndex < SlotRequests.Num(); ++SlotIndex)
	{
		if (!SlotRequests[SlotIndex].IsValid())
		{
			return SlotIndex;
		}
	}
	return INDEX_NONE;
}

void FVoxelPaintGpuPickProvider::IssueRequest(const FGpuPickRequestRef& Request, int32 SlotIndex)
{
	Request->SlotIndex = SlotIndex;
	Request->IssueTimeSeconds = FPlatformTime::Seconds();
	SlotRequests[SlotIndex] = Request;
	EnsureDelegateBound();
	TSharedPtr<FGpuPickRenderState, ESPMode::ThreadSafe> State = RenderState;
	FGpuPickRequestRef Issued = Request;
	ENQUEUE_RENDER_COMMAND(WorldVoxelPaintPickIssue)(
		[State, Issued](FRHICommandListImmediate&)
		{
			State->Pending.Add(Issued);
		});
	EnsureTicker();
}

void FVoxelPaintGpuPickProvider::TryIssueCoalesced()
{
	if (!CoalescedRequest.IsValid())
	{
		return;
	}
	const int32 FreeSlot = FindFreeSlot();
	if (FreeSlot == INDEX_NONE)
	{
		return;
	}
	FGpuPickRequestRef Request = MoveTemp(CoalescedRequest);
	CoalescedRequest.Reset();
	IssueRequest(Request, FreeSlot);
}

void FVoxelPaintGpuPickProvider::EnsureDelegateBound()
{
	if (bDelegateBound)
	{
		return;
	}
	FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated();
	if (!Coordinator)
	{
		return;
	}
	TSharedPtr<FGpuPickRenderState, ESPMode::ThreadSafe> State = RenderState;
	Coordinator->SetPickPassDelegate(
		[State](FRDGBuilder& GraphBuilder, const FSceneView& View,
			TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
		{
			State->ServiceOnePending(GraphBuilder, View, SceneTextures);
		});
	bDelegateBound = true;
}

void FVoxelPaintGpuPickProvider::EnsureTicker()
{
	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FVoxelPaintGpuPickProvider::Tick), 0.0f);
	}
}

bool FVoxelPaintGpuPickProvider::Tick(float)
{
	using namespace GpuPickProviderPrivate;

	if (bDraining)
	{
		TickerHandle.Reset();
		return false;
	}

	TryIssueCoalesced();

	for (int32 SlotIndex = 0; SlotIndex < SlotRequests.Num(); ++SlotIndex)
	{
		FGpuPickRequestRef Request = SlotRequests[SlotIndex];
		if (!Request.IsValid())
		{
			continue;
		}
		FGpuPickRequest& State = *Request;
		if (State.bFailed.load() || State.bDataReady.load())
		{
			FinishSlot(SlotIndex);
		}
		else if (State.Claim.load() == ClaimNone
			&& FPlatformTime::Seconds() - State.IssueTimeSeconds > ConsumeDeadlineSeconds)
		{
			uint8 Expected = ClaimNone;
			if (State.Claim.compare_exchange_strong(Expected, ClaimFallback))
			{
				TSharedPtr<FGpuPickRenderState, ESPMode::ThreadSafe> RenderStateRef = RenderState;
				FGpuPickRequestRef Purged = Request;
				ENQUEUE_RENDER_COMMAND(WorldVoxelPaintPickPurge)(
					[RenderStateRef, Purged](FRHICommandListImmediate&)
					{
						RenderStateRef->Pending.Remove(Purged);
					});
				State.FailReason = TEXT("PassNotServiced");
				FinishSlot(SlotIndex);
			}
		}
		else if (State.bCopyEnqueued.load() && !State.bPollInFlight.exchange(true))
		{
			TSharedPtr<FGpuPickRenderState, ESPMode::ThreadSafe> RenderStateRef = RenderState;
			FGpuPickRequestRef Shared = Request;
			ENQUEUE_RENDER_COMMAND(WorldVoxelPaintPickReadbackPoll)(
				[RenderStateRef, Shared](FRHICommandListImmediate&)
				{
					FRHIGPUBufferReadback* Readback =
						RenderStateRef->Readbacks.IsValidIndex(Shared->SlotIndex)
							? RenderStateRef->Readbacks[Shared->SlotIndex].Get()
							: nullptr;
					if (!Readback)
					{
						Shared->FailReason = TEXT("ReadbackMissing");
						Shared->bFailed = true;
					}
					else if (Readback->IsReady())
					{
						if (void* Data = Readback->Lock(Shared->ReadbackNumBytes))
						{
							Shared->ReadbackData.SetNumUninitialized(
								(int32)(Shared->ReadbackNumBytes / sizeof(uint32)));
							FMemory::Memcpy(Shared->ReadbackData.GetData(), Data,
								Shared->ReadbackNumBytes);
							Readback->Unlock();
							Shared->bDataReady = true;
						}
						else
						{
							Shared->FailReason = TEXT("ReadbackLockFailed");
							Shared->bFailed = true;
						}
					}
					Shared->bPollInFlight = false;
				});
		}
	}

	if (!HasInFlightRequests())
	{
		TickerHandle.Reset();
		return false;
	}
	return true;
}

void FVoxelPaintGpuPickProvider::FinishSlot(int32 SlotIndex)
{
	using namespace GpuPickProviderPrivate;

	FGpuPickRequestRef Request = SlotRequests[SlotIndex];
	SlotRequests[SlotIndex].Reset();
	if (!Request.IsValid())
	{
		return;
	}
	if (Request->bDataReady.load() && Request->Claim.load() == ClaimGpu)
	{
		DeliverReadback(Request);
	}
	else
	{
		FallBackToCpu(Request->FailReason ? Request->FailReason : TEXT("GpuPickUnavailable"),
			Request->Source, MoveTemp(Request->OnReady));
	}
	TryIssueCoalesced();
}

void FVoxelPaintGpuPickProvider::DeliverReadback(const FGpuPickRequestRef& Request)
{
	using namespace GpuPickProviderPrivate;

	const uint32 IndexCapacity = Request->IndexCapacity;
	const TArray<uint32>& Data = Request->ReadbackData;
	if (Data.Num() != (int32)(2u + IndexCapacity + SeedTrailerLanes))
	{
		FallBackToCpu(TEXT("ReadbackTruncated"), Request->Source, MoveTemp(Request->OnReady));
		return;
	}
	if ((Data[1] & 1u) != 0u)
	{
		FallBackToCpu(TEXT("WorkListOverflow"), Request->Source, MoveTemp(Request->OnReady));
		return;
	}

	FVoxelPaintGatherList Out;
	const int32 TrailerBase = (int32)(2u + IndexCapacity);
	if (Data[TrailerBase + 3] != 0u)
	{
		const uint32 Count = FMath::Min(Data[0], IndexCapacity);
		TArray<uint32> TexelIndices;
		TexelIndices.Append(Data.GetData() + 2, (int32)Count);
		const FVector3f BrushCenterTexel(
			BitCast<float>(Data[TrailerBase + 0]),
			BitCast<float>(Data[TrailerBase + 1]),
			BitCast<float>(Data[TrailerBase + 2]));
		WorldVoxelPaint::BuildGatherListFromPick(Request->BrickCounts, MoveTemp(TexelIndices),
			BrushCenterTexel, Request->BrushRadiusTexels, Out);
	}
	LastPickSource = FName(TEXT("GPU"));
	bFallbackLoggedSinceGpuDelivery = false;
	Request->OnReady(MoveTemp(Out));
}
