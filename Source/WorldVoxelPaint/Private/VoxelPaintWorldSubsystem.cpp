// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintWorldSubsystem.h"

#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectBaseUtility.h"
#include "VoxelPaintCoordinator.h"
#include "VoxelPaintStats.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "WorldVoxelPaintConstants.h"

UVoxelPaintWorldSubsystem* UVoxelPaintWorldSubsystem::Get(const UWorld* World)
{
	return World ? World->GetSubsystem<UVoxelPaintWorldSubsystem>() : nullptr;
}

void UVoxelPaintWorldSubsystem::PostInitialize()
{
	Super::PostInitialize();

	EnsureFrontier();

	UWorld* World = GetWorld();
	PostRegisterHandle = World->AddOnPostRegisterAllActorComponentsHandler(
		FOnPostRegisterAllActorComponents::FDelegate::CreateUObject(
			this, &UVoxelPaintWorldSubsystem::OnActorPostRegisterAllComponents));
	PreUnregisterHandle = World->AddOnPreUnregisterAllActorComponentsHandler(
		FOnPreUnregisterAllActorComponents::FDelegate::CreateUObject(
			this, &UVoxelPaintWorldSubsystem::OnActorPreUnregisterAllComponents));

	for (ULevel* Level : World->GetLevels())
	{
		if (!Level)
		{
			continue;
		}
		for (AActor* Actor : Level->Actors)
		{
			if (Actor && Actor->HasActorRegisteredAllComponents())
			{
				OnActorPostRegisterAllComponents(Actor);
			}
		}
	}
}

void UVoxelPaintWorldSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		if (PostRegisterHandle.IsValid())
		{
			World->RemoveOnPostRegisterAllActorComponentsHandler(PostRegisterHandle);
		}
		if (PreUnregisterHandle.IsValid())
		{
			World->RemoveOnPreUnregisterAllActorComponentsHandler(PreUnregisterHandle);
		}
	}
	PostRegisterHandle.Reset();
	PreUnregisterHandle.Reset();

	if (FVoxelPaintCoordinator* Coordinator = FVoxelPaintCoordinator::GetIfCreated())
	{
		Coordinator->NotifyWorldTeardown(GetWorld());
	}

	Super::Deinitialize();
}

void UVoxelPaintWorldSubsystem::RegisterVolume(UVoxelPaintVolumeComponent* Component)
{
	EnsureFrontier();
	// Forced: the Reserved-identity search below consumes demotion state. A component destroyed
	// at component level (no actor pre-unregister delegate, so no UnregisterVolume) leaves its
	// slot Occupied-with-dead-weak until a sweep demotes it, and a same-frame replacement on the
	// same actor (SCS construction-script rerun) must reclaim that exact slot, exactly as when
	// the sweep was ungated. Registration is rare and already O(N) in the slot searches below.
	ExpireStaleReservations(/*bForceSweep*/ true);

	if (!PassesRegistrationFilters(Component))
	{
		return;
	}
	if (Component->GetRegistrySlotIndex() != INDEX_NONE)
	{
		// Instance-data reapplication after a construction-script rerun re-registers the
		// already-slotted component (PostApplyToComponent -> ReregisterComponent) - the slot
		// cache must re-resolve here or the pin/blend edit never reaches the frontier.
		NotifyPropertyChanged(Component);
		return;
	}

	const int32 RegistrySlot = Registry.EmplaceAtLowestFreeIndex(LowestFreeIndexSearchStart, Component);
	Component->SetRegistrySlotIndex(RegistrySlot);

	const FVoxelPaintFrontierIdentity Identity = MakeFrontierIdentity(Component);

	int32 TargetSlot = INDEX_NONE;
	for (int32 Index = 0; Index < FrontierSlots.Num(); ++Index)
	{
		if (FrontierSlots[Index].State == EFrontierSlotState::Reserved
			&& FrontierSlots[Index].Identity == Identity)
		{
			TargetSlot = Index;
			break;
		}
	}
	if (TargetSlot == INDEX_NONE)
	{
		for (int32 Index = 0; Index < FrontierSlots.Num(); ++Index)
		{
			if (FrontierSlots[Index].State == EFrontierSlotState::Empty)
			{
				TargetSlot = Index;
				break;
			}
		}
	}

	if (TargetSlot != INDEX_NONE)
	{
		OccupyFrontierSlot(TargetSlot, Component, Identity);
	}
	else
	{
		FWaiter& Waiter = WaiterQueue.AddDefaulted_GetRef();
		Waiter.Identity = Identity;
		Waiter.Component = Component;
		UE_LOG(LogWorldVoxelPaint, Warning,
			TEXT("[WorldVoxelPaint] Selected-placement frontier is full (%d); '%s' will not render until a slot frees."),
			WorldVoxelPaint::MaxSelectedPlacements,
			*GetPathNameSafe(Component));
	}
}

void UVoxelPaintWorldSubsystem::UnregisterVolume(UVoxelPaintVolumeComponent* Component)
{
	EnsureFrontier();
	// Forced for the same reason as RegisterVolume: every structural call keeps the ungated
	// sweep's demotion timing exactly; only the per-notification sweeps are frame-gated.
	ExpireStaleReservations(/*bForceSweep*/ true);

	if (!Component || Component->GetRegistrySlotIndex() == INDEX_NONE)
	{
		return;
	}

	const int32 RegistrySlot = Component->GetRegistrySlotIndex();
	if (Registry.IsValidIndex(RegistrySlot)
		&& Registry[RegistrySlot].GetEvenIfUnreachable() == Component)
	{
		Registry.RemoveAt(RegistrySlot);
		LowestFreeIndexSearchStart = FMath::Min(LowestFreeIndexSearchStart, RegistrySlot);
	}
	Component->SetRegistrySlotIndex(INDEX_NONE);

	const int32 FrontierIndex = FindOccupiedFrontierSlot(Component);
	if (FrontierIndex != INDEX_NONE)
	{
		FFrontierSlot& Slot = FrontierSlots[FrontierIndex];
		Slot.State = EFrontierSlotState::Reserved;
		Slot.Component.Reset();
		Slot.ReservedFrame = GFrameCounter;
		Component->SetFrontierSlotIndex(INDEX_NONE);
		++LayoutGeneration;
	}
	else
	{
		const int32 WaiterIndex = FindWaiterIndex(Component);
		if (WaiterIndex != INDEX_NONE)
		{
			WaiterQueue.RemoveAt(WaiterIndex);
		}
	}
}

void UVoxelPaintWorldSubsystem::NotifyTransformChanged(UVoxelPaintVolumeComponent* Component)
{
	EnsureFrontier();
	ExpireStaleReservations();

	// A garbage component's slot may still be Occupied when the frame-gated sweep already ran
	// this frame (the ungated sweep used to demote it before the lookup); skipping here keeps
	// the notify a no-op for dead components, exactly as before the gate.
	if (!IsValid(Component))
	{
		return;
	}
	const int32 FrontierIndex = FindOccupiedFrontierSlot(Component);
	if (FrontierIndex == INDEX_NONE)
	{
		return;
	}

	FFrontierSlot& Slot = FrontierSlots[FrontierIndex];
	const FTransform Frame = Component->GetRigidPlacementFrame();
	if (Frame.GetRotation() == Slot.CachedState.Rotation
		&& Frame.GetTranslation() == Slot.CachedState.Translation)
	{
		return;
	}

	RefreshCachedState(Slot, Slot.CachedState);
	++LayoutGeneration;
}

void UVoxelPaintWorldSubsystem::NotifyPropertyChanged(UVoxelPaintVolumeComponent* Component)
{
	EnsureFrontier();
	ExpireStaleReservations();

	// See NotifyTransformChanged: dead components must stay no-ops under the frame-gated sweep.
	if (!IsValid(Component))
	{
		return;
	}
	const int32 FrontierIndex = FindOccupiedFrontierSlot(Component);
	if (FrontierIndex == INDEX_NONE)
	{
		return;
	}

	FFrontierSlot& Slot = FrontierSlots[FrontierIndex];
	FVoxelPaintPlacementState NewState;
	RefreshCachedState(Slot, NewState);

	const FVoxelPaintPlacementState& OldState = Slot.CachedState;
	const bool bLayoutChanged = NewState.Rotation != OldState.Rotation
		|| NewState.Translation != OldState.Translation
		|| NewState.LocalHalfExtent != OldState.LocalHalfExtent
		|| NewState.TexelSize != OldState.TexelSize
		|| NewState.BrickCounts != OldState.BrickCounts
		|| NewState.StorageMin != OldState.StorageMin
		|| NewState.AssetKey != OldState.AssetKey;
	const bool bBindingChanged = NewState.BlendCoefficients != OldState.BlendCoefficients
		|| NewState.SourceGeneration != OldState.SourceGeneration;

	if (!bLayoutChanged && !bBindingChanged)
	{
		return;
	}

	Slot.CachedState = NewState;
	if (bLayoutChanged)
	{
		++LayoutGeneration;
	}
	if (bBindingChanged)
	{
		++BindingGeneration;
	}
}

uint64 UVoxelPaintWorldSubsystem::GetLayoutGeneration() const
{
	return LayoutGeneration;
}

uint64 UVoxelPaintWorldSubsystem::GetBindingGeneration() const
{
	return BindingGeneration;
}

void UVoxelPaintWorldSubsystem::ForceLayoutRepack(int32 NumFrames)
{
	++LayoutGeneration;
	PendingForcedRepackFrames = FMath::Max(NumFrames - 1, PendingForcedRepackFrames);
	LastForcedRepackFrame = GFrameCounter;
}

int32 UVoxelPaintWorldSubsystem::CountRegisteredVolumes() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<UVoxelPaintVolumeComponent>& Entry : Registry)
	{
		if (Entry.IsValid())
		{
			++Count;
		}
	}
	return Count;
}

int32 UVoxelPaintWorldSubsystem::GetNumFrontierMembers() const
{
	int32 Count = 0;
	for (const FFrontierSlot& Slot : FrontierSlots)
	{
		if (Slot.State == EFrontierSlotState::Occupied)
		{
			++Count;
		}
	}
	return Count;
}

int32 UVoxelPaintWorldSubsystem::GetFrontierPosition(const UVoxelPaintVolumeComponent* Component) const
{
	return FindOccupiedFrontierSlot(Component);
}

bool UVoxelPaintWorldSubsystem::GetCachedPlacementState(const UVoxelPaintVolumeComponent* Component,
	FVoxelPaintPlacementState& OutState) const
{
	const int32 FrontierIndex = FindOccupiedFrontierSlot(Component);
	if (FrontierIndex == INDEX_NONE)
	{
		return false;
	}
	OutState = FrontierSlots[FrontierIndex].CachedState;
	return true;
}

UVoxelPaintVolumeData* UVoxelPaintWorldSubsystem::GetResolvedVariant(
	const UVoxelPaintVolumeComponent* Component) const
{
	const int32 FrontierIndex = FindOccupiedFrontierSlot(Component);
	if (FrontierIndex != INDEX_NONE)
	{
		return FrontierSlots[FrontierIndex].ResolvedVariant.Get();
	}
	return Component ? Component->ResolveVariantLocal() : nullptr;
}

void UVoxelPaintWorldSubsystem::BuildDesiredSnapshot(TArray<FVoxelPaintPlacementState>& OutPlacements) const
{
	OutPlacements.Reset();
	for (const FFrontierSlot& Slot : FrontierSlots)
	{
		if (Slot.State != EFrontierSlotState::Occupied || !Slot.Component.IsValid())
		{
			continue;
		}
		OutPlacements.Add(Slot.CachedState);
	}
}

void UVoxelPaintWorldSubsystem::EnumerateFrontier(TFunctionRef<void(const FVoxelPaintPlacementState&)> Fn) const
{
	for (const FFrontierSlot& Slot : FrontierSlots)
	{
		if (Slot.State != EFrontierSlotState::Occupied || !Slot.Component.IsValid())
		{
			continue;
		}
		Fn(Slot.CachedState);
	}
}

void UVoxelPaintWorldSubsystem::EnumerateFrontierSlots(TFunctionRef<void(int32,
	const UVoxelPaintVolumeComponent&, const FVoxelPaintPlacementState&)> Fn) const
{
	for (int32 Index = 0; Index < FrontierSlots.Num(); ++Index)
	{
		const FFrontierSlot& Slot = FrontierSlots[Index];
		const UVoxelPaintVolumeComponent* Component = Slot.Component.Get();
		if (Slot.State != EFrontierSlotState::Occupied || !Component)
		{
			continue;
		}
		Fn(Index, *Component, Slot.CachedState);
	}
}

void UVoxelPaintWorldSubsystem::BuildRecordSnapshot(const FVector& EvaluationOrigin,
	TArray<FVoxelPaintVolumeRecord>& OutRecords) const
{
	OutRecords.Reset();
	const FVoxelPaintResolvedBinding DefaultBinding;
	for (const FFrontierSlot& Slot : FrontierSlots)
	{
		if (Slot.State != EFrontierSlotState::Occupied || !Slot.Component.IsValid())
		{
			continue;
		}
		OutRecords.Emplace(PackVolumeRecord(Slot.CachedState, DefaultBinding, EvaluationOrigin));
	}
}

void UVoxelPaintWorldSubsystem::OnActorPostRegisterAllComponents(AActor* Actor)
{
	if (!Actor)
	{
		return;
	}
	TInlineComponentArray<UVoxelPaintVolumeComponent*> Components;
	Actor->GetComponents(Components);
	for (UVoxelPaintVolumeComponent* Component : Components)
	{
		RegisterVolume(Component);
	}
}

void UVoxelPaintWorldSubsystem::OnActorPreUnregisterAllComponents(AActor* Actor)
{
	if (!Actor)
	{
		return;
	}
	TInlineComponentArray<UVoxelPaintVolumeComponent*> Components;
	Actor->GetComponents(Components);
	for (UVoxelPaintVolumeComponent* Component : Components)
	{
		UnregisterVolume(Component);
	}
}

void UVoxelPaintWorldSubsystem::EnsureFrontier()
{
	const int32 PreviousNum = FrontierSlots.Num();
	if (PreviousNum == WorldVoxelPaint::MaxSelectedPlacements)
	{
		return;
	}
	FrontierSlots.SetNum(WorldVoxelPaint::MaxSelectedPlacements);

	// A live capacity grow (Live Coding / hot reload raising MaxSelectedPlacements) must admit
	// existing waiters into the grown Empty slots before any later registration can claim
	// them, or new registrants would leapfrog the FIFO queue. A shrink still requires an
	// editor restart: occupied tail slots are dropped here without promotion or repack.
	for (int32 Index = PreviousNum; Index < FrontierSlots.Num() && WaiterQueue.Num() > 0; ++Index)
	{
		while (WaiterQueue.Num() > 0)
		{
			const FWaiter Waiter = WaiterQueue[0];
			WaiterQueue.RemoveAt(0);
			UVoxelPaintVolumeComponent* WaiterComponent = Waiter.Component.Get();
			if (WaiterComponent && WaiterComponent->GetRegistrySlotIndex() != INDEX_NONE)
			{
				OccupyFrontierSlot(Index, WaiterComponent, Waiter.Identity);
				break;
			}
		}
	}
}

void UVoxelPaintWorldSubsystem::ExpireStaleReservations(bool bForceSweep)
{
	// Frame-gated so mutating calls within one frame cannot drain a forced-repack burst early;
	// the coordinator's once-per-rendered-frame call advances it.
	if (PendingForcedRepackFrames > 0 && GFrameCounter != LastForcedRepackFrame)
	{
		LastForcedRepackFrame = GFrameCounter;
		--PendingForcedRepackFrames;
		++LayoutGeneration;
	}

	// Reservations stamped this frame cannot expire until a later frame (ReservedFrame <
	// GFrameCounter below), so the expirable set is frozen at frame start: the frame's first
	// sweep consumes all of it and repeat same-frame sweeps are no-ops on it. Gate them out so
	// per-notification callers stop paying O(N); != (not <) because tests rewind GFrameCounter.
	// bForceSweep (Register/Unregister and the coordinator's per-frame call) bypasses the gate:
	// the registration slot search consumes demotion state (same-frame identity reclaim of a
	// dead component's slot), and the coordinator must demote a component that died after this
	// frame's first sweep before the snapshot pack.
	if (!bForceSweep && GFrameCounter == LastReservationSweepFrame)
	{
		return;
	}
	LastReservationSweepFrame = GFrameCounter;

	const int32 MaxRegistryIndex = Registry.GetMaxIndex();
	const int32 NumSweepSteps = FMath::Min(MaxRegistryIndex, 64);
	for (int32 Step = 0; Step < NumSweepSteps; ++Step)
	{
		if (RegistrySweepCursor >= MaxRegistryIndex)
		{
			RegistrySweepCursor = 0;
		}
		const int32 RegistryIndex = RegistrySweepCursor++;
		if (Registry.IsValidIndex(RegistryIndex) && !Registry[RegistryIndex].IsValid())
		{
			Registry.RemoveAt(RegistryIndex);
			LowestFreeIndexSearchStart = FMath::Min(LowestFreeIndexSearchStart, RegistryIndex);
		}
	}

	for (int32 Index = 0; Index < FrontierSlots.Num(); ++Index)
	{
		FFrontierSlot& Slot = FrontierSlots[Index];
		if (Slot.State == EFrontierSlotState::Occupied && !Slot.Component.IsValid())
		{
			Slot.State = EFrontierSlotState::Reserved;
			Slot.Component.Reset();
			Slot.ReservedFrame = GFrameCounter;
			++LayoutGeneration;
		}
		if (Slot.State != EFrontierSlotState::Reserved || Slot.ReservedFrame >= GFrameCounter)
		{
			continue;
		}

		Slot.State = EFrontierSlotState::Empty;
		Slot.Component.Reset();

		while (WaiterQueue.Num() > 0)
		{
			const FWaiter Waiter = WaiterQueue[0];
			WaiterQueue.RemoveAt(0);
			UVoxelPaintVolumeComponent* WaiterComponent = Waiter.Component.Get();
			if (WaiterComponent && WaiterComponent->GetRegistrySlotIndex() != INDEX_NONE)
			{
				OccupyFrontierSlot(Index, WaiterComponent, Waiter.Identity);
				break;
			}
		}
	}

	UpdateStats();
}

bool UVoxelPaintWorldSubsystem::PassesRegistrationFilters(const UVoxelPaintVolumeComponent* Component) const
{
	if (!Component || Component->IsTemplate()
		|| Component->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		return false;
	}
	const AActor* Owner = Component->GetOwner();
	if (!Owner)
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	if (Owner->bIsEditorPreviewActor)
	{
		return false;
	}
#endif
	if (Component->GetWorld() != GetWorld())
	{
		return false;
	}
	return true;
}

int32 UVoxelPaintWorldSubsystem::FindOccupiedFrontierSlot(const UVoxelPaintVolumeComponent* Component) const
{
	if (!Component)
	{
		return INDEX_NONE;
	}
	// The cache is written by OccupyFrontierSlot (sole Occupied writer) and cleared on
	// demotion; a stale value can only fail this verification, never alias another slot.
	const int32 CachedIndex = Component->GetFrontierSlotIndex();
	int32 Result = INDEX_NONE;
	if (FrontierSlots.IsValidIndex(CachedIndex)
		&& FrontierSlots[CachedIndex].State == EFrontierSlotState::Occupied
		&& FrontierSlots[CachedIndex].Component.GetEvenIfUnreachable() == Component)
	{
		Result = CachedIndex;
	}
#if DO_GUARD_SLOW
	int32 LinearResult = INDEX_NONE;
	for (int32 Index = 0; Index < FrontierSlots.Num(); ++Index)
	{
		if (FrontierSlots[Index].State == EFrontierSlotState::Occupied
			&& FrontierSlots[Index].Component.GetEvenIfUnreachable() == Component)
		{
			LinearResult = Index;
			break;
		}
	}
	checkSlow(Result == LinearResult);
#endif
	return Result;
}

int32 UVoxelPaintWorldSubsystem::FindWaiterIndex(const UVoxelPaintVolumeComponent* Component) const
{
	if (!Component)
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index < WaiterQueue.Num(); ++Index)
	{
		if (WaiterQueue[Index].Component.GetEvenIfUnreachable() == Component)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

void UVoxelPaintWorldSubsystem::OccupyFrontierSlot(int32 FrontierIndex, UVoxelPaintVolumeComponent* Component,
	const FVoxelPaintFrontierIdentity& Identity)
{
	FFrontierSlot& Slot = FrontierSlots[FrontierIndex];
	Slot.State = EFrontierSlotState::Occupied;
	Slot.Identity = Identity;
	Slot.Component = Component;
	Component->SetFrontierSlotIndex(FrontierIndex);
	Slot.ReservedFrame = 0;
	RefreshCachedState(Slot, Slot.CachedState);
	++LayoutGeneration;
}

FVoxelPaintFrontierIdentity UVoxelPaintWorldSubsystem::MakeFrontierIdentity(const UVoxelPaintVolumeComponent* Component)
{
#if WITH_EDITOR
	return Component->GetOwner()->GetActorInstanceGuid();
#else
	return FObjectKey(Component->GetOwner());
#endif
}

void UVoxelPaintWorldSubsystem::RefreshCachedState(FFrontierSlot& Slot,
	FVoxelPaintPlacementState& OutState)
{
	const UVoxelPaintVolumeComponent* Component = Slot.Component.Get();
	if (!Component)
	{
		OutState = FVoxelPaintPlacementState();
		OutState.Identity = Slot.Identity;
		Slot.ResolvedVariantIndex = INDEX_NONE;
		Slot.ResolvedVariant.Reset();
		return;
	}

	UVoxelPaintVolumeData* Resolved = nullptr;
#if WITH_EDITOR
	if (const FObjectKey* ForcedKey = ForcedVariantOverrides.Find(Slot.Identity))
	{
		UVoxelPaintVolumeData* Forced = Cast<UVoxelPaintVolumeData>(ForcedKey->ResolveObjectPtr());
		if (Forced && Component->PoolContains(Forced))
		{
			Resolved = Forced;
		}
	}
#endif
	if (!Resolved)
	{
		Resolved = Component->ResolveVariantLocal();
	}

	Slot.ResolvedVariantIndex = INDEX_NONE;
	Slot.ResolvedVariant = Resolved;
	if (Resolved)
	{
		for (int32 Index = 0; Index < Component->VolumeVariants.Num(); ++Index)
		{
			if (Component->VolumeVariants[Index].Variant == Resolved)
			{
				Slot.ResolvedVariantIndex = Index;
				break;
			}
		}
	}

	const FTransform Frame = Component->GetRigidPlacementFrame();
	OutState.Rotation = Frame.GetRotation();
	OutState.Translation = Frame.GetTranslation();
	OutState.LocalHalfExtent = FVector3f(Component->GetEffectiveLocalHalfExtent());
	OutState.TexelSize = FVector3f(Component->GetEffectiveTexelSize());
	OutState.BlendCoefficients = Component->GetBlendCoefficients();
	if (Resolved)
	{
		OutState.StorageMin = FVector3f(Resolved->GetStorageMin());
		OutState.BrickCounts = Resolved->GetBrickCounts();
		OutState.AssetKey = FObjectKey(Resolved);
		OutState.SourceGeneration = Resolved->GetSourceGeneration();
	}
	else
	{
		const FVoxelPaintExpectedTopology& Expected = Component->ExpectedTopology;
		if (Expected.bIsSet)
		{
			OutState.StorageMin = FVector3f(Expected.StorageOrigin - Expected.LocalHalfExtent);
			OutState.BrickCounts = Expected.BrickCounts;
		}
		else
		{
			OutState.StorageMin = -OutState.LocalHalfExtent;
			OutState.BrickCounts = FIntVector(4, 4, 4);
		}
		OutState.AssetKey = FObjectKey();
		OutState.SourceGeneration = 0;
	}
	OutState.Flags = Component->HasValidVolumeData() ? WorldVoxelPaint::RecordFlag_Valid : 0u;
	OutState.Identity = Slot.Identity;
}

#if WITH_EDITOR
void UVoxelPaintWorldSubsystem::SetForcedVariant(const FVoxelPaintFrontierIdentity& Identity,
	const UVoxelPaintVolumeData* Asset)
{
	EnsureFrontier();
	PruneStaleForcedVariants();
	ForcedVariantOverrides.Add(Identity, FObjectKey(Asset));
	ReResolveForcedIdentity(Identity);
}

void UVoxelPaintWorldSubsystem::ClearForcedVariant(const FVoxelPaintFrontierIdentity& Identity)
{
	EnsureFrontier();
	PruneStaleForcedVariants();
	if (ForcedVariantOverrides.Remove(Identity) > 0)
	{
		ReResolveForcedIdentity(Identity);
	}
}

void UVoxelPaintWorldSubsystem::ClearAllForcedVariants()
{
	EnsureFrontier();
	TArray<FVoxelPaintFrontierIdentity> ClearedIdentities;
	ForcedVariantOverrides.GenerateKeyArray(ClearedIdentities);
	ForcedVariantOverrides.Empty();
	for (const FVoxelPaintFrontierIdentity& Identity : ClearedIdentities)
	{
		ReResolveForcedIdentity(Identity);
	}
}

bool UVoxelPaintWorldSubsystem::GetForcedVariant(const FVoxelPaintFrontierIdentity& Identity,
	FObjectKey& OutAsset) const
{
	if (const FObjectKey* Found = ForcedVariantOverrides.Find(Identity))
	{
		OutAsset = *Found;
		return true;
	}
	return false;
}

void UVoxelPaintWorldSubsystem::ReResolveForcedIdentity(const FVoxelPaintFrontierIdentity& Identity)
{
	UVoxelPaintVolumeComponent* Component = nullptr;
	for (const FFrontierSlot& Slot : FrontierSlots)
	{
		if (Slot.State == EFrontierSlotState::Occupied && Slot.Identity == Identity)
		{
			Component = Slot.Component.Get();
			break;
		}
	}
	if (Component)
	{
		NotifyPropertyChanged(Component);
	}
}

void UVoxelPaintWorldSubsystem::PruneStaleForcedVariants()
{
	for (TMap<FVoxelPaintFrontierIdentity, FObjectKey>::TIterator It(ForcedVariantOverrides); It; ++It)
	{
		const UVoxelPaintVolumeData* Asset = Cast<UVoxelPaintVolumeData>(It->Value.ResolveObjectPtr());
		if (!Asset)
		{
			It.RemoveCurrent();
			continue;
		}
		for (const FFrontierSlot& Slot : FrontierSlots)
		{
			if (Slot.State == EFrontierSlotState::Occupied && Slot.Identity == It->Key)
			{
				const UVoxelPaintVolumeComponent* Component = Slot.Component.Get();
				if (Component && !Component->PoolContains(Asset))
				{
					It.RemoveCurrent();
				}
				break;
			}
		}
	}
}
#endif

void UVoxelPaintWorldSubsystem::UpdateStats() const
{
	SET_DWORD_STAT(STAT_VoxelPaint_LiveVolumes, Registry.Num());
	SET_DWORD_STAT(STAT_VoxelPaint_FrontierMembers, GetNumFrontierMembers());
	SET_DWORD_STAT(STAT_VoxelPaint_FrontierWaiters, WaiterQueue.Num());
	SET_DWORD_STAT(STAT_VoxelPaint_FrontierOverflowed, WaiterQueue.Num() > 0 ? 1 : 0);
}
