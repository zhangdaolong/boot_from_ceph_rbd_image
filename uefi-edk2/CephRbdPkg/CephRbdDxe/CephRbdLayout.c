#include <Uefi.h>
#include "CephRbdLayout.h"

EFI_STATUS
CephRbdValidateLayout (
  IN CONST CEPH_RBD_LAYOUT  *Layout
  )
{
  if ((Layout == NULL) || (Layout->ImageSize == 0) ||
      (Layout->ObjectSize == 0) || (Layout->StripeUnit == 0) ||
      (Layout->StripeCount == 0) ||
      (Layout->ObjectSize < Layout->StripeUnit) ||
      ((Layout->ObjectSize % Layout->StripeUnit) != 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdMapExtent (
  IN CONST CEPH_RBD_LAYOUT  *Layout,
  IN UINT64                 ImageOffset,
  IN UINT64                 MaximumLength,
  OUT CEPH_RBD_EXTENT       *Extent
  )
{
  EFI_STATUS  Status;
  UINT64      StripeNumber;
  UINT64      StripeSet;
  UINT64      StripesPerObject;
  UINT64      ObjectSet;
  UINT64      ObjectNumber;
  UINT64      StripeOffset;
  UINT64      ObjectOffset;
  UINT64      RemainingInStripe;
  UINT64      RemainingInImage;
  UINT64      Length;

  if (Extent == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephRbdValidateLayout (Layout);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((MaximumLength == 0) || (ImageOffset >= Layout->ImageSize)) {
    return EFI_INVALID_PARAMETER;
  }

  StripeNumber      = ImageOffset / Layout->StripeUnit;
  StripeSet         = StripeNumber / Layout->StripeCount;
  StripesPerObject  = Layout->ObjectSize / Layout->StripeUnit;
  ObjectSet         = StripeSet / StripesPerObject;
  StripeOffset      = ImageOffset % Layout->StripeUnit;

  // Check object-number multiplication before computing it.
  if (ObjectSet > (MAX_UINT64 - (StripeNumber % Layout->StripeCount)) / Layout->StripeCount) {
    return EFI_BAD_BUFFER_SIZE;
  }

  ObjectNumber = (ObjectSet * Layout->StripeCount) + (StripeNumber % Layout->StripeCount);

  // ObjectOffset is bounded by ObjectSize when the layout is valid.
  ObjectOffset = ((StripeSet % StripesPerObject) * Layout->StripeUnit) + StripeOffset;

  RemainingInStripe = Layout->StripeUnit - StripeOffset;
  RemainingInImage  = Layout->ImageSize - ImageOffset;
  Length            = MaximumLength;
  if (Length > RemainingInStripe) {
    Length = RemainingInStripe;
  }

  if (Length > RemainingInImage) {
    Length = RemainingInImage;
  }

  Extent->ObjectNumber = ObjectNumber;
  Extent->ObjectOffset = ObjectOffset;
  Extent->Length       = Length;
  return EFI_SUCCESS;
}
