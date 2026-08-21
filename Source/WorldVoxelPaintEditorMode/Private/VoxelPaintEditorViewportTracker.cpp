// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintEditorViewportTracker.h"

#include "IAssetViewport.h"
#include "ILevelEditor.h"
#include "LevelEditor.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "SLevelViewport.h"
#include "VoxelPaintPrimaryViewport.h"

void FVoxelPaintEditorViewportTracker::Startup()
{
	PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(
		this, &FVoxelPaintEditorViewportTracker::OnPostEngineInit);
	BindToLevelEditor();
}

void FVoxelPaintEditorViewportTracker::Shutdown()
{
	FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
	PostEngineInitHandle.Reset();
	if (FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor"))
	{
		LevelEditorModule->OnLevelEditorCreated().Remove(LevelEditorCreatedHandle);
		LevelEditorModule->OnMapChanged().Remove(MapChangedHandle);
	}
	LevelEditorCreatedHandle.Reset();
	MapChangedHandle.Reset();
	if (TSharedPtr<ILevelEditor> LevelEditor = BoundLevelEditor.Pin())
	{
		if (ActiveViewportChangedHandle.IsValid())
		{
			LevelEditor->OnActiveViewportChanged().Remove(ActiveViewportChangedHandle);
		}
	}
	ActiveViewportChangedHandle.Reset();
	BoundLevelEditor.Reset();
	WorldVoxelPaint::SetActivePrimaryViewport(nullptr);
}

void FVoxelPaintEditorViewportTracker::OnPostEngineInit()
{
	BindToLevelEditor();
}

void FVoxelPaintEditorViewportTracker::BindToLevelEditor()
{
	if (!GIsEditor)
	{
		return;
	}
	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	if (!LevelEditorCreatedHandle.IsValid())
	{
		LevelEditorCreatedHandle = LevelEditorModule.OnLevelEditorCreated().AddRaw(
			this, &FVoxelPaintEditorViewportTracker::HandleLevelEditorCreated);
	}
	if (!MapChangedHandle.IsValid())
	{
		MapChangedHandle = LevelEditorModule.OnMapChanged().AddRaw(
			this, &FVoxelPaintEditorViewportTracker::HandleMapChanged);
	}
	HandleLevelEditorCreated(LevelEditorModule.GetFirstLevelEditor());
}

void FVoxelPaintEditorViewportTracker::HandleLevelEditorCreated(TSharedPtr<ILevelEditor> LevelEditor)
{
	if (TSharedPtr<ILevelEditor> Previous = BoundLevelEditor.Pin())
	{
		if (ActiveViewportChangedHandle.IsValid())
		{
			Previous->OnActiveViewportChanged().Remove(ActiveViewportChangedHandle);
		}
	}
	ActiveViewportChangedHandle.Reset();
	BoundLevelEditor = LevelEditor;
	if (LevelEditor.IsValid())
	{
		ActiveViewportChangedHandle = LevelEditor->OnActiveViewportChanged().AddRaw(
			this, &FVoxelPaintEditorViewportTracker::HandleActiveViewportChanged);
	}
	PushActiveViewport();
}

void FVoxelPaintEditorViewportTracker::HandleActiveViewportChanged(TSharedPtr<IAssetViewport> OldViewport,
	TSharedPtr<IAssetViewport> NewViewport)
{
	WorldVoxelPaint::SetActivePrimaryViewport(NewViewport.IsValid() ? NewViewport->GetActiveViewport() : nullptr);
}

void FVoxelPaintEditorViewportTracker::HandleMapChanged(UWorld* World, EMapChangeType ChangeType)
{
	PushActiveViewport();
}

void FVoxelPaintEditorViewportTracker::PushActiveViewport()
{
	FViewport* ActiveViewport = nullptr;
	if (TSharedPtr<ILevelEditor> LevelEditor = BoundLevelEditor.Pin())
	{
		if (TSharedPtr<SLevelViewport> LevelViewport = LevelEditor->GetActiveViewportInterface())
		{
			ActiveViewport = LevelViewport->GetActiveViewport();
		}
	}
	WorldVoxelPaint::SetActivePrimaryViewport(ActiveViewport);
}
