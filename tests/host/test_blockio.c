#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "CephRbdBlockIo.h"
#include "CephRbdWire.h"

static UINTN
BuildHeaderReply (
  UINT8  *Front,
  UINT64 ImageSize,
  UINT8  ObjectOrder
  )
{
  CEPH_WIRE_WRITER Writer;

  // cls_client::get_size_finish(): order (u8), then size (u64).
  assert(CephWireWriterInit (&Writer, Front, 256) == EFI_SUCCESS);
  assert(CephWireWriteU8 (&Writer, ObjectOrder) == EFI_SUCCESS);
  assert(CephWireWriteLe64 (&Writer, ImageSize) == EFI_SUCCESS);
  return Writer.Written;
}

static void
TestHeaderDecode (VOID)
{
  UINT8 Front[256];
  UINTN Length;
  CEPH_RBD_FORMAT2_METADATA Metadata;
  CEPH_RBD_FORMAT2_IMAGE Image;
  CEPH_RBD_EXTENT Extent;
  CHAR8 ObjectName[CEPH_RBD_OBJECT_NAME_MAX];
  UINTN NameLength;

  Length = BuildHeaderReply (Front, 1ULL << 20, 20);
  assert(CephRbdBlockIoDecodeHeader (Front, Length, &Metadata) ==
         EFI_SUCCESS);
  assert(Metadata.ImageSize == (1ULL << 20));
  assert(Metadata.ObjectOrder == 20);
  assert(Metadata.Features == 0);

  Metadata.ObjectPrefix = "rbd_data.imageid";
  assert(CephRbdFormat2Initialize (&Metadata, &Image) == EFI_SUCCESS);
  assert(CephRbdFormat2BuildObjectName (
           &Image, 3, ObjectName, sizeof (ObjectName), &NameLength) ==
         EFI_SUCCESS);
  assert(strcmp (ObjectName, "rbd_data.imageid.0000000000000003") == 0);

  assert(CephRbdMapExtent (
           &Image.Layout, 0, CEPH_RBD_BLOCK_SIZE * 2, &Extent) ==
         EFI_SUCCESS);
  assert(Extent.ObjectNumber == 0);
  assert(Extent.ObjectOffset == 0);
  assert(Extent.Length == CEPH_RBD_BLOCK_SIZE * 2);

  // Malformed replies must be rejected.
  Front[0] = 0;
  assert(CephRbdBlockIoDecodeHeader (Front, Length, &Metadata) ==
         EFI_UNSUPPORTED);
  Front[0] = 1;
  assert(CephRbdBlockIoDecodeHeader (Front, Length - 1, &Metadata) ==
         EFI_BAD_BUFFER_SIZE);
}

static void
TestMediaLastLba (VOID)
{
  CEPH_RBD_BLOCK_DEVICE Device;
  UINT64 LastLba;

  memset (&Device, 0, sizeof (Device));
  assert(CephRbdBlockIoGetMediaLastLba (&Device, &LastLba) ==
         EFI_INVALID_PARAMETER);

  Device.Ready = TRUE;
  Device.Image.Layout.ImageSize = 0;
  assert(CephRbdBlockIoGetMediaLastLba (&Device, &LastLba) ==
         EFI_INVALID_PARAMETER);

  Device.Image.Layout.ImageSize = 1ULL << 20;
  assert(CephRbdBlockIoGetMediaLastLba (&Device, &LastLba) == EFI_SUCCESS);
  assert(LastLba == ((1ULL << 20) / CEPH_RBD_BLOCK_SIZE) - 1);

  // Bounds checking in ReadBlocks (no OSD session pool supplied).
  assert(CephRbdBlockIoReadBlocks (
           NULL, &Device, 0, 1, NULL, CEPH_RBD_BLOCK_SIZE) ==
         EFI_INVALID_PARAMETER);
}

VOID
TestBlockIo (VOID)
{
  TestHeaderDecode ();
  TestMediaLastLba ();
  puts("Block I/O tests: passed");
}
