// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintVolumeComponentDetails.h"

#include "AssetRegistry/AssetData.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IPropertyUtilities.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyHandle.h"
#include "ScopedTransaction.h"
#include "Styling/StyleColors.h"
#include "VoxelPaintCanonicalSource.h"
#include "VoxelPaintTools.h"
#include "VoxelPaintVariantSelection.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "WorldVoxelPaintConstants.h"

#define LOCTEXT_NAMESPACE "VoxelPaintEditorMode"

namespace
{

WorldVoxelPaint::FVoxelPaintPoolView MakePoolView(const UVoxelPaintVolumeComponent& Component)
{
	WorldVoxelPaint::FVoxelPaintPoolView View;
	for (const FVoxelPaintVariantPoolEntry& Entry : Component.VolumeVariants)
	{
		View.Assets.Add(Entry.Variant);
		View.Weights.Add(Entry.Weight);
	}
	return View;
}

bool PoolHasValidEntry(const UVoxelPaintVolumeComponent* Component)
{
	if (!Component)
	{
		return false;
	}
	for (const FVoxelPaintVariantPoolEntry& Entry : Component->VolumeVariants)
	{
		if (Entry.Variant)
		{
			return true;
		}
	}
	return false;
}

bool CanCreatePaintData(const WorldVoxelPaint::FVoxelPaintPoolAuthoringContext& Context,
	FText& OutReason)
{
	if (!Context.bPoolActionsEnabled)
	{
		OutReason = Context.DisabledReason;
		return false;
	}
	if (PoolHasValidEntry(Context.WriteTarget))
	{
		OutReason = LOCTEXT("CreateDisabledNonEmpty",
			"The pool already has paint data - use Add Variant instead.");
		return false;
	}
	if (!Context.FreshVariantDisabledReason.IsEmpty())
	{
		OutReason = Context.FreshVariantDisabledReason;
		return false;
	}
	return true;
}

bool CanAddVariant(const WorldVoxelPaint::FVoxelPaintPoolAuthoringContext& Context, FText& OutReason)
{
	if (!Context.bPoolActionsEnabled)
	{
		OutReason = Context.DisabledReason;
		return false;
	}
	if (!PoolHasValidEntry(Context.WriteTarget))
	{
		OutReason = LOCTEXT("AddDisabledEmpty", "The pool is empty - use Create Paint Data first.");
		return false;
	}
	if (!Context.FreshVariantDisabledReason.IsEmpty())
	{
		OutReason = Context.FreshVariantDisabledReason;
		return false;
	}
	return true;
}

bool CanDuplicateSlot(const WorldVoxelPaint::FVoxelPaintPoolAuthoringContext& Context, int32 Slot,
	FText& OutReason)
{
	if (!Context.bPoolActionsEnabled || !Context.WriteTarget)
	{
		OutReason = Context.DisabledReason;
		return false;
	}
	const UVoxelPaintVolumeComponent* Target = Context.WriteTarget;
	if (!Target->VolumeVariants.IsValidIndex(Slot) || !Target->VolumeVariants[Slot].Variant)
	{
		OutReason = LOCTEXT("DuplicateDisabledNoSource",
			"This slot has no variant asset to duplicate.");
		return false;
	}
	if (!Target->ExpectedTopology.bIsSet)
	{
		OutReason = LOCTEXT("DuplicateDisabledPristine",
			"The pool has no expected topology yet - Create Paint Data records it first.");
		return false;
	}
	if (!Target->ExpectedTopology.Matches(*Target->VolumeVariants[Slot].Variant))
	{
		OutReason = FText::Format(LOCTEXT("DuplicateDisabledMismatch",
			"'{0}' does not match the pool's expected topology - fix the topology mismatch first."),
			FText::FromString(Target->VolumeVariants[Slot].Variant->GetName()));
		return false;
	}
	return true;
}

bool CanRemoveSlot(const WorldVoxelPaint::FVoxelPaintPoolAuthoringContext& Context, int32 Slot,
	FText& OutReason)
{
	if (!Context.bPoolActionsEnabled || !Context.WriteTarget)
	{
		OutReason = Context.DisabledReason;
		return false;
	}
	if (!Context.WriteTarget->VolumeVariants.IsValidIndex(Slot))
	{
		OutReason = LOCTEXT("RemoveDisabledNoSlot", "The pool slot no longer exists.");
		return false;
	}
	return true;
}

} // namespace

TSharedRef<IDetailCustomization> FVoxelPaintVolumeComponentDetails::MakeInstance()
{
	return MakeShareable(new FVoxelPaintVolumeComponentDetails);
}

void FVoxelPaintVolumeComponentDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	PropertyUtilities = DetailBuilder.GetPropertyUtilities();

	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	TArray<UVoxelPaintVolumeComponent*> Components;
	for (const TWeakObjectPtr<UObject>& Object : Objects)
	{
		if (UVoxelPaintVolumeComponent* Candidate = Cast<UVoxelPaintVolumeComponent>(Object.Get()))
		{
			Components.Add(Candidate);
		}
	}

	DetailBuilder.EditCategory("Voxel Paint", FText::GetEmpty(), ECategoryPriority::Important);
	IDetailCategoryBuilder& PoolCategory = DetailBuilder.EditCategory("Voxel Paint - Pool",
		LOCTEXT("PoolCategoryDisplay", "Voxel Paint - Pool"), ECategoryPriority::Important);

	if (Components.Num() != 1)
	{
		PoolCategory.AddCustomRow(LOCTEXT("PoolMultiSelectFilter", "Pool"))
		.WholeRowContent()
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.Text(LOCTEXT("PoolMultiSelect",
				"Select a single voxel paint component to manage its variant pool."))
		];
		return;
	}
	UVoxelPaintVolumeComponent* Component = Components[0];
	CustomizedComponent = Component;

	PoolCategory.AddCustomRow(LOCTEXT("PoolStatusFilter", "Pool Status"))
	.Visibility(TAttribute<EVisibility>::CreateLambda([this]()
	{
		return GetPoolStatusText().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
	}))
	.WholeRowContent()
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.Margin(FMargin(2.0f, 4.0f))
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.ColorAndOpacity(FStyleColors::Warning)
		.Text_Lambda([this]() { return GetPoolStatusText(); })
	];

	PoolCategory.AddCustomRow(LOCTEXT("PoolActionsFilter", "Pool Actions"))
	.WholeRowContent()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 2.0f, 4.0f, 4.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("CreatePaintDataButton", "Create Paint Data"))
			.ToolTipText_Lambda([this]()
			{
				FText Reason;
				if (!CanCreatePaintData(GetContext(), Reason))
				{
					return Reason;
				}
				return LOCTEXT("CreatePaintDataToolTip",
					"Create a fresh paint asset for this pool: sized from the mesh-geometry bounds, "
					"recorded as the pool's expected topology, and assigned to the first slot. "
					"Prefab pools create a Shared asset beside the Blueprint; placement-owned pools "
					"create a Private asset beside the map.");
			})
			.IsEnabled_Lambda([this]()
			{
				FText Reason;
				return CanCreatePaintData(GetContext(), Reason);
			})
			.OnClicked_Lambda([this]()
			{
				FText Error;
				NotifyAndRefresh(WorldVoxelPaint::CreatePoolPaintData(GetContext(), Error), Error);
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 2.0f, 0.0f, 4.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("AddVariantButton", "Add Variant"))
			.ToolTipText_Lambda([this]()
			{
				FText Reason;
				if (!CanAddVariant(GetContext(), Reason))
				{
					return Reason;
				}
				return LOCTEXT("AddVariantToolTip",
					"Append a fresh unpainted variant asset matching the pool's expected topology.");
			})
			.IsEnabled_Lambda([this]()
			{
				FText Reason;
				return CanAddVariant(GetContext(), Reason);
			})
			.OnClicked_Lambda([this]()
			{
				FText Error;
				NotifyAndRefresh(WorldVoxelPaint::AddPoolVariant(GetContext(), Error), Error);
				return FReply::Handled();
			})
		]
	];

	const bool bSCSSuppliedPool = Component->IsTemplate()
		? Component->GetTypedOuter<UBlueprintGeneratedClass>() != nullptr
		: Component->CreationMethod == EComponentCreationMethod::SimpleConstructionScript;
	const FText VariantNameToolTip = bSCSSuppliedPool
		? LOCTEXT("VariantRowToolTipScs",
			"Pool entry. The picker excludes assets already in the pool and Private assets - "
			"mark an asset Shared deliberately, or duplicate it and mark the copy Shared, to use "
			"it in a prefab pool.")
		: LOCTEXT("VariantRowToolTip",
			"Pool entry. The picker excludes assets already in the pool.");

	TSharedRef<IPropertyHandle> PoolHandle = DetailBuilder.GetProperty(
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VolumeVariants));
	TSharedPtr<IPropertyHandleArray> PoolArray = PoolHandle->AsArray();
	uint32 NumElements = 0;
	if (PoolArray.IsValid())
	{
		PoolArray->GetNumElements(NumElements);
	}
	const int32 NumEntries = FMath::Min(Component->VolumeVariants.Num(), (int32)NumElements);
	for (int32 SlotIndex = 0; SlotIndex < NumEntries; ++SlotIndex)
	{
		TSharedPtr<IPropertyHandle> VariantHandle = PoolArray->GetElement(SlotIndex)->GetChildHandle(
			GET_MEMBER_NAME_CHECKED(FVoxelPaintVariantPoolEntry, Variant));
		PoolCategory.AddCustomRow(
			FText::Format(LOCTEXT("VariantRowFilter", "Variant {0}"), SlotIndex + 1))
		.NameContent()
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.Text(FText::Format(LOCTEXT("VariantRowLabel", "Variant {0}"), SlotIndex + 1))
			.ToolTipText(VariantNameToolTip)
		]
		.ValueContent()
		.MinDesiredWidth(250.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(SObjectPropertyEntryBox)
				.PropertyHandle(VariantHandle)
				.AllowedClass(UVoxelPaintVolumeData::StaticClass())
				.ThumbnailPool(DetailBuilder.GetThumbnailPool())
				.DisplayThumbnail(false)
				.OnShouldFilterAsset(FOnShouldFilterAsset::CreateLambda(
					[this, SlotIndex](const FAssetData& AssetData)
					{
						return ShouldFilterPoolAsset(AssetData, SlotIndex);
					}))
				.IsEnabled(TAttribute<bool>::CreateLambda([this]()
				{
					return IsSlotAssetEditable();
				}))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.Text_Lambda([this, SlotIndex]() { return GetVariantInfoText(SlotIndex); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("DuplicateVariantButton", "Duplicate Variant"))
					.ToolTipText_Lambda([this, SlotIndex]()
					{
						FText Reason;
						if (!CanDuplicateSlot(GetContext(), SlotIndex, Reason))
						{
							return Reason;
						}
						return LOCTEXT("DuplicateVariantToolTip",
							"Fork this variant's paint into a new pool entry with its own asset.");
					})
					.IsEnabled_Lambda([this, SlotIndex]()
					{
						FText Reason;
						return CanDuplicateSlot(GetContext(), SlotIndex, Reason);
					})
					.OnClicked_Lambda([this, SlotIndex]()
					{
						FText Error;
						NotifyAndRefresh(
							WorldVoxelPaint::DuplicatePoolVariant(GetContext(), SlotIndex, Error),
							Error);
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("RemoveVariantButton", "Remove Variant"))
					.ToolTipText_Lambda([this, SlotIndex]()
					{
						FText Reason;
						if (!CanRemoveSlot(GetContext(), SlotIndex, Reason))
						{
							return Reason;
						}
						return LOCTEXT("RemoveVariantToolTip",
							"Remove this pool entry. The variant asset itself is not deleted.");
					})
					.IsEnabled_Lambda([this, SlotIndex]()
					{
						FText Reason;
						return CanRemoveSlot(GetContext(), SlotIndex, Reason);
					})
					.OnClicked_Lambda([this, SlotIndex]()
					{
						FText Error;
						NotifyAndRefresh(
							WorldVoxelPaint::RemovePoolVariant(GetContext(), SlotIndex, Error),
							Error);
						return FReply::Handled();
					})
				]
			]
		];
	}

	if (!Component->IsTemplate())
	{
		PinPropertyHandle = DetailBuilder.GetProperty(
			GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VariantOverride));
		PoolCategory.AddCustomRow(LOCTEXT("VariantPinFilter", "Variant Pin"))
		.NameContent()
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.Text(LOCTEXT("VariantPinLabel", "Variant Pin"))
			.ToolTipText(LOCTEXT("VariantPinToolTip",
				"Pins this placement to one pool variant (unset = deterministic roll). A pin whose "
				"asset leaves the pool dangles: the placement falls back to the roll and the "
				"toolkit and validator warn."))
		]
		.ValueContent()
		.MinDesiredWidth(250.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 2.0f, 4.0f, 4.0f)
			[
				SNew(SComboButton)
				.OnGetMenuContent_Lambda([this]() { return MakePinMenu(); })
				.ButtonContent()
				[
					SNew(STextBlock)
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.Text_Lambda([this]() { return GetPinValueText(); })
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 2.0f, 0.0f, 4.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("RerollButton", "Reroll"))
				.ToolTipText(LOCTEXT("RerollToolTip",
					"Pin this placement to a uniformly random valid pool variant."))
				.IsEnabled_Lambda([this]()
				{
					const UVoxelPaintVolumeComponent* Pinned = CustomizedComponent.Get();
					if (!Pinned)
					{
						return false;
					}
					TArray<int32> ValidSlots;
					WorldVoxelPaint::ComputeValidSlots(MakePoolView(*Pinned), ValidSlots);
					return ValidSlots.Num() > 0;
				})
				.OnClicked_Lambda([this]() { return OnRerollClicked(); })
			]
		];
	}

	if (!Component->IsTemplate()
		&& Component->CreationMethod == EComponentCreationMethod::SimpleConstructionScript)
	{
		PoolCategory.AddCustomRow(LOCTEXT("ResetToTemplateFilter", "Reset to Template"))
		.Visibility(TAttribute<EVisibility>::CreateLambda([this]()
		{
			return IsPoolDivergedFromArchetype() ? EVisibility::Visible : EVisibility::Collapsed;
		}))
		.WholeRowContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 2.0f, 0.0f, 4.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("ResetToTemplateButton", "Reset to Template"))
				.ToolTipText(LOCTEXT("ResetToTemplateToolTip",
					"This placement's pool or expected topology diverges from its Blueprint "
					"template - restore both from the template."))
				.OnClicked_Lambda([this]() { return OnResetToTemplateClicked(); })
			]
		];
	}
}

WorldVoxelPaint::FVoxelPaintPoolAuthoringContext FVoxelPaintVolumeComponentDetails::GetContext() const
{
	return WorldVoxelPaint::ResolvePoolAuthoringContext(CustomizedComponent.Get());
}

void FVoxelPaintVolumeComponentDetails::RequestRefresh()
{
	if (PropertyUtilities.IsValid())
	{
		TSharedPtr<IPropertyUtilities> Utilities = PropertyUtilities;
		Utilities->EnqueueDeferredAction(FSimpleDelegate::CreateLambda([Utilities]()
		{
			Utilities->ForceRefresh();
		}));
	}
}

void FVoxelPaintVolumeComponentDetails::NotifyAndRefresh(bool bSucceeded, const FText& Error)
{
	if (!bSucceeded)
	{
		FVoxelPaintToolsContext::NotifyUser(Error);
		return;
	}
	RequestRefresh();
}

FText FVoxelPaintVolumeComponentDetails::GetPoolStatusText() const
{
	return GetContext().DisabledReason;
}

FText FVoxelPaintVolumeComponentDetails::GetVariantInfoText(int32 SlotIndex) const
{
	const UVoxelPaintVolumeComponent* Component = CustomizedComponent.Get();
	if (!Component || !Component->VolumeVariants.IsValidIndex(SlotIndex))
	{
		return FText::GetEmpty();
	}
	const UVoxelPaintVolumeData* Variant = Component->VolumeVariants[SlotIndex].Variant;
	if (!Variant)
	{
		return LOCTEXT("VariantInfoEmptySlot", "Empty slot - assign or create a variant asset.");
	}
	if (!Variant->bInitialized)
	{
		return LOCTEXT("VariantInfoUnpainted", "Unpainted");
	}
	const int32 NumBricks = Variant->BrickManifest.PopulatedBricks.Num();
	const FIntVector DimsInBricks = WorldVoxelPaint::ComputePackedSourceDimsInBricks(NumBricks);
	const FIntVector PhysTexels = DimsInBricks * WorldVoxelPaint::BrickSize;
	const int64 SourceBytes = (int64)PhysTexels.X * PhysTexels.Y * PhysTexels.Z * 4;
	return FText::Format(LOCTEXT("VariantInfoPaintedFormat", "Painted - ~{0} MB source ({1} bricks)"),
		FText::FromString(FString::Printf(TEXT("%.1f"), (double)SourceBytes / (1024.0 * 1024.0))),
		FText::AsNumber(NumBricks));
}

FText FVoxelPaintVolumeComponentDetails::GetPinValueText() const
{
	const UVoxelPaintVolumeComponent* Component = CustomizedComponent.Get();
	if (!Component)
	{
		return FText::GetEmpty();
	}
	const UVoxelPaintVolumeData* Pin = Component->VariantOverride;
	if (!Pin)
	{
		return LOCTEXT("PinUnset", "Unset (random)");
	}
	for (int32 SlotIndex = 0; SlotIndex < Component->VolumeVariants.Num(); ++SlotIndex)
	{
		if (Component->VolumeVariants[SlotIndex].Variant == Pin)
		{
			return FText::Format(LOCTEXT("PinSlotFormat", "Variant {0} - {1}"),
				FText::AsNumber(SlotIndex + 1), FText::FromString(Pin->GetName()));
		}
	}
	return FText::Format(LOCTEXT("PinDanglingFormat", "Pin dangling - {0}"),
		FText::FromString(Pin->GetName()));
}

TSharedRef<SWidget> FVoxelPaintVolumeComponentDetails::MakePinMenu()
{
	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);
	MenuBuilder.AddMenuEntry(
		LOCTEXT("PinMenuUnset", "Unset (random)"),
		LOCTEXT("PinMenuUnsetToolTip", "Clear the pin; the placement resolves by deterministic roll."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this]() { SetPin(nullptr); })));

	const UVoxelPaintVolumeComponent* Component = CustomizedComponent.Get();
	if (Component)
	{
		TArray<int32> ValidSlots;
		WorldVoxelPaint::ComputeValidSlots(MakePoolView(*Component), ValidSlots);
		for (const int32 SlotIndex : ValidSlots)
		{
			const UVoxelPaintVolumeData* Asset = Component->VolumeVariants[SlotIndex].Variant;
			MenuBuilder.AddMenuEntry(
				FText::Format(LOCTEXT("PinMenuSlotFormat", "Variant {0} - {1}"),
					FText::AsNumber(SlotIndex + 1), FText::FromString(Asset->GetName())),
				LOCTEXT("PinMenuSlotToolTip", "Pin this placement to this pool variant."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([this, Asset]() { SetPin(Asset); })));
		}
	}
	return MenuBuilder.MakeWidget();
}

void FVoxelPaintVolumeComponentDetails::SetPin(const UVoxelPaintVolumeData* Asset)
{
	if (PinPropertyHandle.IsValid())
	{
		PinPropertyHandle->SetValue(static_cast<const UObject*>(Asset));
	}
}

FReply FVoxelPaintVolumeComponentDetails::OnRerollClicked()
{
	const UVoxelPaintVolumeComponent* Component = CustomizedComponent.Get();
	if (!Component)
	{
		return FReply::Handled();
	}
	TArray<int32> ValidSlots;
	WorldVoxelPaint::ComputeValidSlots(MakePoolView(*Component), ValidSlots);
	if (ValidSlots.Num() == 0)
	{
		return FReply::Handled();
	}
	const int32 Rolled = ValidSlots[FMath::RandRange(0, ValidSlots.Num() - 1)];
	SetPin(Component->VolumeVariants[Rolled].Variant);
	return FReply::Handled();
}

FReply FVoxelPaintVolumeComponentDetails::OnResetToTemplateClicked()
{
	UVoxelPaintVolumeComponent* Component = CustomizedComponent.Get();
	if (Component)
	{
		FScopedTransaction Transaction(
			LOCTEXT("ResetToTemplateTransaction", "Reset Voxel Paint Pool to Template"));
		Component->ResetPoolToArchetype();
		RequestRefresh();
	}
	return FReply::Handled();
}

bool FVoxelPaintVolumeComponentDetails::ShouldFilterPoolAsset(const FAssetData& AssetData,
	int32 SlotIndex) const
{
	const UVoxelPaintVolumeComponent* Component = CustomizedComponent.Get();
	if (!Component)
	{
		return true;
	}
	const FSoftObjectPath AssetPath = AssetData.ToSoftObjectPath();
	for (int32 OtherSlot = 0; OtherSlot < Component->VolumeVariants.Num(); ++OtherSlot)
	{
		if (OtherSlot == SlotIndex)
		{
			continue;
		}
		const UVoxelPaintVolumeData* Other = Component->VolumeVariants[OtherSlot].Variant;
		if (Other && FSoftObjectPath(Other) == AssetPath)
		{
			return true;
		}
	}
	const bool bSCSSuppliedPool = Component->IsTemplate()
		? Component->GetTypedOuter<UBlueprintGeneratedClass>() != nullptr
		: Component->CreationMethod == EComponentCreationMethod::SimpleConstructionScript;
	if (bSCSSuppliedPool)
	{
		if (const UVoxelPaintVolumeData* Loaded =
			Cast<UVoxelPaintVolumeData>(AssetData.FastGetAsset(/*bLoad=*/false)))
		{
			return Loaded->SharingMode == EVoxelPaintAssetSharingMode::Private;
		}
		FString SharingModeTag;
		if (AssetData.GetTagValue(GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeData, SharingMode),
			SharingModeTag))
		{
			const int64 TagValue =
				StaticEnum<EVoxelPaintAssetSharingMode>()->GetValueByNameString(SharingModeTag);
			return TagValue == INDEX_NONE
				|| TagValue == static_cast<int64>(EVoxelPaintAssetSharingMode::Private);
		}
		// Unloaded and untagged (saved before SharingMode became registry-searchable): the
		// Private exclusion cannot be evaluated, so exclude rather than offer a maybe-Private
		// asset to an SCS-supplied pool.
		return true;
	}
	return false;
}

bool FVoxelPaintVolumeComponentDetails::IsSlotAssetEditable() const
{
	const UVoxelPaintVolumeComponent* Component = CustomizedComponent.Get();
	if (!Component)
	{
		return false;
	}
	const FProperty* PoolProperty = UVoxelPaintVolumeComponent::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VolumeVariants));
	return Component->CanEditChange(PoolProperty);
}

bool FVoxelPaintVolumeComponentDetails::IsPoolDivergedFromArchetype() const
{
	const UVoxelPaintVolumeComponent* Component = CustomizedComponent.Get();
	if (!Component || Component->IsTemplate()
		|| Component->CreationMethod != EComponentCreationMethod::SimpleConstructionScript)
	{
		return false;
	}
	const UVoxelPaintVolumeComponent* Archetype =
		Cast<UVoxelPaintVolumeComponent>(Component->GetArchetype());
	if (!Archetype)
	{
		return false;
	}
	return !(Component->VolumeVariants == Archetype->VolumeVariants)
		|| !(Component->ExpectedTopology == Archetype->ExpectedTopology);
}

#undef LOCTEXT_NAMESPACE
