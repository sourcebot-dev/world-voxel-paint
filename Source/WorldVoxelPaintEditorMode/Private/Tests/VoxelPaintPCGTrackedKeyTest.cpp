// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Elements/PCGActorSelector.h"
#include "Misc/AutomationTest.h"
#include "PCGSettings.h"
#include "VoxelPaintPCGSampleSettings.h"
#include "WorldVoxelPaintConstants.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPCG_TrackedKeys,
	"Game.WorldVoxelPaint.Unit.PCG.TrackedKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPCG_TrackedKeys::RunTest(const FString& Parameters)
{
	const UVoxelPaintPCGSampleSettings* Settings = GetDefault<UVoxelPaintPCGSampleSettings>();
	if (!TestNotNull(TEXT("Settings CDO resolves"), Settings))
	{
		return false;
	}

	FPCGSelectionKeyToSettingsMap KeysToSettings;
	TArray<TObjectPtr<const UPCGGraph>> VisitedGraphs;
	Settings->GetStaticTrackedKeys(KeysToSettings, VisitedGraphs);

	TestEqual(TEXT("Exactly one statically tracked key"), KeysToSettings.Num(), 1);
	TestEqual(TEXT("No visited graphs (leaf settings, no subgraph recursion)"),
		VisitedGraphs.Num(), 0);

	// The registered key must unify (exported operator== / GetTypeHash) with a fresh ByTag key
	// built from the frozen host-tag constant - exactly how PCG's tracking maps look it up.
	const FPCGSelectionKey Expected(WorldVoxelPaint::PaintVolumeHostTag);
	const TArray<FPCGSettingsAndCulling>* Entries = KeysToSettings.Find(Expected);
	if (!TestNotNull(TEXT("Registered key is the ByTag key for the WVP host tag"), Entries))
	{
		return false;
	}

	for (const TPair<FPCGSelectionKey, TArray<FPCGSettingsAndCulling>>& Pair : KeysToSettings)
	{
		TestTrue(TEXT("Selection is ByTag"), Pair.Key.Selection == EPCGActorSelection::ByTag);
		TestTrue(TEXT("Tag is the frozen literal"),
			Pair.Key.Tag == FName(TEXT("WVP.PaintVolume")));
		TestTrue(TEXT("Filter is AllWorldActors"),
			Pair.Key.ActorFilter == EPCGActorFilter::AllWorldActors);
		TestTrue(TEXT("No extra dependency"), Pair.Key.OptionalExtraDependency == nullptr);
	}

	if (!TestEqual(TEXT("One settings registration on the key"), Entries->Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("Registration references the settings CDO"),
		(*Entries)[0].Key.Get() == Settings);
	TestTrue(TEXT("Registration is culled (bounds-intersect gated, never refresh-everything)"),
		(*Entries)[0].Value);
	TestTrue(TEXT("Key evaluates culled overall (PCGSettings::IsKeyCulled)"),
		PCGSettings::IsKeyCulled(*Entries));

	return true;
}
