// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintTools.h"

#include "Async/TaskGraphInterfaces.h"
#include "CollisionQueryParams.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/HitResult.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineDefines.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "PrimitiveDrawInterface.h"
#include "RenderingThread.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GameFramework/Actor.h"
#include "InteractiveToolManager.h"
#include "SceneView.h"
#include "ScopedTransaction.h"
#include "Selection.h"
#include "TextureResource.h"
#include "ToolContextInterfaces.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "VoxelPaintAuthority.h"
#include "VoxelPaintDetailLadder.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintTemplateRouting.h"
#include "VoxelPaintTopologyOps.h"
#include "VoxelPaintVolume.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "VoxelPaintWorldSubsystem.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "WorldVoxelPaintConstants.h"

#define LOCTEXT_NAMESPACE "WorldVoxelPaint"

namespace VoxelPaintToolsPrivate
{
	static UVoxelPaintVolumeComponent* FindPlacementInActors(const TArray<AActor*>& Actors)
	{
		for (AActor* Actor : Actors)
		{
			if (Actor)
			{
				if (UVoxelPaintVolumeComponent* Component = Actor->FindComponentByClass<UVoxelPaintVolumeComponent>())
				{
					return Component;
				}
			}
		}
		return nullptr;
	}

	static uint32 BuildChannelMask(bool bRed, bool bGreen, bool bBlue, bool bAlpha)
	{
		return (bRed ? 0x1u : 0u) | (bGreen ? 0x2u : 0u) | (bBlue ? 0x4u : 0u) | (bAlpha ? 0x8u : 0u);
	}

	// Half-extent, in pixels, of the per-volume select icon drawn by the Select tool (96px total).
	static constexpr float VolumeIconScreenHalfSizePx = 48.0f;
	static const TCHAR* VolumeIconTexturePath = TEXT("/Engine/EditorResources/S_TriggerBox.S_TriggerBox");

	// World-space sprite half-size that projects to a constant pixel half-size in both
	// perspective and ortho views (same scaling the engine uses for fixed-size editor sprites,
	// e.g. landscape spline control points).
	static float ComputeFixedScreenSizeSpriteExtent(const FSceneView& View, const FVector& WorldPosition, float PixelHalfSize)
	{
		const FMatrix& Projection = View.ViewMatrices.GetProjectionMatrix();
		const double ZoomFactor = FMath::Min(Projection.M[0][0], Projection.M[1][1]);
		const double ViewportWidth = View.UnscaledViewRect.Width();
		if (ZoomFactor <= 0.0 || ViewportWidth <= 0.0)
		{
			return 0.0f;
		}
		const double DepthW = View.WorldToScreen(WorldPosition).W;
		if (DepthW <= 0.0)
		{
			return 0.0f;
		}
		return static_cast<float>(DepthW * PixelHalfSize * 2.0 / (ViewportWidth * ZoomFactor));
	}
}

FVoxelPaintToolsContext& FVoxelPaintToolsContext::Get()
{
	static FVoxelPaintToolsContext Instance;
	return Instance;
}

IVoxelPaintPickProvider& FVoxelPaintToolsContext::GetPickProvider()
{
	static IConsoleVariable* const GpuPickingCVar =
		IConsoleManager::Get().FindConsoleVariable(TEXT("r.VoxelPaint.Editor.GpuPicking"));
	if (GpuPickingCVar && GpuPickingCVar->GetInt() != 0)
	{
		if (!GpuPickProvider)
		{
			GpuPickProvider = MakeUnique<FVoxelPaintGpuPickProvider>(PickProvider);
		}
		LastPickRoute = EPickRoute::Gpu;
		return *GpuPickProvider;
	}
	LastPickRoute = EPickRoute::Cpu;
	return PickProvider;
}

FName FVoxelPaintToolsContext::GetLastPickSource() const
{
	if (LastPickRoute == EPickRoute::Gpu && GpuPickProvider)
	{
		return GpuPickProvider->GetLastPickSource();
	}
	return LastPickRoute == EPickRoute::Cpu ? FName(TEXT("CPU")) : FName(TEXT("None"));
}

bool FVoxelPaintToolsContext::HasInFlightPickRequests() const
{
	return GpuPickProvider && GpuPickProvider->HasInFlightRequests();
}

void FVoxelPaintToolsContext::ShutdownGpuPickProvider()
{
	if (!GpuPickProvider)
	{
		return;
	}
	// Teardown-only drain: pending pick render commands and their game-thread deliveries must
	// resolve before the provider (and then the session surfaces) are destroyed.
	FlushRenderingCommands();
	TSharedRef<bool> Drained = MakeShared<bool>(false);
	GpuPickProvider->CancelAndDrain([Drained]()
	{
		*Drained = true;
	});
	if (!*Drained)
	{
		// CancelAndDrain enqueues fence render commands whose completion posts game-thread
		// tasks; they must execute before ProcessThreadUntilIdle can drain those tasks.
		FlushRenderingCommands();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}
	ensureMsgf(*Drained, TEXT("GPU pick provider did not drain at shutdown; destroying anyway."));
	GpuPickProvider.Reset();
	if (LastPickRoute == EPickRoute::Gpu)
	{
		LastPickRoute = EPickRoute::None;
	}
}

void FVoxelPaintToolsContext::SyncPipelineToSession()
{
	if (!bBound)
	{
		return;
	}
	const FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
	if (!Controller || !Controller->IsSessionOpen()
		|| Controller->GetSessionGeneration() != BoundSessionGeneration)
	{
		Pipeline.UnbindSession();
		bBound = false;
		BoundSessionGeneration = 0;
	}
}

bool FVoxelPaintToolsContext::EnsureSessionFor(UVoxelPaintVolumeComponent* Placement, FText& OutError)
{
	SyncPipelineToSession();

	if (!Placement || !Placement->HasValidVolumeData())
	{
		OutError = LOCTEXT("EnsureSessionNoPlacement",
			"Select a voxel paint volume with an assigned asset first.");
		return false;
	}

	const UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(Placement->GetWorld());
	if (!Subsystem || Subsystem->GetFrontierPosition(Placement) == INDEX_NONE)
	{
		OutError = LOCTEXT("EnsureSessionFrontierFull",
			"Frontier full - this placement is not rendering; free a slot or paint a rendered copy.");
		return false;
	}

	FVoxelPaintSessionController& Controller = FVoxelPaintSessionController::Get();
	if (Controller.IsSessionOpen())
	{
		if (Controller.GetActiveAsset() != Placement->ResolveActiveVariant())
		{
			OutError = FText::Format(LOCTEXT("EnsureSessionOtherAsset",
				"A paint session is already open on '{0}' (one session at a time). Finish, Discard, or End Anyway first."),
				FText::FromString(GetNameSafe(Controller.GetActiveAsset())));
			return false;
		}
	}
	else if (Controller.OpenSession(Placement, OutError)
		!= WorldVoxelPaint::EVoxelPaintSessionAdmissionVerdict::Admitted)
	{
		return false;
	}

	if (!bBound || BoundSessionGeneration != Controller.GetSessionGeneration())
	{
		UVoxelPaintVolumeData* Asset = Controller.GetActiveAsset();
		FVoxelPaintStrokePipelineSessionBindings Bindings;
		Bindings.Controller = &Controller;
		Bindings.Resources = Controller.GetSessionResources();
		Bindings.Transfers = Controller.GetTransferEngine();
		Bindings.CpuState = Controller.GetSessionCpuState();
		Bindings.Ledger = Controller.GetBudgetLedger();
		Bindings.BrickCounts = Asset ? Asset->GetBrickCounts() : FIntVector::ZeroValue;
		Bindings.TexelSize = Asset ? Asset->GetTexelSize() : FVector::ZeroVector;
		if (!Bindings.IsValid())
		{
			OutError = LOCTEXT("EnsureSessionBindFailed",
				"The open session's surfaces are unavailable; Finish or Discard the session and retry.");
			return false;
		}
		Pipeline.BindSession(Bindings);
		bBound = true;
		BoundSessionGeneration = Controller.GetSessionGeneration();
	}
	return true;
}

UVoxelPaintVolumeComponent* FVoxelPaintToolsContext::FindSelectedPlacement()
{
	if (!GEditor)
	{
		return nullptr;
	}
	USelection* Selection = GEditor->GetSelectedActors();
	if (!Selection)
	{
		return nullptr;
	}
	for (FSelectionIterator It(*Selection); It; ++It)
	{
		if (AActor* Actor = Cast<AActor>(*It))
		{
			if (UVoxelPaintVolumeComponent* Component = Actor->FindComponentByClass<UVoxelPaintVolumeComponent>())
			{
				return Component;
			}
		}
	}
	return nullptr;
}

void FVoxelPaintToolsContext::NotifyUser(const FText& Message)
{
	if (FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = 6.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
	UE_LOG(LogWorldVoxelPaint, Display, TEXT("%s"), *Message.ToString());
}

uint32 UVoxelPaintBrushToolProperties::GetChannelMask() const
{
	return VoxelPaintToolsPrivate::BuildChannelMask(bWriteRed, bWriteGreen, bWriteBlue, bWriteAlpha);
}

void UVoxelPaintWholeVolumeToolProperties::Apply()
{
	if (UVoxelPaintWholeVolumeTool* Tool = OwnerTool.Get())
	{
		Tool->ExecuteOp();
	}
}

uint32 UVoxelPaintWholeVolumeToolProperties::GetChannelMask() const
{
	return VoxelPaintToolsPrivate::BuildChannelMask(bWriteRed, bWriteGreen, bWriteBlue, bWriteAlpha);
}

void UVoxelPaintCreateVolumeToolProperties::CreateVolume()
{
	if (UVoxelPaintCreateVolumeTool* Tool = OwnerTool.Get())
	{
		Tool->CreateVolumeFromSelection();
	}
}

UInteractiveTool* UVoxelPaintSelectToolBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	UVoxelPaintSelectTool* Tool = NewObject<UVoxelPaintSelectTool>(SceneState.ToolManager);
	Tool->SetTargetWorld(SceneState.World);
	return Tool;
}

void UVoxelPaintSelectTool::Setup()
{
	Super::Setup();
	VolumeIconTexture = LoadObject<UTexture2D>(nullptr, VoxelPaintToolsPrivate::VolumeIconTexturePath);
}

// Draws a fixed-screen-size icon over every paint volume origin while the Select tool is active.
// During hit-proxy passes each sprite is wrapped in an HActor proxy, so the icons resolve through
// the same FindVolumeActorAtClick path as clicks on the volumes' own geometry.
void UVoxelPaintSelectTool::Render(IToolsContextRenderAPI* RenderAPI)
{
	Super::Render(RenderAPI);

	FPrimitiveDrawInterface* PDI = RenderAPI ? RenderAPI->GetPrimitiveDrawInterface() : nullptr;
	const FSceneView* View = RenderAPI ? RenderAPI->GetSceneView() : nullptr;
	UWorld* World = TargetWorld.Get();
	const FTexture* IconResource = VolumeIconTexture ? VolumeIconTexture->GetResource() : nullptr;
	if (!PDI || !View || !World || !IconResource)
	{
		return;
	}

	const bool bHitTesting = PDI->IsHitTesting();
	const FLinearColor SelectedTint = GEngine ? GEngine->GetSelectedMaterialColor() : FLinearColor::White;
	for (TObjectIterator<UVoxelPaintVolumeComponent> It; It; ++It)
	{
		UVoxelPaintVolumeComponent* Volume = *It;
		if (!IsValid(Volume) || Volume->IsTemplate() || Volume->GetWorld() != World)
		{
			continue;
		}
		AActor* Owner = Volume->GetOwner();
		if (!Owner || Owner->IsHiddenEd())
		{
			continue;
		}
		const FVector Origin = Volume->GetRigidPlacementFrame().GetTranslation();
		const float SpriteExtent = VoxelPaintToolsPrivate::ComputeFixedScreenSizeSpriteExtent(
			*View, Origin, VoxelPaintToolsPrivate::VolumeIconScreenHalfSizePx);
		if (SpriteExtent <= 0.0f)
		{
			continue;
		}
		if (bHitTesting)
		{
			PDI->SetHitProxy(new HActor(Owner, nullptr, HPP_Foreground));
		}
		PDI->DrawSprite(Origin, SpriteExtent, SpriteExtent, IconResource,
			Owner->IsSelected() ? SelectedTint : FLinearColor::White, SDPG_Foreground,
			0.0f, 0.0f, 0.0f, 0.0f);
		if (bHitTesting)
		{
			PDI->SetHitProxy(nullptr);
		}
	}
}

FInputRayHit UVoxelPaintSelectTool::IsHitByClick(const FInputDeviceRay& ClickPos)
{
	return FindVolumeActorAtClick(ClickPos) ? FInputRayHit(0.0f) : FInputRayHit();
}

void UVoxelPaintSelectTool::OnClicked(const FInputDeviceRay& ClickPos)
{
	AActor* VolumeActor = FindVolumeActorAtClick(ClickPos);
	if (!VolumeActor)
	{
		return;
	}
	GetToolManager()->BeginUndoTransaction(LOCTEXT("SelectVolumeTransaction", "Select Voxel Paint Volume"));
	FSelectedObjectsChangeList NewSelection;
	NewSelection.ModificationType = ESelectedObjectsModificationType::Replace;
	NewSelection.Actors.Add(VolumeActor);
	GetToolManager()->RequestSelectionChange(NewSelection);
	GetToolManager()->EndUndoTransaction();
}

AActor* UVoxelPaintSelectTool::FindVolumeActorAtClick(const FInputDeviceRay& ClickPos) const
{
	UInteractiveToolManager* ToolManager = GetToolManager();
	const IToolsContextQueriesAPI* Queries = ToolManager ? ToolManager->GetContextQueriesAPI() : nullptr;
	FViewport* Viewport = Queries ? Queries->GetFocusedViewport() : nullptr;
	if (!Viewport)
	{
		return nullptr;
	}
	HHitProxy* Proxy = Viewport->GetHitProxy(
		(int32)ClickPos.ScreenPosition.X, (int32)ClickPos.ScreenPosition.Y);
	const HActor* ActorProxy = HitProxyCast<HActor>(Proxy);
	AActor* Actor = ActorProxy ? ActorProxy->Actor : nullptr;
	return (Actor && Actor->FindComponentByClass<UVoxelPaintVolumeComponent>()) ? Actor : nullptr;
}

bool UVoxelPaintBrushToolBuilder::CanBuildTool(const FToolBuilderState& SceneState) const
{
	return VoxelPaintToolsPrivate::FindPlacementInActors(SceneState.SelectedActors) != nullptr;
}

UInteractiveTool* UVoxelPaintBrushToolBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	UVoxelPaintBrushTool* Tool = NewObject<UVoxelPaintBrushTool>(SceneState.ToolManager);
	Tool->SetWorld(SceneState.World);
	Tool->SetStartInEraseMode(bEraseMode);
	return Tool;
}

UVoxelPaintBrushTool::UVoxelPaintBrushTool()
{
	PropertyClass = UBrushBaseProperties::StaticClass();
}

void UVoxelPaintBrushTool::Setup()
{
	Super::Setup();
	VoxelProperties = NewObject<UVoxelPaintBrushToolProperties>(this, TEXT("VoxelBrush"));
	VoxelProperties->RestoreProperties(this);
	VoxelProperties->bErase = bStartInEraseMode;
	AddToolPropertySource(VoxelProperties);
}

void UVoxelPaintBrushTool::Shutdown(EToolShutdownType ShutdownType)
{
	bInDrag = false;
	bStampPending = false;
	bCloseStrokePending = false;
	++StampGeneration;
	FVoxelPaintStrokePipeline& Pipeline = FVoxelPaintToolsContext::Get().GetPipeline();
	if (Pipeline.IsStrokeOpen())
	{
		Pipeline.RequestCloseStroke();
	}
	if (VoxelProperties)
	{
		VoxelProperties->SaveProperties(this);
	}
	Super::Shutdown(ShutdownType);
}

void UVoxelPaintBrushTool::OnTick(float DeltaTime)
{
	Super::OnTick(DeltaTime);
	FVoxelPaintToolsContext& Context = FVoxelPaintToolsContext::Get();
	Context.SyncPipelineToSession();
	if (bCloseStrokePending && !Context.HasInFlightPickRequests())
	{
		CloseStrokeWhenDrained();
	}
	if (bStampPending && !bCloseStrokePending)
	{
		bStampPending = false;
		ApplyPendingStamp();
		if (bInDrag && !bStrokeDeniedThisDrag)
		{
			bStampPending = true;
		}
	}
}

bool UVoxelPaintBrushTool::HitTest(const FRay& Ray, FHitResult& OutHit)
{
	UWorld* World = GetTargetWorld();
	if (!World)
	{
		return false;
	}
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VoxelPaintBrushHitTest), /*bInTraceComplex=*/true);
	return World->LineTraceSingleByChannel(OutHit, Ray.Origin,
		Ray.Origin + Ray.Direction * HALF_WORLD_MAX, ECC_Visibility, QueryParams)
		&& OutHit.bBlockingHit;
}

void UVoxelPaintBrushTool::OnBeginDrag(const FRay& Ray)
{
	Super::OnBeginDrag(Ray);
	bStrokeDeniedThisDrag = false;
	FHitResult Hit;
	if (HitTest(Ray, Hit))
	{
		bInDrag = true;
		PendingStampRay = Ray;
		bStampPending = true;
	}
}

void UVoxelPaintBrushTool::OnUpdateDrag(const FRay& Ray)
{
	Super::OnUpdateDrag(Ray);
	if (bInDrag)
	{
		PendingStampRay = Ray;
		bStampPending = true;
	}
}

void UVoxelPaintBrushTool::OnEndDrag(const FRay& Ray)
{
	Super::OnEndDrag(Ray);
	bInDrag = false;
	bStampPending = false;
	FVoxelPaintToolsContext& Context = FVoxelPaintToolsContext::Get();
	if (Context.GetPipeline().IsStrokeOpen())
	{
		bCloseStrokePending = true;
		if (!Context.HasInFlightPickRequests())
		{
			CloseStrokeWhenDrained();
		}
	}
}

void UVoxelPaintBrushTool::OnCancelDrag()
{
	Super::OnCancelDrag();
	bInDrag = false;
	bStampPending = false;
	bCloseStrokePending = false;
	++StampGeneration;
	FVoxelPaintStrokePipeline& Pipeline = FVoxelPaintToolsContext::Get().GetPipeline();
	if (Pipeline.IsStrokeOpen())
	{
		Pipeline.CancelStroke();
	}
}

double UVoxelPaintBrushTool::EstimateMaximumTargetDimension()
{
	if (const UVoxelPaintVolumeComponent* Placement = FVoxelPaintToolsContext::FindSelectedPlacement())
	{
		return 2.0 * Placement->GetEffectiveLocalHalfExtent().GetMax();
	}
	return 1000.0;
}

void UVoxelPaintBrushTool::ApplyPendingStamp()
{
	UVoxelPaintVolumeComponent* TargetPlacement = FVoxelPaintToolsContext::FindSelectedPlacement();
	FVoxelPaintToolsContext& Context = FVoxelPaintToolsContext::Get();

	FText Error;
	if (!Context.EnsureSessionFor(TargetPlacement, Error))
	{
		DenyStrokeOnce(Error);
		return;
	}

	FVoxelPaintStrokePipeline& Pipeline = Context.GetPipeline();
	const EVoxelPaintStrokeOp Op = VoxelProperties->bErase
		? EVoxelPaintStrokeOp::Erase : EVoxelPaintStrokeOp::Paint;
	if (!Pipeline.IsStrokeOpen())
	{
		// The publication gate admits new strokes only: it requires the previous
		// stroke's close to be resolved, so running it on mid-stroke samples
		// would deny the stroke it is part of.
		const UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(TargetPlacement->GetWorld());
		FVoxelPaintPlacementState PlacementState;
		FText GateWhyNot = LOCTEXT("StampGatePlacementUnpublished",
			"Waiting for this placement's paint binding to publish.");
		if (!Subsystem || !Subsystem->GetCachedPlacementState(TargetPlacement, PlacementState)
			|| !FVoxelPaintSessionController::Get().CanMutateNow(PlacementState.Identity, GateWhyNot))
		{
			DenyStrokeOnce(GateWhyNot);
			return;
		}

		FText WhyNot;
		if (!Pipeline.BeginStroke(Op, MakeBrushParams(), WhyNot))
		{
			DenyStrokeOnce(WhyNot);
			return;
		}
	}

	FVoxelPaintStampRequest Request;
	Request.World = TargetPlacement->GetWorld();
	Request.RayOrigin = PendingStampRay.Origin;
	Request.RayDirection = PendingStampRay.Direction;
	Request.Brush = MakeBrushParams();
	Request.Target = TargetPlacement;

	FVoxelPaintStrokePipeline* PipelinePtr = &Pipeline;
	TWeakObjectPtr<UVoxelPaintBrushTool> WeakThis(this);
	Context.GetPickProvider().RequestStampSelection(Request,
		[PipelinePtr, WeakThis, Generation = StampGeneration](FVoxelPaintGatherList&& Gather)
		{
			const UVoxelPaintBrushTool* Tool = WeakThis.Get();
			if (!Tool || Tool->StampGeneration != Generation)
			{
				return;
			}
			if (!Gather.IsEmpty() && PipelinePtr->IsStrokeOpen())
			{
				PipelinePtr->ApplyStamp(MoveTemp(Gather));
			}
		});
}

void UVoxelPaintBrushTool::DenyStrokeOnce(const FText& Message)
{
	if (!bStrokeDeniedThisDrag)
	{
		bStrokeDeniedThisDrag = true;
		FVoxelPaintToolsContext::NotifyUser(Message);
	}
}

void UVoxelPaintBrushTool::CloseStrokeWhenDrained()
{
	bCloseStrokePending = false;
	++StampGeneration;
	FVoxelPaintStrokePipeline& Pipeline = FVoxelPaintToolsContext::Get().GetPipeline();
	if (Pipeline.IsStrokeOpen())
	{
		Pipeline.RequestCloseStroke();
	}
}

FVoxelPaintBrushParams UVoxelPaintBrushTool::MakeBrushParams() const
{
	FVoxelPaintBrushParams Params;
	Params.RadiusWorld = (float)GetCurrentBrushRadius();
	Params.Strength = BrushProperties ? BrushProperties->BrushStrength : 1.0f;
	Params.FalloffHotspot = BrushProperties ? 1.0f - BrushProperties->BrushFalloffAmount : 0.5f;
	Params.FlattenPct = VoxelProperties->FlattenPct;
	Params.AngleToleranceDeg = VoxelProperties->AngleToleranceDeg;
	Params.Color = VoxelProperties->Color;
	Params.ChannelMask = VoxelProperties->GetChannelMask();
	return Params;
}

bool UVoxelPaintWholeVolumeToolBuilder::CanBuildTool(const FToolBuilderState& SceneState) const
{
	return VoxelPaintToolsPrivate::FindPlacementInActors(SceneState.SelectedActors) != nullptr;
}

UInteractiveTool* UVoxelPaintWholeVolumeToolBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	UVoxelPaintWholeVolumeTool* Tool = NewObject<UVoxelPaintWholeVolumeTool>(SceneState.ToolManager);
	Tool->SetOp(Op);
	return Tool;
}

void UVoxelPaintWholeVolumeTool::Setup()
{
	Super::Setup();
	Properties = NewObject<UVoxelPaintWholeVolumeToolProperties>(this, TEXT("WholeVolume"));
	Properties->RestoreProperties(this);
	Properties->OwnerTool = this;
	Properties->bShowColor = Op == EVoxelPaintStrokeOp::Flood;
	Properties->bShowSwapChannels = Op == EVoxelPaintStrokeOp::Swap;
	AddToolPropertySource(Properties);
}

void UVoxelPaintWholeVolumeTool::Shutdown(EToolShutdownType ShutdownType)
{
	if (Properties)
	{
		Properties->SaveProperties(this);
	}
	Super::Shutdown(ShutdownType);
}

void UVoxelPaintWholeVolumeTool::ExecuteOp()
{
	UVoxelPaintVolumeComponent* Placement = FVoxelPaintToolsContext::FindSelectedPlacement();
	FVoxelPaintToolsContext& Context = FVoxelPaintToolsContext::Get();

	FText Error;
	if (!Context.EnsureSessionFor(Placement, Error))
	{
		FVoxelPaintToolsContext::NotifyUser(Error);
		return;
	}

	const UVoxelPaintVolumeData* Asset = FVoxelPaintSessionController::Get().GetActiveAsset();
	if (!Asset)
	{
		return;
	}

	FVoxelPaintBrushParams Params;
	Params.Strength = Properties->Strength;
	Params.FalloffHotspot = 1.0f;
	Params.Color = Properties->Color;
	Params.ChannelMask = Properties->GetChannelMask();
	Params.SwapSrcChannel = (uint8)Properties->SwapSourceChannel;
	Params.SwapDstChannel = (uint8)Properties->SwapTargetChannel;

	FVoxelPaintStrokePipeline& Pipeline = Context.GetPipeline();
	FText WhyNot;
	if (!Pipeline.BeginStroke(Op, Params, WhyNot))
	{
		FVoxelPaintToolsContext::NotifyUser(WhyNot);
		return;
	}

	FVoxelPaintGatherList Gather;
	FVoxelPaintStrokePipeline::BuildWholeVolumeGatherList(Asset->GetBrickCounts(), Gather);
	Pipeline.ApplyStamp(MoveTemp(Gather));
	Pipeline.RequestCloseStroke();

	GetToolManager()->DisplayMessage(
		LOCTEXT("WholeVolumeApplied", "Whole-volume operation applied."),
		EToolMessageLevel::UserNotification);
}

bool UVoxelPaintCreateVolumeToolBuilder::CanBuildTool(const FToolBuilderState& SceneState) const
{
	for (AActor* Actor : SceneState.SelectedActors)
	{
		if (Actor && !Actor->FindComponentByClass<UVoxelPaintVolumeComponent>())
		{
			return true;
		}
	}
	return false;
}

UInteractiveTool* UVoxelPaintCreateVolumeToolBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	UVoxelPaintCreateVolumeTool* Tool = NewObject<UVoxelPaintCreateVolumeTool>(SceneState.ToolManager);
	Tool->SetTargetWorld(SceneState.World);
	return Tool;
}

void UVoxelPaintCreateVolumeTool::Setup()
{
	Super::Setup();
	Properties = NewObject<UVoxelPaintCreateVolumeToolProperties>(this, TEXT("CreateVolume"));
	Properties->OwnerTool = this;
	const FBox Bounds = ComputeSelectionBounds();
	if (Bounds.IsValid)
	{
		Properties->BoundsCenter = Bounds.GetCenter();
		Properties->BoundsExtent = Bounds.GetExtent();
		Properties->Status = TEXT("Ready: Create Volume fits these bounds.");
	}
	else
	{
		Properties->Status = TEXT("Select one or more actors with valid bounds.");
	}
	AddToolPropertySource(Properties);
}

void UVoxelPaintCreateVolumeTool::CreateVolumeFromSelection()
{
	using namespace WorldVoxelPaint;

	UWorld* World = TargetWorld.Get();
	const FBox Bounds = ComputeSelectionBounds();
	if (!World || !Bounds.IsValid)
	{
		FVoxelPaintToolsContext::NotifyUser(LOCTEXT("CreateVolumeNoBounds",
			"Create Volume From Selection needs selected actors with valid bounds."));
		return;
	}

	AVoxelPaintVolume* Volume = nullptr;
	{
		FScopedTransaction Transaction(
			LOCTEXT("CreateVolumeTransaction", "Create Voxel Paint Volume From Selection"));

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags = RF_Transactional;
		Volume = World->SpawnActor<AVoxelPaintVolume>(Bounds.GetCenter(), FRotator::ZeroRotator, SpawnParams);
		if (!Volume)
		{
			FVoxelPaintToolsContext::NotifyUser(LOCTEXT("CreateVolumeSpawnFailed",
				"Failed to spawn the voxel paint volume actor."));
			return;
		}

		UVoxelPaintVolumeData* Asset = CreateVolumeDataForPlacement(Volume);
		UVoxelPaintVolumeComponent* Component = Volume->GetVolumeComponent();
		if (!Asset || !Component)
		{
			FVoxelPaintToolsContext::NotifyUser(LOCTEXT("CreateVolumeAssetFailed",
				"Failed to create the voxel paint volume asset."));
			return;
		}

		Component->CapturePreEditPoolState();
		Component->Modify();
		Component->VolumeVariants.SetNum(1);
		Component->VolumeVariants[0].Variant = Asset;
		FPropertyChangedEvent ChangedEvent(UVoxelPaintVolumeComponent::StaticClass()->FindPropertyByName(
			GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VolumeVariants)));
		Component->PostEditChangeProperty(ChangedEvent);

		const FVoxelPaintLadderClamps Clamps = BuildLadderClamps(World);
		FVoxelPaintLadderState State;
		State.BrickCounts = Asset->GetBrickCounts();
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			State.HalfExtent[Axis] = FMath::Max(Bounds.GetExtent()[Axis],
				0.5 * Clamps.MinVoxelSize * UniqueBrickSize);
		}
		FString LadderError;
		if (!ValidateLadderState(State, Clamps, LadderError))
		{
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				SelectRungForTexelSize(State, Axis,
					FMath::Max(Clamps.MinVoxelSize, State.GetTexelSize()[Axis]), Clamps);
			}
		}
		FText CommitError;
		UVoxelPaintVolumeComponent* ExpectationTarget = ResolveExpectationWriteTarget(Component, CommitError);
		if (!ExpectationTarget || !CommitPreInitTopologyEdit(Asset, Volume, ExpectationTarget, State, CommitError))
		{
			FVoxelPaintToolsContext::NotifyUser(CommitError);
		}
	}

	FSelectedObjectsChangeList NewSelection;
	NewSelection.ModificationType = ESelectedObjectsModificationType::Replace;
	NewSelection.Actors.Add(Volume);
	GetToolManager()->RequestSelectionChange(NewSelection);

	Properties->Status = FString::Printf(TEXT("Created '%s'."), *Volume->GetActorNameOrLabel());
}

FBox UVoxelPaintCreateVolumeTool::ComputeSelectionBounds() const
{
	FBox Bounds(ForceInit);
	if (!GEditor)
	{
		return Bounds;
	}
	USelection* Selection = GEditor->GetSelectedActors();
	if (!Selection)
	{
		return Bounds;
	}
	for (FSelectionIterator It(*Selection); It; ++It)
	{
		AActor* Actor = Cast<AActor>(*It);
		if (!Actor || Actor->FindComponentByClass<UVoxelPaintVolumeComponent>())
		{
			continue;
		}
		Bounds += Actor->GetComponentsBoundingBox(/*bNonColliding=*/true, /*bIncludeFromChildActors=*/false);
	}
	return Bounds;
}

#undef LOCTEXT_NAMESPACE
