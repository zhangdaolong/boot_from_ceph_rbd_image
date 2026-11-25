#ifndef CEPH_RBD_DXE_H_
#define CEPH_RBD_DXE_H_

#include <Uefi.h>

EFI_STATUS
EFIAPI
CephRbdDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

#endif
