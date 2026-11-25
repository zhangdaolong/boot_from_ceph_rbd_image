#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/DriverBinding.h>

#include "CephRbdConfig.h"
#include "CephRbdDxe.h"

EFI_GUID gCephRbdConfigGuid = {
  0x9e5f1a2b, 0x3c4d, 0x4e5f, { 0x8a, 0x9b, 0x0c, 0x1d, 0x2e, 0x3f, 0x4a, 0x5b }
};

CONST CHAR16 gCephRbdConfigVariableName[] = {
  'C', 'e', 'p', 'h', 'R', 'b', 'd', 'C', 'o', 'n', 'f', 'i', 'g', 0
};

EFI_GUID gEfiDriverBindingProtocolGuid = {
  0x2a03a288, 0xaea0, 0x11d5, { 0xa9, 0xc5, 0x00, 0x10, 0x83, 0x20, 0x7c, 0xcf }
};

EFI_DRIVER_BINDING_PROTOCOL *GetTestDriverBinding (VOID);

static CEPH_RBD_CONFIG  mVariableConfig;
static UINTN            mVariableSize;

static EFI_STATUS EFIAPI
FakeGetVariable (
  IN CONST CHAR16   *VariableName,
  IN CONST EFI_GUID *VendorGuid,
  OUT OPTIONAL UINT32 *Attributes,
  IN OUT UINTN      *DataSize,
  OUT OPTIONAL VOID *Data
  )
{
  if ((VariableName == NULL) || (VendorGuid == NULL) || (DataSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((VariableName[0] != gCephRbdConfigVariableName[0]) ||
      (memcmp (VendorGuid, &gCephRbdConfigGuid, sizeof (*VendorGuid)) != 0))
  {
    return EFI_NOT_FOUND;
  }

  if (mVariableSize == 0) {
    return EFI_NOT_FOUND;
  }

  if (*DataSize < mVariableSize) {
    *DataSize = mVariableSize;
    return EFI_BUFFER_TOO_SMALL;
  }

  if (Data == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  memcpy (Data, &mVariableConfig, mVariableSize);
  *DataSize = mVariableSize;
  if (Attributes != NULL) {
    *Attributes = 0;
  }

  return EFI_SUCCESS;
}

static EFI_RUNTIME_SERVICES  mRuntimeServices = {
  FakeGetVariable
};

EFI_RUNTIME_SERVICES *gRT = &mRuntimeServices;

static void
MakeValidConfig (
  OUT CEPH_RBD_CONFIG  *Config
  )
{
  UINTN  Index;

  memset (Config, 0, sizeof (*Config));
  Config->Signature = CEPH_RBD_CONFIG_SIGNATURE;
  Config->Version = CEPH_RBD_CONFIG_VERSION;
  Config->Length = sizeof (*Config);
  Config->Flags = 0;
  Config->PoolId = 3;
  Config->MonitorCount = 2;
  Config->Monitors[0].Address.Addr[0] = 10;
  Config->Monitors[0].Address.Addr[1] = 20;
  Config->Monitors[0].Address.Addr[2] = 30;
  Config->Monitors[0].Address.Addr[3] = 11;
  Config->Monitors[0].Port = 6789;
  Config->Monitors[1].Address.Addr[0] = 10;
  Config->Monitors[1].Address.Addr[1] = 20;
  Config->Monitors[1].Address.Addr[2] = 30;
  Config->Monitors[1].Address.Addr[3] = 12;
  Config->Monitors[1].Port = 6789;
  for (Index = 0; Index < CEPH_RBD_CONFIG_FSID_SIZE; Index++) {
    Config->Fsid[Index] = (UINT8)(Index + 1);
  }

  memcpy (Config->ImageName, "guest-root", 10);
  Config->ImageNameLength = 10;
  memcpy (Config->Identity, "firmware", 8);
  Config->IdentityLength = 8;
  for (Index = 0; Index < 16; Index++) {
    Config->Key[Index] = (UINT8)Index;
  }

  Config->KeyLength = 16;
}

void
TestConfigAndDriverBinding (
  VOID
  )
{
  CEPH_RBD_CONFIG               Config;
  CEPH_RBD_CONFIG               Parsed;
  CEPH_RBD_CONFIG               Loaded;
  EFI_DRIVER_BINDING_PROTOCOL  *Binding;
  EFI_HANDLE                    ImageHandle;
  EFI_HANDLE                    ControllerHandle;
  UINT8                         Raw[sizeof (CEPH_RBD_CONFIG) + 8];

  MakeValidConfig (&Config);
  assert(CephRbdConfigValidate (&Config) == EFI_SUCCESS);

  Config.MonitorCount = 0;
  assert(CephRbdConfigValidate (&Config) == EFI_INVALID_PARAMETER);
  MakeValidConfig (&Config);

  Config.Monitors[0].Port = 0;
  assert(CephRbdConfigValidate (&Config) == EFI_INVALID_PARAMETER);
  MakeValidConfig (&Config);

  Config.KeyLength = 0;
  assert(CephRbdConfigValidate (&Config) == EFI_INVALID_PARAMETER);
  MakeValidConfig (&Config);

  memset (Raw, 0, sizeof (Raw));
  memcpy (Raw, &Config, sizeof (Config));
  assert(CephRbdConfigParse (Raw, sizeof (Raw), &Parsed) == EFI_SUCCESS);
  assert(memcmp (&Parsed, &Config, sizeof (Config)) == 0);
  assert(CephRbdConfigParse (Raw, sizeof (Config) - 1, &Parsed) == EFI_BAD_BUFFER_SIZE);

  Raw[0] ^= 0xff;
  assert(CephRbdConfigParse (Raw, sizeof (Raw), &Parsed) == EFI_INVALID_PARAMETER);

  mVariableSize = 0;
  assert(CephRbdConfigLoad (&Loaded) == EFI_NOT_FOUND);
  mVariableSize = sizeof (Config);
  memcpy (&mVariableConfig, &Config, sizeof (Config));
  assert(CephRbdConfigLoad (&Loaded) == EFI_SUCCESS);
  assert(memcmp (&Loaded, &Config, sizeof (Config)) == 0);

  // Entry point publishes a Driver Binding protocol without requiring a
  // live controller or network connection.
  ImageHandle = (EFI_HANDLE)&Config;
  ControllerHandle = (EFI_HANDLE)&Loaded;
  assert(CephRbdDxeEntryPoint (ImageHandle, NULL) == EFI_SUCCESS);
  Binding = GetTestDriverBinding ();
  assert(Binding != NULL);
  assert(Binding->Version == 1);
  assert(Binding->ImageHandle == ImageHandle);

  assert(Binding->Supported (Binding, ControllerHandle, NULL) == EFI_SUCCESS);
  mVariableSize = 0;
  assert(Binding->Supported (Binding, ControllerHandle, NULL) == EFI_UNSUPPORTED);
  mVariableSize = sizeof (Config);

  // Stop without a successful Start must be a no-op error, not a crash.
  assert(Binding->Stop (Binding, ControllerHandle, 0, NULL) == EFI_NOT_STARTED);

  puts("config and driver binding tests: passed");
}
