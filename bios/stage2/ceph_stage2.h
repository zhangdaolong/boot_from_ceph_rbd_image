#ifndef CEPH_STAGE2_H_
#define CEPH_STAGE2_H_

#include <stddef.h>
#include <stdint.h>

#define CEPH_BIOS_SECTOR_SIZE 512

typedef int (*CEPH_BIOS_READ_AT)(
  void *Context,
  uint64_t ByteOffset,
  void *Buffer,
  size_t Length
  );

enum {
  CEPH_BIOS_SUCCESS = 0,
  CEPH_BIOS_INVALID_ARGUMENT = -1,
  CEPH_BIOS_IO_ERROR = -2,
  CEPH_BIOS_INVALID_MBR = -3,
  CEPH_BIOS_NO_ACTIVE_PARTITION = -4,
  CEPH_BIOS_OUT_OF_RANGE = -5,
  CEPH_BIOS_INVALID_BOOT_SECTOR = -6
};

int
CephBiosStage2LoadPartitionSector(
  CEPH_BIOS_READ_AT ReadAt,
  void *Context,
  uint64_t ImageSizeBytes,
  uint8_t BootSector[CEPH_BIOS_SECTOR_SIZE],
  uint32_t *PartitionStartLba
  );

#endif
