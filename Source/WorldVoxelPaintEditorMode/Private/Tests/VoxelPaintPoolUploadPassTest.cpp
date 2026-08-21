// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "VoxelPaintPoolRenderer.h"
#include "VoxelPaintResidencyTypes.h"
#include "WorldVoxelPaintConstants.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVoxelPaintPoolRenderer_BrickUploadPass,
	"Game.WorldVoxelPaint.Unit.PoolRenderer.BrickUploadPass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVoxelPaintPoolRenderer_BrickUploadPass::RunTest(const FString& Parameters)
{
	FVoxelPaintIntegrationOps Ops;
	FVoxelPaintBrickUploadOp& Upload = Ops.BrickUploads.AddDefaulted_GetRef();
	TSharedPtr<TArray<uint8>> Payload = MakeShared<TArray<uint8>>();
	Payload->SetNumZeroed(WorldVoxelPaint::BrickPayloadBytes);
	Upload.Payload = Payload;
	Upload.PoolSlot = 0;
	Upload.PayloadBrickIndex = 0;

	bool bPassExecuted = false;
	ENQUEUE_RENDER_COMMAND(WorldVoxelPaintBrickUploadPassTest)(
		[Ops = MoveTemp(Ops), &bPassExecuted](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("WorldVoxelPaint.BrickUploadPassTest"));
			FVoxelPaintPoolRenderer Renderer;
			Renderer.Initialize(GraphBuilder, WorldVoxelPaint::BrickPayloadBytes, sizeof(uint32));
			Renderer.ExecuteOps(GraphBuilder, Ops);
			GraphBuilder.Execute();
			bPassExecuted = true;
		});
	FlushRenderingCommands();

	TestTrue(TEXT("Brick-upload pass builds and executes under RDG validation"), bPassExecuted);
	return true;
}
