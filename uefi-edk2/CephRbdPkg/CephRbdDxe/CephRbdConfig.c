#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "CephRbdConfig.h"

// The configuration blob lives in a fixed UEFI variable. Host tests supply
// their own copy of this name (and of gCephRbdConfigGuid) in test_config.c;
// the firmware build uses the definitions here.
#ifndef CEPH_RBD_CONFIG_HOST_TEST
CONST CHAR16  gCephRbdConfigVariableName[] = {
  'C', 'e', 'p', 'h', 'R', 'b', 'd', 'C', 'o', 'n', 'f', 'i', 'g', 0
};
#endif

EFI_STATUS
CephRbdConfigValidate (
  IN CONST CEPH_RBD_CONFIG  *Config
  )
{
  UINTN  Index;
  UINTN  FsidNonZero;

  if (Config == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Config->Signature != CEPH_RBD_CONFIG_SIGNATURE) ||
      (Config->Version != CEPH_RBD_CONFIG_VERSION) ||
      (Config->Length != sizeof (*Config)))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((Config->MonitorCount == 0) ||
      (Config->MonitorCount > CEPH_RBD_CONFIG_MAX_MONITORS))
  {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < Config->MonitorCount; Index++) {
    if ((Config->Monitors[Index].Port == 0) ||
        (Config->Monitors[Index].Address.Addr[0] == 0) ||
        (Config->Monitors[Index].Address.Addr[3] == 0))
    {
      return EFI_INVALID_PARAMETER;
    }
  }

  FsidNonZero = 0;
  for (Index = 0; Index < CEPH_RBD_CONFIG_FSID_SIZE; Index++) {
    FsidNonZero |= Config->Fsid[Index];
  }

  if (FsidNonZero == 0) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Config->ImageNameLength == 0) ||
      (Config->ImageNameLength >= CEPH_RBD_CONFIG_MAX_IMAGE_NAME) ||
      (Config->IdentityLength == 0) ||
      (Config->IdentityLength >= CEPH_RBD_CONFIG_MAX_IDENTITY) ||
      (Config->KeyLength == 0) ||
      (Config->KeyLength > CEPH_RBD_CONFIG_MAX_KEY))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((Config->Flags & ~(CEPH_RBD_CONFIG_FLAG_KEY_REFERENCE |
                         CEPH_RBD_CONFIG_FLAG_START_ON_ENTRY |
                         CEPH_RBD_CONFIG_FLAG_BOOT_MANAGER |
                         CEPH_RBD_CONFIG_FLAG_TRY_BOOT)) != 0)
  {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdConfigParse (
  IN  CONST VOID       *Buffer,
  IN  UINTN             BufferLength,
  OUT CEPH_RBD_CONFIG   *Config
  )
{
  EFI_STATUS  Status;

  if ((Buffer == NULL) || (Config == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (BufferLength < sizeof (*Config)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  CopyMem (Config, Buffer, sizeof (*Config));
  Status = CephRbdConfigValidate (Config);
  if (EFI_ERROR (Status)) {
    ZeroMem (Config, sizeof (*Config));
  }

  return Status;
}

EFI_STATUS
CephRbdConfigLoad (
  OUT CEPH_RBD_CONFIG  *Config
  )
{
  EFI_STATUS  Status;
  UINTN       DataSize;

  if (Config == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (gRT == NULL) {
    return EFI_NOT_FOUND;
  }

  DataSize = sizeof (*Config);
  Status = gRT->GetVariable (
                  (CHAR16 *)gCephRbdConfigVariableName,
                  (EFI_GUID *)&gCephRbdConfigGuid,
                  NULL,
                  &DataSize,
                  Config
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return CephRbdConfigValidate (Config);
}
