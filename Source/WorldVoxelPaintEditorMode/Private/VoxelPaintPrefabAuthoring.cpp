// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintPrefabAuthoring.h"

#include "Components/MeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "VoxelPaintAuthority.h"
#include "VoxelPaintDetailLadder.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintTemplateRouting.h"
#include "VoxelPaintTopologyOps.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "WorldVoxelPaintConstants.h"

#define LOCTEXT_NAMESPACE "VoxelPaintEditorMode"

namespace WorldVoxelPaint
{

namespace PrefabAuthoringPrivate
{

FString GetIntroducingClassDisplayName(const UVoxelPaintVolumeComponent* Component,
	const UClass* OperatedClass)
{
	if (!Component || !OperatedClass)
	{
		return TEXT("<unresolved class>");
	}
	const FName ComponentName = Component->GetFName();
	for (const UClass* Class = OperatedClass; Class; Class = Class->GetSuperClass())
	{
		const UBlueprintGeneratedClass* Bpgc = Cast<UBlueprintGeneratedClass>(Class);
		if (!Bpgc)
		{
			break;
		}
		if (const USimpleConstructionScript* Scs = Bpgc->SimpleConstructionScript)
		{
			if (Scs->FindSCSNode(ComponentName))
			{
				return Bpgc->ClassGeneratedBy ? Bpgc->ClassGeneratedBy->GetName() : Bpgc->GetName();
			}
		}
	}
	return TEXT("<unresolved class>");
}

UBlueprint* ResolveOwningBlueprint(const UVoxelPaintVolumeComponent* Template)
{
	const UBlueprintGeneratedClass* Bpgc =
		Template ? Template->GetTypedOuter<UBlueprintGeneratedClass>() : nullptr;
	return Bpgc ? Cast<UBlueprint>(Bpgc->ClassGeneratedBy) : nullptr;
}

bool IsBlueprintPackageAuthorable(const UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return false;
	}
	const FString PackageName = Blueprint->GetPackage()->GetName();
	return FPackageName::IsValidLongPackageName(PackageName)
		&& !FPackageName::IsTempPackage(PackageName);
}

bool PoolHasValidEntry(const UVoxelPaintVolumeComponent* Component)
{
	for (const FVoxelPaintVariantPoolEntry& Entry : Component->VolumeVariants)
	{
		if (Entry.Variant)
		{
			return true;
		}
	}
	return false;
}

const UVoxelPaintVolumeData* FindOpenSessionPoolAsset(const UVoxelPaintVolumeComponent* Component)
{
	for (const FVoxelPaintVariantPoolEntry& Entry : Component->VolumeVariants)
	{
		if (Entry.Variant && IsAssetInOpenSession(Entry.Variant))
		{
			return Entry.Variant;
		}
	}
	return nullptr;
}

bool IsAnySessionOpen(FText& OutReason)
{
	if (const FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated())
	{
		if (Controller->IsSessionOpen())
		{
			OutReason = FText::Format(LOCTEXT("FreshVariantSessionOpen",
				"A paint session is open on '{0}' (one session at a time). Finish or Discard it before creating pool variants."),
				FText::FromString(GetNameSafe(Controller->GetActiveAsset())));
			return true;
		}
	}
	return false;
}

UPackage* CreateUniqueBlueprintScopedPackage(const UBlueprint* Blueprint, const FString& BaseName,
	FString& OutAssetName)
{
	const FString Folder = FPackageName::GetLongPackagePath(Blueprint->GetPackage()->GetName());
	FString AssetName = BaseName;
	FString PackageName = Folder + TEXT("/") + AssetName;
	int32 Suffix = 0;
	while (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName))
	{
		++Suffix;
		AssetName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix);
		PackageName = Folder + TEXT("/") + AssetName;
	}
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("VoxelPaintPrefabAuthoring: failed to create package '%s'."), *PackageName);
		return nullptr;
	}
	OutAssetName = AssetName;
	return Package;
}

UVoxelPaintVolumeData* CreateTemplatePoolAsset(const UBlueprint* Blueprint, int32 SlotIndex,
	const UVoxelPaintVolumeData* Source)
{
	const FString BlueprintName = Blueprint->GetName();
	FString BaseName = ObjectTools::SanitizeObjectName(BlueprintName) + TEXT("_VPD");
	if (SlotIndex > 0)
	{
		BaseName += FString::Printf(TEXT("_Variant%d"), SlotIndex);
	}
	FString AssetName;
	UPackage* Package = CreateUniqueBlueprintScopedPackage(Blueprint, BaseName, AssetName);
	if (!Package)
	{
		return nullptr;
	}
	UVoxelPaintVolumeData* Asset = Source
		? DuplicateObject<UVoxelPaintVolumeData>(Source, Package, FName(*AssetName))
		: NewObject<UVoxelPaintVolumeData>(Package, FName(*AssetName),
			RF_Public | RF_Standalone | RF_Transactional);
	if (!Asset)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("VoxelPaintPrefabAuthoring: failed to create asset '%s'."), *AssetName);
		return nullptr;
	}
	Asset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	MarkShared(Asset);
	Asset->OwnerDisplayName = BlueprintName;
	Asset->MarkPackageDirty();
	UE_LOG(LogWorldVoxelPaint, Display,
		TEXT("Created voxel paint asset '%s' for Blueprint '%s' (unsaved; save the package to keep it)."),
		*Asset->GetPathName(), *BlueprintName);
	return Asset;
}

const UVoxelPaintVolumeComponent* ResolveAutoSizeInstance(const FVoxelPaintPoolAuthoringContext& Context)
{
	if (Context.Venue == EVoxelPaintPoolAuthoringVenue::PlacementOwned
		|| Context.Venue == EVoxelPaintPoolAuthoringVenue::RoutedInstance)
	{
		return Context.Component;
	}
	TArray<UObject*> Instances;
	Context.WriteTarget->GetArchetypeInstances(Instances);
	const UVoxelPaintVolumeComponent* EditorMatch = nullptr;
	for (const UObject* Object : Instances)
	{
		const UVoxelPaintVolumeComponent* Instance = Cast<UVoxelPaintVolumeComponent>(Object);
		if (!Instance || Instance->IsTemplate())
		{
			continue;
		}
		const UWorld* World = Instance->GetOwner() ? Instance->GetWorld() : nullptr;
		if (!World)
		{
			continue;
		}
		if (World->IsPreviewWorld())
		{
			return Instance;
		}
		if (!EditorMatch
			&& (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::Inactive))
		{
			EditorMatch = Instance;
		}
	}
	return EditorMatch;
}

// Mesh bounds in the paint component's rigid frame: the volume is an oriented box centered on
// that frame, so a world-axis-aligned sweep would mis-size rotated placements.
FBox ComputeMeshGeometryBoundsInFrame(const AActor* Actor, const FTransform& RigidFrame)
{
	FBox Box(ForceInit);
	if (!Actor)
	{
		return Box;
	}
	const FTransform WorldToFrame = RigidFrame.Inverse();
	Actor->ForEachComponent<UMeshComponent>(/*bIncludeFromChildActors=*/false,
		[&Box, &WorldToFrame](const UMeshComponent* Mesh)
		{
			if (Mesh->IsRegistered())
			{
				Box += Mesh->Bounds.GetBox().TransformBy(WorldToFrame);
			}
		});
	return Box;
}

bool BuildFreshVariantTopologyState(const FVoxelPaintPoolAuthoringContext& Context,
	UVoxelPaintVolumeData* Asset, FVoxelPaintLadderState& OutState,
	FVoxelPaintAutoSizeCapture& OutCapture, FText& OutError)
{
	const UWorld* World = Context.Component->GetWorld();
	const FVoxelPaintLadderClamps Clamps = BuildLadderClamps(World);
	FVoxelPaintLadderState State;
	if (Context.WriteTarget->ExpectedTopology.bIsSet)
	{
		State.HalfExtent = Context.WriteTarget->ExpectedTopology.LocalHalfExtent;
		State.BrickCounts = Context.WriteTarget->ExpectedTopology.BrickCounts;
		Asset->StorageOrigin = Context.WriteTarget->ExpectedTopology.StorageOrigin;
		FString LadderError;
		if (!ValidateLadderState(State, Clamps, LadderError))
		{
			OutError = FText::Format(LOCTEXT("ExpectationNotRepresentable",
				"The pool's expected topology is not a representable size ladder state: {0}"),
				FText::FromString(LadderError));
			return false;
		}
	}
	else
	{
		OutCapture = ComputeAutoSizeCapture(ResolveAutoSizeInstance(Context));
		const FVector Extent = OutCapture.bBoundsValid ? OutCapture.HalfExtent : FVector(500.0);
		State.BrickCounts = Asset->GetBrickCounts();
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			State.HalfExtent[Axis] = FMath::Max(Extent[Axis],
				0.5 * Clamps.MinVoxelSize * UniqueBrickSize);
		}
		FString LadderError;
		if (!ValidateLadderState(State, Clamps, LadderError))
		{
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				SelectRungForTexelSize(State, Axis,
					FMath::Max(Clamps.MinVoxelSize, State.GetTexelSize()[Axis]), Clamps);
			}
		}
	}
	OutState = State;
	return true;
}

void WritePoolSlot(const FVoxelPaintPoolAuthoringContext& Context, int32 SlotIndex,
	UVoxelPaintVolumeData* Asset, TOptional<float> Weight)
{
	UVoxelPaintVolumeComponent* Target = Context.WriteTarget;
	const TArray<FVoxelPaintVariantPoolEntry> PreEditPool = Target->VolumeVariants;
	const FVoxelPaintExpectedTopology PreEditExpectation = Target->ExpectedTopology;
	Target->CapturePreEditPoolState();
	Target->Modify();
	if (!Target->VolumeVariants.IsValidIndex(SlotIndex))
	{
		Target->VolumeVariants.AddDefaulted(SlotIndex + 1 - Target->VolumeVariants.Num());
	}
	FVoxelPaintVariantPoolEntry& Entry = Target->VolumeVariants[SlotIndex];
	Entry.Variant = Asset;
	if (Weight.IsSet())
	{
		Entry.Weight = Weight.GetValue();
	}
	FPropertyChangedEvent ChangedEvent(UVoxelPaintVolumeComponent::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VolumeVariants)));
	Target->PostEditChangeProperty(ChangedEvent);
	if (Target->IsTemplate())
	{
		PropagateTemplatePoolChange(Target, PreEditPool, PreEditExpectation);
	}
}

void MarkOwningBlueprintStructurallyModified(const FVoxelPaintPoolAuthoringContext& Context)
{
	if (Context.Blueprint && Context.WriteTarget && Context.WriteTarget->IsTemplate())
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Context.Blueprint);
	}
}

bool EnsureFreshPoolVariant(const FVoxelPaintPoolAuthoringContext& Context, bool bCreateSlotZero,
	FText& OutError)
{
	if (!Context.bPoolActionsEnabled || !Context.Component || !Context.WriteTarget)
	{
		OutError = Context.DisabledReason.IsEmpty()
			? LOCTEXT("PoolActionsUnavailable", "Pool authoring is unavailable for this component.")
			: Context.DisabledReason;
		return false;
	}
	FText SessionReason;
	if (IsAnySessionOpen(SessionReason))
	{
		OutError = SessionReason;
		return false;
	}
	UVoxelPaintVolumeComponent* Target = Context.WriteTarget;
	int32 SlotIndex = 0;
	if (bCreateSlotZero)
	{
		if (PoolHasValidEntry(Target))
		{
			OutError = LOCTEXT("CreateOnNonEmptyPool",
				"The pool already has paint data - use Add Variant instead.");
			return false;
		}
	}
	else
	{
		if (!PoolHasValidEntry(Target))
		{
			OutError = LOCTEXT("AddOnEmptyPool",
				"The pool is empty - use Create Paint Data first.");
			return false;
		}
		SlotIndex = Target->VolumeVariants.Num();
	}

	FScopedTransaction Transaction(bCreateSlotZero
		? LOCTEXT("CreatePaintDataTransaction", "Create Voxel Paint Data")
		: LOCTEXT("AddVariantTransaction", "Add Voxel Paint Variant"));

	const AActor* InitiatingActor =
		Context.Venue == EVoxelPaintPoolAuthoringVenue::Template
			? nullptr : Context.Component->GetOwner();
	UVoxelPaintVolumeData* Asset =
		Context.Venue == EVoxelPaintPoolAuthoringVenue::PlacementOwned
			? CreateVolumeDataForPlacement(InitiatingActor)
			: CreateTemplatePoolAsset(Context.Blueprint, SlotIndex, nullptr);
	if (!Asset)
	{
		OutError = LOCTEXT("CreateAssetFailed", "Failed to create the voxel paint asset.");
		Transaction.Cancel();
		return false;
	}

	FVoxelPaintLadderState State;
	FVoxelPaintAutoSizeCapture Capture;
	if (!BuildFreshVariantTopologyState(Context, Asset, State, Capture, OutError))
	{
		Transaction.Cancel();
		return false;
	}

	WritePoolSlot(Context, SlotIndex, Asset, TOptional<float>());

	FText CommitError;
	if (!CommitPreInitTopologyEdit(Asset, InitiatingActor, Target, State, CommitError))
	{
		OutError = CommitError;
		Transaction.Cancel();
		return false;
	}
	if (Capture.bMoveWriteTarget)
	{
		ApplyAutoCenterMove(Target, Capture.NewRelativeLocation);
	}
	MarkOwningBlueprintStructurallyModified(Context);
	return true;
}

} // namespace PrefabAuthoringPrivate

FVoxelPaintAutoSizeCapture ComputeAutoSizeCapture(const UVoxelPaintVolumeComponent* Instance)
{
	using namespace PrefabAuthoringPrivate;

	FVoxelPaintAutoSizeCapture Capture;
	if (!Instance)
	{
		return Capture;
	}
	const FTransform RigidFrame = Instance->GetRigidPlacementFrame();
	const FBox LocalBounds = ComputeMeshGeometryBoundsInFrame(Instance->GetOwner(), RigidFrame);
	if (!LocalBounds.IsValid)
	{
		return Capture;
	}
	Capture.bBoundsValid = true;

	const USceneComponent* Parent = Instance->GetAttachParent();
	if (Parent && Instance->GetAttachChildren().IsEmpty())
	{
		Capture.HalfExtent = LocalBounds.GetExtent();
		const FVector WorldCenter = RigidFrame.TransformPosition(LocalBounds.GetCenter());
		Capture.NewRelativeLocation = Parent->GetSocketTransform(Instance->GetAttachSocketName())
			.InverseTransformPosition(WorldCenter);
		Capture.bMoveWriteTarget = true;
	}
	else
	{
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Capture.HalfExtent[Axis] = FMath::Max(-LocalBounds.Min[Axis], LocalBounds.Max[Axis]);
		}
		UE_LOG(LogWorldVoxelPaint, Display,
			TEXT("VoxelPaintPrefabAuthoring: '%s' is the actor root or has attached children, so it stays in place; the paint volume extent was expanded to cover the mesh geometry from there."),
			*Instance->GetPathName());
	}
	return Capture;
}

void ApplyAutoCenterMove(UVoxelPaintVolumeComponent* WriteTarget, const FVector& NewRelativeLocation)
{
	if (!WriteTarget)
	{
		return;
	}
	if (WriteTarget->IsTemplate())
	{
		const FVector OldDefault = WriteTarget->GetRelativeLocation();
		if (OldDefault == NewRelativeLocation)
		{
			return;
		}
		WriteTarget->Modify();
		WriteTarget->SetRelativeLocation_Direct(NewRelativeLocation);
		FProperty* RelativeLocationProperty = FindFieldChecked<FProperty>(
			USceneComponent::StaticClass(), USceneComponent::GetRelativeLocationPropertyName());
		TSet<USceneComponent*> UpdatedInstances;
		FComponentEditorUtils::PropagateDefaultValueChange(WriteTarget, RelativeLocationProperty,
			OldDefault, NewRelativeLocation, UpdatedInstances);
	}
	else
	{
		WriteTarget->Modify();
		WriteTarget->SetRelativeLocation(NewRelativeLocation);
	}
}

FVoxelPaintPoolAuthoringContext ResolvePoolAuthoringContext(UVoxelPaintVolumeComponent* Component)
{
	using namespace PrefabAuthoringPrivate;

	FVoxelPaintPoolAuthoringContext Context;
	Context.Component = Component;
	if (!Component)
	{
		Context.DisabledReason = LOCTEXT("NoComponent", "No voxel paint component.");
		return Context;
	}

	if (Component->IsTemplate())
	{
		UBlueprint* Blueprint = ResolveOwningBlueprint(Component);
		if (!Blueprint)
		{
			Context.DisabledReason = LOCTEXT("TemplateWithoutBlueprint",
				"This component template has no Blueprint to own a paint pool.");
			return Context;
		}
		if (!IsBlueprintPackageAuthorable(Blueprint))
		{
			Context.DisabledReason = LOCTEXT("UnsavedBlueprint",
				"Save the Blueprint to a content folder before creating pool assets.");
			return Context;
		}
		Context.WriteTarget = Component;
		Context.Blueprint = Blueprint;
		Context.Venue = EVoxelPaintPoolAuthoringVenue::Template;
	}
	else
	{
		const AActor* Owner = Component->GetOwner();
		const UWorld* World = Component->GetWorld();
		if (!Owner || !World
			|| (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::Inactive))
		{
			Context.DisabledReason = LOCTEXT("TransientWorld",
				"Pool authoring is available only on editor placements (not PIE or preview worlds).");
			return Context;
		}
		const UClass* OperatedClass = Owner->GetClass();
		switch (ClassifyPoolProvenance(Component, OperatedClass))
		{
		case EVoxelPaintPoolProvenance::Native:
		case EVoxelPaintPoolProvenance::Instance:
			Context.WriteTarget = Component;
			Context.Venue = EVoxelPaintPoolAuthoringVenue::PlacementOwned;
			break;
		case EVoxelPaintPoolProvenance::SCSOwnClass:
		{
			FText RoutingError;
			UVoxelPaintVolumeComponent* Template = ResolveExpectationWriteTarget(Component, RoutingError);
			if (!Template)
			{
				Context.DisabledReason = RoutingError;
				return Context;
			}
			UBlueprint* Blueprint = ResolveOwningBlueprint(Template);
			if (!Blueprint)
			{
				Context.DisabledReason = LOCTEXT("TemplateWithoutBlueprint2",
					"This component template has no Blueprint to own a paint pool.");
				return Context;
			}
			if (!IsBlueprintPackageAuthorable(Blueprint))
			{
				Context.DisabledReason = LOCTEXT("UnsavedBlueprint2",
					"Save the Blueprint to a content folder before creating pool assets.");
				return Context;
			}
			if (!(Component->VolumeVariants == Template->VolumeVariants)
				|| !(Component->ExpectedTopology == Template->ExpectedTopology))
			{
				Context.DisabledReason = LOCTEXT("DivergedFromTemplate",
					"This placement's pool has diverged from its Blueprint template - use Reset to Template first, or fix the pool in the Blueprint's editor.");
				return Context;
			}
			Context.WriteTarget = Template;
			Context.Blueprint = Blueprint;
			Context.Venue = EVoxelPaintPoolAuthoringVenue::RoutedInstance;
			break;
		}
		case EVoxelPaintPoolProvenance::SCSOverrideOwner:
			Context.DisabledReason = FText::Format(LOCTEXT("OverrideOwnerGrayed",
				"This component is inherited from an ancestor class; '{0}' holds a class-local override for it - manage the pool in the Blueprint editor of '{0}'."),
				FText::FromString(Owner->GetClass()->ClassGeneratedBy
					? Owner->GetClass()->ClassGeneratedBy->GetName() : Owner->GetClass()->GetName()));
			return Context;
		case EVoxelPaintPoolProvenance::SCSAncestor:
			Context.DisabledReason = FText::Format(LOCTEXT("AncestorOwnedGrayed",
				"This component's paint pool is owned by '{0}' - manage the pool in that Blueprint's editor."),
				FText::FromString(GetIntroducingClassDisplayName(Component, OperatedClass)));
			return Context;
		case EVoxelPaintPoolProvenance::UCS:
		default:
			Context.DisabledReason = LOCTEXT("UcsUnsupported",
				"Construction-script-created voxel paint components are unsupported; add the component in the Blueprint's Components panel or on the placed actor instead.");
			return Context;
		}
	}

	if (const UVoxelPaintVolumeData* SessionAsset = FindOpenSessionPoolAsset(Context.WriteTarget))
	{
		Context.DisabledReason = FText::Format(LOCTEXT("SessionOpenOnPoolAsset",
			"A paint session is open on '{0}' (one session at a time). Finish or Discard it before changing the pool."),
			FText::FromString(SessionAsset->GetName()));
		return Context;
	}

	IsAnySessionOpen(Context.FreshVariantDisabledReason);
	Context.bPoolActionsEnabled = true;
	return Context;
}

bool CreatePoolPaintData(const FVoxelPaintPoolAuthoringContext& Context, FText& OutError)
{
	return PrefabAuthoringPrivate::EnsureFreshPoolVariant(Context, /*bCreateSlotZero=*/true, OutError);
}

bool AddPoolVariant(const FVoxelPaintPoolAuthoringContext& Context, FText& OutError)
{
	return PrefabAuthoringPrivate::EnsureFreshPoolVariant(Context, /*bCreateSlotZero=*/false, OutError);
}

bool DuplicatePoolVariant(const FVoxelPaintPoolAuthoringContext& Context, int32 SourceSlot,
	FText& OutError)
{
	using namespace PrefabAuthoringPrivate;

	if (!Context.bPoolActionsEnabled || !Context.Component || !Context.WriteTarget)
	{
		OutError = Context.DisabledReason.IsEmpty()
			? LOCTEXT("PoolActionsUnavailable2", "Pool authoring is unavailable for this component.")
			: Context.DisabledReason;
		return false;
	}
	UVoxelPaintVolumeComponent* Target = Context.WriteTarget;
	if (!Target->VolumeVariants.IsValidIndex(SourceSlot)
		|| !Target->VolumeVariants[SourceSlot].Variant)
	{
		OutError = LOCTEXT("DuplicateNoSource", "No source variant to duplicate in this slot.");
		return false;
	}
	const UVoxelPaintVolumeData* Source = Target->VolumeVariants[SourceSlot].Variant;
	if (!Target->ExpectedTopology.bIsSet)
	{
		OutError = LOCTEXT("DuplicatePristine",
			"The pool has no expected topology yet - Create Paint Data records it first.");
		return false;
	}
	if (!Target->ExpectedTopology.Matches(*Source))
	{
		OutError = FText::Format(LOCTEXT("DuplicateSourceMismatch",
			"'{0}' does not match the pool's expected topology - fix the topology mismatch first."),
			FText::FromString(Source->GetName()));
		return false;
	}

	const int32 SlotIndex = Target->VolumeVariants.Num();
	FScopedTransaction Transaction(
		LOCTEXT("DuplicateVariantTransaction", "Duplicate Voxel Paint Variant"));
	UVoxelPaintVolumeData* Asset =
		Context.Venue == EVoxelPaintPoolAuthoringVenue::PlacementOwned
			? DeepCopyVolumeDataForPlacement(Source, Context.Component->GetOwner())
			: CreateTemplatePoolAsset(Context.Blueprint, SlotIndex, Source);
	if (!Asset)
	{
		OutError = LOCTEXT("DuplicateAssetFailed", "Failed to create the duplicated voxel paint asset.");
		Transaction.Cancel();
		return false;
	}
	WritePoolSlot(Context, SlotIndex, Asset, Target->VolumeVariants[SourceSlot].Weight);
	MarkOwningBlueprintStructurallyModified(Context);
	return true;
}

bool RemovePoolVariant(const FVoxelPaintPoolAuthoringContext& Context, int32 Slot, FText& OutError)
{
	using namespace PrefabAuthoringPrivate;

	if (!Context.bPoolActionsEnabled || !Context.Component || !Context.WriteTarget)
	{
		OutError = Context.DisabledReason.IsEmpty()
			? LOCTEXT("PoolActionsUnavailable3", "Pool authoring is unavailable for this component.")
			: Context.DisabledReason;
		return false;
	}
	UVoxelPaintVolumeComponent* Target = Context.WriteTarget;
	if (!Target->VolumeVariants.IsValidIndex(Slot))
	{
		OutError = LOCTEXT("RemoveInvalidSlot", "The pool slot to remove no longer exists.");
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("RemoveVariantTransaction", "Remove Voxel Paint Variant"));
	const TArray<FVoxelPaintVariantPoolEntry> PreEditPool = Target->VolumeVariants;
	const FVoxelPaintExpectedTopology PreEditExpectation = Target->ExpectedTopology;
	Target->CapturePreEditPoolState();
	Target->Modify();
	Target->VolumeVariants.RemoveAt(Slot);
	FPropertyChangedEvent ChangedEvent(UVoxelPaintVolumeComponent::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VolumeVariants)));
	Target->PostEditChangeProperty(ChangedEvent);
	if (Target->IsTemplate())
	{
		PropagateTemplatePoolChange(Target, PreEditPool, PreEditExpectation);
	}
	MarkOwningBlueprintStructurallyModified(Context);
	return true;
}

} // namespace WorldVoxelPaint

#undef LOCTEXT_NAMESPACE
