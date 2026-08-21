// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintEditorMode.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "InputCoreTypes.h"
#include "InteractiveToolManager.h"
#include "Textures/SlateIcon.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "UnrealClient.h"
#include "VoxelPaintEditorModeToolkit.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintSizingTool.h"
#include "VoxelPaintTools.h"
#include "VoxelPaintWorldSubsystem.h"

#define LOCTEXT_NAMESPACE "VoxelPaintEditorMode"

const FName UVoxelPaintMode::ModeName = FName(TEXT("VoxelPaintMode"));
const FName UVoxelPaintMode::VoxelPaintPalette = FName(TEXT("VoxelPaint"));

FString UVoxelPaintMode::SelectToolName = TEXT("VoxelPaintSelectTool");
FString UVoxelPaintMode::PaintToolName = TEXT("VoxelPaintBrushTool");
FString UVoxelPaintMode::EraseToolName = TEXT("VoxelPaintEraseTool");
FString UVoxelPaintMode::FloodToolName = TEXT("VoxelPaintFloodTool");
FString UVoxelPaintMode::InvertToolName = TEXT("VoxelPaintInvertTool");
FString UVoxelPaintMode::SwapToolName = TEXT("VoxelPaintSwapTool");
FString UVoxelPaintMode::CreateVolumeToolName = TEXT("VoxelPaintCreateVolumeTool");
FString UVoxelPaintMode::SizingToolName = TEXT("VoxelPaintSizingTool");

void FVoxelPaintEditorModeCommands::RegisterCommands()
{
	UI_COMMAND(SelectTool, "Select", "Select a voxel paint volume by clicking it", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND(PaintTool, "Paint", "Paint the selected volume's visible surfaces", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND(EraseTool, "Erase", "Erase paint toward zero on visible surfaces", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND(FloodTool, "Flood", "Fill every texel of the volume with the color on masked channels", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND(InvertTool, "Invert", "Invert masked channels across the whole volume", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND(SwapTool, "Swap", "Swap two channels across the whole volume (both must be masked)", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND(CreateVolumeTool, "Create", "Create a voxel paint volume fitted around the selected actors", EUserInterfaceActionType::ToggleButton, FInputChord());
	UI_COMMAND(SizingTool, "Sizing", "Edit an uninitialized volume's size and detail on the exact-fit ladder", EUserInterfaceActionType::ToggleButton, FInputChord());

	Commands.Add(UVoxelPaintMode::VoxelPaintPalette, {
		SelectTool, PaintTool, EraseTool, FloodTool, InvertTool, SwapTool, CreateVolumeTool, SizingTool
	});
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> FVoxelPaintEditorModeCommands::GetCommands()
{
	return FVoxelPaintEditorModeCommands::Get().Commands;
}

UVoxelPaintMode::UVoxelPaintMode()
{
	Info = FEditorModeInfo(
		ModeName,
		LOCTEXT("VoxelPaintModeName", "Voxel Paint"),
		FSlateIcon(FVoxelPaintEditorModeStyle::StyleName, "VoxelPaintMode.ModeIcon", "VoxelPaintMode.ModeIcon.Small"),
		true,
		601);
}

void UVoxelPaintMode::Enter()
{
	Super::Enter();

	if (GEditor && (GEditor->PlayWorld != nullptr || GEditor->IsPlaySessionInProgress()))
	{
		FVoxelPaintToolsContext::NotifyUser(LOCTEXT("ModeEnterDeniedPIE",
			"Voxel Paint is editor-only; end the PIE/SIE session first."));
		TWeakObjectPtr<UVoxelPaintMode> WeakThis(this);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis](float)
		{
			if (UVoxelPaintMode* Mode = WeakThis.Get())
			{
				if (FEditorModeTools* ModeManager = Mode->GetModeManager())
				{
					ModeManager->DeactivateMode(Mode->GetID());
				}
			}
			return false;
		}));
		return;
	}

	const FVoxelPaintEditorModeCommands& Commands = FVoxelPaintEditorModeCommands::Get();

	RegisterTool(Commands.SelectTool, SelectToolName, NewObject<UVoxelPaintSelectToolBuilder>(this));

	RegisterTool(Commands.PaintTool, PaintToolName, NewObject<UVoxelPaintBrushToolBuilder>(this));

	UVoxelPaintBrushToolBuilder* EraseBuilder = NewObject<UVoxelPaintBrushToolBuilder>(this);
	EraseBuilder->bEraseMode = true;
	RegisterTool(Commands.EraseTool, EraseToolName, EraseBuilder);

	UVoxelPaintWholeVolumeToolBuilder* FloodBuilder = NewObject<UVoxelPaintWholeVolumeToolBuilder>(this);
	FloodBuilder->Op = EVoxelPaintStrokeOp::Flood;
	RegisterTool(Commands.FloodTool, FloodToolName, FloodBuilder);

	UVoxelPaintWholeVolumeToolBuilder* InvertBuilder = NewObject<UVoxelPaintWholeVolumeToolBuilder>(this);
	InvertBuilder->Op = EVoxelPaintStrokeOp::Invert;
	RegisterTool(Commands.InvertTool, InvertToolName, InvertBuilder);

	UVoxelPaintWholeVolumeToolBuilder* SwapBuilder = NewObject<UVoxelPaintWholeVolumeToolBuilder>(this);
	SwapBuilder->Op = EVoxelPaintStrokeOp::Swap;
	RegisterTool(Commands.SwapTool, SwapToolName, SwapBuilder);

	RegisterTool(Commands.CreateVolumeTool, CreateVolumeToolName, NewObject<UVoxelPaintCreateVolumeToolBuilder>(this));
	RegisterTool(Commands.SizingTool, SizingToolName, NewObject<UVoxelPaintSizingToolBuilder>(this));
}

void UVoxelPaintMode::Exit()
{
	Super::Exit();

	if (FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated())
	{
		if (Controller->IsSessionOpen())
		{
			Controller->RequestAutoFinish(TEXT("VoxelPaintModeExit"));
		}
	}
	if (UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(GetWorld()))
	{
		Subsystem->ClearAllForcedVariants();
	}
	FVoxelPaintToolsContext::Get().SyncPipelineToSession();
}

void UVoxelPaintMode::ModeTick(float DeltaTime)
{
	Super::ModeTick(DeltaTime);
	FVoxelPaintToolsContext::Get().SyncPipelineToSession();
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> UVoxelPaintMode::GetModeCommands() const
{
	return FVoxelPaintEditorModeCommands::GetCommands();
}

bool UVoxelPaintMode::InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport,
	FKey Key, EInputEvent Event)
{
	if (Event != IE_Pressed && Event != IE_Repeat)
	{
		return false;
	}
	FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
	if (!Controller || !Viewport)
	{
		return false;
	}
	// Private undo routing ends at the commit boundary: from Committed onward the chord falls
	// through to the global transactor so the just-committed Finish stays keyboard-undoable.
	const WorldVoxelPaint::EVoxelPaintSessionStage Stage = Controller->GetStage();
	const bool bPreCommit = Stage == WorldVoxelPaint::EVoxelPaintSessionStage::Active
		|| Stage == WorldVoxelPaint::EVoxelPaintSessionStage::Sealing
		|| Stage == WorldVoxelPaint::EVoxelPaintSessionStage::Canonicalizing;
	if (!bPreCommit)
	{
		return false;
	}

	const bool bCtrl = Viewport->KeyState(EKeys::LeftControl) || Viewport->KeyState(EKeys::RightControl);
	const bool bShift = Viewport->KeyState(EKeys::LeftShift) || Viewport->KeyState(EKeys::RightShift);
	const bool bAlt = Viewport->KeyState(EKeys::LeftAlt) || Viewport->KeyState(EKeys::RightAlt);
	if (!bCtrl || bAlt)
	{
		return false;
	}

	if (Key == EKeys::Z)
	{
		if (bShift)
		{
			Controller->RedoInSession();
		}
		else
		{
			Controller->UndoInSession();
		}
		return true;
	}
	if (Key == EKeys::Y && !bShift)
	{
		Controller->RedoInSession();
		return true;
	}
	return false;
}

void UVoxelPaintMode::CreateToolkit()
{
	Toolkit = MakeShareable(new FVoxelPaintModeToolkit);
}

void UVoxelPaintMode::ActivateDefaultTool()
{
	GetInteractiveToolsContext()->StartTool(SelectToolName);
}

#undef LOCTEXT_NAMESPACE
