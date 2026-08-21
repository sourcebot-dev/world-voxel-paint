// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "VoxelPaintRecord.h"
#include "WorldVoxelPaintConstants.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintRecords_PackingRoundTrip,
	"Game.WorldVoxelPaint.Unit.Records.PackingRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintRecords_PackingRoundTrip::RunTest(const FString& Parameters)
{
	FVoxelPaintPlacementState State;
	State.Rotation = FQuat(FVector(0.3, -0.5, 0.8).GetSafeNormal(), 1.234).GetNormalized();
	State.Translation = FVector(1e6, -2e6, 3e4);
	State.LocalHalfExtent = FVector3f(120.0f, 340.0f, 560.0f);
	State.TexelSize = FVector3f(8.5f, 24.25f, 40.0f);
	State.StorageMin = FVector3f(-120.0f, -300.0f, -512.0f);
	State.BrickCounts = FIntVector(5, 6, 7);
	State.BlendCoefficients = FVector4f(1.0f, -1.0f, 1.0f, -1.0f);
	State.Flags = 0xDEADBEEFu;

	FVoxelPaintResolvedBinding Binding;
	Binding.PageTableBaseIndex = 0x01000001u;
	Binding.SessionSelector = 0xFFFFFFFFu;

	const FVector EvaluationOrigin(1e6, -2e6, 0.0);
	const FVoxelPaintVolumeRecord Record = PackVolumeRecord(State, Binding, EvaluationOrigin);

	uint32 Words[WorldVoxelPaint::RecordNumFloat4Lanes * 4];
	static_assert(sizeof(Words) == sizeof(FVoxelPaintVolumeRecord));
	FMemory::Memcpy(Words, &Record, sizeof(Record));

	TestTrue(TEXT("Lane1.w (offset 28) preserves Flags bits"), Words[7] == 0xDEADBEEFu);
	TestTrue(TEXT("Lane2.w (offset 44) preserves PageTableBaseIndex bits"), Words[11] == 0x01000001u);
	TestTrue(TEXT("Lane4.w (offset 76) preserves SessionSelector bits"), Words[19] == 0xFFFFFFFFu);
	TestTrue(TEXT("Lane5.w (offset 92) carries BrickCountX"), Words[23] == 5u);
	TestTrue(TEXT("Lane6.w (offset 108) packs BrickCountY | BrickCountZ << 16"), Words[27] == (6u | (7u << 16)));

	TestEqual(TEXT("VolumeCenter.X is origin-relative"), Record.VolumeCenter.X, 0.0f, 1e-3f);
	TestEqual(TEXT("VolumeCenter.Y is origin-relative"), Record.VolumeCenter.Y, 0.0f, 1e-3f);
	TestEqual(TEXT("VolumeCenter.Z is origin-relative"), Record.VolumeCenter.Z, 30000.0f, 1e-3f);

	const FVector Probe(17.0, -4.0, 9.0);
	const FVector3f ExpectedLocal = FVector3f(State.Rotation.Inverse().RotateVector(Probe));
	const FVector3f RecordLocal = Record.WorldToVolumeRotation.RotateVector(FVector3f(Probe));
	TestEqual(TEXT("Record rotation maps world to volume-local (X)"), RecordLocal.X, ExpectedLocal.X, 1e-2f);
	TestEqual(TEXT("Record rotation maps world to volume-local (Y)"), RecordLocal.Y, ExpectedLocal.Y, 1e-2f);
	TestEqual(TEXT("Record rotation maps world to volume-local (Z)"), RecordLocal.Z, ExpectedLocal.Z, 1e-2f);

	const FVector3f RoundTrip = FQuat4f(State.Rotation).RotateVector(RecordLocal);
	TestEqual(TEXT("Placement rotation inverts the record rotation (X)"), RoundTrip.X, static_cast<float>(Probe.X), 1e-2f);
	TestEqual(TEXT("Placement rotation inverts the record rotation (Y)"), RoundTrip.Y, static_cast<float>(Probe.Y), 1e-2f);
	TestEqual(TEXT("Placement rotation inverts the record rotation (Z)"), RoundTrip.Z, static_cast<float>(Probe.Z), 1e-2f);

	TestTrue(TEXT("LocalHalfExtent copied verbatim"), Record.LocalHalfExtent == State.LocalHalfExtent);
	TestTrue(TEXT("TexelSize copied verbatim"), Record.TexelSize == State.TexelSize);
	TestTrue(TEXT("StorageMin copied verbatim"), Record.StorageMin == State.StorageMin);
	TestTrue(TEXT("BlendCoefficients copied verbatim"), Record.BlendCoefficients == State.BlendCoefficients);

	const FVector3f ExpectedRcp(1.0f / State.TexelSize.X, 1.0f / State.TexelSize.Y, 1.0f / State.TexelSize.Z);
	TestTrue(TEXT("TexelSizeRcp is the componentwise reciprocal of TexelSize"), Record.TexelSizeRcp == ExpectedRcp);

	return true;
}
