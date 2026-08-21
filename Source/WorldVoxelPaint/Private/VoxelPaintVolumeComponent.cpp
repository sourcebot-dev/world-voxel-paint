// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintVolumeComponent.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "VoxelPaintVariantSelection.h"
#include "VoxelPaintVolumeData.h"
#include "VoxelPaintWorldSubsystem.h"
#include "WorldVoxelPaintConstants.h"

#if WITH_EDITOR
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "UObject/UnrealType.h"
#include "VoxelPaintAuthority.h"
#include "Widgets/Notifications/SNotificationList.h"
#else
#include "WorldPartition/ActorInstanceGuids.h"
#endif

namespace
{

WorldVoxelPaint::FVoxelPaintPoolView MakePoolView(const TArray<FVoxelPaintVariantPoolEntry>& Pool)
{
	WorldVoxelPaint::FVoxelPaintPoolView View;
	View.Assets.Reserve(Pool.Num());
	View.Weights.Reserve(Pool.Num());
	for (const FVoxelPaintVariantPoolEntry& Entry : Pool)
	{
		View.Assets.Add(Entry.Variant.Get());
		View.Weights.Add(Entry.Weight);
	}
	return View;
}

#if WITH_EDITOR

void ShowVoxelPaintComponentNotification(const FString& Message)
{
	if (FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(FText::FromString(Message));
		Info.ExpireDuration = 6.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
	else
	{
		UE_LOG(LogWorldVoxelPaint, Warning, TEXT("%s"), *Message);
	}
}

int32 CountPoolOccurrences(const TArray<FVoxelPaintVariantPoolEntry>& Pool, const UVoxelPaintVolumeData* Asset)
{
	int32 Count = 0;
	for (const FVoxelPaintVariantPoolEntry& Entry : Pool)
	{
		if (Entry.Variant == Asset)
		{
			++Count;
		}
	}
	return Count;
}

const UVoxelPaintVolumeData* FindIntroducedDuplicateEntry(
	const TArray<FVoxelPaintVariantPoolEntry>& PreEditPool,
	const TArray<FVoxelPaintVariantPoolEntry>& PostEditPool)
{
	for (const FVoxelPaintVariantPoolEntry& Entry : PostEditPool)
	{
		const UVoxelPaintVolumeData* Asset = Entry.Variant.Get();
		if (!Asset)
		{
			continue;
		}
		const int32 PostCount = CountPoolOccurrences(PostEditPool, Asset);
		if (PostCount > 1 && PostCount > CountPoolOccurrences(PreEditPool, Asset))
		{
			return Asset;
		}
	}
	return nullptr;
}

const UVoxelPaintVolumeData* FindIntroducedPrivateEntry(
	const TArray<FVoxelPaintVariantPoolEntry>& PreEditPool,
	const TArray<FVoxelPaintVariantPoolEntry>& PostEditPool)
{
	for (const FVoxelPaintVariantPoolEntry& Entry : PostEditPool)
	{
		const UVoxelPaintVolumeData* Asset = Entry.Variant.Get();
		if (Asset && Asset->SharingMode == EVoxelPaintAssetSharingMode::Private
			&& CountPoolOccurrences(PreEditPool, Asset) == 0)
		{
			return Asset;
		}
	}
	return nullptr;
}

bool IsPoolGatedProperty(const FProperty* Property)
{
	if (!Property)
	{
		return false;
	}
	const FProperty* OwnerProperty = Property->GetOwnerProperty();
	const FName OwnerName = OwnerProperty ? OwnerProperty->GetFName() : NAME_None;
	if (OwnerName == GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VolumeVariants)
		|| OwnerName == GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, ExpectedTopology))
	{
		return true;
	}
	const UScriptStruct* OwnerStruct = Property->GetOwner<UScriptStruct>();
	return OwnerStruct == FVoxelPaintVariantPoolEntry::StaticStruct()
		|| OwnerStruct == FVoxelPaintExpectedTopology::StaticStruct();
}

#endif

} // namespace

bool FVoxelPaintExpectedTopology::Matches(const UVoxelPaintVolumeData& Asset) const
{
	return LocalHalfExtent == Asset.LocalHalfExtent
		&& BrickCounts == Asset.BrickCounts
		&& StorageOrigin == Asset.StorageOrigin;
}

UVoxelPaintVolumeComponent::UVoxelPaintVolumeComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

UVoxelPaintVolumeData* UVoxelPaintVolumeComponent::ResolveActiveVariant() const
{
	if (UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(GetWorld()))
	{
		return Subsystem->GetResolvedVariant(this);
	}
	return ResolveVariantLocal();
}

UVoxelPaintVolumeData* UVoxelPaintVolumeComponent::ResolveVariantLocal() const
{
	if (VariantOverride && PoolContains(VariantOverride))
	{
		return VariantOverride;
	}

	uint64 Seed;
	if (bTier1Seed)
	{
		Seed = RetainedTier1Seed;
	}
	else
	{
		const FTransform Frame = GetRigidPlacementFrame();
		Seed = WorldVoxelPaint::MakeTransformSeed(Frame.GetRotation(), Frame.GetTranslation());
	}

	const WorldVoxelPaint::FVoxelPaintPoolView View = MakePoolView(VolumeVariants);
	const int32 Slot = WorldVoxelPaint::ResolveWeightedVariantSlot(Seed, View);
	return Slot != INDEX_NONE ? VolumeVariants[Slot].Variant.Get() : nullptr;
}

int32 UVoxelPaintVolumeComponent::GetResolvedVariantIndex() const
{
	const UVoxelPaintVolumeData* Resolved = ResolveActiveVariant();
	if (!Resolved)
	{
		return INDEX_NONE;
	}
	for (int32 SlotIndexIt = 0; SlotIndexIt < VolumeVariants.Num(); ++SlotIndexIt)
	{
		if (VolumeVariants[SlotIndexIt].Variant == Resolved)
		{
			return SlotIndexIt;
		}
	}
	return INDEX_NONE;
}

bool UVoxelPaintVolumeComponent::PoolContains(const UVoxelPaintVolumeData* Asset) const
{
	if (!Asset)
	{
		return false;
	}
	for (const FVoxelPaintVariantPoolEntry& Entry : VolumeVariants)
	{
		if (Entry.Variant == Asset)
		{
			return true;
		}
	}
	return false;
}

bool UVoxelPaintVolumeComponent::HasValidVolumeData() const
{
	return ResolveActiveVariant() != nullptr;
}

FTransform UVoxelPaintVolumeComponent::GetRigidPlacementFrame() const
{
	const FTransform& ComponentTransform = GetComponentTransform();
	return FTransform(ComponentTransform.GetRotation(), ComponentTransform.GetTranslation());
}

FVector UVoxelPaintVolumeComponent::GetEffectiveLocalHalfExtent() const
{
	if (const UVoxelPaintVolumeData* Resolved = ResolveActiveVariant())
	{
		return Resolved->LocalHalfExtent;
	}
	return ExpectedTopology.bIsSet ? ExpectedTopology.LocalHalfExtent : FVector(500.0);
}

FIntVector UVoxelPaintVolumeComponent::GetEffectiveResolution() const
{
	if (const UVoxelPaintVolumeData* Resolved = ResolveActiveVariant())
	{
		return Resolved->GetResolution();
	}
	const FIntVector BrickCounts = ExpectedTopology.bIsSet ? ExpectedTopology.BrickCounts : FIntVector(4, 4, 4);
	return BrickCounts * WorldVoxelPaint::UniqueBrickSize;
}

FVector UVoxelPaintVolumeComponent::GetEffectiveTexelSize() const
{
	return (2.0 * GetEffectiveLocalHalfExtent()) / FVector(GetEffectiveResolution());
}

FVector4f UVoxelPaintVolumeComponent::GetBlendCoefficients() const
{
	const auto Coefficient = [](EVoxelPaintBlendMode BlendMode)
	{
		return BlendMode == EVoxelPaintBlendMode::Add ? 1.0f : -1.0f;
	};
	return FVector4f(Coefficient(BlendModeR), Coefficient(BlendModeG), Coefficient(BlendModeB), Coefficient(BlendModeA));
}

int32 UVoxelPaintVolumeComponent::GetRegistrySlotIndex() const
{
	return SlotIndex;
}

void UVoxelPaintVolumeComponent::SetRegistrySlotIndex(int32 InSlotIndex)
{
	SlotIndex = InSlotIndex;
}

int32 UVoxelPaintVolumeComponent::GetFrontierSlotIndex() const
{
	return FrontierSlotIndex;
}

void UVoxelPaintVolumeComponent::SetFrontierSlotIndex(int32 InFrontierSlotIndex)
{
	FrontierSlotIndex = InFrontierSlotIndex;
}

FBoxSphereBounds UVoxelPaintVolumeComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const FTransform RigidFrame(LocalToWorld.GetRotation(), LocalToWorld.GetTranslation());
	const FVector HalfExtent = GetEffectiveLocalHalfExtent();
	return FBoxSphereBounds(FBox(-HalfExtent, HalfExtent)).TransformBy(RigidFrame);
}

bool UVoxelPaintVolumeComponent::RequiresGameThreadEndOfFrameUpdates() const
{
	return true;
}

void UVoxelPaintVolumeComponent::CaptureSeedState()
{
	if (bSeedCaptured)
	{
		return;
	}
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	WorldVoxelPaint::FVoxelPaintSeedClassifierInputs Inputs;
	FGuid ActorGuid;
#if WITH_EDITOR
	ActorGuid = Owner->GetActorGuid();
	const FGuid InstanceGuid = Owner->GetActorInstanceGuid();
	Inputs.bInstanceGuidMatchesActorGuid = !InstanceGuid.IsValid() || InstanceGuid == ActorGuid;
	const UWorld* World = GetWorld();
	Inputs.bEditorWorld = World
		&& (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::Inactive);
#else
	// The cook-installed guid annotation is released at the end of the actor's own
	// PostRegisterAllComponents, before the world post-register delegate - it is only readable
	// in this OnRegister window.
	const FActorInstanceGuid Guids = FActorInstanceGuid::GetActorGuids(*Owner);
	ActorGuid = Guids.ActorGuid;
	Inputs.bInstanceGuidMatchesActorGuid =
		!Guids.ActorInstanceGuid.IsValid() || Guids.ActorInstanceGuid == ActorGuid;
#endif
	Inputs.bActorGuidValid = ActorGuid.IsValid();
	Inputs.bWasLoaded = Owner->HasAnyFlags(RF_WasLoaded);

	bTier1Seed = WorldVoxelPaint::ClassifyAsIdentitySeeded(Inputs);
	RetainedTier1Seed = bTier1Seed ? WorldVoxelPaint::MakeIdentitySeed(ActorGuid) : 0;
	bSeedCaptured = true;
}

void UVoxelPaintVolumeComponent::OnRegister()
{
	Super::OnRegister();
	CaptureSeedState();
	// M6 host auto-tag: PCG change tracking matches host ACTORS by tag (a component-class key
	// can never match an arbitrary prefab host). Idempotent single check/add - no Modify(), no
	// transaction, no package dirtying: mass prefab registration must stay hitch-free, and the
	// tag may already be serialized on the actor from a previous session or a duplicated
	// prefab instance.
	if (AActor* TagOwner = GetOwner())
	{
		TagOwner->Tags.AddUnique(WorldVoxelPaint::PaintVolumeHostTag);
	}
	// Runtime component adds never re-broadcast the actor-level registration delegate; only
	// self-register once the actor has completed its own registration pass.
	const AActor* Owner = GetOwner();
	if (Owner && Owner->HasActorRegisteredAllComponents())
	{
		if (UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(GetWorld()))
		{
			Subsystem->RegisterVolume(this);
		}
	}
}

void UVoxelPaintVolumeComponent::OnUnregister()
{
	// M6 host auto-tag removal - tag-only override. Deliberately does NOT touch subsystem
	// registration (that contract stays on the actor-level delegates; see the stale-slot sweep
	// comment in VoxelPaintWorldSubsystem). Runs while this component is still registered
	// (ExecuteUnregisterEvents calls OnUnregister before clearing bRegistered), so exclude
	// 'this' explicitly and trust siblings' IsRegistered(). No Modify()/transaction, matching
	// the add side. Unregister/re-register churn (render-state cycles, reregister contexts)
	// nets out: remove-then-AddUnique, and PCG ByTag matching reads live Tags state, not
	// tag-change events.
	if (AActor* TagOwner = GetOwner())
	{
		bool bAnotherRegisteredVolume = false;
		TInlineComponentArray<UVoxelPaintVolumeComponent*> Components;
		TagOwner->GetComponents(Components);
		for (UVoxelPaintVolumeComponent* Component : Components)
		{
			if (Component != this && Component->IsRegistered())
			{
				bAnotherRegisteredVolume = true;
				break;
			}
		}
		if (!bAnotherRegisteredVolume)
		{
			TagOwner->Tags.Remove(WorldVoxelPaint::PaintVolumeHostTag);
		}
	}
	Super::OnUnregister();
}

void UVoxelPaintVolumeComponent::SendRenderTransform_Concurrent()
{
	Super::SendRenderTransform_Concurrent();
	if (UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(GetWorld()))
	{
		Subsystem->NotifyTransformChanged(this);
	}
}

#if WITH_EDITOR
bool UVoxelPaintVolumeComponent::AllowsPoolEdits() const
{
	if (IsTemplate())
	{
		return true;
	}
	return CreationMethod != EComponentCreationMethod::SimpleConstructionScript;
}

bool UVoxelPaintVolumeComponent::CanEditChange(const FProperty* InProperty) const
{
	if (!Super::CanEditChange(InProperty))
	{
		return false;
	}
	if (IsPoolGatedProperty(InProperty) && !AllowsPoolEdits())
	{
		return false;
	}
	return true;
}

bool UVoxelPaintVolumeComponent::CanEditChange(const FEditPropertyChain& PropertyChain) const
{
	if (!UObject::CanEditChange(PropertyChain))
	{
		return false;
	}
	const FEditPropertyChain::TDoubleLinkedListNode* MemberNode = PropertyChain.GetActiveMemberNode();
	if (!MemberNode)
	{
		MemberNode = PropertyChain.GetHead();
	}
	const FProperty* MemberProperty = MemberNode ? MemberNode->GetValue() : nullptr;
	if (MemberProperty && IsPoolGatedProperty(MemberProperty) && !AllowsPoolEdits())
	{
		return false;
	}
	return true;
}

void UVoxelPaintVolumeComponent::PreEditChange(FProperty* PropertyAboutToChange)
{
	Super::PreEditChange(PropertyAboutToChange);
	CapturePreEditPoolState();
}

void UVoxelPaintVolumeComponent::CapturePreEditPoolState()
{
	PreEditPoolCache = VolumeVariants;
	PreEditExpectationCache = ExpectedTopology;
}

void UVoxelPaintVolumeComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName MemberName = PropertyChangedEvent.MemberProperty
		? PropertyChangedEvent.MemberProperty->GetFName()
		: PropertyChangedEvent.GetPropertyName();

	// Undo/redo routes here with a NAME_None event and stale PreEdit caches; the guards
	// therefore key strictly on explicitly named pool edits.
	if (MemberName == GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VolumeVariants))
	{
		FString RevertReason;
		if (const UVoxelPaintVolumeData* Duplicate = FindIntroducedDuplicateEntry(PreEditPoolCache, VolumeVariants))
		{
			RevertReason = FString::Printf(
				TEXT("'%s' is already in this pool; a duplicate slot would alias the first occurrence"),
				*Duplicate->GetName());
		}
		else if (!AllowsPoolEdits() || IsTemplate())
		{
			if (const UVoxelPaintVolumeData* PrivateEntry = FindIntroducedPrivateEntry(PreEditPoolCache, VolumeVariants))
			{
				RevertReason = FString::Printf(
					TEXT("'%s' is Private and this pool is Blueprint-supplied; mark the asset Shared deliberately, or duplicate it and mark the copy Shared"),
					*PrivateEntry->GetName());
			}
		}

		if (!RevertReason.IsEmpty())
		{
			VolumeVariants = PreEditPoolCache;
			if (PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
			{
				ShowVoxelPaintComponentNotification(FString::Printf(
					TEXT("%s: pool edit reverted - %s."), *GetName(), *RevertReason));
			}
		}
		else if (!ExpectedTopology.bIsSet)
		{
			const UVoxelPaintVolumeData* FirstValidEntry = nullptr;
			for (const FVoxelPaintVariantPoolEntry& Entry : VolumeVariants)
			{
				if (Entry.Variant)
				{
					FirstValidEntry = Entry.Variant;
					break;
				}
			}
			if (FirstValidEntry)
			{
				ExpectedTopology.LocalHalfExtent = FirstValidEntry->LocalHalfExtent;
				ExpectedTopology.BrickCounts = FirstValidEntry->BrickCounts;
				ExpectedTopology.StorageOrigin = FirstValidEntry->StorageOrigin;
				ExpectedTopology.bIsSet = true;
			}
		}
	}
	else if (MemberName == GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, ExpectedTopology))
	{
		ExpectedTopology.bIsSet = true;
	}

	if (UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(GetWorld()))
	{
		Subsystem->NotifyPropertyChanged(this);
	}
}

void UVoxelPaintVolumeComponent::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);
	if (bDuplicateForPIE || IsTemplate() || !GIsEditor)
	{
		return;
	}
	const AActor* Owner = GetOwner();
	const UWorld* World = GetWorld();
	const bool bPreviewContext =
		(World == nullptr) || World->IsPreviewWorld()
#if WITH_EDITORONLY_DATA
		|| (Owner && Owner->bIsEditorPreviewActor)
#endif
		;
	if (bPreviewContext)
	{
		return;
	}

	TMap<UVoxelPaintVolumeData*, UVoxelPaintVolumeData*> PrivateCopies;
	for (FVoxelPaintVariantPoolEntry& Entry : VolumeVariants)
	{
		UVoxelPaintVolumeData* Source = Entry.Variant.Get();
		if (!Source || Source->SharingMode != EVoxelPaintAssetSharingMode::Private)
		{
			continue;
		}
		UVoxelPaintVolumeData*& Copy = PrivateCopies.FindOrAdd(Source);
		if (!Copy)
		{
			Copy = WorldVoxelPaint::DeepCopyVolumeDataForPlacement(Source, GetOwner());
		}
		if (Copy)
		{
			if (VariantOverride == Source)
			{
				VariantOverride = Copy;
			}
			Entry.Variant = Copy;
		}
	}
}

void UVoxelPaintVolumeComponent::ResetPoolToArchetype()
{
	const UVoxelPaintVolumeComponent* Archetype = Cast<UVoxelPaintVolumeComponent>(GetArchetype());
	if (!Archetype)
	{
		return;
	}
	FProperty* PoolProperty = FindFieldChecked<FProperty>(StaticClass(),
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VolumeVariants));
	Modify();
	PreEditChange(PoolProperty);
	VolumeVariants = Archetype->VolumeVariants;
	ExpectedTopology = Archetype->ExpectedTopology;
	FPropertyChangedEvent ChangedEvent(PoolProperty);
	PostEditChangeProperty(ChangedEvent);
}
#endif
