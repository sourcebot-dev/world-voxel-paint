// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintDeveloperSettings.h"

#include "VoxelPaintGridSetup.h"

UVoxelPaintDeveloperSettings::UVoxelPaintDeveloperSettings()
{
	const TCHAR* ChannelNames[] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };
	ChannelContexts.Reserve(UE_ARRAY_COUNT(ChannelNames));
	for (const TCHAR* ChannelName : ChannelNames)
	{
		FVoxelPaintChannelContext& Context = ChannelContexts.AddDefaulted_GetRef();
		Context.DisplayName = ChannelName;
	}
}

FName UVoxelPaintDeveloperSettings::GetCategoryName() const
{
	return FName(TEXT("Plugins"));
}

int64 UVoxelPaintDeveloperSettings::GetBrickPoolCapacityBytes() const
{
#if WITH_EDITOR
	return static_cast<int64>(BrickPoolCapacityMiBEditor) << 20;
#else
	return static_cast<int64>(BrickPoolCapacityMiBCooked) << 20;
#endif
}

int64 UVoxelPaintDeveloperSettings::GetPageTableCapacityBytes() const
{
#if WITH_EDITOR
	return static_cast<int64>(PageTableCapacityMiBEditor) << 20;
#else
	return static_cast<int64>(PageTableCapacityMiBCooked) << 20;
#endif
}

#if WITH_EDITOR
int64 UVoxelPaintDeveloperSettings::GetJournalCapacityBytes() const
{
	return static_cast<int64>(JournalCapacityMiB) << 20;
}
#endif

#if WITH_EDITOR
void UVoxelPaintDeveloperSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	WorldVoxelPaint::OnDeveloperSettingsChanged().Broadcast();
}
#endif
