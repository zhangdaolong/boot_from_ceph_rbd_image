#ifndef CEPH_RBD_ESP_FAT_H_
#define CEPH_RBD_ESP_FAT_H_

#include <Uefi.h>

/** Callback used by the FAT reader to fetch 512-byte sectors from the ESP. */
typedef
EFI_STATUS
(*CEPH_RBD_FAT_READ_LBA)(
  IN     VOID    *Context,
  IN     UINT64   Lba,
  OUT    UINT8   *Buffer,
  IN     UINTN    BufferSize
  );

/**
  Read \EFI\BOOT\BOOTX64.EFI from a FAT12/FAT16 ESP into an allocated buffer.

  @param[in]  ReadLba           Sector read callback.
  @param[in]  Context           Callback context.
  @param[in]  PartitionStartLba First LBA of the ESP partition.
  @param[out] Buffer            Receives an allocated buffer; caller frees it.
  @param[out] BufferSize        Receives the file size in bytes.

  @retval EFI_SUCCESS           The bootloader was read.
  @retval EFI_NOT_FOUND         The file or a directory component is missing.
  @retval EFI_VOLUME_CORRUPTED  The FAT structures are invalid.
  @retval EFI_OUT_OF_RESOURCES  Allocation failed.
**/
EFI_STATUS
CephRbdEspFatLoadBootloader (
  IN  CEPH_RBD_FAT_READ_LBA    ReadLba,
  IN  VOID                    *Context,
  IN  UINT64                   PartitionStartLba,
  OUT VOID                    **Buffer,
  OUT UINTN                   *BufferSize
  );

#endif
