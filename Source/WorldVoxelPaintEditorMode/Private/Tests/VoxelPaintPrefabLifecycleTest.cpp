// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "VoxelPaintPrefabAuthoring.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintTemplateRouting.h"
#include "VoxelPaintTypes.h"
#include "VoxelPaintVolume.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"
#include "VoxelPaintWorldSubsystem.h"

namespace VoxelPaintPrefabLifecycleTestPrivate
{

struct FVoxelPaintTestWorld
{
	UWorld* World = nullptr;

	FVoxelPaintTestWorld()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelPaintPrefabLifecycleTest"));
		FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
		WorldContext.SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
	}

	~FVoxelPaintTestWorld()
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}
};

AVoxelPaintVolume* SpawnTestVolume(UWorld* World, const FVector& Location)
{
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World->SpawnActor<AVoxelPaintVolume>(Location, FRotator::ZeroRotator, SpawnParameters);
}

UVoxelPaintVolumeData* MakeAsset(EVoxelPaintAssetSharingMode SharingMode)
{
	UVoxelPaintVolumeData* Asset = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	Asset->SharingMode = SharingMode;
	return Asset;
}

void AddPoolEntry(UVoxelPaintVolumeComponent* Component, UVoxelPaintVolumeData* Asset, float Weight)
{
	FVoxelPaintVariantPoolEntry& Entry = Component->VolumeVariants.AddDefaulted_GetRef();
	Entry.Variant = Asset;
	Entry.Weight = Weight;
}

UBlueprint* CreatePaintActorBlueprint(const TCHAR* BaseName, UClass* ParentClass,
	const TCHAR* ComponentVariableName, USCS_Node*& OutNode)
{
	OutNode = nullptr;
	const FName BlueprintName =
		MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), BaseName);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(ParentClass, GetTransientPackage(),
		BlueprintName, BPTYPE_Normal, NAME_None);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return Blueprint;
	}
	if (ComponentVariableName)
	{
		OutNode = Blueprint->SimpleConstructionScript->CreateNode(
			UVoxelPaintVolumeComponent::StaticClass(), ComponentVariableName);
		Blueprint->SimpleConstructionScript->AddNode(OutNode);
	}
	return Blueprint;
}

AActor* SpawnPrefabInstance(UWorld* World, UBlueprint* Blueprint, const FVector& Location)
{
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World->SpawnActor<AActor>(Blueprint->GeneratedClass, Location, FRotator::ZeroRotator,
		SpawnParameters);
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintVariants_PinRemapOnDuplicate,
	"Game.WorldVoxelPaint.Unit.Variants.PinRemapOnDuplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintVariants_PinRemapOnDuplicate::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintPrefabLifecycleTestPrivate;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;

	UVoxelPaintVolumeData* PrivateAsset = MakeAsset(EVoxelPaintAssetSharingMode::Private);
	UVoxelPaintVolumeData* SharedAsset = MakeAsset(EVoxelPaintAssetSharingMode::Shared);

	AVoxelPaintVolume* Source = SpawnTestVolume(World, FVector(10.0, 0.0, 0.0));
	UVoxelPaintVolumeComponent* SourceComponent = Source ? Source->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("Source volume component spawned"), SourceComponent))
	{
		return false;
	}
	AddPoolEntry(SourceComponent, PrivateAsset, 1.0f);
	AddPoolEntry(SourceComponent, SharedAsset, 1.0f);
	SourceComponent->VariantOverride = PrivateAsset;

	{
		AVoxelPaintVolume* Duplicate = Cast<AVoxelPaintVolume>(
			StaticDuplicateObject(Source, World->PersistentLevel));
		UVoxelPaintVolumeComponent* DuplicateComponent =
			Duplicate ? Duplicate->GetVolumeComponent() : nullptr;
		if (!TestNotNull(TEXT("Duplicated volume component exists"), DuplicateComponent)
			|| !TestEqual(TEXT("Duplicated pool keeps both slots"),
				DuplicateComponent->VolumeVariants.Num(), 2))
		{
			return false;
		}
		UVoxelPaintVolumeData* PrivateCopy = DuplicateComponent->VolumeVariants[0].Variant;
		TestNotNull(TEXT("Private entry was deep-copied"), PrivateCopy);
		TestTrue(TEXT("Private copy is a new asset"), PrivateCopy != PrivateAsset);
		if (PrivateCopy)
		{
			TestTrue(TEXT("Private copy stays Private"),
				PrivateCopy->SharingMode == EVoxelPaintAssetSharingMode::Private);
		}
		TestTrue(TEXT("Shared entry keeps the shared reference"),
			DuplicateComponent->VolumeVariants[1].Variant == SharedAsset);
		TestTrue(TEXT("Pin pointing at the replaced Private entry remaps to its copy"),
			DuplicateComponent->VariantOverride == PrivateCopy);
	}

	{
		SourceComponent->VariantOverride = SharedAsset;
		AVoxelPaintVolume* Duplicate = Cast<AVoxelPaintVolume>(
			StaticDuplicateObject(Source, World->PersistentLevel));
		UVoxelPaintVolumeComponent* DuplicateComponent =
			Duplicate ? Duplicate->GetVolumeComponent() : nullptr;
		if (!TestNotNull(TEXT("Second duplicated volume component exists"), DuplicateComponent))
		{
			return false;
		}
		TestTrue(TEXT("Pin pointing at a Shared entry is unchanged by duplication"),
			DuplicateComponent->VariantOverride == SharedAsset);
		TestTrue(TEXT("Private entry deep-copies independently of the pin"),
			DuplicateComponent->VolumeVariants[0].Variant != nullptr
				&& DuplicateComponent->VolumeVariants[0].Variant != PrivateAsset);
	}

	{
		UWorld* PreviewWorld =
			UWorld::CreateWorld(EWorldType::EditorPreview, false, TEXT("VoxelPaintPreviewWorld"));
		if (!TestNotNull(TEXT("Preview world created"), PreviewWorld))
		{
			return false;
		}
		AVoxelPaintVolume* PreviewDuplicate = Cast<AVoxelPaintVolume>(
			StaticDuplicateObject(Source, PreviewWorld->PersistentLevel));
		UVoxelPaintVolumeComponent* PreviewComponent =
			PreviewDuplicate ? PreviewDuplicate->GetVolumeComponent() : nullptr;
		if (TestNotNull(TEXT("Preview-world duplicate component exists"), PreviewComponent))
		{
			TestTrue(TEXT("Preview-world duplication keeps the Private reference (no fork)"),
				PreviewComponent->VolumeVariants[0].Variant == PrivateAsset);
			TestTrue(TEXT("Preview-world duplication keeps the Shared reference"),
				PreviewComponent->VolumeVariants[1].Variant == SharedAsset);
		}
		PreviewWorld->DestroyWorld(false);
	}

	{
		UVoxelPaintVolumeComponent* PieDuplicate = Cast<UVoxelPaintVolumeComponent>(
			StaticDuplicateObject(SourceComponent, Source, NAME_None, RF_AllFlags, nullptr,
				EDuplicateMode::PIE));
		if (TestNotNull(TEXT("PIE-mode duplicate component exists"), PieDuplicate))
		{
			TestTrue(TEXT("PIE duplication keeps the Private reference (guard holds)"),
				PieDuplicate->VolumeVariants[0].Variant == PrivateAsset);
		}
	}

	{
		UVoxelPaintVolumeComponent* TemplateComponent = NewObject<UVoxelPaintVolumeComponent>(
			GetTransientPackage(), TEXT("VPPinRemapTemplate"), RF_ArchetypeObject);
		AddPoolEntry(TemplateComponent, PrivateAsset, 1.0f);
		UVoxelPaintVolumeComponent* TemplateDuplicate = Cast<UVoxelPaintVolumeComponent>(
			StaticDuplicateObject(TemplateComponent, GetTransientPackage()));
		if (TestNotNull(TEXT("Template duplicate component exists"), TemplateDuplicate))
		{
			TestTrue(TEXT("Template duplicate stays a template"), TemplateDuplicate->IsTemplate());
			TestTrue(TEXT("Template duplication keeps the Private reference (guard holds)"),
				TemplateDuplicate->VolumeVariants[0].Variant == PrivateAsset);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintVariants_CanEditChangeGate,
	"Game.WorldVoxelPaint.Unit.Variants.CanEditChangeGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintVariants_CanEditChangeGate::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintPrefabLifecycleTestPrivate;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;

	FProperty* PoolProperty = FindFieldChecked<FProperty>(UVoxelPaintVolumeComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VolumeVariants));
	FProperty* ExpectationProperty = FindFieldChecked<FProperty>(UVoxelPaintVolumeComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, ExpectedTopology));
	FProperty* PinProperty = FindFieldChecked<FProperty>(UVoxelPaintVolumeComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VariantOverride));
	FProperty* BlendProperty = FindFieldChecked<FProperty>(UVoxelPaintVolumeComponent::StaticClass(),
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, BlendModeR));
	FProperty* PoolInnerProperty = CastFieldChecked<FArrayProperty>(PoolProperty)->Inner;
	FProperty* EntryVariantProperty =
		FVoxelPaintVariantPoolEntry::StaticStruct()->FindPropertyByName(TEXT("Variant"));
	FProperty* EntryWeightProperty =
		FVoxelPaintVariantPoolEntry::StaticStruct()->FindPropertyByName(TEXT("Weight"));
	FProperty* ExpectationExtentProperty =
		FVoxelPaintExpectedTopology::StaticStruct()->FindPropertyByName(TEXT("LocalHalfExtent"));
	if (!TestNotNull(TEXT("Pool entry Variant property found"), EntryVariantProperty)
		|| !TestNotNull(TEXT("Pool entry Weight property found"), EntryWeightProperty)
		|| !TestNotNull(TEXT("Expectation LocalHalfExtent property found"), ExpectationExtentProperty))
	{
		return false;
	}

	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Owner actor spawned"), Owner))
	{
		return false;
	}

	UVoxelPaintVolumeComponent* ScsInstance =
		NewObject<UVoxelPaintVolumeComponent>(Owner, TEXT("VPScsGateInstance"));
	ScsInstance->CreationMethod = EComponentCreationMethod::SimpleConstructionScript;

	TestFalse(TEXT("SCS instance refuses VolumeVariants edits"),
		ScsInstance->CanEditChange(PoolProperty));
	TestFalse(TEXT("SCS instance refuses ExpectedTopology edits"),
		ScsInstance->CanEditChange(ExpectationProperty));
	TestFalse(TEXT("SCS instance refuses the array INNER struct property (owner-property rule)"),
		ScsInstance->CanEditChange(PoolInnerProperty));
	TestFalse(TEXT("SCS instance refuses the pool entry Variant member (owner-struct rule)"),
		ScsInstance->CanEditChange(EntryVariantProperty));
	TestFalse(TEXT("SCS instance refuses the pool entry Weight member (owner-struct rule)"),
		ScsInstance->CanEditChange(EntryWeightProperty));
	TestFalse(TEXT("SCS instance refuses the expectation LocalHalfExtent member (owner-struct rule)"),
		ScsInstance->CanEditChange(ExpectationExtentProperty));
	TestTrue(TEXT("SCS instance allows the variant pin"),
		ScsInstance->CanEditChange(PinProperty));
	TestTrue(TEXT("SCS instance allows blend modes"),
		ScsInstance->CanEditChange(BlendProperty));

	UVoxelPaintVolumeComponent* PastedTemplate = NewObject<UVoxelPaintVolumeComponent>(
		GetTransientPackage(), TEXT("VPScsGateTemplate"), RF_ArchetypeObject);
	PastedTemplate->CreationMethod = EComponentCreationMethod::SimpleConstructionScript;
	TestTrue(TEXT("A template stays pool-editable even with SCS creation method (pasted template)"),
		PastedTemplate->CanEditChange(PoolProperty));
	TestTrue(TEXT("A template stays expectation-editable"),
		PastedTemplate->CanEditChange(ExpectationProperty));
	TestTrue(TEXT("A template allows the inner struct property"),
		PastedTemplate->CanEditChange(PoolInnerProperty));
	TestTrue(TEXT("A template allows the pool entry Variant member"),
		PastedTemplate->CanEditChange(EntryVariantProperty));
	TestTrue(TEXT("A template allows the pool entry Weight member"),
		PastedTemplate->CanEditChange(EntryWeightProperty));
	TestTrue(TEXT("A template allows the expectation LocalHalfExtent member"),
		PastedTemplate->CanEditChange(ExpectationExtentProperty));

	AVoxelPaintVolume* NativeVolume = SpawnTestVolume(World, FVector(50.0, 0.0, 0.0));
	UVoxelPaintVolumeComponent* NativeComponent =
		NativeVolume ? NativeVolume->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("Native placed component spawned"), NativeComponent))
	{
		return false;
	}
	TestTrue(TEXT("Native placed component allows pool edits"),
		NativeComponent->CanEditChange(PoolProperty));
	TestTrue(TEXT("Native placed component allows expectation edits"),
		NativeComponent->CanEditChange(ExpectationProperty));
	TestTrue(TEXT("Native placed component allows the pool entry Variant member"),
		NativeComponent->CanEditChange(EntryVariantProperty));

	UVoxelPaintVolumeComponent* InstanceComponent =
		NewObject<UVoxelPaintVolumeComponent>(Owner, TEXT("VPInstanceGateComponent"));
	InstanceComponent->CreationMethod = EComponentCreationMethod::Instance;
	TestTrue(TEXT("Instance-added component allows pool edits"),
		InstanceComponent->CanEditChange(PoolProperty));
	TestTrue(TEXT("Instance-added component allows the expectation LocalHalfExtent member"),
		InstanceComponent->CanEditChange(ExpectationExtentProperty));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintVariants_ReinstancingIdentity,
	"Game.WorldVoxelPaint.Unit.Variants.ReinstancingIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintVariants_ReinstancingIdentity::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintPrefabLifecycleTestPrivate;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(World);
	if (!TestNotNull(TEXT("Subsystem exists in the test world"), Subsystem))
	{
		return false;
	}

	USCS_Node* Node = nullptr;
	UBlueprint* Blueprint =
		CreatePaintActorBlueprint(TEXT("VPReinstanceBP"), AActor::StaticClass(), TEXT("PaintPool"), Node);
	if (!TestNotNull(TEXT("Transient Blueprint created"), Blueprint)
		|| !TestNotNull(TEXT("SCS paint component node added"), Node))
	{
		return false;
	}

	UVoxelPaintVolumeData* AssetA = MakeAsset(EVoxelPaintAssetSharingMode::Shared);
	UVoxelPaintVolumeData* AssetB = MakeAsset(EVoxelPaintAssetSharingMode::Shared);
	{
		UVoxelPaintVolumeComponent* Template =
			Cast<UVoxelPaintVolumeComponent>(Node->ComponentTemplate);
		if (!TestNotNull(TEXT("SCS node carries a paint component template"), Template))
		{
			return false;
		}
		AddPoolEntry(Template, AssetA, 1.0f);
		AddPoolEntry(Template, AssetB, 1.0f);
		Template->ExpectedTopology.LocalHalfExtent = AssetA->LocalHalfExtent;
		Template->ExpectedTopology.BrickCounts = AssetA->BrickCounts;
		Template->ExpectedTopology.StorageOrigin = AssetA->StorageOrigin;
		Template->ExpectedTopology.bIsSet = true;
	}
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	AActor* Prefab = SpawnPrefabInstance(World, Blueprint, FVector(100.0, 0.0, 0.0));
	UVoxelPaintVolumeComponent* Component =
		Prefab ? Prefab->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
	if (!TestNotNull(TEXT("Prefab instance carries the paint component"), Component))
	{
		return false;
	}
	TestEqual(TEXT("One registered component before recompile"), Subsystem->CountRegisteredVolumes(), 1);
	TestEqual(TEXT("One frontier member before recompile"), Subsystem->GetNumFrontierMembers(), 1);

	FVoxelPaintPlacementState PreState;
	if (!TestTrue(TEXT("Cached placement state exists before recompile"),
		Subsystem->GetCachedPlacementState(Component, PreState)))
	{
		return false;
	}
	const FVoxelPaintFrontierIdentity PreIdentity = PreState.Identity;

	Subsystem->SetForcedVariant(PreIdentity, AssetB);
	TestTrue(TEXT("Forced variant resolves before recompile"),
		Subsystem->GetResolvedVariant(Component) == AssetB);

	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	AActor* NewPrefab = nullptr;
	int32 NumInstances = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetClass() == Blueprint->GeneratedClass)
		{
			NewPrefab = *It;
			++NumInstances;
		}
	}
	TestEqual(TEXT("Exactly one prefab instance survives the recompile"), NumInstances, 1);
	UVoxelPaintVolumeComponent* NewComponent =
		NewPrefab ? NewPrefab->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
	if (!TestNotNull(TEXT("Reinstanced prefab carries the paint component"), NewComponent))
	{
		return false;
	}

	TestEqual(TEXT("Exactly one registered component after recompile (no fork)"),
		Subsystem->CountRegisteredVolumes(), 1);
	TestEqual(TEXT("Exactly one frontier member after recompile (no double registration)"),
		Subsystem->GetNumFrontierMembers(), 1);

	FVoxelPaintPlacementState PostState;
	if (!TestTrue(TEXT("Cached placement state exists after recompile"),
		Subsystem->GetCachedPlacementState(NewComponent, PostState)))
	{
		return false;
	}
	TestTrue(TEXT("Frontier identity survives reinstancing (Reserved-slot reclaim)"),
		PostState.Identity == PreIdentity);
	TestTrue(TEXT("Forced variant still resolves after reinstancing"),
		Subsystem->GetResolvedVariant(NewComponent) == AssetB);

	Subsystem->ClearAllForcedVariants();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintVariants_TemplateWritePropagation,
	"Game.WorldVoxelPaint.Unit.Variants.TemplateWritePropagation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintVariants_TemplateWritePropagation::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintPrefabLifecycleTestPrivate;
	using namespace WorldVoxelPaint;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(World);
	if (!TestNotNull(TEXT("Subsystem exists in the test world"), Subsystem))
	{
		return false;
	}

	{
		USCS_Node* Node = nullptr;
		UBlueprint* Blueprint = CreatePaintActorBlueprint(TEXT("VPPropagationBP"),
			AActor::StaticClass(), TEXT("SharedPaint"), Node);
		if (!TestNotNull(TEXT("Propagation Blueprint created"), Blueprint)
			|| !TestNotNull(TEXT("Propagation SCS node added"), Node))
		{
			return false;
		}

		UVoxelPaintVolumeData* AssetA = MakeAsset(EVoxelPaintAssetSharingMode::Shared);
		{
			UVoxelPaintVolumeComponent* Template =
				Cast<UVoxelPaintVolumeComponent>(Node->ComponentTemplate);
			if (!TestNotNull(TEXT("Propagation node carries a template"), Template))
			{
				return false;
			}
			AddPoolEntry(Template, AssetA, 1.0f);
			Template->ExpectedTopology.LocalHalfExtent = AssetA->LocalHalfExtent;
			Template->ExpectedTopology.BrickCounts = AssetA->BrickCounts;
			Template->ExpectedTopology.StorageOrigin = AssetA->StorageOrigin;
			Template->ExpectedTopology.bIsSet = true;
		}
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		UVoxelPaintVolumeComponent* Template =
			Cast<UVoxelPaintVolumeComponent>(Node->ComponentTemplate);
		if (!TestNotNull(TEXT("Template survives compilation"), Template))
		{
			return false;
		}
		UVoxelPaintVolumeData* AssetB = MakeAsset(EVoxelPaintAssetSharingMode::Shared);

		AActor* InstanceOne = SpawnPrefabInstance(World, Blueprint, FVector(100.0, 0.0, 0.0));
		AActor* InstanceTwo = SpawnPrefabInstance(World, Blueprint, FVector(200.0, 0.0, 0.0));
		UVoxelPaintVolumeComponent* ComponentOne =
			InstanceOne ? InstanceOne->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
		UVoxelPaintVolumeComponent* ComponentTwo =
			InstanceTwo ? InstanceTwo->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
		if (!TestNotNull(TEXT("First instance component exists"), ComponentOne)
			|| !TestNotNull(TEXT("Second instance component exists"), ComponentTwo))
		{
			return false;
		}
		TestTrue(TEXT("Instances inherit the template pool"),
			ComponentOne->VolumeVariants.Num() == 1
				&& ComponentOne->VolumeVariants[0].Variant == AssetA);

		const FVector DivergedExtent(777.0, 777.0, 777.0);
		ComponentTwo->VolumeVariants[0].Weight = 9.0f;
		ComponentTwo->ExpectedTopology.LocalHalfExtent = DivergedExtent;
		Subsystem->NotifyPropertyChanged(ComponentTwo);

		AActor* InstanceThree = SpawnPrefabInstance(World, Blueprint, FVector(300.0, 0.0, 0.0));
		UVoxelPaintVolumeComponent* ComponentThree =
			InstanceThree ? InstanceThree->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
		if (!TestNotNull(TEXT("Third instance component exists"), ComponentThree))
		{
			return false;
		}
		const FVector MixedDivergedExtent(555.0, 555.0, 555.0);
		ComponentThree->ExpectedTopology.LocalHalfExtent = MixedDivergedExtent;
		Subsystem->NotifyPropertyChanged(ComponentThree);

		const TArray<FVoxelPaintVariantPoolEntry> PreEditPool = Template->VolumeVariants;
		const FVoxelPaintExpectedTopology PreEditExpectation = Template->ExpectedTopology;
		const FVector NewTemplateExtent(900.0, 800.0, 700.0);
		Template->Modify();
		Template->VolumeVariants[0].Variant = AssetB;
		Template->ExpectedTopology.LocalHalfExtent = NewTemplateExtent;
		PropagateTemplatePoolChange(Template, PreEditPool, PreEditExpectation);

		TestTrue(TEXT("Matched instance carries the template's new pool value"),
			ComponentOne->VolumeVariants[0].Variant == AssetB);
		TestTrue(TEXT("Matched instance carries the template's new expectation"),
			ComponentOne->ExpectedTopology.LocalHalfExtent == NewTemplateExtent);
		FVoxelPaintPlacementState StateOne;
		TestTrue(TEXT("Matched instance state cached"),
			Subsystem->GetCachedPlacementState(ComponentOne, StateOne));
		TestTrue(TEXT("Matched instance received the notify (cached AssetKey re-resolved)"),
			StateOne.AssetKey == FObjectKey(AssetB));

		TestTrue(TEXT("Pre-diverged instance pool is preserved"),
			ComponentTwo->VolumeVariants[0].Variant == AssetA
				&& ComponentTwo->VolumeVariants[0].Weight == 9.0f);
		TestTrue(TEXT("Pre-diverged instance expectation is preserved"),
			ComponentTwo->ExpectedTopology.LocalHalfExtent == DivergedExtent);

		TestTrue(TEXT("Mixed instance's matching pool member is synced per-member"),
			ComponentThree->VolumeVariants[0].Variant == AssetB);
		TestTrue(TEXT("Mixed instance's diverged expectation member is preserved per-member"),
			ComponentThree->ExpectedTopology.LocalHalfExtent == MixedDivergedExtent);
	}

	{
		USCS_Node* ParentNode = nullptr;
		UBlueprint* ParentBlueprint = CreatePaintActorBlueprint(TEXT("VPRoutingParentBP"),
			AActor::StaticClass(), TEXT("RoutedPaint"), ParentNode);
		if (!TestNotNull(TEXT("Routing parent Blueprint created"), ParentBlueprint)
			|| !TestNotNull(TEXT("Routing parent SCS node added"), ParentNode))
		{
			return false;
		}
		UVoxelPaintVolumeData* RoutedAsset = MakeAsset(EVoxelPaintAssetSharingMode::Shared);
		{
			UVoxelPaintVolumeComponent* Template =
				Cast<UVoxelPaintVolumeComponent>(ParentNode->ComponentTemplate);
			if (!TestNotNull(TEXT("Routing parent node carries a template"), Template))
			{
				return false;
			}
			AddPoolEntry(Template, RoutedAsset, 1.0f);
			Template->ExpectedTopology.LocalHalfExtent = RoutedAsset->LocalHalfExtent;
			Template->ExpectedTopology.BrickCounts = RoutedAsset->BrickCounts;
			Template->ExpectedTopology.StorageOrigin = RoutedAsset->StorageOrigin;
			Template->ExpectedTopology.bIsSet = true;
		}
		FKismetEditorUtilities::CompileBlueprint(ParentBlueprint);

		USCS_Node* UnusedChildNode = nullptr;
		UBlueprint* ChildBlueprint = CreatePaintActorBlueprint(TEXT("VPRoutingChildBP"),
			ParentBlueprint->GeneratedClass, nullptr, UnusedChildNode);
		if (!TestNotNull(TEXT("Routing child Blueprint created"), ChildBlueprint))
		{
			return false;
		}
		FKismetEditorUtilities::CompileBlueprint(ChildBlueprint);

		AActor* ParentPlacementOne = SpawnPrefabInstance(World, ParentBlueprint, FVector(300.0, 0.0, 0.0));
		AActor* ParentPlacementTwo = SpawnPrefabInstance(World, ParentBlueprint, FVector(400.0, 0.0, 0.0));
		AActor* ChildPlacement = SpawnPrefabInstance(World, ChildBlueprint, FVector(500.0, 0.0, 0.0));
		UVoxelPaintVolumeComponent* ParentComponentOne = ParentPlacementOne
			? ParentPlacementOne->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
		UVoxelPaintVolumeComponent* ParentComponentTwo = ParentPlacementTwo
			? ParentPlacementTwo->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
		UVoxelPaintVolumeComponent* ChildComponent = ChildPlacement
			? ChildPlacement->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
		if (!TestNotNull(TEXT("Parent placement one component exists"), ParentComponentOne)
			|| !TestNotNull(TEXT("Parent placement two component exists"), ParentComponentTwo)
			|| !TestNotNull(TEXT("Child placement component exists"), ChildComponent))
		{
			return false;
		}

		TestTrue(TEXT("Introducing class's placement classifies SCSOwnClass"),
			ClassifyPoolProvenance(ParentComponentOne, ParentPlacementOne->GetClass())
				== EVoxelPaintPoolProvenance::SCSOwnClass);
		TestTrue(TEXT("Child placement without an override classifies SCSAncestor"),
			ClassifyPoolProvenance(ChildComponent, ChildPlacement->GetClass())
				== EVoxelPaintPoolProvenance::SCSAncestor);

		UVoxelPaintVolumeComponent* SupplyingOne = ResolveEffectiveSupplyingTemplate(ParentComponentOne);
		UVoxelPaintVolumeComponent* SupplyingTwo = ResolveEffectiveSupplyingTemplate(ParentComponentTwo);
		TestNotNull(TEXT("Parent placements resolve a supplying template"), SupplyingOne);
		TestTrue(TEXT("Same-template placements group into one domain"),
			SupplyingOne == SupplyingTwo);
		TestTrue(TEXT("The parent domain's supplying template is the introducing node template"),
			SupplyingOne == Cast<UVoxelPaintVolumeComponent>(ParentNode->ComponentTemplate));

		{
			AVoxelPaintVolume* NativeOne = SpawnTestVolume(World, FVector(600.0, 0.0, 0.0));
			AVoxelPaintVolume* NativeTwo = SpawnTestVolume(World, FVector(700.0, 0.0, 0.0));
			TestNull(TEXT("A native AVoxelPaintVolume placement resolves no supplying template"),
				ResolveEffectiveSupplyingTemplate(NativeOne->GetVolumeComponent()));
			TestNull(TEXT("A second native placement resolves no supplying template either"),
				ResolveEffectiveSupplyingTemplate(NativeTwo->GetVolumeComponent()));
		}

		{
			const FVector ExtentBefore = RoutedAsset->LocalHalfExtent;
			FText AncestorError;
			UVoxelPaintVolumeComponent* AncestorTarget =
				ResolveExpectationWriteTarget(ChildComponent, AncestorError);
			TestNull(TEXT("SCSAncestor placement refuses the expectation write target"), AncestorTarget);
			TestFalse(TEXT("SCSAncestor refusal carries an error"), AncestorError.IsEmpty());
			TestTrue(TEXT("SCSAncestor refusal fires with no asset mutation"),
				RoutedAsset->LocalHalfExtent == ExtentBefore);
		}

		UBlueprintGeneratedClass* ChildClass =
			Cast<UBlueprintGeneratedClass>(ChildBlueprint->GeneratedClass);
		if (!TestNotNull(TEXT("Child generated class exists"), ChildClass))
		{
			return false;
		}
		UInheritableComponentHandler* Handler = ChildClass->GetInheritableComponentHandler(true);
		if (!TestNotNull(TEXT("Child inheritable component handler created"), Handler))
		{
			return false;
		}
		UVoxelPaintVolumeComponent* OverrideTemplate = Cast<UVoxelPaintVolumeComponent>(
			Handler->CreateOverridenComponentTemplate(FComponentKey(ParentNode)));
		if (!TestNotNull(TEXT("Class-local override template created on the child"), OverrideTemplate))
		{
			return false;
		}

		TestTrue(TEXT("Override-holding child placement classifies SCSOverrideOwner"),
			ClassifyPoolProvenance(ChildComponent, ChildPlacement->GetClass())
				== EVoxelPaintPoolProvenance::SCSOverrideOwner);
		UVoxelPaintVolumeComponent* ChildSupplying = ResolveEffectiveSupplyingTemplate(ChildComponent);
		TestTrue(TEXT("Override-holding child resolves its override as the supplying template"),
			ChildSupplying == OverrideTemplate);
		TestTrue(TEXT("Override-holding child is excluded from the parent's domain"),
			ChildSupplying != SupplyingOne);

		FText OverrideError;
		TestTrue(TEXT("SCSOverrideOwner routes the expectation write to the existing override"),
			ResolveExpectationWriteTarget(ChildComponent, OverrideError) == OverrideTemplate);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintVariants_AutoSizeCentersOnMeshBounds,
	"Game.WorldVoxelPaint.Unit.Variants.AutoSizeCentersOnMeshBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintVariants_AutoSizeCentersOnMeshBounds::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintPrefabLifecycleTestPrivate;
	using namespace WorldVoxelPaint;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube mesh loaded"), CubeMesh))
	{
		return false;
	}

	// The reported repro: four 100 cm cubes whose union has an off-origin center. The paint
	// volume is centered on the component transform, so a capture that keeps the extent but
	// drops the center leaves the volume displaced by exactly ExpectedCenter.
	const FVector CubePositions[] = {
		FVector(0.0, 0.0, 0.0),
		FVector(130.0, 0.0, 0.0),
		FVector(60.0, 80.0, 0.0),
		FVector(200.0, 0.0, 50.0),
	};
	const FVector ExpectedCenter(100.0, 40.0, 25.0);
	const FVector ExpectedExtent(150.0, 90.0, 75.0);
	const FVector ExpectedCoverExtent(250.0, 130.0, 100.0);

	USCS_Node* UnusedNode = nullptr;
	UBlueprint* Blueprint = CreatePaintActorBlueprint(TEXT("VPAutoSizeBP"), AActor::StaticClass(),
		nullptr, UnusedNode);
	if (!TestNotNull(TEXT("Auto-size Blueprint created"), Blueprint)
		|| !TestNotNull(TEXT("Auto-size Blueprint has an SCS"), Blueprint->SimpleConstructionScript.Get()))
	{
		return false;
	}
	USimpleConstructionScript* Scs = Blueprint->SimpleConstructionScript;
	USCS_Node* RootNode = Scs->CreateNode(USceneComponent::StaticClass(), TEXT("PrefabRoot"));
	Scs->AddNode(RootNode);
	for (int32 Index = 0; Index < (int32)UE_ARRAY_COUNT(CubePositions); ++Index)
	{
		USCS_Node* MeshNode = Scs->CreateNode(UStaticMeshComponent::StaticClass(),
			*FString::Printf(TEXT("Cube%d"), Index));
		UStaticMeshComponent* MeshTemplate = Cast<UStaticMeshComponent>(MeshNode->ComponentTemplate);
		if (!TestNotNull(TEXT("Cube node carries a mesh template"), MeshTemplate))
		{
			return false;
		}
		MeshTemplate->SetStaticMesh(CubeMesh);
		MeshTemplate->SetRelativeLocation_Direct(CubePositions[Index]);
		RootNode->AddChildNode(MeshNode);
	}
	USCS_Node* PaintNode = Scs->CreateNode(UVoxelPaintVolumeComponent::StaticClass(), TEXT("PaintPool"));
	RootNode->AddChildNode(PaintNode);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	{
		AActor* Prefab = SpawnPrefabInstance(World, Blueprint, FVector::ZeroVector);
		UVoxelPaintVolumeComponent* Component =
			Prefab ? Prefab->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
		if (!TestNotNull(TEXT("Prefab instance carries the paint component"), Component))
		{
			return false;
		}
		const FVoxelPaintAutoSizeCapture Capture = ComputeAutoSizeCapture(Component);
		TestTrue(TEXT("Registered cube meshes produce valid bounds"), Capture.bBoundsValid);
		TestTrue(TEXT("Capture requests the centering move"), Capture.bMoveWriteTarget);
		TestTrue(TEXT("Half extent matches the cube union"),
			Capture.HalfExtent.Equals(ExpectedExtent, 0.5));
		TestTrue(TEXT("Centering move lands on the cube-union center (previously discarded)"),
			Capture.NewRelativeLocation.Equals(ExpectedCenter, 0.5));
	}

	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Prefab = World->SpawnActor<AActor>(Blueprint->GeneratedClass,
			FVector(1000.0, -200.0, 30.0), FRotator(0.0, 90.0, 0.0), SpawnParameters);
		UVoxelPaintVolumeComponent* Component =
			Prefab ? Prefab->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
		if (!TestNotNull(TEXT("Moved and rotated prefab instance carries the paint component"),
			Component))
		{
			return false;
		}
		const FVoxelPaintAutoSizeCapture Capture = ComputeAutoSizeCapture(Component);
		TestTrue(TEXT("Half extent is invariant under rigid placement"),
			Capture.HalfExtent.Equals(ExpectedExtent, 0.5));
		TestTrue(TEXT("Centering move is parent-relative, invariant under rigid placement"),
			Capture.NewRelativeLocation.Equals(ExpectedCenter, 0.5));
	}

	{
		AActor* Prefab = SpawnPrefabInstance(World, Blueprint, FVector::ZeroVector);
		UVoxelPaintVolumeComponent* Component =
			Prefab ? Prefab->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
		if (!TestNotNull(TEXT("Guard prefab instance carries the paint component"), Component))
		{
			return false;
		}
		USceneComponent* Child = NewObject<USceneComponent>(Prefab, TEXT("PaintChild"));
		Child->SetupAttachment(Component);
		Child->RegisterComponentWithWorld(World);
		const FVoxelPaintAutoSizeCapture Capture = ComputeAutoSizeCapture(Component);
		TestTrue(TEXT("Bounds stay valid with an attached child"), Capture.bBoundsValid);
		TestFalse(TEXT("A component with attached children refuses the move"),
			Capture.bMoveWriteTarget);
		TestTrue(TEXT("Extent instead expands to cover the cubes from the unmoved position"),
			Capture.HalfExtent.Equals(ExpectedCoverExtent, 0.5));
	}

	{
		UVoxelPaintVolumeComponent* Template =
			Cast<UVoxelPaintVolumeComponent>(PaintNode->ComponentTemplate);
		if (!TestNotNull(TEXT("Paint node carries a template"), Template))
		{
			return false;
		}
		AActor* InstanceOne = SpawnPrefabInstance(World, Blueprint, FVector(100.0, 0.0, 0.0));
		AActor* InstanceTwo = SpawnPrefabInstance(World, Blueprint, FVector(200.0, 0.0, 0.0));
		UVoxelPaintVolumeComponent* ComponentOne =
			InstanceOne ? InstanceOne->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
		UVoxelPaintVolumeComponent* ComponentTwo =
			InstanceTwo ? InstanceTwo->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
		if (!TestNotNull(TEXT("First propagation instance component exists"), ComponentOne)
			|| !TestNotNull(TEXT("Second propagation instance component exists"), ComponentTwo))
		{
			return false;
		}
		const FVector DivergedLocation(7.0, 7.0, 7.0);
		ComponentTwo->SetRelativeLocation(DivergedLocation);

		ApplyAutoCenterMove(Template, ExpectedCenter);

		TestTrue(TEXT("Template default carries the centering move"),
			Template->GetRelativeLocation().Equals(ExpectedCenter, 0.01));
		TestTrue(TEXT("Matching instance follows the template's centering move"),
			ComponentOne->GetRelativeLocation().Equals(ExpectedCenter, 0.01));
		TestTrue(TEXT("Moved instance's volume centers on the cubes in world space"),
			ComponentOne->GetComponentLocation().Equals(FVector(100.0, 0.0, 0.0) + ExpectedCenter, 0.01));
		TestTrue(TEXT("Diverged instance keeps its own placement"),
			ComponentTwo->GetRelativeLocation().Equals(DivergedLocation, 0.01));
	}

	return true;
}
