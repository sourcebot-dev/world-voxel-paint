// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintEditorModeToolkit.h"

#include "Engine/World.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Misc/Attribute.h"
#include "Misc/MessageDialog.h"
#include "SPrimaryButton.h"
#include "SSimpleButton.h"
#include "SWarningOrErrorBox.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Textures/SlateIcon.h"
#include "UObject/ObjectKey.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "VoxelPaintAuthority.h"
#include "VoxelPaintBudgetTab.h"
#include "VoxelPaintBudgetViewModel.h"
#include "VoxelPaintCoordinator.h"
#include "VoxelPaintDeveloperSettings.h"
#include "VoxelPaintEditorMode.h"
#include "VoxelPaintEditorWorldFilter.h"
#include "VoxelPaintGridReadiness.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintTemplateRouting.h"
#include "VoxelPaintTools.h"
#include "VoxelPaintTypes.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "VoxelPaintWorldSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "VoxelPaintEditorMode"

namespace VoxelPaintToolkitPrivate
{
	static WorldVoxelPaint::EVoxelPaintSessionStage GetStage()
	{
		const FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
		return Controller ? Controller->GetStage() : WorldVoxelPaint::EVoxelPaintSessionStage::Closed;
	}

	static bool IsSessionOpen()
	{
		const FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
		return Controller && Controller->IsSessionOpen();
	}

	static FText GetSessionStageValueText()
	{
		const FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
		if (!Controller || !Controller->IsSessionOpen())
		{
			return LOCTEXT("SessionStageNone", "None");
		}
		return FText::Format(LOCTEXT("SessionStageFormat", "{0} on '{1}'"),
			FText::FromString(WorldVoxelPaint::LexToString(Controller->GetStage())),
			FText::FromString(GetNameSafe(Controller->GetActiveAsset())));
	}

	static FSlateColor GetSessionStageColor()
	{
		if (!IsSessionOpen())
		{
			return FSlateColor::UseSubduedForeground();
		}
		switch (GetStage())
		{
		case WorldVoxelPaint::EVoxelPaintSessionStage::Active:
			return FStyleColors::AccentGreen;
		case WorldVoxelPaint::EVoxelPaintSessionStage::DerivedFailed:
			return FStyleColors::Error;
		default:
			return FStyleColors::Warning;
		}
	}

	static bool ShouldShowDerivedIdleWarning()
	{
		const FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
		return Controller && Controller->IsSessionOpen()
			&& ShouldWarnDerivedLaneIdle(static_cast<uint8>(Controller->GetStage()),
				Controller->GetSecondsSinceDerivedServiced());
	}

	static FText GetGridReadinessValueText()
	{
		const FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated();
		if (!Coordinator)
		{
			return LOCTEXT("GridValueUnavailable", "Unavailable");
		}
		switch (Coordinator->GetGridReadiness())
		{
		case WorldVoxelPaint::EVoxelPaintGridReadiness::Ready:
			return LOCTEXT("GridValueReady", "Ready");
		case WorldVoxelPaint::EVoxelPaintGridReadiness::Failed:
			return LOCTEXT("GridValueFailed", "Failed");
		case WorldVoxelPaint::EVoxelPaintGridReadiness::Pending:
		default:
			return LOCTEXT("GridValuePending", "Pending");
		}
	}

	static FSlateColor GetGridReadinessColor()
	{
		const FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated();
		if (!Coordinator)
		{
			return FSlateColor::UseSubduedForeground();
		}
		switch (Coordinator->GetGridReadiness())
		{
		case WorldVoxelPaint::EVoxelPaintGridReadiness::Ready:
			return FStyleColors::AccentGreen;
		case WorldVoxelPaint::EVoxelPaintGridReadiness::Failed:
			return FStyleColors::Error;
		case WorldVoxelPaint::EVoxelPaintGridReadiness::Pending:
		default:
			return FStyleColors::Warning;
		}
	}

	static FText GetGridReadinessToolTipText()
	{
		const FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated();
		if (!Coordinator)
		{
			return LOCTEXT("GridToolTipUnavailable", "The voxel paint coordinator has not been created yet.");
		}
		switch (Coordinator->GetGridReadiness())
		{
		case WorldVoxelPaint::EVoxelPaintGridReadiness::Ready:
			return LOCTEXT("GridToolTipReady", "The clipmap grid is up to date; painting is available.");
		case WorldVoxelPaint::EVoxelPaintGridReadiness::Failed:
			return LOCTEXT("GridToolTipFailed",
				"The clipmap grid rebuild failed. Painting is gated; a recovery rebuild is requested automatically.");
		case WorldVoxelPaint::EVoxelPaintGridReadiness::Pending:
		default:
			return LOCTEXT("GridToolTipPending",
				"Painting is gated until the pending clipmap grid rebuild is observed.");
		}
	}

	static FText GetSessionGpuValueText()
	{
		FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
		WorldVoxelPaint::FVoxelPaintSessionBudgetLedger* Ledger =
			(Controller && Controller->IsSessionOpen()) ? Controller->GetBudgetLedger() : nullptr;
		if (!Ledger)
		{
			return LOCTEXT("SessionGpuNone", "None");
		}
		return FText::Format(LOCTEXT("SessionGpuFormat", "{0} / {1} MiB"),
			FText::AsNumber((int64)(Ledger->GetUsedBytes() >> 20)),
			FText::AsNumber((int64)(Ledger->GetBudgetBytes() >> 20)));
	}

	static FText GetJournalValueText()
	{
		FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
		if (!Controller || !Controller->IsSessionOpen())
		{
			return FText::Format(LOCTEXT("JournalCapacityFormat", "{0} MiB capacity"),
				FText::AsNumber(GetDefault<UVoxelPaintDeveloperSettings>()->GetJournalCapacityBytes() >> 20));
		}
		return FText::Format(LOCTEXT("JournalLiveFormat", "{0} / {1} ({2} entries)"),
			FText::AsMemory((uint64)FMath::Max<int64>(0, Controller->GetJournalRetainedBytes())),
			FText::AsMemory((uint64)FMath::Max<int64>(0, Controller->GetJournalCapacityBytes())),
			FText::AsNumber(Controller->GetJournalEntryCount()));
	}

	static FText GetPickSourceValueText()
	{
		return FText::FromName(FVoxelPaintToolsContext::Get().GetLastPickSource());
	}
}

bool FVoxelPaintForcePreviewPolicy::CanForceVariant(const UVoxelPaintVolumeData* Asset, FText& OutWhyNot)
{
	const FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
	if (!Controller || !Controller->IsSessionOpen())
	{
		return true;
	}
	const UVoxelPaintVolumeData* ActiveAsset = Controller->GetActiveAsset();
	if (Asset == ActiveAsset)
	{
		return true;
	}
	OutWhyNot = FText::Format(LOCTEXT("ForcePreviewSessionRefusal",
		"A paint session is open on '{0}' (one session at a time). Finish or Discard it first."),
		FText::FromString(GetNameSafe(ActiveAsset)));
	return false;
}

bool FVoxelPaintForcePreviewPolicy::ApplyForcedVariant(UVoxelPaintVolumeComponent* Placement,
	const UVoxelPaintVolumeData* Asset, bool bAllCopies, FText& OutWhyNot)
{
	if (!Placement)
	{
		OutWhyNot = LOCTEXT("ForcePreviewNoPlacement", "Select a voxel paint volume placement first.");
		return false;
	}
	if (Asset && !CanForceVariant(Asset, OutWhyNot))
	{
		return false;
	}
	UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(Placement->GetWorld());
	if (!Subsystem)
	{
		OutWhyNot = LOCTEXT("ForcePreviewNoSubsystem",
			"The voxel paint subsystem is unavailable in this world.");
		return false;
	}
	TArray<UVoxelPaintVolumeComponent*> Domain;
	if (bAllCopies)
	{
		ComputeAllCopiesDomain(Placement, Domain);
	}
	else if (Subsystem->GetFrontierPosition(Placement) != INDEX_NONE)
	{
		Domain.Add(Placement);
	}
	if (Domain.IsEmpty())
	{
		OutWhyNot = LOCTEXT("ForcePreviewFrontierRefusal",
			"Frontier full - this placement is not rendering; free a slot or paint a rendered copy.");
		return false;
	}
	for (UVoxelPaintVolumeComponent* Member : Domain)
	{
		FVoxelPaintPlacementState State;
		if (!Subsystem->GetCachedPlacementState(Member, State))
		{
			continue;
		}
		if (Asset)
		{
			Subsystem->SetForcedVariant(State.Identity, Asset);
		}
		else
		{
			Subsystem->ClearForcedVariant(State.Identity);
		}
	}
	return true;
}

void FVoxelPaintForcePreviewPolicy::ComputeAllCopiesDomain(UVoxelPaintVolumeComponent* Placement,
	TArray<UVoxelPaintVolumeComponent*>& OutDomain)
{
	OutDomain.Reset();
	if (!Placement)
	{
		return;
	}
	UWorld* World = Placement->GetWorld();
	UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(World);
	if (!Subsystem)
	{
		return;
	}
	UVoxelPaintVolumeComponent* SupplyingTemplate = WorldVoxelPaint::ResolveEffectiveSupplyingTemplate(Placement);
	if (!SupplyingTemplate)
	{
		if (Subsystem->GetFrontierPosition(Placement) != INDEX_NONE)
		{
			OutDomain.Add(Placement);
		}
		return;
	}
	for (TObjectIterator<UVoxelPaintVolumeComponent> It; It; ++It)
	{
		UVoxelPaintVolumeComponent* Candidate = *It;
		if (!IsValid(Candidate) || Candidate->IsTemplate() || Candidate->GetWorld() != World)
		{
			continue;
		}
		if (Subsystem->GetFrontierPosition(Candidate) == INDEX_NONE)
		{
			continue;
		}
		if (WorldVoxelPaint::ResolveEffectiveSupplyingTemplate(Candidate) != SupplyingTemplate)
		{
			continue;
		}
		OutDomain.Add(Candidate);
	}
}

void FVoxelPaintModeToolkit::Init(const TSharedPtr<IToolkitHost>& InitToolkitHost,
	TWeakObjectPtr<UEdMode> InOwningMode)
{
	FModeToolkit::Init(InitToolkitHost, InOwningMode);
}

FName FVoxelPaintModeToolkit::GetToolkitFName() const
{
	return FName("VoxelPaintMode");
}

FText FVoxelPaintModeToolkit::GetBaseToolkitName() const
{
	return LOCTEXT("ToolkitName", "Voxel Paint Mode");
}

void FVoxelPaintModeToolkit::GetToolPaletteNames(TArray<FName>& PaletteNames) const
{
	PaletteNames.Add(UVoxelPaintMode::VoxelPaintPalette);
}

FText FVoxelPaintModeToolkit::GetToolPaletteDisplayName(FName Palette) const
{
	if (Palette == UVoxelPaintMode::VoxelPaintPalette)
	{
		return LOCTEXT("VoxelPaintPalette", "Voxel Paint");
	}
	return FText();
}

TSharedPtr<SWidget> FVoxelPaintModeToolkit::GetInlineContent() const
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			BuildSessionPanel()
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			FModeToolkit::GetInlineContent().ToSharedRef()
		];
}

TSharedRef<SWidget> FVoxelPaintModeToolkit::BuildSessionPanel() const
{
	using namespace VoxelPaintToolkitPrivate;

	const FMargin StandardPadding(6.0f, 3.0f);
	const FMargin StandardLeftPadding(6.0f, 3.0f, 3.0f, 3.0f);
	const FMargin StandardRightPadding(3.0f, 3.0f, 6.0f, 3.0f);
	const FSlateFontInfo StandardFont = FAppStyle::GetFontStyle(TEXT("PropertyWindow.NormalFont"));

	// Section header styled like a details-view category (see Mesh Paint's Resource Usage /
	// Visualization sections and the modeling mode quick-settings panel).
	auto MakeSection = [](const FText& Title, const TSharedRef<SWidget>& Body) -> TSharedRef<SWidget>
	{
		return SNew(SExpandableArea)
			.BorderImage(FAppStyle::Get().GetBrush("DetailsView.CategoryTop"))
			.HeaderPadding(FMargin(4.0f, 4.0f))
			.Padding(FMargin(0.0f, 2.0f, 0.0f, 6.0f))
			.AreaTitle(Title)
			.AreaTitleFont(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			.BodyContent()
			[
				Body
			];
	};

	auto MakeRow = [StandardLeftPadding, StandardRightPadding, StandardFont](const FText& Label,
		const TAttribute<FText>& Value, const TAttribute<FSlateColor>& ValueColor,
		const TAttribute<FText>& ToolTip, const TAttribute<EVisibility>& Visibility) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			.Visibility(Visibility)
			.ToolTipText(ToolTip)
			+ SHorizontalBox::Slot()
			.Padding(StandardLeftPadding)
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(StandardFont)
			]
			+ SHorizontalBox::Slot()
			.Padding(StandardRightPadding)
			.FillWidth(2.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Value)
				.Font(StandardFont)
				.ColorAndOpacity(ValueColor)
				.AutoWrapText(true)
			];
	};

	auto MakeActionButton = [this](const FText& Label, const FText& ToolTip,
		FReply (FVoxelPaintModeToolkit::*Handler)() const, bool (FVoxelPaintModeToolkit::*Enabled)() const,
		const TAttribute<EVisibility>& Visibility) -> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.Text(Label)
			.ToolTipText(ToolTip)
			.Visibility(Visibility)
			.IsEnabled_Lambda([this, Enabled]() { return (this->*Enabled)(); })
			.OnClicked_Lambda([this, Handler]() { return (this->*Handler)(); });
	};

	auto MakeRemedyButton = [this](const FText& Label, const FText& ToolTip,
		FReply (FVoxelPaintModeToolkit::*Handler)() const, bool (FVoxelPaintModeToolkit::*Enabled)() const,
		const TAttribute<EVisibility>& Visibility, bool bProvenanceGated) -> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.Text(Label)
			.ToolTipText_Lambda([this, ToolTip, bProvenanceGated]()
			{
				const FText GateReason = GetAuthorityGateToolTipText(bProvenanceGated);
				return GateReason.IsEmpty() ? ToolTip : GateReason;
			})
			.Visibility(Visibility)
			.IsEnabled_Lambda([this, Enabled]() { return (this->*Enabled)(); })
			.OnClicked_Lambda([this, Handler]() { return (this->*Handler)(); });
	};

	const TSharedRef<SWidget> SessionBody = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRow(LOCTEXT("SessionStateLabel", "State"),
				MakeAttributeLambda([]() { return GetSessionStageValueText(); }),
				MakeAttributeLambda([]() { return GetSessionStageColor(); }),
				LOCTEXT("SessionStateToolTip", "Lifecycle stage of the open paint session and the asset it edits."),
				EVisibility::Visible)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(StandardPadding)
		[
			SNew(SWarningOrErrorBox)
			.MessageStyle(EMessageStyle::Warning)
			.Message_Lambda([]() { return FText::FromString(GetDerivedLaneIdleHint()); })
			.Visibility_Lambda([]()
			{
				return ShouldShowDerivedIdleWarning() ? EVisibility::Visible : EVisibility::Collapsed;
			})
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRow(LOCTEXT("GridReadinessLabel", "Grid Readiness"),
				MakeAttributeLambda([]() { return GetGridReadinessValueText(); }),
				MakeAttributeLambda([]() { return GetGridReadinessColor(); }),
				MakeAttributeLambda([]() { return GetGridReadinessToolTipText(); }),
				EVisibility::Visible)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRow(LOCTEXT("SessionGpuLabel", "Session GPU"),
				MakeAttributeLambda([]() { return GetSessionGpuValueText(); }),
				FSlateColor::UseForeground(),
				LOCTEXT("SessionGpuToolTip", "GPU memory used by the open session against its budget."),
				EVisibility::Visible)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRow(LOCTEXT("JournalLabel", "Journal"),
				MakeAttributeLambda([]() { return GetJournalValueText(); }),
				FSlateColor::UseForeground(),
				LOCTEXT("JournalToolTip", "Stroke journal retention against its configured capacity."),
				EVisibility::Visible)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRow(LOCTEXT("PickSourceLabel", "Pick Source"),
				MakeAttributeLambda([]() { return GetPickSourceValueText(); }),
				FSlateColor::UseForeground(),
				LOCTEXT("PickSourceToolTip", "The provider that resolved the most recent brush pick."),
				EVisibility::Visible)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(StandardPadding)
		[
			SNew(SWrapBox)
			.UseAllottedSize(true)
			.InnerSlotPadding(FVector2D(4.0f, 4.0f))
			+ SWrapBox::Slot()
			[
				SNew(SPrimaryButton)
				.Text(LOCTEXT("SessionFinish", "Finish"))
				.ToolTipText(LOCTEXT("SessionFinishToolTip", "Seal and commit the open paint session."))
				.IsEnabled_Lambda([this]() { return CanFinish(); })
				.OnClicked_Lambda([this]() { return OnFinishClicked(); })
			]
			+ SWrapBox::Slot()
			[
				MakeActionButton(LOCTEXT("SessionDiscard", "Discard"),
					LOCTEXT("SessionDiscardToolTip", "Discard the open paint session, reverting all session paint."),
					&FVoxelPaintModeToolkit::OnDiscardClicked, &FVoxelPaintModeToolkit::CanDiscard,
					EVisibility::Visible)
			]
			+ SWrapBox::Slot()
			[
				MakeActionButton(LOCTEXT("SessionCancelSealing", "Cancel Sealing"),
					LOCTEXT("SessionCancelSealingToolTip", "Return the sealing session to active painting."),
					&FVoxelPaintModeToolkit::OnCancelSealingClicked, &FVoxelPaintModeToolkit::CanCancelSealing,
					MakeAttributeLambda([]()
					{
						return GetStage() == WorldVoxelPaint::EVoxelPaintSessionStage::Sealing
							? EVisibility::Visible : EVisibility::Collapsed;
					}))
			]
			+ SWrapBox::Slot()
			[
				MakeActionButton(LOCTEXT("SessionRetry", "Retry"),
					LOCTEXT("SessionRetryToolTip", "Retry the failed derived-data build."),
					&FVoxelPaintModeToolkit::OnRetryClicked, &FVoxelPaintModeToolkit::CanRetryOrEndAnyway,
					MakeAttributeLambda([]()
					{
						return GetStage() == WorldVoxelPaint::EVoxelPaintSessionStage::DerivedFailed
							? EVisibility::Visible : EVisibility::Collapsed;
					}))
			]
			+ SWrapBox::Slot()
			[
				MakeActionButton(LOCTEXT("SessionEndAnyway", "End Anyway"),
					LOCTEXT("SessionEndAnywayToolTip", "End the session despite the failed derived-data build."),
					&FVoxelPaintModeToolkit::OnEndAnywayClicked, &FVoxelPaintModeToolkit::CanRetryOrEndAnyway,
					MakeAttributeLambda([]()
					{
						return GetStage() == WorldVoxelPaint::EVoxelPaintSessionStage::DerivedFailed
							? EVisibility::Visible : EVisibility::Collapsed;
					}))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(StandardPadding)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SSpacer)
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SSimpleButton)
				.Text(LOCTEXT("OpenBudgetPanel", "Budget Panel"))
				.Icon(FAppStyle::Get().GetBrush("Icons.Settings"))
				.ToolTipText(LOCTEXT("OpenBudgetPanelToolTip",
					"Open the voxel paint volume, residency, and session budget panel."))
				.OnClicked_Lambda([]()
				{
					FGlobalTabmanager::Get()->TryInvokeTab(FTabId(SVoxelPaintBudgetPanel::TabId));
					return FReply::Handled();
				})
			]
		];

	const TSharedRef<SWidget> AssetBody = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRow(LOCTEXT("AssetLabel", "Asset"),
				MakeAttributeLambda([this]() { return GetAssetValueText(); }),
				MakeAttributeLambda([this]() { return GetAssetValueColor(); }),
				MakeAttributeLambda([this]() { return GetAssetToolTipText(); }),
				EVisibility::Visible)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRow(LOCTEXT("SharingLabel", "Sharing"),
				MakeAttributeLambda([this]() { return GetSharingValueText(); }),
				FSlateColor::UseForeground(),
				LOCTEXT("SharingToolTip",
					"Shared assets are equal peers: every linked placement may edit. "
					"Private assets are editable only through the owning placement."),
				MakeAttributeLambda([this]() { return GetAssetDetailVisibility(); }))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRow(LOCTEXT("OwnerLabel", "Owner"),
				MakeAttributeLambda([this]() { return GetOwnerValueText(); }),
				MakeAttributeLambda([this]() { return GetOwnerValueColor(); }),
				MakeAttributeLambda([this]() { return GetOwnerToolTipText(); }),
				MakeAttributeLambda([this]() { return GetOwnerRowVisibility(); }))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRow(LOCTEXT("LinkedPlacementsLabel", "Linked Placements"),
				MakeAttributeLambda([this]() { return GetLinkedPlacementsValueText(); }),
				FSlateColor::UseForeground(),
				LOCTEXT("LinkedPlacementsToolTip", "Placements in loaded levels that reference this asset."),
				MakeAttributeLambda([this]() { return GetAssetDetailVisibility(); }))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(StandardPadding)
		[
			SNew(SWarningOrErrorBox)
			.MessageStyle(EMessageStyle::Warning)
			.Message_Lambda([this]() { return GetDanglingPinText(); })
			.Visibility_Lambda([this]() { return GetDanglingPinVisibility(); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(StandardPadding)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ScaleNote",
				"voxel paint ignores scale; painted data will not match scaled geometry"))
			.Font(StandardFont)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
			.Visibility_Lambda([this]() { return GetScaleNoteVisibility(); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(StandardPadding)
		[
			SNew(SWrapBox)
			.UseAllottedSize(true)
			.InnerSlotPadding(FVector2D(4.0f, 4.0f))
			.Visibility_Lambda([this]() { return GetAssetDetailVisibility(); })
			+ SWrapBox::Slot()
			[
				MakeRemedyButton(LOCTEXT("RemedyTakeOwnership", "Take Ownership"),
					LOCTEXT("RemedyTakeOwnershipToolTip", "Make this placement the owner of the asset."),
					&FVoxelPaintModeToolkit::OnTakeOwnershipClicked, &FVoxelPaintModeToolkit::CanTakeOwnership,
					MakeAttributeLambda([this]() { return GetTakeOwnershipVisibility(); }), true)
			]
			+ SWrapBox::Slot()
			[
				MakeRemedyButton(LOCTEXT("RemedyMakeUnique", "Make Unique"),
					LOCTEXT("RemedyMakeUniqueToolTip",
						"Copy the asset so this placement gets its own private copy."),
					&FVoxelPaintModeToolkit::OnMakeUniqueClicked, &FVoxelPaintModeToolkit::CanMakeUnique,
					EVisibility::Visible, true)
			]
			+ SWrapBox::Slot()
			[
				MakeRemedyButton(LOCTEXT("RemedyMarkShared", "Mark Shared"),
					LOCTEXT("RemedyMarkSharedToolTip", "Allow every linked placement to edit the asset."),
					&FVoxelPaintModeToolkit::OnMarkSharedClicked, &FVoxelPaintModeToolkit::CanMarkShared,
					MakeAttributeLambda([this]() { return GetMarkSharedVisibility(); }), false)
			]
			+ SWrapBox::Slot()
			[
				MakeRemedyButton(LOCTEXT("RemedyMarkPrivate", "Mark Private"),
					LOCTEXT("RemedyMarkPrivateToolTip", "Restrict editing to the owning placement."),
					&FVoxelPaintModeToolkit::OnMarkPrivateClicked, &FVoxelPaintModeToolkit::CanMarkPrivate,
					MakeAttributeLambda([this]() { return GetMarkPrivateVisibility(); }), true)
			]
		];

	const TSharedRef<SWidget> PreviewBody = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			.ToolTipText_Lambda([this]() { return GetForcePreviewToolTipText(); })
			+ SHorizontalBox::Slot()
			.Padding(StandardLeftPadding)
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ForcePreviewLabel", "Preview Variant"))
				.Font(StandardFont)
			]
			+ SHorizontalBox::Slot()
			.Padding(StandardRightPadding)
			.FillWidth(2.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SComboButton)
				.IsEnabled_Lambda([this]() { return CanUseForcePreview(); })
				.OnGetMenuContent_Lambda([this]() { return MakeForcePreviewMenu(); })
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return GetForcePreviewValueText(); })
					.Font(StandardFont)
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.Padding(StandardLeftPadding)
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ForcePreviewScopeLabel", "Scope"))
				.Font(StandardFont)
			]
			+ SHorizontalBox::Slot()
			.Padding(StandardRightPadding)
			.FillWidth(2.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]()
				{
					return bForcePreviewAllCopies ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
				{
					bForcePreviewAllCopies = NewState == ECheckBoxState::Checked;
				})
				.ToolTipText(LOCTEXT("ForcePreviewScopeToolTip",
					"Apply the forced preview to every rendered placement drawing its pool from the "
					"same supplying template. Placement-owned pools apply to the selected copy only."))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ForcePreviewScopeAllCopies", "All copies of this pool"))
					.Font(StandardFont)
				]
			]
		];

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 1.0f)
		[
			MakeSection(LOCTEXT("SessionSection", "Session"), SessionBody)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 1.0f)
		[
			MakeSection(LOCTEXT("VolumeAssetSection", "Volume Asset"), AssetBody)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 1.0f)
		[
			MakeSection(LOCTEXT("VariantPreviewSection", "Variant Preview"), PreviewBody)
		];
}

FText FVoxelPaintModeToolkit::GetAssetValueText() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	if (!Placement)
	{
		return LOCTEXT("AssetNoneSelected", "None selected");
	}
	const UVoxelPaintVolumeData* Asset = Placement->ResolveActiveVariant();
	if (!Asset)
	{
		return LOCTEXT("AssetNoneAssigned", "No asset assigned");
	}
	const int32 ResolvedSlot = Placement->GetResolvedVariantIndex();
	if (ResolvedSlot != INDEX_NONE)
	{
		return FText::Format(LOCTEXT("AssetValueVariantFormat", "{0} [variant {1}/{2}]"),
			FText::FromString(Asset->GetName()), FText::AsNumber(ResolvedSlot + 1),
			FText::AsNumber(Placement->VolumeVariants.Num()));
	}
	return FText::FromString(Asset->GetName());
}

FSlateColor FVoxelPaintModeToolkit::GetAssetValueColor() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	if (!Placement)
	{
		return FSlateColor::UseSubduedForeground();
	}
	return Placement->ResolveActiveVariant() ? FSlateColor::UseForeground() : FSlateColor(FStyleColors::Warning);
}

FText FVoxelPaintModeToolkit::GetAssetToolTipText() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	if (!Placement)
	{
		return LOCTEXT("AssetToolTipNoneSelected",
			"Select a voxel paint volume to see its asset, sharing, and ownership.");
	}
	if (!Placement->ResolveActiveVariant())
	{
		return LOCTEXT("AssetToolTipNoneAssigned",
			"The selected volume has no asset assigned. Author new volumes with the Create tool, "
			"or assign an existing asset in the Details panel.");
	}
	return LOCTEXT("AssetToolTipAssigned",
		"The voxel paint volume data asset referenced by the selected placement.");
}

FText FVoxelPaintModeToolkit::GetSharingValueText() const
{
	const UVoxelPaintVolumeData* Asset = GetSelectedAsset();
	if (!Asset)
	{
		return FText::GetEmpty();
	}
	return Asset->SharingMode == EVoxelPaintAssetSharingMode::Shared
		? LOCTEXT("SharingShared", "Shared") : LOCTEXT("SharingPrivate", "Private");
}

FText FVoxelPaintModeToolkit::GetOwnerValueText() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	const UVoxelPaintVolumeData* Asset = Placement ? Placement->ResolveActiveVariant() : nullptr;
	if (!Asset)
	{
		return FText::GetEmpty();
	}
	if (!Asset->OwnerActorGuid.IsValid())
	{
		return LOCTEXT("OwnerNone", "None (editing blocked)");
	}
	const AActor* OwnerActor = Placement->GetOwner();
	const bool bOwnedHere = OwnerActor && OwnerActor->GetActorGuid() == Asset->OwnerActorGuid;
	return FText::Format(LOCTEXT("OwnerValueFormat", "{0} {1}"),
		FText::FromString(Asset->OwnerDisplayName.IsEmpty() ? TEXT("<unnamed>") : *Asset->OwnerDisplayName),
		bOwnedHere ? LOCTEXT("OwnerThisPlacement", "(this placement)")
			: LOCTEXT("OwnerOtherPlacement", "(another placement)"));
}

FSlateColor FVoxelPaintModeToolkit::GetOwnerValueColor() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	const UVoxelPaintVolumeData* Asset = Placement ? Placement->ResolveActiveVariant() : nullptr;
	if (!Asset)
	{
		return FSlateColor::UseForeground();
	}
	if (!Asset->OwnerActorGuid.IsValid())
	{
		return FStyleColors::Error;
	}
	const AActor* OwnerActor = Placement->GetOwner();
	const bool bOwnedHere = OwnerActor && OwnerActor->GetActorGuid() == Asset->OwnerActorGuid;
	return bOwnedHere ? FSlateColor::UseForeground() : FSlateColor(FStyleColors::Warning);
}

FText FVoxelPaintModeToolkit::GetOwnerToolTipText() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	const UVoxelPaintVolumeData* Asset = Placement ? Placement->ResolveActiveVariant() : nullptr;
	if (!Asset)
	{
		return FText::GetEmpty();
	}
	if (!Asset->OwnerActorGuid.IsValid())
	{
		return LOCTEXT("OwnerToolTipNone",
			"The asset is ownerless, so editing is blocked. Use Take Ownership, Make Unique, or Mark Shared.");
	}
	const AActor* OwnerActor = Placement->GetOwner();
	const bool bOwnedHere = OwnerActor && OwnerActor->GetActorGuid() == Asset->OwnerActorGuid;
	if (!bOwnedHere)
	{
		return LOCTEXT("OwnerToolTipOther",
			"Another placement owns this asset. Use Take Ownership, Make Unique, or Mark Shared to edit here.");
	}
	return LOCTEXT("OwnerToolTipHere", "This placement owns the asset and may edit it.");
}

EVisibility FVoxelPaintModeToolkit::GetOwnerRowVisibility() const
{
	const UVoxelPaintVolumeData* Asset = GetSelectedAsset();
	return (Asset && Asset->SharingMode == EVoxelPaintAssetSharingMode::Private)
		? EVisibility::Visible : EVisibility::Collapsed;
}

FText FVoxelPaintModeToolkit::GetLinkedPlacementsValueText() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	const UVoxelPaintVolumeData* Asset = Placement ? Placement->ResolveActiveVariant() : nullptr;
	if (!Asset)
	{
		return FText::GetEmpty();
	}

	const double Now = FPlatformTime::Seconds();
	if (Now - LinkedPlacementsCacheTime > 0.25)
	{
		LinkedPlacementsCacheTime = Now;
		TArray<FString> Names;
		for (TObjectIterator<UVoxelPaintVolumeComponent> It; It; ++It)
		{
			const UVoxelPaintVolumeComponent* Component = *It;
			if (!IsValid(Component) || Component->IsTemplate()
				|| !WorldVoxelPaint::IsEnumerableWorld(Component->GetWorld()))
			{
				continue;
			}
			if (Component->ResolveActiveVariant() != Asset)
			{
				continue;
			}
			const AActor* Owner = Component->GetOwner();
			Names.Add(Owner ? Owner->GetActorNameOrLabel() : Component->GetName());
		}
		FString Joined;
		const int32 MaxListed = 8;
		for (int32 Index = 0; Index < Names.Num() && Index < MaxListed; ++Index)
		{
			if (Index > 0)
			{
				Joined += TEXT(", ");
			}
			Joined += Names[Index];
		}
		if (Names.Num() > MaxListed)
		{
			Joined += TEXT(", ...");
		}
		LinkedPlacementsCountCache = Names.Num();
		LinkedPlacementsNamesCache = MoveTemp(Joined);
	}

	if (IsPrefabProvenance(Placement))
	{
		const int32 ResolvedSlot = Placement->GetResolvedVariantIndex();
		const FString VariantPart = ResolvedSlot != INDEX_NONE
			? FString::Printf(TEXT(" [variant %d/%d]"), ResolvedSlot + 1, Placement->VolumeVariants.Num())
			: FString();
		return FText::FromString(FString::Printf(TEXT("Shared paint - editing '%s'%s, used by %d placements"),
			*Asset->GetName(), *VariantPart, LinkedPlacementsCountCache));
	}
	return FText::FromString(FString::Printf(TEXT("%d: %s"),
		LinkedPlacementsCountCache, *LinkedPlacementsNamesCache));
}

EVisibility FVoxelPaintModeToolkit::GetAssetDetailVisibility() const
{
	return GetSelectedAsset() ? EVisibility::Visible : EVisibility::Collapsed;
}

bool FVoxelPaintModeToolkit::CanFinish() const
{
	return VoxelPaintToolkitPrivate::GetStage() == WorldVoxelPaint::EVoxelPaintSessionStage::Active;
}

bool FVoxelPaintModeToolkit::CanCancelSealing() const
{
	return VoxelPaintToolkitPrivate::GetStage() == WorldVoxelPaint::EVoxelPaintSessionStage::Sealing;
}

bool FVoxelPaintModeToolkit::CanDiscard() const
{
	return VoxelPaintToolkitPrivate::GetStage() == WorldVoxelPaint::EVoxelPaintSessionStage::Active;
}

bool FVoxelPaintModeToolkit::CanRetryOrEndAnyway() const
{
	return VoxelPaintToolkitPrivate::GetStage() == WorldVoxelPaint::EVoxelPaintSessionStage::DerivedFailed;
}

FReply FVoxelPaintModeToolkit::OnFinishClicked() const
{
	if (FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated())
	{
		Controller->RequestFinish();
	}
	return FReply::Handled();
}

FReply FVoxelPaintModeToolkit::OnCancelSealingClicked() const
{
	if (FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated())
	{
		Controller->CancelSealing();
	}
	return FReply::Handled();
}

FReply FVoxelPaintModeToolkit::OnDiscardClicked() const
{
	FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
	if (!Controller || !Controller->IsSessionOpen())
	{
		return FReply::Handled();
	}
	const FText Prompt = FText::Format(LOCTEXT("DiscardConfirm",
		"Discard the open paint session on '{0}'? All session paint since it opened will be reverted."),
		FText::FromString(GetNameSafe(Controller->GetActiveAsset())));
	if (FMessageDialog::Open(EAppMsgType::YesNo, Prompt) == EAppReturnType::Yes)
	{
		Controller->RequestDiscard();
	}
	return FReply::Handled();
}

FReply FVoxelPaintModeToolkit::OnRetryClicked() const
{
	if (FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated())
	{
		Controller->RequestRetry();
	}
	return FReply::Handled();
}

FReply FVoxelPaintModeToolkit::OnEndAnywayClicked() const
{
	if (FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated())
	{
		Controller->RequestEndAnyway();
	}
	return FReply::Handled();
}

FReply FVoxelPaintModeToolkit::OnTakeOwnershipClicked() const
{
	UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	UVoxelPaintVolumeData* Asset = Placement ? Placement->ResolveActiveVariant() : nullptr;
	if (!Placement || !Asset || IsPrefabProvenance(Placement) || !EnsureAuthorityGatePasses(Asset))
	{
		return FReply::Handled();
	}
	FScopedTransaction Transaction(LOCTEXT("TakeOwnershipTransaction", "Take Ownership of Voxel Paint Asset"));
	WorldVoxelPaint::TakeOwnership(Asset, Placement->GetOwner());
	return FReply::Handled();
}

FReply FVoxelPaintModeToolkit::OnMakeUniqueClicked() const
{
	UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	UVoxelPaintVolumeData* Asset = Placement ? Placement->ResolveActiveVariant() : nullptr;
	if (!Placement || !Asset || IsPrefabProvenance(Placement) || !EnsureAuthorityGatePasses(Asset))
	{
		return FReply::Handled();
	}
	const int32 ResolvedSlot = Placement->GetResolvedVariantIndex();
	if (!Placement->VolumeVariants.IsValidIndex(ResolvedSlot)
		|| Placement->VolumeVariants[ResolvedSlot].Variant != Asset)
	{
		return FReply::Handled();
	}
	FScopedTransaction Transaction(LOCTEXT("MakeUniqueTransaction", "Make Voxel Paint Asset Unique"));
	UVoxelPaintVolumeData* NewAsset = WorldVoxelPaint::DeepCopyVolumeDataForPlacement(Asset, Placement->GetOwner());
	if (!NewAsset)
	{
		FVoxelPaintToolsContext::NotifyUser(LOCTEXT("MakeUniqueFailed",
			"Make Unique failed to create the copied asset."));
		return FReply::Handled();
	}
	Placement->CapturePreEditPoolState();
	Placement->Modify();
	Placement->VolumeVariants[ResolvedSlot].Variant = NewAsset;
	FPropertyChangedEvent ChangedEvent(UVoxelPaintVolumeComponent::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VolumeVariants)));
	Placement->PostEditChangeProperty(ChangedEvent);
	return FReply::Handled();
}

FReply FVoxelPaintModeToolkit::OnMarkSharedClicked() const
{
	UVoxelPaintVolumeData* Asset = GetSelectedAsset();
	if (!Asset || !EnsureAuthorityGatePasses(Asset))
	{
		return FReply::Handled();
	}
	FScopedTransaction Transaction(LOCTEXT("MarkSharedTransaction", "Mark Voxel Paint Asset Shared"));
	WorldVoxelPaint::MarkShared(Asset);
	return FReply::Handled();
}

FReply FVoxelPaintModeToolkit::OnMarkPrivateClicked() const
{
	UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	UVoxelPaintVolumeData* Asset = Placement ? Placement->ResolveActiveVariant() : nullptr;
	if (!Placement || !Asset || IsPrefabProvenance(Placement) || !EnsureAuthorityGatePasses(Asset))
	{
		return FReply::Handled();
	}
	FScopedTransaction Transaction(LOCTEXT("MarkPrivateTransaction", "Mark Voxel Paint Asset Private"));
	WorldVoxelPaint::MarkPrivate(Asset, Placement->GetOwner());
	return FReply::Handled();
}

bool FVoxelPaintModeToolkit::CanTakeOwnership() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	const UVoxelPaintVolumeData* Asset = Placement ? Placement->ResolveActiveVariant() : nullptr;
	if (!Placement || !Asset || IsPrefabProvenance(Placement))
	{
		return false;
	}
	FText WhyNot;
	if (!PassesAuthorityGate(Asset, WhyNot))
	{
		return false;
	}
	return WorldVoxelPaint::CheckEditingAuthority(Asset, Placement->GetOwner())
		!= WorldVoxelPaint::EVoxelPaintAuthorityVerdict::Allowed;
}

bool FVoxelPaintModeToolkit::CanMakeUnique() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	const UVoxelPaintVolumeData* Asset = Placement ? Placement->ResolveActiveVariant() : nullptr;
	if (!Asset || IsPrefabProvenance(Placement))
	{
		return false;
	}
	FText WhyNot;
	return PassesAuthorityGate(Asset, WhyNot);
}

bool FVoxelPaintModeToolkit::CanMarkShared() const
{
	const UVoxelPaintVolumeData* Asset = GetSelectedAsset();
	if (!Asset || Asset->SharingMode != EVoxelPaintAssetSharingMode::Private)
	{
		return false;
	}
	FText WhyNot;
	return PassesAuthorityGate(Asset, WhyNot);
}

bool FVoxelPaintModeToolkit::CanMarkPrivate() const
{
	const UVoxelPaintVolumeData* Asset = GetSelectedAsset();
	if (!Asset || Asset->SharingMode != EVoxelPaintAssetSharingMode::Shared
		|| IsPrefabProvenance(GetSelectedPlacement()))
	{
		return false;
	}
	FText WhyNot;
	return PassesAuthorityGate(Asset, WhyNot);
}

EVisibility FVoxelPaintModeToolkit::GetTakeOwnershipVisibility() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	const UVoxelPaintVolumeData* Asset = Placement ? Placement->ResolveActiveVariant() : nullptr;
	const bool bApplicable = Placement && Asset
		&& WorldVoxelPaint::CheckEditingAuthority(Asset, Placement->GetOwner())
			!= WorldVoxelPaint::EVoxelPaintAuthorityVerdict::Allowed;
	return bApplicable ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility FVoxelPaintModeToolkit::GetMarkSharedVisibility() const
{
	const UVoxelPaintVolumeData* Asset = GetSelectedAsset();
	return (Asset && Asset->SharingMode == EVoxelPaintAssetSharingMode::Private)
		? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility FVoxelPaintModeToolkit::GetMarkPrivateVisibility() const
{
	const UVoxelPaintVolumeData* Asset = GetSelectedAsset();
	return (Asset && Asset->SharingMode == EVoxelPaintAssetSharingMode::Shared)
		? EVisibility::Visible : EVisibility::Collapsed;
}

bool FVoxelPaintModeToolkit::PassesAuthorityGate(const UVoxelPaintVolumeData* Asset, FText& OutWhyNot) const
{
	const FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
	return !Controller || Controller->GateAuthorityTransition(Asset, OutWhyNot);
}

bool FVoxelPaintModeToolkit::EnsureAuthorityGatePasses(const UVoxelPaintVolumeData* Asset) const
{
	FText WhyNot;
	if (!PassesAuthorityGate(Asset, WhyNot))
	{
		FVoxelPaintToolsContext::NotifyUser(WhyNot);
		return false;
	}
	return true;
}

FText FVoxelPaintModeToolkit::GetAuthorityGateToolTipText(bool bProvenanceGated) const
{
	if (bProvenanceGated && IsPrefabProvenance(GetSelectedPlacement()))
	{
		return LOCTEXT("CustodyProvenanceGateReason",
			"Shared prefab paint data - pool custody belongs to the Blueprint; "
			"use the variant pin or Duplicate Variant instead.");
	}
	const UVoxelPaintVolumeData* Asset = GetSelectedAsset();
	FText WhyNot;
	if (Asset && !PassesAuthorityGate(Asset, WhyNot))
	{
		return WhyNot;
	}
	return FText::GetEmpty();
}

FText FVoxelPaintModeToolkit::GetDanglingPinText() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	if (!Placement || !Placement->VariantOverride)
	{
		return FText::GetEmpty();
	}
	return FText::Format(LOCTEXT("DanglingPinFormat",
		"Pin dangling - '{0}' is not in this pool; showing the deterministic roll."),
		FText::FromString(Placement->VariantOverride->GetName()));
}

EVisibility FVoxelPaintModeToolkit::GetDanglingPinVisibility() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	const bool bDangling = Placement && Placement->VariantOverride
		&& !Placement->PoolContains(Placement->VariantOverride);
	return bDangling ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility FVoxelPaintModeToolkit::GetScaleNoteVisibility() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	if (!Placement)
	{
		return EVisibility::Collapsed;
	}
	const FVector Scale = Placement->GetComponentTransform().GetScale3D();
	return Scale.Equals(FVector::OneVector, 1.0e-3) ? EVisibility::Collapsed : EVisibility::Visible;
}

TSharedRef<SWidget> FVoxelPaintModeToolkit::MakeForcePreviewMenu() const
{
	FMenuBuilder MenuBuilder(true, nullptr);
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	if (!Placement)
	{
		return MenuBuilder.MakeWidget();
	}

	MenuBuilder.AddMenuEntry(
		LOCTEXT("ForcePreviewNoneEntry", "None (resolved)"),
		LOCTEXT("ForcePreviewNoneEntryToolTip",
			"Clear the forced preview; the placement shows its pin or the deterministic roll."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([this]()
			{
				OnForcePreviewPicked(TWeakObjectPtr<const UVoxelPaintVolumeData>());
			}),
			FCanExecuteAction(),
			FIsActionChecked::CreateLambda([this]()
			{
				return GetForcedVariantForSelected() == nullptr;
			})),
		NAME_None, EUserInterfaceActionType::RadioButton);

	TSet<const UVoxelPaintVolumeData*> Seen;
	for (int32 Index = 0; Index < Placement->VolumeVariants.Num(); ++Index)
	{
		const UVoxelPaintVolumeData* Variant = Placement->VolumeVariants[Index].Variant;
		if (!Variant || Seen.Contains(Variant))
		{
			continue;
		}
		Seen.Add(Variant);
		FText WhyNot;
		const bool bAllowed = FVoxelPaintForcePreviewPolicy::CanForceVariant(Variant, WhyNot);
		TWeakObjectPtr<const UVoxelPaintVolumeData> WeakVariant(Variant);
		MenuBuilder.AddMenuEntry(
			FText::Format(LOCTEXT("ForcePreviewEntryFormat", "Variant {0} - {1}"),
				FText::AsNumber(Index + 1), FText::FromString(Variant->GetName())),
			bAllowed
				? LOCTEXT("ForcePreviewEntryToolTip", "Force this placement to display this variant.")
				: WhyNot,
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([this, WeakVariant]()
				{
					OnForcePreviewPicked(WeakVariant);
				}),
				FCanExecuteAction::CreateLambda([bAllowed]() { return bAllowed; }),
				FIsActionChecked::CreateLambda([this, WeakVariant]()
				{
					return WeakVariant.IsValid() && GetForcedVariantForSelected() == WeakVariant.Get();
				})),
			NAME_None, EUserInterfaceActionType::RadioButton);
	}
	return MenuBuilder.MakeWidget();
}

FText FVoxelPaintModeToolkit::GetForcePreviewValueText() const
{
	const UVoxelPaintVolumeData* Forced = GetForcedVariantForSelected();
	if (!Forced)
	{
		return LOCTEXT("ForcePreviewValueNone", "None (resolved)");
	}
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	if (Placement && !Placement->PoolContains(Forced))
	{
		return FText::Format(LOCTEXT("ForcePreviewValueDroppedFormat", "{0} (not in pool - ignored)"),
			FText::FromString(Forced->GetName()));
	}
	return FText::FromString(Forced->GetName());
}

FText FVoxelPaintModeToolkit::GetForcePreviewToolTipText() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	if (!Placement)
	{
		return LOCTEXT("ForcePreviewToolTipNoSelection",
			"Select a voxel paint volume to force which pool variant it displays.");
	}
	if (!CanUseForcePreview())
	{
		return LOCTEXT("ForcePreviewToolTipWaiter",
			"Frontier full - this placement is not rendering; free a slot or paint a rendered copy.");
	}
	return LOCTEXT("ForcePreviewToolTip",
		"Force which pool variant this placement displays. Transient: never saved, never written "
		"to the pin or the pool, cleared on mode exit and session end.");
}

bool FVoxelPaintModeToolkit::CanUseForcePreview() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	if (!Placement)
	{
		return false;
	}
	const UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(Placement->GetWorld());
	return Subsystem && Subsystem->GetFrontierPosition(Placement) != INDEX_NONE;
}

void FVoxelPaintModeToolkit::OnForcePreviewPicked(TWeakObjectPtr<const UVoxelPaintVolumeData> Variant) const
{
	UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	FText WhyNot;
	if (!FVoxelPaintForcePreviewPolicy::ApplyForcedVariant(Placement, Variant.Get(),
			bForcePreviewAllCopies, WhyNot)
		&& !WhyNot.IsEmpty())
	{
		FVoxelPaintToolsContext::NotifyUser(WhyNot);
	}
}

const UVoxelPaintVolumeData* FVoxelPaintModeToolkit::GetForcedVariantForSelected() const
{
	const UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	if (!Placement)
	{
		return nullptr;
	}
	const UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(Placement->GetWorld());
	if (!Subsystem)
	{
		return nullptr;
	}
	FVoxelPaintPlacementState State;
	if (!Subsystem->GetCachedPlacementState(Placement, State))
	{
		return nullptr;
	}
	FObjectKey ForcedKey;
	if (!Subsystem->GetForcedVariant(State.Identity, ForcedKey))
	{
		return nullptr;
	}
	return Cast<UVoxelPaintVolumeData>(ForcedKey.ResolveObjectPtr());
}

bool FVoxelPaintModeToolkit::IsPrefabProvenance(const UVoxelPaintVolumeComponent* Placement)
{
	return Placement && !Placement->IsTemplate()
		&& Placement->CreationMethod == EComponentCreationMethod::SimpleConstructionScript;
}

UVoxelPaintVolumeComponent* FVoxelPaintModeToolkit::GetSelectedPlacement()
{
	return FVoxelPaintToolsContext::FindSelectedPlacement();
}

UVoxelPaintVolumeData* FVoxelPaintModeToolkit::GetSelectedAsset()
{
	UVoxelPaintVolumeComponent* Placement = GetSelectedPlacement();
	return Placement ? Placement->ResolveActiveVariant() : nullptr;
}

#undef LOCTEXT_NAMESPACE
