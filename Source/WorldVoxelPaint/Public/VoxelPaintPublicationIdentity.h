// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FVoxelPaintPublicationIdentity
{
	uint64 OwnerEpoch = 0;
	uint64 LayoutGeneration = 0;
	uint64 BindingGeneration = 0;

	bool operator==(const FVoxelPaintPublicationIdentity& Other) const
	{
		return OwnerEpoch == Other.OwnerEpoch
			&& LayoutGeneration == Other.LayoutGeneration
			&& BindingGeneration == Other.BindingGeneration;
	}

	bool operator!=(const FVoxelPaintPublicationIdentity& Other) const
	{
		return !(*this == Other);
	}

	void OnEpochAdvance(uint64 NewEpoch)
	{
		OwnerEpoch = NewEpoch;
		LayoutGeneration = 1;
		BindingGeneration = 1;
	}

	void OnRegistryLayoutChange()
	{
		++LayoutGeneration;
	}

	void OnGuardConsumed()
	{
		++LayoutGeneration;
	}

	void OnZEnvelopeOrSettingsChange()
	{
		++LayoutGeneration;
	}

	void OnRegistryBindingChange()
	{
		++BindingGeneration;
	}

	void OnResidencyBindingEvent()
	{
		++BindingGeneration;
	}

	void OnSessionBindingEvent()
	{
		++BindingGeneration;
	}
};
