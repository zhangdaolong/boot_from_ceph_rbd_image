#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "CephRbdAuth.h"
#include "CephRbdRpc.h"
#include "CephRbdWire.h"

static void
WriteLe32 (UINT8 *Buffer, UINT32 Value)
{
  Buffer[0] = (UINT8)Value;
  Buffer[1] = (UINT8)(Value >> 8);
  Buffer[2] = (UINT8)(Value >> 16);
  Buffer[3] = (UINT8)(Value >> 24);
}

static void
WriteLe64 (UINT8 *Buffer, UINT64 Value)
{
  WriteLe32 (Buffer, (UINT32)Value);
  WriteLe32 (Buffer + 4, (UINT32)(Value >> 32));
}

static void
TestEncodeInitialRequest (VOID)
{
  UINT8             Buffer[256];
  UINTN             Length;
  CEPH_WIRE_CURSOR  Cursor;
  UINT64            Version;
  UINT16            SessionMon;
  UINT64            SessionMonTid;
  UINT32            Protocol;
  UINT32            BlobLength;
  UINT8             StructVersion;
  UINT32            Count;
  UINT32            Supported;
  UINT32            EntityType;
  UINT32            StringLength;
  UINT8             EntityId[CEPHX_MAX_ENTITY_NAME];
  UINT64            GlobalId;
  UINT32            Epoch;

  memset (Buffer, 0, sizeof (Buffer));
  assert(CephRbdAuthEncodeInitialRequest (
           (CONST CHAR8 *)"firmware01", 10,
           Buffer, sizeof (Buffer), &Length) == EFI_SUCCESS);

  // AuthPayload: u8(1) + u32(1) + u32(method) + u32(type) + u32(len=10)
  //              + 10 bytes + u64(0) = 35 bytes.
  // MAuth front: u64 + s16 + u64 + u32 + (u32 + 35) + u32 = 65 bytes.
  assert(Length == 65);

  assert(CephWireInit (&Cursor, Buffer, Length) == EFI_SUCCESS);
  assert(CephWireReadLe64 (&Cursor, &Version) == EFI_SUCCESS);
  assert(Version == 0);
  assert(CephWireReadLe16 (&Cursor, &SessionMon) == EFI_SUCCESS);
  assert(SessionMon == (UINT16)0xFFFF);
  assert(CephWireReadLe64 (&Cursor, &SessionMonTid) == EFI_SUCCESS);
  assert(SessionMonTid == 0);
  assert(CephWireReadLe32 (&Cursor, &Protocol) == EFI_SUCCESS);
  assert(Protocol == CEPH_AUTH_UNKNOWN);
  assert(CephWireReadLe32 (&Cursor, &BlobLength) == EFI_SUCCESS);
  assert(BlobLength == 35);

  // Inner auth_payload.
  assert(CephWireReadU8 (&Cursor, &StructVersion) == EFI_SUCCESS);
  assert(StructVersion == 1);
  assert(CephWireReadLe32 (&Cursor, &Count) == EFI_SUCCESS);
  assert(Count == 1);
  assert(CephWireReadLe32 (&Cursor, &Supported) == EFI_SUCCESS);
  assert(Supported == CEPH_AUTH_CEPHX);
  assert(CephWireReadLe32 (&Cursor, &EntityType) == EFI_SUCCESS);
  assert(EntityType == CEPH_ENTITY_TYPE_CLIENT);
  assert(CephWireReadLe32 (&Cursor, &StringLength) == EFI_SUCCESS);
  assert(StringLength == 10);
  assert(CephWireReadBytes (&Cursor, EntityId, 10) == EFI_SUCCESS);
  assert(memcmp (EntityId, "firmware01", 10) == 0);
  assert(CephWireReadLe64 (&Cursor, &GlobalId) == EFI_SUCCESS);
  assert(GlobalId == 0);

  assert(CephWireReadLe32 (&Cursor, &Epoch) == EFI_SUCCESS);
  assert(Epoch == 0);
  assert(Cursor.Remaining == 0);

  // Parameter and buffer validation.
  assert(CephRbdAuthEncodeInitialRequest (
           NULL, 10, Buffer, sizeof (Buffer), &Length) == EFI_INVALID_PARAMETER);
  assert(CephRbdAuthEncodeInitialRequest (
           (CONST CHAR8 *)"x", 0, Buffer, sizeof (Buffer), &Length) ==
         EFI_INVALID_PARAMETER);
  assert(CephRbdAuthEncodeInitialRequest (
           (CONST CHAR8 *)"x", 1, Buffer, sizeof (Buffer), NULL) ==
         EFI_INVALID_PARAMETER);
  Length = 99;
  assert(CephRbdAuthEncodeInitialRequest (
           (CONST CHAR8 *)"x", 1, Buffer, 8, &Length) == EFI_BAD_BUFFER_SIZE);
  assert(Length == 0);
}

static void
TestDecodeReply (VOID)
{
  UINT8             Buffer[64];
  CEPH_RBD_AUTH_REPLY  Reply;

  memset (Buffer, 0, sizeof (Buffer));
  WriteLe32 (Buffer, CEPH_AUTH_CEPHX);      // protocol
  WriteLe32 (Buffer + 4, 0);                // result
  WriteLe64 (Buffer + 8, 0x1122334455667788ULL);  // global_id
  WriteLe32 (Buffer + 16, 3);               // result_bl length
  Buffer[20] = 0xaa;
  Buffer[21] = 0xbb;
  Buffer[22] = 0xcc;
  WriteLe32 (Buffer + 23, 2);               // result_msg length
  Buffer[27] = 'o';
  Buffer[28] = 'k';

  assert(CephRbdAuthDecodeReply (Buffer, 29, &Reply) == EFI_SUCCESS);
  assert(Reply.Protocol == CEPH_AUTH_CEPHX);
  assert(Reply.Result == 0);
  assert(Reply.GlobalId == 0x1122334455667788ULL);
  assert(Reply.ResultBlLength == 3);
  assert(memcmp (Reply.ResultBl, "\xaa\xbb\xcc", 3) == 0);
  assert(Reply.ResultMsgLength == 2);
  assert(memcmp (Reply.ResultMsg, "ok", 2) == 0);

  // A negative result must decode as a negative INT32.
  WriteLe32 (Buffer + 4, (UINT32)-22);
  assert(CephRbdAuthDecodeReply (Buffer, 29, &Reply) == EFI_SUCCESS);
  assert(Reply.Result == -22);

  // Trailing bytes are rejected.
  assert(CephRbdAuthDecodeReply (Buffer, 30, &Reply) == EFI_COMPROMISED_DATA);

  // Truncated buffers are rejected.
  assert(CephRbdAuthDecodeReply (Buffer, 28, &Reply) == EFI_BAD_BUFFER_SIZE);

  // An oversized result_bl is rejected before skipping.
  WriteLe32 (Buffer + 16, 100);
  assert(CephRbdAuthDecodeReply (Buffer, 29, &Reply) == EFI_BAD_BUFFER_SIZE);

  assert(CephRbdAuthDecodeReply (NULL, 29, &Reply) == EFI_INVALID_PARAMETER);
  assert(CephRbdAuthDecodeReply (Buffer, 29, NULL) == EFI_INVALID_PARAMETER);

  // Empty result_bl and result_msg are valid.
  memset (Buffer, 0, sizeof (Buffer));
  WriteLe32 (Buffer + 16, 0);
  WriteLe32 (Buffer + 20, 0);
  assert(CephRbdAuthDecodeReply (Buffer, 24, &Reply) == EFI_SUCCESS);
  assert(Reply.ResultBlLength == 0 && Reply.ResultBl == NULL);
  assert(Reply.ResultMsgLength == 0 && Reply.ResultMsg == NULL);
}

static void
TestEncodeAuthenticateRequest (VOID)
{
  UINT8             Buffer[256];
  UINTN             Length;
  CEPH_WIRE_CURSOR  Cursor;
  UINT64            Version;
  UINT16            SessionMon;
  UINT64            SessionMonTid;
  UINT32            Protocol;
  UINT32            BlobLength;
  UINT16            RequestType;
  UINT8             AuthStructVersion;
  UINT64            ClientChallenge;
  UINT64            ChallengeResponse;
  UINT8             OldTicketVersion;
  UINT64            OldTicketSecretId;
  UINT32            OldTicketBlobLength;
  UINT32            NeedKeys;
  UINT32            Epoch;

  memset (Buffer, 0, sizeof (Buffer));
  assert(CephRbdAuthEncodeAuthenticateRequest (
           UINT64_C(0x1122334455667788),
           UINT64_C(0x8877665544332211),
           CEPH_RBD_AUTH_NEED_KEYS,
           Buffer, sizeof (Buffer), &Length) == EFI_SUCCESS);

  // AuthPayload: CephXRequestHeader u16(2) + CephXAuthenticate
  //              (u8 + u64 + u64 + u8 + u64 + u32 + u32 = 34) = 36 bytes.
  // MAuth front: u64 + s16 + u64 + u32 + (u32 + 36) + u32 = 66 bytes.
  assert(Length == 66);

  assert(CephWireInit (&Cursor, Buffer, Length) == EFI_SUCCESS);
  assert(CephWireReadLe64 (&Cursor, &Version) == EFI_SUCCESS);
  assert(Version == 0);
  assert(CephWireReadLe16 (&Cursor, &SessionMon) == EFI_SUCCESS);
  assert(SessionMon == (UINT16)0xFFFF);
  assert(CephWireReadLe64 (&Cursor, &SessionMonTid) == EFI_SUCCESS);
  assert(SessionMonTid == 0);
  assert(CephWireReadLe32 (&Cursor, &Protocol) == EFI_SUCCESS);
  assert(Protocol == CEPH_AUTH_CEPHX);
  assert(CephWireReadLe32 (&Cursor, &BlobLength) == EFI_SUCCESS);
  assert(BlobLength == 36);

  assert(CephWireReadLe16 (&Cursor, &RequestType) == EFI_SUCCESS);
  assert(RequestType == CEPHX_GET_AUTH_SESSION_KEY);
  assert(CephWireReadU8 (&Cursor, &AuthStructVersion) == EFI_SUCCESS);
  assert(AuthStructVersion == 3);
  assert(CephWireReadLe64 (&Cursor, &ClientChallenge) == EFI_SUCCESS);
  assert(ClientChallenge == UINT64_C(0x1122334455667788));
  assert(CephWireReadLe64 (&Cursor, &ChallengeResponse) == EFI_SUCCESS);
  assert(ChallengeResponse == UINT64_C(0x8877665544332211));
  assert(CephWireReadU8 (&Cursor, &OldTicketVersion) == EFI_SUCCESS);
  assert(OldTicketVersion == 1);
  assert(CephWireReadLe64 (&Cursor, &OldTicketSecretId) == EFI_SUCCESS);
  assert(OldTicketSecretId == 0);
  assert(CephWireReadLe32 (&Cursor, &OldTicketBlobLength) == EFI_SUCCESS);
  assert(OldTicketBlobLength == 0);
  assert(CephWireReadLe32 (&Cursor, &NeedKeys) == EFI_SUCCESS);
  assert(NeedKeys == CEPH_RBD_AUTH_NEED_KEYS);
  assert(CephWireReadLe32 (&Cursor, &Epoch) == EFI_SUCCESS);
  assert(Epoch == 0);
  assert(Cursor.Remaining == 0);

  assert(CephRbdAuthEncodeAuthenticateRequest (
           1, 2, 3, NULL, sizeof (Buffer), &Length) == EFI_INVALID_PARAMETER);
  Length = 99;
  assert(CephRbdAuthEncodeAuthenticateRequest (
           1, 2, 3, Buffer, 8, &Length) == EFI_BAD_BUFFER_SIZE);
  assert(Length == 0);
}

VOID
TestAuthCodecs (VOID)
{
  TestEncodeInitialRequest ();
  TestDecodeReply ();
  TestEncodeAuthenticateRequest ();
  puts("auth codec tests: passed");
}
