// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintVolumeVisualizer.h"

#include "CanvasTypes.h"
#include "Components/ActorComponent.h"
#include "Engine/Engine.h"
#include "Engine/EngineTypes.h"
#include "Engine/Font.h"
#include "GameFramework/Actor.h"
#include "PrimitiveDrawInterface.h"
#include "PrimitiveDrawingUtils.h"
#include "SceneView.h"
#include "Templates/Casts.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintVolumeVisualizerLocals
{

const FLinearColor BoxColor(0.1f, 0.9f, 0.25f);
const FLinearColor LatticeColor(0.03f, 0.35f, 0.1f);
const FLinearColor VoxelTickColor(0.1f, 0.75f, 0.85f);
constexpr double BrickTickScale = 0.1;
constexpr double VoxelTickScale = 0.05;

FIntVector GetLiveBrickCounts(const UVoxelPaintVolumeComponent* Volume)
{
	FIntVector Bricks = Volume->GetEffectiveResolution() / WorldVoxelPaint::UniqueBrickSize;
	Bricks.X = FMath::Max(Bricks.X, 1);
	Bricks.Y = FMath::Max(Bricks.Y, 1);
	Bricks.Z = FMath::Max(Bricks.Z, 1);
	return Bricks;
}

FVector FindNearestCornerSigns(const FTransform& Frame, const FVector& HalfExtent, const FSceneView* View)
{
	FVector Signs(1.0, 1.0, 1.0);
	if (!View)
	{
		return Signs;
	}
	const FVector ViewOrigin = View->ViewMatrices.GetViewOrigin();
	double BestDistSquared = TNumericLimits<double>::Max();
	for (int32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
	{
		const FVector CandidateSigns(
			(CornerIndex & 1) ? 1.0 : -1.0,
			(CornerIndex & 2) ? 1.0 : -1.0,
			(CornerIndex & 4) ? 1.0 : -1.0);
		const FVector WorldCorner = Frame.TransformPosition(CandidateSigns * HalfExtent);
		const double DistSquared = FVector::DistSquared(WorldCorner, ViewOrigin);
		if (DistSquared < BestDistSquared)
		{
			BestDistSquared = DistSquared;
			Signs = CandidateSigns;
		}
	}
	return Signs;
}

void DrawLatticePlanes(FPrimitiveDrawInterface* PDI, const FTransform& Frame, const FVector& HalfExtent,
	const FIntVector& Bricks)
{
	const double SignsU[4] = { -1.0, 1.0, 1.0, -1.0 };
	const double SignsV[4] = { -1.0, -1.0, 1.0, 1.0 };
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const int32 AxisU = (Axis + 1) % 3;
		const int32 AxisV = (Axis + 2) % 3;
		const double Pitch = (2.0 * HalfExtent[Axis]) / Bricks[Axis];
		for (int32 PlaneIndex = 1; PlaneIndex < Bricks[Axis]; ++PlaneIndex)
		{
			const double PlaneCoord = -HalfExtent[Axis] + Pitch * PlaneIndex;
			FVector Corners[4];
			for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
			{
				FVector LocalCorner;
				LocalCorner[Axis] = PlaneCoord;
				LocalCorner[AxisU] = SignsU[CornerIndex] * HalfExtent[AxisU];
				LocalCorner[AxisV] = SignsV[CornerIndex] * HalfExtent[AxisV];
				Corners[CornerIndex] = Frame.TransformPosition(LocalCorner);
			}
			for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
			{
				PDI->DrawLine(Corners[CornerIndex], Corners[(CornerIndex + 1) % 4], LatticeColor,
					SDPG_World);
			}
		}
	}
}

int32 DrawEdgeTicks(FPrimitiveDrawInterface* PDI, const FTransform& Frame, const FVector& HalfExtent,
	const FVector& CornerSigns, int32 Axis, int32 BoundaryCount, int32 Stride, double TickScale,
	const FLinearColor& Color)
{
	if (BoundaryCount <= 0 || Stride <= 0)
	{
		return 0;
	}
	const int32 PerpAxis = (Axis + 1) % 3;
	const double Pitch = (2.0 * HalfExtent[Axis]) / (BoundaryCount + 1);
	FVector AxisDir = FVector::ZeroVector;
	AxisDir[Axis] = -CornerSigns[Axis];
	FVector TickDir = FVector::ZeroVector;
	TickDir[PerpAxis] = -CornerSigns[PerpAxis];
	const double TickLength = TickScale * HalfExtent[PerpAxis];
	const FVector LocalStart = CornerSigns * HalfExtent;
	int32 Drawn = 0;
	for (int32 BoundaryIndex = Stride; BoundaryIndex <= BoundaryCount; BoundaryIndex += Stride)
	{
		const FVector LocalPos = LocalStart + AxisDir * (Pitch * BoundaryIndex);
		PDI->DrawLine(Frame.TransformPosition(LocalPos),
			Frame.TransformPosition(LocalPos + TickDir * TickLength), Color, SDPG_World);
		++Drawn;
	}
	return Drawn;
}

} // namespace VoxelPaintVolumeVisualizerLocals

void FVoxelPaintVolumeVisualizer::DrawVisualization(const UActorComponent* Component,
	const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	using namespace VoxelPaintVolumeVisualizerLocals;

	const UVoxelPaintVolumeComponent* Volume = Cast<const UVoxelPaintVolumeComponent>(Component);
	if (!Volume || !PDI)
	{
		return;
	}

	const FTransform Frame = Volume->GetRigidPlacementFrame();
	const FVector HalfExtent =
		Volume->GetEffectiveLocalHalfExtent().ComponentMax(FVector(UE_DOUBLE_KINDA_SMALL_NUMBER));
	const FIntVector Bricks = GetLiveBrickCounts(Volume);

	DrawOrientedWireBox(PDI, Frame.GetTranslation(), Frame.GetUnitAxis(EAxis::X),
		Frame.GetUnitAxis(EAxis::Y), Frame.GetUnitAxis(EAxis::Z), HalfExtent, BoxColor, SDPG_World);

	const FVector CornerSigns = FindNearestCornerSigns(Frame, HalfExtent, View);

	int32 RemainingLines = WorldVoxelPaint::MaxVisualizerGridLines;
	const int64 LatticeLines =
		4ll * ((int64)(Bricks.X - 1) + (int64)(Bricks.Y - 1) + (int64)(Bricks.Z - 1));
	if (LatticeLines <= (int64)RemainingLines)
	{
		DrawLatticePlanes(PDI, Frame, HalfExtent, Bricks);
		RemainingLines -= (int32)LatticeLines;
	}
	else
	{
		const int32 PerAxisAllowance = RemainingLines / 3;
		int32 TicksDrawn = 0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const int32 BoundaryCount = Bricks[Axis] - 1;
			if (BoundaryCount <= 0 || PerAxisAllowance <= 0)
			{
				continue;
			}
			const int32 Stride = FMath::DivideAndRoundUp(BoundaryCount, PerAxisAllowance);
			TicksDrawn += DrawEdgeTicks(PDI, Frame, HalfExtent, CornerSigns, Axis, BoundaryCount,
				Stride, BrickTickScale, BoxColor);
		}
		RemainingLines -= TicksDrawn;
	}

	int64 VoxelTickLines = 0;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		VoxelTickLines += (int64)Bricks[Axis] * WorldVoxelPaint::UniqueBrickSize - 1;
	}
	if (VoxelTickLines > 0 && VoxelTickLines <= (int64)RemainingLines)
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			DrawEdgeTicks(PDI, Frame, HalfExtent, CornerSigns, Axis,
				Bricks[Axis] * WorldVoxelPaint::UniqueBrickSize - 1, 1, VoxelTickScale,
				VoxelTickColor);
		}
	}
}

void FVoxelPaintVolumeVisualizer::DrawVisualizationHUD(const UActorComponent* Component,
	const FViewport* Viewport, const FSceneView* View, FCanvas* Canvas)
{
	using namespace VoxelPaintVolumeVisualizerLocals;

	const UVoxelPaintVolumeComponent* Volume = Cast<const UVoxelPaintVolumeComponent>(Component);
	if (!Volume || !Canvas || !GEngine)
	{
		return;
	}

	const UVoxelPaintVolumeData* Asset = Volume->ResolveActiveVariant();
	const FVector HalfExtent = Volume->GetEffectiveLocalHalfExtent();
	const FIntVector Bricks = GetLiveBrickCounts(Volume);
	const FVector TexelSize =
		(2.0 * HalfExtent) / FVector(Bricks * WorldVoxelPaint::UniqueBrickSize);
	const int64 BrickProduct = (int64)Bricks.X * Bricks.Y * Bricks.Z;
	const AActor* Owner = Volume->GetOwner();

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("Asset: %s"), Asset ? *Asset->GetName() : TEXT("(none)")));
	const int32 VariantSlot = Volume->GetResolvedVariantIndex();
	if (VariantSlot != INDEX_NONE)
	{
		Lines.Add(FString::Printf(TEXT("Variant: %d/%d"), VariantSlot + 1,
			Volume->VolumeVariants.Num()));
	}
	Lines.Add(FString::Printf(TEXT("Mode: %s   Initialized: %s"),
		Asset
			? (Asset->SharingMode == EVoxelPaintAssetSharingMode::Shared ? TEXT("Shared")
				: TEXT("Private"))
			: TEXT("-"),
		Asset ? (Asset->bInitialized ? TEXT("Yes") : TEXT("No")) : TEXT("-")));
	Lines.Add(Asset
		? FString::Printf(TEXT("StorageOrigin: %.1f %.1f %.1f"), Asset->StorageOrigin.X,
			Asset->StorageOrigin.Y, Asset->StorageOrigin.Z)
		: FString(TEXT("StorageOrigin: -")));
	Lines.Add(FString::Printf(TEXT("HalfExtent: %.1f %.1f %.1f"), HalfExtent.X, HalfExtent.Y,
		HalfExtent.Z));
	Lines.Add(FString::Printf(TEXT("TexelSize: %.2f %.2f %.2f"), TexelSize.X, TexelSize.Y,
		TexelSize.Z));
	Lines.Add(FString::Printf(TEXT("Bricks: %d x %d x %d = %lld / %d"), Bricks.X, Bricks.Y,
		Bricks.Z, BrickProduct, WorldVoxelPaint::AssetAdmissionCap));
	Lines.Add(FString::Printf(TEXT("PopulatedBricks: %d"),
		Asset ? Asset->BrickManifest.PopulatedBricks.Num() : 0));

	const UFont* Font = GEngine->GetSmallFont();
	const FIntRect ViewRect = Canvas->GetViewRect();
	const double LineHeight = (double)Font->GetMaxCharHeight() + 2.0;
	const double DrawX = (double)ViewRect.Min.X + 24.0;
	double DrawY = (double)ViewRect.Min.Y + 46.0;

	Canvas->DrawShadowedString(DrawX, DrawY,
		*FString::Printf(TEXT("Voxel Paint Volume: %s"),
			Owner ? *Owner->GetActorLabel() : *Volume->GetName()),
		Font, BoxColor);
	DrawY += LineHeight;
	for (const FString& Line : Lines)
	{
		Canvas->DrawShadowedString(DrawX, DrawY, *Line, Font, FLinearColor::White);
		DrawY += LineHeight;
	}
}

TSharedRef<FComponentVisualizer> MakeVoxelPaintVolumeVisualizer()
{
	return MakeShared<FVoxelPaintVolumeVisualizer>();
}
