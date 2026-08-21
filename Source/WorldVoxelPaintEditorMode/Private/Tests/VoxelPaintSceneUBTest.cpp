// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "ShaderParameterMetadata.h"
#include "VoxelPaintSceneUniformBuffer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintSceneUB_MemberRegistered,
	"Game.WorldVoxelPaint.Unit.SceneUB.MemberRegistered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintSceneUB_MemberRegistered::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("SceneUB::VoxelPaint member was committed at engine init"), SceneUB::VoxelPaint.MemberId >= 0);

	const FShaderParametersMetadata* Metadata = FVoxelPaintSceneParameters::FTypeInfo::GetStructMetadata();
	if (TestNotNull(TEXT("FVoxelPaintSceneParameters metadata is resolvable"), Metadata))
	{
		TestEqual(TEXT("Metadata size matches the parameter struct"),
			static_cast<int32>(Metadata->GetSize()), static_cast<int32>(sizeof(FVoxelPaintSceneParameters)));
	}

	return true;
}
