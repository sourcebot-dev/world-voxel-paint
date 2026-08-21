// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Optional.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintSessionMachine.h"
#include "VoxelPaintSessionRenderTypes.h"
#include "VoxelPaintVolumeData.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintSession_PerPlacementStrokeGate,
	"Game.WorldVoxelPaint.Unit.Session.PerPlacementStrokeGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintSession_PerPlacementStrokeGate::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	const FVoxelPaintFrontierIdentity PublishedIdentity = FGuid::NewGuid();
	const FVoxelPaintFrontierIdentity UnpublishedIdentity = FGuid::NewGuid();

	{
		FVoxelPaintSessionRenderStatus Status;
		Status.SessionGeneration = 7;
		Status.SelectorPublishedIdentities.Add(PublishedIdentity);

		TestTrue(TEXT("Predicate: identity present + generation match is published"),
			IsPlacementSelectorPublished(Status, 7, PublishedIdentity));
		TestFalse(TEXT("Predicate: a generation mismatch is never published"),
			IsPlacementSelectorPublished(Status, 8, PublishedIdentity));
		TestFalse(TEXT("Predicate: an absent identity is never published"),
			IsPlacementSelectorPublished(Status, 7, UnpublishedIdentity));

		FVoxelPaintSessionRenderStatus GlobalOnlyStatus;
		GlobalOnlyStatus.SessionGeneration = 7;
		GlobalOnlyStatus.bSelectorPublishedLastFrame = true;
		TestFalse(TEXT("Predicate: the global selector flag alone is necessary but not sufficient"),
			IsPlacementSelectorPublished(GlobalOnlyStatus, 7, UnpublishedIdentity));
	}

	FVoxelPaintSessionController& Controller = FVoxelPaintSessionController::Get();
	ON_SCOPE_EXIT
	{
		Controller.SetTestSessionStateOverride(TOptional<FVoxelPaintTestSessionState>());
	};

	UVoxelPaintVolumeData* Asset = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
	if (!TestNotNull(TEXT("Transient asset created"), Asset))
	{
		return false;
	}

	{
		FVoxelPaintTestSessionState Injected;
		Injected.Asset = Asset;
		Injected.Stage = EVoxelPaintSessionStage::Active;
		Injected.SessionGeneration = 7;
		FVoxelPaintSessionRenderStatus Status;
		Status.SessionGeneration = 7;
		Status.bSelectorPublishedLastFrame = true;
		Status.SelectorPublishedIdentities.Add(PublishedIdentity);
		Injected.RenderStatus = Status;
		Controller.SetTestSessionStateOverride(Injected);

		FText WhyNot;
		TestTrue(TEXT("Base CanMutateNow passes on the fabricated status"),
			Controller.CanMutateNow(WhyNot));
		TestTrue(TEXT("Per-placement gate opens for a published identity"),
			Controller.CanMutateNow(PublishedIdentity, WhyNot));

		WhyNot = FText::GetEmpty();
		TestFalse(TEXT("Per-placement gate blocks an unpublished identity"),
			Controller.CanMutateNow(UnpublishedIdentity, WhyNot));
		TestTrue(FString::Printf(TEXT("Unpublished-identity refusal carries the reason text (got '%s')"),
			*WhyNot.ToString()),
			WhyNot.ToString().Contains(TEXT("Waiting for this placement's paint binding to publish")));
	}

	{
		FVoxelPaintTestSessionState Injected;
		Injected.Asset = Asset;
		Injected.Stage = EVoxelPaintSessionStage::Active;
		Injected.SessionGeneration = 7;
		FVoxelPaintSessionRenderStatus StaleStatus;
		StaleStatus.SessionGeneration = 6;
		StaleStatus.bSelectorPublishedLastFrame = true;
		StaleStatus.SelectorPublishedIdentities.Add(PublishedIdentity);
		Injected.RenderStatus = StaleStatus;
		Controller.SetTestSessionStateOverride(Injected);

		FText WhyNot;
		TestFalse(TEXT("Per-placement gate blocks on a stale status generation"),
			Controller.CanMutateNow(PublishedIdentity, WhyNot));
		TestTrue(TEXT("Stale-generation refusal carries the reason text"),
			WhyNot.ToString().Contains(TEXT("Waiting for this placement's paint binding to publish")));
	}

	Controller.SetTestSessionStateOverride(TOptional<FVoxelPaintTestSessionState>());
	{
		FText WhyNot;
		TestFalse(TEXT("Cleared override reverts to the live machine (no session, cannot mutate)"),
			Controller.CanMutateNow(WhyNot));
	}

	return true;
}
