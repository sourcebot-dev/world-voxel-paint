// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelPaintVolume.generated.h"

class UVoxelPaintVolumeComponent;

UCLASS(NotBlueprintable, hideCategories = (Actor, Collision, Cooking, Input, LOD, Physics, Networking, Replication))
class WORLDVOXELPAINT_API AVoxelPaintVolume : public AActor
{
	GENERATED_BODY()
public:
	AVoxelPaintVolume(const FObjectInitializer& ObjectInitializer);

	UVoxelPaintVolumeComponent* GetVolumeComponent() const;

	virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

private:
#if WITH_EDITOR
	void RefreshEditorBoxExtent();
	void CommitGizmoScaleResize();
#endif

	UPROPERTY(VisibleAnywhere, Category = "Voxel Paint", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UVoxelPaintVolumeComponent> VolumeComponent;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<class UBoxComponent> EditorBoxComponent;
#endif
};
