// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintTopologyLock.h"

namespace WorldVoxelPaint
{

void FVoxelPaintTopologyLockMachine::Reset(bool bAssetDurablyInitialized)
{
	State = bAssetDurablyInitialized
		? EVoxelPaintTopologyState::DurablyInitialized
		: EVoxelPaintTopologyState::Uninitialized;
}

EVoxelPaintTopologyState FVoxelPaintTopologyLockMachine::GetState() const
{
	return State;
}

void FVoxelPaintTopologyLockMachine::OnOpAdmitted(bool bNonZeroWriteIntent)
{
	if (State == EVoxelPaintTopologyState::Uninitialized && bNonZeroWriteIntent)
	{
		State = EVoxelPaintTopologyState::SessionLocalInitialized;
	}
}

void FVoxelPaintTopologyLockMachine::OnFinishInstalled()
{
	if (State == EVoxelPaintTopologyState::SessionLocalInitialized)
	{
		State = EVoxelPaintTopologyState::DurablyInitialized;
	}
}

void FVoxelPaintTopologyLockMachine::OnDiscardConfirmed()
{
	if (State == EVoxelPaintTopologyState::SessionLocalInitialized)
	{
		State = EVoxelPaintTopologyState::Uninitialized;
	}
}

void FVoxelPaintTopologyLockMachine::OnForcedTeardownNeverCommitted()
{
	if (State == EVoxelPaintTopologyState::SessionLocalInitialized)
	{
		State = EVoxelPaintTopologyState::Uninitialized;
	}
}

void FVoxelPaintTopologyLockMachine::OnResetDataAndUnlock()
{
	if (State == EVoxelPaintTopologyState::DurablyInitialized)
	{
		State = EVoxelPaintTopologyState::Uninitialized;
	}
}

void FVoxelPaintTopologyLockMachine::OnUndoOfFirstFinish()
{
	if (State == EVoxelPaintTopologyState::DurablyInitialized)
	{
		State = EVoxelPaintTopologyState::Uninitialized;
	}
}

bool FVoxelPaintTopologyLockMachine::CanEditTopology(bool bSessionOpenOnAsset) const
{
	return State == EVoxelPaintTopologyState::Uninitialized && !bSessionOpenOnAsset;
}

}
