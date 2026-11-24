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
TestWireAndCephX(void)
{
  const uint8_t challenge_wire[] = { 1, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
  const uint8_t ticket[] = { 0xaa, 0xbb, 0xcc };
  uint8_t encoded[64];
  CEPHX_SERVER_CHALLENGE challenge;
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
  TestRbdFormat2();
  TestBiosStage2();
  puts("core firmware parser tests: passed");
  return 0;
}
