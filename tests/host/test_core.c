#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "CephRbdCephX.h"
#include "CephRbdFormat2.h"
#include "CephRbdMessenger.h"
#include "CephRbdWire.h"
#include "ceph_stage2.h"

void TestTcp4AndMessengerHandshake(void);
void TestMessengerFrameTransport(void);
void TestMessengerSession(void);
void TestCephXAndRecovery(void);
void TestRpcCodecs(void);
void TestBlockIo(void);
void TestConfigAndDriverBinding(void);
void TestAuthCodecs(void);
void TestCrushPlacement(void);
void TestPlacement(void);
void TestGpt(void);
void TestAes256Krb5(void);
void TestNetwork(void);
void TestSecretResolver(void);

static void
WriteLe32(uint8_t *Buffer, uint32_t Value)
{
  Buffer[0] = (uint8_t)Value;
  Buffer[1] = (uint8_t)(Value >> 8);
  Buffer[2] = (uint8_t)(Value >> 16);
  Buffer[3] = (uint8_t)(Value >> 24);
}

static void
WriteLe64(uint8_t *Buffer, uint64_t Value)
{
  WriteLe32(Buffer, (uint32_t)Value);
  WriteLe32(Buffer + 4, (uint32_t)(Value >> 32));
}

static void
TestWireAndCephX(void)
{
  const uint8_t challenge_wire[] = { 1, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
  const uint8_t authorize_challenge_wire[] = { 1, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
  const uint8_t authorize_reply_wire[] = { 2, 0x09, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 'k', 'e', 'y' };
  const uint8_t ticket[] = { 0xaa, 0xbb, 0xcc };
  uint8_t encoded[64];
  uint8_t connection_secret[8];
  CEPHX_SERVER_CHALLENGE challenge;
  CEPHX_AUTHORIZE_CHALLENGE authorize_challenge;
  CEPHX_AUTHORIZE_REPLY authorize_reply;
  CEPHX_RESPONSE_HEADER response;
  UINTN encoded_length = 0;
  const uint8_t response_wire[] = { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };

  assert(CephXDecodeServerChallenge(
           challenge_wire, sizeof(challenge_wire), &challenge) == EFI_SUCCESS);
  assert(challenge.ServerChallenge == UINT64_C(0x0102030405060708));
  assert(CephXDecodeServerChallenge(
           challenge_wire, sizeof(challenge_wire) - 1, &challenge) == EFI_BAD_BUFFER_SIZE);
  assert(CephXDecodeResponseHeader(
           response_wire, sizeof(response_wire), &response) == EFI_SUCCESS);
  assert(response.RequestType == CEPHX_REQUEST_GET_AUTH_SESSION_KEY);
  assert(response.Status == 0);

  assert(CephXEncodeRequestHeader(
           CEPHX_REQUEST_GET_AUTH_SESSION_KEY, encoded, sizeof(encoded),
           &encoded_length) == EFI_SUCCESS);
  assert(encoded_length == 2 && encoded[0] == 0 && encoded[1] == 1);
  assert(CephXEncodeRequestHeader(
           0x0300, encoded, sizeof(encoded), &encoded_length) == EFI_INVALID_PARAMETER);
  assert(CephXEncodeServiceTicketRequest(
           0x0c, encoded, sizeof(encoded), &encoded_length) == EFI_SUCCESS);
  assert(encoded_length == 5 && encoded[0] == 1 && encoded[1] == 0x0c);

  assert(CephXDecodeAuthorizeChallenge(
           authorize_challenge_wire, sizeof(authorize_challenge_wire),
           &authorize_challenge) == EFI_SUCCESS);
  assert(authorize_challenge.ServerChallenge == UINT64_C(0x0102030405060708));
  assert(CephXEncodeAuthorize(
           0x11, TRUE, authorize_challenge.ServerChallenge + 1,
           encoded, sizeof(encoded), &encoded_length) == EFI_SUCCESS);
  assert(encoded_length == 18 && encoded[0] == 2 && encoded[9] == 1);
  assert(encoded[10] == 9 && encoded[17] == 1);

  assert(CephXDecodeAuthorizeReply(
           authorize_reply_wire, sizeof(authorize_reply_wire),
           &authorize_reply, connection_secret, sizeof(connection_secret)) == EFI_SUCCESS);
  assert(authorize_reply.NoncePlusOne == 9);
  assert(authorize_reply.ConnectionSecretLength == 3);
  assert(memcmp(connection_secret, "key", 3) == 0);
  assert(CephXDecodeAuthorizeReply(
           authorize_reply_wire, sizeof(authorize_reply_wire),
           &authorize_reply, connection_secret, 2) == EFI_BUFFER_TOO_SMALL);

  assert(CephXEncodeAuthenticate(
           0x1122334455667788ULL,
           0x8877665544332211ULL,
           7,
           ticket,
           sizeof(ticket),
           5,
           encoded,
           sizeof(encoded),
           &encoded_length) == EFI_SUCCESS);
  assert(encoded_length == 37);
  assert(encoded[0] == 3 && encoded[1] == 0x88 && encoded[8] == 0x11);
  assert(CephXEncodeAuthenticate(
           1, 2, 0, ticket, sizeof(ticket), 0,
           encoded, 8, &encoded_length) == EFI_BUFFER_TOO_SMALL);
  assert(encoded_length == 0);
}

static void
TestCephXAes(void)
{
  static const UINT8 key[CEPHX_AES_KEY_SIZE] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
  };
  // CephXChallengeBlob encodes only two u64 challenges (no struct_v).
  static const UINT8 challenge[16] = {
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11
  };
  static const UINT8 expected_encrypted[] = {
    0x20, 0x00, 0x00, 0x00,
    0x7c, 0x19, 0xa6, 0x41, 0xa7, 0x49, 0xb8, 0x61,
    0x01, 0xb6, 0xfa, 0xf3, 0x20, 0x8e, 0xbe, 0x40,
    0x86, 0x5d, 0x83, 0x31, 0x1b, 0xe0, 0x71, 0x13,
    0xb5, 0x1d, 0x4b, 0x5c, 0x66, 0xfd, 0xc7, 0x8c
  };
  UINT8 encoded[64];
  UINTN encoded_length = 0;
  UINT64 response = 0;

  assert(CephXEncryptAes128Payload(
           key, sizeof (key), challenge, sizeof (challenge),
           encoded, sizeof (encoded), &encoded_length) == EFI_SUCCESS);
  assert(encoded_length == sizeof (expected_encrypted));
  assert(memcmp (encoded, expected_encrypted, sizeof (expected_encrypted)) == 0);
  assert(CephXComputeAesChallengeResponse(
           key, sizeof (key),
           UINT64_C(0x0102030405060708),
           UINT64_C(0x1122334455667788),
           &response) == EFI_SUCCESS);
  assert(response == UINT64_C(0xdf94ef4e327727bc));

  encoded_length = 99;
  assert(CephXEncryptAes128Payload(
           key, sizeof (key), challenge, sizeof (challenge),
           encoded, 35, &encoded_length) == EFI_BUFFER_TOO_SMALL);
  assert(encoded_length == 0);
  assert(CephXComputeAesChallengeResponse(
           key, sizeof (key) - 1, 1, 2, &response) == EFI_UNSUPPORTED);
}

static void
TestMessenger(void)
{
  static const uint8_t crc_vector[] = "123456789";
  static const uint8_t expected_frame[] = {
    0x07, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
    0x2a, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x3f, 0x7c, 0x5f, 0xce,
    0x6f, 0x6b, 0x6d, 0x78, 0x79, 0x7a,
    0xa2, 0x17, 0xaa, 0x9f, 0xb1, 0xdf, 0xee, 0xde,
    0xff, 0xcb, 0x47, 0x45, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01
  };
  uint8_t wire[1 + CEPH_MSGR_V1_HEADER_SIZE + CEPH_MSGR_V1_FOOTER_SIZE] = { 0 };
  uint8_t payload_wire[1 + CEPH_MSGR_V1_HEADER_SIZE + 2 + CEPH_MSGR_V1_FOOTER_SIZE] = { 0 };
  uint8_t *header = wire + 1;
  uint8_t *footer = header + CEPH_MSGR_V1_HEADER_SIZE;
  uint8_t *payload_header = payload_wire + 1;
  uint8_t *payload = payload_header + CEPH_MSGR_V1_HEADER_SIZE;
  uint8_t *payload_footer = payload + 2;
  uint8_t encoded_frame[1 + CEPH_MSGR_V1_HEADER_SIZE + 6 +
                        CEPH_MSGR_V1_FOOTER_SIZE];
  CEPH_MSGR_V1_FRAME frame;
  CEPH_MSGR_V1_FRAME outbound = { 0 };
  UINTN encoded_length = 0;

  wire[0] = CEPH_MSGR_V1_TAG_MESSAGE;
  WriteLe32(header + 49, CephMsgrCrc32c(header, 49));
  WriteLe32(footer, CephMsgrCrc32c(NULL, 0));
  WriteLe32(footer + 4, CephMsgrCrc32c(NULL, 0));
  WriteLe32(footer + 8, CephMsgrCrc32c(NULL, 0));
  footer[20] = CEPH_MSGR_V1_FOOTER_COMPLETE;

  assert(CephMsgrCrc32c(crc_vector, sizeof(crc_vector) - 1) == 0x58e3fa20U);
  assert(CephMsgrDecodeV1Frame(
           wire, sizeof(wire), 1024, &frame) == EFI_SUCCESS);
  assert(frame.FrontLength == 0 && frame.DataLength == 0);
  assert(CephMsgrDecodeV1Frame(
           wire, sizeof(wire) - 1, 1024, &frame) == EFI_BAD_BUFFER_SIZE);

  header[0] ^= 1;
  assert(CephMsgrDecodeV1Frame(
           wire, sizeof(wire), 1024, &frame) == EFI_CRC_ERROR);

  payload_wire[0] = CEPH_MSGR_V1_TAG_MESSAGE;
  WriteLe32(payload_header + 22, 1);
  WriteLe32(payload_header + 30, 1);
  payload[0] = 0x41;
  payload[1] = 0x42;
  WriteLe32(payload_header + 49, CephMsgrCrc32c(payload_header, 49));
  WriteLe32(payload_footer, CephMsgrCrc32c(payload, 1));
  WriteLe32(payload_footer + 4, CephMsgrCrc32c(NULL, 0));
  WriteLe32(payload_footer + 8, 0xdeadbeefU);
  payload_footer[20] = CEPH_MSGR_V1_FOOTER_COMPLETE | CEPH_MSGR_V1_FOOTER_NOCRC;
  assert(CephMsgrDecodeV1Frame(
           payload_wire, sizeof(payload_wire), 1024, &frame) == EFI_SUCCESS);
  payload_footer[20] = CEPH_MSGR_V1_FOOTER_COMPLETE;
  assert(CephMsgrDecodeV1Frame(
           payload_wire, sizeof(payload_wire), 1024, &frame) == EFI_CRC_ERROR);

  outbound.Sequence = 0x0102030405060708ULL;
  outbound.TransactionId = 0x1122334455667788ULL;
  outbound.Type = 42;
  outbound.Priority = 10;
  outbound.Version = 1;
  outbound.SourceType = CEPH_MSGR_CONNECT_HOST_CLIENT;
  outbound.SourceNumber = 7;
  outbound.CompatVersion = 1;
  outbound.FrontLength = 2;
  outbound.MiddleLength = 1;
  outbound.DataLength = 3;
  outbound.Front = (const uint8_t *)"ok";
  outbound.Middle = (const uint8_t *)"m";
  outbound.Data = (const uint8_t *)"xyz";
  assert(CephMsgrEncodeV1Frame(
           &outbound, 6, encoded_frame, sizeof(encoded_frame),
           &encoded_length) == EFI_SUCCESS);
  assert(encoded_length == sizeof(encoded_frame));
  assert(sizeof(expected_frame) == encoded_length);
  assert(memcmp(encoded_frame, expected_frame, sizeof(expected_frame)) == 0);
  assert(CephMsgrDecodeV1Frame(
           encoded_frame, encoded_length, 6, &frame) == EFI_SUCCESS);
  assert(frame.Sequence == outbound.Sequence);
  assert(frame.TransactionId == outbound.TransactionId);
  assert(frame.Type == outbound.Type && frame.Priority == outbound.Priority);
  assert(frame.Version == outbound.Version);
  assert(frame.SourceType == outbound.SourceType);
  assert(frame.SourceNumber == outbound.SourceNumber);
  assert(frame.CompatVersion == outbound.CompatVersion);
  assert(frame.FrontLength == 2 && memcmp(frame.Front, "ok", 2) == 0);
  assert(frame.MiddleLength == 1 && memcmp(frame.Middle, "m", 1) == 0);
  assert(frame.DataLength == 3 && memcmp(frame.Data, "xyz", 3) == 0);
  assert(CephMsgrEncodeV1Frame(
           &outbound, 5, encoded_frame, sizeof(encoded_frame),
           &encoded_length) == EFI_BAD_BUFFER_SIZE);
  assert(encoded_length == 0);
  assert(CephMsgrEncodeV1Frame(
           &outbound, 6, encoded_frame, sizeof(encoded_frame) - 1,
           &encoded_length) == EFI_BUFFER_TOO_SMALL);
  outbound.FooterFlags = CEPH_MSGR_V1_FOOTER_NOCRC;
  assert(CephMsgrEncodeV1Frame(
           &outbound, 6, encoded_frame, sizeof(encoded_frame),
           &encoded_length) == EFI_SUCCESS);
  assert(CephMsgrDecodeV1Frame(
           encoded_frame, encoded_length, 6, &frame) == EFI_SUCCESS);
  assert((frame.FooterFlags & CEPH_MSGR_V1_FOOTER_NOCRC) != 0);
  encoded_frame[1 + 48] = 1;
  WriteLe32 (
    encoded_frame + 1 + CEPH_MSGR_V1_HEADER_CRC_OFFSET,
    CephMsgrCrc32c (
      encoded_frame + 1, CEPH_MSGR_V1_HEADER_CRC_OFFSET));
  assert(CephMsgrDecodeV1Frame(
           encoded_frame, encoded_length, 6, &frame) == EFI_COMPROMISED_DATA);
  outbound.FooterFlags = CEPH_MSGR_V1_FOOTER_SIGNED;
  assert(CephMsgrEncodeV1Frame(
           &outbound, 6, encoded_frame, sizeof(encoded_frame),
           &encoded_length) == EFI_UNSUPPORTED);
  assert(encoded_length == 0);

  // ceph_msg_footer_old (13 bytes, flags at index 12) is used when MSG_AUTH
  // is not negotiated. No u64 signature is emitted in that form.
  outbound.FooterFlags = CEPH_MSGR_V1_FOOTER_LEGACY;
  assert(CephMsgrEncodeV1Frame(
           &outbound, 6, encoded_frame, sizeof(encoded_frame),
           &encoded_length) == EFI_SUCCESS);
  assert(encoded_length ==
         (1 + CEPH_MSGR_V1_HEADER_SIZE + 6 +
          CEPH_MSGR_V1_FOOTER_LEGACY_SIZE));
  assert((encoded_frame[1 + CEPH_MSGR_V1_HEADER_SIZE + 6 + 12] &
          CEPH_MSGR_V1_FOOTER_COMPLETE) != 0);
  assert(CephMsgrDecodeV1Frame(
           encoded_frame, encoded_length, 6, &frame) == EFI_SUCCESS);
  assert(frame.FrontLength == 2 && memcmp(frame.Front, "ok", 2) == 0);
  assert(frame.DataLength == 3 && memcmp(frame.Data, "xyz", 3) == 0);
  assert((frame.FooterFlags & CEPH_MSGR_V1_FOOTER_COMPLETE) != 0);
}

static void
TestMessengerConnect(void)
{
  const uint8_t authorizer[] = { 0xa1, 0xb2 };
  uint8_t connect[CEPH_MSGR_V1_CONNECT_SIZE + sizeof(authorizer)];
  uint8_t reply_wire[CEPH_MSGR_V1_CONNECT_REPLY_SIZE] = { 0 };
  UINTN encoded_length = 0;
  CEPH_MSGR_V1_CONNECT_REPLY reply;
  CEPH_MSGR_CONNECT_ACTION action;

  assert(CephMsgrEncodeV1Connect(
           0x1122334455667788ULL,
           CEPH_MSGR_CONNECT_HOST_CLIENT,
           0x01020304,
           7,
           2,
           2,
           authorizer,
           sizeof(authorizer),
           0,
           connect,
           sizeof(connect),
           &encoded_length) == EFI_SUCCESS);
  assert(encoded_length == sizeof(connect));
  assert(connect[0] == 0x88 && connect[7] == 0x11);
  assert(connect[8] == CEPH_MSGR_CONNECT_HOST_CLIENT);
  assert(connect[28] == sizeof(authorizer));
  assert(connect[CEPH_MSGR_V1_CONNECT_SIZE] == 0xa1);
  assert(CephMsgrEncodeV1Connect(
           0, CEPH_MSGR_CONNECT_HOST_CLIENT, 1, 1, 1, 0,
           authorizer, sizeof(authorizer), 0,
           connect, CEPH_MSGR_V1_CONNECT_SIZE, &encoded_length) == EFI_BUFFER_TOO_SMALL);

  reply_wire[0] = CEPH_MSGR_TAG_READY;
  WriteLe32(reply_wire + 17, 2);
  WriteLe32(reply_wire + 13, 8);
  assert(CephMsgrDecodeV1ConnectReply(
           reply_wire, sizeof(reply_wire), &reply) == EFI_SUCCESS);
  assert(CephMsgrClassifyV1ConnectReply(
           &reply, 0, 2, 7, &action) == EFI_SUCCESS);
  assert(action == CephMsgrConnectReady);

  WriteLe32(reply_wire + 13, 9);
  assert(CephMsgrDecodeV1ConnectReply(
           reply_wire, sizeof(reply_wire), &reply) == EFI_SUCCESS);
  assert(CephMsgrClassifyV1ConnectReply(
           &reply, 0, 2, 7, &action) == EFI_UNSUPPORTED);
  assert(action == CephMsgrConnectRejected);

  reply_wire[0] = CEPH_MSGR_TAG_READY;
  WriteLe32(reply_wire + 13, 8);
  WriteLe64(reply_wire + 1, 0x1);
  assert(CephMsgrDecodeV1ConnectReply(
           reply_wire, sizeof(reply_wire), &reply) == EFI_SUCCESS);
  assert(CephMsgrClassifyV1ConnectReply(
           &reply, 0x3, 2, 7, &action) == EFI_UNSUPPORTED);
  assert(action == CephMsgrConnectRejected);

  reply_wire[0] = CEPH_MSGR_TAG_RETRY_SESSION;
  WriteLe32(reply_wire + 13, 12);
  assert(CephMsgrDecodeV1ConnectReply(
           reply_wire, sizeof(reply_wire), &reply) == EFI_SUCCESS);
  assert(CephMsgrClassifyV1ConnectReply(
           &reply, 0, 2, 7, &action) == EFI_SUCCESS);
  assert(action == CephMsgrConnectRetrySession);

  WriteLe32(reply_wire + 13, 7);
  assert(CephMsgrDecodeV1ConnectReply(
           reply_wire, sizeof(reply_wire), &reply) == EFI_SUCCESS);
  assert(CephMsgrClassifyV1ConnectReply(
           &reply, 0, 2, 7, &action) == EFI_COMPROMISED_DATA);
  assert(action == CephMsgrConnectRejected);

  reply_wire[0] = CEPH_MSGR_TAG_SEQ;
  assert(CephMsgrDecodeV1ConnectReply(
           reply_wire, sizeof(reply_wire), &reply) == EFI_SUCCESS);
  assert(CephMsgrClassifyV1ConnectReply(
           &reply, 0, 2, 7, &action) == EFI_SUCCESS);
  assert(action == CephMsgrConnectAckSequence);

  WriteLe64(reply_wire + 1, 0);
  assert(CephMsgrDecodeV1ConnectReply(
           reply_wire, sizeof(reply_wire), &reply) == EFI_SUCCESS);
  assert(CephMsgrClassifyV1ConnectReply(
           &reply, 1, 2, 7, &action) == EFI_UNSUPPORTED);
  assert(action == CephMsgrConnectRejected);

  reply_wire[0] = CEPH_MSGR_TAG_CHALLENGE_AUTH;
  WriteLe32(reply_wire + 21, 8);
  assert(CephMsgrDecodeV1ConnectReply(
           reply_wire, sizeof(reply_wire), &reply) == EFI_SUCCESS);
  assert(CephMsgrClassifyV1ConnectReply(
           &reply, 0, 2, 7, &action) == EFI_SUCCESS);
  assert(action == CephMsgrConnectNeedAuthorizerChallenge);
  WriteLe32(reply_wire + 21, 0);
  assert(CephMsgrDecodeV1ConnectReply(
           reply_wire, sizeof(reply_wire), &reply) == EFI_SUCCESS);
  assert(CephMsgrClassifyV1ConnectReply(
           &reply, 0, 2, 7, &action) == EFI_COMPROMISED_DATA);

  assert(CephMsgrDecodeV1ConnectReply(
           reply_wire, sizeof(reply_wire) - 1, &reply) == EFI_BAD_BUFFER_SIZE);
}

static void
TestRbdFormat2(void)
{
  CEPH_RBD_FORMAT2_METADATA metadata = {
    "rbd_data.imageid", 1ULL << 24, 22, 0, 0, 0
  };
  CEPH_RBD_FORMAT2_IMAGE image;
  CEPH_RBD_EXTENT extent;
  char object_name[128];
  UINTN name_length;

  assert(CephRbdFormat2Initialize(&metadata, &image) == EFI_SUCCESS);
  assert(image.Layout.ObjectSize == (1ULL << 22));
  assert(CephRbdFormat2BuildObjectName(
           &image, 0x2a, object_name, sizeof(object_name), &name_length) == EFI_SUCCESS);
  assert(strcmp(object_name, "rbd_data.imageid.000000000000002a") == 0);
  assert(name_length == strlen(object_name));
  assert(CephRbdFormat2BuildObjectName(
           &image, 0, object_name, 8, &name_length) == EFI_BUFFER_TOO_SMALL);

  metadata.Features = CEPH_RBD_FEATURE_STRIPINGV2;
  metadata.ObjectOrder = 13;
  metadata.StripeUnit = 4096;
  metadata.StripeCount = 2;
  assert(CephRbdFormat2Initialize(&metadata, &image) == EFI_SUCCESS);
  assert(CephRbdMapExtent(&image.Layout, 4096, 4096, &extent) == EFI_SUCCESS);
  assert(extent.ObjectNumber == 1 && extent.ObjectOffset == 0 && extent.Length == 4096);

  // Default image features from a real Ceph cluster must be readable.
  metadata.Features = CEPH_RBD_FEATURE_LAYERING |
                      CEPH_RBD_FEATURE_EXCLUSIVE_LOCK |
                      CEPH_RBD_FEATURE_OBJECT_MAP |
                      CEPH_RBD_FEATURE_FAST_DIFF |
                      CEPH_RBD_FEATURE_DEEP_FLATTEN;
  metadata.ObjectOrder = 22;
  metadata.StripeUnit = 0;
  metadata.StripeCount = 0;
  assert(CephRbdFormat2Initialize(&metadata, &image) == EFI_SUCCESS);
  metadata.Features = CEPH_RBD_FEATURE_DATA_POOL;
  assert(CephRbdFormat2Initialize(&metadata, &image) == EFI_UNSUPPORTED);
  metadata.Features = CEPH_RBD_FEATURES_INCOMPATIBLE;
  assert(CephRbdFormat2Initialize(&metadata, &image) == EFI_UNSUPPORTED);
  metadata.Features = 0;
  metadata.ObjectOrder = 32;
  assert(CephRbdFormat2Initialize(&metadata, &image) == EFI_INVALID_PARAMETER);
}

typedef struct {
  uint8_t Bytes[100 * CEPH_BIOS_SECTOR_SIZE];
} MOCK_RBD;

static int
MockReadAt(void *Context, uint64_t Offset, void *Buffer, size_t Length)
{
  MOCK_RBD *image = (MOCK_RBD *)Context;
  if (Offset > sizeof(image->Bytes) || Length > sizeof(image->Bytes) - Offset) {
    return -1;
  }
  memcpy(Buffer, image->Bytes + Offset, Length);
  return 0;
}

static void
TestBiosStage2(void)
{
  MOCK_RBD image = { { 0 } };
  uint8_t *mbr = image.Bytes;
  uint8_t *partition = mbr + 446;
  uint8_t boot_sector[CEPH_BIOS_SECTOR_SIZE];
  uint32_t start_lba = 0;

  partition[0] = 0x80;
  partition[4] = 0x83;
  WriteLe32(partition + 8, 1);
  WriteLe32(partition + 12, 50);
  mbr[510] = 0x55;
  mbr[511] = 0xaa;
  image.Bytes[CEPH_BIOS_SECTOR_SIZE + 510] = 0x55;
  image.Bytes[CEPH_BIOS_SECTOR_SIZE + 511] = 0xaa;

  assert(CephBiosStage2LoadPartitionSector(
           MockReadAt, &image, sizeof(image.Bytes), boot_sector, &start_lba) == CEPH_BIOS_SUCCESS);
  assert(start_lba == 1 && boot_sector[510] == 0x55 && boot_sector[511] == 0xaa);

  image.Bytes[CEPH_BIOS_SECTOR_SIZE + 511] = 0;
  assert(CephBiosStage2LoadPartitionSector(
           MockReadAt, &image, sizeof(image.Bytes), boot_sector, &start_lba) ==
         CEPH_BIOS_INVALID_BOOT_SECTOR);
}

int
main(void)
{
  TestWireAndCephX();
  TestCephXAes();
  TestMessenger();
  TestMessengerConnect();
  TestRbdFormat2();
  TestBiosStage2();
  TestTcp4AndMessengerHandshake();
  TestMessengerFrameTransport();
  TestMessengerSession();
  TestCephXAndRecovery();
  TestRpcCodecs();
  TestBlockIo();
  TestConfigAndDriverBinding();
  TestAuthCodecs();
  TestCrushPlacement();
  TestPlacement();
  TestGpt();
  TestAes256Krb5();
  TestNetwork();
  TestSecretResolver();
  puts("core firmware tests: passed");
  return 0;
}
