#ifndef CEPH_RBD_FORMAT2_H_
#define CEPH_RBD_FORMAT2_H_

#include <Uefi.h>
#include "CephRbdLayout.h"

#define CEPH_RBD_FEATURE_STRIPINGV2  (1ULL << 1)
#define CEPH_RBD_MAX_OBJECT_ORDER    31
#define CEPH_RBD_OBJECT_NAME_MAX     95
#define CEPH_RBD_OBJECT_PREFIX_MAX   (CEPH_RBD_OBJECT_NAME_MAX - 17)

typedef struct {
  CONST CHAR8  *ObjectPrefix;
  UINT64       ImageSize;
  UINT8        ObjectOrder;
  UINT64       Features;
  UINT64       StripeUnit;
  UINT32       StripeCount;
} CEPH_RBD_FORMAT2_METADATA;

typedef struct {
  CHAR8            ObjectPrefix[CEPH_RBD_OBJECT_PREFIX_MAX + 1];
  UINT64           Features;
  CEPH_RBD_LAYOUT  Layout;
} CEPH_RBD_FORMAT2_IMAGE;

EFI_STATUS
CephRbdFormat2Initialize (
  IN CONST CEPH_RBD_FORMAT2_METADATA  *Metadata,
  OUT CEPH_RBD_FORMAT2_IMAGE          *Image
  );

EFI_STATUS
CephRbdFormat2BuildObjectName (
  IN CONST CEPH_RBD_FORMAT2_IMAGE  *Image,
  IN UINT64                        ObjectNumber,
  OUT CHAR8                        *Buffer,
  IN UINTN                         BufferSize,
  OUT UINTN                        *NameLength
  );

#endif
