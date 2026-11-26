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
#include "CephRbdBlockIo.h"

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
  CEPH_RBD_MSGR_V1_CLIENT          OsdClient;
  CEPH_RBD_MSGR_V1_CONNECT_CONFIG  OsdConfig;
  CEPH_RBD_AUTH_SESSION            Session;
  CEPH_RBD_SERVICE_AUTH            OsdAuth;
  CEPH_RBD_OSD_CLIENT_POOL         OsdPool;
  CEPH_RBD_BLOCK_DEVICE            Device;
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
  Config.ProtocolVersion = CEPH_MONC_PROTOCOL;
  Config.AuthorizerProtocol = 0;
  Config.TimeoutSeconds = 15;

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
    if (Session.HasSessionKey) {
      printf ("auth session key:");
      for (int KeyIndex = 0; KeyIndex < CEPHX_AES_KEY_SIZE; KeyIndex++) {
        printf (" %02x", Session.SessionKey[KeyIndex]);
      }
      printf ("\n");
    }
    if (EFI_ERROR (Status)) {
      CephRbdMsgrV1ClientClose (&Client, Config.TimeoutSeconds);
      return 1;
    }

    if (Session.AuthTicket.Valid) {
      printf ("auth ticket: secret_id=%llu blob_len=%lu\n",
              (unsigned long long)Session.AuthTicket.SecretId,
              (unsigned long)Session.AuthTicket.BlobLength);
    }

    // Request the OSD service ticket from the monitor using the AUTH ticket
    // as old_ticket. This is the credential needed to authenticate to OSDs.
    Status = CephRbdAuthGetServiceTicket (
               &Client, &Session, CEPH_ENTITY_TYPE_OSD, &OsdAuth
               );
    printf ("osd_service_ticket: status=%d valid=%d secret_id=%llu "
            "blob_len=%lu\n",
            Status, OsdAuth.Valid,
            (unsigned long long)OsdAuth.Ticket.SecretId,
            (unsigned long)OsdAuth.Ticket.BlobLength);
    if (OsdAuth.Valid) {
      printf ("osd session key:");
      for (int KeyIndex = 0; KeyIndex < CEPHX_AES_KEY_SIZE; KeyIndex++) {
        printf (" %02x", OsdAuth.SessionKey[KeyIndex]);
      }
      printf ("\n");

      // Build the per-OSD authenticated session pool exactly as the DXE
      // driver does at Start(). All subsequent OSD ops reuse this pool.
      Status = CephRbdOsdClientPoolInit (&OsdPool, &OsdAuth, &Config);
      printf ("osd_pool_init: status=%d\n", Status);
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

    // Report each OSD's messenger-v1 endpoint decoded from the live OSDMap.
    // Ipv4 is stored in network byte order.
    {
      UINTN  AddrIndex;
      printf ("osd_addrs: count=%u\n", (unsigned)Placement.OsdCount);
      for (AddrIndex = 0; AddrIndex < Placement.OsdCount; AddrIndex++) {
        const CEPH_RBD_OSD_ADDR *Addr = &Placement.OsdAddr[AddrIndex];
        const UINT8             *Bytes = (const UINT8 *)&Addr->Ipv4;
        printf ("  osd%d: %u.%u.%u.%u:%u\n", (int)Addr->OsdId,
                Bytes[0], Bytes[1], Bytes[2], Bytes[3],
                (unsigned)Addr->Port);
      }
    }

    // Connect to the primary OSD for a known object, authenticate with the
    // OSD service ticket, then issue a real OSD READ op against it.
    {
      static const CHAR8  TestObject[] = "rbd_id.close-race-test";
      INT32               TestOsdIds[16];
      UINTN               TestOsdCount;
      UINT32              TestPg;
      UINT32              TestPrimaryIpv4;
      UINT16              TestPrimaryPort;
      CEPH_OSD_OP_REQUEST ReadOp;
      UINT8               OpRequest[512];
      UINTN               OpRequestLength;
      UINT8               OpReplyFront[512];
      UINT8               OpReplyData[256];
      UINTN               OpReplyFrontLength;
      UINTN               OpReplyDataLength;
      CEPH_OSD_OP_REPLY   OpReply;

      Status = CephRbdPlacementMapObject (
                 &Placement, TestObject, sizeof (TestObject) - 1,
                 &TestPg, NULL, TestOsdIds, 16, &TestOsdCount
                 );
      printf ("test object: status=%d pg=%u osd_count=%lu acting=[%d,%d,%d]\n",
              Status, TestPg, (unsigned long)TestOsdCount,
              TestOsdCount > 0 ? TestOsdIds[0] : -1,
              TestOsdCount > 1 ? TestOsdIds[1] : -1,
              TestOsdCount > 2 ? TestOsdIds[2] : -1);

      if (!EFI_ERROR (Status) && (TestOsdCount > 0) && OsdAuth.Valid &&
          !EFI_ERROR (CephRbdPlacementGetOsdAddress (
                        &Placement, TestOsdIds[0],
                        &TestPrimaryIpv4, &TestPrimaryPort)))
      {
        const UINT8 *Bytes = (const UINT8 *)&TestPrimaryIpv4;

        memset (&OsdConfig, 0, sizeof (OsdConfig));
        OsdConfig.ImageHandle = (EFI_HANDLE)&OsdClient;
        OsdConfig.ControllerHandle = (EFI_HANDLE)&OsdConfig;
        OsdConfig.RemoteAddress.Addr[0] = Bytes[0];
        OsdConfig.RemoteAddress.Addr[1] = Bytes[1];
        OsdConfig.RemoteAddress.Addr[2] = Bytes[2];
        OsdConfig.RemoteAddress.Addr[3] = Bytes[3];
        OsdConfig.RemotePort = TestPrimaryPort;
        // Do not advertise MSG_AUTH: this probe does not sign message
        // footers, and Ceph verifies signatures whenever the feature is
        // negotiated. The cluster has cephx_require_signatures=false.
        //
        // Do not advertise RESEND_ON_SPLIT (ceph_features.h bit 21,
        // incarnation 2). Our MOSDOp uses the legacy v1 encoding: the OSD
        // recalculates the placement seed from the object name and stores the
        // full 32-bit Jenkins hash in pg_t::ps. When RESEND_ON_SPLIT is
        // advertised the OSD trusts that raw pgid directly and enqueues it,
        // yielding e.g. "4.1228e1dc ... no pg, shouldn't exist". Without the
        // feature the OSD routes through dispatch_session_waiting(), which
        // calls raw_pg_to_pg() to reduce the hash with ceph_stable_mod()
        // against the pool's pg_num/pg_num_mask, producing the canonical PG
        // (for this object, 4.1c / pg 28) and finding the primary OSD.
        OsdConfig.SupportedFeatures = Config.SupportedFeatures &
                                      ~(UINT64_C(1) << 23) &
                                      ~(UINT64_C(1) << 21);
        OsdConfig.RequiredFeatures = Config.RequiredFeatures;
        OsdConfig.HostType = CEPH_MSGR_CONNECT_HOST_CLIENT;
        OsdConfig.ProtocolVersion = CEPH_OSDC_PROTOCOL;
        OsdConfig.TimeoutSeconds = Config.TimeoutSeconds;
        OsdConfig.ClientNonce = Config.ClientNonce;

        Status = CephRbdAuthConnectOsd (&OsdConfig, &OsdAuth, &OsdClient);
        printf ("osd_connect: osd=%d addr=%u.%u.%u.%u:%u status=%d "
                "ready=%d action=%d\n",
                (int)TestOsdIds[0], Bytes[0], Bytes[1], Bytes[2], Bytes[3],
                (unsigned)TestPrimaryPort, Status,
                OsdClient.Session.Ready,
                (int)OsdClient.Negotiation.Action);
        printf ("osd_negotiation: features=0x%llx msg_auth=%d footer_size=%lu\n",
                (unsigned long long)OsdClient.Negotiation.ConnectReply.Features,
                (int)((OsdClient.Negotiation.ConnectReply.Features &
                       CEPH_FEATURE_MSG_AUTH) != 0),
                (unsigned long)OsdClient.Session.FooterSize);

        if (!EFI_ERROR (Status) && OsdClient.Session.Ready) {
          memset (&ReadOp, 0, sizeof (ReadOp));
          ReadOp.Op = CEPH_OSD_OP_READ;
          ReadOp.Flags = CEPH_OSD_FLAG_READ;
          ReadOp.Offset = 0;
          ReadOp.Length = 16;

          Status = CephRpcEncodeOsdOp (
                     4, 1, (UINT32)Placement.Epoch, 1,
                     TestObject, sizeof (TestObject) - 1,
                     &ReadOp, 1,
                     OpRequest, sizeof (OpRequest), &OpRequestLength
                     );
          if (!EFI_ERROR (Status)) {
            // Call directly (no auto-reconnect) so the real OSD op error is
            // visible; the generic reconnect path has no CephX service ticket.
            Status = CephRpcCall (
                       &OsdClient, CEPH_MSG_OSD_OP, CEPH_MSG_OSD_OPREPLY,
                       OpRequest, OpRequestLength,
                       NULL, 0,
                       OpReplyFront, sizeof (OpReplyFront), &OpReplyFrontLength,
                       OpReplyData, sizeof (OpReplyData), &OpReplyDataLength
                       );
          }
          printf ("osd_read: status=%d reply_len=%lu data_len=%lu\n",
                  Status, (unsigned long)OpReplyFrontLength,
                  (unsigned long)OpReplyDataLength);

          if (!EFI_ERROR (Status)) {
            Status = CephRpcDecodeOsdOpReply (
                       OpReplyFront, OpReplyFrontLength,
                       OpReplyData, OpReplyDataLength, &OpReply
                       );
          printf ("osd_read reply: status=%d result=%d num_ops=%u "
                  "data_len=%lu\n",
                  Status, OpReply.Result, OpReply.NumOps,
                  (unsigned long)OpReply.DataLength);
            if (!EFI_ERROR (Status) && (OpReply.DataLength > 0)) {
              UINTN  DataIndex;
              printf ("osd_read data:");
              for (DataIndex = 0; DataIndex < OpReply.DataLength &&
                                  DataIndex < 32; DataIndex++) {
                printf (" %02x", OpReply.Data[DataIndex]);
              }
              printf ("\n");
            }
          }
        }

        if (OsdClient.Connected) {
          CephRbdMsgrV1ClientClose (&OsdClient, OsdConfig.TimeoutSeconds);
        }
      }
    }

    // End-to-end validation of the DXE data path. Metadata discovery and
    // block reads both run through the per-OSD authenticated session pool,
    // never over the monitor connection.
    if (!EFI_ERROR (Status) && OsdAuth.Valid) {
      static const CHAR8  DxeImage[] = "close-race-test";
      UINT8               BlockData[512];
      UINTN               DataIndex;

      memset (&Device, 0, sizeof (Device));
      Status = CephRbdBlockIoDiscover (
                 &Client, &OsdPool, 4,
                 DxeImage, sizeof (DxeImage) - 1,
                 Fsid, &Device
                 );
      printf ("dxe_discover: status=%d ready=%d image_size=%llu\n",
              Status, Device.Ready,
              (unsigned long long)Device.Image.Layout.ImageSize);

      if (!EFI_ERROR (Status)) {
        memset (BlockData, 0, sizeof (BlockData));
        Status = CephRbdBlockIoReadBlocks (
                   &OsdPool, &Device, 0, 1, BlockData, sizeof (BlockData)
                   );
        printf ("dxe_read: status=%d data:", Status);
        for (DataIndex = 0; DataIndex < 32; DataIndex++) {
          printf (" %02x", BlockData[DataIndex]);
        }
        printf ("\n");
      }

      CephRbdOsdClientPoolClose (&OsdPool);
    }

  }

  CephRbdMsgrV1ClientClose (&Client, Config.TimeoutSeconds);
  return EFI_ERROR (Status) ? 1 : 0;
}
