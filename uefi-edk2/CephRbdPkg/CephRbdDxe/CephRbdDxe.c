#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DriverBinding.h>
#include "CephRbdAuth.h"
#include "CephRbdBlockIo.h"
#include "CephRbdConfig.h"
#include "CephRbdDxe.h"
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
  BOOLEAN                      BlockIoInstalled;
} CEPH_RBD_DRIVER;

#define CEPH_RBD_DRIVER_SIGNATURE  0x52424443  // 'CDBR'

// Firmware boots a single configured image, so one driver instance is enough.
STATIC CEPH_RBD_DRIVER  mCephRbdDriver;

STATIC EFI_STATUS EFIAPI CephRbdDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  );

STATIC EFI_STATUS EFIAPI CephRbdDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  );

STATIC EFI_STATUS EFIAPI CephRbdDriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer OPTIONAL
  );

STATIC EFI_DRIVER_BINDING_PROTOCOL  mCephRbdDriverBinding = {
  CephRbdDriverBindingSupported,
  CephRbdDriverBindingStart,
  CephRbdDriverBindingStop,
  0x00000001,
  NULL,
  NULL
};

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
  IN CONST UINT8                            *Fsid,
  IN CONST CHAR8                            *Identity,
  IN UINTN                                  IdentityLength,
  IN CONST UINT8                            *Secret,
  IN UINTN                                  SecretLength
  )
{
  EFI_STATUS              Status;
  UINT64                  LastLba;
  CEPH_RBD_AUTH_SESSION   Session;

  if ((Driver == NULL) || (Config == NULL) || (ControllerHandle == NULL) ||
      (ImageName == NULL) || (Fsid == NULL) ||
      (Identity == NULL) || (IdentityLength == 0) ||
      (Secret == NULL))
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

  // Authenticate the monitor session before any MON/OSD RPC. Without this
  // step the monitor dispatches but drops every request with
  // "is not authenticated". The CephX secret must be the raw 16-byte AES-128
  // key; a key reference requires a platform-specific secret resolver.
  Driver->State = CephRbdStateMonitoring;
  Driver->State = CephRbdStateAuthenticating;
  Status = CephRbdAuthHandshake (
             &Driver->Client,
             Identity,
             IdentityLength,
             Secret,
             SecretLength,
             &Session
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
            "CephRbdDxe: CephX authentication failed with %d\n",
            (INT32)Status
            ));
    CephRbdMsgrV1ClientClose (
      &Driver->Client,
      Driver->ConnectConfig.TimeoutSeconds
      );
    Driver->State = CephRbdStateFailed;
    return Status;
  }

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

    Driver->BlockIoInstalled = TRUE;
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

STATIC EFI_STATUS EFIAPI
CephRbdDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  CEPH_RBD_CONFIG  Config;

  if ((This == NULL) || (ControllerHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  (VOID)RemainingDevicePath;
  if (EFI_ERROR (CephRbdConfigLoad (&Config))) {
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
CephRbdDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  CEPH_RBD_CONFIG                  Config;
  CEPH_RBD_MSGR_V1_CONNECT_CONFIG  ConnectConfig;
  EFI_STATUS                       Status;
  UINTN                            Index;

  if ((This == NULL) || (ControllerHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  (VOID)RemainingDevicePath;
  Status = CephRbdConfigLoad (&Config);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Try every configured monitor before giving up. This is simple failover:
  // the first monitor that accepts the handshake and maps the image wins.
  for (Index = 0; Index < Config.MonitorCount; Index++) {
    ZeroMem (&ConnectConfig, sizeof (ConnectConfig));
    ConnectConfig.ImageHandle = This->ImageHandle;
    ConnectConfig.ControllerHandle = ControllerHandle;
    ConnectConfig.RemoteAddress = Config.Monitors[Index].Address;
    ConnectConfig.RemotePort = Config.Monitors[Index].Port;
    ConnectConfig.SupportedFeatures = 0x0F;
    ConnectConfig.RequiredFeatures = 0;
    ConnectConfig.HostType = CEPH_MSGR_CONNECT_HOST_CLIENT;
    ConnectConfig.ProtocolVersion = 15;  // CEPH_MONC_PROTOCOL
    ConnectConfig.AuthorizerProtocol = 0;
    ConnectConfig.Flags = 0;
    ConnectConfig.TimeoutSeconds = 5;

    // CephRbdAuthHandshake needs the raw 16-byte AES-128 secret. The config
    // Key field carries raw key bytes unless the KEY_REFERENCE flag is set,
    // in which case resolving it requires platform-specific protected storage
    // that a generic DXE driver cannot provide.
    if ((Config.Flags & CEPH_RBD_CONFIG_FLAG_KEY_REFERENCE) != 0) {
      DEBUG ((DEBUG_ERROR,
              "CephRbdDxe: key references are not supported by this driver\n"));
      return EFI_UNSUPPORTED;
    }

    Status = CephRbdDriverInitialize (
               &mCephRbdDriver,
               &ConnectConfig,
               ControllerHandle,
               Config.PoolId,
               Config.ImageName,
               Config.ImageNameLength,
               Config.Fsid,
               Config.Identity,
               Config.IdentityLength,
               Config.Key,
               Config.KeyLength
               );
    if (!EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }

    DEBUG ((DEBUG_ERROR,
            "CephRbdDxe: monitor %u failed with %d, trying next\n",
            (UINT32)Index,
            (INT32)Status
            ));
    CephRbdDriverShutdown (&mCephRbdDriver);
  }

  return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
}

STATIC EFI_STATUS EFIAPI
CephRbdDriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer OPTIONAL
  )
{
  EFI_STATUS  Status;

  if ((This == NULL) || (ControllerHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  (VOID)NumberOfChildren;
  (VOID)ChildHandleBuffer;
  if (mCephRbdDriver.Signature != CEPH_RBD_DRIVER_SIGNATURE) {
    return EFI_NOT_STARTED;
  }

  if (mCephRbdDriver.BlockIoInstalled && (gBS != NULL)) {
    Status = gBS->UninstallProtocolInterface (
                    mCephRbdDriver.BlockIoHandle,
                    &gEfiBlockIoProtocolGuid,
                    &mCephRbdDriver.BlockIo
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    mCephRbdDriver.BlockIoInstalled = FALSE;
  }

  return CephRbdDriverShutdown (&mCephRbdDriver);
}

EFI_STATUS
EFIAPI
CephRbdDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS       Status;
  EFI_HANDLE       BindingHandle;
  CEPH_RBD_CONFIG  Config;

  (VOID)SystemTable;
  DEBUG ((DEBUG_INFO,
          "CephRbdDxe: firmware-side Ceph RBD boot driver\n"));

  if (gBS == NULL) {
    return EFI_UNSUPPORTED;
  }

  mCephRbdDriverBinding.ImageHandle = ImageHandle;
  mCephRbdDriverBinding.DriverBindingHandle = ImageHandle;
  BindingHandle = ImageHandle;
  Status = gBS->InstallProtocolInterface (
                  &BindingHandle,
                  &gEfiDriverBindingProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mCephRbdDriverBinding
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Firmware that cannot enumerate a network controller can still map the
  // configured image directly from the entry point.
  if ((gRT != NULL) &&
      !EFI_ERROR (CephRbdConfigLoad (&Config)) &&
      ((Config.Flags & CEPH_RBD_CONFIG_FLAG_START_ON_ENTRY) != 0))
  {
    Status = CephRbdDriverBindingStart (
               &mCephRbdDriverBinding,
               ImageHandle,
               NULL
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR,
              "CephRbdDxe: entry-point mapping failed with %d\n",
              (INT32)Status
              ));
    }
  }

  return EFI_SUCCESS;
}
