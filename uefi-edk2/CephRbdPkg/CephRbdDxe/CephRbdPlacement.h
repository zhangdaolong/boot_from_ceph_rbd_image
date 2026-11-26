#ifndef CEPH_RBD_PLACEMENT_H_
#define CEPH_RBD_PLACEMENT_H_

#include <Uefi.h>
#include "CephRbdCrush.h"

// pg_pool_t flags and object hash values from include/rados.h and ceph_hash.h.
#define CEPH_RBD_POOL_FLAG_HASHPSPOOL   0x00000001U
#define CEPH_RBD_STR_HASH_RJENKINS      0x2U

// entity_addr_t type values (msg/msg_types.h).
#define CEPH_RBD_ADDR_TYPE_NONE         0U
#define CEPH_RBD_ADDR_TYPE_LEGACY       1U  // messenger v1
#define CEPH_RBD_ADDR_TYPE_ANY          2U  // messenger v2

// Maximum number of OSD client addresses retained from an OSDMap.
#define CEPH_RBD_MAX_OSDS               16

// One OSD's messenger-v1 client address. Ipv4 is stored in network byte
// order, Port in host byte order, so the pair can be copied directly into
// CephRbdTcp4/CEPH_RBD_MSGR_V1_CONNECT_CONFIG.
typedef struct {
  INT32   OsdId;
  UINT32  Ipv4;
  UINT16  Port;
  BOOLEAN Valid;
} CEPH_RBD_OSD_ADDR;

// Placement parameters decoded from one pg_pool_t inside an OSDMap.
typedef struct {
  UINT64  Epoch;
  INT32   PoolId;
  UINT8   Type;
  UINT8   Size;
  INT32   CrushRule;
  UINT8   ObjectHash;
  UINT32  PgNum;
  UINT32  PgpNum;
  UINT32  Flags;
} CEPH_RBD_OSD_POOL;

// Everything needed to place an object in a pool. The CRUSH map is parsed
// once and retained so each read can map its object name to an acting set.
typedef struct {
  UINT64                 Epoch;
  INT32                  PoolId;
  UINT8                  Type;
  UINT32                 PgNum;
  UINT32                 PgpNum;
  INT32                  CrushRule;
  UINT8                  Size;
  UINT8                  ObjectHash;
  UINT32                 Flags;
  BOOLEAN                Valid;
  CEPH_RBD_CRUSH_MAP     Crush;
  UINT32                 OsdCount;
  CEPH_RBD_OSD_ADDR      OsdAddr[CEPH_RBD_MAX_OSDS];
} CEPH_RBD_PLACEMENT;

// Decode a CEPH_MSG_OSD_MAP front payload and extract the placement
// parameters for PoolId plus the embedded CRUSH map. The blob is located by
// its length-prefixed magic and then fully validated by the CRUSH parser, so
// no fixed field offsets into the middle of OSDMap are assumed.
EFI_STATUS
CephRbdPlacementFromOsdMap (
  IN  CONST VOID           *Buffer,
  IN  UINTN                BufferLength,
  IN  INT32                PoolId,
  OUT CEPH_RBD_PLACEMENT   *Placement
  );

// Map an object name to its placement group and acting OSD set. Pg and Pps
// may be NULL if the caller only needs the OSD list. The first OSD id in
// OsdIds is the primary.
EFI_STATUS
CephRbdPlacementMapObject (
  IN  CONST CEPH_RBD_PLACEMENT  *Placement,
  IN  CONST CHAR8               *ObjectName,
  IN  UINTN                     ObjectNameLength,
  OUT UINT32                    *Pg,  OPTIONAL
  OUT UINT32                    *Pps,  OPTIONAL
  OUT INT32                     *OsdIds,
  IN  UINTN                     OsdIdsCapacity,
  OUT UINTN                     *OsdIdCount
  );

// Look up the messenger-v1 client address of OsdId from the decoded OSDMap.
// Ipv4 is returned in network byte order and Port in host byte order.
EFI_STATUS
CephRbdPlacementGetOsdAddress (
  IN  CONST CEPH_RBD_PLACEMENT  *Placement,
  IN  INT32                     OsdId,
  OUT UINT32                    *Ipv4,
  OUT UINT16                    *Port
  );

#endif
