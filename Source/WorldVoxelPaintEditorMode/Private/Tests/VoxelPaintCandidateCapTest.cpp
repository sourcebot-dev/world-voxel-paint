// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "VoxelPaintGridRenderer.h"
#include "VoxelPaintGridSetup.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintSceneUniformBuffer.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintCandidateCapTestPrivate
{
	static FVoxelPaintVolumeRecord MakeRecord(const FVector& Translation, const FVector3f& HalfExtent,
		const FVector& EvaluationOrigin)
	{
		FVoxelPaintPlacementState State;
		State.Rotation = FQuat::Identity;
		State.Translation = Translation;
		State.LocalHalfExtent = HalfExtent;
		State.Flags = WorldVoxelPaint::RecordFlag_Valid;
		return PackVolumeRecord(State, FVoxelPaintResolvedBinding(), EvaluationOrigin);
	}

	static FString LoadShaderSource(FAutomationTestBase& Test, const FString& RelativePath)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("WorldVoxelPaint"));
		if (!Plugin.IsValid())
		{
			Test.AddError(TEXT("WorldVoxelPaint plugin not found"));
			return FString();
		}
		const FString FullPath = Plugin->GetBaseDir() / RelativePath;
		FString Source;
		if (!FFileHelper::LoadFileToString(Source, *FullPath))
		{
			Test.AddError(FString::Printf(TEXT("Failed to read shader source '%s'"), *FullPath));
		}
		return Source;
	}

	static int32 CountOccurrences(const FString& Haystack, const FString& Needle)
	{
		int32 Count = 0;
		for (int32 Index = Haystack.Find(Needle, ESearchCase::CaseSensitive);
			Index != INDEX_NONE;
			Index = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 1))
		{
			++Count;
		}
		return Count;
	}

	// One canonicalized grid generation read back to the CPU.
	struct FGridReadbackResult
	{
		TArray<uint32> Headers;
		TArray<uint32> IndexList;
		uint32 ErrorFlag = 0;
		bool bExecuted = false;
	};

	static void RebuildAndReadback(const WorldVoxelPaint::FVoxelPaintGridCapacity& Capacity,
		const WorldVoxelPaint::FVoxelPaintGridBuildInput& Input,
		const TArray<FVoxelPaintVolumeRecord>& Records, FGridReadbackResult& OutResult)
	{
		TArray<FVector4f> Lanes;
		Lanes.SetNumZeroed(FMath::Max(Records.Num(), 1) * WorldVoxelPaint::RecordNumFloat4Lanes);
		if (Records.Num() > 0)
		{
			FMemory::Memcpy(Lanes.GetData(), Records.GetData(), Lanes.Num() * sizeof(FVector4f));
		}

		ENQUEUE_RENDER_COMMAND(WorldVoxelPaintCanonicalizeGpuCapTest)(
			[Capacity, Input, Lanes = MoveTemp(Lanes), &OutResult](FRHICommandListImmediate& RHICmdList)
			{
				FRHIGPUBufferReadback HeadersReadback(TEXT("WorldVoxelPaint.Test.HeadersReadback"));
				FRHIGPUBufferReadback ListReadback(TEXT("WorldVoxelPaint.Test.IndexListReadback"));
				FRHIGPUBufferReadback ErrorReadback(TEXT("WorldVoxelPaint.Test.ErrorFlagReadback"));
				bool bErrorFlagCopied = false;

				{
					FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("WorldVoxelPaint.CanonicalizeGpuCapTest"));
					FVoxelPaintGridRenderer Renderer;
					Renderer.Initialize(GraphBuilder, Capacity);

					const FRDGBufferRef VolumesBuffer = CreateStructuredBuffer(
						GraphBuilder, TEXT("WorldVoxelPaint.Test.Volumes"), Lanes);
					FRDGBufferRef ErrorFlagBuffer = nullptr;
					Renderer.Rebuild(GraphBuilder, Input, GraphBuilder.CreateSRV(VolumesBuffer), &ErrorFlagBuffer);

					FVoxelPaintSceneParameters Params;
					Renderer.Register(GraphBuilder, Params);
					AddEnqueueCopyPass(GraphBuilder, &HeadersReadback,
						Params.GridHeaders->Desc.Buffer, Capacity.CCap * sizeof(uint32));
					AddEnqueueCopyPass(GraphBuilder, &ListReadback,
						Params.GridIndexList->Desc.Buffer, Capacity.KCap * sizeof(uint32));
					if (ErrorFlagBuffer)
					{
						AddEnqueueCopyPass(GraphBuilder, &ErrorReadback, ErrorFlagBuffer, sizeof(uint32));
						bErrorFlagCopied = true;
					}
					GraphBuilder.Execute();
				}

				RHICmdList.BlockUntilGPUIdle();

				OutResult.Headers.SetNumZeroed(Capacity.CCap);
				if (const void* Data = HeadersReadback.Lock(Capacity.CCap * sizeof(uint32)))
				{
					FMemory::Memcpy(OutResult.Headers.GetData(), Data, Capacity.CCap * sizeof(uint32));
					HeadersReadback.Unlock();
				}
				OutResult.IndexList.SetNumZeroed(Capacity.KCap);
				if (const void* Data = ListReadback.Lock(Capacity.KCap * sizeof(uint32)))
				{
					FMemory::Memcpy(OutResult.IndexList.GetData(), Data, Capacity.KCap * sizeof(uint32));
					ListReadback.Unlock();
				}
				if (bErrorFlagCopied)
				{
					if (const void* Data = ErrorReadback.Lock(sizeof(uint32)))
					{
						FMemory::Memcpy(&OutResult.ErrorFlag, Data, sizeof(uint32));
						ErrorReadback.Unlock();
					}
				}
				OutResult.bExecuted = true;
			});
		FlushRenderingCommands();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintCandidateCap_HeaderContract,
	"Game.WorldVoxelPaint.Unit.Grid.CandidateCapHeaderContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintCandidateCap_HeaderContract::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;

	TestEqual(TEXT("Per-cell evaluation cap is 32"), MaxEvaluatedCandidatesPerCell, 32u);
	TestTrue(TEXT("Cap fits the 9-bit header count field"),
		MaxEvaluatedCandidatesPerCell <= GridHeaderCountMask);
	TestTrue(TEXT("Cap is far below the frontier size (distinct concepts)"),
		MaxEvaluatedCandidatesPerCell < static_cast<uint32>(MaxSelectedPlacements));

	// Every published count the canonicalize pass can now emit round-trips exactly.
	const uint32 Counts[] = { 0u, 1u, MaxEvaluatedCandidatesPerCell - 1u, MaxEvaluatedCandidatesPerCell };
	for (const uint32 Count : Counts)
	{
		const uint32 Offset = 977u;
		uint32 UnpackedOffset = MAX_uint32;
		uint32 UnpackedCount = MAX_uint32;
		UnpackGridHeader(PackGridHeader(Offset, Count), UnpackedOffset, UnpackedCount);
		TestTrue(FString::Printf(TEXT("Offset/Count (%u, %u) round-trips"), Offset, Count),
			UnpackedOffset == Offset && UnpackedCount == Count);
	}

	// The 9-bit representational headroom above the cap is unchanged: counts still mask, and
	// the offset field still spans the full K_cap ceiling.
	uint32 UnpackedOffset = 0;
	uint32 UnpackedCount = 0;
	UnpackGridHeader(PackGridHeader(0u, 600u), UnpackedOffset, UnpackedCount);
	TestEqual(TEXT("Over-mask counts keep masking to 9 bits"), UnpackedCount, 600u & GridHeaderCountMask);
	const uint32 MaxOffset = GridCandidateCapCeiling - MaxEvaluatedCandidatesPerCell;
	UnpackGridHeader(PackGridHeader(MaxOffset, MaxEvaluatedCandidatesPerCell), UnpackedOffset, UnpackedCount);
	TestTrue(TEXT("Ceiling-adjacent offset with a capped count round-trips"),
		UnpackedOffset == MaxOffset && UnpackedCount == MaxEvaluatedCandidatesPerCell);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintCandidateCap_ShaderMirrors,
	"Game.WorldVoxelPaint.Unit.Grid.ShaderMirrorConsistency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintCandidateCap_ShaderMirrors::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;
	using namespace VoxelPaintCandidateCapTestPrivate;

	const FString Sample = LoadShaderSource(*this, TEXT("Shaders/VoxelPaintSample.ush"));
	const FString GridBuild = LoadShaderSource(*this, TEXT("Shaders/VoxelPaintGridBuild.usf"));
	const FString Heatmap = LoadShaderSource(*this, TEXT("Shaders/VoxelPaintHeatmap.usf"));
	if (Sample.IsEmpty() || GridBuild.IsEmpty() || Heatmap.IsEmpty())
	{
		return false;
	}

	// The comment-only "mirrors" convention has no compiler backstop, so pin every hand-mirrored
	// literal to its C++ constant here.
	TestTrue(TEXT("Sample.ush mirrors MaxEvaluatedCandidatesPerCell"),
		Sample.Contains(FString::Printf(TEXT("#define WORLDVOXELPAINT_MAX_EVALUATED_CANDIDATES %uu"),
			MaxEvaluatedCandidatesPerCell)));
	TestTrue(TEXT("Sample.ush mirrors GridHeaderCountBits"),
		Sample.Contains(FString::Printf(TEXT("#define WORLDVOXELPAINT_HEADER_COUNT_BITS %u "),
			GridHeaderCountBits)));
	TestTrue(TEXT("Sample.ush mirrors GridHeaderCountMask"),
		Sample.Contains(FString::Printf(TEXT("#define WORLDVOXELPAINT_HEADER_COUNT_MASK 0x%Xu"),
			GridHeaderCountMask)));

	// The material loop's compile-time bound is the per-cell cap - nothing may rebind it to the
	// placement frontier (the old WORLDVOXELPAINT_MAX_VOLUMES=512 mistake).
	TestTrue(TEXT("Material loop clamps to the per-cell cap"),
		Sample.Contains(TEXT("min(SegmentCount, WORLDVOXELPAINT_MAX_EVALUATED_CANDIDATES)")));
	TestEqual(TEXT("The retired MAX_VOLUMES macro is gone"),
		CountOccurrences(Sample, TEXT("WORLDVOXELPAINT_MAX_VOLUMES")), 0);
	TestEqual(TEXT("Material eval never references the placement frontier size"),
		CountOccurrences(Sample, TEXT("MaxSelectedPlacements")), 0);
	TestEqual(TEXT("No literal frontier size in material eval"),
		CountOccurrences(Sample, TEXT("4096")), 0);

	// Canonicalization walks at most the cap and no longer treats per-cell overflow as an error;
	// the two genuine fatals (node K_cap exhaustion, segment allocation overflow) survive.
	TestTrue(TEXT("Canonicalize chain walk is bounded by the per-cell cap"),
		GridBuild.Contains(TEXT("Count < WORLDVOXELPAINT_MAX_EVALUATED_CANDIDATES")));
	TestEqual(TEXT("Per-cell overflow no longer raises the global error flag"),
		CountOccurrences(GridBuild, TEXT("Count > WORLDVOXELPAINT_HEADER_COUNT_MASK")), 0);
	TestTrue(TEXT("Node allocator K_cap exhaustion stays fatal"),
		GridBuild.Contains(TEXT("Node >= KCap")));
	TestTrue(TEXT("Segment allocation overflow stays fatal"),
		GridBuild.Contains(TEXT("Offset + Count > KCap")));
	TestEqual(TEXT("Exactly the two genuine fatals write the error flag"),
		CountOccurrences(GridBuild, TEXT("RWErrorFlag[0] = 1u")), 2);

	TestTrue(TEXT("Heatmap mirrors MaxEvaluatedCandidatesPerCell"),
		Heatmap.Contains(FString::Printf(TEXT("#define WVP_HEATMAP_MAX_EVALUATED_CANDIDATES %uu"),
			MaxEvaluatedCandidatesPerCell)));
	TestTrue(TEXT("Heatmap warns at the cap"),
		Heatmap.Contains(TEXT("Count >= WVP_HEATMAP_MAX_EVALUATED_CANDIDATES")));
	TestTrue(TEXT("Heatmap mirrors GridHeaderCountMask"),
		Heatmap.Contains(FString::Printf(TEXT("#define WVP_HEATMAP_HEADER_COUNT_MASK 0x%Xu"),
			GridHeaderCountMask)));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintCandidateCap_CanonicalizeGpu,
	"Game.WorldVoxelPaint.Unit.Grid.CanonicalizeGpuCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintCandidateCap_CanonicalizeGpu::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;
	using namespace VoxelPaintCandidateCapTestPrivate;

	if (GUsingNullRHI || !GIsRHIInitialized)
	{
		// The -nullrhi suite cannot execute compute work or read anything back; the real-RHI
		// run (no -nullrhi) is the validating pass for this test.
		AddInfo(TEXT("Skipped: canonicalization requires a real RHI (run without -nullrhi)."));
		return true;
	}

	FVoxelPaintZEnvelope Envelope;
	Envelope.ZMin = -500.0;
	Envelope.ZMax = 500.0;

	// Case A: one 8x8x1 window with stacks of 0/1/31/32/33/200 co-located volumes in distinct
	// cells - every stack lands whole in exactly one cell (half extent 100 vs 500x500x1000 cells).
	{
		FVoxelPaintGridCapacity Capacity;
		Capacity.WindowCellsCap = FIntVector(8, 8, 1);
		Capacity.CCap = 64;
		Capacity.KCap = 1024;
		Capacity.bValid = true;

		const FVoxelPaintExpectedWindow Window = MakeExpectedWindow(
			FVector::ZeroVector, Envelope, Capacity, 500.0f, 1000.0f);
		if (!TestTrue(TEXT("Stack window valid"), Window.bValid && Window.LiveCells == FIntVector(8, 8, 1)))
		{
			return false;
		}

		const auto CellCenter = [&Window](int32 CellX, int32 CellY)
		{
			return Window.EvaluationOrigin
				+ FVector(-2000.0 + (CellX + 0.5) * 500.0, -2000.0 + (CellY + 0.5) * 500.0, 0.0);
		};
		const auto CellIndex = [](int32 CellX, int32 CellY) { return CellX + 8 * CellY; };

		struct FStack
		{
			int32 CellX;
			int32 CellY;
			int32 NumVolumes;
			int32 FirstRecord = 0;
		};
		TArray<FStack> Stacks = {
			{ 0, 0, 1 }, { 2, 0, 31 }, { 4, 0, 32 }, { 6, 0, 33 }, { 0, 2, 200 } };

		TArray<FVoxelPaintVolumeRecord> Records;
		for (FStack& Stack : Stacks)
		{
			Stack.FirstRecord = Records.Num();
			for (int32 Index = 0; Index < Stack.NumVolumes; ++Index)
			{
				Records.Add(MakeRecord(CellCenter(Stack.CellX, Stack.CellY), FVector3f(100.0f),
					Window.EvaluationOrigin));
			}
		}

		FVoxelPaintGridBuildInput Input;
		BuildGridInput(Records, Window, Capacity, 500.0f, 1000.0f, Input);
		if (!TestTrue(TEXT("Stack input valid under K_cap"), Input.bValid))
		{
			return false;
		}

		FGridReadbackResult Result;
		RebuildAndReadback(Capacity, Input, Records, Result);
		if (!TestTrue(TEXT("Stack rebuild executed"), Result.bExecuted))
		{
			return false;
		}
		TestEqual(TEXT("No fatal error flag for truncated cells"), Result.ErrorFlag, 0u);

		TSet<int32> StackCells;
		for (const FStack& Stack : Stacks)
		{
			StackCells.Add(CellIndex(Stack.CellX, Stack.CellY));
			uint32 Offset = 0;
			uint32 Count = 0;
			UnpackGridHeader(Result.Headers[CellIndex(Stack.CellX, Stack.CellY)], Offset, Count);

			const uint32 ExpectedCount = FMath::Min<uint32>(Stack.NumVolumes, MaxEvaluatedCandidatesPerCell);
			TestEqual(FString::Printf(TEXT("Stack of %d publishes Count = min(N, cap)"), Stack.NumVolumes),
				Count, ExpectedCount);
			if (!TestTrue(FString::Printf(TEXT("Stack of %d publishes an in-range segment"), Stack.NumVolumes),
				Count > 0u && Offset + Count <= static_cast<uint32>(Result.IndexList.Num())))
			{
				continue;
			}

			uint32 Previous = 0;
			for (uint32 Entry = 0; Entry < Count; ++Entry)
			{
				const uint32 RecordIndex = Result.IndexList[Offset + Entry];
				TestTrue(FString::Printf(TEXT("Stack of %d entry %u is a record of this stack"),
						Stack.NumVolumes, Entry),
					RecordIndex >= static_cast<uint32>(Stack.FirstRecord)
						&& RecordIndex < static_cast<uint32>(Stack.FirstRecord + Stack.NumVolumes));
				if (Entry > 0)
				{
					TestTrue(FString::Printf(TEXT("Stack of %d entry %u ascends"), Stack.NumVolumes, Entry),
						RecordIndex > Previous);
				}
				Previous = RecordIndex;
			}
			if (Stack.NumVolumes <= static_cast<int32>(MaxEvaluatedCandidatesPerCell))
			{
				// At or under the cap the full set publishes deterministically: exactly
				// FirstRecord..FirstRecord+N-1 in ascending record order.
				bool bComplete = Count == static_cast<uint32>(Stack.NumVolumes);
				for (uint32 Entry = 0; bComplete && Entry < Count; ++Entry)
				{
					bComplete = Result.IndexList[Offset + Entry] == static_cast<uint32>(Stack.FirstRecord) + Entry;
				}
				TestTrue(FString::Printf(TEXT("Stack of %d publishes its complete ascending set"),
					Stack.NumVolumes), bComplete);
			}
		}

		for (int32 Cell = 0; Cell < static_cast<int32>(Capacity.CCap); ++Cell)
		{
			if (!StackCells.Contains(Cell) && Result.Headers[Cell] != 0u)
			{
				AddError(FString::Printf(TEXT("Cell %d unexpectedly published header 0x%08X"),
					Cell, Result.Headers[Cell]));
				break;
			}
		}
	}

	// Case B: 4096 records in 4096 distinct cells of a 64x64x1 window - the highest record
	// index the raised frontier can produce flows through discover/canonicalize/index list.
	{
		FVoxelPaintGridCapacity Capacity;
		Capacity.WindowCellsCap = FIntVector(64, 64, 1);
		Capacity.CCap = 4096;
		Capacity.KCap = 8192;
		Capacity.bValid = true;

		const FVoxelPaintExpectedWindow Window = MakeExpectedWindow(
			FVector::ZeroVector, Envelope, Capacity, 500.0f, 1000.0f);
		if (!TestTrue(TEXT("Spread window valid"), Window.bValid && Window.LiveCells == FIntVector(64, 64, 1)))
		{
			return false;
		}

		TArray<FVoxelPaintVolumeRecord> Records;
		Records.Reserve(MaxSelectedPlacements);
		for (int32 Index = 0; Index < MaxSelectedPlacements; ++Index)
		{
			const FVector Center = Window.EvaluationOrigin + FVector(
				-16000.0 + ((Index % 64) + 0.5) * 500.0,
				-16000.0 + ((Index / 64) + 0.5) * 500.0,
				0.0);
			Records.Add(MakeRecord(Center, FVector3f(100.0f), Window.EvaluationOrigin));
		}

		FVoxelPaintGridBuildInput Input;
		BuildGridInput(Records, Window, Capacity, 500.0f, 1000.0f, Input);
		if (!TestTrue(TEXT("Spread input valid under K_cap"), Input.bValid
			&& Input.TotalThreads == static_cast<uint32>(MaxSelectedPlacements)))
		{
			return false;
		}

		FGridReadbackResult Result;
		RebuildAndReadback(Capacity, Input, Records, Result);
		if (!TestTrue(TEXT("Spread rebuild executed"), Result.bExecuted))
		{
			return false;
		}
		TestEqual(TEXT("No error flag for the full-frontier spread"), Result.ErrorFlag, 0u);

		int32 PublishedCells = 0;
		bool bEveryCellHoldsItsRecord = true;
		for (int32 Cell = 0; Cell < static_cast<int32>(Capacity.CCap); ++Cell)
		{
			uint32 Offset = 0;
			uint32 Count = 0;
			UnpackGridHeader(Result.Headers[Cell], Offset, Count);
			if (Count != 1u || Offset >= static_cast<uint32>(Result.IndexList.Num()))
			{
				bEveryCellHoldsItsRecord = false;
				break;
			}
			++PublishedCells;
			if (Result.IndexList[Offset] != static_cast<uint32>(Cell))
			{
				bEveryCellHoldsItsRecord = false;
				break;
			}
		}
		TestTrue(TEXT("All 4096 cells publish exactly their own record (record index 4095 included)"),
			bEveryCellHoldsItsRecord && PublishedCells == MaxSelectedPlacements);
	}

	return true;
}
