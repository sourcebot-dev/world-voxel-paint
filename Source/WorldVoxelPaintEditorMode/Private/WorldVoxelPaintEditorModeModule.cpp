// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetRegistry/AssetData.h"
#include "Editor/UnrealEdEngine.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "IPlacementModeModule.h"
#include "Internationalization/Internationalization.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Templates/UniquePtr.h"
#include "UnrealEdGlobals.h"
#include "VoxelPaintBudgetTab.h"
#include "VoxelPaintChannelMaskCustomization.h"
#include "VoxelPaintEditorMode.h"
#include "VoxelPaintEditorModeStyle.h"
#include "VoxelPaintEditorViewportTracker.h"
#include "VoxelPaintSizingDetails.h"
#include "VoxelPaintSizingTool.h"
#include "VoxelPaintTools.h"
#include "VoxelPaintVolume.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeComponentDetails.h"
#include "VoxelPaintVolumeVisualizer.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "WorldVoxelPaintEditorMode"

class FWorldVoxelPaintEditorModeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		ViewportTracker = MakeUnique<FVoxelPaintEditorViewportTracker>();
		ViewportTracker->Startup();

		FCoreDelegates::OnPostEngineInit.AddRaw(this, &FWorldVoxelPaintEditorModeModule::OnPostEngineInit);
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnPostEngineInit.RemoveAll(this);

		if (FPropertyEditorModule* PropertyEditorModule =
			FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor"))
		{
			for (const FName& ClassName : CustomizedClassNames)
			{
				PropertyEditorModule->UnregisterCustomClassLayout(ClassName);
			}
		}
		CustomizedClassNames.Empty();

		if (bBudgetTabRegistered && FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(SVoxelPaintBudgetPanel::TabId);
		}
		bBudgetTabRegistered = false;

		if (!RegisteredVisualizerName.IsNone() && GUnrealEd)
		{
			GUnrealEd->UnregisterComponentVisualizer(RegisteredVisualizerName);
		}
		RegisteredVisualizerName = NAME_None;

		if (IPlacementModeModule::IsAvailable())
		{
			IPlacementModeModule& PlacementMode = IPlacementModeModule::Get();
			PlacementMode.OnPlacementModeCategoryRefreshed().RemoveAll(this);
			if (PlacementModeID.IsSet())
			{
				PlacementMode.UnregisterPlaceableItem(PlacementModeID.GetValue());
			}
		}
		PlacementModeID.Reset();

		FVoxelPaintEditorModeCommands::Unregister();
		FVoxelPaintEditorModeStyle::Shutdown();

		if (ViewportTracker)
		{
			ViewportTracker->Shutdown();
			ViewportTracker.Reset();
		}
	}

private:
	void OnPostEngineInit()
	{
		// The style must exist before the commands register: FUICommandInfo captures the
		// style-set name for its palette icon at registration time.
		FVoxelPaintEditorModeStyle::Initialize();
		FVoxelPaintEditorModeCommands::Register();

		FPropertyEditorModule& PropertyEditorModule =
			FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		for (const UClass* PropertySetClass : { UVoxelPaintBrushToolProperties::StaticClass(),
			UVoxelPaintWholeVolumeToolProperties::StaticClass() })
		{
			PropertyEditorModule.RegisterCustomClassLayout(PropertySetClass->GetFName(),
				FOnGetDetailCustomizationInstance::CreateStatic(
					&FVoxelPaintChannelMaskCustomization::MakeInstance));
			CustomizedClassNames.Add(PropertySetClass->GetFName());
		}

		const FName SizingPropertiesClassName = UVoxelPaintSizingToolProperties::StaticClass()->GetFName();
		PropertyEditorModule.RegisterCustomClassLayout(SizingPropertiesClassName,
			FOnGetDetailCustomizationInstance::CreateStatic(&FVoxelPaintSizingDetails::MakeInstance));
		CustomizedClassNames.Add(SizingPropertiesClassName);

		const FName VolumeComponentClassName = UVoxelPaintVolumeComponent::StaticClass()->GetFName();
		PropertyEditorModule.RegisterCustomClassLayout(VolumeComponentClassName,
			FOnGetDetailCustomizationInstance::CreateStatic(
				&FVoxelPaintVolumeComponentDetails::MakeInstance));
		CustomizedClassNames.Add(VolumeComponentClassName);

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(SVoxelPaintBudgetPanel::TabId,
			FOnSpawnTab::CreateStatic(&SVoxelPaintBudgetPanel::SpawnBudgetTab))
			.SetDisplayName(LOCTEXT("BudgetTabDisplayName", "Voxel Paint Budget"))
			.SetTooltipText(LOCTEXT("BudgetTabTooltip",
				"Voxel paint volume, residency, and session budget panel."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsDebugCategory());
		bBudgetTabRegistered = true;

		if (GUnrealEd)
		{
			const FName ComponentClassName = UVoxelPaintVolumeComponent::StaticClass()->GetFName();
			GUnrealEd->RegisterComponentVisualizer(ComponentClassName, MakeVoxelPaintVolumeVisualizer());
			RegisteredVisualizerName = ComponentClassName;
		}

		IPlacementModeModule& PlacementMode = IPlacementModeModule::Get();
		PlacementMode.OnPlacementModeCategoryRefreshed().AddRaw(
			this, &FWorldVoxelPaintEditorModeModule::OnPlacementModeCategoryRefreshed);
		RegisterPlaceableVolume();
	}

	void OnPlacementModeCategoryRefreshed(FName CategoryName)
	{
		if (CategoryName == FBuiltInPlacementCategories::Volumes())
		{
			RegisterPlaceableVolume();
		}
	}

	void RegisterPlaceableVolume()
	{
		PlacementModeID = IPlacementModeModule::Get().RegisterPlaceableItem(
			FBuiltInPlacementCategories::Volumes(),
			MakeShareable(new FPlaceableItem(nullptr, FAssetData(AVoxelPaintVolume::StaticClass()))));
	}

	TUniquePtr<FVoxelPaintEditorViewportTracker> ViewportTracker;
	TOptional<FPlacementModeID> PlacementModeID;
	TArray<FName> CustomizedClassNames;
	FName RegisteredVisualizerName;
	bool bBudgetTabRegistered = false;
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FWorldVoxelPaintEditorModeModule, WorldVoxelPaintEditorMode)
