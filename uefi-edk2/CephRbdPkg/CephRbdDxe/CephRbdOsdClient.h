#ifndef CEPH_RBD_OSD_CLIENT_H_
#define CEPH_RBD_OSD_CLIENT_H_

#include <Uefi.h>
#include "CephRbdAuth.h"
#include "CephRbdPlacement.h"
#include "CephRbdMsgrV1Recovery.h"

// Firmware boots a single image, so a small cache of one authenticated
// messenger-v1 session per OSD covers every object of that image.
#define CEPH_RBD_MAX_OSD_SESSIONS  16

// CEPH_FEATURE_RESEND_ON_SPLIT (ceph_features.h, incarnation 2 bit 21).
// When advertised the OSD trusts the raw pgid carried in our legacy MOSDOp
// instead of canonicalizing it against pg_num, which makes the OSD drop the
// op ("no pg, shouldn't exist"). See tests/live/live_probe.c for details.
#define CEPH_FEATURE_RESEND_ON_SPLIT  0x00200000ULL

typedef struct {
  INT32                       OsdId;
  CEPH_RBD_MSGR_V1_CLIENT     Client;
  BOOLEAN                     InUse;
} CEPH_RBD_OSD_SESSION;

typedef struct {
  CEPH_RBD_SERVICE_AUTH           OsdAuth;
  CEPH_RBD_MSGR_V1_CONNECT_CONFIG BaseConfig;
  CEPH_RBD_OSD_SESSION            Sessions[CEPH_RBD_MAX_OSD_SESSIONS];
} CEPH_RBD_OSD_CLIENT_POOL;

// Initialize a pool from the OSD service ticket obtained on the authenticated
// monitor connection and a template connect configuration (monitor endpoint
// fields are replaced per OSD by CephRbdOsdClientPoolGet).
EFI_STATUS
CephRbdOsdClientPoolInit (
  OUT CEPH_RBD_OSD_CLIENT_POOL              *Pool,
  IN  CONST CEPH_RBD_SERVICE_AUTH           *OsdAuth,
  IN  CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG *BaseConfig
  );

// Return an authenticated client for OsdId, connecting and caching it on
// first use. Ipv4/port are resolved from Placement (decoded from OSDMap).
EFI_STATUS
CephRbdOsdClientPoolGet (
  IN OUT CEPH_RBD_OSD_CLIENT_POOL   *Pool,
  IN     CONST CEPH_RBD_PLACEMENT   *Placement,
  IN     INT32                      OsdId,
  OUT    CEPH_RBD_MSGR_V1_CLIENT    **Client
  );

// Drop the cached session for OsdId (used to force a fresh connect after a
// transport/RPC failure). The next PoolGet reconnects with the same ticket.
VOID
CephRbdOsdClientPoolRelease (
  IN OUT CEPH_RBD_OSD_CLIENT_POOL   *Pool,
  IN     INT32                      OsdId
  );

// Close every cached OSD session.
VOID
CephRbdOsdClientPoolClose (
  IN OUT CEPH_RBD_OSD_CLIENT_POOL   *Pool
  );

#endif
