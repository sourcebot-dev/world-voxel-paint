// Copyright Epic Games, Inc. All Rights Reserved.

#include "VoxelPaintVolumeData.h"

#include "Engine/VolumeTexture.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"
#include "VoxelPaintCanonicalSource.h"
#include "WorldVoxelPaintConstants.h"

#if WITH_EDITOR
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Memory/SharedBuffer.h"
#include "Misc/ScopeExit.h"
#include "TextureCompiler.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "VoxelPaintAuthority.h"
#include "VoxelPaintVolumeComponent.h"
#include "VoxelPaintWorldSubsystem.h"
#include "Widgets/Notifications/SNotificationList.h"
#endif

#if WITH_EDITOR
namespace
{

void ShowVoxelPaintDataNotification(const FString& Message)
{
	if (FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(FText::FromString(Message));
		Info.ExpireDuration = 6.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
	else
	{
		UE_LOG(LogWorldVoxelPaint, Warning, TEXT("%s"), *Message);
	}
}

} // namespace
#endif

FIntVector UVoxelPaintVolumeData::GetResolution() const
{
	return BrickCounts * WorldVoxelPaint::UniqueBrickSize;
}

FVector UVoxelPaintVolumeData::GetTexelSize() const
{
	return (2.0 * LocalHalfExtent) / FVector(GetResolution());
}

uint64 UVoxelPaintVolumeData::GetSourceGeneration() const
{
	return SourceGeneration;
}

FVoxelPaintCanonicalIdentity UVoxelPaintVolumeData::GetCanonicalIdentity() const
{
	FVoxelPaintCanonicalIdentity Identity;
	if (CanonicalIdentityBytes.Num() == sizeof(Identity.Hash.Hash))
	{
		FMemory::Memcpy(Identity.Hash.Hash, CanonicalIdentityBytes.GetData(), sizeof(Identity.Hash.Hash));
	}
	return Identity;
}

TSharedPtr<const FVoxelPaintSourceManifest> UVoxelPaintVolumeData::GetManifestSnapshot() const
{
	return ManifestSnapshot;
}

FIntVector UVoxelPaintVolumeData::GetBrickCounts() const
{
	return BrickCounts;
}

FVector UVoxelPaintVolumeData::GetStorageMin() const
{
	return StorageOrigin - LocalHalfExtent;
}

void UVoxelPaintVolumeData::RefreshManifestSnapshot()
{
	TSharedRef<FVoxelPaintSourceManifest> NewSnapshot = MakeShared<FVoxelPaintSourceManifest>();
	NewSnapshot->BrickCounts = BrickCounts;
	NewSnapshot->PopulatedBricks = BrickManifest.PopulatedBricks;
	ManifestSnapshot = NewSnapshot;
}

TSharedPtr<FVoxelPaintPreparedSourceSnapshot> UVoxelPaintVolumeData::MakeSnapshotShell() const
{
	TSharedPtr<FVoxelPaintPreparedSourceSnapshot> Snapshot = MakeShared<FVoxelPaintPreparedSourceSnapshot>();
	Snapshot->AssetKey = FObjectKey(this);
	Snapshot->SourceGeneration = SourceGeneration;
	Snapshot->Identity = GetCanonicalIdentity();
	Snapshot->Manifest = ManifestSnapshot;
	Snapshot->AcquirePayload = [](TArray<uint8>&) { return false; };
	Snapshot->DebugName = GetPathName();
	return Snapshot;
}

void UVoxelPaintVolumeData::PostLoad()
{
	Super::PostLoad();
	++SourceGeneration;
	RefreshManifestSnapshot();
}

void UVoxelPaintVolumeData::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);
	++SourceGeneration;
	RefreshManifestSnapshot();
}

void UVoxelPaintVolumeData::Serialize(FArchive& Ar)
{
#if WITH_EDITOR
	const bool bCookingSave = Ar.IsCooking() && Ar.IsSaving();
	if (bCookingSave)
	{
		HarvestCookedPayload();
	}
#endif

	Super::Serialize(Ar);

	bool bCooked = false;
#if WITH_EDITOR
	bCooked = bCookingSave;
#endif
	Ar << bCooked;
	if (bCooked)
	{
		CookedBrickPayload.Serialize(Ar, this);
	}

#if WITH_EDITORONLY_DATA
	if (!bCooked)
	{
		PackedSourceBulk.Serialize(Ar, this);
	}
#endif

#if WITH_EDITOR
	if (bCookingSave)
	{
		CookedHeader = FVoxelPaintCookedPayloadHeader();
		CookedBrickPayload.RemoveBulkData();
	}
#endif
}

#if WITH_EDITOR

bool UVoxelPaintVolumeData::GetPackedSourceBytes(TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();
	const int64 PayloadSize = PackedSourceBulk.GetPayloadSize();
	if (PayloadSize <= 0)
	{
		return true;
	}
	FSharedBuffer Buffer = PackedSourceBulk.GetPayload().Get();
	if (Buffer.IsNull() || (int64)Buffer.GetSize() != PayloadSize)
	{
		return false;
	}
	OutBytes.SetNumUninitialized((int32)PayloadSize);
	FMemory::Memcpy(OutBytes.GetData(), Buffer.GetData(), PayloadSize);
	return true;
}

bool UVoxelPaintVolumeData::ValidatePackedSourceBytesAgainstManifest(int64 NumBytes,
	int32 NumManifestBricks, const TCHAR* UseSite) const
{
	const FIntVector DimsInBricks = WorldVoxelPaint::ComputePackedSourceDimsInBricks(NumManifestBricks);
	const FIntVector PhysTexels = DimsInBricks * WorldVoxelPaint::BrickSize;
	const int64 ExpectedBytes = (int64)PhysTexels.X * PhysTexels.Y * PhysTexels.Z * 4;
	if (NumBytes != ExpectedBytes)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("%s: %s: packed source size %lld does not match %d manifest bricks (expected %lld) - failing closed."),
			*GetPathName(), UseSite, NumBytes, NumManifestBricks, ExpectedBytes);
		return false;
	}
	return true;
}

UVolumeTexture* UVoxelPaintVolumeData::CreateBuildHelperTexture(TConstArrayView<uint8> PackedBytes) const
{
	const int32 NumPackedRecords = BrickManifest.PopulatedBricks.Num();
	const FIntVector DimsInBricks = WorldVoxelPaint::ComputePackedSourceDimsInBricks(NumPackedRecords);
	const FIntVector PhysTexels = DimsInBricks * WorldVoxelPaint::BrickSize;
	const int64 ExpectedBytes = (int64)PhysTexels.X * PhysTexels.Y * PhysTexels.Z * 4;
	if ((int64)PackedBytes.Num() != ExpectedBytes)
	{
		return nullptr;
	}

	UVolumeTexture* Helper = NewObject<UVolumeTexture>(GetTransientPackage(), NAME_None, RF_Transient);
	Helper->CompressionSettings = TC_BC7;
	Helper->SRGB = false;
	Helper->NeverStream = true;
	Helper->MipGenSettings = TMGS_NoMipmaps;
	Helper->Source.Init(PhysTexels.X, PhysTexels.Y, PhysTexels.Z, 1, TSF_BGRA8, PackedBytes.GetData());
	return Helper;
}

void UVoxelPaintVolumeData::ReleaseAdmissionHelper()
{
	if (AdmissionHelperTexture)
	{
		if (!FTextureCompilingManager::Get().IsCompilingTexture(AdmissionHelperTexture))
		{
			AdmissionHelperTexture->ReleaseResource();
		}
		AdmissionHelperTexture = nullptr;
	}
	AdmissionHelperGeneration = 0;
}

void UVoxelPaintVolumeData::BeginCacheForCookedPlatformData(const ITargetPlatform* TargetPlatform)
{
	Super::BeginCacheForCookedPlatformData(TargetPlatform);
	if (BrickManifest.PopulatedBricks.Num() == 0)
	{
		return;
	}
	if (!CookHelperTexture)
	{
		TArray<uint8> PackedBytes;
		if (GetPackedSourceBytes(PackedBytes))
		{
			CookHelperTexture = CreateBuildHelperTexture(PackedBytes);
		}
		if (!CookHelperTexture)
		{
			UE_LOG(LogWorldVoxelPaint, Error,
				TEXT("%s: canonical source bytes missing or mis-sized for %d manifest bricks - cook harvest will be empty"),
				*GetPathName(), BrickManifest.PopulatedBricks.Num());
			return;
		}
	}
	CookHelperTexture->BeginCacheForCookedPlatformData(TargetPlatform);
}

bool UVoxelPaintVolumeData::IsCachedCookedPlatformDataLoaded(const ITargetPlatform* TargetPlatform)
{
	if (CookHelperTexture)
	{
		return CookHelperTexture->IsCachedCookedPlatformDataLoaded(TargetPlatform);
	}
	return true;
}

void UVoxelPaintVolumeData::ClearCachedCookedPlatformData(const ITargetPlatform* TargetPlatform)
{
	if (CookHelperTexture)
	{
		CookHelperTexture->ClearCachedCookedPlatformData(TargetPlatform);
	}
	Super::ClearCachedCookedPlatformData(TargetPlatform);
}

void UVoxelPaintVolumeData::ClearAllCachedCookedPlatformData()
{
	if (CookHelperTexture)
	{
		CookHelperTexture->ClearAllCachedCookedPlatformData();
		CookHelperTexture = nullptr;
	}
	Super::ClearAllCachedCookedPlatformData();
}

void UVoxelPaintVolumeData::HarvestCookedPayload()
{
	CookedHeader = FVoxelPaintCookedPayloadHeader();
	CookedBrickPayload.RemoveBulkData();

	const int32 NumPackedRecords = BrickManifest.PopulatedBricks.Num();
	if (NumPackedRecords == 0)
	{
		return;
	}

	if (!CookHelperTexture)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("%s: cooked BC7 harvest failed (no build-helper texture for %d manifest bricks) - cooking empty payload"),
			*GetPathName(), NumPackedRecords);
		return;
	}

	TMap<FString, FTexturePlatformData*>* CookedPlatformData = CookHelperTexture->GetCookedPlatformData();
	if (!CookedPlatformData || CookedPlatformData->IsEmpty())
	{
		UE_LOG(LogWorldVoxelPaint, Log,
			TEXT("%s: no cooked texture platform data for this target - cooking empty payload"), *GetPathName());
		return;
	}

	FTexturePlatformData* Chosen = nullptr;
	WorldVoxelPaint::EVoxelPaintPayloadVerdict FirstVerdict = WorldVoxelPaint::EVoxelPaintPayloadVerdict::WrongFormat;
	bool bFirstVerdictSet = false;
	for (const TPair<FString, FTexturePlatformData*>& Pair : *CookedPlatformData)
	{
		FTexturePlatformData* PlatformData = Pair.Value;
		if (!PlatformData || PlatformData->Mips.Num() == 0)
		{
			continue;
		}
		PlatformData->TryInlineMipData(0, GetPathName());
		FTexture2DMipMap& Mip0 = PlatformData->Mips[0];
		const WorldVoxelPaint::EVoxelPaintPayloadVerdict Verdict = WorldVoxelPaint::ClassifyBuiltPayload(
			PlatformData->PixelFormat, FIntVector(Mip0.SizeX, Mip0.SizeY, Mip0.SizeZ),
			NumPackedRecords, Mip0.BulkData.GetBulkDataSize());
		if (Verdict == WorldVoxelPaint::EVoxelPaintPayloadVerdict::Valid)
		{
			Chosen = PlatformData;
			break;
		}
		if (!bFirstVerdictSet)
		{
			FirstVerdict = Verdict;
			bFirstVerdictSet = true;
		}
	}

	if (!Chosen)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("%s: cooked BC7 harvest failed (%s) - cooking empty payload"),
			*GetPathName(), WorldVoxelPaint::LexToString(FirstVerdict));
		return;
	}

	FTexture2DMipMap& Mip0 = Chosen->Mips[0];
	const int64 PayloadBytes = Mip0.BulkData.GetBulkDataSize();
	const void* MipData = Mip0.BulkData.LockReadOnly();
	if (!MipData)
	{
		Mip0.BulkData.Unlock();
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("%s: cooked BC7 harvest failed (mip data unavailable) - cooking empty payload"), *GetPathName());
		return;
	}

	CookedBrickPayload.Lock(LOCK_READ_WRITE);
	void* Dest = CookedBrickPayload.Realloc(PayloadBytes);
	FMemory::Memcpy(Dest, MipData, PayloadBytes);
	CookedBrickPayload.Unlock();
	CookedBrickPayload.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);
	Mip0.BulkData.Unlock();

	CookedHeader.LayoutVersion = WorldVoxelPaint::CanonicalLayoutVersion;
	CookedHeader.NumPackedRecords = (uint32)NumPackedRecords;
	CookedHeader.PayloadBytes = (uint64)PayloadBytes;
}

namespace WorldVoxelPaint
{

TMulticastDelegate<void(UVoxelPaintVolumeData*)>& OnVolumeDataCommitted()
{
	static TMulticastDelegate<void(UVoxelPaintVolumeData*)> Delegate;
	return Delegate;
}

}

void UVoxelPaintVolumeData::InstallCanonicalState(const FVoxelPaintCanonicalPayload& Payload)
{
	if (Payload.bInitialized)
	{
		if (!ValidatePackedSourceBytesAgainstManifest(Payload.PackedSourceBGRA8.Num(),
			Payload.ManifestBricks.Num(), TEXT("InstallCanonicalState")))
		{
			return;
		}
		if (Payload.IdentityBytes.Num() != 0
			&& Payload.IdentityBytes.Num() != (int32)sizeof(FSHAHash().Hash))
		{
			UE_LOG(LogWorldVoxelPaint, Error,
				TEXT("%s: InstallCanonicalState: identity must be %d bytes or empty (got %d) - failing closed."),
				*GetPathName(), (int32)sizeof(FSHAHash().Hash), Payload.IdentityBytes.Num());
			return;
		}
	}
	else if (Payload.PackedSourceBGRA8.Num() != 0 || Payload.ManifestBricks.Num() != 0
		|| Payload.IdentityBytes.Num() != 0)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("%s: InstallCanonicalState: an uninitialized payload must carry no source, manifest, or identity - failing closed."),
			*GetPathName());
		return;
	}

	LocalHalfExtent = Payload.LocalHalfExtent;
	BrickCounts = Payload.BrickCounts;
	StorageOrigin = Payload.StorageOrigin;
	BrickManifest.PopulatedBricks = Payload.ManifestBricks;
	if (Payload.PackedSourceBGRA8.Num() > 0)
	{
		PackedSourceBulk.UpdatePayload(FSharedBuffer::Clone(
			Payload.PackedSourceBGRA8.GetData(), Payload.PackedSourceBGRA8.Num()), this);
	}
	else
	{
		PackedSourceBulk.Reset();
	}
	CanonicalIdentityBytes = Payload.IdentityBytes;
	bInitialized = Payload.bInitialized;

	++SourceGeneration;
	ReleaseAdmissionHelper();
	RefreshManifestSnapshot();
	MarkPackageDirty();
	NotifyConsumingComponents();
	WorldVoxelPaint::OnVolumeDataCommitted().Broadcast(this);
}

void UVoxelPaintVolumeData::CaptureCanonicalPayload(FVoxelPaintCanonicalPayload& Out) const
{
	if (!GetPackedSourceBytes(Out.PackedSourceBGRA8))
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("%s: CaptureCanonicalPayload: packed source payload unavailable - captured empty source."),
			*GetPathName());
		Out.PackedSourceBGRA8.Reset();
	}
	Out.ManifestBricks = BrickManifest.PopulatedBricks;
	Out.IdentityBytes = CanonicalIdentityBytes;
	Out.BrickCounts = BrickCounts;
	Out.LocalHalfExtent = LocalHalfExtent;
	Out.StorageOrigin = StorageOrigin;
	Out.bInitialized = bInitialized;
}

void UVoxelPaintVolumeData::ResetCanonicalData()
{
	FVoxelPaintCanonicalPayload Payload;
	Payload.BrickCounts = BrickCounts;
	Payload.LocalHalfExtent = LocalHalfExtent;
	Payload.StorageOrigin = StorageOrigin;
	Payload.bInitialized = false;
	InstallCanonicalState(Payload);
}

void UVoxelPaintVolumeData::CommitCanonicalSource(const WorldVoxelPaint::FVoxelPaintCanonicalBuild& Build)
{
	const FVoxelPaintCanonicalIdentity Identity =
		WorldVoxelPaint::ComputeCanonicalIdentity(Build, StorageOrigin, LocalHalfExtent);

	FVoxelPaintCanonicalPayload Payload;
	Payload.PackedSourceBGRA8 = Build.PackedSourceBGRA8;
	Payload.ManifestBricks = Build.Manifest.PopulatedBricks;
	Payload.IdentityBytes.SetNumUninitialized(sizeof(Identity.Hash.Hash));
	FMemory::Memcpy(Payload.IdentityBytes.GetData(), Identity.Hash.Hash, sizeof(Identity.Hash.Hash));
	Payload.BrickCounts = Build.Manifest.BrickCounts;
	Payload.LocalHalfExtent = LocalHalfExtent;
	Payload.StorageOrigin = StorageOrigin;
	Payload.bInitialized = true;
	InstallCanonicalState(Payload);
}

void UVoxelPaintVolumeData::RecomputeCanonicalIdentityFromStored()
{
	WorldVoxelPaint::FVoxelPaintCanonicalBuild Build;
	Build.Manifest.BrickCounts = BrickCounts;
	Build.Manifest.PopulatedBricks = BrickManifest.PopulatedBricks;
	Build.PhysicalDimsInBricks =
		WorldVoxelPaint::ComputePackedSourceDimsInBricks(BrickManifest.PopulatedBricks.Num());
	if (!GetPackedSourceBytes(Build.PackedSourceBGRA8)
		|| !ValidatePackedSourceBytesAgainstManifest(Build.PackedSourceBGRA8.Num(),
			BrickManifest.PopulatedBricks.Num(), TEXT("RecomputeCanonicalIdentityFromStored")))
	{
		return;
	}
	const FVoxelPaintCanonicalIdentity Identity =
		WorldVoxelPaint::ComputeCanonicalIdentity(Build, StorageOrigin, LocalHalfExtent);
	CanonicalIdentityBytes.SetNumUninitialized(sizeof(Identity.Hash.Hash));
	FMemory::Memcpy(CanonicalIdentityBytes.GetData(), Identity.Hash.Hash, sizeof(Identity.Hash.Hash));
}

void UVoxelPaintVolumeData::NotifyConsumingComponents(const FVoxelPaintTopologyTriplet* OldTopology)
{
	TArray<UVoxelPaintVolumeComponent*> Consumers;
	for (TObjectIterator<UVoxelPaintVolumeComponent> It; It; ++It)
	{
		UVoxelPaintVolumeComponent* Component = *It;
		if (Component && !Component->IsTemplate() && Component->PoolContains(this))
		{
			Consumers.Add(Component);
		}
	}

	if (OldTopology && Consumers.Num() == 1)
	{
		UVoxelPaintVolumeComponent* Component = Consumers[0];
		const bool bPlacementOwned =
			Component->CreationMethod == EComponentCreationMethod::Native
			|| Component->CreationMethod == EComponentCreationMethod::Instance;
		bool bSoleDistinctEntry = true;
		for (const FVoxelPaintVariantPoolEntry& Entry : Component->VolumeVariants)
		{
			if (Entry.Variant && Entry.Variant != this)
			{
				bSoleDistinctEntry = false;
				break;
			}
		}
		if (bPlacementOwned && bSoleDistinctEntry
			&& Component->ExpectedTopology.bIsSet
			&& Component->ExpectedTopology.LocalHalfExtent == OldTopology->LocalHalfExtent
			&& Component->ExpectedTopology.BrickCounts == OldTopology->BrickCounts
			&& Component->ExpectedTopology.StorageOrigin == OldTopology->StorageOrigin)
		{
			Component->Modify();
			Component->ExpectedTopology.LocalHalfExtent = LocalHalfExtent;
			Component->ExpectedTopology.BrickCounts = BrickCounts;
			Component->ExpectedTopology.StorageOrigin = StorageOrigin;
		}
	}

	for (UVoxelPaintVolumeComponent* Component : Consumers)
	{
		if (UVoxelPaintWorldSubsystem* Subsystem = UVoxelPaintWorldSubsystem::Get(Component->GetWorld()))
		{
			Subsystem->NotifyPropertyChanged(Component);
		}
	}
}

void UVoxelPaintVolumeData::PreEditChange(FProperty* PropertyAboutToChange)
{
	Super::PreEditChange(PropertyAboutToChange);
	PreEditLocalHalfExtent = LocalHalfExtent;
	PreEditBrickCounts = BrickCounts;
	PreEditStorageOrigin = StorageOrigin;
}

void UVoxelPaintVolumeData::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName MemberName = PropertyChangedEvent.MemberProperty
		? PropertyChangedEvent.MemberProperty->GetFName()
		: PropertyChangedEvent.GetPropertyName();
	const bool bNamedTopologyProperty =
		MemberName == GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeData, LocalHalfExtent)
		|| MemberName == GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeData, BrickCounts)
		|| MemberName == GET_MEMBER_NAME_CHECKED(UVoxelPaintVolumeData, StorageOrigin);
	if (MemberName != NAME_None && !bNamedTopologyProperty)
	{
		return;
	}

	// Undo/redo routes here with a NAME_None event and stale PreEdit caches; the freeze revert
	// therefore keys strictly on explicitly named topology edits, which always follow a paired
	// PreEditChange.
	const bool bTopologyChanged = bNamedTopologyProperty
		&& (!LocalHalfExtent.Equals(PreEditLocalHalfExtent, 0.0)
			|| BrickCounts != PreEditBrickCounts
			|| !StorageOrigin.Equals(PreEditStorageOrigin, 0.0));
	if (bTopologyChanged)
	{
		const TCHAR* RevertReason = nullptr;
		if (bInitialized)
		{
			RevertReason = TEXT("topology is frozen at first paint; use Reset Data and Unlock Size to change size or detail");
		}
		else if (WorldVoxelPaint::IsAssetInOpenSession(this))
		{
			RevertReason = TEXT("a paint session is open on this asset; Finish or Discard it first");
		}
		if (RevertReason)
		{
			LocalHalfExtent = PreEditLocalHalfExtent;
			BrickCounts = PreEditBrickCounts;
			StorageOrigin = PreEditStorageOrigin;
			if (PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
			{
				ShowVoxelPaintDataNotification(FString::Printf(
					TEXT("%s: edit reverted - %s."), *GetName(), RevertReason));
			}
			return;
		}
	}

	if (CanonicalIdentityBytes.Num() > 0)
	{
		RecomputeCanonicalIdentityFromStored();
	}
	++SourceGeneration;
	ReleaseAdmissionHelper();
	RefreshManifestSnapshot();
	if (bTopologyChanged)
	{
		FVoxelPaintTopologyTriplet OldTopology;
		OldTopology.LocalHalfExtent = PreEditLocalHalfExtent;
		OldTopology.BrickCounts = PreEditBrickCounts;
		OldTopology.StorageOrigin = PreEditStorageOrigin;
		NotifyConsumingComponents(&OldTopology);
	}
	else
	{
		NotifyConsumingComponents();
	}
}

TSharedPtr<FVoxelPaintPreparedSourceSnapshot> UVoxelPaintVolumeData::CreatePreparationSnapshot_Editor(bool* bOutCapturePending)
{
	if (!ManifestSnapshot || !GetCanonicalIdentity().IsValid())
	{
		return nullptr;
	}

	const int32 NumPackedRecords = ManifestSnapshot->PopulatedBricks.Num();
	if (NumPackedRecords == 0)
	{
		TSharedPtr<FVoxelPaintPreparedSourceSnapshot> Snapshot = MakeSnapshotShell();
		Snapshot->AcquirePayload = [](TArray<uint8>& OutBrickPayload)
		{
			OutBrickPayload.Reset();
			return true;
		};
		return Snapshot;
	}

	if (AdmissionHelperTexture && AdmissionHelperGeneration != SourceGeneration)
	{
		ReleaseAdmissionHelper();
	}

	if (!AdmissionHelperTexture)
	{
		TArray<uint8> PackedBytes;
		UVolumeTexture* Helper = nullptr;
		if (GetPackedSourceBytes(PackedBytes))
		{
			Helper = CreateBuildHelperTexture(PackedBytes);
		}
		if (!Helper)
		{
			UE_LOG(LogWorldVoxelPaint, Error,
				TEXT("%s: canonical source bytes missing or mis-sized for %d manifest bricks - preparation fails closed"),
				*GetPathName(), NumPackedRecords);
			return MakeSnapshotShell();
		}
		AdmissionHelperTexture = Helper;
		AdmissionHelperGeneration = SourceGeneration;
		AdmissionHelperTexture->UpdateResource();
	}

	if (FTextureCompilingManager::Get().IsCompilingTexture(AdmissionHelperTexture))
	{
		if (bOutCapturePending)
		{
			*bOutCapturePending = true;
		}
		return nullptr;
	}

	TSharedPtr<FVoxelPaintPreparedSourceSnapshot> Snapshot = MakeSnapshotShell();
	// The transient second GPU copy is scoped to the build: end the helper's resource lifetime
	// at the harvest point instead of the next GC pass.
	ON_SCOPE_EXIT { ReleaseAdmissionHelper(); };

	FTexturePlatformData* PlatformData = AdmissionHelperTexture->GetPlatformData();
	if (!PlatformData || PlatformData->Mips.Num() == 0)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("%s: build-helper BC7 platform data missing - preparation fails closed"), *GetPathName());
		return Snapshot;
	}
	PlatformData->TryInlineMipData(0, GetPathName());

	FTexture2DMipMap& Mip0 = PlatformData->Mips[0];
	const WorldVoxelPaint::EVoxelPaintPayloadVerdict Verdict = WorldVoxelPaint::ClassifyBuiltPayload(
		PlatformData->PixelFormat, FIntVector(Mip0.SizeX, Mip0.SizeY, Mip0.SizeZ),
		NumPackedRecords, Mip0.BulkData.GetBulkDataSize());
	if (Verdict != WorldVoxelPaint::EVoxelPaintPayloadVerdict::Valid)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("%s: build-helper BC7 platform data invalid (%s) - preparation fails closed"),
			*GetPathName(), WorldVoxelPaint::LexToString(Verdict));
		return Snapshot;
	}

	TSharedPtr<TArray<uint8>> BrickBlob = MakeShared<TArray<uint8>>();
	const void* MipData = Mip0.BulkData.LockReadOnly();
	const bool bExtracted = MipData && WorldVoxelPaint::ExtractPackedBrickPayloads(
		MakeArrayView(reinterpret_cast<const uint8*>(MipData), (int32)Mip0.BulkData.GetBulkDataSize()),
		WorldVoxelPaint::ComputePackedSourceDimsInBricks(NumPackedRecords), NumPackedRecords, *BrickBlob);
	Mip0.BulkData.Unlock();

	if (!bExtracted)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("%s: build-helper BC7 brick extraction failed - preparation fails closed"), *GetPathName());
		return Snapshot;
	}

	Snapshot->AcquirePayload = [BrickBlob](TArray<uint8>& OutBrickPayload)
	{
		OutBrickPayload = *BrickBlob;
		return true;
	};
	return Snapshot;
}

#else

TSharedPtr<FVoxelPaintPreparedSourceSnapshot> UVoxelPaintVolumeData::CreatePreparationSnapshot_Cooked()
{
	if (!ManifestSnapshot)
	{
		return nullptr;
	}
	if (CookedHeader.NumPackedRecords == 0 || CookedHeader.PayloadBytes == 0)
	{
		return nullptr;
	}

	TSharedPtr<FVoxelPaintPreparedSourceSnapshot> Snapshot = MakeSnapshotShell();
	const int32 NumPackedRecords = ManifestSnapshot->PopulatedBricks.Num();
	const FIntVector DimsInBricks = WorldVoxelPaint::ComputePackedSourceDimsInBricks(NumPackedRecords);
	const int64 ExpectedBytes =
		(int64)DimsInBricks.X * DimsInBricks.Y * DimsInBricks.Z * WorldVoxelPaint::BrickPayloadBytes;

	if (CookedHeader.LayoutVersion != WorldVoxelPaint::CanonicalLayoutVersion
		|| CookedHeader.NumPackedRecords != (uint32)NumPackedRecords
		|| CookedHeader.PayloadBytes != (uint64)ExpectedBytes)
	{
		UE_LOG(LogWorldVoxelPaint, Error,
			TEXT("%s: cooked payload header mismatch (version %u records %u bytes %llu, expected %u/%d/%lld) - preparation fails closed"),
			*GetPathName(), CookedHeader.LayoutVersion, CookedHeader.NumPackedRecords, CookedHeader.PayloadBytes,
			WorldVoxelPaint::CanonicalLayoutVersion, NumPackedRecords, ExpectedBytes);
		return Snapshot;
	}

	Snapshot->AcquirePayload =
		[Pin = TStrongObjectPtr<UVoxelPaintVolumeData>(this), DimsInBricks, NumPackedRecords, ExpectedBytes]
		(TArray<uint8>& OutBrickPayload)
	{
		UVoxelPaintVolumeData* Data = Pin.Get();
		if (!Data)
		{
			return false;
		}
		FByteBulkData& Bulk = Data->CookedBrickPayload;
		if (Bulk.GetBulkDataSize() != ExpectedBytes)
		{
			return false;
		}
		void* Raw = nullptr;
		Bulk.GetCopy(&Raw, false);
		if (!Raw)
		{
			return false;
		}
		const bool bExtracted = WorldVoxelPaint::ExtractPackedBrickPayloads(
			MakeArrayView(reinterpret_cast<const uint8*>(Raw), (int32)ExpectedBytes),
			DimsInBricks, NumPackedRecords, OutBrickPayload);
		FMemory::Free(Raw);
		return bExtracted;
	};
	return Snapshot;
}

#endif

TSharedPtr<FVoxelPaintPreparedSourceSnapshot> UVoxelPaintVolumeData::CreatePreparationSnapshot(bool* bOutCapturePending)
{
	if (bOutCapturePending)
	{
		*bOutCapturePending = false;
	}
#if WITH_EDITOR
	return CreatePreparationSnapshot_Editor(bOutCapturePending);
#else
	return CreatePreparationSnapshot_Cooked();
#endif
}
