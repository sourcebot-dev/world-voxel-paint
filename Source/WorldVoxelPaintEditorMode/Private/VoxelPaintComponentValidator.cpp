// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintComponentValidator.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/Level.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/DataValidation.h"
#include "Templates/Casts.h"
#include "Templates/Function.h"
#include "UObject/UObjectGlobals.h"
#include "VoxelPaintEditorWorldFilter.h"

#define LOCTEXT_NAMESPACE "WorldVoxelPaint"

namespace WorldVoxelPaint
{

void BuildComponentRuleInputs(const UVoxelPaintVolumeComponent& Component,
	FVoxelPaintComponentRuleInputs& OutInputs)
{
	OutInputs = FVoxelPaintComponentRuleInputs();

	const AActor* Owner = Component.GetOwner();
	const FGuid OwnerGuid = Owner ? Owner->GetActorGuid() : FGuid();

	OutInputs.Pool.Assets.Reserve(Component.VolumeVariants.Num());
	OutInputs.Pool.Weights.Reserve(Component.VolumeVariants.Num());
	OutInputs.Entries.Reserve(Component.VolumeVariants.Num());
	for (const FVoxelPaintVariantPoolEntry& PoolEntry : Component.VolumeVariants)
	{
		const UVoxelPaintVolumeData* Asset = PoolEntry.Variant.Get();
		OutInputs.Pool.Assets.Add(Asset);
		OutInputs.Pool.Weights.Add(PoolEntry.Weight);

		FVoxelPaintComponentRuleEntry& RuleEntry = OutInputs.Entries.AddDefaulted_GetRef();
		if (Asset)
		{
			RuleEntry.bHasAsset = true;
			RuleEntry.AssetName = Asset->GetName();
			RuleEntry.Topology.LocalHalfExtent = Asset->LocalHalfExtent;
			RuleEntry.Topology.BrickCounts = Asset->BrickCounts;
			RuleEntry.Topology.StorageOrigin = Asset->StorageOrigin;
			RuleEntry.SharingMode = Asset->SharingMode;
			RuleEntry.bOwnedByThisPlacement = OwnerGuid.IsValid() && Asset->OwnerActorGuid.IsValid()
				&& Asset->OwnerActorGuid == OwnerGuid;
			RuleEntry.OwnerDisplayName = Asset->OwnerDisplayName;
		}
	}

	OutInputs.Expectation = Component.ExpectedTopology;

	OutInputs.bPinSet = Component.VariantOverride != nullptr;
	OutInputs.bPinInPool = Component.PoolContains(Component.VariantOverride);
	OutInputs.PinAssetName = Component.VariantOverride ? Component.VariantOverride->GetName() : FString();

	OutInputs.CreationMethod = Component.CreationMethod;
	OutInputs.bIsTemplate = Component.IsTemplate();
	OutInputs.bOwnerClassIsBlueprintGeneratedClass =
		Owner && Cast<UBlueprintGeneratedClass>(Owner->GetClass()) != nullptr;

	if (const UVoxelPaintVolumeComponent* Archetype =
		Cast<UVoxelPaintVolumeComponent>(Component.GetArchetype()))
	{
		OutInputs.bHasArchetype = true;
		OutInputs.ArchetypeAssets.Reserve(Archetype->VolumeVariants.Num());
		OutInputs.ArchetypeWeights.Reserve(Archetype->VolumeVariants.Num());
		for (const FVoxelPaintVariantPoolEntry& ArchetypeEntry : Archetype->VolumeVariants)
		{
			OutInputs.ArchetypeAssets.Add(ArchetypeEntry.Variant.Get());
			OutInputs.ArchetypeWeights.Add(ArchetypeEntry.Weight);
		}
		OutInputs.ArchetypeExpectation = Archetype->ExpectedTopology;
	}

	if (Component.IsRegistered())
	{
		const FVector Scale = Component.GetComponentTransform().GetScale3D();
		OutInputs.MaxScaleDeviation = static_cast<float>(FMath::Max3(
			FMath::Abs(Scale.X - 1.0), FMath::Abs(Scale.Y - 1.0), FMath::Abs(Scale.Z - 1.0)));
	}
}

void ValidateComponentPoolState(const FVoxelPaintComponentRuleInputs& Inputs,
	TArray<FVoxelPaintComponentFinding>& OutFindings)
{
	OutFindings.Reset();

	static const FVoxelPaintComponentRuleEntry DefaultRuleEntry;
	const auto GetEntry = [&Inputs](int32 SlotIndex) -> const FVoxelPaintComponentRuleEntry&
	{
		return Inputs.Entries.IsValidIndex(SlotIndex) ? Inputs.Entries[SlotIndex] : DefaultRuleEntry;
	};

	const auto Emit = [&OutFindings](EVoxelPaintComponentRule Rule,
		EVoxelPaintComponentFindingSeverity Severity, FText&& Message)
	{
		FVoxelPaintComponentFinding& Finding = OutFindings.AddDefaulted_GetRef();
		Finding.Rule = Rule;
		Finding.Severity = Severity;
		Finding.Message = MoveTemp(Message);
	};

	const bool bScsInstance = !Inputs.bIsTemplate
		&& Inputs.CreationMethod == EComponentCreationMethod::SimpleConstructionScript;
	const bool bScsSupplied = Inputs.bIsTemplate || bScsInstance;
	const bool bPlacementOwned = !Inputs.bIsTemplate
		&& (Inputs.CreationMethod == EComponentCreationMethod::Native
			|| Inputs.CreationMethod == EComponentCreationMethod::Instance);

	if (Inputs.Expectation.bIsSet)
	{
		for (int32 SlotIndex = 0; SlotIndex < Inputs.Pool.Assets.Num(); ++SlotIndex)
		{
			const FVoxelPaintComponentRuleEntry& Entry = GetEntry(SlotIndex);
			if (!Inputs.Pool.Assets[SlotIndex] || !Entry.bHasAsset)
			{
				continue;
			}
			const bool bMatches = Entry.Topology.LocalHalfExtent == Inputs.Expectation.LocalHalfExtent
				&& Entry.Topology.BrickCounts == Inputs.Expectation.BrickCounts
				&& Entry.Topology.StorageOrigin == Inputs.Expectation.StorageOrigin;
			if (bMatches)
			{
				continue;
			}
			FFormatOrderedArguments Arguments;
			Arguments.Add(FText::AsNumber(SlotIndex + 1));
			Arguments.Add(FText::FromString(Entry.AssetName));
			Arguments.Add(FText::FromString(Entry.Topology.LocalHalfExtent.ToString()));
			Arguments.Add(FText::FromString(Entry.Topology.BrickCounts.ToString()));
			Arguments.Add(FText::FromString(Entry.Topology.StorageOrigin.ToString()));
			Arguments.Add(FText::FromString(Inputs.Expectation.LocalHalfExtent.ToString()));
			Arguments.Add(FText::FromString(Inputs.Expectation.BrickCounts.ToString()));
			Arguments.Add(FText::FromString(Inputs.Expectation.StorageOrigin.ToString()));
			Emit(EVoxelPaintComponentRule::TopologyMismatch,
				EVoxelPaintComponentFindingSeverity::Error,
				FText::Format(LOCTEXT("ComponentRuleTopologyMismatch",
					"Variant {0} '{1}': asset topology (half-extent {2}, bricks {3}, origin {4}) does not match the expected topology (half-extent {5}, bricks {6}, origin {7}). Resize pre-init variants with the sizing tool, or correct the ExpectedTopology property on the pool owner."),
					Arguments));
		}
	}

	for (int32 SlotIndex = 0; SlotIndex < Inputs.Pool.Assets.Num(); ++SlotIndex)
	{
		const UVoxelPaintVolumeData* Asset = Inputs.Pool.Assets[SlotIndex];
		if (!Asset)
		{
			Emit(EVoxelPaintComponentRule::NullEntry,
				EVoxelPaintComponentFindingSeverity::Warning,
				FText::Format(LOCTEXT("ComponentRuleNullEntry", "Variant {0} has no asset assigned."),
					FText::AsNumber(SlotIndex + 1)));
			continue;
		}
		for (int32 EarlierIndex = 0; EarlierIndex < SlotIndex; ++EarlierIndex)
		{
			if (Inputs.Pool.Assets[EarlierIndex] == Asset)
			{
				Emit(EVoxelPaintComponentRule::DuplicateEntry,
					EVoxelPaintComponentFindingSeverity::Error,
					FText::Format(LOCTEXT("ComponentRuleDuplicateEntry",
						"Variant {0} duplicates variant {1} ('{2}'); duplicate pool entries alias one paint target - remove one."),
						FText::AsNumber(SlotIndex + 1), FText::AsNumber(EarlierIndex + 1),
						FText::FromString(GetEntry(SlotIndex).AssetName)));
				break;
			}
		}
	}

	if (Inputs.bPinSet && !Inputs.bPinInPool)
	{
		Emit(EVoxelPaintComponentRule::DanglingPin,
			EVoxelPaintComponentFindingSeverity::Warning,
			FText::Format(LOCTEXT("ComponentRuleDanglingPin",
				"Pin dangling - '{0}' is not in this pool; the placement shows the deterministic roll."),
				FText::FromString(Inputs.PinAssetName)));
	}

	if (bScsInstance && Inputs.bHasArchetype)
	{
		const bool bPoolDiverges = Inputs.Pool.Assets != Inputs.ArchetypeAssets
			|| Inputs.Pool.Weights != Inputs.ArchetypeWeights;
		const bool bExpectationDiverges = !(Inputs.Expectation == Inputs.ArchetypeExpectation);
		if (bPoolDiverges || bExpectationDiverges)
		{
			Emit(EVoxelPaintComponentRule::ScsPoolDivergence,
				EVoxelPaintComponentFindingSeverity::Error,
				LOCTEXT("ComponentRuleScsPoolDivergence",
					"Pool or expected topology diverges from the Blueprint archetype; use the Reset to template button (ResetPoolToArchetype) to restore the template state."));
		}
	}

	if (bScsSupplied)
	{
		for (int32 SlotIndex = 0; SlotIndex < Inputs.Pool.Assets.Num(); ++SlotIndex)
		{
			const FVoxelPaintComponentRuleEntry& Entry = GetEntry(SlotIndex);
			if (!Inputs.Pool.Assets[SlotIndex] || !Entry.bHasAsset
				|| Entry.SharingMode != EVoxelPaintAssetSharingMode::Private)
			{
				continue;
			}
			Emit(EVoxelPaintComponentRule::PrivateEntryInScsPool,
				EVoxelPaintComponentFindingSeverity::Error,
				FText::Format(LOCTEXT("ComponentRulePrivateEntryInScsPool",
					"Variant {0} '{1}' is a Private asset in a Blueprint-supplied pool - broken by construction (no placement can own it); mark it Shared deliberately, or duplicate it and mark the copy Shared."),
					FText::AsNumber(SlotIndex + 1), FText::FromString(Entry.AssetName)));
		}
	}

	if (bPlacementOwned)
	{
		for (int32 SlotIndex = 0; SlotIndex < Inputs.Pool.Assets.Num(); ++SlotIndex)
		{
			const FVoxelPaintComponentRuleEntry& Entry = GetEntry(SlotIndex);
			if (!Inputs.Pool.Assets[SlotIndex] || !Entry.bHasAsset
				|| Entry.SharingMode != EVoxelPaintAssetSharingMode::Private
				|| Entry.bOwnedByThisPlacement)
			{
				continue;
			}
			const FText OwnerText = !Entry.OwnerDisplayName.IsEmpty()
				? FText::FromString(Entry.OwnerDisplayName)
				: LOCTEXT("ComponentRuleForeignOwnerFallback", "another placement");
			Emit(EVoxelPaintComponentRule::ForeignPrivateEntry,
				EVoxelPaintComponentFindingSeverity::Warning,
				FText::Format(LOCTEXT("ComponentRuleForeignPrivateEntry",
					"Variant {0} '{1}' is a Private asset owned by {2}; remediate with Take Ownership, Make Unique, or Mark Shared."),
					FText::AsNumber(SlotIndex + 1), FText::FromString(Entry.AssetName), OwnerText));
		}
	}

	if (Inputs.CreationMethod == EComponentCreationMethod::UserConstructionScript)
	{
		Emit(EVoxelPaintComponentRule::UcsProvenance,
			EVoxelPaintComponentFindingSeverity::Warning,
			LOCTEXT("ComponentRuleUcsProvenance",
				"Construction-script-created voxel paint components are unsupported in v1; no Blueprint template exists to own the pool."));
	}

	if (!Inputs.bIsTemplate && Inputs.CreationMethod == EComponentCreationMethod::Native
		&& Inputs.bOwnerClassIsBlueprintGeneratedClass)
	{
		Emit(EVoxelPaintComponentRule::NativeBlueprintCarrier,
			EVoxelPaintComponentFindingSeverity::Warning,
			LOCTEXT("ComponentRuleNativeBlueprintCarrier",
				"Native voxel paint component on a Blueprint-classed owner is an unsupported carrier in v1; supported carriers are the native volume actor and Blueprint-added (SCS) components."));
	}

	if (Inputs.MaxScaleDeviation > 1.0e-3f)
	{
		Emit(EVoxelPaintComponentRule::NonUnitEffectiveScale,
			EVoxelPaintComponentFindingSeverity::Note,
			LOCTEXT("ComponentRuleNonUnitEffectiveScale",
				"voxel paint ignores scale; painted data will not match scaled geometry"));
	}

	TArray<int32> ValidSlots;
	ComputeValidSlots(Inputs.Pool, ValidSlots);
	if (ValidSlots.Num() > 0)
	{
		bool bAnyPositiveWeight = false;
		for (const int32 Slot : ValidSlots)
		{
			const float Weight = Inputs.Pool.Weights.IsValidIndex(Slot) ? Inputs.Pool.Weights[Slot] : 0.0f;
			if (FMath::IsFinite(Weight) && Weight > 0.0f)
			{
				bAnyPositiveWeight = true;
				break;
			}
		}
		if (!bAnyPositiveWeight)
		{
			Emit(EVoxelPaintComponentRule::AllZeroWeights,
				EVoxelPaintComponentFindingSeverity::Warning,
				LOCTEXT("ComponentRuleAllZeroWeights",
					"Every selection weight in this pool is zero or invalid; selection falls back to uniform weights over the valid variants."));
		}
	}
}

} // namespace WorldVoxelPaint

namespace
{

struct FVoxelPaintComponentToValidate
{
	const UVoxelPaintVolumeComponent* Component = nullptr;
	FText ContextLabel;
};

// The visitor returns true to keep enumerating; returning false stops the walk. Each Visit
// function returns false when the visitor stopped it, so nested walks can propagate the stop.
using FVoxelPaintComponentVisitor = TFunctionRef<bool(const FVoxelPaintComponentToValidate&)>;

bool VisitActorComponents(const AActor* Actor, FVoxelPaintComponentVisitor Visitor)
{
	if (!IsValid(Actor) || Actor->IsTemplate())
	{
		return true;
	}
	for (UActorComponent* ActorComponent : Actor->GetComponents())
	{
		const UVoxelPaintVolumeComponent* Component = Cast<UVoxelPaintVolumeComponent>(ActorComponent);
		if (!Component || !IsValid(Component))
		{
			continue;
		}
		FVoxelPaintComponentToValidate Entry;
		Entry.Component = Component;
		Entry.ContextLabel = FText::FromString(FString::Printf(TEXT("%s.%s"),
			*Actor->GetActorNameOrLabel(), *Component->GetName()));
		if (!Visitor(Entry))
		{
			return false;
		}
	}
	return true;
}

bool VisitBlueprintTemplates(const UBlueprint* Blueprint, FVoxelPaintComponentVisitor Visitor)
{
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	if (!GeneratedClass)
	{
		return true;
	}
	// Class-local templates only: ancestor SCS templates belong to the parent Blueprint and are
	// validated there, while an inherited-component override is owned by this class.
	if (const USimpleConstructionScript* ConstructionScript = GeneratedClass->SimpleConstructionScript)
	{
		for (const USCS_Node* Node : ConstructionScript->GetAllNodes())
		{
			const UVoxelPaintVolumeComponent* Template =
				Node ? Cast<UVoxelPaintVolumeComponent>(Node->ComponentTemplate) : nullptr;
			if (!Template)
			{
				continue;
			}
			FVoxelPaintComponentToValidate Entry;
			Entry.Component = Template;
			Entry.ContextLabel = FText::FromString(FString::Printf(TEXT("%s SCS template '%s'"),
				*Blueprint->GetName(), *Template->GetName()));
			if (!Visitor(Entry))
			{
				return false;
			}
		}
	}
	if (const UInheritableComponentHandler* OverrideHandler =
		GeneratedClass->GetInheritableComponentHandler(false))
	{
		TArray<UActorComponent*> OverrideTemplates;
		OverrideHandler->GetAllTemplates(OverrideTemplates);
		for (const UActorComponent* OverrideTemplate : OverrideTemplates)
		{
			const UVoxelPaintVolumeComponent* Template =
				Cast<UVoxelPaintVolumeComponent>(OverrideTemplate);
			if (!Template)
			{
				continue;
			}
			FVoxelPaintComponentToValidate Entry;
			Entry.Component = Template;
			Entry.ContextLabel = FText::FromString(FString::Printf(TEXT("%s override template '%s'"),
				*Blueprint->GetName(), *Template->GetName()));
			if (!Visitor(Entry))
			{
				return false;
			}
		}
	}
	return true;
}

// Single enumeration primitive shared by CanValidateAsset (existence check) and
// ValidateLoadedAsset (collection), so eligibility and validation cannot drift. Assets outside
// this validator's scope (unhandled types, non-enumerable worlds) visit nothing.
void VisitAssetComponents(const UObject* Asset, FVoxelPaintComponentVisitor Visitor)
{
	if (const UWorld* World = Cast<UWorld>(Asset))
	{
		if (!WorldVoxelPaint::IsEnumerableWorld(World))
		{
			return;
		}
		for (const ULevel* Level : World->GetLevels())
		{
			if (!Level)
			{
				continue;
			}
			for (const AActor* Actor : Level->Actors)
			{
				if (!VisitActorComponents(Actor, Visitor))
				{
					return;
				}
			}
		}
	}
	else if (const AActor* Actor = Cast<AActor>(Asset))
	{
		// A null world is allowed: an OFPA actor can be validated before it joins a level.
		const UWorld* ActorWorld = Actor->GetWorld();
		if (ActorWorld && !WorldVoxelPaint::IsEnumerableWorld(ActorWorld))
		{
			return;
		}
		VisitActorComponents(Actor, Visitor);
	}
	else if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
	{
		VisitBlueprintTemplates(Blueprint, Visitor);
	}
}

} // namespace

bool UVoxelPaintComponentValidator::CanValidateAsset_Implementation(const FAssetData& InAssetData,
	UObject* InObject, FDataValidationContext& InContext) const
{
	// Only claim assets that actually carry a paint component: once this returns true, the engine
	// requires ValidateLoadedAsset to return Valid or Invalid (never NotValidated).
	bool bHasComponent = false;
	VisitAssetComponents(InObject, [&bHasComponent](const FVoxelPaintComponentToValidate&)
	{
		bHasComponent = true;
		return false;
	});
	return bHasComponent;
}

EDataValidationResult UVoxelPaintComponentValidator::ValidateLoadedAsset_Implementation(
	const FAssetData& InAssetData, UObject* InAsset, FDataValidationContext& Context)
{
	TArray<FVoxelPaintComponentToValidate> Components;
	VisitAssetComponents(InAsset, [&Components](const FVoxelPaintComponentToValidate& Entry)
	{
		Components.Add(Entry);
		return true;
	});

	// CanValidateAsset saw a component on this same loaded object in the same synchronous pass,
	// so an empty collection here means engine state shifted between the two calls. Returning
	// NotValidated would trip the engine's ensure now that eligibility was claimed, so pass.
	if (Components.Num() == 0)
	{
		AssetPasses(InAsset);
		return EDataValidationResult::Valid;
	}

	bool bFailed = false;
	for (const FVoxelPaintComponentToValidate& Entry : Components)
	{
		WorldVoxelPaint::FVoxelPaintComponentRuleInputs Inputs;
		WorldVoxelPaint::BuildComponentRuleInputs(*Entry.Component, Inputs);
		TArray<WorldVoxelPaint::FVoxelPaintComponentFinding> Findings;
		WorldVoxelPaint::ValidateComponentPoolState(Inputs, Findings);
		for (const WorldVoxelPaint::FVoxelPaintComponentFinding& Finding : Findings)
		{
			const FText Message = FText::Format(LOCTEXT("ComponentFindingWithContext", "{0}: {1}"),
				Entry.ContextLabel, Finding.Message);
			switch (Finding.Severity)
			{
			case WorldVoxelPaint::EVoxelPaintComponentFindingSeverity::Error:
				AssetFails(InAsset, Message);
				bFailed = true;
				break;
			case WorldVoxelPaint::EVoxelPaintComponentFindingSeverity::Warning:
				AssetWarning(InAsset, Message);
				break;
			default:
				AssetMessage(EMessageSeverity::Info, Message);
				break;
			}
		}
	}

	if (!bFailed)
	{
		AssetPasses(InAsset);
		return EDataValidationResult::Valid;
	}
	return EDataValidationResult::Invalid;
}

#undef LOCTEXT_NAMESPACE
