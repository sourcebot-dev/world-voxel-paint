// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Features/IModularFeatures.h"
#include "IPIEAuthorizer.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintAuthority.h"
#include "VoxelPaintGridReadiness.h"
#include "VoxelPaintPIEAuthorizer.h"
#include "VoxelPaintSessionController.h"
#include "VoxelPaintSessionMachine.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintSession_PIEGate,
	"Game.WorldVoxelPaint.Unit.Session.PIEGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintSession_PIEGate::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	const EVoxelPaintSessionStage AllStages[] =
	{
		EVoxelPaintSessionStage::Closed,
		EVoxelPaintSessionStage::Active,
		EVoxelPaintSessionStage::Sealing,
		EVoxelPaintSessionStage::Canonicalizing,
		EVoxelPaintSessionStage::Committed,
		EVoxelPaintSessionStage::Deriving,
		EVoxelPaintSessionStage::Publication,
		EVoxelPaintSessionStage::Retirement,
		EVoxelPaintSessionStage::DerivedFailed
	};

	for (EVoxelPaintSessionStage Stage : AllStages)
	{
		FString Reason;
		const bool bAllowed = ResolvePIEVerdict(Stage, Reason);
		if (Stage == EVoxelPaintSessionStage::Closed)
		{
			TestTrue(TEXT("Only Closed allows PIE"), bAllowed);
			TestTrue(TEXT("Allowed verdict carries no reason"), Reason.IsEmpty());
		}
		else
		{
			TestFalse(TEXT("Every non-Closed stage denies PIE"), bAllowed);
			TestTrue(TEXT("Denial names the stage"), Reason.Contains(LexToString(Stage)));
			TestTrue(TEXT("Denial names the Finish remedy"), Reason.Contains(TEXT("Finish")));
			TestTrue(TEXT("Denial names the Discard remedy"), Reason.Contains(TEXT("Discard")));
			TestTrue(TEXT("Denial names the End Anyway remedy"), Reason.Contains(TEXT("End Anyway")));
		}
	}

	{
		FVoxelPaintSessionAdmissionInputs Inputs;
		Inputs.bAnotherSessionOpen = false;
		Inputs.bWorldIsMaster = true;
		Inputs.bEligiblePrimary = true;
		Inputs.GridReadiness = (uint8)EVoxelPaintGridReadiness::Ready;
		Inputs.bPIEActiveOrQueued = true;
		Inputs.AuthorityVerdict = (uint8)EVoxelPaintAuthorityVerdict::Allowed;
		Inputs.bAssetValid = true;
		TestTrue(TEXT("Symmetric exclusion: PIE active or queued denies session admission"),
			FVoxelPaintSessionMachine::JudgeAdmission(Inputs)
				== EVoxelPaintSessionAdmissionVerdict::DeniedPIEActiveOrQueued);
	}

	{
		TestTrue(TEXT("The PIEAuthorizer modular feature is registered (module wiring smoke test)"),
			IModularFeatures::Get().GetModularFeatureImplementations<IPIEAuthorizer>(
				IPIEAuthorizer::GetModularFeatureName()).Num() >= 1);
	}

	{
		FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated();
		if (Controller)
		{
			TestTrue(TEXT("Headless run holds no session: controller stage is Closed"),
				Controller->GetStage() == EVoxelPaintSessionStage::Closed);
		}

		FVoxelPaintPIEAuthorizer Authorizer;
		FString Reason;
		TestTrue(TEXT("Direct RequestPIEPermission (PIE) allows while fully Closed"),
			Authorizer.RequestPIEPermission(false, Reason));
		TestTrue(TEXT("Allowed PIE request carries no reason"), Reason.IsEmpty());
		TestTrue(TEXT("Direct RequestPIEPermission (SIE) allows while fully Closed"),
			Authorizer.RequestPIEPermission(true, Reason));

		for (EVoxelPaintSessionStage Stage : AllStages)
		{
			Authorizer.SetStageOverrideForTest(Stage);
			FString StageReason;
			const bool bAllowed = Authorizer.RequestPIEPermission(false, StageReason);
			if (Stage == EVoxelPaintSessionStage::Closed)
			{
				TestTrue(TEXT("Authorizer allows PIE only at fabricated Closed"), bAllowed);
				TestTrue(TEXT("Fabricated Closed carries no reason"), StageReason.IsEmpty());
			}
			else
			{
				TestFalse(TEXT("Authorizer denies PIE at every fabricated non-Closed stage"), bAllowed);
				TestTrue(TEXT("Fabricated denial names the stage"),
					StageReason.Contains(LexToString(Stage)));
				TestTrue(TEXT("Fabricated denial names the Finish remedy"),
					StageReason.Contains(TEXT("Finish")));
				TestTrue(TEXT("Fabricated denial names the Discard remedy"),
					StageReason.Contains(TEXT("Discard")));
				TestTrue(TEXT("Fabricated denial names the End Anyway remedy"),
					StageReason.Contains(TEXT("End Anyway")));
			}
			const bool bSIEAllowed = Authorizer.RequestPIEPermission(true, StageReason);
			TestTrue(TEXT("SIE verdict matches the PIE verdict at every fabricated stage"),
				bSIEAllowed == bAllowed);
		}
		Authorizer.ClearStageOverrideForTest();
		TestTrue(TEXT("Cleared override falls back to the live controller stage (Closed)"),
			Authorizer.RequestPIEPermission(false, Reason));
	}

	return true;
}
