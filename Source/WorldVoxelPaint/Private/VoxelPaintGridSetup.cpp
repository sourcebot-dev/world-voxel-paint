// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintGridSetup.h"

#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "VoxelPaintDeveloperSettings.h"
#include "VoxelPaintWorldSettings.h"
#include "WorldVoxelPaintConstants.h"

namespace WorldVoxelPaint
{

namespace Private
{

static uint64 SaturatingAdd(uint64 A, uint64 B)
{
	return (A > MAX_uint64 - B) ? MAX_uint64 : A + B;
}

static uint64 SaturatingMul(uint64 A, uint64 B)
{
	return (B != 0 && A > MAX_uint64 / B) ? MAX_uint64 : A * B;
}

}

FSimpleMulticastDelegate& OnDeveloperSettingsChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

FVoxelPaintGridCapacity DeriveGridCapacity(const UVoxelPaintDeveloperSettings& Settings)
{
	FVoxelPaintGridCapacity Capacity;

	const double CellXY = Settings.GridCellSizeXY;
	const double CellZ = Settings.GridCellSizeZ;
	const double WindowRadius = static_cast<double>(Settings.EvaluationRadius)
		+ static_cast<double>(Settings.GuardBandWidth);
	if (CellXY <= 0.0 || CellZ <= 0.0 || WindowRadius <= 0.0
		|| Settings.DefaultGridZMax <= Settings.DefaultGridZMin)
	{
		return Capacity;
	}

	const int64 CellsXY = 2 * FMath::CeilToInt64(WindowRadius / CellXY);
	const int64 CellsZ = FMath::CeilToInt64(static_cast<double>(Settings.DefaultGridZMax) / CellZ)
		- FMath::FloorToInt64(static_cast<double>(Settings.DefaultGridZMin) / CellZ);
	if (CellsXY <= 0 || CellsZ <= 0)
	{
		return Capacity;
	}

	Capacity.WindowCellsCap = FIntVector(
		static_cast<int32>(FMath::Min<int64>(CellsXY, MAX_int32)),
		static_cast<int32>(FMath::Min<int64>(CellsXY, MAX_int32)),
		static_cast<int32>(FMath::Min<int64>(CellsZ, MAX_int32)));

	const uint64 CeilingU64 = static_cast<uint64>(GridCandidateCapCeiling);
	if (CellsXY > static_cast<int64>(GridCandidateCapCeiling)
		|| CellsZ > static_cast<int64>(GridCandidateCapCeiling))
	{
		Capacity.CCap = MAX_uint32;
		return Capacity;
	}
	uint64 CellCount = static_cast<uint64>(CellsXY) * static_cast<uint64>(CellsXY);
	if (CellCount <= CeilingU64)
	{
		CellCount *= static_cast<uint64>(CellsZ);
	}
	Capacity.CCap = static_cast<uint32>(FMath::Min<uint64>(CellCount, MAX_uint32));
	if (CellCount > CeilingU64)
	{
		return Capacity;
	}

	const uint32 Override = static_cast<uint32>(FMath::Max(Settings.GridCandidateCapOverride, 0));
	Capacity.KCap = Override == 0u
		? Capacity.CCap
		: FMath::Clamp(Override, Capacity.CCap, GridCandidateCapCeiling);
	Capacity.bValid = true;
	return Capacity;
}

FVoxelPaintZEnvelope ResolveZEnvelope(const UWorld* MasterWorld,
	const UVoxelPaintDeveloperSettings& Settings)
{
	double ZMin = Settings.DefaultGridZMin;
	double ZMax = Settings.DefaultGridZMax;
	if (MasterWorld)
	{
		if (AWorldSettings* WorldSettings = MasterWorld->GetWorldSettings(false, false))
		{
			const UVoxelPaintWorldSettings* WorldOverride =
				WorldSettings->GetAssetUserData<UVoxelPaintWorldSettings>();
			if (WorldOverride && WorldOverride->bOverrideGridZRange)
			{
				ZMin = WorldOverride->GridZMin;
				ZMax = WorldOverride->GridZMax;
			}
		}
	}

	FVoxelPaintZEnvelope Envelope;
	const double CellZ = Settings.GridCellSizeZ;
	if (CellZ > 0.0)
	{
		Envelope.ZMin = FMath::FloorToDouble(ZMin / CellZ) * CellZ;
		Envelope.ZMax = FMath::CeilToDouble(ZMax / CellZ) * CellZ;
	}
	else
	{
		Envelope.ZMin = ZMin;
		Envelope.ZMax = ZMax;
	}
	return Envelope;
}

FVoxelPaintExpectedWindow MakeExpectedWindow(const FVector& CameraLWC,
	const FVoxelPaintZEnvelope& ZEnvelope, const FVoxelPaintGridCapacity& Capacity,
	float CellSizeXY, float CellSizeZ)
{
	FVoxelPaintExpectedWindow Window;
	Window.ZEnvelope = ZEnvelope;
	if (!Capacity.bValid || CellSizeXY <= 0.0f || CellSizeZ <= 0.0f)
	{
		return Window;
	}

	const double CellXY = CellSizeXY;
	Window.SnappedCenterXY = FVector2D(
		FMath::RoundToDouble(CameraLWC.X / CellXY) * CellXY,
		FMath::RoundToDouble(CameraLWC.Y / CellXY) * CellXY);
	Window.EvaluationOrigin = FVector(
		Window.SnappedCenterXY.X,
		Window.SnappedCenterXY.Y,
		0.5 * (ZEnvelope.ZMin + ZEnvelope.ZMax));

	const int64 CellsZ = FMath::RoundToInt64(
		(ZEnvelope.ZMax - ZEnvelope.ZMin) / static_cast<double>(CellSizeZ));
	if (CellsZ <= 0)
	{
		return Window;
	}
	Window.LiveCells = FIntVector(
		Capacity.WindowCellsCap.X,
		Capacity.WindowCellsCap.Y,
		static_cast<int32>(FMath::Min<int64>(CellsZ, MAX_int32)));
	Window.bValid = true;
	return Window;
}

bool IsGuardConsumed(const FVoxelPaintExpectedWindow& Window,
	const FVector& CameraLWC, float GuardBandWidth)
{
	const double DistX = FMath::Abs(CameraLWC.X - Window.SnappedCenterXY.X);
	const double DistY = FMath::Abs(CameraLWC.Y - Window.SnappedCenterXY.Y);
	return FMath::Max(DistX, DistY) > static_cast<double>(GuardBandWidth);
}

void BuildGridInput(TConstArrayView<FVoxelPaintVolumeRecord> Records,
	const FVoxelPaintExpectedWindow& Window, const FVoxelPaintGridCapacity& Capacity,
	float CellSizeXY, float CellSizeZ, FVoxelPaintGridBuildInput& OutInput)
{
	OutInput = FVoxelPaintGridBuildInput();
	if (!Capacity.bValid || !Window.bValid || CellSizeXY <= 0.0f || CellSizeZ <= 0.0f)
	{
		return;
	}

	const FIntVector Dims = Window.LiveCells;
	OutInput.GridDims = Dims;
	OutInput.CellSize = FVector2f(CellSizeXY, CellSizeZ);
	OutInput.KCap = Capacity.KCap;

	const double HalfSpanX = 0.5 * static_cast<double>(Dims.X) * static_cast<double>(CellSizeXY);
	const double HalfSpanY = 0.5 * static_cast<double>(Dims.Y) * static_cast<double>(CellSizeXY);
	const double HalfSpanZ = 0.5 * (Window.ZEnvelope.ZMax - Window.ZEnvelope.ZMin);
	OutInput.GridOriginRel = FVector3f(
		static_cast<float>(-HalfSpanX), static_cast<float>(-HalfSpanY), static_cast<float>(-HalfSpanZ));

	const double WindowMin[3] = { -HalfSpanX, -HalfSpanY, -HalfSpanZ };
	const double WindowMax[3] = { HalfSpanX, HalfSpanY, HalfSpanZ };
	const double CellSizes[3] = { CellSizeXY, CellSizeXY, CellSizeZ };
	const int32 DimsPerAxis[3] = { Dims.X, Dims.Y, Dims.Z };

	uint64 EmittedPairs = 0;
	for (int32 RecordIndex = 0; RecordIndex < Records.Num(); ++RecordIndex)
	{
		const FVoxelPaintVolumeRecord& Record = Records[RecordIndex];
		if ((Record.Flags & RecordFlag_Valid) == 0u)
		{
			continue;
		}

		const FQuat4f VolumeToWorld = Record.WorldToVolumeRotation.Inverse();
		const FVector3f AxisX = VolumeToWorld.RotateVector(FVector3f(1.0f, 0.0f, 0.0f)).GetAbs();
		const FVector3f AxisY = VolumeToWorld.RotateVector(FVector3f(0.0f, 1.0f, 0.0f)).GetAbs();
		const FVector3f AxisZ = VolumeToWorld.RotateVector(FVector3f(0.0f, 0.0f, 1.0f)).GetAbs();
		const FVector3f WorldExtent = AxisX * Record.LocalHalfExtent.X
			+ AxisY * Record.LocalHalfExtent.Y
			+ AxisZ * Record.LocalHalfExtent.Z;

		const double BoxMin[3] = {
			static_cast<double>(Record.VolumeCenter.X) - static_cast<double>(WorldExtent.X),
			static_cast<double>(Record.VolumeCenter.Y) - static_cast<double>(WorldExtent.Y),
			static_cast<double>(Record.VolumeCenter.Z) - static_cast<double>(WorldExtent.Z) };
		const double BoxMax[3] = {
			static_cast<double>(Record.VolumeCenter.X) + static_cast<double>(WorldExtent.X),
			static_cast<double>(Record.VolumeCenter.Y) + static_cast<double>(WorldExtent.Y),
			static_cast<double>(Record.VolumeCenter.Z) + static_cast<double>(WorldExtent.Z) };

		if (BoxMin[2] > WindowMax[2] || BoxMax[2] < WindowMin[2])
		{
			++OutInput.NumOutsideZEnvelope;
			continue;
		}
		if (BoxMin[0] > WindowMax[0] || BoxMax[0] < WindowMin[0]
			|| BoxMin[1] > WindowMax[1] || BoxMax[1] < WindowMin[1])
		{
			continue;
		}

		FIntVector CellMin = FIntVector::ZeroValue;
		FIntVector CellDims = FIntVector::ZeroValue;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const int64 MinCell = FMath::Clamp<int64>(
				FMath::FloorToInt64((BoxMin[Axis] - WindowMin[Axis]) / CellSizes[Axis]),
				0, DimsPerAxis[Axis] - 1);
			const int64 MaxCell = FMath::Clamp<int64>(
				FMath::FloorToInt64((BoxMax[Axis] - WindowMin[Axis]) / CellSizes[Axis]),
				MinCell, DimsPerAxis[Axis] - 1);
			CellMin[Axis] = static_cast<int32>(MinCell);
			CellDims[Axis] = static_cast<int32>(MaxCell - MinCell + 1);
		}

		const uint64 CellCount = Private::SaturatingMul(
			Private::SaturatingMul(static_cast<uint64>(CellDims.X), static_cast<uint64>(CellDims.Y)),
			static_cast<uint64>(CellDims.Z));

		FVoxelPaintGridVolumeRange& Range = OutInput.Ranges.AddDefaulted_GetRef();
		Range.RecordIndex = static_cast<uint32>(RecordIndex);
		Range.CellMin = CellMin;
		Range.CellDims = CellDims;
		Range.FirstThread = static_cast<uint32>(FMath::Min<uint64>(EmittedPairs, MAX_uint32));
		Range.CellCount = static_cast<uint32>(FMath::Min<uint64>(CellCount, MAX_uint32));
		EmittedPairs = Private::SaturatingAdd(EmittedPairs, CellCount);
	}

	OutInput.TotalThreads = static_cast<uint32>(FMath::Min<uint64>(EmittedPairs, MAX_uint32));

	const uint64 LiveCellCount = static_cast<uint64>(Dims.X)
		* static_cast<uint64>(Dims.Y) * static_cast<uint64>(Dims.Z);
	OutInput.bValid = LiveCellCount <= static_cast<uint64>(Capacity.CCap)
		&& EmittedPairs <= static_cast<uint64>(Capacity.KCap);
}

uint32 PackGridHeader(uint32 Offset, uint32 Count)
{
	return (Offset << GridHeaderCountBits) | (Count & GridHeaderCountMask);
}

void UnpackGridHeader(uint32 Header, uint32& OutOffset, uint32& OutCount)
{
	OutOffset = Header >> GridHeaderCountBits;
	OutCount = Header & GridHeaderCountMask;
}

}
