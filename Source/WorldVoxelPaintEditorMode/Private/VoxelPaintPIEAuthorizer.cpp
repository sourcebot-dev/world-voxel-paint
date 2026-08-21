// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintPIEAuthorizer.h"

#include "Features/IModularFeatures.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/CoreDelegates.h"
#include "VoxelPaintSessionController.h"
#include "Widgets/Notifications/SNotificationList.h"

bool FVoxelPaintPIEAuthorizer::RequestPIEPermission(bool bIsSimulateInEditor, FString& OutReason) const
{
	WorldVoxelPaint::EVoxelPaintSessionStage Stage = WorldVoxelPaint::EVoxelPaintSessionStage::Closed;
	if (StageOverrideForTest.IsSet())
	{
		Stage = StageOverrideForTest.GetValue();
	}
	else if (const FVoxelPaintSessionController* Controller = FVoxelPaintSessionController::GetIfCreated())
	{
		Stage = Controller->GetStage();
	}
	const bool bAllowed = WorldVoxelPaint::ResolvePIEVerdict(Stage, OutReason);
	// The engine consumer (PlayLevel.cpp) only logs the denial; the authorizer owns the
	// user-facing notification. The test-override path stays silent.
	if (!bAllowed && !StageOverrideForTest.IsSet() && FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(FText::FromString(OutReason));
		Info.ExpireDuration = 8.0f;
		Info.bUseLargeFont = true;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
	return bAllowed;
}

void FVoxelPaintPIEAuthorizer::SetStageOverrideForTest(WorldVoxelPaint::EVoxelPaintSessionStage Stage)
{
	StageOverrideForTest = Stage;
}

void FVoxelPaintPIEAuthorizer::ClearStageOverrideForTest()
{
	StageOverrideForTest.Reset();
}

namespace
{

struct FVoxelPaintPIEAuthorizerRegistrar
{
	FVoxelPaintPIEAuthorizerRegistrar()
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([this]()
		{
			IModularFeatures::Get().RegisterModularFeature(
				IPIEAuthorizer::GetModularFeatureName(), &Authorizer);
		});
	}

	~FVoxelPaintPIEAuthorizerRegistrar()
	{
		if (PostEngineInitHandle.IsValid())
		{
			FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
			PostEngineInitHandle.Reset();
		}
		IModularFeatures::Get().UnregisterModularFeature(
			IPIEAuthorizer::GetModularFeatureName(), &Authorizer);
	}

	FVoxelPaintPIEAuthorizer Authorizer;
	FDelegateHandle PostEngineInitHandle;
};

FVoxelPaintPIEAuthorizerRegistrar GVoxelPaintPIEAuthorizerRegistrar;

}
