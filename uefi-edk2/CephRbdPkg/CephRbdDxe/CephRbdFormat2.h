#ifndef CEPH_RBD_FORMAT2_H_
#define CEPH_RBD_FORMAT2_H_

#include <Uefi.h>
#include "CephRbdLayout.h"

// RBD feature bits from include/rbd/features.h. Features that only affect
// the write path do not change how a read-only client maps objects.
#define CEPH_RBD_FEATURE_LAYERING       (1ULL << 0)
#define CEPH_RBD_FEATURE_STRIPINGV2     (1ULL << 1)
#define CEPH_RBD_FEATURE_EXCLUSIVE_LOCK (1ULL << 2)
#define CEPH_RBD_FEATURE_OBJECT_MAP     (1ULL << 3)
#define CEPH_RBD_FEATURE_FAST_DIFF      (1ULL << 4)
#define CEPH_RBD_FEATURE_DEEP_FLATTEN   (1ULL << 5)
#define CEPH_RBD_FEATURE_JOURNALING     (1ULL << 6)
#define CEPH_RBD_FEATURE_DATA_POOL      (1ULL << 7)
#define CEPH_RBD_FEATURE_MIGRATING      (1ULL << 8)
#define CEPH_RBD_FEATURE_NON_PRIMARY    (1ULL << 9)
#define CEPH_RBD_FEATURES_INCOMPATIBLE  (1ULL << 63)

// Read-only clients can ignore these; striping v2 is handled separately.
#define CEPH_RBD_FEATURES_READ_SAFE                          \
  (CEPH_RBD_FEATURE_LAYERING | CEPH_RBD_FEATURE_EXCLUSIVE_LOCK | \
   CEPH_RBD_FEATURE_OBJECT_MAP | CEPH_RBD_FEATURE_FAST_DIFF |   \
   CEPH_RBD_FEATURE_DEEP_FLATTEN | CEPH_RBD_FEATURE_JOURNALING)

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
