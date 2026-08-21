// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/DataValidation.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "VoxelPaintAssetValidator.h"
#include "VoxelPaintComponentValidator.h"
#include "VoxelPaintTypes.h"
#include "VoxelPaintVolume.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"

namespace VoxelPaintComponentValidatorTestPrivate
{

struct FVoxelPaintTestWorld
{
	UWorld* World = nullptr;

	FVoxelPaintTestWorld()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelPaintComponentValidatorTest"));
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

UVoxelPaintVolumeData* MakeAsset(const FVector& HalfExtent, const FIntVector& BrickCounts,
	const FVector& StorageOrigin, EVoxelPaintAssetSharingMode SharingMode)
{
	UVoxelPaintVolumeData* Asset = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	Asset->LocalHalfExtent = HalfExtent;
	Asset->BrickCounts = BrickCounts;
	Asset->StorageOrigin = StorageOrigin;
	Asset->SharingMode = SharingMode;
	return Asset;
}

void AddInputEntry(WorldVoxelPaint::FVoxelPaintComponentRuleInputs& Inputs,
	const UVoxelPaintVolumeData* Asset, float Weight,
	EVoxelPaintAssetSharingMode SharingMode = EVoxelPaintAssetSharingMode::Shared,
	bool bOwnedByThisPlacement = false, const TCHAR* OwnerDisplayName = TEXT(""))
{
	Inputs.Pool.Assets.Add(Asset);
	Inputs.Pool.Weights.Add(Weight);
	WorldVoxelPaint::FVoxelPaintComponentRuleEntry& Entry = Inputs.Entries.AddDefaulted_GetRef();
	if (Asset)
	{
		Entry.bHasAsset = true;
		Entry.AssetName = Asset->GetName();
		Entry.Topology.LocalHalfExtent = Asset->LocalHalfExtent;
		Entry.Topology.BrickCounts = Asset->BrickCounts;
		Entry.Topology.StorageOrigin = Asset->StorageOrigin;
		Entry.SharingMode = SharingMode;
		Entry.bOwnedByThisPlacement = bOwnedByThisPlacement;
		Entry.OwnerDisplayName = OwnerDisplayName;
	}
}

int32 CountRule(const TArray<WorldVoxelPaint::FVoxelPaintComponentFinding>& Findings,
	WorldVoxelPaint::EVoxelPaintComponentRule Rule)
{
	int32 Count = 0;
	for (const WorldVoxelPaint::FVoxelPaintComponentFinding& Finding : Findings)
	{
		if (Finding.Rule == Rule)
		{
			++Count;
		}
	}
	return Count;
}

const WorldVoxelPaint::FVoxelPaintComponentFinding* FindRule(
	const TArray<WorldVoxelPaint::FVoxelPaintComponentFinding>& Findings,
	WorldVoxelPaint::EVoxelPaintComponentRule Rule)
{
	for (const WorldVoxelPaint::FVoxelPaintComponentFinding& Finding : Findings)
	{
		if (Finding.Rule == Rule)
		{
			return &Finding;
		}
	}
	return nullptr;
}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintValidator_ComponentRules,
	"Game.WorldVoxelPaint.Unit.Validator.ComponentRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintValidator_ComponentRules::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintComponentValidatorTestPrivate;
	using namespace WorldVoxelPaint;

	FVoxelPaintTestWorld TestWorld;
	UWorld* World = TestWorld.World;

	const FVector ExtentA(600.0, 500.0, 400.0);
	const FIntVector BricksA(5, 4, 3);
	const FVector OriginA(10.0, 0.0, 0.0);
	UVoxelPaintVolumeData* SharedA =
		MakeAsset(ExtentA, BricksA, OriginA, EVoxelPaintAssetSharingMode::Shared);
	UVoxelPaintVolumeData* SharedB =
		MakeAsset(FVector(300.0), FIntVector(4, 4, 4), FVector::ZeroVector,
			EVoxelPaintAssetSharingMode::Shared);

	TArray<FVoxelPaintComponentFinding> Findings;

	{
		FVoxelPaintComponentRuleInputs Inputs;
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A pristine empty component produces no findings"), Findings.Num(), 0);

		FVoxelPaintComponentRuleInputs EntryNoExpectation;
		AddInputEntry(EntryNoExpectation, SharedA, 1.0f);
		ValidateComponentPoolState(EntryNoExpectation, Findings);
		TestEqual(TEXT("An unset expectation skips the topology-mismatch rule entirely"),
			Findings.Num(), 0);
	}

	{
		FVoxelPaintComponentRuleInputs Inputs;
		AddInputEntry(Inputs, SharedA, 1.0f);
		Inputs.Expectation.LocalHalfExtent = FVector(999.0);
		Inputs.Expectation.BrickCounts = BricksA;
		Inputs.Expectation.StorageOrigin = OriginA;
		Inputs.Expectation.bIsSet = true;
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A mismatching SOLE entry fires the topology-mismatch error"),
			CountRule(Findings, EVoxelPaintComponentRule::TopologyMismatch), 1);
		const FVoxelPaintComponentFinding* Mismatch =
			FindRule(Findings, EVoxelPaintComponentRule::TopologyMismatch);
		if (Mismatch)
		{
			TestTrue(TEXT("Topology mismatch is an error"),
				Mismatch->Severity == EVoxelPaintComponentFindingSeverity::Error);
			TestTrue(TEXT("Topology mismatch names the entry's asset"),
				Mismatch->Message.ToString().Contains(SharedA->GetName()));
		}

		Inputs.Expectation.LocalHalfExtent = ExtentA;
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A matching entry fires no topology mismatch"),
			CountRule(Findings, EVoxelPaintComponentRule::TopologyMismatch), 0);
	}

	{
		FVoxelPaintComponentRuleInputs Inputs;
		AddInputEntry(Inputs, SharedA, 1.0f);
		AddInputEntry(Inputs, SharedA, 1.0f);
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A duplicate entry fires exactly one duplicate error"),
			CountRule(Findings, EVoxelPaintComponentRule::DuplicateEntry), 1);
		const FVoxelPaintComponentFinding* Duplicate =
			FindRule(Findings, EVoxelPaintComponentRule::DuplicateEntry);
		if (Duplicate)
		{
			TestTrue(TEXT("Duplicate entries are an error"),
				Duplicate->Severity == EVoxelPaintComponentFindingSeverity::Error);
		}
	}

	{
		FVoxelPaintComponentRuleInputs Inputs;
		AddInputEntry(Inputs, SharedA, 1.0f);
		AddInputEntry(Inputs, nullptr, 1.0f);
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A null entry fires exactly one null warning"),
			CountRule(Findings, EVoxelPaintComponentRule::NullEntry), 1);
		const FVoxelPaintComponentFinding* Null = FindRule(Findings, EVoxelPaintComponentRule::NullEntry);
		if (Null)
		{
			TestTrue(TEXT("Null entries are a warning (legitimate mid-authoring state)"),
				Null->Severity == EVoxelPaintComponentFindingSeverity::Warning);
		}
	}

	{
		FVoxelPaintComponentRuleInputs Inputs;
		AddInputEntry(Inputs, SharedA, 1.0f);
		Inputs.bPinSet = true;
		Inputs.bPinInPool = false;
		Inputs.PinAssetName = SharedB->GetName();
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A dangling pin fires exactly one warning"),
			CountRule(Findings, EVoxelPaintComponentRule::DanglingPin), 1);

		Inputs.bPinInPool = true;
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("An in-pool pin fires no dangling-pin warning"),
			CountRule(Findings, EVoxelPaintComponentRule::DanglingPin), 0);
	}

	{
		FVoxelPaintComponentRuleInputs Inputs;
		Inputs.CreationMethod = EComponentCreationMethod::SimpleConstructionScript;
		AddInputEntry(Inputs, SharedA, 1.0f, EVoxelPaintAssetSharingMode::Private);
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A Private entry in an SCS-supplied pool fires the error"),
			CountRule(Findings, EVoxelPaintComponentRule::PrivateEntryInScsPool), 1);
		const FVoxelPaintComponentFinding* PrivateInScs =
			FindRule(Findings, EVoxelPaintComponentRule::PrivateEntryInScsPool);
		if (PrivateInScs)
		{
			TestTrue(TEXT("Private-in-SCS is an error"),
				PrivateInScs->Severity == EVoxelPaintComponentFindingSeverity::Error);
		}

		FVoxelPaintComponentRuleInputs TemplateInputs;
		TemplateInputs.bIsTemplate = true;
		AddInputEntry(TemplateInputs, SharedA, 1.0f, EVoxelPaintAssetSharingMode::Private);
		ValidateComponentPoolState(TemplateInputs, Findings);
		TestEqual(TEXT("A Private entry on a template (the SCS supply) fires the error too"),
			CountRule(Findings, EVoxelPaintComponentRule::PrivateEntryInScsPool), 1);
	}

	{
		FVoxelPaintComponentRuleInputs Inputs;
		AddInputEntry(Inputs, SharedA, 1.0f, EVoxelPaintAssetSharingMode::Private,
			/*bOwnedByThisPlacement=*/false, TEXT("Rock_02 in TestMap"));
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A foreign-owned Private entry in a placement-owned pool fires the warning"),
			CountRule(Findings, EVoxelPaintComponentRule::ForeignPrivateEntry), 1);
		const FVoxelPaintComponentFinding* Foreign =
			FindRule(Findings, EVoxelPaintComponentRule::ForeignPrivateEntry);
		if (Foreign)
		{
			TestTrue(TEXT("Foreign-Private is a warning"),
				Foreign->Severity == EVoxelPaintComponentFindingSeverity::Warning);
			TestTrue(TEXT("Foreign-Private names the owner"),
				Foreign->Message.ToString().Contains(TEXT("Rock_02 in TestMap")));
		}

		FVoxelPaintComponentRuleInputs OwnedInputs;
		AddInputEntry(OwnedInputs, SharedA, 1.0f, EVoxelPaintAssetSharingMode::Private,
			/*bOwnedByThisPlacement=*/true);
		ValidateComponentPoolState(OwnedInputs, Findings);
		TestEqual(TEXT("A self-owned Private entry fires no foreign warning"),
			CountRule(Findings, EVoxelPaintComponentRule::ForeignPrivateEntry), 0);
	}

	{
		FVoxelPaintComponentRuleInputs Inputs;
		Inputs.CreationMethod = EComponentCreationMethod::UserConstructionScript;
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A UCS-created component fires the unsupported warning"),
			CountRule(Findings, EVoxelPaintComponentRule::UcsProvenance), 1);
	}

	{
		FVoxelPaintComponentRuleInputs Inputs;
		Inputs.CreationMethod = EComponentCreationMethod::Native;
		Inputs.bOwnerClassIsBlueprintGeneratedClass = true;
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A native component on a Blueprint-classed owner fires the carrier warning"),
			CountRule(Findings, EVoxelPaintComponentRule::NativeBlueprintCarrier), 1);
	}

	{
		FVoxelPaintComponentRuleInputs Inputs;
		Inputs.MaxScaleDeviation = 0.5f;
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A non-unit effective scale fires the note"),
			CountRule(Findings, EVoxelPaintComponentRule::NonUnitEffectiveScale), 1);
		const FVoxelPaintComponentFinding* Scale =
			FindRule(Findings, EVoxelPaintComponentRule::NonUnitEffectiveScale);
		if (Scale)
		{
			TestTrue(TEXT("Scale finding is a note"),
				Scale->Severity == EVoxelPaintComponentFindingSeverity::Note);
		}

		Inputs.MaxScaleDeviation = 0.0005f;
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A within-tolerance scale fires no note"),
			CountRule(Findings, EVoxelPaintComponentRule::NonUnitEffectiveScale), 0);
	}

	{
		FVoxelPaintComponentRuleInputs Inputs;
		AddInputEntry(Inputs, SharedA, 0.0f);
		AddInputEntry(Inputs, SharedB, 0.0f);
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("All-zero selection weights fire the degenerate-pool warning"),
			CountRule(Findings, EVoxelPaintComponentRule::AllZeroWeights), 1);

		FVoxelPaintComponentRuleInputs MixedInputs;
		AddInputEntry(MixedInputs, SharedA, 0.0f);
		AddInputEntry(MixedInputs, SharedB, 1.0f);
		ValidateComponentPoolState(MixedInputs, Findings);
		TestEqual(TEXT("A positive weight suppresses the degenerate-pool warning"),
			CountRule(Findings, EVoxelPaintComponentRule::AllZeroWeights), 0);
	}

	{
		const FName BlueprintName = MakeUniqueObjectName(GetTransientPackage(),
			UBlueprint::StaticClass(), TEXT("VPValidatorDivergenceBP"));
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
			GetTransientPackage(), BlueprintName, BPTYPE_Normal, NAME_None);
		if (!TestNotNull(TEXT("Divergence Blueprint created"), Blueprint))
		{
			return false;
		}
		USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(
			UVoxelPaintVolumeComponent::StaticClass(), TEXT("DivergencePaint"));
		Blueprint->SimpleConstructionScript->AddNode(Node);
		UVoxelPaintVolumeComponent* Template = Cast<UVoxelPaintVolumeComponent>(Node->ComponentTemplate);
		if (!TestNotNull(TEXT("Divergence node carries a template"), Template))
		{
			return false;
		}
		FVoxelPaintVariantPoolEntry& TemplateEntry = Template->VolumeVariants.AddDefaulted_GetRef();
		TemplateEntry.Variant = SharedA;
		TemplateEntry.Weight = 1.0f;
		Template->ExpectedTopology.LocalHalfExtent = ExtentA;
		Template->ExpectedTopology.BrickCounts = BricksA;
		Template->ExpectedTopology.StorageOrigin = OriginA;
		Template->ExpectedTopology.bIsSet = true;
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Instance = World->SpawnActor<AActor>(Blueprint->GeneratedClass,
			FVector(50.0, 0.0, 0.0), FRotator::ZeroRotator, SpawnParameters);
		UVoxelPaintVolumeComponent* InstanceComponent =
			Instance ? Instance->FindComponentByClass<UVoxelPaintVolumeComponent>() : nullptr;
		if (!TestNotNull(TEXT("Divergence instance component exists"), InstanceComponent))
		{
			return false;
		}

		FVoxelPaintComponentRuleInputs Inputs;
		BuildComponentRuleInputs(*InstanceComponent, Inputs);
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A conforming SCS instance fires no divergence error"),
			CountRule(Findings, EVoxelPaintComponentRule::ScsPoolDivergence), 0);

		InstanceComponent->VolumeVariants[0].Weight = 7.0f;
		BuildComponentRuleInputs(*InstanceComponent, Inputs);
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("A diverged SCS instance fires the divergence error"),
			CountRule(Findings, EVoxelPaintComponentRule::ScsPoolDivergence), 1);
		const FVoxelPaintComponentFinding* Divergence =
			FindRule(Findings, EVoxelPaintComponentRule::ScsPoolDivergence);
		if (Divergence)
		{
			TestTrue(TEXT("Divergence is an error"),
				Divergence->Severity == EVoxelPaintComponentFindingSeverity::Error);
			TestTrue(TEXT("Divergence names the ResetPoolToArchetype fix-up"),
				Divergence->Message.ToString().Contains(TEXT("ResetPoolToArchetype")));
		}

		InstanceComponent->ResetPoolToArchetype();
		InstanceComponent = Instance->FindComponentByClass<UVoxelPaintVolumeComponent>();
		if (!TestNotNull(TEXT("Instance component survives the reset's construction-script rerun"),
			InstanceComponent))
		{
			return false;
		}
		BuildComponentRuleInputs(*InstanceComponent, Inputs);
		ValidateComponentPoolState(Inputs, Findings);
		TestEqual(TEXT("ResetPoolToArchetype clears the divergence error"),
			CountRule(Findings, EVoxelPaintComponentRule::ScsPoolDivergence), 0);
	}

	{
		UVoxelPaintAssetValidator::SetEnumerableWorldOverrideForTest(
			[World](const UWorld* CandidateWorld) { return CandidateWorld == World; });
		ON_SCOPE_EXIT
		{
			UVoxelPaintAssetValidator::SetEnumerableWorldOverrideForTest(
				UVoxelPaintAssetValidator::FEnumerableWorldOverride());
		};

		UVoxelPaintVolumeData* GatherAsset = MakeAsset(FVector(500.0), FIntVector(4, 4, 4),
			FVector::ZeroVector, EVoxelPaintAssetSharingMode::Private);

		AVoxelPaintVolume* PoolHolder = SpawnTestVolume(World, FVector(100.0, 0.0, 0.0));
		PoolHolder->GetVolumeComponent()->VolumeVariants.AddDefaulted_GetRef().Variant = GatherAsset;

		AVoxelPaintVolume* PinOnlyHolder = SpawnTestVolume(World, FVector(200.0, 0.0, 0.0));
		PinOnlyHolder->GetVolumeComponent()->VariantOverride = GatherAsset;

		TArray<FVoxelPaintReferenceRecord> Records;
		GatherReferenceRecords(GatherAsset, Records);
		int32 NumLoadedRecords = 0;
		bool bPoolHolderCounted = false;
		bool bPinOnlyCounted = false;
		for (const FVoxelPaintReferenceRecord& Record : Records)
		{
			if (!Record.bLoaded)
			{
				continue;
			}
			++NumLoadedRecords;
			bPoolHolderCounted |= Record.ActorGuid == PoolHolder->GetActorGuid();
			bPinOnlyCounted |= Record.ActorGuid == PinOnlyHolder->GetActorGuid();
		}
		TestEqual(TEXT("Gather counts exactly the pool-slot holder"), NumLoadedRecords, 1);
		TestTrue(TEXT("The pool-slot holder is the counted reference"), bPoolHolderCounted);
		TestFalse(TEXT("A pin-only reference is NOT counted"), bPinOnlyCounted);

		int32 NumLogicalPlacements = 0;
		TestTrue(TEXT("One pool holder classifies Legitimate for a Private asset"),
			ClassifyReferences(EVoxelPaintAssetSharingMode::Private, Records, NumLogicalPlacements)
				== EVoxelPaintMultiRefVerdict::Legitimate);
		TestEqual(TEXT("One logical placement counted"), NumLogicalPlacements, 1);

		AVoxelPaintVolume* SecondPoolHolder = SpawnTestVolume(World, FVector(300.0, 0.0, 0.0));
		SecondPoolHolder->GetVolumeComponent()->VolumeVariants.AddDefaulted_GetRef().Variant = GatherAsset;
		Records.Reset();
		GatherReferenceRecords(GatherAsset, Records);
		TestTrue(TEXT("Two pool holders classify PrivateMultiReference for a Private asset"),
			ClassifyReferences(EVoxelPaintAssetSharingMode::Private, Records, NumLogicalPlacements)
				== EVoxelPaintMultiRefVerdict::PrivateMultiReference);
		TestEqual(TEXT("Two logical placements counted"), NumLogicalPlacements, 2);
		TestTrue(TEXT("Shared stays legitimate at any count"),
			ClassifyReferences(EVoxelPaintAssetSharingMode::Shared, Records, NumLogicalPlacements)
				== EVoxelPaintMultiRefVerdict::Legitimate);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintValidator_ComponentEligibility,
	"Game.WorldVoxelPaint.Unit.Validator.ComponentEligibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintValidator_ComponentEligibility::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintComponentValidatorTestPrivate;

	UVoxelPaintComponentValidator* Validator =
		NewObject<UVoxelPaintComponentValidator>(GetTransientPackage());
	// Blueprint compilation below can trigger garbage collection and nothing else references the
	// validator, so keep it rooted for the duration of the test.
	Validator->AddToRoot();
	ON_SCOPE_EXIT
	{
		Validator->RemoveFromRoot();
	};

	// The public ValidateLoadedAsset is the engine dispatcher: it consults CanValidateAsset first
	// and returns NotValidated without tripping the engine's ensure when the validator declines.
	const auto Validate = [Validator](UObject* Asset)
	{
		FDataValidationContext Context;
		return Validator->ValidateLoadedAsset(FAssetData(Asset), Asset, Context);
	};

	{
		const FName EmptyName = MakeUniqueObjectName(GetTransientPackage(),
			UBlueprint::StaticClass(), TEXT("VPEligibilityEmptyBP"));
		UBlueprint* EmptyBlueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
			GetTransientPackage(), EmptyName, BPTYPE_Normal, NAME_None);
		if (!TestNotNull(TEXT("Empty Blueprint created"), EmptyBlueprint))
		{
			return false;
		}
		TestTrue(TEXT("A Blueprint with no paint component is declined"),
			Validate(EmptyBlueprint) == EDataValidationResult::NotValidated);
	}

	const FName ParentName = MakeUniqueObjectName(GetTransientPackage(),
		UBlueprint::StaticClass(), TEXT("VPEligibilityParentBP"));
	UBlueprint* ParentBlueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),
		GetTransientPackage(), ParentName, BPTYPE_Normal, NAME_None);
	if (!TestNotNull(TEXT("Parent Blueprint created"), ParentBlueprint))
	{
		return false;
	}
	USCS_Node* ParentNode = ParentBlueprint->SimpleConstructionScript->CreateNode(
		UVoxelPaintVolumeComponent::StaticClass(), TEXT("EligibilityPaint"));
	ParentBlueprint->SimpleConstructionScript->AddNode(ParentNode);
	FKismetEditorUtilities::CompileBlueprint(ParentBlueprint);
	TestTrue(TEXT("A Blueprint whose SCS paint template has an empty pool is still a validation target"),
		Validate(ParentBlueprint) == EDataValidationResult::Valid);

	{
		const FName ChildName = MakeUniqueObjectName(GetTransientPackage(),
			UBlueprint::StaticClass(), TEXT("VPEligibilityChildBP"));
		UBlueprint* ChildBlueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentBlueprint->GeneratedClass, GetTransientPackage(), ChildName, BPTYPE_Normal,
			NAME_None);
		if (!TestNotNull(TEXT("Child Blueprint created"), ChildBlueprint))
		{
			return false;
		}
		TestTrue(TEXT("An inherited-only child Blueprint is declined; the parent owns the template"),
			Validate(ChildBlueprint) == EDataValidationResult::NotValidated);

		UBlueprintGeneratedClass* ChildClass =
			Cast<UBlueprintGeneratedClass>(ChildBlueprint->GeneratedClass);
		if (!TestNotNull(TEXT("Child generated class exists"), ChildClass))
		{
			return false;
		}
		UActorComponent* OverrideTemplate = ChildClass->GetInheritableComponentHandler(true)
			->CreateOverridenComponentTemplate(FComponentKey(ParentNode));
		if (!TestNotNull(TEXT("Child override template created"), OverrideTemplate))
		{
			return false;
		}
		TestTrue(TEXT("A child with a class-local inherited-component override is validated"),
			Validate(ChildBlueprint) == EDataValidationResult::Valid);
	}

	{
		FVoxelPaintTestWorld TestWorld;
		UWorld* World = TestWorld.World;

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);

		World->WorldType = EWorldType::Inactive;
		TestTrue(TEXT("An enumerable world with no paint components is declined"),
			Validate(World) == EDataValidationResult::NotValidated);
		World->WorldType = EWorldType::Game;

		AVoxelPaintVolume* Volume = SpawnTestVolume(World, FVector(100.0, 0.0, 0.0));
		if (!TestNotNull(TEXT("Test volume spawned"), Volume))
		{
			return false;
		}
		TestTrue(TEXT("A non-enumerable world is declined even when it holds a paint component"),
			Validate(World) == EDataValidationResult::NotValidated);
		TestTrue(TEXT("An actor in a non-enumerable world is declined"),
			Validate(Volume) == EDataValidationResult::NotValidated);

		World->WorldType = EWorldType::Inactive;
		TestTrue(TEXT("An enumerable world holding a paint component validates"),
			Validate(World) == EDataValidationResult::Valid);
		TestTrue(TEXT("An actor holding a paint component in an enumerable world validates"),
			Validate(Volume) == EDataValidationResult::Valid);
		World->WorldType = EWorldType::Game;
	}

	{
		AVoxelPaintVolume* DetachedVolume = NewObject<AVoxelPaintVolume>(GetTransientPackage());
		if (!TestNotNull(TEXT("Detached volume created"), DetachedVolume)
			|| !TestNotNull(TEXT("Detached volume has a component"),
				DetachedVolume->GetVolumeComponent()))
		{
			return false;
		}
		TestTrue(TEXT("A null-world actor with a paint component validates (pre-level OFPA case)"),
			Validate(DetachedVolume) == EDataValidationResult::Valid);

		AActor* DetachedEmpty = NewObject<AActor>(GetTransientPackage());
		TestTrue(TEXT("A null-world actor with no paint component is declined"),
			Validate(DetachedEmpty) == EDataValidationResult::NotValidated);

		UVoxelPaintVolumeData* PinAsset = MakeAsset(FVector(500.0), FIntVector(4, 4, 4),
			FVector::ZeroVector, EVoxelPaintAssetSharingMode::Shared);
		DetachedVolume->GetVolumeComponent()->VariantOverride = PinAsset;
		{
			FDataValidationContext Context;
			TestTrue(TEXT("An empty-pool component with a dangling pin warns but stays Valid"),
				Validator->ValidateLoadedAsset(FAssetData(DetachedVolume), DetachedVolume, Context)
					== EDataValidationResult::Valid);
			bool bFoundPinWarning = false;
			for (const FDataValidationContext::FIssue& Issue : Context.GetIssues())
			{
				bFoundPinWarning |= Issue.Message.ToString().Contains(TEXT("Pin dangling"));
			}
			TestTrue(TEXT("The dangling-pin warning reaches the validation context"), bFoundPinWarning);
		}
		DetachedVolume->GetVolumeComponent()->VariantOverride = nullptr;

		UVoxelPaintVolumeData* DupAsset = MakeAsset(FVector(300.0), FIntVector(3, 3, 3),
			FVector::ZeroVector, EVoxelPaintAssetSharingMode::Shared);
		DetachedVolume->GetVolumeComponent()->VolumeVariants.AddDefaulted_GetRef().Variant = DupAsset;
		DetachedVolume->GetVolumeComponent()->VolumeVariants.AddDefaulted_GetRef().Variant = DupAsset;
		TestTrue(TEXT("A duplicate-entry error still reports Invalid through the dispatcher"),
			Validate(DetachedVolume) == EDataValidationResult::Invalid);
	}

	return true;
}
