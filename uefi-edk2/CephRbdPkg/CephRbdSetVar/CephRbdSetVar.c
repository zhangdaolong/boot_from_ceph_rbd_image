/** @file
  Minimal UEFI application that installs the CephRbdConfig and
  CephRbdNetworkConfig UEFI variables from blobs compiled into this image.

  This is a validation helper: it lets an OVMF guest exercise the real
  CephRbdDxe driver without depending on the Shell's dmpstore -l command,
  which cannot create variables under a non-EDK2-global GUID.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>

#include "CephRbdConfigBlob.h"

// Must match gCephRbdConfigGuid in CephRbdPkg.dec.
STATIC CONST EFI_GUID mCephRbdConfigGuid = {
  0x9E5F1A2B, 0x3C4D, 0x4E5F,
  { 0x8A, 0x9B, 0x0C, 0x1D, 0x2E, 0x3F, 0x4A, 0x5B }
};

// Must match gCephRbdNetworkGuid in CephRbdPkg.dec.
STATIC CONST EFI_GUID mCephRbdNetworkGuid = {
  0xA7E4C1B2, 0x3D5F, 0x4E68,
  { 0x9B, 0x12, 0x77, 0x0A, 0x44, 0x63, 0x2E, 0xF1 }
};

STATIC CONST CHAR16 mConfigName[] = L"CephRbdConfig";
STATIC CONST CHAR16 mNetworkName[] = L"CephRbdNetworkConfig";

// NV + BS + RT, matching the attributes used by the DXE driver.
#define CEPH_RBD_VAR_ATTRS  (EFI_VARIABLE_NON_VOLATILE | \
                             EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                             EFI_VARIABLE_RUNTIME_ACCESS)

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Print (L"CephRbdSetVar: writing CephRbdConfig (%d bytes)\n", MCEPHRBDCONFIG_SIZE);
  Status = gRT->SetVariable (
                  (CHAR16 *)mConfigName,
                  (EFI_GUID *)&mCephRbdConfigGuid,
                  CEPH_RBD_VAR_ATTRS,
                  MCEPHRBDCONFIG_SIZE,
                  mCephRbdConfig
                  );
  Print (L"CephRbdSetVar: CephRbdConfig status=%r\n", Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Print (L"CephRbdSetVar: writing CephRbdNetworkConfig (%d bytes)\n", MCEPHRBDNETWORKCONFIG_SIZE);
  Status = gRT->SetVariable (
                  (CHAR16 *)mNetworkName,
                  (EFI_GUID *)&mCephRbdNetworkGuid,
                  CEPH_RBD_VAR_ATTRS,
                  MCEPHRBDNETWORKCONFIG_SIZE,
                  mCephRbdNetworkConfig
                  );
  Print (L"CephRbdSetVar: CephRbdNetworkConfig status=%r\n", Status);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Print (L"CephRbdSetVar: done\n");
  return EFI_SUCCESS;
}
