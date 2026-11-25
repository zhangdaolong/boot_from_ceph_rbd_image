#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include "CephRbdFormat2.h"

EFI_STATUS
CephRbdFormat2Initialize (
  IN CONST CEPH_RBD_FORMAT2_METADATA  *Metadata,
  OUT CEPH_RBD_FORMAT2_IMAGE          *Image
  )
{
  UINTN  PrefixLength;

  if ((Metadata == NULL) || (Image == NULL) ||
      (Metadata->ObjectPrefix == NULL) || (Metadata->ImageSize == 0) ||
      (Metadata->ObjectOrder > CEPH_RBD_MAX_OBJECT_ORDER))
  {
    return EFI_INVALID_PARAMETER;
  }

  PrefixLength = 0;
  while ((PrefixLength <= CEPH_RBD_OBJECT_PREFIX_MAX) &&
         (Metadata->ObjectPrefix[PrefixLength] != '\0'))
  {
    PrefixLength++;
  }

  if ((PrefixLength == 0) || (PrefixLength > CEPH_RBD_OBJECT_PREFIX_MAX)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  // Reject data-pool/migration/non-primary layouts and any unknown or
  // incompatible feature, but accept the write-path-only features that do
  // not change how a read-only client locates object data.
  if ((Metadata->Features &
       ~(CEPH_RBD_FEATURES_READ_SAFE | CEPH_RBD_FEATURE_STRIPINGV2)) != 0)
  {
    return EFI_UNSUPPORTED;
  }

  if ((Metadata->Features & CEPH_RBD_FEATURE_STRIPINGV2) == 0) {
    if ((Metadata->StripeUnit != 0) || (Metadata->StripeCount != 0)) {
      return EFI_COMPROMISED_DATA;
    }

    Image->Layout.StripeUnit  = LShiftU64 (1, Metadata->ObjectOrder);
    Image->Layout.StripeCount = 1;
  } else {
    if ((Metadata->StripeUnit == 0) || (Metadata->StripeCount == 0)) {
      return EFI_COMPROMISED_DATA;
    }

    Image->Layout.StripeUnit  = Metadata->StripeUnit;
    Image->Layout.StripeCount = Metadata->StripeCount;
  }

  Image->Layout.ImageSize  = Metadata->ImageSize;
  Image->Layout.ObjectSize = LShiftU64 (1, Metadata->ObjectOrder);
  if (EFI_ERROR (CephRbdValidateLayout (&Image->Layout))) {
    return EFI_COMPROMISED_DATA;
  }

  Image->Features = Metadata->Features;
  CopyMem (Image->ObjectPrefix, Metadata->ObjectPrefix, PrefixLength);
  Image->ObjectPrefix[PrefixLength] = '\0';
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdFormat2BuildObjectName (
  IN CONST CEPH_RBD_FORMAT2_IMAGE  *Image,
  IN UINT64                        ObjectNumber,
  OUT CHAR8                        *Buffer,
  IN UINTN                         BufferSize,
  OUT UINTN                        *NameLength
  )
{
  STATIC CONST CHAR8  HexDigits[] = "0123456789abcdef";
  UINTN               PrefixLength;
  UINTN               Index;
  UINTN               RequiredSize;

  if ((Image == NULL) || (Buffer == NULL) || (NameLength == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  PrefixLength = 0;
  while ((PrefixLength <= CEPH_RBD_OBJECT_PREFIX_MAX) &&
         (Image->ObjectPrefix[PrefixLength] != '\0'))
  {
    PrefixLength++;
  }

  if ((PrefixLength == 0) || (PrefixLength > CEPH_RBD_OBJECT_PREFIX_MAX)) {
    return EFI_COMPROMISED_DATA;
  }

  // RBD object numbers in the object namespace are 16 lowercase hex digits.
  RequiredSize = PrefixLength + 1 + 16 + 1;
  if (BufferSize < RequiredSize) {
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (Buffer, Image->ObjectPrefix, PrefixLength);
  Buffer[PrefixLength] = '.';
  for (Index = 0; Index < 16; Index++) {
    Buffer[PrefixLength + 1 + Index] =
      HexDigits[(ObjectNumber >> ((15 - Index) * 4)) & 0x0f];
  }

  Buffer[RequiredSize - 1] = '\0';
  *NameLength = RequiredSize - 1;
  return EFI_SUCCESS;
}
