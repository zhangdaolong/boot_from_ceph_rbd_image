// Live protocol probe: connects to a Ceph messenger v1 endpoint with the
// firmware wire sources, optionally performs the CephX MAuth handshake, then
// issues MON_GET_MAP and "osd getmap". Run on a host that can reach the
// cluster's v1 monitor port. This is a validation tool, not firmware.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Uefi.h>
#include "CephRbdMessenger.h"
#include "CephRbdMsgrV1Recovery.h"
#include "CephRbdRpc.h"
#include "CephRbdAuth.h"
#include "CephRbdPlacement.h"

EFI_BOOT_SERVICES *gBS = NULL;

// The canonical OSDMap blob sent as command reply data can exceed 4 KiB.
static UINT8 mOsdMapReply[CEPH_RPC_FRAME_MAX];
static UINT8 mOsdMapData[CEPH_RPC_FRAME_MAX];

// Decode a 32-hex-digit AES-128 secret into 16 raw bytes. Returns 0 on success.
static int
ParseSecret (const char *Hex, UINT8 *Out, UINTN OutCapacity)
{
  UINTN Index;
  char  Pair[3];

  if ((Hex == NULL) || (Out == NULL) || (OutCapacity < CEPHX_AES_KEY_SIZE)) {
    return -1;
  }
  if (strlen (Hex) != (CEPHX_AES_KEY_SIZE * 2)) {
    return -1;
  }

  for (Index = 0; Index < CEPHX_AES_KEY_SIZE; Index++) {
    Pair[0] = Hex[Index * 2];
    Pair[1] = Hex[Index * 2 + 1];
    Pair[2] = 0;
    if (((Pair[0] < '0' || Pair[0] > '9') &&
         (Pair[0] < 'a' || Pair[0] > 'f') &&
         (Pair[0] < 'A' || Pair[0] > 'F')) ||
        ((Pair[1] < '0' || Pair[1] > '9') &&
         (Pair[1] < 'a' || Pair[1] > 'f') &&
         (Pair[1] < 'A' || Pair[1] > 'F')))
    {
      return -1;
    }
    Out[Index] = (UINT8)strtoul (Pair, NULL, 16);
  }

  return 0;
}

int
main (
  int    argc,
  char **argv
  )
{
  CEPH_RBD_MSGR_V1_CLIENT          Client;
  CEPH_RBD_MSGR_V1_CONNECT_CONFIG  Config;
  CEPH_RBD_AUTH_SESSION            Session;
  CEPH_MON_REQUEST_HEADER          Header;
  UINT8                            Fsid[16];
  UINT8                            Secret[CEPHX_AES_KEY_SIZE];
  UINT8                            Request[256];
  UINT8                            Reply[4096];
  UINTN                            RequestLength;
  UINTN                            ReplyLength;
  UINT64                           Epoch;
  EFI_STATUS                       Status;
  UINT16                           Port;
  const char                      *Identity;

  // usage: live_probe <port> [address-hex] [identity] [secret-hex]
  if (argc < 2) {
    fprintf (stderr,
             "usage: live_probe <port> [address-hex] [identity] [secret-hex]\n"
             "  address-hex : 32 hex digits of cluster fsid (optional)\n"
             "  identity    : client id, e.g. firmware01 (optional)\n"
             "  secret-hex  : 32 hex digits of the AES-128 secret (optional)\n"
             "Authentication is performed only when identity and secret are given.\n");
    return 2;
  }

  Port = (UINT16)strtoul (argv[1], NULL, 10);
  memset (Fsid, 0, sizeof (Fsid));
  if (argc >= 3) {
    // Cluster fsid as 32 hex digits for protocol diagnostics only.
    const char *Hex = argv[2];
    UINTN       Index;
    for (Index = 0; (Index < 16) && (Hex[Index * 2] != 0); Index++) {
      char Pair[3] = { Hex[Index * 2], Hex[Index * 2 + 1], 0 };
      Fsid[Index] = (UINT8)strtoul (Pair, NULL, 16);
    }
  }

  Identity = (argc >= 4) ? argv[3] : NULL;
  if (argc >= 5) {
    if (ParseSecret (argv[4], Secret, sizeof (Secret)) != 0) {
      fprintf (stderr, "invalid secret: expected 32 hex digits\n");
      return 2;
    }
  } else {
    Identity = NULL;
  }

  memset (&Config, 0, sizeof (Config));
  Config.ImageHandle = (EFI_HANDLE)&Client;
  Config.ControllerHandle = (EFI_HANDLE)&Config;
  Config.RemoteAddress.Addr[0] = 192;
  Config.RemoteAddress.Addr[1] = 168;
  Config.RemoteAddress.Addr[2] = 122;
  Config.RemoteAddress.Addr[3] = 89;
  Config.RemotePort = (Port != 0) ? Port : 40338;
  Config.SupportedFeatures = ~UINT64_C(0);
  Config.HostType = CEPH_MSGR_CONNECT_HOST_CLIENT;
  Config.ProtocolVersion = 15;  // CEPH_MONC_PROTOCOL
  Config.AuthorizerProtocol = 0;
  Config.TimeoutSeconds = 5;

  Status = CephRbdMsgrV1ClientConnect (&Config, &Client);
  printf ("connect: status=%d seq=%llu/%llu tag=%u action=%d\n",
          Status,
          (unsigned long long)Client.Session.NextOutgoingSequence,
          (unsigned long long)Client.Session.LastIncomingSequence,
          Client.Negotiation.ConnectReply.Tag,
          Client.Negotiation.Action);
  if (EFI_ERROR (Status)) {
    return 1;
  }

  if (Identity != NULL) {
    Status = CephRbdAuthHandshake (
               &Client,
               (CONST CHAR8 *)Identity,
               strlen (Identity),
               Secret,
               sizeof (Secret),
               &Session
               );
    printf ("auth: status=%d global_id=%llu has_session_key=%d\n",
            Status,
            (unsigned long long)Session.GlobalId,
            Session.HasSessionKey);
    if (EFI_ERROR (Status)) {
      CephRbdMsgrV1ClientClose (&Client, Config.TimeoutSeconds);
      return 1;
    }
  }

  Header.HaveVersion = 0;
  Header.SessionMon = 0;
  Header.SessionMonTid = 0;
  Status = CephRpcEncodeMonGetMap (
             &Header, Fsid, 0, Request, sizeof (Request), &RequestLength
             );
  if (EFI_ERROR (Status)) {
    printf ("encode: status=%d\n", Status);
    CephRbdMsgrV1ClientClose (&Client, Config.TimeoutSeconds);
    return 1;
  }

  // MON_GET_MAP is answered with a MonMap (CEPH_MSG_MON_MAP=4), not an OSDMap.
  Status = CephRpcCall (
             &Client,
             CEPH_MSG_MON_GET_MAP,
             CEPH_MSG_MON_MAP,
             Request,
             RequestLength,
             NULL,
             0,
             Reply,
             sizeof (Reply),
             &ReplyLength,
             NULL,
             0,
             NULL
             );
  printf ("get_map: status=%d reply_len=%lu\n",
          Status, (unsigned long)ReplyLength);
  if (EFI_ERROR (Status)) {
    CephRbdMsgrV1ClientClose (&Client, Config.TimeoutSeconds);
    return 1;
  }

  Status = CephRpcDecodeMonMapEpoch (Reply, ReplyLength, &Epoch);
  printf ("epoch: status=%d value=%llu\n",
          Status, (unsigned long long)Epoch);
  if (EFI_ERROR (Status)) {
    CephRbdMsgrV1ClientClose (&Client, Config.TimeoutSeconds);
    return 1;
  }

  // Fetch the canonical OSDMap through the "osd getmap" monitor command.
  // MON_GET_MAP above only returns a MonMap, so a separate request is needed.
  {
    static const char Command[] = "{\"prefix\":\"osd getmap\"}";
    CEPH_RBD_PLACEMENT  Placement;
    INT32               OsdIds[CEPH_RBD_CRUSH_MAX_RESULT];
    UINTN               OsdCount;
    UINTN               CommandReplyLength;
    UINTN               OsdMapLength;
    INT32               Result;
    UINT32              Pg;

    Status = CephRpcEncodeMonCommand (
               &Header, Fsid, Command, sizeof (Command) - 1,
               Request, sizeof (Request), &RequestLength
               );
    if (EFI_ERROR (Status)) {
      printf ("osd_getmap encode: status=%d\n", Status);
      CephRbdMsgrV1ClientClose (&Client, Config.TimeoutSeconds);
      return 1;
    }

    Status = CephRpcCall (
               &Client,
               CEPH_MSG_MON_COMMAND,
               CEPH_MSG_MON_COMMAND_ACK,
               Request,
               RequestLength,
               NULL,
               0,
               mOsdMapReply,
               sizeof (mOsdMapReply),
               &CommandReplyLength,
               mOsdMapData,
               sizeof (mOsdMapData),
               &OsdMapLength
               );
    printf ("osd_getmap: status=%d reply_len=%lu map_len=%lu\n",
            Status, (unsigned long)CommandReplyLength,
            (unsigned long)OsdMapLength);
    if (EFI_ERROR (Status)) {
      CephRbdMsgrV1ClientClose (&Client, Config.TimeoutSeconds);
      return 1;
    }

    Status = CephRpcDecodeMonCommandAck (
               mOsdMapReply, CommandReplyLength, &Result, NULL, NULL
               );
    printf ("osd_getmap ack: status=%d result=%d\n", Status, Result);
    if (EFI_ERROR (Status) || (Result != 0)) {
      CephRbdMsgrV1ClientClose (&Client, Config.TimeoutSeconds);
      return 1;
    }

    Status = CephRbdPlacementFromOsdMap (
               mOsdMapData, OsdMapLength, 4, &Placement
               );
    printf ("placement: status=%d epoch=%llu pg_num=%u pgp_num=%u "
            "rule=%d size=%u flags=0x%x\n",
            Status, (unsigned long long)Placement.Epoch,
            Placement.PgNum, Placement.PgpNum,
            Placement.CrushRule, Placement.Size, Placement.Flags);
    if (EFI_ERROR (Status)) {
      CephRbdMsgrV1ClientClose (&Client, Config.TimeoutSeconds);
      return 1;
    }

    Status = CephRbdPlacementMapObject (
               &Placement, "foo", 3, &Pg, NULL,
               OsdIds, CEPH_RBD_CRUSH_MAX_RESULT, &OsdCount
               );
    printf ("object foo: status=%d pg=%u osd_count=%lu acting=[%d,%d,%d]\n",
            Status, Pg, (unsigned long)OsdCount,
            OsdCount > 0 ? OsdIds[0] : -1,
            OsdCount > 1 ? OsdIds[1] : -1,
            OsdCount > 2 ? OsdIds[2] : -1);
  }

  CephRbdMsgrV1ClientClose (&Client, Config.TimeoutSeconds);
  return EFI_ERROR (Status) ? 1 : 0;
}
