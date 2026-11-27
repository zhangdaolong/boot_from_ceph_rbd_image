/** @file
  Minimal read-only FAT12/FAT16 loader used by the firmware-side boot chain.

  The RBD GPT/ESP handoff registers a Boot#### option whose device path is a
  synthetic HD(GPT) node. In a firmware that loaded the driver during DXE the
  platform partition/FAT drivers would create child handles and LoadImage(by
  device path) would resolve. When the driver is loaded from the UEFI shell
  those child handles do not exist, so the same load option returns Not Found.

  To make the handoff self-contained, this module reads \EFI\BOOT\BOOTX64.EFI
  straight out of the ESP sectors (via the driver's BlockIo read callback) and
  returns a freshly allocated buffer. The caller passes that buffer to
  gBS->LoadImage(SourceBuffer=...), which needs no partition child handle.

  The subset needed for a UEFI ESP is implemented: FAT12/FAT16/FAT32,
  512-byte sectors, short (8.3) directory entries, contiguous and chained
  clusters. FAT32 is used by most modern OS installers for the ESP. Long file
  names and writes are out of scope.
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#include "CephRbdEspFat.h"

#define FAT_DIR_ENTRY_SIZE        32
#define FAT_ATTR_DIRECTORY        0x10
#define FAT_ATTR_LONG_NAME        0x0F
#define FAT_END_OF_CHAIN_MIN      0xFFF8

typedef struct {
  UINT16  BytesPerSector;
  UINT8   SectorsPerCluster;
  UINT16  ReservedSectors;
  UINT8   NumFats;
  UINT16  RootEntryCount;
  UINT16  FatSize16;      // sectors per FAT (FAT12/16)
  UINT32  FatSize32;      // sectors per FAT (FAT32)
  UINT32  RootCluster;    // root dir cluster (FAT32)
  UINT32  RootDirSectors; // root dir sectors (FAT12/16, 0 for FAT32)
  UINT32  DataSectors;
  UINT32  ClusterCount;
  BOOLEAN IsFat32;
  BOOLEAN IsFat12;
} CEPH_RBD_FAT_VOLUME;

STATIC
UINT16
FatReadLe16 (
  IN CONST UINT8  *Buffer
  )
{
  return (UINT16)(Buffer[0] | ((UINT16)Buffer[1] << 8));
}

STATIC
UINT32
FatReadLe32 (
  IN CONST UINT8  *Buffer
  )
{
  return (UINT32)Buffer[0] |
         ((UINT32)Buffer[1] << 8) |
         ((UINT32)Buffer[2] << 16) |
         ((UINT32)Buffer[3] << 24);
}

/** Parse the FAT boot sector at the start of the partition. */
STATIC
EFI_STATUS
FatParseVolume (
  IN  CONST UINT8          *BootSector,
  IN  UINTN                 BufferSize,
  OUT CEPH_RBD_FAT_VOLUME   *Vol
  )
{
  UINT32  TotSec16;
  UINT32  TotSec32;
  UINT32  TotSec;
  UINT32  RootDirSectors;
  UINT32  FatsSize;

  if ((BootSector == NULL) || (Vol == NULL) ||
      (BufferSize < 64))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Vol, sizeof (*Vol));
  Vol->BytesPerSector = FatReadLe16 (BootSector + 11);
  Vol->SectorsPerCluster = BootSector[13];
  Vol->ReservedSectors = FatReadLe16 (BootSector + 14);
  Vol->NumFats = BootSector[16];
  Vol->RootEntryCount = FatReadLe16 (BootSector + 17);
  TotSec16 = FatReadLe16 (BootSector + 19);
  Vol->FatSize16 = FatReadLe16 (BootSector + 22);
  Vol->FatSize32 = FatReadLe32 (BootSector + 36);
  Vol->RootCluster = FatReadLe32 (BootSector + 44);
  TotSec32 = FatReadLe32 (BootSector + 32);
  TotSec = (TotSec16 != 0) ? TotSec16 : TotSec32;

  if ((Vol->BytesPerSector != 512) || (Vol->SectorsPerCluster == 0) ||
      (Vol->NumFats == 0) || (TotSec == 0))
  {
    return EFI_VOLUME_CORRUPTED;
  }

  if (Vol->FatSize16 != 0) {
    Vol->IsFat32 = FALSE;
    FatsSize = (UINT32)Vol->NumFats * Vol->FatSize16;
  } else {
    Vol->IsFat32 = TRUE;
    FatsSize = (UINT32)Vol->NumFats * Vol->FatSize32;
  }

  RootDirSectors = Vol->IsFat32 ? 0 :
      ((UINT32)Vol->RootEntryCount * FAT_DIR_ENTRY_SIZE +
       Vol->BytesPerSector - 1) / Vol->BytesPerSector;
  Vol->RootDirSectors = RootDirSectors;
  if (TotSec <= Vol->ReservedSectors + FatsSize + RootDirSectors) {
    return EFI_VOLUME_CORRUPTED;
  }

  Vol->DataSectors = TotSec -
      (Vol->ReservedSectors + FatsSize + RootDirSectors);
  Vol->ClusterCount = Vol->DataSectors / Vol->SectorsPerCluster;
  Vol->IsFat12 = (Vol->ClusterCount < 4085);
  return EFI_SUCCESS;
}

/** Return the sector of a given cluster relative to partition start. */
STATIC
UINT32
FatClusterToSector (
  IN CONST CEPH_RBD_FAT_VOLUME  *Vol,
  IN UINT32                      Cluster
  )
{
  return Vol->ReservedSectors +
         (UINT32)Vol->NumFats * (Vol->IsFat32 ? Vol->FatSize32 : Vol->FatSize16) +
         Vol->RootDirSectors +
         (Cluster - 2) * Vol->SectorsPerCluster;
}

/** Read one FAT entry (12/16/32 bit) for the given cluster. */
STATIC
EFI_STATUS
FatGetNextCluster (
  IN  CEPH_RBD_FAT_READ_LBA    ReadLba,
  IN  VOID                    *Context,
  IN  CONST CEPH_RBD_FAT_VOLUME  *Vol,
  IN  UINT64                   PartitionStart,
  IN  UINT32                   Cluster,
  OUT UINT32                   *Next
  )
{
  EFI_STATUS  Status;
  UINTN       FatOffset;
  UINT32      FatSectorRel;
  UINT8       Sector[512];
  UINT32      Value;

  FatOffset = Vol->IsFat32 ? (UINTN)Cluster * 4 :
              (Vol->IsFat12 ? (UINTN)Cluster * 3 / 2 : (UINTN)Cluster * 2);
  FatSectorRel = Vol->ReservedSectors + (UINT32)(FatOffset / 512);

  Status = ReadLba (Context, PartitionStart + FatSectorRel, Sector, 512);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  FatOffset %= 512;
  if (Vol->IsFat32) {
    Value = FatReadLe32 (Sector + FatOffset) & 0x0FFFFFFF;
  } else if (Vol->IsFat12) {
    Value = FatReadLe16 (Sector + FatOffset);
    if ((Cluster & 1) != 0) {
      Value >>= 4;
    }
    Value &= 0x0FFF;
  } else {
    Value = FatReadLe16 (Sector + FatOffset);
  }
  *Next = Value;
  return EFI_SUCCESS;
}

/** Compare a FAT short (8.3) name with a 11-byte on-disk name. */
STATIC
BOOLEAN
FatNameMatch (
  IN CONST UINT8   *EntryName,
  IN CONST CHAR8   *Name11
  )
{
  UINTN  Index;

  for (Index = 0; Index < 11; Index++) {
    if (EntryName[Index] != (UINT8)Name11[Index]) {
      return FALSE;
    }
  }
  return TRUE;
}

/**
  Find a file by 8.3 short name inside a directory (given as a list of
  clusters), returning its first cluster and size.
**/
STATIC
EFI_STATUS
FatFindInDir (
  IN  CEPH_RBD_FAT_READ_LBA      ReadLba,
  IN  VOID                      *Context,
  IN  CONST CEPH_RBD_FAT_VOLUME  *Vol,
  IN  UINT64                     PartitionStart,
  IN  UINT32                     DirCluster,
  IN  CONST CHAR8                *Name11,
  OUT UINT32                    *FoundCluster,
  OUT UINT32                    *FoundSize
  )
{
  EFI_STATUS  Status;
  UINT32      Cluster;
  UINT32      Next;
  UINT32      SectorRel;
  UINT8       Sector[512];
  UINT8      *Entry;
  UINTN       Index;
  UINTN       EntriesPerSector;

  EntriesPerSector = Vol->BytesPerSector / FAT_DIR_ENTRY_SIZE;
  Cluster = DirCluster;

  while ((Cluster >= 2) && (Cluster < 0x0FF0)) {
    SectorRel = FatClusterToSector (Vol, Cluster);
    Status = ReadLba (Context, PartitionStart + SectorRel, Sector, 512);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    for (Index = 0; Index < EntriesPerSector; Index++) {
      Entry = Sector + (Index * FAT_DIR_ENTRY_SIZE);
      if (Entry[0] == 0x00) {
        return EFI_NOT_FOUND;
      }
      if ((Entry[0] == 0xE5) || (Entry[11] == FAT_ATTR_LONG_NAME)) {
        continue;
      }
      if (FatNameMatch (Entry, Name11)) {
        *FoundCluster = FatReadLe16 (Entry + 26);
        *FoundSize = FatReadLe32 (Entry + 28);
        if (Vol->IsFat32) {
          *FoundCluster |= ((UINT32)FatReadLe16 (Entry + 20)) << 16;
        }
        return EFI_SUCCESS;
      }
    }

    Status = FatGetNextCluster (
               ReadLba, Context, Vol, PartitionStart, Cluster, &Next
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Cluster = Next;
  }
  return EFI_NOT_FOUND;
}

/** Root directory first sector (relative to partition start). */
STATIC
UINT32
FatRootDirSector (
  IN CONST CEPH_RBD_FAT_VOLUME  *Vol
  )
{
  if (Vol->IsFat32) {
    return FatClusterToSector (Vol, Vol->RootCluster);
  }
  return Vol->ReservedSectors +
         (UINT32)Vol->NumFats * Vol->FatSize16;
}

/**
  Read \EFI\BOOT\BOOTX64.EFI from the ESP into a caller-allocated buffer.
  Caller frees the buffer with FreePool().
**/
EFI_STATUS
CephRbdEspFatLoadBootloader (
  IN  CEPH_RBD_FAT_READ_LBA    ReadLba,
  IN  VOID                    *Context,
  IN  UINT64                   PartitionStartLba,
  OUT VOID                    **Buffer,
  OUT UINTN                   *BufferSize
  )
{
  EFI_STATUS            Status;
  CEPH_RBD_FAT_VOLUME   Vol;
  UINT8                 BootSector[512];
  UINT32                RootSector;
  UINT32                EfiCluster;
  UINT32                BootCluster;
  UINT32                BootSize;
  UINT32                FileCluster;
  UINT32                FileSize;
  UINT32                Next;
  UINT32                Cluster;
  UINT8                 *Out;
  UINTN                 Offset;
  UINTN                 ToRead;
  UINT8                 Sector[512];

  if ((ReadLba == NULL) || (Context == NULL) ||
      (Buffer == NULL) || (BufferSize == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *Buffer = NULL;
  *BufferSize = 0;

  Status = ReadLba (Context, PartitionStartLba, BootSector, 512);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = FatParseVolume (BootSector, 512, &Vol);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Look up EFI directory in the root directory.
  RootSector = FatRootDirSector (&Vol);
  {
    UINT8  RootBlock[512];
    UINT8 *Entry;
    UINTN  Index;
    UINTN  EntriesPerSector;

    Status = ReadLba (Context, PartitionStartLba + RootSector, RootBlock, 512);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    EntriesPerSector = Vol.BytesPerSector / FAT_DIR_ENTRY_SIZE;
    EfiCluster = 0;
    for (Index = 0; Index < EntriesPerSector; Index++) {
      Entry = RootBlock + (Index * FAT_DIR_ENTRY_SIZE);
      if (Entry[0] == 0x00) {
        break;
      }
      if (FatNameMatch (Entry, "EFI        ")) {
        EfiCluster = FatReadLe16 (Entry + 26);
        break;
      }
    }
    if (EfiCluster < 2) {
      return EFI_NOT_FOUND;
    }
  }

  Status = FatFindInDir (
             ReadLba, Context, &Vol, PartitionStartLba, EfiCluster,
             "BOOT       ", &BootCluster, &BootSize
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = FatFindInDir (
             ReadLba, Context, &Vol, PartitionStartLba, BootCluster,
             "BOOTX64 EFI", &FileCluster, &FileSize
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((FileSize == 0) || (FileSize > 0x00400000)) {
    return EFI_VOLUME_CORRUPTED;
  }

  Out = AllocatePool (FileSize);
  if (Out == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Offset = 0;
  Cluster = FileCluster;
  while ((Offset < FileSize) && (Cluster >= 2) && (Cluster < 0x0FF0)) {
    UINT32  SectorRel = FatClusterToSector (&Vol, Cluster);
    UINT32  Sect = Vol.SectorsPerCluster;

    while ((Sect > 0) && (Offset < FileSize)) {
      Status = ReadLba (
                 Context,
                 PartitionStartLba + SectorRel,
                 Sector,
                 512
                 );
      if (EFI_ERROR (Status)) {
        FreePool (Out);
        return Status;
      }
      ToRead = FileSize - Offset;
      if (ToRead > 512) {
        ToRead = 512;
      }
      CopyMem (Out + Offset, Sector, ToRead);
      Offset += ToRead;
      SectorRel++;
      Sect--;
    }

    Status = FatGetNextCluster (
               ReadLba, Context, &Vol, PartitionStartLba, Cluster, &Next
               );
    if (EFI_ERROR (Status)) {
      FreePool (Out);
      return Status;
    }
    Cluster = Next;
  }

  if (Offset < FileSize) {
    FreePool (Out);
    return EFI_VOLUME_CORRUPTED;
  }

  *Buffer = Out;
  *BufferSize = FileSize;
  return EFI_SUCCESS;
}
