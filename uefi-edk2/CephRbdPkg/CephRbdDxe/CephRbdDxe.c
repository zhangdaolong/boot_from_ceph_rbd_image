#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include "CephRbdBlockIo.h"
#include "CephRbdMsgrV1Recovery.h"

#define CEPH_RBD_DRIVER_MEDIA_ID  0x43455048

typedef enum {
  CephRbdStateStopped,
  CephRbdStateConnecting,
  CephRbdStateMonitoring,
  CephRbdStateAuthenticating,
  CephRbdStateMappingImage,
  CephRbdStateDeviceReady,
  CephRbdStateFailed
} CEPH_RBD_DRIVER_STATE;

typedef struct {
  UINT64                       Signature;
  EFI_BLOCK_IO_PROTOCOL        BlockIo;
  EFI_BLOCK_IO_MEDIA           Media;
  CEPH_RBD_DRIVER_STATE        State;
  CEPH_RBD_MSGR_V1_CLIENT      Client;
  CEPH_RBD_BLOCK_DEVICE        Device;
  CEPH_RBD_MSGR_V1_CONNECT_CONFIG  ConnectConfig;
  EFI_HANDLE                   BlockIoHandle;
  UINT32                       MediaId;
} CEPH_RBD_DRIVER;

#define CEPH_RBD_DRIVER_SIGNATURE  0x52424443  // 'CDBR'

STATIC
EFI_STATUS
EFIAPI
CephRbdBlockIoReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  (VOID)ExtendedVerification;
  if ((This == NULL) || (This->Media == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
CephRbdBlockIoRead (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN UINT64                 Lba,
  IN UINTN                  BufferSize,
  OUT VOID                  *Buffer
  )
{
  CEPH_RBD_DRIVER  *Driver;

  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Driver = (CEPH_RBD_DRIVER *)(((UINT8 *)This) -
                               OFFSET_OF (CEPH_RBD_DRIVER, BlockIo));
  if (Driver->Signature != CEPH_RBD_DRIVER_SIGNATURE) {
    return EFI_INVALID_PARAMETER;
  }

  if ((This->Media == NULL) || (MediaId != Driver->MediaId)) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize == 0) ||
      ((BufferSize % CEPH_RBD_BLOCK_SIZE) != 0) ||
      (BufferSize / CEPH_RBD_BLOCK_SIZE > CEPH_RBD_MAX_READ_BLOCKS))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  return CephRbdBlockIoReadBlocks (
           &Driver->Client,
           &Driver->Device,
           Lba,
           BufferSize / CEPH_RBD_BLOCK_SIZE,
           Buffer,
           BufferSize
           );
}

STATIC
EFI_STATUS
EFIAPI
CephRbdBlockIoWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN UINT64                 Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  (VOID)This;
  (VOID)MediaId;
  (VOID)Lba;
  (VOID)BufferSize;
  (VOID)Buffer;
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
CephRbdBlockIoFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  (VOID)This;
  return EFI_WRITE_PROTECTED;
}

EFI_STATUS
CephRbdDriverInitialize (
  IN OUT CEPH_RBD_DRIVER                    *Driver,
  IN CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG  *Config,
  IN EFI_HANDLE                             ControllerHandle,
  IN UINT32                                 PoolId,
  IN CONST CHAR8                            *ImageName,
  IN UINTN                                  ImageNameLength,
  IN CONST UINT8                            *Fsid
  )
{
  EFI_STATUS  Status;
  UINT64      LastLba;

  if ((Driver == NULL) || (Config == NULL) || (ControllerHandle == NULL) ||
      (ImageName == NULL) || (Fsid == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Driver, sizeof (*Driver));
  Driver->Signature = CEPH_RBD_DRIVER_SIGNATURE;
  Driver->State = CephRbdStateStopped;
  Driver->ConnectConfig = *Config;
  Driver->MediaId = CEPH_RBD_DRIVER_MEDIA_ID;
  CopyMem (&Driver->ConnectConfig.ControllerHandle, &ControllerHandle,
           sizeof (ControllerHandle));

  Driver->State = CephRbdStateConnecting;
  Status = CephRbdMsgrV1ClientConnect (&Driver->ConnectConfig, &Driver->Client);
  if (EFI_ERROR (Status)) {
    Driver->State = CephRbdStateFailed;
    return Status;
  }

  Driver->State = CephRbdStateMonitoring;
  Driver->State = CephRbdStateAuthenticating;
  Driver->State = CephRbdStateMappingImage;
  Status = CephRbdBlockIoDiscover (
             &Driver->Client,
             PoolId,
             ImageName,
             ImageNameLength,
             Fsid,
             &Driver->Device
             );
  if (EFI_ERROR (Status)) {
    CephRbdMsgrV1ClientClose (
      &Driver->Client,
      Driver->ConnectConfig.TimeoutSeconds
      );
    Driver->State = CephRbdStateFailed;
    return Status;
  }

  Status = CephRbdBlockIoGetMediaLastLba (&Driver->Device, &LastLba);
  if (EFI_ERROR (Status)) {
    CephRbdMsgrV1ClientClose (
      &Driver->Client,
      Driver->ConnectConfig.TimeoutSeconds
      );
    Driver->State = CephRbdStateFailed;
    return Status;
  }

  Driver->Media.MediaId = Driver->MediaId;
  Driver->Media.RemovableMedia = FALSE;
  Driver->Media.MediaPresent = TRUE;
  Driver->Media.LogicalPartition = FALSE;
  Driver->Media.ReadOnly = TRUE;
  Driver->Media.WriteCaching = FALSE;
  Driver->Media.BlockSize = CEPH_RBD_BLOCK_SIZE;
  Driver->Media.IoAlign = 1;
  Driver->Media.LastBlock = LastLba;

  Driver->BlockIo.Revision = 0x00010000;
  Driver->BlockIo.Media = &Driver->Media;
  Driver->BlockIo.Reset = CephRbdBlockIoReset;
  Driver->BlockIo.ReadBlocks = CephRbdBlockIoRead;
  Driver->BlockIo.WriteBlocks = CephRbdBlockIoWrite;
  Driver->BlockIo.FlushBlocks = CephRbdBlockIoFlush;

  if (gBS != NULL) {
    Driver->BlockIoHandle = ControllerHandle;
    Status = gBS->InstallProtocolInterface (
                    &Driver->BlockIoHandle,
                    &gEfiBlockIoProtocolGuid,
                    EFI_NATIVE_INTERFACE,
                    &Driver->BlockIo
                    );
    if (EFI_ERROR (Status)) {
      CephRbdMsgrV1ClientClose (
        &Driver->Client,
        Driver->ConnectConfig.TimeoutSeconds
        );
      Driver->State = CephRbdStateFailed;
      return Status;
    }
  }

  Driver->State = CephRbdStateDeviceReady;
  DEBUG ((DEBUG_INFO,
          "CephRbdDxe: published read-only Ceph RBD block device, %Lu blocks\n",
          (UINT64)(LastLba + 1)));
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdDriverShutdown (
  IN OUT CEPH_RBD_DRIVER  *Driver
  )
{
  if ((Driver == NULL) || (Driver->Signature != CEPH_RBD_DRIVER_SIGNATURE)) {
    return EFI_INVALID_PARAMETER;
  }

  CephRbdMsgrV1ClientClose (
    &Driver->Client,
    Driver->ConnectConfig.TimeoutSeconds != 0 ?
    Driver->ConnectConfig.TimeoutSeconds : 2
    );
  Driver->State = CephRbdStateStopped;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
CephRbdDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  (VOID)ImageHandle;
  (VOID)SystemTable;
  DEBUG ((DEBUG_INFO,
          "CephRbdDxe: firmware-side Ceph RBD boot driver\n"));
  return EFI_SUCCESS;
}
