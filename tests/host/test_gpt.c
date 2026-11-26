#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "CephRbdGpt.h"

#define MOCK_SECTORS  64
#define PART_COUNT    32

typedef struct {
  UINT8  Sectors[MOCK_SECTORS][CEPH_RBD_GPT_SECTOR_SIZE];
} MOCK_GPT_DISK;

static void
WriteLe32 (UINT8 *Buffer, UINT32 Value)
{
  Buffer[0] = (UINT8)Value;
  Buffer[1] = (UINT8)(Value >> 8);
  Buffer[2] = (UINT8)(Value >> 16);
  Buffer[3] = (UINT8)(Value >> 24);
}

static void
WriteLe64 (UINT8 *Buffer, UINT64 Value)
{
  WriteLe32 (Buffer, (UINT32)Value);
  WriteLe32 (Buffer + 4, (UINT32)(Value >> 32));
}

static EFI_STATUS
MockReadLba (VOID *Context, UINT64 Lba, UINT8 *Buffer, UINTN BufferSize)
{
  MOCK_GPT_DISK *Disk = (MOCK_GPT_DISK *)Context;

  if ((Lba >= MOCK_SECTORS) || (BufferSize > CEPH_RBD_GPT_SECTOR_SIZE)) {
    return EFI_INVALID_PARAMETER;
  }
  memcpy (Buffer, Disk->Sectors[Lba], BufferSize);
  return EFI_SUCCESS;
}

static void
BuildFakeGpt (MOCK_GPT_DISK *Disk, UINT8 *TableOut, UINTN *TableLengthOut)
{
  UINT8   *Mbr = Disk->Sectors[0];
  UINT8   *Header = Disk->Sectors[1];
  UINT8   *Table = Disk->Sectors[2];
  UINT8   *Entry;
  UINT32  ArrayCrc;
  UINT8   Zeroed[CEPH_RBD_GPT_HEADER_SIZE];
  UINTN   I;

  memset (Disk, 0, sizeof (*Disk));

  // Protective MBR.
  Mbr[510] = 0x55;
  Mbr[511] = 0xaa;
  Mbr[446 + 4] = 0xee;

  // Partition table: 32 entries x 128 bytes = 4096 bytes = 8 sectors.
  // Entry 0: Linux filesystem; Entry 1: EFI System Partition.
  Entry = Table;
  Entry[0] = 0xaf;  // a non-ESP type GUID
  WriteLe64 (Entry + 32, 10);
  WriteLe64 (Entry + 40, 30);
  Entry += CEPH_RBD_GPT_PARTITION_ENTRY_SIZE;
  {
    static const UINT8 EspGuid[16] = CEPH_RBD_GPT_ESP_TYPE_GUID;
    memcpy (Entry, EspGuid, 16);
  }
  WriteLe64 (Entry + 32, 31);
  WriteLe64 (Entry + 40, 63);
  Entry[56] = (UINT8)'E';  // "ESP" in UTF-16LE
  Entry[58] = (UINT8)'S';
  Entry[60] = (UINT8)'P';

  ArrayCrc = CephRbdGptCrc32 (
               Table, PART_COUNT * CEPH_RBD_GPT_PARTITION_ENTRY_SIZE
               );

  // GPT header.
  memcpy (Header, "EFI PART", 8);
  WriteLe32 (Header + 8, CEPH_RBD_GPT_REVISION);
  WriteLe32 (Header + 12, CEPH_RBD_GPT_HEADER_SIZE);
  WriteLe32 (Header + 16, 0);  // CRC filled below
  WriteLe64 (Header + 24, 1);  // current LBA
  WriteLe64 (Header + 32, MOCK_SECTORS - 1);  // backup LBA
  WriteLe64 (Header + 40, 10);  // first usable LBA
  WriteLe64 (Header + 48, MOCK_SECTORS - 1);  // last usable LBA
  WriteLe64 (Header + 72, 2);  // partition entries LBA
  WriteLe32 (Header + 80, PART_COUNT);
  WriteLe32 (Header + 84, CEPH_RBD_GPT_PARTITION_ENTRY_SIZE);
  WriteLe32 (Header + 88, ArrayCrc);

  memcpy (Zeroed, Header, CEPH_RBD_GPT_HEADER_SIZE);
  memset (Zeroed + 16, 0, 4);
  WriteLe32 (Header + 16, CephRbdGptCrc32 (Zeroed, CEPH_RBD_GPT_HEADER_SIZE));

  if (TableOut != NULL) {
    memcpy (TableOut, Table, PART_COUNT * CEPH_RBD_GPT_PARTITION_ENTRY_SIZE);
  }
  if (TableLengthOut != NULL) {
    *TableLengthOut = PART_COUNT * CEPH_RBD_GPT_PARTITION_ENTRY_SIZE;
  }
  for (I = 0; I < 8; I++) {
    // no-op; keeps the loop-based sector fill explicit for clarity
  }
}

void
TestGpt (void)
{
  MOCK_GPT_DISK   Disk;
  CEPH_RBD_GPT_HEADER   Header;
  CEPH_RBD_GPT_PARTITION Esp;
  CEPH_RBD_GPT_PARTITION *EspPtr = &Esp;
  UINT8   Table[PART_COUNT * CEPH_RBD_GPT_PARTITION_ENTRY_SIZE];
  UINTN   TableLength = 0;
  UINT32  ArrayCrc;
  CHAR16  Path[64];
  UINT8   Corrupt[CEPH_RBD_GPT_SECTOR_SIZE];

  // CRC-32 (IEEE) known vector.
  assert(CephRbdGptCrc32 ((const UINT8 *)"123456789", 9) == 0xcbf43926U);
  assert(CephRbdGptCrc32 (NULL, 0) == 0);

  BuildFakeGpt (&Disk, Table, &TableLength);
  assert(TableLength == PART_COUNT * CEPH_RBD_GPT_PARTITION_ENTRY_SIZE);

  // Protective MBR validation.
  assert(CephRbdGptValidateProtectiveMbr (
           Disk.Sectors[0], CEPH_RBD_GPT_SECTOR_SIZE) == EFI_SUCCESS);
  assert(CephRbdGptValidateProtectiveMbr (
           Disk.Sectors[0], 100) == EFI_INVALID_PARAMETER);
  Corrupt[0] = Disk.Sectors[0][0];
  memcpy (Corrupt, Disk.Sectors[0], sizeof (Corrupt));
  Corrupt[510] = 0;
  assert(CephRbdGptValidateProtectiveMbr (
           Corrupt, sizeof (Corrupt)) == EFI_UNSUPPORTED);

  // Header parse + CRC.
  assert(CephRbdGptParseHeader (
           Disk.Sectors[1], CEPH_RBD_GPT_SECTOR_SIZE, &Header) == EFI_SUCCESS);
  assert(Header.CurrentLba == 1);
  assert(Header.FirstUsableLba == 10);
  assert(Header.PartitionEntriesLba == 2);
  assert(Header.PartitionEntryCount == PART_COUNT);
  assert(Header.PartitionEntrySize == CEPH_RBD_GPT_PARTITION_ENTRY_SIZE);

  memcpy (Corrupt, Disk.Sectors[1], sizeof (Corrupt));
  Corrupt[20] ^= 0xff;
  assert(CephRbdGptParseHeader (
           Corrupt, sizeof (Corrupt), &Header) == EFI_CRC_ERROR);
  memcpy (Corrupt, Disk.Sectors[1], sizeof (Corrupt));
  Corrupt[0] = 'X';
  assert(CephRbdGptParseHeader (
           Corrupt, sizeof (Corrupt), &Header) == EFI_UNSUPPORTED);

  // Entry decode + ESP identification.
  {
    CEPH_RBD_GPT_PARTITION Partition;
    assert(CephRbdGptParsePartitionEntry (
             Table, CEPH_RBD_GPT_PARTITION_ENTRY_SIZE, &Partition) == EFI_SUCCESS);
    assert(CephRbdGptIsEsp (&Partition) == FALSE);
    assert(Partition.FirstLba == 10 && Partition.LastLba == 30);

    assert(CephRbdGptParsePartitionEntry (
             Table + CEPH_RBD_GPT_PARTITION_ENTRY_SIZE,
             CEPH_RBD_GPT_PARTITION_ENTRY_SIZE, &Partition) == EFI_SUCCESS);
    assert(CephRbdGptIsEsp (&Partition) == TRUE);
    assert(Partition.FirstLba == 31 && Partition.LastLba == 63);
    assert(Partition.Name[0] == 'E' &&
           Partition.Name[1] == 'S' &&
           Partition.Name[2] == 'P' &&
           Partition.Name[3] == 0);

    assert(CephRbdGptParsePartitionEntry (
             Table, 8, &Partition) == EFI_INVALID_PARAMETER);
  }

  // Find the ESP with and without an array CRC.
  ArrayCrc = CephRbdGptCrc32 (Table, TableLength);
  assert(CephRbdGptFindEspInTable (
           Table, TableLength, &Header, ArrayCrc, EspPtr, NULL) == EFI_SUCCESS);
  assert(Esp.FirstLba == 31);
  assert(CephRbdGptFindEspInTable (
           Table, TableLength, &Header, 0, EspPtr, NULL) == EFI_SUCCESS);
  assert(CephRbdGptFindEspInTable (
           Table, TableLength, &Header, ArrayCrc ^ 1, EspPtr, NULL) == EFI_CRC_ERROR);
  assert(CephRbdGptFindEspInTable (
           Table, 16, &Header, ArrayCrc, EspPtr, NULL) == EFI_BUFFER_TOO_SMALL);

  // No ESP in the table.
  {
    UINT8 NoEsp[PART_COUNT * CEPH_RBD_GPT_PARTITION_ENTRY_SIZE];
    memcpy (NoEsp, Table, TableLength);
    memset (NoEsp + CEPH_RBD_GPT_PARTITION_ENTRY_SIZE, 0, 16);
    assert(CephRbdGptFindEspInTable (
             NoEsp, TableLength, &Header, 0, EspPtr, NULL) == EFI_NOT_FOUND);
  }

  // High-level discovery through the callback.
  assert(CephRbdGptDiscoverEsp (
           MockReadLba, &Disk, MOCK_SECTORS - 1, EspPtr, NULL) == EFI_SUCCESS);
  assert(Esp.FirstLba == 31 && Esp.LastLba == 63);
  assert(CephRbdGptDiscoverEsp (
           MockReadLba, &Disk, 1, EspPtr, NULL) == EFI_INVALID_PARAMETER);

  // Boot path.
  assert(CephRbdGptBuildEspBootPath (Path, sizeof (Path) / sizeof (Path[0])) ==
         EFI_SUCCESS);
  {
    static const CHAR16 Expected[] = {
      '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\',
      'B', 'O', 'O', 'T', 'X', '6', '4', '.', 'E', 'F', 'I', 0
    };
    assert(memcmp (Path, Expected, sizeof (Expected)) == 0);
  }
  assert(CephRbdGptBuildEspBootPath (Path, 4) == EFI_BUFFER_TOO_SMALL);

  puts("GPT/ESP discovery tests: passed");
}
