// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VoxelPaintAuthority.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintSessionMachine.h"
#include "VoxelPaintTypes.h"
#include "VoxelPaintVolumeData.h"

namespace VoxelPaintAuthorityTestPrivate
{

struct FVoxelPaintAuthorityTestWorld
{
	UWorld* World = nullptr;

	FVoxelPaintAuthorityTestWorld()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("VoxelPaintAuthorityTest"));
		FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
		WorldContext.SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
	}

	~FVoxelPaintAuthorityTestWorld()
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}
};

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintAuthority_EditingGate,
	"Game.WorldVoxelPaint.Unit.Authority.EditingGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintAuthority_EditingGate::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;
	using namespace VoxelPaintAuthorityTestPrivate;

	FVoxelPaintAuthorityTestWorld TestWorld;
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* ActorA = TestWorld.World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
	AActor* ActorB = TestWorld.World->SpawnActor<AActor>(FVector(100.0), FRotator::ZeroRotator, SpawnParameters);
	if (!TestNotNull(TEXT("Actor A spawned"), ActorA) || !TestNotNull(TEXT("Actor B spawned"), ActorB))
	{
		return false;
	}
	TestTrue(TEXT("Spawned actors carry valid serialized ActorGuids"),
		ActorA->GetActorGuid().IsValid() && ActorB->GetActorGuid().IsValid());
	TestTrue(TEXT("Distinct actors carry distinct ActorGuids"),
		ActorA->GetActorGuid() != ActorB->GetActorGuid());

	UVoxelPaintVolumeData* Asset = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	if (!TestNotNull(TEXT("Transient asset created"), Asset))
	{
		return false;
	}
	TestTrue(TEXT("Fresh asset defaults to Private"), Asset->SharingMode == EVoxelPaintAssetSharingMode::Private);

	{
		FVoxelPaintAuthorityDenial Denial;
		TestTrue(TEXT("Null asset is DeniedOwnerless"),
			CheckEditingAuthority(nullptr, ActorA, &Denial) == EVoxelPaintAuthorityVerdict::DeniedOwnerless);
		TestTrue(TEXT("Null-asset denial carries a description"),
			Denial.Verdict == EVoxelPaintAuthorityVerdict::DeniedOwnerless && !Denial.OwnerDescription.IsEmpty());
	}

	{
		FVoxelPaintAuthorityDenial Denial;
		TestTrue(TEXT("Private asset with no owner guid is DeniedOwnerless"),
			CheckEditingAuthority(Asset, ActorA, &Denial) == EVoxelPaintAuthorityVerdict::DeniedOwnerless);
		TestTrue(TEXT("Ownerless denial names the missing owner placement"),
			!Denial.OwnerDescription.IsEmpty());
	}

	AssignOwner(Asset, ActorA);
	TestTrue(TEXT("AssignOwner keys on the serialized ActorGuid"),
		Asset->OwnerActorGuid == ActorA->GetActorGuid());
	TestTrue(TEXT("AssignOwner records an owner display name"), !Asset->OwnerDisplayName.IsEmpty());

	{
		FVoxelPaintAuthorityDenial Denial;
		TestTrue(TEXT("Owner placement is Allowed"),
			CheckEditingAuthority(Asset, ActorA, &Denial) == EVoxelPaintAuthorityVerdict::Allowed);
		TestTrue(TEXT("Non-owner placement is DeniedNotOwner"),
			CheckEditingAuthority(Asset, ActorB, &Denial) == EVoxelPaintAuthorityVerdict::DeniedNotOwner);
		TestTrue(TEXT("Not-owner denial names the owner"),
			Denial.Verdict == EVoxelPaintAuthorityVerdict::DeniedNotOwner && !Denial.OwnerDescription.IsEmpty());
		TestTrue(TEXT("Missing initiating actor on an owned Private asset is DeniedNotOwner"),
			CheckEditingAuthority(Asset, nullptr, &Denial) == EVoxelPaintAuthorityVerdict::DeniedNotOwner);
	}

	MarkShared(Asset);
	TestTrue(TEXT("MarkShared switches the mode"), Asset->SharingMode == EVoxelPaintAssetSharingMode::Shared);
	TestFalse(TEXT("MarkShared clears the owner guid"), Asset->OwnerActorGuid.IsValid());
	TestTrue(TEXT("MarkShared clears the owner display name"), Asset->OwnerDisplayName.IsEmpty());
	TestTrue(TEXT("Shared allows peer A"),
		CheckEditingAuthority(Asset, ActorA) == EVoxelPaintAuthorityVerdict::Allowed);
	TestTrue(TEXT("Shared allows peer B (equal peers)"),
		CheckEditingAuthority(Asset, ActorB) == EVoxelPaintAuthorityVerdict::Allowed);
	TestTrue(TEXT("Shared allows without an initiating actor"),
		CheckEditingAuthority(Asset, nullptr) == EVoxelPaintAuthorityVerdict::Allowed);

	MarkPrivate(Asset, ActorB);
	TestTrue(TEXT("MarkPrivate switches back to Private"),
		Asset->SharingMode == EVoxelPaintAssetSharingMode::Private);
	TestTrue(TEXT("MarkPrivate assigns the initiating placement as owner (no ownerless Private)"),
		Asset->OwnerActorGuid.IsValid() && Asset->OwnerActorGuid == ActorB->GetActorGuid());
	TestTrue(TEXT("New owner is Allowed after MarkPrivate"),
		CheckEditingAuthority(Asset, ActorB) == EVoxelPaintAuthorityVerdict::Allowed);
	TestTrue(TEXT("Old owner is DeniedNotOwner after MarkPrivate"),
		CheckEditingAuthority(Asset, ActorA) == EVoxelPaintAuthorityVerdict::DeniedNotOwner);

	TakeOwnership(Asset, ActorA);
	TestTrue(TEXT("TakeOwnership reassigns the owner"),
		Asset->OwnerActorGuid == ActorA->GetActorGuid()
			&& CheckEditingAuthority(Asset, ActorA) == EVoxelPaintAuthorityVerdict::Allowed);

	TakeOwnership(Asset, nullptr);
	MarkPrivate(Asset, nullptr);
	TestTrue(TEXT("Ownerless transitions are no-ops: no plugin action yields an ownerless Private asset"),
		Asset->SharingMode == EVoxelPaintAssetSharingMode::Private
			&& Asset->OwnerActorGuid == ActorA->GetActorGuid());

	{
		Asset->LocalHalfExtent = FVector(123.0, 456.0, 789.0);
		Asset->BrickCounts = FIntVector(2, 3, 4);
		UVoxelPaintVolumeData* Copy = DeepCopyVolumeDataForPlacement(Asset, ActorB);
		if (TestNotNull(TEXT("Make Unique deep copy created"), Copy))
		{
			TestTrue(TEXT("Deep copy is a new object in a new package"),
				Copy != Asset && Copy->GetPackage() != Asset->GetPackage());
			TestTrue(TEXT("Deep copy carries the source topology"),
				Copy->LocalHalfExtent.Equals(Asset->LocalHalfExtent, 0.0)
					&& Copy->BrickCounts == Asset->BrickCounts);
			TestTrue(TEXT("Deep copy is Private and owned by the initiator"),
				Copy->SharingMode == EVoxelPaintAssetSharingMode::Private
					&& Copy->OwnerActorGuid == ActorB->GetActorGuid());
			TestTrue(TEXT("Deep copy leaves the source owner untouched"),
				Asset->OwnerActorGuid == ActorA->GetActorGuid());
			TestTrue(TEXT("Initiator may edit the copy"),
				CheckEditingAuthority(Copy, ActorB) == EVoxelPaintAuthorityVerdict::Allowed);
			TestTrue(TEXT("Source owner may not edit the copy"),
				CheckEditingAuthority(Copy, ActorA) == EVoxelPaintAuthorityVerdict::DeniedNotOwner);
			Copy->GetPackage()->SetDirtyFlag(false);
		}
	}

	{
		FVoxelPaintSessionController& Controller = FVoxelPaintSessionController::Get();
		TestFalse(TEXT("No session is open in the headless run"), Controller.IsSessionOpen());
		TestTrue(TEXT("Stage for an unbound asset reads Closed"),
			Controller.GetStageForAsset(Asset) == WorldVoxelPaint::EVoxelPaintSessionStage::Closed);
		TestFalse(TEXT("Freeze window is inactive without a session"), Controller.IsAssetFrozen(Asset));
		FText WhyNot;
		TestFalse(TEXT("Nothing can mutate without a session"), Controller.CanMutateNow(WhyNot));
	}

	return true;
}
