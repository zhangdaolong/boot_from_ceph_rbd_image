/** @file
  Minimal UEFI application used as the ESP bootloader in OVMF guest
  validation. It prints a unique marker so the firmware-side boot chain
  (CephRbdDxe -> GPT/ESP -> Boot Manager -> LoadImage/StartImage) can be
  confirmed end-to-end: seeing the marker proves firmware loaded and ran a
  binary directly out of the Ceph RBD image's ESP partition.

  This is a validation helper only and is never part of a production image.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  (VOID)ImageHandle;
  (VOID)SystemTable;

  Print (L"\n");
  Print (L"********************************************************\n");
  Print (L"* CEPH_RBD_BOOTLOADER_STARTED: booted from Ceph RBD ESP  *\n");
  Print (L"********************************************************\n");
  Print (L"\n");
  return EFI_SUCCESS;
}
