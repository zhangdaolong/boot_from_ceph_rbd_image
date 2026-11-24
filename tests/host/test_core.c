#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "CephRbdCephX.h"
#include "CephRbdFormat2.h"
#include "CephRbdMessenger.h"
#include "CephRbdWire.h"
#include "ceph_stage2.h"

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
TestMessenger(void)
{
  static const uint8_t crc_vector[] = "123456789";
  uint8_t wire[1 + CEPH_MSGR_V1_HEADER_SIZE + CEPH_MSGR_V1_FOOTER_SIZE] = { 0 };
  uint8_t payload_wire[1 + CEPH_MSGR_V1_HEADER_SIZE + 2 + CEPH_MSGR_V1_FOOTER_SIZE] = { 0 };
  uint8_t *header = wire + 1;
  uint8_t *footer = header + CEPH_MSGR_V1_HEADER_SIZE;
  uint8_t *payload_header = payload_wire + 1;
  uint8_t *payload = payload_header + CEPH_MSGR_V1_HEADER_SIZE;
  uint8_t *payload_footer = payload + 2;
  CEPH_MSGR_V1_FRAME frame;

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

  metadata.Features = 1;
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
  TestMessenger();
  TestMessengerConnect();
  TestRbdFormat2();
  TestBiosStage2();
  puts("core firmware parser tests: passed");
  return 0;
}
