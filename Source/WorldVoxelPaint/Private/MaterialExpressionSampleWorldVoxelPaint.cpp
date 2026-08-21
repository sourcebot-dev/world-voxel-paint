// Copyright Epic Games, Inc. All Rights Reserved.

#include "MaterialExpressionSampleWorldVoxelPaint.h"

#include "MaterialCompiler.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "VoxelPaintDeveloperSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MaterialExpressionSampleWorldVoxelPaint)

#if WITH_EDITORONLY_DATA
namespace
{
	void BuildVoxelPaintOutputs(TArray<FExpressionOutput>& Outputs)
	{
		// RGBA must stay at output index 0: saved materials reference outputs by index, and
		// connections made before the per-channel pins existed compiled the full float4.
		Outputs.Reset();
		Outputs.Add(FExpressionOutput(TEXT("RGBA"), 1, 1, 1, 1, 1));
		Outputs.Add(FExpressionOutput(TEXT("R"), 1, 1, 0, 0, 0));
		Outputs.Add(FExpressionOutput(TEXT("G"), 1, 0, 1, 0, 0));
		Outputs.Add(FExpressionOutput(TEXT("B"), 1, 0, 0, 1, 0));
		Outputs.Add(FExpressionOutput(TEXT("A"), 1, 0, 0, 0, 1));
	}
}
#endif

UMaterialExpressionSampleWorldVoxelPaint::UMaterialExpressionSampleWorldVoxelPaint(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	struct FConstructorStatics
	{
		FText NAME_WorldVoxelPaint;
		FConstructorStatics()
			: NAME_WorldVoxelPaint(NSLOCTEXT("WorldVoxelPaint", "MenuCategory", "WorldVoxelPaint"))
		{
		}
	};
	static FConstructorStatics ConstructorStatics;

#if WITH_EDITORONLY_DATA
	MenuCategories.Add(ConstructorStatics.NAME_WorldVoxelPaint);
	BuildVoxelPaintOutputs(Outputs);
	bShowOutputNameOnPin = true;
#endif
}

#if WITH_EDITOR

int32 UMaterialExpressionSampleWorldVoxelPaint::Compile(FMaterialCompiler* Compiler, int32 OutputIndex)
{
	if (CachedCustomExpression == nullptr)
	{
		CachedCustomExpression = NewObject<UMaterialExpressionCustom>(this, NAME_None, RF_Transient);
		CachedCustomExpression->Description = TEXT("SampleWorldVoxelPaint");
		CachedCustomExpression->OutputType = CMOT_Float4;
		CachedCustomExpression->Code = TEXT("return SampleWorldVoxelPaint(CurrentColor, LWCWorldPosition);");
		CachedCustomExpression->IncludeFilePaths = { TEXT("/Plugin/WorldVoxelPaint/VoxelPaintSample.ush") };
		CachedCustomExpression->Inputs.Reset();
		FCustomInput& CurrentColorInput = CachedCustomExpression->Inputs.AddDefaulted_GetRef();
		CurrentColorInput.InputName = TEXT("CurrentColor");
		FCustomInput& WorldPositionInput = CachedCustomExpression->Inputs.AddDefaulted_GetRef();
		WorldPositionInput.InputName = TEXT("WorldPosition");
	}

	const int32 CurrentColorCode = CurrentColor.GetTracedInput().Expression ? CurrentColor.Compile(Compiler) : Compiler->Constant4(0.0f, 0.0f, 0.0f, 0.0f);
	int32 WorldPositionCode = WorldPosition.GetTracedInput().Expression ? WorldPosition.Compile(Compiler) : Compiler->WorldPosition(WPT_Default);
	WorldPositionCode = Compiler->ValidCast(WorldPositionCode, MCT_LWCVector3);

	// Always compile the custom expression's single float4 output; per-channel pins are
	// extracted by the consuming FExpressionInput from the output masks set in GetOutputs.
	TArray<int32> CompiledInputs { CurrentColorCode, WorldPositionCode };
	return Compiler->CustomExpression(CachedCustomExpression, 0, CompiledInputs);
}

void UMaterialExpressionSampleWorldVoxelPaint::GetCaption(TArray<FString>& OutCaptions) const
{
	OutCaptions.Add(TEXT("Sample World Voxel Paint"));
}

TArray<FExpressionOutput>& UMaterialExpressionSampleWorldVoxelPaint::GetOutputs()
{
	if (Outputs.Num() != 5)
	{
		BuildVoxelPaintOutputs(Outputs);
	}

	static const TCHAR* Letters[] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };
	const TArray<FVoxelPaintChannelContext>& ChannelContexts = GetDefault<UVoxelPaintDeveloperSettings>()->ChannelContexts;
	for (int32 ChannelIndex = 0; ChannelIndex < 4; ++ChannelIndex)
	{
		const FString Letter = Letters[ChannelIndex];
		FString PinName = Letter;
		if (ChannelContexts.IsValidIndex(ChannelIndex))
		{
			const FString DisplayName = ChannelContexts[ChannelIndex].DisplayName.TrimStartAndEnd();
			if (!DisplayName.IsEmpty() && !DisplayName.Equals(Letter, ESearchCase::IgnoreCase))
			{
				PinName = FString::Printf(TEXT("%s (%s)"), *DisplayName, *Letter);
			}
		}
		Outputs[ChannelIndex + 1].OutputName = FName(*PinName);
	}
	return Outputs;
}

EMaterialValueType UMaterialExpressionSampleWorldVoxelPaint::GetOutputValueType(int32 OutputIndex)
{
	return OutputIndex == 0 ? MCT_Float4 : MCT_Float;
}

EMaterialValueType UMaterialExpressionSampleWorldVoxelPaint::GetInputValueType(int32 InputIndex)
{
	return InputIndex == 0 ? MCT_Float4 : MCT_LWCVector3;
}

void UMaterialExpressionSampleWorldVoxelPaint::GetIncludeFilePaths(TSet<FString>& OutIncludeFilePaths) const
{
	OutIncludeFilePaths.Add(TEXT("/Plugin/WorldVoxelPaint/VoxelPaintSample.ush"));
}

#endif // WITH_EDITOR
