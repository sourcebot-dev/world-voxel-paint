// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintDebugDraw.h"
#include "CanvasItem.h"
#include "Debug/DebugDrawService.h"
#include "Engine/Canvas.h"
#include "Engine/World.h"
#include "Math/Box.h"
#include "Math/ColorList.h"
#include "Math/Transform.h"
#include "Misc/CoreDelegates.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "ShowFlags.h"
#include "VoxelPaintCoordinator.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintWorldSubsystem.h"
#include "WorldVoxelPaintConstants.h"

static TCustomShowFlag<EShowFlagShippingValue::ForceDisabled> GVoxelPaintShowFlag(
	TEXT("WorldVoxelPaint"), false /*DefaultEnabled*/, SFG_Developer,
	NSLOCTEXT("WorldVoxelPaint", "ShowFlagName", "World Voxel Paint"));

namespace WorldVoxelPaint
{

namespace DebugDrawPrivate
{

void DrawProjectedLine(UCanvas* Canvas, const FVector& Start, const FVector& End, const FColor& Color)
{
	const FVector ProjectedStart = Canvas->Project(Start);
	const FVector ProjectedEnd = Canvas->Project(End);
	if (ProjectedStart.Z <= 0.0 || ProjectedEnd.Z <= 0.0)
	{
		return;
	}
	FCanvasLineItem LineItem(
		FVector2D(ProjectedStart.X, ProjectedStart.Y), FVector2D(ProjectedEnd.X, ProjectedEnd.Y));
	LineItem.SetColor(Color.ReinterpretAsLinear());
	Canvas->DrawItem(LineItem);
}

void DrawWireBoxEdges(UCanvas* Canvas, const FTransform& Frame, const FBox& LocalBox, const FColor& Color)
{
	const FVector Vertices[8] =
	{
		Frame.TransformPosition(FVector(LocalBox.Min.X, LocalBox.Min.Y, LocalBox.Min.Z)),
		Frame.TransformPosition(FVector(LocalBox.Min.X, LocalBox.Min.Y, LocalBox.Max.Z)),
		Frame.TransformPosition(FVector(LocalBox.Min.X, LocalBox.Max.Y, LocalBox.Min.Z)),
		Frame.TransformPosition(FVector(LocalBox.Min.X, LocalBox.Max.Y, LocalBox.Max.Z)),
		Frame.TransformPosition(FVector(LocalBox.Max.X, LocalBox.Min.Y, LocalBox.Min.Z)),
		Frame.TransformPosition(FVector(LocalBox.Max.X, LocalBox.Min.Y, LocalBox.Max.Z)),
		Frame.TransformPosition(FVector(LocalBox.Max.X, LocalBox.Max.Y, LocalBox.Min.Z)),
		Frame.TransformPosition(FVector(LocalBox.Max.X, LocalBox.Max.Y, LocalBox.Max.Z))
	};

	const FIntVector2 Edges[12] =
	{
		{ 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
		{ 0, 4 }, { 4, 6 }, { 6, 2 }, { 2, 0 },
		{ 1, 5 }, { 5, 7 }, { 7, 3 }, { 3, 1 }
	};

	for (const FIntVector2& Edge : Edges)
	{
		DrawProjectedLine(Canvas, Vertices[Edge.X], Vertices[Edge.Y], Color);
	}
}

} // namespace DebugDrawPrivate

void DrawVoxelPaintDebug(UCanvas* Canvas, APlayerController* PlayerController)
{
	if (!Canvas || !Canvas->SceneView || !Canvas->SceneView->Family || !Canvas->SceneView->Family->Scene)
	{
		return;
	}

	const UWorld* World = Canvas->SceneView->Family->Scene->GetWorld();
	if (const UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(World))
	{
		Subsystem->EnumerateFrontier([Canvas](const FVoxelPaintPlacementState& State)
		{
			if ((State.Flags & RecordFlag_Valid) == 0u)
			{
				return;
			}
			const FVector HalfExtent(State.LocalHalfExtent);
			DebugDrawPrivate::DrawWireBoxEdges(Canvas, FTransform(State.Rotation, State.Translation),
				FBox(-HalfExtent, HalfExtent), FColorList::Yellow);
		});
	}

	if (const FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
	{
		FBox WindowBounds(ForceInit);
		if (Coordinator->GetExpectedWindowBounds(WindowBounds))
		{
			DebugDrawPrivate::DrawWireBoxEdges(Canvas, FTransform::Identity, WindowBounds,
				FColorList::Cyan);
		}
	}
}

} // namespace WorldVoxelPaint

namespace
{

struct FVoxelPaintDebugDrawRegistrar
{
	FVoxelPaintDebugDrawRegistrar()
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([this]()
		{
			DrawServiceHandle = UDebugDrawService::Register(TEXT("WorldVoxelPaint"),
				FDebugDrawDelegate::CreateStatic(&WorldVoxelPaint::DrawVoxelPaintDebug));
		});
	}

	~FVoxelPaintDebugDrawRegistrar()
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		if (DrawServiceHandle.IsValid())
		{
			UDebugDrawService::Unregister(DrawServiceHandle);
		}
	}

	FDelegateHandle PostEngineInitHandle;
	FDelegateHandle DrawServiceHandle;
};

FVoxelPaintDebugDrawRegistrar GVoxelPaintDebugDrawRegistrar;

} // namespace
