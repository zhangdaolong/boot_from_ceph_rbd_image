#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include "CephRbdGpt.h"

STATIC CONST UINT8  mEspTypeGuid[16] = CEPH_RBD_GPT_ESP_TYPE_GUID;

STATIC
UINT32
GptReadLe32 (
  IN CONST UINT8  *Buffer
  )
{
  return (UINT32)Buffer[0] |
         ((UINT32)Buffer[1] << 8) |
         ((UINT32)Buffer[2] << 16) |
         ((UINT32)Buffer[3] << 24);
}

STATIC
UINT64
GptReadLe64 (
  IN CONST UINT8  *Buffer
  )
{
  return (UINT64)GptReadLe32 (Buffer) |
         ((UINT64)GptReadLe32 (Buffer + 4) << 32);
}

UINT32
CephRbdGptCrc32 (
  IN CONST VOID  *Data,
  IN UINTN       Length
  )
{
  CONST UINT8  *Bytes = (CONST UINT8 *)Data;
  UINT32       Crc = 0xffffffffU;
  UINTN        Index;
  UINTN        Bit;

  if ((Data == NULL) && (Length != 0)) {
    return 0;
  }
  for (Index = 0; Index < Length; Index++) {
    Crc ^= Bytes[Index];
    for (Bit = 0; Bit < 8; Bit++) {
      if ((Crc & 1U) != 0) {
        Crc = (Crc >> 1) ^ 0xedb88320U;
      } else {
        Crc >>= 1;
      }
    }
  }
  return ~Crc;
}

EFI_STATUS
CephRbdGptValidateProtectiveMbr (
  IN CONST UINT8  *Mbr,
  IN UINTN        Length
  )
{
  if ((Mbr == NULL) || (Length < CEPH_RBD_GPT_SECTOR_SIZE)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Mbr[510] != 0x55) || (Mbr[511] != 0xaa)) {
    return EFI_UNSUPPORTED;
  }
  // MBR partition table begins at 446; the first entry's type is at +4.
  if (Mbr[446 + 4] != 0xee) {
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdGptParseHeader (
  IN CONST UINT8          *Sector,
  IN UINTN                Length,
  OUT CEPH_RBD_GPT_HEADER *Header
  )
{
  UINT32  Revision;
  UINT32  HeaderSize;
  UINT32  StoredCrc;
  UINT32  ComputedCrc;
  UINT8   Zeroed[CEPH_RBD_GPT_HEADER_SIZE];

  if ((Sector == NULL) || (Header == NULL) ||
      (Length < CEPH_RBD_GPT_HEADER_SIZE))
  {
    return EFI_INVALID_PARAMETER;
  }
  if (GptReadLe64 (Sector) != CEPH_RBD_GPT_SIGNATURE_PART) {
    return EFI_UNSUPPORTED;
  }

  Revision = GptReadLe32 (Sector + 8);
  HeaderSize = GptReadLe32 (Sector + 12);
  if ((Revision != CEPH_RBD_GPT_REVISION) ||
      (HeaderSize < CEPH_RBD_GPT_HEADER_SIZE) ||
      (HeaderSize > Length))
  {
    return EFI_UNSUPPORTED;
  }

  StoredCrc = GptReadLe32 (Sector + 16);
  CopyMem (Zeroed, Sector, CEPH_RBD_GPT_HEADER_SIZE);
  Zeroed[16] = 0;
  Zeroed[17] = 0;
  Zeroed[18] = 0;
  Zeroed[19] = 0;
  ComputedCrc = CephRbdGptCrc32 (Zeroed, HeaderSize);
  if (ComputedCrc != StoredCrc) {
    return EFI_CRC_ERROR;
  }

  Header->CurrentLba = GptReadLe64 (Sector + 24);
  Header->BackupLba = GptReadLe64 (Sector + 32);
  Header->FirstUsableLba = GptReadLe64 (Sector + 40);
  Header->LastUsableLba = GptReadLe64 (Sector + 48);
  Header->PartitionEntriesLba = GptReadLe64 (Sector + 72);
  Header->PartitionEntrySize = GptReadLe32 (Sector + 84);
  Header->PartitionEntryCount = GptReadLe32 (Sector + 80);

  if ((Header->FirstUsableLba >= Header->LastUsableLba) ||
      (Header->PartitionEntrySize < CEPH_RBD_GPT_PARTITION_ENTRY_SIZE) ||
      (Header->PartitionEntrySize == 0) ||
      (Header->PartitionEntryCount > CEPH_RBD_GPT_MAX_PARTITION_ENTRIES))
  {
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdGptParsePartitionEntry (
  IN CONST UINT8             *Entry,
  IN UINTN                   Length,
  OUT CEPH_RBD_GPT_PARTITION  *Partition
  )
{
  UINTN  Index;
  UINTN  CharCount;

  if ((Entry == NULL) || (Partition == NULL) ||
      (Length < CEPH_RBD_GPT_PARTITION_ENTRY_SIZE))
  {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Partition->TypeGuid, Entry, sizeof (Partition->TypeGuid));
  CopyMem (Partition->UniqueGuid, Entry + 16, sizeof (Partition->UniqueGuid));
  Partition->FirstLba = GptReadLe64 (Entry + 32);
  Partition->LastLba = GptReadLe64 (Entry + 40);
  Partition->Attributes = GptReadLe64 (Entry + 48);

  CharCount = 0;
  for (Index = 0; Index < CEPH_RBD_GPT_MAX_PARTITION_NAME_LEN; Index++) {
    UINT16 Code = (UINT16)(Entry[56 + (Index * 2)] |
                           ((UINT16)Entry[57 + (Index * 2)] << 8));
    if (Code == 0) {
      break;
    }
    Partition->Name[CharCount++] = (CHAR16)Code;
  }
  Partition->Name[CharCount] = 0;

  if ((Partition->FirstLba > Partition->LastLba) ||
      (Partition->FirstLba == 0))
  {
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

BOOLEAN
CephRbdGptIsEsp (
  IN CONST CEPH_RBD_GPT_PARTITION  *Partition
  )
{
  if (Partition == NULL) {
    return FALSE;
  }
  return (CompareMem (Partition->TypeGuid, mEspTypeGuid, 16) == 0);
}

EFI_STATUS
CephRbdGptFindEspInTable (
  IN CONST UINT8                 *Table,
  IN UINTN                       TableLength,
  IN CONST CEPH_RBD_GPT_HEADER   *Header,
  IN UINT32                      ExpectedCrc,
  OUT CEPH_RBD_GPT_PARTITION     *Esp
  )
{
  UINTN                 RequiredLength;
  UINT32                ComputedCrc;
  UINTN                 Index;
  EFI_STATUS            Status;

  if ((Table == NULL) || (Header == NULL) || (Esp == NULL) ||
      (Header->PartitionEntrySize == 0) ||
      (Header->PartitionEntryCount > CEPH_RBD_GPT_MAX_PARTITION_ENTRIES))
  {
    return EFI_INVALID_PARAMETER;
  }

  RequiredLength = (UINTN)Header->PartitionEntrySize *
                   (UINTN)Header->PartitionEntryCount;
  if (TableLength < RequiredLength) {
    return EFI_BUFFER_TOO_SMALL;
  }

  ComputedCrc = CephRbdGptCrc32 (Table, RequiredLength);
  if ((ExpectedCrc != 0) && (ComputedCrc != ExpectedCrc)) {
    return EFI_CRC_ERROR;
  }

  for (Index = 0; Index < Header->PartitionEntryCount; Index++) {
    CEPH_RBD_GPT_PARTITION  Partition;
    CONST UINT8             *Entry = Table + (Index * Header->PartitionEntrySize);

    Status = CephRbdGptParsePartitionEntry (
               Entry, Header->PartitionEntrySize, &Partition
               );
    if (EFI_ERROR (Status)) {
      continue;
    }
    if (CephRbdGptIsEsp (&Partition)) {
      CopyMem (Esp, &Partition, sizeof (Partition));
      return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND;
}

EFI_STATUS
CephRbdGptDiscoverEsp (
  IN     CEPH_RBD_GPT_READ_LBA   ReadLba,
  IN     VOID                   *Context,
  IN     UINT64                 LastLba,
  OUT    CEPH_RBD_GPT_PARTITION *Esp
  )
{
  EFI_STATUS             Status;
  UINT8                  Sector[CEPH_RBD_GPT_SECTOR_SIZE];
  CEPH_RBD_GPT_HEADER    Header;
  UINT64                 TableLba;
  UINT64                 TableSectors;
  UINT32                 ExpectedCrc;
  UINTN                  TableLength;
  UINT8                  *Table;
  UINTN                  Index;
  UINT64                 Lba;

  if ((ReadLba == NULL) || (Esp == NULL) || (LastLba < 2)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = ReadLba (Context, 0, Sector, sizeof (Sector));
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = CephRbdGptValidateProtectiveMbr (Sector, sizeof (Sector));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = ReadLba (Context, 1, Sector, sizeof (Sector));
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = CephRbdGptParseHeader (Sector, sizeof (Sector), &Header);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // The partition entry array is recorded in the GPT header; read it LBA by
  // LBA and bound the total by both the header count and the device size.
  TableLba = Header.PartitionEntriesLba;
  TableLength = (UINTN)Header.PartitionEntrySize *
                (UINTN)Header.PartitionEntryCount;
  TableSectors = (TableLength + CEPH_RBD_GPT_SECTOR_SIZE - 1) /
                 CEPH_RBD_GPT_SECTOR_SIZE;
  if ((TableLba == 0) || (TableLba >= Header.FirstUsableLba) ||
      (TableLba > LastLba) || (TableSectors > (LastLba - TableLba + 1)))
  {
    return EFI_UNSUPPORTED;
  }

  Table = AllocatePool (TableLength);
  if (Table == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index < (UINTN)TableSectors; Index++) {
    Lba = TableLba + Index;
    Status = ReadLba (
               Context, Lba,
               Table + (Index * CEPH_RBD_GPT_SECTOR_SIZE),
               CEPH_RBD_GPT_SECTOR_SIZE
               );
    if (EFI_ERROR (Status)) {
      FreePool (Table);
      return Status;
    }
  }

  ExpectedCrc = 0;
  Status = ReadLba (Context, 1, Sector, sizeof (Sector));
  if (!EFI_ERROR (Status)) {
    ExpectedCrc = GptReadLe32 (Sector + 88);
  }

  Status = CephRbdGptFindEspInTable (
             Table, TableLength, &Header, ExpectedCrc, Esp
             );
  FreePool (Table);
  return Status;
}

EFI_STATUS
CephRbdGptBuildEspBootPath (
  OUT CHAR16  *Path,
  IN UINTN    PathCapacity
  )
{
  // \EFI\BOOT\BOOTX64.EFI, written as explicit UTF-16 code points so the
  // same source compiles both in EDK II (CHAR16) and against the host's
  // uint16_t stub (where L"..." would be a 4-byte wchar_t literal).
  STATIC CONST CHAR16  BootPath[] = {
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\',
    'B', 'O', 'O', 'T', 'X', '6', '4', '.', 'E', 'F', 'I', 0
  };
  UINTN  Length = sizeof (BootPath) / sizeof (BootPath[0]);

  if ((Path == NULL) || (PathCapacity < Length)) {
    return EFI_BUFFER_TOO_SMALL;
  }
  CopyMem (Path, BootPath, sizeof (BootPath));
  return EFI_SUCCESS;
}
