// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

// Details customization for UVoxelPaintSizingToolProperties. Orders the categories in
// dependency order (asset, inputs, derived, cost, status), places the Snap button directly
// under Desired Texel Size, renders Status as a wrapped color-coded row, and puts the
// destructive Reset button under the status text with an availability-aware tooltip.
class FVoxelPaintSizingDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};
