// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintBrushMath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPick_BrushMath,
	"Game.WorldVoxelPaint.Unit.Pick.BrushMath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPick_BrushMath::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	{
		TestTrue(TEXT("Radius: distance equal to radius passes (boundary inclusive)"),
			PassesRadius(10.0f, 10.0f));
		TestTrue(TEXT("Radius: distance inside passes"), PassesRadius(0.0f, 10.0f));
		TestFalse(TEXT("Radius: distance beyond radius rejects"), PassesRadius(10.0001f, 10.0f));
		TestTrue(TEXT("Radius: zero distance at zero radius passes"), PassesRadius(0.0f, 0.0f));
		TestFalse(TEXT("Radius: any distance at zero radius rejects"), PassesRadius(0.0001f, 0.0f));
	}

	{
		TestTrue(TEXT("Flatten: pct 0 window equals the radius (positive depth)"),
			PassesFlatten(100.0f, 100.0f, 0.0f, 2.0f));
		TestTrue(TEXT("Flatten: pct 0 window equals the radius (negative depth)"),
			PassesFlatten(-100.0f, 100.0f, 0.0f, 2.0f));
		TestFalse(TEXT("Flatten: pct 0 depth beyond the radius rejects"),
			PassesFlatten(100.5f, 100.0f, 0.0f, 2.0f));

		TestTrue(TEXT("Flatten: pct 0.75 window is radius * 0.25"),
			PassesFlatten(25.0f, 100.0f, 0.75f, 2.0f));
		TestTrue(TEXT("Flatten: pct 0.75 window is sign-symmetric"),
			PassesFlatten(-25.0f, 100.0f, 0.75f, 2.0f));
		TestFalse(TEXT("Flatten: pct 0.75 depth beyond the window rejects"),
			PassesFlatten(25.1f, 100.0f, 0.75f, 2.0f));

		TestTrue(TEXT("Flatten: pct 1 clamps the window to MinFlattenDepth"),
			PassesFlatten(2.0f, 100.0f, 1.0f, 2.0f));
		TestFalse(TEXT("Flatten: pct 1 depth beyond MinFlattenDepth rejects"),
			PassesFlatten(2.5f, 100.0f, 1.0f, 2.0f));
		TestTrue(TEXT("Flatten: MinFlattenDepth clamp engages below pct 1 when it dominates"),
			PassesFlatten(1.5f, 100.0f, 0.99f, 2.0f));

		TestTrue(TEXT("Flatten: pct above 1 clamps to 1"),
			PassesFlatten(2.0f, 100.0f, 1.5f, 2.0f));
		TestFalse(TEXT("Flatten: pct above 1 keeps the MinFlattenDepth window"),
			PassesFlatten(2.5f, 100.0f, 1.5f, 2.0f));
		TestTrue(TEXT("Flatten: negative pct clamps to 0 (full-radius window)"),
			PassesFlatten(100.0f, 100.0f, -1.0f, 2.0f));
	}

	{
		const FVector3f XAxis(1.0f, 0.0f, 0.0f);
		const FVector3f YAxis(0.0f, 1.0f, 0.0f);
		const FVector3f NegXAxis(-1.0f, 0.0f, 0.0f);
		const FVector3f Zero(0.0f, 0.0f, 0.0f);

		TestTrue(TEXT("Angle: identical normals pass at tolerance 0"),
			PassesAngle(XAxis, XAxis, 0.0f));
		TestTrue(TEXT("Angle: perpendicular normals pass at tolerance 90"),
			PassesAngle(YAxis, XAxis, 90.0f));
		TestFalse(TEXT("Angle: opposing normals reject at tolerance 90"),
			PassesAngle(NegXAxis, XAxis, 90.0f));
		TestTrue(TEXT("Angle: near-opposing normals pass at tolerance 180"),
			PassesAngle(FVector3f(-1.0f, 0.01f, 0.0f), XAxis, 180.0f));

		const float Cos449 = FMath::Cos(FMath::DegreesToRadians(44.9f));
		const float Sin449 = FMath::Sin(FMath::DegreesToRadians(44.9f));
		const float Cos451 = FMath::Cos(FMath::DegreesToRadians(45.1f));
		const float Sin451 = FMath::Sin(FMath::DegreesToRadians(45.1f));
		TestTrue(TEXT("Angle: 44.9 degrees passes at tolerance 45"),
			PassesAngle(FVector3f(Cos449, Sin449, 0.0f), XAxis, 45.0f));
		TestFalse(TEXT("Angle: 45.1 degrees rejects at tolerance 45"),
			PassesAngle(FVector3f(Cos451, Sin451, 0.0f), XAxis, 45.0f));

		TestTrue(TEXT("Angle: zero surface normal passes (degenerate no-hit rule)"),
			PassesAngle(Zero, XAxis, 0.0f));
		TestTrue(TEXT("Angle: zero brush normal passes"),
			PassesAngle(XAxis, Zero, 0.0f));
		TestTrue(TEXT("Angle: sub-normalizable surface normal passes as degenerate"),
			PassesAngle(FVector3f(1.0e-6f, 0.0f, 0.0f), XAxis, 0.0f));
		TestTrue(TEXT("Angle: unnormalized inputs are safe-normalized"),
			PassesAngle(FVector3f(0.0f, 0.0f, 10.0f), FVector3f(0.0f, 0.0f, 1.0f), 5.0f));
	}

	{
		TestEqual(TEXT("Falloff: exact 1 at the center"),
			FalloffWeight(0.0f, 10.0f, 0.5f), 1.0f, 0.0f);
		TestEqual(TEXT("Falloff: exact 1 at the hotspot edge (plateau)"),
			FalloffWeight(5.0f, 10.0f, 0.5f), 1.0f, 0.0f);
		TestEqual(TEXT("Falloff: linear 0.5 midway between hotspot and radius"),
			FalloffWeight(7.5f, 10.0f, 0.5f), 0.5f, 0.0f);
		TestEqual(TEXT("Falloff: exact 0 at the radius"),
			FalloffWeight(10.0f, 10.0f, 0.5f), 0.0f, 0.0f);
		TestEqual(TEXT("Falloff: exact 0 beyond the radius"),
			FalloffWeight(15.0f, 10.0f, 0.5f), 0.0f, 0.0f);

		TestEqual(TEXT("Falloff: hotspot 0 is linear from the center"),
			FalloffWeight(5.0f, 10.0f, 0.0f), 0.5f, 0.0f);
		TestEqual(TEXT("Falloff: hotspot 1 plateaus to the radius"),
			FalloffWeight(10.0f, 10.0f, 1.0f), 1.0f, 0.0f);
		TestEqual(TEXT("Falloff: hotspot 1 drops to 0 just past the radius (denominator floor)"),
			FalloffWeight(10.001f, 10.0f, 1.0f), 0.0f, 0.0f);
		TestEqual(TEXT("Falloff: hotspot above 1 clamps to 1"),
			FalloffWeight(9.9f, 10.0f, 5.0f), 1.0f, 0.0f);
		TestEqual(TEXT("Falloff: zero radius keeps the center at 1"),
			FalloffWeight(0.0f, 0.0f, 0.5f), 1.0f, 0.0f);
		TestEqual(TEXT("Falloff: zero radius is 0 away from the center"),
			FalloffWeight(0.001f, 0.0f, 0.5f), 0.0f, 0.0f);
	}

	return true;
}
