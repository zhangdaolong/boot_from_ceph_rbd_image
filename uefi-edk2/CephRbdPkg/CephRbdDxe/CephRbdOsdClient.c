#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include "CephRbdOsdClient.h"

EFI_STATUS
CephRbdOsdClientPoolInit (
  OUT CEPH_RBD_OSD_CLIENT_POOL              *Pool,
  IN  CONST CEPH_RBD_SERVICE_AUTH           *OsdAuth,
  IN  CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG *BaseConfig
  )
{
  if ((Pool == NULL) || (OsdAuth == NULL) || !OsdAuth->Valid ||
      (BaseConfig == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Pool, sizeof (*Pool));
  CopyMem (&Pool->OsdAuth, OsdAuth, sizeof (Pool->OsdAuth));
  CopyMem (&Pool->BaseConfig, BaseConfig, sizeof (Pool->BaseConfig));
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdOsdClientPoolGet (
  IN OUT CEPH_RBD_OSD_CLIENT_POOL   *Pool,
  IN     CONST CEPH_RBD_PLACEMENT   *Placement,
  IN     INT32                      OsdId,
  OUT    CEPH_RBD_MSGR_V1_CLIENT    **Client
  )
{
  EFI_STATUS                      Status;
  CEPH_RBD_OSD_SESSION           *Session;
  CEPH_RBD_MSGR_V1_CONNECT_CONFIG Config;
  UINT32                          Ipv4;
  UINT16                          Port;
  CONST UINT8                    *AddrBytes;
  UINTN                           Index;
  UINTN                           FreeIndex;

  if ((Pool == NULL) || (Placement == NULL) || (Client == NULL) ||
      (OsdId < 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  // Reuse an existing authenticated session for this OSD.
  for (Index = 0; Index < CEPH_RBD_MAX_OSD_SESSIONS; Index++) {
    Session = &Pool->Sessions[Index];
    if (Session->InUse && (Session->OsdId == OsdId)) {
      *Client = &Session->Client;
      return EFI_SUCCESS;
    }
  }

  FreeIndex = CEPH_RBD_MAX_OSD_SESSIONS;
  for (Index = 0; Index < CEPH_RBD_MAX_OSD_SESSIONS; Index++) {
    if (!Pool->Sessions[Index].InUse) {
      FreeIndex = Index;
      break;
    }
  }
  if (FreeIndex == CEPH_RBD_MAX_OSD_SESSIONS) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = CephRbdPlacementGetOsdAddress (Placement, OsdId, &Ipv4, &Port);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Config = Pool->BaseConfig;
  AddrBytes = (CONST UINT8 *)&Ipv4;
  Config.RemoteAddress.Addr[0] = AddrBytes[0];
  Config.RemoteAddress.Addr[1] = AddrBytes[1];
  Config.RemoteAddress.Addr[2] = AddrBytes[2];
  Config.RemoteAddress.Addr[3] = AddrBytes[3];
  Config.RemotePort = Port;
  Config.ProtocolVersion = CEPH_OSDC_PROTOCOL;
  Config.HostType = CEPH_MSGR_CONNECT_HOST_CLIENT;
  // The authorizer is built from the OSD service ticket by
  // CephRbdAuthConnectOsd; the template's monitor authorizer is not reused.
  Config.AuthorizerProtocol = 0;
  Config.Authorizer = NULL;
  Config.AuthorizerLength = 0;
  // Our legacy MOSDOp path does not sign message footers and relies on the
  // OSD canonicalizing the PG seed; do not negotiate either feature.
  Config.SupportedFeatures &= ~(CEPH_FEATURE_MSG_AUTH |
                                CEPH_FEATURE_RESEND_ON_SPLIT);
  // Give each OSD connection a distinct nonce so its CephX authorizer is not
  // a replay of another session's challenge.
  Config.ClientNonce = Pool->BaseConfig.ClientNonce + FreeIndex + 1;

  Session = &Pool->Sessions[FreeIndex];
  ZeroMem (Session, sizeof (*Session));
  Status = CephRbdAuthConnectOsd (&Config, &Pool->OsdAuth, &Session->Client);
  if (EFI_ERROR (Status)) {
    ZeroMem (Session, sizeof (*Session));
    return Status;
  }
  if (!Session->Client.Session.Ready) {
    CephRbdMsgrV1ClientClose (
      &Session->Client,
      Config.TimeoutSeconds != 0 ? Config.TimeoutSeconds : 2
      );
    ZeroMem (Session, sizeof (*Session));
    return EFI_DEVICE_ERROR;
  }

  Session->OsdId = OsdId;
  Session->InUse = TRUE;
  *Client = &Session->Client;
  return EFI_SUCCESS;
}

VOID
CephRbdOsdClientPoolRelease (
  IN OUT CEPH_RBD_OSD_CLIENT_POOL   *Pool,
  IN     INT32                      OsdId
  )
{
  UINTN Index;

  if (Pool == NULL) {
    return;
  }

  for (Index = 0; Index < CEPH_RBD_MAX_OSD_SESSIONS; Index++) {
    if (Pool->Sessions[Index].InUse && (Pool->Sessions[Index].OsdId == OsdId)) {
      if (Pool->Sessions[Index].Client.Connected) {
        CephRbdMsgrV1ClientClose (
          &Pool->Sessions[Index].Client,
          Pool->BaseConfig.TimeoutSeconds != 0 ?
          Pool->BaseConfig.TimeoutSeconds : 2
          );
      }
      ZeroMem (&Pool->Sessions[Index], sizeof (Pool->Sessions[Index]));
      return;
    }
  }
}

VOID
CephRbdOsdClientPoolClose (
  IN OUT CEPH_RBD_OSD_CLIENT_POOL   *Pool
  )
{
  UINTN Index;

  if (Pool == NULL) {
    return;
  }

  for (Index = 0; Index < CEPH_RBD_MAX_OSD_SESSIONS; Index++) {
    if (!Pool->Sessions[Index].InUse) {
      continue;
    }
    if (Pool->Sessions[Index].Client.Connected) {
      CephRbdMsgrV1ClientClose (
        &Pool->Sessions[Index].Client,
        Pool->BaseConfig.TimeoutSeconds != 0 ?
        Pool->BaseConfig.TimeoutSeconds : 2
        );
    }
    ZeroMem (&Pool->Sessions[Index], sizeof (Pool->Sessions[Index]));
  }
}
