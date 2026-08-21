// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ComponentVisualizer.h"
#include "Templates/SharedPointer.h"

class FVoxelPaintVolumeVisualizer : public FComponentVisualizer
{
public:
	virtual void DrawVisualization(const UActorComponent* Component, const FSceneView* View,
		FPrimitiveDrawInterface* PDI) override;
	virtual void DrawVisualizationHUD(const UActorComponent* Component, const FViewport* Viewport,
		const FSceneView* View, FCanvas* Canvas) override;
};

TSharedRef<FComponentVisualizer> MakeVoxelPaintVolumeVisualizer();
