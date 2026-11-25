#ifndef CEPH_RBD_CRUSH_H_
#define CEPH_RBD_CRUSH_H_

#include <Uefi.h>

// Minimal, bounds-checked CRUSH decoder and rule evaluator. It implements the
// subset needed for replicated RBD placement: straw2 buckets, TAKE,
// CHOOSE_FIRSTN, CHOOSELEAF_FIRSTN and EMIT. Other bucket algorithms and MSR
// rules are reported as EFI_UNSUPPORTED rather than guessed at.

#define CEPH_RBD_CRUSH_MAGIC            0x00010000U

#define CEPH_RBD_CRUSH_BUCKET_UNIFORM   1
#define CEPH_RBD_CRUSH_BUCKET_LIST      2
#define CEPH_RBD_CRUSH_BUCKET_TREE      3
#define CEPH_RBD_CRUSH_BUCKET_STRAW     4
#define CEPH_RBD_CRUSH_BUCKET_STRAW2    5

#define CEPH_RBD_CRUSH_RULE_TAKE        1
#define CEPH_RBD_CRUSH_RULE_CHOOSE_FIRSTN  2
#define CEPH_RBD_CRUSH_RULE_CHOOSE_INDEP   3
#define CEPH_RBD_CRUSH_RULE_EMIT        4
#define CEPH_RBD_CRUSH_RULE_CHOOSELEAF_FIRSTN 6
#define CEPH_RBD_CRUSH_RULE_CHOOSELEAF_INDEP  7

#define CEPH_RBD_CRUSH_HASH_RJENKINS1   0

#define CEPH_RBD_CRUSH_MAX_BUCKETS      32
#define CEPH_RBD_CRUSH_MAX_ITEMS        64
#define CEPH_RBD_CRUSH_MAX_RULES        16
#define CEPH_RBD_CRUSH_MAX_STEPS        16
#define CEPH_RBD_CRUSH_MAX_RESULT       32
#define CEPH_RBD_CRUSH_MAX_NAME         64

// A CRUSH bucket. Only straw2 uses ItemWeights; the other algorithms are
// decoded for bounds checking but rejected by the rule evaluator.
typedef struct {
  INT32   Id;
  UINT16  Type;
  UINT8   Alg;
  UINT8   Hash;
  UINT32  Weight;
  UINT32  Size;
  INT32   Items[CEPH_RBD_CRUSH_MAX_ITEMS];
  UINT32  ItemWeights[CEPH_RBD_CRUSH_MAX_ITEMS];
} CEPH_RBD_CRUSH_BUCKET;

typedef struct {
  UINT32  Op;
  INT32   Arg1;
  INT32   Arg2;
} CEPH_RBD_CRUSH_STEP;

typedef struct {
  UINT8               Ruleset;
  UINT8               Type;
  UINT32              Len;
  CEPH_RBD_CRUSH_STEP Steps[CEPH_RBD_CRUSH_MAX_STEPS];
} CEPH_RBD_CRUSH_RULE;

typedef struct {
  UINT32  MaxBuckets;
  UINT32  MaxRules;
  UINT32  MaxDevices;
  UINT32  BucketCount;
  UINT32  RuleCount;
  CEPH_RBD_CRUSH_BUCKET Buckets[CEPH_RBD_CRUSH_MAX_BUCKETS];
  CEPH_RBD_CRUSH_RULE   Rules[CEPH_RBD_CRUSH_MAX_RULES];
  UINT32  ChooseTotalTries;
  UINT8   ChooseleafVaryR;
  UINT8   StrawCalcVersion;
  UINT32  AllowedBucketAlgs;
  UINT8   ChooseleafStable;
} CEPH_RBD_CRUSH_MAP;

// Decode a CrushWrapper-encoded blob (the bufferlist embedded in OSDMap).
EFI_STATUS
CephRbdCrushParseMap (
  IN  CONST VOID           *Buffer,
  IN  UINTN                BufferLength,
  OUT CEPH_RBD_CRUSH_MAP   *Map
  );

// Robert Jenkins string hash used by Ceph for object -> PG mapping.
UINT32
CephRbdCrushStrHash (
  IN CONST CHAR8  *Name,
  IN UINTN        NameLength
  );

// crush_hash32_2 / crush_hash32_3 for the Ceph rjenkins1 hash type.
UINT32
CephRbdCrushHash32_2 (
  IN UINT32  HashType,
  IN UINT32  A,
  IN UINT32  B
  );

// Evaluate a rule for a placement seed. OsdWeights, when provided, must have
// MaxDevices entries and zero marks an out OSD. The output is the acting set
// in Ceph order; the first entry is the primary.
EFI_STATUS
CephRbdCrushMapRule (
  IN  CONST CEPH_RBD_CRUSH_MAP  *Map,
  IN  UINT32                    RuleId,
  IN  UINT32                    Pps,
  IN  UINT32                    Size,
  IN  CONST UINT32              *OsdWeights  OPTIONAL,
  OUT INT32                     *OsdIds,
  IN  UINTN                     OsdIdsCapacity,
  OUT UINTN                     *OsdIdCount
  );

#endif
