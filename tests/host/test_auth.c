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

static void
TestDecodeAuthSessionKey (VOID)
{
  UINT8  ClientSecret[CEPHX_AES_KEY_SIZE];
  UINT8  ExpectedKey[CEPHX_AES_KEY_SIZE];
  UINT8  TicketPayload[128];
  UINTN  TicketPayloadLength;
  UINT8  Encrypted[256];
  UINTN  EncryptedLength;
  UINT8  ResultBl[512];
  UINT8  WriterBuffer[512];
  UINT8  OutKey[CEPHX_AES_KEY_SIZE];
  UINTN  ResultBlLength;
  UINTN  Offset;
  UINT16 Value16;
  UINTN  Index;

  for (Index = 0; Index < CEPHX_AES_KEY_SIZE; Index++) {
    ClientSecret[Index] = (UINT8)(0x10 + Index);
    ExpectedKey[Index] = (UINT8)(0xa0 + Index);
  }

  // CephXServiceTicket: u8 struct_v=1, CryptoKey (u16 type, utime_t created,
  // u16 len, key bytes), utime_t validity.
  Offset = 0;
  WriterBuffer[Offset++] = 1;                 // CephXServiceTicket struct_v
  Value16 = CEPHX_CRYPTO_AES;
  WriterBuffer[Offset++] = (UINT8)Value16;
  WriterBuffer[Offset++] = (UINT8)(Value16 >> 8);
  WriteLe32 (WriterBuffer + Offset, 1234); Offset += 4;  // created sec
  WriteLe32 (WriterBuffer + Offset, 5678); Offset += 4;  // created nsec
  Value16 = CEPHX_AES_KEY_SIZE;
  WriterBuffer[Offset++] = (UINT8)Value16;
  WriterBuffer[Offset++] = (UINT8)(Value16 >> 8);
  memcpy (WriterBuffer + Offset, ExpectedKey, CEPHX_AES_KEY_SIZE);
  Offset += CEPHX_AES_KEY_SIZE;
  WriteLe32 (WriterBuffer + Offset, 3600); Offset += 4;  // validity sec
  WriteLe32 (WriterBuffer + Offset, 0);    Offset += 4;  // validity nsec
  TicketPayloadLength = Offset;
  memcpy (TicketPayload, WriterBuffer, TicketPayloadLength);

  // Encrypt the CephXServiceTicket with the client's own secret.
  assert (CephXEncryptAes128Payload (
            ClientSecret, CEPHX_AES_KEY_SIZE,
            TicketPayload, TicketPayloadLength,
            Encrypted, sizeof (Encrypted), &EncryptedLength) == EFI_SUCCESS);

  // Assemble result_bl:
  //   u16 request_type, s32 status,
  //   u8 reply_v=1, u32 num_tickets=1,
  //   u32 service_id=AUTH, u8 ticket_v=1,
  //   <encrypted service ticket>,
  //   u8 should_encrypt=0, bufferlist service_ticket_bl (u32 len=0).
  Offset = 0;
  Value16 = CEPHX_GET_AUTH_SESSION_KEY;
  ResultBl[Offset++] = (UINT8)Value16;
  ResultBl[Offset++] = (UINT8)(Value16 >> 8);
  WriteLe32 (ResultBl + Offset, 0); Offset += 4;   // status
  ResultBl[Offset++] = 1;                          // service_ticket_reply_v
  WriteLe32 (ResultBl + Offset, 1); Offset += 4;   // num tickets
  WriteLe32 (ResultBl + Offset, CEPHX_ENTITY_TYPE_AUTH); Offset += 4;
  ResultBl[Offset++] = 1;                          // ticket struct_v
  memcpy (ResultBl + Offset, Encrypted, EncryptedLength);
  Offset += EncryptedLength;
  ResultBl[Offset++] = 0;                          // should_encrypt_ticket
  WriteLe32 (ResultBl + Offset, 0); Offset += 4;   // service_ticket_bl len
  ResultBlLength = Offset;

  memset (OutKey, 0, sizeof (OutKey));
  assert (CephXDecodeAuthSessionKey (
            ResultBl, ResultBlLength,
            ClientSecret, CEPHX_AES_KEY_SIZE,
            OutKey, sizeof (OutKey),
            NULL, NULL, 0, NULL) == EFI_SUCCESS);
  assert (memcmp (OutKey, ExpectedKey, CEPHX_AES_KEY_SIZE) == 0);

  // Wrong secret, tampered ciphertext, truncated buffer and bad args are
  // rejected without yielding a key.
  {
    UINT8 WrongSecret[CEPHX_AES_KEY_SIZE];
    memset (WrongSecret, 0x77, sizeof (WrongSecret));
    assert (CephXDecodeAuthSessionKey (
              ResultBl, ResultBlLength, WrongSecret, CEPHX_AES_KEY_SIZE,
              OutKey, sizeof (OutKey), NULL, NULL, 0, NULL) != EFI_SUCCESS);
  }
  ResultBl[Offset / 2] ^= 0x01;
  assert (CephXDecodeAuthSessionKey (
            ResultBl, ResultBlLength, ClientSecret, CEPHX_AES_KEY_SIZE,
            OutKey, sizeof (OutKey), NULL, NULL, 0, NULL) != EFI_SUCCESS);
  assert (CephXDecodeAuthSessionKey (
            ResultBl, ResultBlLength / 2, ClientSecret, CEPHX_AES_KEY_SIZE,
            OutKey, sizeof (OutKey), NULL, NULL, 0, NULL) != EFI_SUCCESS);
  assert (CephXDecodeAuthSessionKey (
            NULL, ResultBlLength, ClientSecret, CEPHX_AES_KEY_SIZE,
            OutKey, sizeof (OutKey), NULL, NULL, 0, NULL) == EFI_INVALID_PARAMETER);
  assert (CephXDecodeAuthSessionKey (
            ResultBl, ResultBlLength, ClientSecret, CEPHX_AES_KEY_SIZE,
            OutKey, 8, NULL, NULL, 0, NULL) == EFI_INVALID_PARAMETER);

}

static void
TestDecodeAuthTicket (VOID)
{
  UINT8  ClientSecret[CEPHX_AES_KEY_SIZE];
  UINT8  ExpectedKey[CEPHX_AES_KEY_SIZE];
  UINT8  TicketPayload[128];
  UINTN  TicketPayloadLength;
  UINT8  Encrypted[256];
  UINTN  EncryptedLength;
  UINT8  ResultBl[1024];
  UINTN  ResultBlLength;
  UINT8  OutKey[CEPHX_AES_KEY_SIZE];
  UINT8  OutBlob[CEPHX_MAX_TICKET_BLOB];
  UINTN  OutBlobLength;
  UINT64 OutSecretId;
  UINT8  InnerBlob[24];
  UINTN  Offset;
  UINTN  Index;
  UINT16 Value16;

  for (Index = 0; Index < CEPHX_AES_KEY_SIZE; Index++) {
    ClientSecret[Index] = (UINT8)(0x20 + Index);
    ExpectedKey[Index] = (UINT8)(0xb0 + Index);
  }
  for (Index = 0; Index < sizeof (InnerBlob); Index++) {
    InnerBlob[Index] = (UINT8)(0x5a + Index);
  }

  // Encrypted CephXServiceTicket carrying the session key.
  Offset = 0;
  TicketPayload[Offset++] = 1;                 // CephXServiceTicket struct_v
  Value16 = CEPHX_CRYPTO_AES;
  TicketPayload[Offset++] = (UINT8)Value16;
  TicketPayload[Offset++] = (UINT8)(Value16 >> 8);
  WriteLe32 (TicketPayload + Offset, 1); Offset += 4;
  WriteLe32 (TicketPayload + Offset, 0); Offset += 4;
  Value16 = CEPHX_AES_KEY_SIZE;
  TicketPayload[Offset++] = (UINT8)Value16;
  TicketPayload[Offset++] = (UINT8)(Value16 >> 8);
  memcpy (TicketPayload + Offset, ExpectedKey, CEPHX_AES_KEY_SIZE);
  Offset += CEPHX_AES_KEY_SIZE;
  WriteLe32 (TicketPayload + Offset, 3600); Offset += 4;
  WriteLe32 (TicketPayload + Offset, 0); Offset += 4;
  TicketPayloadLength = Offset;

  assert (CephXEncryptAes128Payload (
            ClientSecret, CEPHX_AES_KEY_SIZE,
            TicketPayload, TicketPayloadLength,
            Encrypted, sizeof (Encrypted), &EncryptedLength) == EFI_SUCCESS);

  // result_bl with one AUTH ticket and a plain CephXTicketBlob.
  Offset = 0;
  Value16 = CEPHX_GET_AUTH_SESSION_KEY;
  ResultBl[Offset++] = (UINT8)Value16;
  ResultBl[Offset++] = (UINT8)(Value16 >> 8);
  WriteLe32 (ResultBl + Offset, 0); Offset += 4;   // status
  ResultBl[Offset++] = 1;                          // service_ticket_reply_v
  WriteLe32 (ResultBl + Offset, 1); Offset += 4;   // num tickets
  WriteLe32 (ResultBl + Offset, CEPHX_ENTITY_TYPE_AUTH); Offset += 4;
  ResultBl[Offset++] = 1;                          // ticket struct_v
  memcpy (ResultBl + Offset, Encrypted, EncryptedLength);
  Offset += EncryptedLength;
  ResultBl[Offset++] = 0;                          // should_encrypt_ticket

  // service_ticket_bl bufferlist: CephXTicketBlob = {u8 v, u64 secret_id,
  // bufferlist blob}. Build it into a temporary then wrap with a u32 prefix.
  {
    UINT8  TicketBlob[256];
    UINTN  BlobOffset = 0;

    TicketBlob[BlobOffset++] = 1;                          // struct_v
    WriteLe64 (TicketBlob + BlobOffset, 0x123456789abcdef0ULL);
    BlobOffset += 8;
    WriteLe32 (TicketBlob + BlobOffset, sizeof (InnerBlob));
    BlobOffset += 4;
    memcpy (TicketBlob + BlobOffset, InnerBlob, sizeof (InnerBlob));
    BlobOffset += sizeof (InnerBlob);

    WriteLe32 (ResultBl + Offset, (UINT32)BlobOffset); Offset += 4;
    memcpy (ResultBl + Offset, TicketBlob, BlobOffset);
    Offset += BlobOffset;
  }
  ResultBlLength = Offset;

  memset (OutKey, 0, sizeof (OutKey));
  memset (OutBlob, 0, sizeof (OutBlob));
  OutBlobLength = 0;
  OutSecretId = 0;
  assert (CephXDecodeAuthSessionKey (
            ResultBl, ResultBlLength,
            ClientSecret, CEPHX_AES_KEY_SIZE,
            OutKey, sizeof (OutKey),
            &OutSecretId, OutBlob, sizeof (OutBlob), &OutBlobLength) ==
          EFI_SUCCESS);
  assert (memcmp (OutKey, ExpectedKey, CEPHX_AES_KEY_SIZE) == 0);
  assert (OutSecretId == 0x123456789abcdef0ULL);
  assert (OutBlobLength == sizeof (InnerBlob));
  assert (memcmp (OutBlob, InnerBlob, sizeof (InnerBlob)) == 0);

  // Too-small ticket blob capacity is rejected.
  OutBlobLength = 0;
  assert (CephXDecodeAuthSessionKey (
            ResultBl, ResultBlLength,
            ClientSecret, CEPHX_AES_KEY_SIZE,
            OutKey, sizeof (OutKey),
            &OutSecretId, OutBlob, 4, &OutBlobLength) == EFI_BUFFER_TOO_SMALL);
}

static void
TestServiceTicketCodecs (VOID)
{
  UINT8  PrincipalKey[CEPHX_AES_KEY_SIZE];
  UINT8  ExpectedServiceKey[CEPHX_AES_KEY_SIZE];
  UINT8  OpaqueBlob[32];
  UINT8  ServiceTicketPayload[128];
  UINTN  ServiceTicketPayloadLength;
  UINT8  EncServiceTicket[256];
  UINTN  EncServiceTicketLength;
  UINT8  TicketBlobWire[128];
  UINTN  TicketBlobWireLength;
  UINT8  EncTicketBlob[256];
  UINTN  EncTicketBlobLength;
  UINT8  ResultBl[1024];
  UINTN  ResultBlLength;
  UINT8  OutKey[CEPHX_AES_KEY_SIZE];
  UINT8  OutBlob[CEPHX_MAX_TICKET_BLOB];
  UINTN  OutBlobLength;
  UINT64 OutSecretId;
  UINTN  Index;
  UINTN  Offset;
  UINT16 Value16;

  for (Index = 0; Index < CEPHX_AES_KEY_SIZE; Index++) {
    PrincipalKey[Index] = (UINT8)(0x30 + Index);
    ExpectedServiceKey[Index] = (UINT8)(0xc0 + Index);
  }
  for (Index = 0; Index < sizeof (OpaqueBlob); Index++) {
    OpaqueBlob[Index] = (UINT8)(0xa5 + Index);
  }

  // CephXServiceTicket carrying the OSD service session key.
  Offset = 0;
  ServiceTicketPayload[Offset++] = 1;
  Value16 = CEPHX_CRYPTO_AES;
  ServiceTicketPayload[Offset++] = (UINT8)Value16;
  ServiceTicketPayload[Offset++] = (UINT8)(Value16 >> 8);
  WriteLe32 (ServiceTicketPayload + Offset, 7); Offset += 4;
  WriteLe32 (ServiceTicketPayload + Offset, 0); Offset += 4;
  Value16 = CEPHX_AES_KEY_SIZE;
  ServiceTicketPayload[Offset++] = (UINT8)Value16;
  ServiceTicketPayload[Offset++] = (UINT8)(Value16 >> 8);
  memcpy (ServiceTicketPayload + Offset, ExpectedServiceKey, CEPHX_AES_KEY_SIZE);
  Offset += CEPHX_AES_KEY_SIZE;
  WriteLe32 (ServiceTicketPayload + Offset, 7200); Offset += 4;
  WriteLe32 (ServiceTicketPayload + Offset, 0); Offset += 4;
  ServiceTicketPayloadLength = Offset;

  assert (CephXEncryptAes128Payload (
            PrincipalKey, CEPHX_AES_KEY_SIZE,
            ServiceTicketPayload, ServiceTicketPayloadLength,
            EncServiceTicket, sizeof (EncServiceTicket),
            &EncServiceTicketLength) == EFI_SUCCESS);

  // CephXTicketBlob wire form.
  Offset = 0;
  TicketBlobWire[Offset++] = 1;                                  // struct_v
  WriteLe64 (TicketBlobWire + Offset, 0xfedcba9876543210ULL); Offset += 8;
  WriteLe32 (TicketBlobWire + Offset, sizeof (OpaqueBlob)); Offset += 4;
  memcpy (TicketBlobWire + Offset, OpaqueBlob, sizeof (OpaqueBlob));
  Offset += sizeof (OpaqueBlob);
  TicketBlobWireLength = Offset;

  // should_encrypt_ticket = 1: ticket blob wrapped with encode_encrypt using
  // the principal session key.
  assert (CephXEncryptAes128Payload (
            PrincipalKey, CEPHX_AES_KEY_SIZE,
            TicketBlobWire, TicketBlobWireLength,
            EncTicketBlob, sizeof (EncTicketBlob),
            &EncTicketBlobLength) == EFI_SUCCESS);

  // Assemble GET_PRINCIPAL_SESSION_KEY result_bl with one OSD ticket.
  Offset = 0;
  Value16 = CEPHX_GET_PRINCIPAL_SESSION_KEY;
  ResultBl[Offset++] = (UINT8)Value16;
  ResultBl[Offset++] = (UINT8)(Value16 >> 8);
  WriteLe32 (ResultBl + Offset, 0); Offset += 4;
  ResultBl[Offset++] = 1;                          // reply_v
  WriteLe32 (ResultBl + Offset, 1); Offset += 4;   // num_tickets
  WriteLe32 (ResultBl + Offset, CEPH_ENTITY_TYPE_OSD); Offset += 4;
  ResultBl[Offset++] = 1;                          // ticket_v
  memcpy (ResultBl + Offset, EncServiceTicket, EncServiceTicketLength);
  Offset += EncServiceTicketLength;
  ResultBl[Offset++] = 1;                          // should_encrypt_ticket
  WriteLe32 (ResultBl + Offset, (UINT32)EncTicketBlobLength); Offset += 4;
  memcpy (ResultBl + Offset, EncTicketBlob, EncTicketBlobLength);
  Offset += EncTicketBlobLength;
  ResultBlLength = Offset;

  memset (OutKey, 0, sizeof (OutKey));
  memset (OutBlob, 0, sizeof (OutBlob));
  OutBlobLength = 0;
  OutSecretId = 0;
  assert (CephXDecodeServiceTicket (
            ResultBl, ResultBlLength,
            PrincipalKey, CEPHX_AES_KEY_SIZE,
            CEPH_ENTITY_TYPE_OSD,
            OutKey, sizeof (OutKey),
            &OutSecretId, OutBlob, sizeof (OutBlob), &OutBlobLength) ==
          EFI_SUCCESS);
  assert (memcmp (OutKey, ExpectedServiceKey, CEPHX_AES_KEY_SIZE) == 0);
  assert (OutSecretId == 0xfedcba9876543210ULL);
  assert (OutBlobLength == sizeof (OpaqueBlob));
  assert (memcmp (OutBlob, OpaqueBlob, sizeof (OpaqueBlob)) == 0);

  // Wrong expected service id -> not found.
  assert (CephXDecodeServiceTicket (
            ResultBl, ResultBlLength,
            PrincipalKey, CEPHX_AES_KEY_SIZE,
            CEPH_ENTITY_TYPE_MON,
            OutKey, sizeof (OutKey),
            &OutSecretId, OutBlob, sizeof (OutBlob), &OutBlobLength) ==
          EFI_NOT_FOUND);

  // Wrong principal key is rejected.
  {
    UINT8 WrongKey[CEPHX_AES_KEY_SIZE];
    memset (WrongKey, 0x11, sizeof (WrongKey));
    assert (CephXDecodeServiceTicket (
              ResultBl, ResultBlLength,
              WrongKey, CEPHX_AES_KEY_SIZE,
              CEPH_ENTITY_TYPE_OSD,
              OutKey, sizeof (OutKey),
              &OutSecretId, OutBlob, sizeof (OutBlob), &OutBlobLength) !=
            EFI_SUCCESS);
  }

  // Build a service authorizer and verify its layout.
  {
    UINT8  Authorizer[512];
    UINTN  AuthorizerLength;
    CEPH_WIRE_CURSOR  Cursor;
    UINT8  Version;
    UINT64 GlobalId;
    UINT32 ServiceId;
    UINT64 SecretId;
    UINT32 BlobLength;
    UINT8  Blob[CEPHX_MAX_TICKET_BLOB];
    UINT32 EncLength;

    assert (CephXEncodeServiceAuthorizer (
              0x1122334455667788ULL, CEPH_ENTITY_TYPE_OSD,
              OutSecretId, OutBlob, OutBlobLength,
              ExpectedServiceKey, CEPHX_AES_KEY_SIZE,
              0x99ULL,
              Authorizer, sizeof (Authorizer), &AuthorizerLength) ==
            EFI_SUCCESS);
    assert (CephWireInit (&Cursor, Authorizer, AuthorizerLength) == EFI_SUCCESS);
    assert (CephWireReadU8 (&Cursor, &Version) == EFI_SUCCESS);
    assert (Version == 1);
    assert (CephWireReadLe64 (&Cursor, &GlobalId) == EFI_SUCCESS);
    assert (GlobalId == 0x1122334455667788ULL);
    assert (CephWireReadLe32 (&Cursor, &ServiceId) == EFI_SUCCESS);
    assert (ServiceId == CEPH_ENTITY_TYPE_OSD);
    assert (CephWireReadU8 (&Cursor, &Version) == EFI_SUCCESS);
    assert (Version == 1);
    assert (CephWireReadLe64 (&Cursor, &SecretId) == EFI_SUCCESS);
    assert (SecretId == OutSecretId);
    assert (CephWireReadLe32 (&Cursor, &BlobLength) == EFI_SUCCESS);
    assert (BlobLength == OutBlobLength);
    assert (CephWireReadBytes (&Cursor, Blob, BlobLength) == EFI_SUCCESS);
    assert (memcmp (Blob, OutBlob, OutBlobLength) == 0);
    assert (CephWireReadLe32 (&Cursor, &EncLength) == EFI_SUCCESS);
    assert (EncLength == Cursor.Remaining);
  }
}

VOID
TestAuthCodecs (VOID)
{
  TestEncodeInitialRequest ();
  TestDecodeReply ();
  TestEncodeAuthenticateRequest ();
  TestDecodeAuthSessionKey ();
  TestDecodeAuthTicket ();
  TestServiceTicketCodecs ();
  puts("auth codec tests: passed");
}
