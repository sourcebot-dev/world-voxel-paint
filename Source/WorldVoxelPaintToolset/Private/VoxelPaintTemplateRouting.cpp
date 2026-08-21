// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintTemplateRouting.h"

#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintVolumeData.h"

#define LOCTEXT_NAMESPACE "WorldVoxelPaint"

namespace WorldVoxelPaint
{

namespace TemplateRoutingPrivate
{

struct FIntroducingNode
{
	USCS_Node* Node = nullptr;
	UBlueprintGeneratedClass* IntroducingClass = nullptr;
};

static FIntroducingNode FindIntroducingNode(const UVoxelPaintVolumeComponent* Component,
	const UClass* OperatedClass)
{
	FIntroducingNode Result;
	if (!Component || !OperatedClass)
	{
		return Result;
	}
	const FName ComponentName = Component->GetFName();
	for (UClass* Class = const_cast<UClass*>(OperatedClass); Class; Class = Class->GetSuperClass())
	{
		UBlueprintGeneratedClass* Bpgc = Cast<UBlueprintGeneratedClass>(Class);
		if (!Bpgc)
		{
			break;
		}
		if (const USimpleConstructionScript* Scs = Bpgc->SimpleConstructionScript)
		{
			if (USCS_Node* Node = Scs->FindSCSNode(ComponentName))
			{
				Result.Node = Node;
				Result.IntroducingClass = Bpgc;
				break;
			}
		}
	}
	return Result;
}

static UVoxelPaintVolumeComponent* FindClassLocalOverrideTemplate(
	const UClass* OperatedClass, const USCS_Node* Node)
{
	UBlueprintGeneratedClass* OperatedBpgc =
		Cast<UBlueprintGeneratedClass>(const_cast<UClass*>(OperatedClass));
	if (!OperatedBpgc || !Node)
	{
		return nullptr;
	}
	UInheritableComponentHandler* Handler = OperatedBpgc->GetInheritableComponentHandler(false);
	if (!Handler)
	{
		return nullptr;
	}
	return Cast<UVoxelPaintVolumeComponent>(
		Handler->GetOverridenComponentTemplate(FComponentKey(Node)));
}

static FString GetClassDisplayName(const UClass* Class)
{
	if (!Class)
	{
		return TEXT("<unresolved class>");
	}
	if (const UObject* GeneratedBy = Class->ClassGeneratedBy)
	{
		return GeneratedBy->GetName();
	}
	return Class->GetName();
}

static bool PoolEntriesEqual(const TArray<FVoxelPaintVariantPoolEntry>& A,
	const TArray<FVoxelPaintVariantPoolEntry>& B)
{
	if (A.Num() != B.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		if (A[Index].Variant != B[Index].Variant || A[Index].Weight != B[Index].Weight)
		{
			return false;
		}
	}
	return true;
}

static bool ExpectedTopologiesEqual(const FVoxelPaintExpectedTopology& A,
	const FVoxelPaintExpectedTopology& B)
{
	return A.bIsSet == B.bIsSet
		&& A.LocalHalfExtent == B.LocalHalfExtent
		&& A.BrickCounts == B.BrickCounts
		&& A.StorageOrigin == B.StorageOrigin;
}

} // namespace TemplateRoutingPrivate

EVoxelPaintPoolProvenance ClassifyPoolProvenance(const UVoxelPaintVolumeComponent* Component,
	const UClass* OperatedClass)
{
	using namespace TemplateRoutingPrivate;

	if (!Component)
	{
		return EVoxelPaintPoolProvenance::UCS;
	}
	switch (Component->CreationMethod)
	{
	case EComponentCreationMethod::Native:
		return EVoxelPaintPoolProvenance::Native;
	case EComponentCreationMethod::Instance:
		return EVoxelPaintPoolProvenance::Instance;
	case EComponentCreationMethod::UserConstructionScript:
		return EVoxelPaintPoolProvenance::UCS;
	case EComponentCreationMethod::SimpleConstructionScript:
	default:
		break;
	}

	const FIntroducingNode Introducing = FindIntroducingNode(Component, OperatedClass);
	if (!Introducing.Node)
	{
		return EVoxelPaintPoolProvenance::SCSAncestor;
	}
	if (Introducing.IntroducingClass == OperatedClass)
	{
		return EVoxelPaintPoolProvenance::SCSOwnClass;
	}
	if (FindClassLocalOverrideTemplate(OperatedClass, Introducing.Node))
	{
		return EVoxelPaintPoolProvenance::SCSOverrideOwner;
	}
	return EVoxelPaintPoolProvenance::SCSAncestor;
}

UVoxelPaintVolumeComponent* ResolveEffectiveSupplyingTemplate(
	const UVoxelPaintVolumeComponent* Component)
{
	using namespace TemplateRoutingPrivate;

	if (!Component || Component->IsTemplate()
		|| Component->CreationMethod != EComponentCreationMethod::SimpleConstructionScript)
	{
		return nullptr;
	}
	const AActor* Owner = Component->GetOwner();
	UBlueprintGeneratedClass* OwnerBpgc =
		Owner ? Cast<UBlueprintGeneratedClass>(Owner->GetClass()) : nullptr;
	if (!OwnerBpgc)
	{
		return nullptr;
	}
	const FIntroducingNode Introducing = FindIntroducingNode(Component, OwnerBpgc);
	if (!Introducing.Node)
	{
		return nullptr;
	}
	return Cast<UVoxelPaintVolumeComponent>(Introducing.Node->GetActualComponentTemplate(OwnerBpgc));
}

UVoxelPaintVolumeComponent* ResolveExpectationWriteTarget(UVoxelPaintVolumeComponent* Component,
	FText& OutError)
{
	using namespace TemplateRoutingPrivate;

	if (!Component)
	{
		OutError = LOCTEXT("ExpectationTargetNoComponent",
			"No voxel paint component to route the size expectation to.");
		return nullptr;
	}
	if (Component->IsTemplate())
	{
		return Component;
	}

	const AActor* Owner = Component->GetOwner();
	const UClass* OperatedClass = Owner ? Owner->GetClass() : nullptr;
	const EVoxelPaintPoolProvenance Provenance = ClassifyPoolProvenance(Component, OperatedClass);
	switch (Provenance)
	{
	case EVoxelPaintPoolProvenance::Native:
	case EVoxelPaintPoolProvenance::Instance:
		return Component;
	case EVoxelPaintPoolProvenance::SCSOwnClass:
	{
		const FIntroducingNode Introducing = FindIntroducingNode(Component, OperatedClass);
		UVoxelPaintVolumeComponent* NodeTemplate = Introducing.Node
			? Cast<UVoxelPaintVolumeComponent>(Introducing.Node->ComponentTemplate) : nullptr;
		if (!NodeTemplate)
		{
			OutError = FText::Format(LOCTEXT("ExpectationTargetNoNodeTemplate",
				"Could not resolve the Blueprint component template supplying '{0}'."),
				FText::FromString(Component->GetName()));
			return nullptr;
		}
		return NodeTemplate;
	}
	case EVoxelPaintPoolProvenance::SCSOverrideOwner:
	{
		const FIntroducingNode Introducing = FindIntroducingNode(Component, OperatedClass);
		UVoxelPaintVolumeComponent* OverrideTemplate =
			FindClassLocalOverrideTemplate(OperatedClass, Introducing.Node);
		if (!OverrideTemplate)
		{
			OutError = FText::Format(LOCTEXT("ExpectationTargetNoOverrideTemplate",
				"Could not resolve the class-local component override supplying '{0}'."),
				FText::FromString(Component->GetName()));
			return nullptr;
		}
		return OverrideTemplate;
	}
	case EVoxelPaintPoolProvenance::SCSAncestor:
	{
		const FIntroducingNode Introducing = FindIntroducingNode(Component, OperatedClass);
		const FString OwningClassName = GetClassDisplayName(Introducing.IntroducingClass);
		OutError = FText::Format(LOCTEXT("ExpectationTargetAncestorOwned",
			"This component's paint pool is owned by '{0}' - resize from a placement of '{0}', or create a class-local override in this Blueprint's editor first."),
			FText::FromString(OwningClassName));
		return nullptr;
	}
	case EVoxelPaintPoolProvenance::UCS:
	default:
		OutError = LOCTEXT("ExpectationTargetUCS",
			"Construction-script-created voxel paint components are unsupported; add the component in the Blueprint's Components panel or on the placed actor instead.");
		return nullptr;
	}
}

void PropagateTemplatePoolChange(UVoxelPaintVolumeComponent* Template,
	const TArray<FVoxelPaintVariantPoolEntry>& PreEditPool,
	const FVoxelPaintExpectedTopology& PreEditExpectation)
{
	using namespace TemplateRoutingPrivate;

	if (!Template || !Template->IsTemplate())
	{
		return;
	}
	const bool bPoolChanged = !PoolEntriesEqual(Template->VolumeVariants, PreEditPool);
	const bool bExpectationChanged =
		!ExpectedTopologiesEqual(Template->ExpectedTopology, PreEditExpectation);
	if (!bPoolChanged && !bExpectationChanged)
	{
		return;
	}

	FProperty* PoolProperty = UVoxelPaintVolumeComponent::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, VolumeVariants));
	FProperty* ExpectationProperty = UVoxelPaintVolumeComponent::StaticClass()->FindPropertyByName(
		GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeComponent, ExpectedTopology));

	TArray<UObject*> ArchetypeInstances;
	Template->GetArchetypeInstances(ArchetypeInstances);
	for (UObject* Object : ArchetypeInstances)
	{
		UVoxelPaintVolumeComponent* Instance = Cast<UVoxelPaintVolumeComponent>(Object);
		// GetArchetypeInstances is transitive: it also returns instances supplied by a derived
		// class's ICH override template. Those instances follow their override, not this
		// template - syncing them here would diverge them from their own archetype.
		if (!Instance || Instance->IsTemplate() || Instance->GetArchetype() != Template)
		{
			continue;
		}
		const bool bSyncPool = bPoolChanged
			&& PoolEntriesEqual(Instance->VolumeVariants, PreEditPool);
		const bool bSyncExpectation = bExpectationChanged
			&& ExpectedTopologiesEqual(Instance->ExpectedTopology, PreEditExpectation);
		if (!bSyncPool && !bSyncExpectation)
		{
			continue;
		}
		Instance->CapturePreEditPoolState();
		Instance->Modify();
		if (bSyncPool)
		{
			Instance->VolumeVariants = Template->VolumeVariants;
			FPropertyChangedEvent PoolEvent(PoolProperty);
			Instance->PostEditChangeProperty(PoolEvent);
		}
		if (bSyncExpectation)
		{
			Instance->ExpectedTopology = Template->ExpectedTopology;
			FPropertyChangedEvent ExpectationEvent(ExpectationProperty);
			Instance->PostEditChangeProperty(ExpectationEvent);
		}
	}
}

} // namespace WorldVoxelPaint

#undef LOCTEXT_NAMESPACE
