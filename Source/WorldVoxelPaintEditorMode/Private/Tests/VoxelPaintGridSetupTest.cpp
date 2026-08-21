// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "VoxelPaintDeveloperSettings.h"
#include "VoxelPaintGridSetup.h"
#include "VoxelPaintRecord.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintGridSetupTestPrivate
{
	static FVoxelPaintVolumeRecord MakeRecord(const FVector& Translation, const FVector3f& HalfExtent,
		const FQuat& Rotation, uint32 Flags, const FVector& EvaluationOrigin)
	{
		FVoxelPaintPlacementState State;
		State.Rotation = Rotation;
		State.Translation = Translation;
		State.LocalHalfExtent = HalfExtent;
		State.Flags = Flags;
		return PackVolumeRecord(State, FVoxelPaintResolvedBinding(), EvaluationOrigin);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintGrid_CapacityDerivation,
	"Game.WorldVoxelPaint.Unit.Grid.CapacityDerivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintGrid_CapacityDerivation::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	UVoxelPaintDeveloperSettings* Settings = NewObject<UVoxelPaintDeveloperSettings>(GetTransientPackage());
	Settings->GridCellSizeXY = 500.0f;
	Settings->GridCellSizeZ = 1000.0f;
	Settings->EvaluationRadius = 50000.0f;
	Settings->GuardBandWidth = 10000.0f;
	Settings->GridCandidateCapOverride = 0;
	Settings->DefaultGridZMin = -10000.0f;
	Settings->DefaultGridZMax = 30000.0f;

	FVoxelPaintGridCapacity Capacity = DeriveGridCapacity(*Settings);
	TestTrue(TEXT("Shipped defaults derive a valid capacity"), Capacity.bValid);
	TestTrue(TEXT("Shipped defaults give Nx = Ny = 240"),
		Capacity.WindowCellsCap.X == 240 && Capacity.WindowCellsCap.Y == 240);
	TestTrue(TEXT("Shipped defaults give NzCap = 40"), Capacity.WindowCellsCap.Z == 40);
	TestTrue(TEXT("Shipped defaults give C_cap = 2304000"), Capacity.CCap == 2304000u);
	TestTrue(TEXT("Zero override gives K_cap = C_cap"), Capacity.KCap == Capacity.CCap);

	Settings->GridCandidateCapOverride = 5000000;
	Capacity = DeriveGridCapacity(*Settings);
	TestTrue(TEXT("In-range override is taken verbatim"), Capacity.bValid && Capacity.KCap == 5000000u);

	Settings->GridCandidateCapOverride = 1;
	Capacity = DeriveGridCapacity(*Settings);
	TestTrue(TEXT("Under-C_cap override clamps up to C_cap"), Capacity.bValid && Capacity.KCap == Capacity.CCap);

	Settings->GridCandidateCapOverride = 10000000;
	Capacity = DeriveGridCapacity(*Settings);
	TestTrue(TEXT("Over-ceiling override clamps to 2^23"),
		Capacity.bValid && Capacity.KCap == GridCandidateCapCeiling);

	Settings->GridCandidateCapOverride = 0;
	Settings->GridCellSizeXY = 0.01f;
	Capacity = DeriveGridCapacity(*Settings);
	TestFalse(TEXT("A C_cap beyond the 2^23 ceiling is invalid"), Capacity.bValid);

	Settings->GridCellSizeXY = 0.0f;
	Capacity = DeriveGridCapacity(*Settings);
	TestFalse(TEXT("Zero cell size is invalid"), Capacity.bValid);
	TestTrue(TEXT("Invalid derivation leaves C_cap empty"), Capacity.CCap == 0u);

	Settings->GridCellSizeXY = 500.0f;
	Settings->DefaultGridZMax = Settings->DefaultGridZMin;
	Capacity = DeriveGridCapacity(*Settings);
	TestFalse(TEXT("An empty Z envelope is invalid"), Capacity.bValid);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintGrid_CellRangesAndE,
	"Game.WorldVoxelPaint.Unit.Grid.CellRangesAndE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintGrid_CellRangesAndE::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;
	using namespace VoxelPaintGridSetupTestPrivate;

	FVoxelPaintGridCapacity Capacity;
	Capacity.WindowCellsCap = FIntVector(4, 4, 2);
	Capacity.CCap = 32;
	Capacity.KCap = 32;
	Capacity.bValid = true;

	FVoxelPaintZEnvelope Envelope;
	Envelope.ZMin = -1000.0;
	Envelope.ZMax = 1000.0;

	const FVoxelPaintExpectedWindow Window = MakeExpectedWindow(FVector::ZeroVector, Envelope, Capacity, 500.0f, 1000.0f);
	TestTrue(TEXT("Window valid"), Window.bValid);
	TestTrue(TEXT("Window live cells"), Window.LiveCells == FIntVector(4, 4, 2));
	TestTrue(TEXT("Window origin at the snapped center and Z midpoint"), Window.EvaluationOrigin == FVector::ZeroVector);

	const FQuat Rot90Z(FVector(0.0, 0.0, 1.0), UE_DOUBLE_PI * 0.5);
	TArray<FVoxelPaintVolumeRecord> Records;
	Records.Add(MakeRecord(FVector(0, 0, 0), FVector3f(100.0f), FQuat::Identity, RecordFlag_Valid, Window.EvaluationOrigin));
	Records.Add(MakeRecord(FVector(600, 0, 0), FVector3f(200.0f, 50.0f, 100.0f), Rot90Z, RecordFlag_Valid, Window.EvaluationOrigin));
	Records.Add(MakeRecord(FVector(600, 0, 0), FVector3f(200.0f, 50.0f, 100.0f), FQuat::Identity, RecordFlag_Valid, Window.EvaluationOrigin));
	Records.Add(MakeRecord(FVector(900, -900, 0), FVector3f(300.0f, 300.0f, 100.0f), FQuat::Identity, RecordFlag_Valid, Window.EvaluationOrigin));
	Records.Add(MakeRecord(FVector(0, 0, 1500), FVector3f(100.0f), FQuat::Identity, RecordFlag_Valid, Window.EvaluationOrigin));
	Records.Add(MakeRecord(FVector(0, 0, 0), FVector3f(100.0f), FQuat::Identity, 0u, Window.EvaluationOrigin));

	FVoxelPaintGridBuildInput Input;
	BuildGridInput(Records, Window, Capacity, 500.0f, 1000.0f, Input);

	TestTrue(TEXT("Input valid under K_cap"), Input.bValid);
	TestTrue(TEXT("Grid dims are the live cells"), Input.GridDims == FIntVector(4, 4, 2));
	TestTrue(TEXT("Grid origin is the window min corner relative to the evaluation origin"),
		Input.GridOriginRel == FVector3f(-1000.0f, -1000.0f, -1000.0f));
	TestTrue(TEXT("Cell sizes carried"), Input.CellSize == FVector2f(500.0f, 1000.0f));
	TestTrue(TEXT("K_cap carried"), Input.KCap == 32u);

	TestTrue(TEXT("Four ranges emitted (Z-excluded and invalid records emit nothing)"), Input.Ranges.Num() == 4);
	TestTrue(TEXT("Outside-Z volume counted"), Input.NumOutsideZEnvelope == 1u);

	if (Input.Ranges.Num() == 4)
	{
		TestTrue(TEXT("Axis-aligned volume clamps to cells (1,1,0)..(2,2,1)"),
			Input.Ranges[0].RecordIndex == 0u
			&& Input.Ranges[0].CellMin == FIntVector(1, 1, 0)
			&& Input.Ranges[0].CellDims == FIntVector(2, 2, 2)
			&& Input.Ranges[0].FirstThread == 0u && Input.Ranges[0].CellCount == 8u);
		TestTrue(TEXT("Rotated volume ranges follow the rotated world AABB"),
			Input.Ranges[1].RecordIndex == 1u
			&& Input.Ranges[1].CellMin == FIntVector(3, 1, 0)
			&& Input.Ranges[1].CellDims == FIntVector(1, 2, 2)
			&& Input.Ranges[1].FirstThread == 8u && Input.Ranges[1].CellCount == 4u);
		TestTrue(TEXT("The same placement unrotated spans the wider X range"),
			Input.Ranges[2].RecordIndex == 2u
			&& Input.Ranges[2].CellMin == FIntVector(2, 1, 0)
			&& Input.Ranges[2].CellDims == FIntVector(2, 2, 2)
			&& Input.Ranges[2].FirstThread == 12u && Input.Ranges[2].CellCount == 8u);
		TestTrue(TEXT("Window-straddling volume clamps to the window edge"),
			Input.Ranges[3].RecordIndex == 3u
			&& Input.Ranges[3].CellMin == FIntVector(3, 0, 0)
			&& Input.Ranges[3].CellDims == FIntVector(1, 1, 2)
			&& Input.Ranges[3].FirstThread == 20u && Input.Ranges[3].CellCount == 2u);
	}
	TestTrue(TEXT("E is the exclusive prefix total"), Input.TotalThreads == 22u);

	TArray<FVoxelPaintVolumeRecord> Overflow;
	Overflow.Add(MakeRecord(FVector(0, 0, 0), FVector3f(1100.0f), FQuat::Identity, RecordFlag_Valid, Window.EvaluationOrigin));
	Overflow.Add(MakeRecord(FVector(0, 0, 0), FVector3f(1100.0f), FQuat::Identity, RecordFlag_Valid, Window.EvaluationOrigin));
	FVoxelPaintGridBuildInput OverflowInput;
	BuildGridInput(Overflow, Window, Capacity, 500.0f, 1000.0f, OverflowInput);
	TestTrue(TEXT("Whole-window volumes each cover every live cell"),
		OverflowInput.Ranges.Num() == 2 && OverflowInput.Ranges[0].CellCount == 32u && OverflowInput.Ranges[1].CellCount == 32u);
	TestTrue(TEXT("E over K_cap accumulates without loss"), OverflowInput.TotalThreads == 64u);
	TestFalse(TEXT("E over K_cap fails neutral before dispatch"), OverflowInput.bValid);

	FVoxelPaintGridCapacity BigCapacity;
	BigCapacity.WindowCellsCap = FIntVector(2048, 2048, 2);
	BigCapacity.CCap = 8388608u;
	BigCapacity.KCap = 8388608u;
	BigCapacity.bValid = true;
	const FVoxelPaintExpectedWindow BigWindow = MakeExpectedWindow(FVector::ZeroVector, Envelope, BigCapacity, 500.0f, 1000.0f);
	TestTrue(TEXT("Big window valid"), BigWindow.bValid && BigWindow.LiveCells == FIntVector(2048, 2048, 2));

	TArray<FVoxelPaintVolumeRecord> Saturating;
	for (int32 Index = 0; Index < 512; ++Index)
	{
		Saturating.Add(MakeRecord(FVector(0, 0, 0), FVector3f(600000.0f, 600000.0f, 1100.0f),
			FQuat::Identity, RecordFlag_Valid, BigWindow.EvaluationOrigin));
	}
	FVoxelPaintGridBuildInput SaturatingInput;
	BuildGridInput(Saturating, BigWindow, BigCapacity, 500.0f, 1000.0f, SaturatingInput);
	TestTrue(TEXT("Late range prefix sums accumulate in 64 bits without wrapping"),
		SaturatingInput.Ranges.Num() == 512 && SaturatingInput.Ranges[511].FirstThread == 4286578688u);
	TestTrue(TEXT("E saturates instead of wrapping"), SaturatingInput.TotalThreads == MAX_uint32);
	TestFalse(TEXT("Saturated E fails neutral"), SaturatingInput.bValid);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintGrid_WindowSnapGuard,
	"Game.WorldVoxelPaint.Unit.Grid.WindowSnapGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintGrid_WindowSnapGuard::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	FVoxelPaintGridCapacity Capacity;
	Capacity.WindowCellsCap = FIntVector(4, 4, 4);
	Capacity.CCap = 64;
	Capacity.KCap = 64;
	Capacity.bValid = true;

	FVoxelPaintZEnvelope Envelope;
	Envelope.ZMin = -1000.0;
	Envelope.ZMax = 3000.0;

	const FVoxelPaintExpectedWindow Window = MakeExpectedWindow(FVector(1234.0, -777.0, 555.0), Envelope, Capacity, 500.0f, 1000.0f);
	TestTrue(TEXT("Window valid"), Window.bValid);
	TestTrue(TEXT("Center snaps to the nearest XY cell multiples"),
		Window.SnappedCenterXY == FVector2D(1000.0, -1000.0));
	TestTrue(TEXT("Evaluation origin is (snapped XY, Z envelope center)"),
		Window.EvaluationOrigin == FVector(1000.0, -1000.0, 1000.0));
	TestTrue(TEXT("Live Z cells follow the envelope"), Window.LiveCells == FIntVector(4, 4, 4));

	const FVoxelPaintExpectedWindow HighCamera = MakeExpectedWindow(FVector(1234.0, -777.0, 99999.0), Envelope, Capacity, 500.0f, 1000.0f);
	TestTrue(TEXT("Camera Z never moves the origin"), HighCamera.EvaluationOrigin == Window.EvaluationOrigin);

	const FVoxelPaintExpectedWindow SameCell = MakeExpectedWindow(FVector(1100.0, -900.0, 0.0), Envelope, Capacity, 500.0f, 1000.0f);
	TestTrue(TEXT("A camera snapping to the same center reproduces the same window and origin"),
		SameCell.SnappedCenterXY == Window.SnappedCenterXY
		&& SameCell.EvaluationOrigin == Window.EvaluationOrigin);

	const FVoxelPaintExpectedWindow Moved = MakeExpectedWindow(FVector(1600.0, -1000.0, 0.0), Envelope, Capacity, 500.0f, 1000.0f);
	TestTrue(TEXT("The origin moves only when the snapped window moves"),
		Moved.SnappedCenterXY == FVector2D(1500.0, -1000.0)
		&& Moved.EvaluationOrigin == FVector(1500.0, -1000.0, 1000.0));

	const float GuardBandWidth = 800.0f;
	TestFalse(TEXT("Displacement exactly at the guard width does not consume"),
		IsGuardConsumed(Window, FVector(1800.0, -1000.0, 0.0), GuardBandWidth));
	TestTrue(TEXT("Displacement just past the guard width consumes"),
		IsGuardConsumed(Window, FVector(1800.5, -1000.0, 0.0), GuardBandWidth));
	TestTrue(TEXT("The guard test is per-axis Chebyshev (Y alone consumes)"),
		IsGuardConsumed(Window, FVector(1000.0, -1800.5, 0.0), GuardBandWidth));
	TestFalse(TEXT("A diagonal below the guard width on both axes does not consume"),
		IsGuardConsumed(Window, FVector(1799.0, -1799.0, 0.0), GuardBandWidth));
	TestFalse(TEXT("The deriving camera itself sits inside the guard"),
		IsGuardConsumed(Window, FVector(1234.0, -777.0, 555.0), GuardBandWidth));

	return true;
}
