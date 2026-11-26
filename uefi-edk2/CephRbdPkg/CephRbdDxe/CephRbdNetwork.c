#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "CephRbdNetwork.h"

// gCephRbdNetworkGuid is generated from CephRbdPkg.dec by EDK II AutoGen in
// firmware builds and supplied by the host test file in host builds. Only the
// variable name lives here (as CephRbdConfig.c does for its variable name).
#ifndef CEPH_RBD_NETWORK_HOST_TEST
CONST CHAR16  gCephRbdNetworkVariableName[] = {
  'C', 'e', 'p', 'h', 'R', 'b', 'd', 'N', 'e', 't', 'w', 'o', 'r', 'k', 0
};
#endif

STATIC
BOOLEAN
Ipv4IsZero (
  IN CONST EFI_IPv4_ADDRESS  *Address
  )
{
  return (BOOLEAN)((Address->Addr[0] | Address->Addr[1] |
                    Address->Addr[2] | Address->Addr[3]) == 0);
}

EFI_STATUS
CephRbdNetworkValidate (
  IN CONST CEPH_RBD_NETWORK_CONFIG  *Config
  )
{
  if (Config == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Config->Signature != CEPH_RBD_NETWORK_SIGNATURE) ||
      (Config->Version != CEPH_RBD_NETWORK_VERSION) ||
      (Config->Length != sizeof (*Config)))
  {
    return EFI_INVALID_PARAMETER;
  }
  if ((Config->Mode != CEPH_RBD_NETWORK_MODE_DHCP) &&
      (Config->Mode != CEPH_RBD_NETWORK_MODE_STATIC))
  {
    return EFI_INVALID_PARAMETER;
  }
  if ((Config->Flags & ~CEPH_RBD_NETWORK_FLAG_GATEWAY) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (Config->Mode == CEPH_RBD_NETWORK_MODE_STATIC) {
    // A static configuration must supply a non-zero station address and a
    // non-zero subnet mask. The gateway is optional.
    if (Ipv4IsZero (&Config->StationAddress) ||
        Ipv4IsZero (&Config->SubnetMask))
    {
      return EFI_INVALID_PARAMETER;
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdNetworkParse (
  IN  CONST VOID                *Buffer,
  IN  UINTN                     BufferLength,
  OUT CEPH_RBD_NETWORK_CONFIG   *Config
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
  Status = CephRbdNetworkValidate (Config);
  if (EFI_ERROR (Status)) {
    ZeroMem (Config, sizeof (*Config));
  }
  return Status;
}

EFI_STATUS
CephRbdNetworkLoad (
  OUT CEPH_RBD_NETWORK_CONFIG  *Config
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
                  (CHAR16 *)gCephRbdNetworkVariableName,
                  &gCephRbdNetworkGuid,
                  NULL,
                  &DataSize,
                  Config
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return CephRbdNetworkValidate (Config);
}

EFI_STATUS
CephRbdNetworkResolveLocalIp (
  IN     CONST CEPH_RBD_NETWORK_CONFIG  *Config OPTIONAL,
  OUT    CEPH_RBD_LOCAL_IP_CONFIG       *LocalIp
  )
{
  if (LocalIp == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (LocalIp, sizeof (*LocalIp));

  // No explicit configuration, or DHCP mode: defer to the platform's IP4
  // auto-configuration (which performs DHCP in OVMF).
  if ((Config == NULL) || (Config->Mode == CEPH_RBD_NETWORK_MODE_DHCP)) {
    LocalIp->UseDefaultAddress = TRUE;
    return EFI_SUCCESS;
  }

  if (Config->Mode == CEPH_RBD_NETWORK_MODE_STATIC) {
    LocalIp->UseDefaultAddress = FALSE;
    CopyMem (
      &LocalIp->StationAddress,
      &Config->StationAddress,
      sizeof (EFI_IPv4_ADDRESS)
      );
    CopyMem (
      &LocalIp->SubnetMask,
      &Config->SubnetMask,
      sizeof (EFI_IPv4_ADDRESS)
      );
    return EFI_SUCCESS;
  }

  return EFI_UNSUPPORTED;
}
