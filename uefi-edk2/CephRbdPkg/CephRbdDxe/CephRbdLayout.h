#ifndef CEPH_RBD_LAYOUT_H_
#define CEPH_RBD_LAYOUT_H_

#include <Uefi.h>

typedef struct {
  UINT64  ImageSize;
  UINT64  ObjectSize;
  UINT64  StripeUnit;
  UINT32  StripeCount;
} CEPH_RBD_LAYOUT;

typedef struct {
  UINT64  ObjectNumber;
  UINT64  ObjectOffset;
  UINT64  Length;
} CEPH_RBD_EXTENT;

EFI_STATUS
CephRbdValidateLayout (
  IN CONST CEPH_RBD_LAYOUT  *Layout
  );

// Map one contiguous logical image range to one RBD object extent. The caller
// repeats this for the remaining bytes, issuing one object read per extent.
EFI_STATUS
CephRbdMapExtent (
  IN CONST CEPH_RBD_LAYOUT  *Layout,
  IN UINT64                 ImageOffset,
  IN UINT64                 MaximumLength,
  OUT CEPH_RBD_EXTENT       *Extent
  );

#endif
