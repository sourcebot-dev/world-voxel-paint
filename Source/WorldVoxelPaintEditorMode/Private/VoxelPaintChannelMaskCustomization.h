// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/ArrayView.h"
#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "Internationalization/Text.h"
#include "Templates/SharedPointer.h"

struct FVoxelPaintChannelContext;

namespace WorldVoxelPaint
{
	struct FVoxelPaintChannelMaskUiText
	{
		FText Label;
		FText ToolTip;
	};

	FVoxelPaintChannelMaskUiText ResolveChannelMaskUiText(int32 ChannelIndex,
		TConstArrayView<FVoxelPaintChannelContext> ChannelContexts);
}

class FVoxelPaintChannelMaskCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
