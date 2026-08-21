// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Engine/World.h"

namespace WorldVoxelPaint
{

// PIE/preview world copies preserve ActorGuid but live in renamed packages, so counting them
// would misread one placement as a package-copy clone.
inline bool IsEnumerableWorld(const UWorld* World)
{
	return World
		&& (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::Inactive);
}

} // namespace WorldVoxelPaint
