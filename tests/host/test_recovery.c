#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "CephRbdCephX.h"
#include "CephRbdMessenger.h"
#include "CephRbdMsgrV1Recovery.h"
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
TestCephXRoundTrip (VOID)
{
  static CONST UINT8 Key[CEPHX_AES_KEY_SIZE] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
  };
  CONST UINT8 Payload[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  UINT8 Encoded[128];
  UINT8 Decoded[128];
  UINTN EncodedLength;
  UINTN DecodedLength;
  UINT64 ServerChallenge;
  UINT64 Response;
  UINT8 Corrupt[128];

  assert(CephXEncryptAes128Payload(
           Key, sizeof (Key), Payload, sizeof (Payload),
           Encoded, sizeof (Encoded), &EncodedLength) == EFI_SUCCESS);
  assert(CephXDecryptAes128Payload(
           Key, sizeof (Key), Encoded, EncodedLength,
           Decoded, sizeof (Decoded), &DecodedLength) == EFI_SUCCESS);
  assert(DecodedLength == sizeof (Payload));
  assert(memcmp (Decoded, Payload, sizeof (Payload)) == 0);

  // Wrong key must fail the AUTH_ENC_MAGIC check.
  {
    UINT8 WrongKey[CEPHX_AES_KEY_SIZE];
    memset (WrongKey, 0xff, sizeof (WrongKey));
    assert(CephXDecryptAes128Payload(
             WrongKey, sizeof (WrongKey), Encoded, EncodedLength,
             Decoded, sizeof (Decoded), &DecodedLength) == EFI_COMPROMISED_DATA);
  }

  // A single flipped ciphertext byte is detected by padding/magic validation.
  memcpy (Corrupt, Encoded, EncodedLength);
  Corrupt[EncodedLength - 1] ^= 0x01;
  assert(CephXDecryptAes128Payload(
           Key, sizeof (Key), Corrupt, EncodedLength,
           Decoded, sizeof (Decoded), &DecodedLength) != EFI_SUCCESS);

  assert(CephXComputeAesChallengeResponse(
           Key, sizeof (Key), 0x0102030405060708ULL,
           0x1122334455667788ULL, &Response) == EFI_SUCCESS);

  // Encrypted authorize challenge: struct_v=1 + server_challenge (u64).
  {
    UINT8 Challenge[9];
    UINT8 ChallengeEncoded[128];

    Challenge[0] = 1;
    WriteLe64 (Challenge + 1, 0x0102030405060708ULL);
    assert(CephXEncryptAes128Payload(
             Key, sizeof (Key), Challenge, sizeof (Challenge),
             ChallengeEncoded, sizeof (ChallengeEncoded),
             &EncodedLength) == EFI_SUCCESS);
    assert(CephXDecodeAuthorizeChallengeSecret(
             Key, sizeof (Key), ChallengeEncoded, EncodedLength,
             &ServerChallenge) == EFI_SUCCESS);
    assert(ServerChallenge == 0x0102030405060708ULL);
  }
}

static void
BuildTicket (
  OUT UINT8     *Blob,
  IN UINTN       BlobCapacity,
  IN CONST UINT8 *ServiceKey,
  IN CONST UINT8 *SessionKey,
  IN UINT64      GlobalId,
  OUT UINTN      *BlobLength
  )
{
  UINT8 TicketInfo[256];
  UINT8 Encrypted[256];
  UINTN InfoLength;
  UINTN EncryptedLength;
  CEPH_WIRE_WRITER Writer;
  UINTN NameLength;
  UINTN CapsLength;

  NameLength = strlen ("client.ceph-rbd");
  CapsLength = 0;
  assert(CephWireWriterInit (&Writer, TicketInfo, sizeof (TicketInfo)) ==
         EFI_SUCCESS);
  assert(CephWireWriteU8 (&Writer, 1) == EFI_SUCCESS);          // info v1
  assert(CephWireWriteU8 (&Writer, 2) == EFI_SUCCESS);          // AuthTicket v2
  assert(CephWireWriteU8 (&Writer, CEPH_MSGR_CONNECT_HOST_CLIENT) ==
         EFI_SUCCESS);                                          // entity type
  assert(CephWireWriteLe32 (&Writer, (UINT32)NameLength) ==
         EFI_SUCCESS);
  assert(CephWireWriteBytes (&Writer, "client.ceph-rbd", NameLength) ==
         EFI_SUCCESS);
  assert(CephWireWriteLe64 (&Writer, GlobalId) == EFI_SUCCESS);
  assert(CephWireWriteLe64 (&Writer, CEPHX_AUTH_UID_DEFAULT) ==
         EFI_SUCCESS);
  assert(CephWireWriteLe32 (&Writer, 1000) == EFI_SUCCESS);     // created sec
  assert(CephWireWriteLe32 (&Writer, 0) == EFI_SUCCESS);        // created nsec
  assert(CephWireWriteLe32 (&Writer, 9999) == EFI_SUCCESS);     // expires sec
  assert(CephWireWriteLe32 (&Writer, 0) == EFI_SUCCESS);        // expires nsec
  assert(CephWireWriteU8 (&Writer, 1) == EFI_SUCCESS);          // caps v1
  assert(CephWireWriteU8 (&Writer, 0) == EFI_SUCCESS);          // allow_all
  assert(CephWireWriteLe32 (&Writer, (UINT32)CapsLength) ==
         EFI_SUCCESS);
  assert(CephWireWriteLe32 (&Writer, 0) == EFI_SUCCESS);        // flags
  assert(CephWireWriteLe16 (&Writer, CEPHX_CRYPTO_AES) ==
         EFI_SUCCESS);                                          // key type
  assert(CephWireWriteLe32 (&Writer, 1000) == EFI_SUCCESS);     // key created
  assert(CephWireWriteLe32 (&Writer, 0) == EFI_SUCCESS);
  assert(CephWireWriteLe16 (&Writer, CEPHX_AES_KEY_SIZE) ==
         EFI_SUCCESS);
  assert(CephWireWriteBytes (&Writer, SessionKey, CEPHX_AES_KEY_SIZE) ==
         EFI_SUCCESS);
  InfoLength = Writer.Written;

  assert(CephXEncryptAes128Payload(
           ServiceKey, CEPHX_AES_KEY_SIZE, TicketInfo, InfoLength,
           Encrypted, sizeof (Encrypted), &EncryptedLength) == EFI_SUCCESS);

  assert(CephWireWriterInit (&Writer, Blob, BlobCapacity) == EFI_SUCCESS);
  assert(CephWireWriteU8 (&Writer, 1) == EFI_SUCCESS);          // ticket v1
  assert(CephWireWriteLe64 (&Writer, 42) == EFI_SUCCESS);       // secret id
  assert(CephWireWriteLe32 (&Writer, (UINT32)EncryptedLength) ==
         EFI_SUCCESS);
  assert(CephWireWriteBytes (&Writer, Encrypted, EncryptedLength) ==
         EFI_SUCCESS);
  *BlobLength = Writer.Written;
}

static void
TestTicketValidation (VOID)
{
  static CONST UINT8 ServiceKey[CEPHX_AES_KEY_SIZE] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
  };
  static CONST UINT8 SessionKey[CEPHX_AES_KEY_SIZE] = {
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f
  };
  UINT8 Blob[512];
  UINTN BlobLength;
  CEPHX_TICKET_BLOB Ticket;
  CEPHX_SERVICE_TICKET_INFO Info;
  UINT8 Reply[128];
  UINT8 Challenge[9];
  UINTN EncodedLength;
  UINTN SecretLength;
  UINT8 Secret[64];

  BuildTicket (Blob, sizeof (Blob), ServiceKey, SessionKey, 7, &BlobLength);
  assert(CephXDecodeTicketBlob (Blob, BlobLength, &Ticket) == EFI_SUCCESS);
  assert(Ticket.SecretId == 42);
  assert(Ticket.BlobLength > 0);

  assert(CephXDecodeAndValidateTicket(
           &Ticket, ServiceKey, sizeof (ServiceKey), 7, 5000,
           &Info) == EFI_SUCCESS);
  assert(Info.GlobalId == 7);
  assert(Info.SecretId == 42);
  assert(Info.SessionKey.Type == CEPHX_CRYPTO_AES);
  assert(memcmp (Info.SessionKey.Key, SessionKey,
                 CEPHX_AES_KEY_SIZE) == 0);
  assert(Info.Auid == CEPHX_AUTH_UID_DEFAULT);

  // Wrong global id and expired ticket must be rejected.
  assert(CephXDecodeAndValidateTicket(
           &Ticket, ServiceKey, sizeof (ServiceKey), 8, 5000,
           &Info) == EFI_ACCESS_DENIED);
  assert(CephXDecodeAndValidateTicket(
           &Ticket, ServiceKey, sizeof (ServiceKey), 7, 100000,
           &Info) == EFI_ACCESS_DENIED);
  assert(CephXDecodeAndValidateTicket(
           &Ticket, ServiceKey, sizeof (ServiceKey) - 1, 7, 5000,
           &Info) == EFI_UNSUPPORTED);

  // Authorizer reply: struct_v=1 + nonce + 1.
  Challenge[0] = 1;
  WriteLe64 (Challenge + 1, 0x1122334455667788ULL + 1);
  assert(CephXEncryptAes128Payload(
           SessionKey, sizeof (SessionKey), Challenge, sizeof (Challenge),
           Reply, sizeof (Reply), &EncodedLength) == EFI_SUCCESS);
  assert(CephXVerifyAuthorizeReply(
           SessionKey, sizeof (SessionKey), Reply, EncodedLength,
           0x1122334455667788ULL, NULL, 0, NULL) == EFI_SUCCESS);
  assert(CephXVerifyAuthorizeReply(
           SessionKey, sizeof (SessionKey), Reply, EncodedLength,
           0x1122334455667787ULL, NULL, 0, NULL) == EFI_COMPROMISED_DATA);

  // Authorizer reply v2 with connection secret.
  {
    CEPH_WIRE_WRITER Writer;
    UINT8 Plain[64];

    assert(CephWireWriterInit (&Writer, Plain, sizeof (Plain)) ==
           EFI_SUCCESS);
    assert(CephWireWriteU8 (&Writer, 2) == EFI_SUCCESS);
    assert(CephWireWriteLe64 (&Writer, 9) == EFI_SUCCESS);
    assert(CephWireWriteLe32 (&Writer, 3) == EFI_SUCCESS);
    assert(CephWireWriteBytes (&Writer, "key", 3) == EFI_SUCCESS);
    assert(CephXEncryptAes128Payload(
             SessionKey, sizeof (SessionKey), Plain, Writer.Written,
             Reply, sizeof (Reply), &EncodedLength) == EFI_SUCCESS);
    memset (Secret, 0, sizeof (Secret));
    assert(CephXVerifyAuthorizeReply(
             SessionKey, sizeof (SessionKey), Reply, EncodedLength,
             8, Secret, sizeof (Secret), &SecretLength) == EFI_SUCCESS);
    assert(SecretLength == 3);
    assert(memcmp (Secret, "key", 3) == 0);
    assert(CephXVerifyAuthorizeReply(
             SessionKey, sizeof (SessionKey), Reply, EncodedLength,
             8, Secret, 2, &SecretLength) == EFI_BUFFER_TOO_SMALL);
  }
}

static EFI_STATUS
EFIAPI
CountFrameCallback (
  IN VOID         *Context,
  IN CONST VOID   *Frame,
  IN UINTN        FrameLength,
  IN UINT64       Sequence
  )
{
  UINTN *Count;

  (VOID)Frame;
  (VOID)FrameLength;
  (VOID)Sequence;
  Count = Context;
  (*Count)++;
  return EFI_SUCCESS;
}

static void
TestRetransmitQueue (VOID)
{
  CEPH_MSGR_V1_RETRANSMIT_QUEUE Queue;
  UINT8 Frame[32];
  UINTN Count;
  EFI_STATUS Status;
  UINTN Index;

  memset (Frame, 0x5a, sizeof (Frame));
  CephMsgrV1RetransmitQueueInit (&Queue, 2);
  assert(Queue.MaxRetries == 2);

  for (Index = 0; Index < CEPH_MSGR_V1_RETRANSMIT_MAX; Index++) {
    Status = CephMsgrV1RetransmitQueueTrack (
               &Queue, Index + 1, Frame, sizeof (Frame));
    assert(Status == EFI_SUCCESS);
  }

  assert(CephMsgrV1RetransmitQueueTrack (
           &Queue, 100, Frame, sizeof (Frame)) == EFI_OUT_OF_RESOURCES);
  assert(CephMsgrV1RetransmitQueuePendingCount (&Queue, 0) ==
         CEPH_MSGR_V1_RETRANSMIT_MAX);

  Count = 0;
  assert(CephMsgrV1RetransmitQueueForEachPending (
           &Queue, 0, &Count, CountFrameCallback) == EFI_SUCCESS);
  assert(Count == CEPH_MSGR_V1_RETRANSMIT_MAX);

  assert(CephMsgrV1RetransmitQueueExhausted (&Queue, 0) == FALSE);
  Count = 0;
  assert(CephMsgrV1RetransmitQueueForEachPending (
           &Queue, 0, &Count, CountFrameCallback) == EFI_SUCCESS);
  Count = 0;
  assert(CephMsgrV1RetransmitQueueForEachPending (
           &Queue, 0, &Count, CountFrameCallback) == EFI_TIMEOUT);
  assert(CephMsgrV1RetransmitQueueExhausted (&Queue, 0) == TRUE);

  assert(CephMsgrV1RetransmitQueueAcknowledge (&Queue, 5, &Count) ==
         EFI_SUCCESS);
  assert(Count == CEPH_MSGR_V1_RETRANSMIT_MAX - 5);
  // Slots freed by ACKs are reusable; the high-water mark stays at 16 until
  // trailing inactive entries are trimmed.
  assert(Queue.Count == CEPH_MSGR_V1_RETRANSMIT_MAX);
  assert(CephMsgrV1RetransmitQueueTrack (
           &Queue, 100, Frame, sizeof (Frame)) == EFI_SUCCESS);
}

VOID
TestCephXAndRecovery (VOID)
{
  TestCephXRoundTrip ();
  TestTicketValidation ();
  TestRetransmitQueue ();
  puts("CephX/recovery tests: passed");
}
