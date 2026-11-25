#include "ceph_stage2.h"

static uint32_t
ReadLe32(const uint8_t *Data)
{
  return (uint32_t)Data[0]
         | ((uint32_t)Data[1] << 8)
         | ((uint32_t)Data[2] << 16)
         | ((uint32_t)Data[3] << 24);
}

int
CephBiosStage2LoadPartitionSector(
  CEPH_BIOS_READ_AT ReadAt,
  void *Context,
  uint64_t ImageSizeBytes,
  uint8_t BootSector[CEPH_BIOS_SECTOR_SIZE],
  uint32_t *PartitionStartLba
  )
{
  uint8_t mbr[CEPH_BIOS_SECTOR_SIZE];
  const uint8_t *active = NULL;
  uint32_t start_lba;
  uint32_t sectors;
  uint64_t offset;
  unsigned int index;

  if (ReadAt == NULL || BootSector == NULL || PartitionStartLba == NULL ||
      ImageSizeBytes < CEPH_BIOS_SECTOR_SIZE) {
    return CEPH_BIOS_INVALID_ARGUMENT;
  }

  if (ReadAt(Context, 0, mbr, sizeof(mbr)) != 0) {
    return CEPH_BIOS_IO_ERROR;
  }

  if (mbr[510] != 0x55 || mbr[511] != 0xaa) {
    return CEPH_BIOS_INVALID_MBR;
  }

  for (index = 0; index < 4; index++) {
    const uint8_t *entry = mbr + 446 + index * 16;
    if (entry[0] != 0 && entry[0] != 0x80) {
      return CEPH_BIOS_INVALID_MBR;
    }

    if (entry[4] == 0 &&
        (entry[0] != 0 || ReadLe32(entry + 8) != 0 || ReadLe32(entry + 12) != 0)) {
      return CEPH_BIOS_INVALID_MBR;
    }

    if (entry[0] == 0x80) {
      if (entry[4] == 0) {
        return CEPH_BIOS_INVALID_MBR;
      }

      if (active != NULL) {
        return CEPH_BIOS_INVALID_MBR;
      }
      active = entry;
    }
  }

  if (active == NULL) {
    return CEPH_BIOS_NO_ACTIVE_PARTITION;
  }

  start_lba = ReadLe32(active + 8);
  sectors = ReadLe32(active + 12);
  if (start_lba == 0 || sectors == 0 ||
      (uint64_t)start_lba > ImageSizeBytes / CEPH_BIOS_SECTOR_SIZE ||
      (uint64_t)sectors > ImageSizeBytes / CEPH_BIOS_SECTOR_SIZE - start_lba) {
    return CEPH_BIOS_OUT_OF_RANGE;
  }

  offset = (uint64_t)start_lba * CEPH_BIOS_SECTOR_SIZE;
  if (ReadAt(Context, offset, BootSector, CEPH_BIOS_SECTOR_SIZE) != 0) {
    return CEPH_BIOS_IO_ERROR;
  }

  if (BootSector[510] != 0x55 || BootSector[511] != 0xaa) {
    return CEPH_BIOS_INVALID_BOOT_SECTOR;
  }

  *PartitionStartLba = start_lba;
  return CEPH_BIOS_SUCCESS;
}
