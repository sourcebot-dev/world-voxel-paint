// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "Misc/AutomationTest.h"
#include "RHI.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "VoxelPaintExternalSampling.h"
#include "VoxelPaintRecord.h"
#include "VoxelPaintSampleParityTestShader.h"
#include "WorldVoxelPaintConstants.h"

namespace VoxelPaintPCGSampleParityTestPrivate
{

struct FParityLegInputs
{
	TArray<FVoxelPaintVolumeRecord> Records;
	TArray<uint32> RecordIndices;              // ascending record order (production contract)
	TArray<uint32> PageTable;                  // bound as-is when non-empty; else one absent entry
	TArray<FVector4f> Probes;                  // xyz = evaluation-origin-relative position
	FColor SessionColor = FColor(0, 0, 0, 0);
	bool bBindSessionTexture = false;          // uniform 7x7x7 PF_B8G8R8A8 when true (production format)
	FVector3f CameraCenter = FVector3f::ZeroVector;
	float EvaluationRadiusSq = 1.0e12f;
	bool bNeutralOnly = false;                 // production neutral bindings, untouched
};

struct FParityLegResult
{
	TArray<FVector4f> Weights;
	bool bExecuted = false;
};

static FVoxelPaintVolumeRecord MakeRecord(const FVector& Translation, const FVector3f& HalfExtent,
	const FVector4f& BlendCoefficients, const FVoxelPaintResolvedBinding& Binding)
{
	FVoxelPaintPlacementState State;
	State.Rotation = FQuat::Identity;
	State.Translation = Translation;
	State.LocalHalfExtent = HalfExtent;
	State.TexelSize = FVector3f(1.0f);
	State.StorageMin = -HalfExtent;
	State.BrickCounts = FIntVector(1, 1, 1);
	State.BlendCoefficients = BlendCoefficients;
	State.Flags = WorldVoxelPaint::RecordFlag_Valid;
	// Evaluation origin fixed at zero for the whole test: probe positions ARE origin-relative.
	return PackVolumeRecord(State, Binding, FVector::ZeroVector);
}

static void DispatchParityLeg(const FParityLegInputs& Inputs, FParityLegResult& Out)
{
	TArray<FVector4f> Lanes;
	Lanes.SetNumZeroed(FMath::Max(Inputs.Records.Num(), 1) * WorldVoxelPaint::RecordNumFloat4Lanes);
	if (Inputs.Records.Num() > 0)
	{
		FMemory::Memcpy(Lanes.GetData(), Inputs.Records.GetData(), Lanes.Num() * sizeof(FVector4f));
	}
	TArray<uint32> IndexData = Inputs.RecordIndices;
	if (IndexData.IsEmpty())
	{
		IndexData.Add(0u);   // buffers must be non-empty; NumRecordIndices carries the real count
	}
	TArray<uint32> PageTableData = Inputs.PageTable;
	if (PageTableData.IsEmpty())
	{
		PageTableData.Add(WorldVoxelPaint::PageEntryAbsent);
	}

	const int32 NumRecords = Inputs.Records.Num();
	const int32 NumIndices = Inputs.RecordIndices.Num();
	const int32 NumProbes = Inputs.Probes.Num();

	ENQUEUE_RENDER_COMMAND(WorldVoxelPaintSampleParityTest)(
		[Lanes = MoveTemp(Lanes), IndexData = MoveTemp(IndexData),
			PageTableData = MoveTemp(PageTableData), Probes = Inputs.Probes,
			SessionColor = Inputs.SessionColor, bBindSessionTexture = Inputs.bBindSessionTexture,
			bNeutralOnly = Inputs.bNeutralOnly, CameraCenter = Inputs.CameraCenter,
			EvaluationRadiusSq = Inputs.EvaluationRadiusSq,
			NumRecords, NumIndices, NumProbes, &Out](FRHICommandListImmediate& RHICmdList)
		{
			// Session texture is a plain RHI resource; create and fill it before the builder
			// opens (mirrors the pool renderer's raw-create + UpdateTexture3D upload path).
			FTextureRHIRef SessionTexture;
			if (bBindSessionTexture)
			{
				const int32 Resolution = WorldVoxelPaint::UniqueBrickSize;   // 1x1x1-brick volume
				const FRHITextureCreateDesc SessionDesc =
					FRHITextureCreateDesc::Create3D(TEXT("WorldVoxelPaint.Test.SessionTexture"),
						FIntVector(Resolution, Resolution, Resolution), PF_B8G8R8A8)
					.SetFlags(ETextureCreateFlags::ShaderResource)
					.SetInitialState(ERHIAccess::SRVMask);
				SessionTexture = RHICmdList.CreateTexture(SessionDesc);
				TArray<uint8> Texels;
				Texels.SetNumUninitialized(Resolution * Resolution * Resolution * 4);
				// PF_B8G8R8A8 byte order (B,G,R,A) - the production session texture's packing
				// (PackedSourceBGRA8): this leg also pins the B/R swizzle SampleLevel resolves.
				for (int32 TexelIndex = 0; TexelIndex < Resolution * Resolution * Resolution; ++TexelIndex)
				{
					Texels[TexelIndex * 4 + 0] = SessionColor.B;
					Texels[TexelIndex * 4 + 1] = SessionColor.G;
					Texels[TexelIndex * 4 + 2] = SessionColor.R;
					Texels[TexelIndex * 4 + 3] = SessionColor.A;
				}
				const FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0, Resolution, Resolution, Resolution);
				RHICmdList.UpdateTexture3D(SessionTexture, 0, Region,
					Resolution * 4, Resolution * Resolution * 4, Texels.GetData());
			}

			FRHIGPUBufferReadback WeightsReadback(TEXT("WorldVoxelPaint.Test.ParityWeights"));
			{
				FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("WorldVoxelPaint.SampleParityTest"));

				FVoxelPaintExternalSamplingParams Params;
				WorldVoxelPaint::BuildNeutralExternalSamplingParams(Params, GraphBuilder);
				if (!bNeutralOnly)
				{
					const FRDGBufferRef VolumesBuffer = CreateStructuredBuffer(
						GraphBuilder, TEXT("WorldVoxelPaint.Test.Volumes"), Lanes);
					Params.Volumes = GraphBuilder.CreateSRV(VolumesBuffer);
					Params.NumVolumes = static_cast<uint32>(NumRecords);
					const FRDGBufferRef PageTableBuffer = CreateStructuredBuffer(
						GraphBuilder, TEXT("WorldVoxelPaint.Test.PageTables"), PageTableData);
					Params.PageTables = GraphBuilder.CreateSRV(PageTableBuffer);
					Params.CameraCenter = CameraCenter;
					Params.EvaluationRadiusSq = EvaluationRadiusSq;
					if (SessionTexture.IsValid())
					{
						Params.SessionTexture = SessionTexture;
					}
				}

				const FRDGBufferRef IndexBuffer = CreateStructuredBuffer(
					GraphBuilder, TEXT("WorldVoxelPaint.Test.RecordIndices"), IndexData);
				const FRDGBufferRef ProbeBuffer = CreateStructuredBuffer(
					GraphBuilder, TEXT("WorldVoxelPaint.Test.Probes"), Probes);
				const FRDGBufferRef WeightsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), NumProbes),
					TEXT("WorldVoxelPaint.Test.OutWeights"));

				FVoxelPaintSampleParityCS::FParameters* PassParameters =
					GraphBuilder.AllocParameters<FVoxelPaintSampleParityCS::FParameters>();
				PassParameters->Volumes = Params.Volumes;
				PassParameters->PageTables = Params.PageTables;
				PassParameters->BrickPool = Params.BrickPool;
				PassParameters->SessionTexture = Params.SessionTexture;
				PassParameters->PoolSampler = Params.PoolSampler;
				PassParameters->RecordIndices = GraphBuilder.CreateSRV(IndexBuffer);
				PassParameters->ProbePositions = GraphBuilder.CreateSRV(ProbeBuffer);
				PassParameters->OutWeights = GraphBuilder.CreateUAV(WeightsBuffer);
				PassParameters->NumRecordIndices = static_cast<uint32>(NumIndices);
				PassParameters->NumVolumes = Params.NumVolumes;
				PassParameters->NumProbes = static_cast<uint32>(NumProbes);
				PassParameters->BrickSize = Params.BrickSize;
				PassParameters->UniqueBrickSize = Params.UniqueBrickSize;
				PassParameters->PoolDimsInBricks = Params.PoolDimsInBricks;
				PassParameters->PoolTexelExtentRcp = Params.PoolTexelExtentRcp;
				PassParameters->CameraCenter = Params.CameraCenter;
				PassParameters->EvaluationRadiusSq = Params.EvaluationRadiusSq;

				TShaderMapRef<FVoxelPaintSampleParityCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("WorldVoxelPaint.ParitySample"),
					Shader, PassParameters,
					FComputeShaderUtils::GetGroupCount(NumProbes, FVoxelPaintSampleParityCS::ThreadGroupSize));

				AddEnqueueCopyPass(GraphBuilder, &WeightsReadback, WeightsBuffer,
					NumProbes * sizeof(FVector4f));
				GraphBuilder.Execute();
			}
			RHICmdList.BlockUntilGPUIdle();

			Out.Weights.SetNumZeroed(NumProbes);
			if (const void* Data = WeightsReadback.Lock(NumProbes * sizeof(FVector4f)))
			{
				FMemory::Memcpy(Out.Weights.GetData(), Data, NumProbes * sizeof(FVector4f));
				WeightsReadback.Unlock();
			}
			Out.bExecuted = true;
		});
	FlushRenderingCommands();
}

static bool NearlyEqual(const FVector4f& A, const FVector4f& B, float Tolerance)
{
	return FMath::Abs(A.X - B.X) <= Tolerance && FMath::Abs(A.Y - B.Y) <= Tolerance
		&& FMath::Abs(A.Z - B.Z) <= Tolerance && FMath::Abs(A.W - B.W) <= Tolerance;
}

} // namespace VoxelPaintPCGSampleParityTestPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPCG_SampleParity,
	"Game.WorldVoxelPaint.Unit.PCG.SampleParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPCG_SampleParity::RunTest(const FString& Parameters)
{
	using namespace WorldVoxelPaint;
	using namespace VoxelPaintPCGSampleParityTestPrivate;

	if (GUsingNullRHI || !GIsRHIInitialized)
	{
		AddInfo(TEXT("Skipped: GPU sampling parity requires a real RHI (run without -nullrhi)."));
		return true;
	}

	const float Tolerance = 1.0e-6f;
	const FVector4f Zero(0.0f, 0.0f, 0.0f, 0.0f);
	const FColor SessionColor(51, 102, 153, 204);
	const FVector4f SessionValue(51.0f / 255.0f, 102.0f / 255.0f, 153.0f / 255.0f, 204.0f / 255.0f);
	const FVector3f HalfExtent(3.5f);

	// Leg 1 - published-empty / neutral: production neutral bindings, empty index list.
	{
		FParityLegInputs Inputs;
		Inputs.bNeutralOnly = true;
		Inputs.Probes = { FVector4f(0.0f, 0.0f, 0.0f, 0.0f), FVector4f(100.0f, 50.0f, 0.0f, 0.0f) };
		FParityLegResult Result;
		DispatchParityLeg(Inputs, Result);
		if (!TestTrue(TEXT("Neutral leg executed"), Result.bExecuted)) { return false; }
		TestTrue(TEXT("Neutral bindings yield zero at the origin"), NearlyEqual(Result.Weights[0], Zero, Tolerance));
		TestTrue(TEXT("Neutral bindings yield zero off-origin"), NearlyEqual(Result.Weights[1], Zero, Tolerance));
	}

	// Leg 2 - no-page-table sentinel and absent-page entries both contribute exact zero.
	{
		FParityLegInputs Inputs;
		// Record 0: sentinel binding (neutral proxy / no table) - the default resolved binding.
		Inputs.Records.Add(MakeRecord(FVector::ZeroVector, HalfExtent,
			FVector4f(1.0f, 1.0f, 1.0f, 1.0f), FVoxelPaintResolvedBinding()));
		// Record 1: a REAL page-table binding whose single entry reads PageEntryAbsent.
		FVoxelPaintResolvedBinding PagedBinding;
		PagedBinding.PageTableBaseIndex = 0u;
		PagedBinding.SessionSelector = 0u;
		Inputs.Records.Add(MakeRecord(FVector(100.0, 0.0, 0.0), HalfExtent,
			FVector4f(1.0f, 1.0f, 1.0f, 1.0f), PagedBinding));
		Inputs.RecordIndices = { 0u, 1u };
		Inputs.PageTable = { WorldVoxelPaint::PageEntryAbsent };
		Inputs.Probes = { FVector4f(0.0f, 0.0f, 0.0f, 0.0f), FVector4f(100.0f, 0.0f, 0.0f, 0.0f) };
		FParityLegResult Result;
		DispatchParityLeg(Inputs, Result);
		if (!TestTrue(TEXT("Sentinel/absent leg executed"), Result.bExecuted)) { return false; }
		TestTrue(TEXT("Sentinel record contributes exact zero"), NearlyEqual(Result.Weights[0], Zero, Tolerance));
		TestTrue(TEXT("Absent page entry contributes exact zero"), NearlyEqual(Result.Weights[1], Zero, Tolerance));
	}

	// Leg 3 - session-selector branch: dense-texture sampling with exact expected values
	// (BGRA8, the production packing - spec-inputs J), ascending-order accumulate,
	// raw/unclamped negative coefficients, OBB reject.
	{
		FVoxelPaintResolvedBinding SelectorBinding;
		SelectorBinding.PageTableBaseIndex = WorldVoxelPaint::NoPageTableSentinel;
		SelectorBinding.SessionSelector = WorldVoxelPaint::SessionSelectorActive;

		FParityLegInputs Inputs;
		Inputs.Records.Add(MakeRecord(FVector::ZeroVector, HalfExtent,
			FVector4f(1.0f, 1.0f, 1.0f, 1.0f), SelectorBinding));
		Inputs.Records.Add(MakeRecord(FVector(100.0, 0.0, 0.0), HalfExtent,
			FVector4f(-1.0f, -1.0f, -1.0f, -1.0f), SelectorBinding));
		Inputs.RecordIndices = { 0u, 1u };
		Inputs.bBindSessionTexture = true;
		Inputs.SessionColor = SessionColor;
		Inputs.Probes = {
			FVector4f(0.0f, 0.0f, 0.0f, 0.0f),      // inside record 0 only -> +SessionValue
			FVector4f(100.0f, 0.0f, 0.0f, 0.0f),    // inside record 1 only -> -SessionValue
			FVector4f(10.0f, 0.0f, 0.0f, 0.0f) };   // outside both OBBs -> zero
		FParityLegResult Result;
		DispatchParityLeg(Inputs, Result);
		if (!TestTrue(TEXT("Selector leg executed"), Result.bExecuted)) { return false; }
		TestTrue(TEXT("Selector record samples the dense session texture exactly"),
			NearlyEqual(Result.Weights[0], SessionValue, Tolerance));
		TestTrue(TEXT("Negative blend coefficients stay raw/unclamped"),
			NearlyEqual(Result.Weights[1], -SessionValue, Tolerance));
		TestTrue(TEXT("OBB containment rejects an outside probe"),
			NearlyEqual(Result.Weights[2], Zero, Tolerance));
	}

	// Leg 4 - the 2D XY evaluation-radius cull: far camera culls everything; a camera offset
	// only in Z does not (the cull is XY-only, matching the material path).
	{
		FVoxelPaintResolvedBinding SelectorBinding;
		SelectorBinding.PageTableBaseIndex = WorldVoxelPaint::NoPageTableSentinel;
		SelectorBinding.SessionSelector = WorldVoxelPaint::SessionSelectorActive;

		FParityLegInputs Culled;
		Culled.Records.Add(MakeRecord(FVector::ZeroVector, HalfExtent,
			FVector4f(1.0f, 1.0f, 1.0f, 1.0f), SelectorBinding));
		Culled.RecordIndices = { 0u };
		Culled.bBindSessionTexture = true;
		Culled.SessionColor = SessionColor;
		Culled.CameraCenter = FVector3f(100000.0f, 100000.0f, 0.0f);
		Culled.EvaluationRadiusSq = 10000.0f;
		Culled.Probes = { FVector4f(0.0f, 0.0f, 0.0f, 0.0f) };
		FParityLegResult CulledResult;
		DispatchParityLeg(Culled, CulledResult);
		if (!TestTrue(TEXT("Cull leg executed"), CulledResult.bExecuted)) { return false; }
		TestTrue(TEXT("Probe beyond the XY evaluation radius yields zero"),
			NearlyEqual(CulledResult.Weights[0], Zero, Tolerance));

		FParityLegInputs ZOffset = Culled;
		ZOffset.CameraCenter = FVector3f(0.0f, 0.0f, 99999.0f);
		ZOffset.EvaluationRadiusSq = 1.0f;
		FParityLegResult ZResult;
		DispatchParityLeg(ZOffset, ZResult);
		if (!TestTrue(TEXT("Z-offset leg executed"), ZResult.bExecuted)) { return false; }
		TestTrue(TEXT("Radius cull is 2D: a Z-only camera offset does not cull"),
			NearlyEqual(ZResult.Weights[0], SessionValue, Tolerance));
	}

	// BC7 brick-pool leg intentionally omitted (spec-inputs J: optional; exact-value BC7
	// authoring is not worth the fixture cost - the page-table chain is covered to the
	// page-entry read by leg 2, and the pool fetch shares its math with the material path).
	return true;
}
