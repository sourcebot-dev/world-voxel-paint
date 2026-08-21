// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace WorldVoxelPaint
{
	enum class EVoxelPaintTopologyState : uint8
	{
		Uninitialized,
		SessionLocalInitialized,
		DurablyInitialized
	};

	class WORLDVOXELPAINTTOOLSET_API FVoxelPaintTopologyLockMachine
	{
	public:
		void Reset(bool bAssetDurablyInitialized);
		EVoxelPaintTopologyState GetState() const;

		void OnOpAdmitted(bool bNonZeroWriteIntent);
		void OnFinishInstalled();
		void OnDiscardConfirmed();
		void OnForcedTeardownNeverCommitted();
		void OnResetDataAndUnlock();
		void OnUndoOfFirstFinish();
		bool CanEditTopology(bool bSessionOpenOnAsset) const;

	private:
		EVoxelPaintTopologyState State = EVoxelPaintTopologyState::Uninitialized;
	};
}
