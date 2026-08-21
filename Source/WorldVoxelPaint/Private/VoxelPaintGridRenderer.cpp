// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintGridRenderer.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"
#include "RHIFeatureLevel.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "VoxelPaintStats.h"
#include "WorldVoxelPaintConstants.h"

DECLARE_GPU_STAT_NAMED(VoxelPaintGridRebuild, TEXT("VoxelPaint GridRebuild"));

namespace
{

constexpr int32 GVoxelPaintGridGroupSize = 64;

struct FVoxelPaintGridRangePacked
{
	uint32 RecordIndex;
	uint32 CellMinX;
	uint32 CellMinY;
	uint32 CellMinZ;
	uint32 CellDimsX;
	uint32 CellDimsY;
	uint32 CellDimsZ;
	uint32 FirstThread;
};
static_assert(sizeof(FVoxelPaintGridRangePacked) == 32);

}

class FVoxelPaintGridClearHeadersCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelPaintGridClearHeadersCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPaintGridClearHeadersCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWGridHeaders)
		SHADER_PARAMETER(uint32, NumLiveCells)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GVoxelPaintGridGroupSize);
	}
};

class FVoxelPaintGridDiscoverCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelPaintGridDiscoverCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPaintGridDiscoverCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Volumes)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FWVPGridRange>, Ranges)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWGridHeaders)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, RWNodes)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWActiveCells)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWErrorFlag)
		SHADER_PARAMETER(uint32, NumRanges)
		SHADER_PARAMETER(uint32, TotalThreads)
		SHADER_PARAMETER(uint32, KCap)
		SHADER_PARAMETER(FIntVector, GridDims)
		SHADER_PARAMETER(FVector3f, GridOriginRel)
		SHADER_PARAMETER(FVector2f, CellSize)
	END_SHADER_PARAMETER_STRUCT()

	class FWaveOps : SHADER_PERMUTATION_BOOL("WAVE_OPS");
	using FPermutationDomain = TShaderPermutationDomain<FWaveOps>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FWaveOps>() && !RHISupportsWaveOperations(Parameters.Platform))
		{
			return false;
		}

		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GVoxelPaintGridGroupSize);

		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FWaveOps>())
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
		}
	}
};

class FVoxelPaintGridSetupActiveDispatchCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelPaintGridSetupActiveDispatchCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPaintGridSetupActiveDispatchCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, Counters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndirectArgs)
		SHADER_PARAMETER(FIntVector, MaxDispatchThreadGroupsPerDimension)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GVoxelPaintGridGroupSize);
	}
};

class FVoxelPaintGridCanonicalizeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelPaintGridCanonicalizeCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPaintGridCanonicalizeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, Nodes)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ActiveCells)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWGridHeaders)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWGridIndexList)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWErrorFlag)
		SHADER_PARAMETER(uint32, KCap)
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	class FWaveOps : SHADER_PERMUTATION_BOOL("WAVE_OPS");
	using FPermutationDomain = TShaderPermutationDomain<FWaveOps>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FWaveOps>() && !RHISupportsWaveOperations(Parameters.Platform))
		{
			return false;
		}

		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GVoxelPaintGridGroupSize);

		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FWaveOps>())
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
		}
	}
};

class FVoxelPaintGridErrorClearCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FVoxelPaintGridErrorClearCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelPaintGridErrorClearCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ErrorFlag)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ActiveCells)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, Counters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWGridHeaders)
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GVoxelPaintGridGroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVoxelPaintGridClearHeadersCS, "/Plugin/WorldVoxelPaint/VoxelPaintGridBuild.usf", "ClearHeadersCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelPaintGridDiscoverCS, "/Plugin/WorldVoxelPaint/VoxelPaintGridBuild.usf", "DiscoverCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelPaintGridSetupActiveDispatchCS, "/Plugin/WorldVoxelPaint/VoxelPaintGridBuild.usf", "SetupActiveCellIndirectArgsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelPaintGridCanonicalizeCS, "/Plugin/WorldVoxelPaint/VoxelPaintGridBuild.usf", "CanonicalizeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelPaintGridErrorClearCS, "/Plugin/WorldVoxelPaint/VoxelPaintGridBuild.usf", "ErrorClearCS", SF_Compute);

bool FVoxelPaintGridRenderer::IsInitialized() const
{
	return GridHeadersBuffer.IsValid() && GridIndexListBuffer.IsValid();
}

uint64 FVoxelPaintGridRenderer::GetSteadyBytes() const
{
	if (!IsInitialized())
	{
		return 0;
	}
	return 4ull * Capacity.CCap + 4ull * Capacity.KCap;
}

void FVoxelPaintGridRenderer::Initialize(FRDGBuilder& GraphBuilder, const WorldVoxelPaint::FVoxelPaintGridCapacity& InCapacity)
{
	Capacity = InCapacity;
	GridHeadersBuffer.SafeRelease();
	GridIndexListBuffer.SafeRelease();
	PublishedGridDims = FIntVector::ZeroValue;
	PublishedGridOrigin = FVector3f::ZeroVector;
	PublishedCellSize = FVector2f::ZeroVector;
	LastLiveCellCount = 0;
	LastEmitUpperBound = 0;
	LastRebuildScratchBytes = 0;

	if (!Capacity.bValid || Capacity.CCap == 0u || Capacity.KCap == 0u)
	{
		UE_LOG(LogWorldVoxelPaint, Warning,
			TEXT("Grid capacity invalid (CCap=%u KCap=%u); grid renders neutral until settings change."),
			Capacity.CCap, Capacity.KCap);
		SET_DWORD_STAT(STAT_VoxelPaint_GridCandidateCap, 0);
		SET_DWORD_STAT(STAT_VoxelPaint_GridSteadyBytes, 0);
		SET_DWORD_STAT(STAT_VoxelPaint_GridLiveCells, 0);
		SET_DWORD_STAT(STAT_VoxelPaint_GridEmitUpperBound, 0);
		SET_DWORD_STAT(STAT_VoxelPaint_GridRebuildScratchBytes, 0);
		return;
	}

	GridHeadersBuffer = AllocatePooledBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Capacity.CCap), TEXT("WorldVoxelPaint.GridHeaders"));
	GridIndexListBuffer = AllocatePooledBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Capacity.KCap), TEXT("WorldVoxelPaint.GridIndexList"));

	AddClearUAVPass(GraphBuilder,
		GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(GridHeadersBuffer)), 0u);

	SET_DWORD_STAT(STAT_VoxelPaint_GridCandidateCap, static_cast<int64>(Capacity.KCap));
	SET_DWORD_STAT(STAT_VoxelPaint_GridSteadyBytes, static_cast<int64>(GetSteadyBytes()));
	SET_DWORD_STAT(STAT_VoxelPaint_GridLiveCells, 0);
	SET_DWORD_STAT(STAT_VoxelPaint_GridEmitUpperBound, 0);
	SET_DWORD_STAT(STAT_VoxelPaint_GridRebuildScratchBytes, 0);
}

void FVoxelPaintGridRenderer::Reinitialize(FRDGBuilder& GraphBuilder, const WorldVoxelPaint::FVoxelPaintGridCapacity& InCapacity)
{
	Initialize(GraphBuilder, InCapacity);
}

void FVoxelPaintGridRenderer::Rebuild(FRDGBuilder& GraphBuilder, const WorldVoxelPaint::FVoxelPaintGridBuildInput& Input,
	FRDGBufferSRVRef VolumesSRV
#if WITH_EDITOR
	, FRDGBufferRef* OutErrorFlagBuffer
#endif
	)
{
#if WITH_EDITOR
	if (OutErrorFlagBuffer)
	{
		*OutErrorFlagBuffer = nullptr;
	}
#endif
	RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelPaintGridRebuild, "WorldVoxelPaint.GridRebuild");

	const uint64 LiveCellCount64 = static_cast<uint64>(FMath::Max(Input.GridDims.X, 0))
		* static_cast<uint64>(FMath::Max(Input.GridDims.Y, 0))
		* static_cast<uint64>(FMath::Max(Input.GridDims.Z, 0));

	if (!IsInitialized() || !Input.bValid || VolumesSRV == nullptr
		|| Input.GridDims.X <= 0 || Input.GridDims.Y <= 0 || Input.GridDims.Z <= 0
		|| LiveCellCount64 == 0u || LiveCellCount64 > static_cast<uint64>(Capacity.CCap)
		|| Input.KCap != Capacity.KCap
		|| Input.TotalThreads > Capacity.KCap)
	{
		UE_LOG(LogWorldVoxelPaint, Warning,
			TEXT("Grid rebuild rejected (Valid=%d Dims=%dx%dx%d CCap=%u InputKCap=%u KCap=%u E=%u); failing neutral."),
			Input.bValid ? 1 : 0, Input.GridDims.X, Input.GridDims.Y, Input.GridDims.Z,
			Capacity.CCap, Input.KCap, Capacity.KCap, Input.TotalThreads);
		FailNeutral(GraphBuilder, Capacity.KCap != 0u && Input.TotalThreads > Capacity.KCap);
		return;
	}

	const uint32 NumLiveCells = static_cast<uint32>(LiveCellCount64);
	FRDGBufferRef HeadersBuffer = GraphBuilder.RegisterExternalBuffer(GridHeadersBuffer);
	FRDGBufferRef IndexListBuffer = GraphBuilder.RegisterExternalBuffer(GridIndexListBuffer);
	FRDGBufferUAVRef HeadersUAV = GraphBuilder.CreateUAV(HeadersBuffer);

	{
		FVoxelPaintGridClearHeadersCS::FParameters* Parameters =
			GraphBuilder.AllocParameters<FVoxelPaintGridClearHeadersCS::FParameters>();
		Parameters->RWGridHeaders = HeadersUAV;
		Parameters->NumLiveCells = NumLiveCells;
		TShaderMapRef<FVoxelPaintGridClearHeadersCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelPaintGridClearHeaders"),
			ComputeShader, Parameters,
			FComputeShaderUtils::GetGroupCountWrapped(static_cast<int32>(NumLiveCells), GVoxelPaintGridGroupSize));
	}

	FRDGBufferRef ErrorFlagBuffer = nullptr;
	FRDGBufferUAVRef ErrorFlagUAV = nullptr;
	bool bCreateErrorFlag = Input.TotalThreads > 0u && Input.Ranges.Num() > 0;
#if WITH_EDITOR
	bCreateErrorFlag = bCreateErrorFlag || OutErrorFlagBuffer != nullptr;
#endif
	if (bCreateErrorFlag)
	{
		ErrorFlagBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("WorldVoxelPaint.GridErrorFlag"));
		ErrorFlagUAV = GraphBuilder.CreateUAV(ErrorFlagBuffer);
		AddClearUAVPass(GraphBuilder, ErrorFlagUAV, 0u);
	}

	uint64 ScratchBytes = 0;
	if (Input.TotalThreads > 0u && Input.Ranges.Num() > 0)
	{
		const uint32 MaxActiveCells = FMath::Min(Input.TotalThreads, NumLiveCells);
		FRDGBufferRef NodesBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector2), Capacity.KCap), TEXT("WorldVoxelPaint.GridNodes"));
		FRDGBufferRef ActiveCellsBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), MaxActiveCells), TEXT("WorldVoxelPaint.GridActiveCells"));
		FRDGBufferRef CountersBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 3), TEXT("WorldVoxelPaint.GridCounters"));
		FRDGBufferRef ActiveDispatchArgs = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(), TEXT("WorldVoxelPaint.GridActiveDispatchArgs"));

		FRDGUploadData<FVoxelPaintGridRangePacked> RangeData(GraphBuilder, Input.Ranges.Num());
		for (int32 RangeIndex = 0; RangeIndex < Input.Ranges.Num(); ++RangeIndex)
		{
			const WorldVoxelPaint::FVoxelPaintGridVolumeRange& Range = Input.Ranges[RangeIndex];
			FVoxelPaintGridRangePacked& Packed = RangeData[RangeIndex];
			Packed.RecordIndex = Range.RecordIndex;
			Packed.CellMinX = static_cast<uint32>(Range.CellMin.X);
			Packed.CellMinY = static_cast<uint32>(Range.CellMin.Y);
			Packed.CellMinZ = static_cast<uint32>(Range.CellMin.Z);
			Packed.CellDimsX = static_cast<uint32>(Range.CellDims.X);
			Packed.CellDimsY = static_cast<uint32>(Range.CellDims.Y);
			Packed.CellDimsZ = static_cast<uint32>(Range.CellDims.Z);
			Packed.FirstThread = Range.FirstThread;
		}
		FRDGBufferRef RangesBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("WorldVoxelPaint.GridRanges"), RangeData);

		FRDGBufferUAVRef CountersUAV = GraphBuilder.CreateUAV(CountersBuffer);
		AddClearUAVPass(GraphBuilder, CountersUAV, 0u);

		const bool bWaveOps = GRHISupportsWaveOperations && RHISupportsWaveOperations(GMaxRHIShaderPlatform);

		{
			FVoxelPaintGridDiscoverCS::FParameters* Parameters =
				GraphBuilder.AllocParameters<FVoxelPaintGridDiscoverCS::FParameters>();
			Parameters->Volumes = VolumesSRV;
			Parameters->Ranges = GraphBuilder.CreateSRV(RangesBuffer);
			Parameters->RWGridHeaders = HeadersUAV;
			Parameters->RWNodes = GraphBuilder.CreateUAV(NodesBuffer);
			Parameters->RWActiveCells = GraphBuilder.CreateUAV(ActiveCellsBuffer);
			Parameters->RWCounters = CountersUAV;
			Parameters->RWErrorFlag = ErrorFlagUAV;
			Parameters->NumRanges = static_cast<uint32>(Input.Ranges.Num());
			Parameters->TotalThreads = Input.TotalThreads;
			Parameters->KCap = Capacity.KCap;
			Parameters->GridDims = Input.GridDims;
			Parameters->GridOriginRel = Input.GridOriginRel;
			Parameters->CellSize = Input.CellSize;
			FVoxelPaintGridDiscoverCS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FVoxelPaintGridDiscoverCS::FWaveOps>(bWaveOps);
			TShaderMapRef<FVoxelPaintGridDiscoverCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelPaintGridDiscover"),
				ComputeShader, Parameters,
				FComputeShaderUtils::GetGroupCountWrapped(static_cast<int32>(Input.TotalThreads), GVoxelPaintGridGroupSize));
		}

		{
			FVoxelPaintGridSetupActiveDispatchCS::FParameters* Parameters =
				GraphBuilder.AllocParameters<FVoxelPaintGridSetupActiveDispatchCS::FParameters>();
			Parameters->Counters = GraphBuilder.CreateSRV(CountersBuffer);
			Parameters->RWIndirectArgs = GraphBuilder.CreateUAV(ActiveDispatchArgs, PF_R32_UINT);
			Parameters->MaxDispatchThreadGroupsPerDimension = GRHIMaxDispatchThreadGroupsPerDimension;
			TShaderMapRef<FVoxelPaintGridSetupActiveDispatchCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelPaintGridSetupActiveDispatch"),
				ComputeShader, Parameters, FIntVector(1, 1, 1));
		}

		{
			FVoxelPaintGridCanonicalizeCS::FParameters* Parameters =
				GraphBuilder.AllocParameters<FVoxelPaintGridCanonicalizeCS::FParameters>();
			Parameters->Nodes = GraphBuilder.CreateSRV(NodesBuffer);
			Parameters->ActiveCells = GraphBuilder.CreateSRV(ActiveCellsBuffer);
			Parameters->RWGridHeaders = HeadersUAV;
			Parameters->RWGridIndexList = GraphBuilder.CreateUAV(IndexListBuffer);
			Parameters->RWCounters = CountersUAV;
			Parameters->RWErrorFlag = ErrorFlagUAV;
			Parameters->KCap = Capacity.KCap;
			Parameters->IndirectArgs = ActiveDispatchArgs;
			FVoxelPaintGridCanonicalizeCS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FVoxelPaintGridCanonicalizeCS::FWaveOps>(bWaveOps);
			TShaderMapRef<FVoxelPaintGridCanonicalizeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelPaintGridCanonicalize"),
				ComputeShader, Parameters, ActiveDispatchArgs, 0);
		}

		{
			FVoxelPaintGridErrorClearCS::FParameters* Parameters =
				GraphBuilder.AllocParameters<FVoxelPaintGridErrorClearCS::FParameters>();
			Parameters->ErrorFlag = GraphBuilder.CreateSRV(ErrorFlagBuffer);
			Parameters->ActiveCells = GraphBuilder.CreateSRV(ActiveCellsBuffer);
			Parameters->Counters = GraphBuilder.CreateSRV(CountersBuffer);
			Parameters->RWGridHeaders = HeadersUAV;
			Parameters->IndirectArgs = ActiveDispatchArgs;
			TShaderMapRef<FVoxelPaintGridErrorClearCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelPaintGridErrorClear"),
				ComputeShader, Parameters, ActiveDispatchArgs, 0);
		}

		ScratchBytes = static_cast<uint64>(sizeof(FUintVector2)) * Capacity.KCap
			+ static_cast<uint64>(sizeof(uint32)) * MaxActiveCells
			+ 3u * sizeof(uint32) + sizeof(uint32)
			+ sizeof(FRHIDispatchIndirectParameters)
			+ static_cast<uint64>(sizeof(FVoxelPaintGridRangePacked)) * Input.Ranges.Num();
	}

	PublishedGridDims = Input.GridDims;
	PublishedGridOrigin = Input.GridOriginRel;
	PublishedCellSize = Input.CellSize;
	LastLiveCellCount = NumLiveCells;
	LastEmitUpperBound = Input.TotalThreads;
	LastRebuildScratchBytes = ScratchBytes;
	++RebuildCount;

#if WITH_EDITOR
	if (OutErrorFlagBuffer)
	{
		*OutErrorFlagBuffer = ErrorFlagBuffer;
	}
#endif

	SET_DWORD_STAT(STAT_VoxelPaint_GridLiveCells, static_cast<int64>(NumLiveCells));
	SET_DWORD_STAT(STAT_VoxelPaint_GridEmitUpperBound, static_cast<int64>(LastEmitUpperBound));
	SET_DWORD_STAT(STAT_VoxelPaint_GridRebuildScratchBytes, static_cast<int64>(LastRebuildScratchBytes));
	SET_DWORD_STAT(STAT_VoxelPaint_VolumesOutsideZEnvelope, static_cast<int64>(Input.NumOutsideZEnvelope));
	INC_DWORD_STAT(STAT_VoxelPaint_GridRebuilds);
}

void FVoxelPaintGridRenderer::FailNeutral(FRDGBuilder& GraphBuilder, bool bCandidateOverflow)
{
	PublishedGridDims = FIntVector::ZeroValue;
	PublishedGridOrigin = FVector3f::ZeroVector;
	PublishedCellSize = FVector2f::ZeroVector;
	LastLiveCellCount = 0;
	LastEmitUpperBound = 0;
	LastRebuildScratchBytes = 0;

	SET_DWORD_STAT(STAT_VoxelPaint_GridLiveCells, 0);
	SET_DWORD_STAT(STAT_VoxelPaint_GridEmitUpperBound, 0);
	SET_DWORD_STAT(STAT_VoxelPaint_GridRebuildScratchBytes, 0);
	if (bCandidateOverflow)
	{
		++OverflowEventCount;
		INC_DWORD_STAT(STAT_VoxelPaint_GridOverflowEvents);
	}
	else
	{
		++RejectedRebuildCount;
		INC_DWORD_STAT(STAT_VoxelPaint_GridRejectedRebuilds);
	}

	if (IsInitialized())
	{
		AddClearUAVPass(GraphBuilder,
			GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(GridHeadersBuffer)), 0u);
	}
}

void FVoxelPaintGridRenderer::Register(FRDGBuilder& GraphBuilder, FVoxelPaintSceneParameters& Params) const
{
	if (!IsInitialized())
	{
		return;
	}

	Params.GridHeaders = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(GridHeadersBuffer));
	Params.GridIndexList = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(GridIndexListBuffer));
	Params.GridDims = PublishedGridDims;
	Params.GridOrigin = PublishedGridOrigin;
	Params.GridCellSize = PublishedCellSize;
}
