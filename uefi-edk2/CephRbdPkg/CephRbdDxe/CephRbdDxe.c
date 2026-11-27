#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/Tcp4.h>
#include "CephRbdAuth.h"
#include "CephRbdBlockIo.h"
#include "CephRbdBootManager.h"
#include "CephRbdConfig.h"
#include "CephRbdDxe.h"
#include "CephRbdEspFat.h"
#include "CephRbdGpt.h"
#include "CephRbdNetwork.h"
#include "CephRbdSecretResolver.h"
#include "CephRbdMsgrV1Recovery.h"
#include "CephRbdOsdClient.h"

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
  CEPH_RBD_OSD_CLIENT_POOL     OsdPool;
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
           &Driver->OsdPool,
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
  EFI_STATUS               Status;
  UINT64                   LastLba;
  CEPH_RBD_AUTH_SESSION    Session;
  CEPH_RBD_SERVICE_AUTH    OsdAuth;

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

  // Ask the monitor for an OSD service ticket. Both metadata class-method
  // calls and data reads are OSD ops, so they must authenticate to OSD
  // endpoints directly rather than being sent over the monitor session.
  Status = CephRbdAuthGetServiceTicket (
             &Driver->Client,
             &Session,
             CEPH_ENTITY_TYPE_OSD,
             &OsdAuth
             );
  if (EFI_ERROR (Status) || !OsdAuth.Valid) {
    DEBUG ((DEBUG_ERROR,
            "CephRbdDxe: OSD service ticket request failed with %d\n",
            (INT32)Status
            ));
    CephRbdMsgrV1ClientClose (
      &Driver->Client,
      Driver->ConnectConfig.TimeoutSeconds
      );
    Driver->State = CephRbdStateFailed;
    return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
  }

  Status = CephRbdOsdClientPoolInit (
             &Driver->OsdPool,
             &OsdAuth,
             &Driver->ConnectConfig
             );
  if (EFI_ERROR (Status)) {
    CephRbdOsdClientPoolClose (&Driver->OsdPool);
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
             &Driver->OsdPool,
             PoolId,
             ImageName,
             ImageNameLength,
             Fsid,
             &Driver->Device
             );
  if (EFI_ERROR (Status)) {
    CephRbdOsdClientPoolClose (&Driver->OsdPool);
    CephRbdMsgrV1ClientClose (
      &Driver->Client,
      Driver->ConnectConfig.TimeoutSeconds
      );
    Driver->State = CephRbdStateFailed;
    return Status;
  }

  Status = CephRbdBlockIoGetMediaLastLba (&Driver->Device, &LastLba);
  if (EFI_ERROR (Status)) {
    CephRbdOsdClientPoolClose (&Driver->OsdPool);
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
      CephRbdOsdClientPoolClose (&Driver->OsdPool);
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

  CephRbdOsdClientPoolClose (&Driver->OsdPool);
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
  EFI_STATUS       Status;

  if ((This == NULL) || (ControllerHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  (VOID)RemainingDevicePath;

  // Bind only to a controller that exposes the TCP4 service binding, i.e. a
  // configured network device. Accepting arbitrary handles makes the DXE
  // dispatcher bind to non-network controllers first, where the TCP4 open in
  // Start() fails and (because this driver is a singleton) never retries on
  // the real network handle. Host stubs do not model TCP4 controller handles,
  // so this check is firmware-only.
#ifndef CEPH_RBD_CONFIG_HOST_TEST
  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiTcp4ServiceBindingProtocolGuid,
                  NULL,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_TEST_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }
#endif

  Status = CephRbdConfigLoad (&Config);
  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

// The EFI-backed Boot Manager handoff is only compiled for real firmware.
// Host core tests exercise CephRbdBootManager.c separately with mocks, and the
// host stubs do not declare gBS->LoadImage/gRT->SetVariable.
#ifndef CEPH_RBD_CONFIG_HOST_TEST

// Adapter that lets the GPT parser read LBAs through the mapped BlockIo
// device. The driver has already authenticated an OSD session, so these reads
// reuse the driver's OSD client pool rather than opening new connections.
typedef struct {
  CEPH_RBD_OSD_CLIENT_POOL  *Pool;
  CEPH_RBD_BLOCK_DEVICE     *Device;
} CEPH_RBD_DXE_GPT_CONTEXT;

STATIC
EFI_STATUS
CephRbdDxeReadLba (
  IN     VOID   *Context,
  IN     UINT64  Lba,
  OUT    UINT8  *Buffer,
  IN     UINTN   BufferSize
  )
{
  CEPH_RBD_DXE_GPT_CONTEXT  *Ctx;

  if ((Context == NULL) || (Buffer == NULL) ||
      ((BufferSize % CEPH_RBD_BLOCK_SIZE) != 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Ctx = (CEPH_RBD_DXE_GPT_CONTEXT *)Context;
  return CephRbdBlockIoReadBlocks (
           Ctx->Pool, Ctx->Device, Lba,
           BufferSize / CEPH_RBD_BLOCK_SIZE, Buffer, BufferSize
           );
}

// Load and start \EFI\BOOT\BOOTX64.EFI directly from an allocated buffer.
// Using SourceBuffer rather than the synthetic HD(GPT) device path makes the
// immediate boot self-contained: no partition/FAT child handles are required
// (those are absent when the driver is loaded from the UEFI shell).
STATIC
EFI_STATUS
CephRbdDxeTryBootFromBuffer (
  IN OUT CEPH_RBD_DRIVER  *Driver,
  IN     EFI_HANDLE        ParentImageHandle,
  IN     UINT64            EspStartLba
  )
{
  EFI_STATUS                Status;
  VOID                      *Loader;
  UINTN                     LoaderSize;
  EFI_HANDLE                ImageHandle;
  CEPH_RBD_DXE_GPT_CONTEXT  GptCtx;

  Loader = NULL;
  LoaderSize = 0;
  ImageHandle = NULL;

  GptCtx.Pool = &Driver->OsdPool;
  GptCtx.Device = &Driver->Device;
  Status = CephRbdEspFatLoadBootloader (
             CephRbdDxeReadLba,
             (VOID *)&GptCtx,
             EspStartLba,
             &Loader,
             &LoaderSize
             );
  Print (L"CephRbdDxe: FAT loader read status=%r size=%d\n",
         Status, (INT32)LoaderSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->LoadImage (
                  FALSE,
                  ParentImageHandle,
                  NULL,
                  Loader,
                  LoaderSize,
                  &ImageHandle
                  );
  Print (L"CephRbdDxe: LoadImage(buffer) status=%r\n", Status);
  if (EFI_ERROR (Status)) {
    FreePool (Loader);
    return Status;
  }

  // The loader is now independent of the source buffer.
  FreePool (Loader);

  Status = gBS->StartImage (ImageHandle, NULL, NULL);
  Print (L"CephRbdDxe: StartImage status=%r\n", Status);
  return Status;
}

// Discover the ESP on the freshly mapped image and register a Boot#### load
// option for its x64 fallback loader. When TryBoot is TRUE the loader is also
// handed to LoadImage()/StartImage() immediately. Failure to find an ESP or
// install the option is non-fatal: the BlockIo device remains published.
STATIC
EFI_STATUS
CephRbdDxeBootstrapHandoff (
  IN OUT CEPH_RBD_DRIVER  *Driver,
  IN     EFI_HANDLE        ParentImageHandle,
  IN     BOOLEAN           TryBoot
  )
{
  EFI_STATUS                     Status;
  CEPH_RBD_DXE_GPT_CONTEXT       GptCtx;
  CEPH_RBD_GPT_PARTITION         Esp;
  CEPH_RBD_BOOT_MANAGER_BACKEND  Backend;
  UINT64                         LastLba;
  UINT32                         PartitionNumber;
  UINT16                         BootIndex;

  if ((Driver == NULL) || (Driver->State != CephRbdStateDeviceReady)) {
    return EFI_NOT_READY;
  }

  Status = CephRbdBlockIoGetMediaLastLba (&Driver->Device, &LastLba);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Print (L"CephRbdDxe: boot handoff start, lastLba=%llu\n",
         (unsigned long long)LastLba);

  GptCtx.Pool = &Driver->OsdPool;
  GptCtx.Device = &Driver->Device;
  Status = CephRbdGptDiscoverEsp (
             CephRbdDxeReadLba, &GptCtx, LastLba, &Esp, &PartitionNumber
             );
  Print (L"CephRbdDxe: ESP discover status=%r partition=%d\n",
         Status, (INT32)PartitionNumber);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN,
            "CephRbdDxe: ESP discovery failed with %d (non-fatal)\n",
            (INT32)Status
            ));
    return Status;
  }

  Status = CephRbdBootManagerGetEfiBackend (&Backend);
  if (EFI_ERROR (Status)) {
    Print (L"CephRbdDxe: BootManager backend status=%r\n", Status);
    return Status;
  }

  // Always register the load option without attempting the device-path boot.
  // The synthetic HD(GPT) path only resolves when the platform partition/FAT
  // drivers have created child handles for this disk, which does not happen
  // for a driver loaded from the UEFI shell. Immediate boot, when requested,
  // is done separately from an in-memory buffer below.
  Status = CephRbdBootManagerHandoff (
             &Backend,
             &Esp,
             PartitionNumber,
             CEPH_RBD_BM_AUTO_BOOT_INDEX,
             FALSE,
             ParentImageHandle,
             &BootIndex
             );
  Print (L"CephRbdDxe: BootManager handoff status=%r Boot%04x\n",
         Status, BootIndex);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((DEBUG_INFO,
          "CephRbdDxe: registered Boot%04x for Ceph RBD ESP (partition %u)\n",
          BootIndex, PartitionNumber
          ));
  Print (L"CephRbdDxe: registered Boot%04x for Ceph RBD ESP (partition %d)\n",
         BootIndex, (INT32)PartitionNumber);

  if (TryBoot) {
    // Read the loader directly from the ESP sectors and start it from a
    // buffer; this needs no partition child handle and proves the firmware
    // can execute a binary read straight off the Ceph RBD image.
    Status = CephRbdDxeTryBootFromBuffer (
               Driver,
               ParentImageHandle,
               Esp.FirstLba
               );
    Print (L"CephRbdDxe: try-boot-from-buffer status=%r\n", Status);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

#endif  // !CEPH_RBD_CONFIG_HOST_TEST

STATIC EFI_STATUS EFIAPI
CephRbdDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  CEPH_RBD_CONFIG                  Config;
  CEPH_RBD_NETWORK_CONFIG          NetworkConfig;
  CEPH_RBD_LOCAL_IP_CONFIG         LocalIp;
  CEPH_RBD_MSGR_V1_CONNECT_CONFIG  ConnectConfig;
#ifndef CEPH_RBD_CONFIG_HOST_TEST
  UINT8                            ResolvedKey[CEPHX_MAX_KEY_SIZE];
  UINTN                            ResolvedKeyLength;
#endif
  CONST UINT8                      *Secret;
  UINTN                            SecretLength;
  EFI_STATUS                       Status;
  UINTN                            Index;

  if ((This == NULL) || (ControllerHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  (VOID)RemainingDevicePath;

  // The driver is a singleton: CephRbdDriverInitialize() ZeroMem()s the one
  // static instance and InstallProtocolInterface() stores a pointer into it,
  // not a copy. A second Start() on the same controller (the DXE dispatcher
  // re-drives Start() after the shell-loaded image installs DriverBinding,
  // bypassing the entry-point helper's driver-model bookkeeping) would wipe
  // the published BlockIo interface and make the handle disappear from
  // `dh -p BlockIo`. Treat a repeated Start() on the ready device as a
  // no-op success; refuse to bind a second controller to the singleton.
  if ((mCephRbdDriver.Signature == CEPH_RBD_DRIVER_SIGNATURE) &&
      (mCephRbdDriver.State == CephRbdStateDeviceReady))
  {
    Print (L"CephRbdDxe: Start() already ready on %p (skip)\n",
           mCephRbdDriver.ConnectConfig.ControllerHandle);
    if (mCephRbdDriver.ConnectConfig.ControllerHandle == ControllerHandle) {
      return EFI_SUCCESS;
    }
    return EFI_ALREADY_STARTED;
  }

  Print (L"CephRbdDxe: Start() controller=%p\n", ControllerHandle);
  Status = CephRbdConfigLoad (&Config);
  if (EFI_ERROR (Status)) {
    Print (L"CephRbdDxe: config load failed %r\n", Status);
    return Status;
  }
  Print (L"CephRbdDxe: config monitors=%d flags=0x%x image=%a\n",
         Config.MonitorCount, Config.Flags, Config.ImageName);

  // Resolve the local IPv4 configuration. An absent or DHCP-mode variable
  // resolves UseDefaultAddress TRUE so the TCP4 layer uses platform DHCP.
  ZeroMem (&NetworkConfig, sizeof (NetworkConfig));
  if (EFI_ERROR (CephRbdNetworkLoad (&NetworkConfig))) {
    Status = CephRbdNetworkResolveLocalIp (NULL, &LocalIp);
  } else {
    Status = CephRbdNetworkResolveLocalIp (&NetworkConfig, &LocalIp);
  }
  if (EFI_ERROR (Status)) {
    Print (L"CephRbdDxe: network resolve failed %r\n", Status);
    return Status;
  }
  Print (L"CephRbdDxe: local IP default=%d station=%d.%d.%d.%d\n",
         LocalIp.UseDefaultAddress,
         LocalIp.StationAddress.Addr[0], LocalIp.StationAddress.Addr[1],
         LocalIp.StationAddress.Addr[2], LocalIp.StationAddress.Addr[3]);

  // Resolve the CephX secret. Raw key bytes are used directly; a KEY_REFERENCE
  // is passed through the platform secret resolver to obtain the raw key.
  if ((Config.Flags & CEPH_RBD_CONFIG_FLAG_KEY_REFERENCE) != 0) {
#ifndef CEPH_RBD_CONFIG_HOST_TEST
    CEPH_RBD_SECRET_RESOLVER_BACKEND  Resolver;

    Status = CephRbdSecretResolverGetEfiBackend (&Resolver);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = CephRbdSecretResolve (
               &Resolver,
               (CONST CHAR8 *)Config.Key,
               Config.KeyLength,
               ResolvedKey,
               sizeof (ResolvedKey),
               &ResolvedKeyLength
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR,
              "CephRbdDxe: failed to resolve key reference with %d\n",
              (INT32)Status
              ));
      return Status;
    }
    Secret = ResolvedKey;
    SecretLength = ResolvedKeyLength;
#else
    // Host tests do not exercise the firmware-only resolver; reject here.
    DEBUG ((DEBUG_ERROR,
            "CephRbdDxe: key references need a platform secret resolver\n"));
    return EFI_UNSUPPORTED;
#endif
  } else {
    Secret = Config.Key;
    SecretLength = Config.KeyLength;
  }

  // Try every configured monitor before giving up. This is simple failover:
  // the first monitor that accepts the handshake and maps the image wins.
  for (Index = 0; Index < Config.MonitorCount; Index++) {
    ZeroMem (&ConnectConfig, sizeof (ConnectConfig));
    ConnectConfig.ImageHandle = This->ImageHandle;
    ConnectConfig.ControllerHandle = ControllerHandle;
    ConnectConfig.RemoteAddress = Config.Monitors[Index].Address;
    ConnectConfig.RemotePort = Config.Monitors[Index].Port;
    // Advertise the full messenger-v1 feature set. Ceph 22 monitors require
    // CEPH_FEATURE_MSG_AUTH (and CEPHX_V2) when cephx signatures are enabled;
    // advertising only 0x0F makes the monitor reply CEPH_MSGR_TAG_FEATURES and
    // reject the connection. The effective session features are the
    // intersection with the monitor reply, so the footer/frame layout stays
    // exactly what the peer negotiates (legacy 13-byte footer here).
    ConnectConfig.SupportedFeatures = 0xFFFFFFFFFFFFFFFFULL;
    ConnectConfig.RequiredFeatures = 0;
    ConnectConfig.HostType = CEPH_MSGR_CONNECT_HOST_CLIENT;
    ConnectConfig.ProtocolVersion = 15;  // CEPH_MONC_PROTOCOL
    ConnectConfig.AuthorizerProtocol = 0;
    ConnectConfig.Flags = 0;
    ConnectConfig.TimeoutSeconds = 5;
    ConnectConfig.UseExplicitLocalIp = TRUE;
    CopyMem (
      &ConnectConfig.LocalIp,
      &LocalIp,
      sizeof (LocalIp)
      );

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
               Secret,
               SecretLength
               );
    Print (L"CephRbdDxe: monitor %d %d.%d.%d.%d:%d init status=0x%llx\n",
           (INT32)Index,
           Config.Monitors[Index].Address.Addr[0],
           Config.Monitors[Index].Address.Addr[1],
           Config.Monitors[Index].Address.Addr[2],
           Config.Monitors[Index].Address.Addr[3],
           Config.Monitors[Index].Port,
           (unsigned long long)Status);
    if (!EFI_ERROR (Status)) {
      Print (L"CephRbdDxe: image mapped, BlockIo published\n");
#ifndef CEPH_RBD_CONFIG_HOST_TEST
      // Self-check: enumerate BlockIo handles from inside the driver. The
      // UEFI shell can stall after loading a driver that installs a new
      // DriverBinding (BDS ConnectAll), so a shell-side `dh -p BlockIo` is
      // not a reliable probe here. Confirming the handle database directly
      // proves the published device is visible to the rest of firmware.
      {
        EFI_HANDLE  *BlockHandles;
        UINTN        BlockCount;

        BlockHandles = NULL;
        BlockCount = 0;
        Status = gBS->LocateHandleBuffer (
                        ByProtocol,
                        &gEfiBlockIoProtocolGuid,
                        NULL,
                        &BlockCount,
                        &BlockHandles
                        );
        if (!EFI_ERROR (Status)) {
          Print (L"CephRbdDxe: BlockIo handle count=%d (self-check)\n",
                 (INT32)BlockCount);
          gBS->FreePool (BlockHandles);
        } else {
          Print (L"CephRbdDxe: BlockIo enumerate failed %r\n", Status);
        }
        Status = EFI_SUCCESS;
      }
#endif
      // With the BlockIo device published, optionally drive the GPT/ESP ->
      // UEFI Boot Manager handoff. This is intentionally best-effort: the
      // mapped read-only disk is still useful without an ESP.
#ifndef CEPH_RBD_CONFIG_HOST_TEST
      Print (L"CephRbdDxe: BM flag check flags=0x%x\n", Config.Flags);
      if ((Config.Flags & CEPH_RBD_CONFIG_FLAG_BOOT_MANAGER) != 0) {
        EFI_STATUS  HandoffStatus;

        Print (L"CephRbdDxe: invoking boot manager handoff tryBoot=%d\n",
               (INT32)((Config.Flags & CEPH_RBD_CONFIG_FLAG_TRY_BOOT) != 0));
        HandoffStatus = CephRbdDxeBootstrapHandoff (
                          &mCephRbdDriver,
                          This->ImageHandle,
                          (Config.Flags & CEPH_RBD_CONFIG_FLAG_TRY_BOOT) != 0
                          );
        if (EFI_ERROR (HandoffStatus)) {
          DEBUG ((DEBUG_WARN,
                  "CephRbdDxe: boot manager handoff failed with %d\n",
                  (INT32)HandoffStatus
                  ));
        }
      }
#endif
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

#ifndef CEPH_RBD_CONFIG_HOST_TEST

// The DXE image handle has no TCP4 service binding, so a driver loaded from
// the UEFI shell cannot start on ImageHandle directly: CephRbdTcp4ConnectEx()
// would fail to open the TCP4 service binding on it. Standard DXE connect
// semantics do not cover a shell-loaded image either. Locate a configured
// network controller explicitly and start the singleton driver on it. Each
// candidate that fails is shut down before the next one is tried.
STATIC
EFI_STATUS
CephRbdDxeStartOnNetworkController (
  IN EFI_DRIVER_BINDING_PROTOCOL  *Binding
  )
{
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  EFI_STATUS  Status;
  UINTN       Index;

  Handles = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiTcp4ServiceBindingProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN,
            "CephRbdDxe: cannot locate TCP4 controller: %d\n",
            (INT32)Status
            ));
    return Status;
  }

  if (HandleCount == 0) {
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = CephRbdDriverBindingStart (Binding, Handles[Index], NULL);
    if (!EFI_ERROR (Status)) {
      gBS->FreePool (Handles);
      return EFI_SUCCESS;
    }
    DEBUG ((DEBUG_WARN,
            "CephRbdDxe: Start on TCP4 controller %u failed: %d\n",
            (UINT32)Index, (INT32)Status
            ));
  }

  gBS->FreePool (Handles);
  return EFI_DEVICE_ERROR;
}

#endif  // !CEPH_RBD_CONFIG_HOST_TEST

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
  Print (L"CephRbdDxe: entry point\n");
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
#ifndef CEPH_RBD_CONFIG_HOST_TEST
    Status = CephRbdDxeStartOnNetworkController (&mCephRbdDriverBinding);
#else
    // Host stubs do not model a TCP4 controller handle.
    Status = EFI_UNSUPPORTED;
#endif
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR,
              "CephRbdDxe: entry-point mapping failed with %d\n",
              (INT32)Status
              ));
    }
  }

  return EFI_SUCCESS;
}
