// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class IAssetViewport;
class ILevelEditor;
class UWorld;
enum class EMapChangeType : uint8;

// Pushes the active Level Editor viewport into the runtime primary-viewport provider. Game
// thread only.
class FVoxelPaintEditorViewportTracker
{
public:
	void Startup();
	void Shutdown();

private:
	void OnPostEngineInit();
	void BindToLevelEditor();
	void HandleLevelEditorCreated(TSharedPtr<ILevelEditor> LevelEditor);
	void HandleActiveViewportChanged(TSharedPtr<IAssetViewport> OldViewport,
		TSharedPtr<IAssetViewport> NewViewport);
	void HandleMapChanged(UWorld* World, EMapChangeType ChangeType);
	void PushActiveViewport();

	TWeakPtr<ILevelEditor> BoundLevelEditor;
	FDelegateHandle PostEngineInitHandle;
	FDelegateHandle LevelEditorCreatedHandle;
	FDelegateHandle MapChangedHandle;
	FDelegateHandle ActiveViewportChangedHandle;
};
