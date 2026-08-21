// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IPIEAuthorizer.h"
#include "Misc/Optional.h"
#include "VoxelPaintSessionMachine.h"

class FVoxelPaintPIEAuthorizer : public IPIEAuthorizer
{
public:
	virtual bool RequestPIEPermission(bool bIsSimulateInEditor, FString& OutReason) const override;

	void SetStageOverrideForTest(WorldVoxelPaint::EVoxelPaintSessionStage Stage);
	void ClearStageOverrideForTest();

private:
	TOptional<WorldVoxelPaint::EVoxelPaintSessionStage> StageOverrideForTest;
};
