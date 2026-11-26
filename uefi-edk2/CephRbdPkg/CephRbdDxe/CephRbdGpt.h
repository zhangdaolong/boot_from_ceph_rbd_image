#ifndef CEPH_RBD_GPT_H_
#define CEPH_RBD_GPT_H_

#include <Uefi.h>

// Minimal, bounds-checked GPT / EFI System Partition discovery used by the
// firmware boot path. It parses the protective MBR (LBA 0), the GPT header
// (LBA 1) and the partition entry array (LBA 2..), then locates the partition
// whose type GUID is the EFI System Partition. No dynamic allocation is used
// so the same code runs in DXE and in host tests.

#define CEPH_RBD_GPT_SECTOR_SIZE              512
#define CEPH_RBD_GPT_HEADER_SIZE              92
#define CEPH_RBD_GPT_PARTITION_ENTRY_SIZE     128
#define CEPH_RBD_GPT_SIGNATURE_PART           UINT64_C(0x5452415020494645)  // "EFI PART"
#define CEPH_RBD_GPT_REVISION                 UINT32_C(0x00010000)
#define CEPH_RBD_GPT_MAX_PARTITION_ENTRIES    4096
#define CEPH_RBD_GPT_MAX_PARTITION_NAME_LEN   36  // CHAR16 count, 72 bytes

// EFI System Partition type GUID C12A7328-F81F-11D2-BA4B-00A0C93EC93B in
// on-disk (mixed-endian) byte order.
// EFI System Partition type GUID C12A7328-F81F-11D2-BA4B-00A0C93EC93B in
// GPT on-disk (mixed-endian) byte order: Data1/2/3 little-endian, Data4 as-is
// (BA4B-00A0C93EC93B -> ba 4b 00 a0 c9 3e c9 3b).
#define CEPH_RBD_GPT_ESP_TYPE_GUID           \
  { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, \
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b }

typedef struct {
  UINT64  CurrentLba;
  UINT64  BackupLba;
  UINT64  FirstUsableLba;
  UINT64  LastUsableLba;
  UINT64  PartitionEntriesLba;
  UINT32  PartitionEntrySize;
  UINT32  PartitionEntryCount;
} CEPH_RBD_GPT_HEADER;

typedef struct {
  UINT8   TypeGuid[16];
  UINT8   UniqueGuid[16];
  UINT64  FirstLba;
  UINT64  LastLba;
  UINT64  Attributes;
  CHAR16  Name[CEPH_RBD_GPT_MAX_PARTITION_NAME_LEN + 1];
} CEPH_RBD_GPT_PARTITION;

// Read one LBA-sized block through a callback. Context is opaque to the GPT
// parser; in firmware it is the EFI_BLOCK_IO_PROTOCOL, in the live probe it
// is the BlockIo device, and in host tests it is a memory buffer.
typedef
EFI_STATUS
(*CEPH_RBD_GPT_READ_LBA) (
  IN     VOID   *Context,
  IN     UINT64  Lba,
  OUT    UINT8  *Buffer,
  IN     UINTN   BufferSize
  );

// IEEE CRC-32 (polynomial 0xEDB88320) used by the GPT header and partition
// array. This is distinct from the messenger v1 CRC-32C.
UINT32
CephRbdGptCrc32 (
  IN CONST VOID  *Data,
  IN UINTN       Length
  );

// Validate the protective MBR at LBA 0 (0x55AA signature, 0xEE partition).
EFI_STATUS
CephRbdGptValidateProtectiveMbr (
  IN CONST UINT8  *Mbr,
  IN UINTN        Length
  );

// Parse and validate a GPT header sector. Verifies the "EFI PART" signature,
// revision, header size and the header CRC after zeroing its own CRC field.
EFI_STATUS
CephRbdGptParseHeader (
  IN CONST UINT8         *Sector,
  IN UINTN               Length,
  OUT CEPH_RBD_GPT_HEADER *Header
  );

// Decode one 128-byte partition entry.
EFI_STATUS
CephRbdGptParsePartitionEntry (
  IN CONST UINT8             *Entry,
  IN UINTN                   Length,
  OUT CEPH_RBD_GPT_PARTITION  *Partition
  );

// Return TRUE when the partition type GUID is the EFI System Partition.
BOOLEAN
CephRbdGptIsEsp (
  IN CONST CEPH_RBD_GPT_PARTITION  *Partition
  );

// Scan a raw partition entry array for the ESP. Validates the array CRC when
// the caller supplies the CRC recorded in the GPT header.
EFI_STATUS
CephRbdGptFindEspInTable (
  IN CONST UINT8                  *Table,
  IN UINTN                        TableLength,
  IN CONST CEPH_RBD_GPT_HEADER    *Header,
  IN UINT32                       ExpectedCrc,
  OUT CEPH_RBD_GPT_PARTITION      *Esp
  );

// High-level discovery: read LBA 0/1 and the partition array through ReadLba,
// then return the EFI System Partition. LastLba is the device's last LBA and
// is used only to bound the reads.
EFI_STATUS
CephRbdGptDiscoverEsp (
  IN     CEPH_RBD_GPT_READ_LBA   ReadLba,
  IN     VOID                   *Context,
  IN     UINT64                 LastLba,
  OUT    CEPH_RBD_GPT_PARTITION *Esp
  );

// Build the standard removable-media boot path for x64: \EFI\BOOT\BOOTX64.EFI.
EFI_STATUS
CephRbdGptBuildEspBootPath (
  OUT CHAR16  *Path,
  IN UINTN    PathCapacity
  );

#endif
