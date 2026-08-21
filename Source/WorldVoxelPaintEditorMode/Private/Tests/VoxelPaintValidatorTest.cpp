// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetRegistry/AssetData.h"
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/DataValidation.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "VoxelPaintAssetValidator.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintSessionMachine.h"
#include "VoxelPaintTypes.h"
#include "VoxelPaintVolumeData.h"

namespace VoxelPaintValidatorTestPrivate
{
	static FVoxelPaintReferenceRecord MakeRecord(const FGuid& ActorGuid, const TCHAR* SourcePackage)
	{
		FVoxelPaintReferenceRecord Record;
		Record.ActorGuid = ActorGuid;
		Record.SourcePackage = FName(SourcePackage);
		Record.bLoaded = true;
		return Record;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintValidator_Classification,
	"Game.WorldVoxelPaint.Unit.Validator.Classification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintValidator_Classification::RunTest(const FString& Parameters)
{
	using namespace VoxelPaintValidatorTestPrivate;

	const FGuid GuidA = FGuid::NewGuid();
	const FGuid GuidB = FGuid::NewGuid();
	const TCHAR* MapPackage = TEXT("/Game/Maps/TestMap");
	const TCHAR* LevelInstanceSource = TEXT("/Game/Levels/LI_SharedSource");
	const TCHAR* CopiedMapPackage = TEXT("/Game/Maps/TestMap_Copy");

	{
		int32 LogicalPlacements = -1;
		TestTrue(TEXT("Shared with no records is legitimate"),
			ClassifyReferences(EVoxelPaintAssetSharingMode::Shared,
				TConstArrayView<FVoxelPaintReferenceRecord>(), LogicalPlacements)
				== EVoxelPaintMultiRefVerdict::Legitimate);

		TArray<FVoxelPaintReferenceRecord> Many;
		Many.Add(MakeRecord(GuidA, MapPackage));
		Many.Add(MakeRecord(GuidB, MapPackage));
		Many.Add(MakeRecord(FGuid::NewGuid(), CopiedMapPackage));
		TestTrue(TEXT("Shared is legitimate at ANY reference count"),
			ClassifyReferences(EVoxelPaintAssetSharingMode::Shared, Many, LogicalPlacements)
				== EVoxelPaintMultiRefVerdict::Legitimate);
	}

	{
		int32 LogicalPlacements = -1;
		TestTrue(TEXT("Private with no records is legitimate"),
			ClassifyReferences(EVoxelPaintAssetSharingMode::Private,
				TConstArrayView<FVoxelPaintReferenceRecord>(), LogicalPlacements)
				== EVoxelPaintMultiRefVerdict::Legitimate);
		TestTrue(TEXT("No records means no logical placements"), LogicalPlacements == 0);

		TArray<FVoxelPaintReferenceRecord> Single;
		Single.Add(MakeRecord(GuidA, MapPackage));
		TestTrue(TEXT("Private with a single placement is legitimate"),
			ClassifyReferences(EVoxelPaintAssetSharingMode::Private, Single, LogicalPlacements)
				== EVoxelPaintMultiRefVerdict::Legitimate);
		TestTrue(TEXT("Single placement counts once"), LogicalPlacements == 1);
	}

	{
		int32 LogicalPlacements = -1;
		TArray<FVoxelPaintReferenceRecord> TwoActors;
		TwoActors.Add(MakeRecord(GuidA, MapPackage));
		TwoActors.Add(MakeRecord(GuidB, MapPackage));
		TestTrue(TEXT("Private with two distinct actors is a multi-reference failure"),
			ClassifyReferences(EVoxelPaintAssetSharingMode::Private, TwoActors, LogicalPlacements)
				== EVoxelPaintMultiRefVerdict::PrivateMultiReference);
		TestTrue(TEXT("Two distinct actors count twice"), LogicalPlacements == 2);
	}

	{
		int32 LogicalPlacements = -1;
		TArray<FVoxelPaintReferenceRecord> RepeatedLevelInstance;
		RepeatedLevelInstance.Add(MakeRecord(GuidA, LevelInstanceSource));
		RepeatedLevelInstance.Add(MakeRecord(GuidA, LevelInstanceSource));
		RepeatedLevelInstance.Add(MakeRecord(GuidA, LevelInstanceSource));
		TestTrue(TEXT("Repeated Level Instances of one serialized actor are ONE logical placement"),
			ClassifyReferences(EVoxelPaintAssetSharingMode::Private, RepeatedLevelInstance, LogicalPlacements)
				== EVoxelPaintMultiRefVerdict::Legitimate);
		TestTrue(TEXT("Repeated Level Instances count once"), LogicalPlacements == 1);
	}

	{
		int32 LogicalPlacements = -1;
		TArray<FVoxelPaintReferenceRecord> PackageCopy;
		PackageCopy.Add(MakeRecord(GuidA, MapPackage));
		PackageCopy.Add(MakeRecord(GuidA, CopiedMapPackage));
		TestTrue(TEXT("A package-copy clone (same guid, different package) fails as a second placement"),
			ClassifyReferences(EVoxelPaintAssetSharingMode::Private, PackageCopy, LogicalPlacements)
				== EVoxelPaintMultiRefVerdict::PrivateMultiReference);
		TestTrue(TEXT("The package-copy residual counts twice (the accepted residual's backstop)"),
			LogicalPlacements == 2);
	}

	{
		int32 LogicalPlacements = -1;
		TArray<FVoxelPaintReferenceRecord> Mixed;
		Mixed.Add(MakeRecord(GuidA, LevelInstanceSource));
		Mixed.Add(MakeRecord(GuidA, LevelInstanceSource));
		Mixed.Add(MakeRecord(GuidB, MapPackage));
		TestTrue(TEXT("An LI-repeat pair plus one distinct actor is a multi-reference failure"),
			ClassifyReferences(EVoxelPaintAssetSharingMode::Private, Mixed, LogicalPlacements)
				== EVoxelPaintMultiRefVerdict::PrivateMultiReference);
		TestTrue(TEXT("The mixed set counts two logical placements"), LogicalPlacements == 2);
	}

	{
		using namespace WorldVoxelPaint;
		FVoxelPaintSessionMachine Machine;
		TestFalse(TEXT("Closed is not pre-commit: a closed asset never fails the session check"),
			Machine.IsPreCommit());

		UVoxelPaintVolumeData* Asset = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
		FVoxelPaintSessionController& Controller = FVoxelPaintSessionController::Get();
		TestTrue(TEXT("GetStageForAsset reads Closed with no session (validator passes the session check)"),
			Controller.GetStageForAsset(Asset) == EVoxelPaintSessionStage::Closed);
		TestTrue(TEXT("GetStageForAsset reads Closed for a null asset"),
			Controller.GetStageForAsset(nullptr) == EVoxelPaintSessionStage::Closed);
	}

	{
		using namespace WorldVoxelPaint;
		ON_SCOPE_EXIT
		{
			UVoxelPaintAssetValidator::SetStageQueryOverrideForTest(nullptr);
		};

		UVoxelPaintAssetValidator* Validator =
			NewObject<UVoxelPaintAssetValidator>(GetTransientPackage());
		UVoxelPaintVolumeData* Asset = NewObject<UVoxelPaintVolumeData>(GetTransientPackage());
		const FAssetData AssetData(Asset);
		const TCHAR* FailureText = TEXT("Finish or Discard the open paint session, save, then submit again.");

		const EVoxelPaintSessionStage FailingStages[] =
		{
			EVoxelPaintSessionStage::Active,
			EVoxelPaintSessionStage::Sealing,
			EVoxelPaintSessionStage::Canonicalizing
		};
		for (EVoxelPaintSessionStage Stage : FailingStages)
		{
			UVoxelPaintAssetValidator::SetStageQueryOverrideForTest(
				[Stage](const UVoxelPaintVolumeData*) { return Stage; });
			FDataValidationContext Context;
			TestTrue(TEXT("Pre-commit stage fails validation via the stage-query stub"),
				Validator->ValidateLoadedAsset(AssetData, Asset, Context)
					== EDataValidationResult::Invalid);
			bool bFoundFailureText = false;
			for (const FDataValidationContext::FIssue& Issue : Context.GetIssues())
			{
				bFoundFailureText |= Issue.Message.ToString().Contains(FailureText);
			}
			TestTrue(TEXT("Uncommitted-session failure text names the remedy"), bFoundFailureText);
		}

		const EVoxelPaintSessionStage PassingStages[] =
		{
			EVoxelPaintSessionStage::Closed,
			EVoxelPaintSessionStage::Committed,
			EVoxelPaintSessionStage::Deriving,
			EVoxelPaintSessionStage::Publication,
			EVoxelPaintSessionStage::Retirement,
			EVoxelPaintSessionStage::DerivedFailed
		};
		for (EVoxelPaintSessionStage Stage : PassingStages)
		{
			UVoxelPaintAssetValidator::SetStageQueryOverrideForTest(
				[Stage](const UVoxelPaintVolumeData*) { return Stage; });
			FDataValidationContext Context;
			TestTrue(TEXT("Closed and post-commit stages never fail the session check"),
				Validator->ValidateLoadedAsset(AssetData, Asset, Context)
					== EDataValidationResult::Valid);
			bool bFoundFailureText = false;
			for (const FDataValidationContext::FIssue& Issue : Context.GetIssues())
			{
				bFoundFailureText |= Issue.Message.ToString().Contains(FailureText);
			}
			TestFalse(TEXT("No uncommitted-session failure text at Closed/post-commit stages"),
				bFoundFailureText);
		}
	}

	return true;
}
