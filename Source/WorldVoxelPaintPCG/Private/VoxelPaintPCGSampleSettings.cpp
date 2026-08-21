// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintPCGSampleSettings.h"

#include "VoxelPaintPCGSampleKernel.h"

#include "PCGCommon.h"
#include "PCGContext.h"
#include "PCGElement.h"
#include "Compute/PCGKernelHelpers.h"
#include "Elements/PCGActorSelector.h"
#include "Graph/PCGGPUGraphCompilationContext.h"
#include "Metadata/PCGMetadataAttribute.h"

#include "VoxelPaintDeveloperSettings.h"
#include "VoxelPaintGridSetup.h"
#include "WorldVoxelPaintConstants.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VoxelPaintPCGSampleSettings)

#define LOCTEXT_NAMESPACE "VoxelPaintPCGSampleSettings"

namespace VoxelPaintPCGSampleSettingsPrivate
{
	// CPU element stub for a GPU-only node (Custom HLSL precedent, PCGCustomHLSL.cpp:1083):
	// valid nodes are replaced by a compute-graph element at compile time; executing this
	// means the node was not set up correctly.
	class FVoxelPaintPCGSampleElement : public IPCGElement
	{
	protected:
		virtual bool ExecuteInternal(FPCGContext* Context) const override
		{
			// This element does not support CPU execution and we are never supposed to land here.
			check(false);
			return true;
		}
	};
}

UVoxelPaintPCGSampleSettings::UVoxelPaintPCGSampleSettings()
{
	bExecuteOnGPU = true;

#if WITH_EDITOR
	// ChannelContexts renames must retrigger graph compile: the attribute names are baked
	// into the compiled compute graph's static attribute table (Custom HLSL's
	// compute-source-modified precedent, PCGCustomHLSL.cpp:33-45). Same ctor guards: never
	// on the CDO, never while PCG is initializing a settings copy for execution.
	if (!HasAnyFlags(RF_ClassDefaultObject) && !FPCGContext::IsInitializingSettings())
	{
		if (!WorldVoxelPaint::OnDeveloperSettingsChanged().IsBoundToObject(this))
		{
			WorldVoxelPaint::OnDeveloperSettingsChanged().AddUObject(
				this, &UVoxelPaintPCGSampleSettings::OnVoxelPaintDeveloperSettingsChanged);
		}
		GetChannelAttributeNames(CachedChannelAttributeNames);
	}
#endif
}

void UVoxelPaintPCGSampleSettings::GetChannelAttributeNames(FName OutChannelNames[4])
{
	static const TCHAR* Letters[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };
	static const TCHAR* GenericNames[4] =
		{ TEXT("VoxelChannel0"), TEXT("VoxelChannel1"), TEXT("VoxelChannel2"), TEXT("VoxelChannel3") };

	const TArray<FVoxelPaintChannelContext>& ChannelContexts =
		GetDefault<UVoxelPaintDeveloperSettings>()->ChannelContexts;
	for (int32 ChannelIndex = 0; ChannelIndex < 4; ++ChannelIndex)
	{
		FString Candidate;
		if (ChannelContexts.IsValidIndex(ChannelIndex))
		{
			Candidate = ChannelContexts[ChannelIndex].DisplayName.TrimStartAndEnd();
		}
		// "Unnamed" per the material-node rule (MaterialExpressionSampleWorldVoxelPaint.cpp:
		// empty or letter-equal display names carry no real name) -> stable generic fallback.
		if (Candidate.IsEmpty() || Candidate.Equals(Letters[ChannelIndex], ESearchCase::IgnoreCase))
		{
			OutChannelNames[ChannelIndex] = FName(GenericNames[ChannelIndex]);
			continue;
		}
		// PCG attribute names are restricted; replace invalid characters with underscores.
		FPCGMetadataAttributeBase::SanitizeName(Candidate);
		const FName CandidateName(*Candidate);
		// The four generic names are RESERVED: a user name equal to ANY VoxelChannel<k> takes
		// this channel's own generic fallback (mirroring the letter-equal rule above). Without
		// this, a user name equal to a LATER channel's generic (e.g. channel 0 named
		// "VoxelChannel1" with channel 1 unnamed) collides with that channel's fallback and
		// collapses two channels onto one attribute - fallbacks are never re-checked, so
		// uniqueness must hold by construction: generics only ever surface at their own index,
		// user names never equal a generic, and user-vs-user duplicates fall back to the own
		// (unique) generic. FName equality is case-insensitive, matching the letter rule.
		bool bReservedOrDuplicate = false;
		for (int32 GenericIndex = 0; GenericIndex < 4; ++GenericIndex)
		{
			if (CandidateName == FName(GenericNames[GenericIndex]))
			{
				bReservedOrDuplicate = true;
				break;
			}
		}
		for (int32 PriorIndex = 0; PriorIndex < ChannelIndex && !bReservedOrDuplicate; ++PriorIndex)
		{
			if (OutChannelNames[PriorIndex] == CandidateName)
			{
				bReservedOrDuplicate = true;
			}
		}
		OutChannelNames[ChannelIndex] = bReservedOrDuplicate ? FName(GenericNames[ChannelIndex]) : CandidateName;
	}
}

TArray<FPCGPinProperties> UVoxelPaintPCGSampleSettings::InputPinProperties() const
{
	// Built with the exported (Label, Type) constructor only: the FPCGPinProperties DEFAULT
	// constructor is unexported and must never be instantiated from this module (no
	// AddDefaulted/SetNum on pin arrays). Mirrors UPCGSettings::DefaultPointInputPinProperties
	// (PCGSettings.cpp:1211-1217) without calling it.
	TArray<FPCGPinProperties> Pins;
	FPCGPinProperties& InputPin = Pins.Emplace_GetRef(PCGPinConstants::DefaultInputLabel, EPCGDataType::Point);
	InputPin.SetRequiredPin();
	return Pins;
}

TArray<FPCGPinProperties> UVoxelPaintPCGSampleSettings::OutputPinProperties() const
{
	// Mirrors UPCGSettings::DefaultPointOutputPinProperties (PCGSettings.cpp:1219-1224).
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultOutputLabel, EPCGDataType::Point);
	return Pins;
}

FString UVoxelPaintPCGSampleSettings::GetAdditionalTitleInformation() const
{
	// Surfaces the resolved ChannelContexts display for the density channel on the node body
	// (the dynamic-display surface PCG offers without a details customization).
	if (!bWriteDensity)
	{
		return FString();
	}
	FName ChannelNames[4];
	GetChannelAttributeNames(ChannelNames);
	const int32 ClampedChannel = FMath::Clamp(DensityChannel, 0, 3);
	return FString::Printf(TEXT("Density: %s"), *ChannelNames[ClampedChannel].ToString());
}

#if WITH_EDITOR

FName UVoxelPaintPCGSampleSettings::GetDefaultNodeName() const
{
	return FName(TEXT("SampleWorldVoxelPaint"));
}

FText UVoxelPaintPCGSampleSettings::GetDefaultNodeTitle() const
{
	return LOCTEXT("NodeTitle", "Sample World Voxel Paint");
}

FText UVoxelPaintPCGSampleSettings::GetNodeTooltipText() const
{
	return LOCTEXT("NodeTooltip",
		"Samples the painted World Voxel Paint field on the GPU and writes the four project "
		"channel weights onto each point as float attributes (named from the paint channel "
		"contexts in Project Settings). Weights are raw and unclamped, identical to the Sample "
		"World Voxel Paint material node - saturate before use as lerp-style factors. "
		"Optionally writes the selected channel to Density, saturated. Samples whatever voxel "
		"data is GPU-resident; unpublished regions read as zero.");
}

EPCGSettingsType UVoxelPaintPCGSampleSettings::GetType() const
{
	return EPCGSettingsType::Sampler;
}

void UVoxelPaintPCGSampleSettings::CreateKernels(FPCGGPUCompilationContext& InOutContext, UObject* InObjectOuter, TArray<UPCGComputeKernel*>& OutKernels, TArray<FPCGKernelEdge>& OutEdges) const
{
	// Defaults wire the node's In/Out pins to the kernel's In/Out pins.
	PCGKernelHelpers::FCreateKernelParams CreateParams(InObjectOuter, this);
	PCGKernelHelpers::CreateKernel<UVoxelPaintPCGSampleKernel>(InOutContext, CreateParams, OutKernels, OutEdges);
}

void UVoxelPaintPCGSampleSettings::GetStaticTrackedKeys(FPCGSelectionKeyToSettingsMap& OutKeysToSettings, TArray<TObjectPtr<const UPCGGraph>>& OutVisitedGraphs) const
{
	// Exactly one CULLED ByTag key on the host auto-tag (M6 plan 6.1/5.4): every actor hosting
	// a registered UVoxelPaintVolumeComponent carries the tag, so move/add/delete/undo of any
	// placement - dedicated actor or arbitrary prefab host - refreshes only PCG components
	// whose bounds intersect. Semantics (matching, culling, commit-time fan-out) are owned by
	// the refresh area; this declaration is the node's entire part.
	OutKeysToSettings.FindOrAdd(FPCGSelectionKey(WorldVoxelPaint::PaintVolumeHostTag)).Emplace(this, /*bCull=*/true);
}

void UVoxelPaintPCGSampleSettings::OnVoxelPaintDeveloperSettingsChanged()
{
	// The core delegate fires for EVERY developer-settings edit; only channel renames concern
	// this node. Recompile (Structural - attribute names live in the compiled graph's static
	// attribute table) only when the derived names actually changed.
	FName NewNames[4];
	GetChannelAttributeNames(NewNames);
	bool bChanged = false;
	for (int32 ChannelIndex = 0; ChannelIndex < 4; ++ChannelIndex)
	{
		if (CachedChannelAttributeNames[ChannelIndex] != NewNames[ChannelIndex])
		{
			bChanged = true;
			break;
		}
	}
	if (bChanged)
	{
		for (int32 ChannelIndex = 0; ChannelIndex < 4; ++ChannelIndex)
		{
			CachedChannelAttributeNames[ChannelIndex] = NewNames[ChannelIndex];
		}
		OnSettingsChangedDelegate.Broadcast(this, EPCGChangeType::Structural);
	}
}

void UVoxelPaintPCGSampleSettings::BeginDestroy()
{
	WorldVoxelPaint::OnDeveloperSettingsChanged().RemoveAll(this);

	Super::BeginDestroy();
}

#endif // WITH_EDITOR

FPCGElementPtr UVoxelPaintPCGSampleSettings::CreateElement() const
{
	return MakeShared<VoxelPaintPCGSampleSettingsPrivate::FVoxelPaintPCGSampleElement>();
}

#undef LOCTEXT_NAMESPACE
