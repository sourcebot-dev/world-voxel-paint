// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "MaterialExpressionIO.h"
#include "MaterialValueType.h"
#include "Materials/MaterialExpression.h"
#include "MaterialExpressionSampleWorldVoxelPaint.generated.h"

UCLASS(collapsecategories, hidecategories = Object, MinimalAPI)
class UMaterialExpressionSampleWorldVoxelPaint : public UMaterialExpression
{
	GENERATED_UCLASS_BODY()

	UPROPERTY(meta = (RequiredInput = "false", ToolTip = "Color to composite voxel paint over. Defaults to (0,0,0,0)."))
	FExpressionInput CurrentColor;

	UPROPERTY(meta = (RequiredInput = "false", ToolTip = "LWC world position. Defaults to the pixel's absolute world position."))
	FExpressionInput WorldPosition;

	UPROPERTY(Transient)
	TObjectPtr<class UMaterialExpressionCustom> CachedCustomExpression;

#if WITH_EDITOR
	virtual int32 Compile(class FMaterialCompiler* Compiler, int32 OutputIndex) override;
	virtual void GetCaption(TArray<FString>& OutCaptions) const override;
	virtual TArray<FExpressionOutput>& GetOutputs() override;
	virtual EMaterialValueType GetOutputValueType(int32 OutputIndex) override;
	virtual EMaterialValueType GetInputValueType(int32 InputIndex) override;
	virtual void GetIncludeFilePaths(TSet<FString>& OutIncludeFilePaths) const override;
#endif
};
