#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CephRbdPlacement.h"
#include "CephRbdBlockIo.h"

#ifndef CRUSH_VECTOR_DIR
#define CRUSH_VECTOR_DIR "tests/vectors"
#endif

static uint8_t *
ReadVector (const char *Name, size_t *Length)
{
  char      Path[512];
  FILE      *File;
  uint8_t   *Data;
  long      Size;

  snprintf (Path, sizeof (Path), "%s/%s", CRUSH_VECTOR_DIR, Name);
  File = fopen (Path, "rb");
  assert (File != NULL);
  assert (fseek (File, 0, SEEK_END) == 0);
  Size = ftell (File);
  assert (Size > 0);
  assert (fseek (File, 0, SEEK_SET) == 0);
  Data = malloc ((size_t)Size);
  assert (Data != NULL);
  assert (fread (Data, 1, (size_t)Size, File) == (size_t)Size);
  fclose (File);
  *Length = (size_t)Size;
  return Data;
}

void
TestPlacement (void)
{
  static const struct {
    const char  *Name;
    uint32_t     Pg;
    int32_t      Osd[3];
  } Cases[] = {
    { "rbd_id.verifyimg",           1, { 2, 0, 1 } },
    { "rbd_header.abc",             2, { 1, 0, 2 } },
    { "rbd_data.1.0000000000000000", 12, { 0, 1, 2 } },
    { "foo",                        6, { 0, 1, 2 } },
    { "bar",                       11, { 0, 1, 2 } },
  };

  CEPH_RBD_BLOCK_DEVICE  Device;
  CEPH_RBD_PLACEMENT     Placement;
  INT32                  OsdIds[CEPH_RBD_CRUSH_MAX_RESULT];
  UINTN               OsdCount;
  UINT32              Pg;
  UINT32              Pps;
  uint8_t             *OsdMap;
  size_t              OsdMapLength;
  size_t              I;
  UINTN               J;

  OsdMap = ReadVector ("osdmap.bin", &OsdMapLength);

  // Invalid arguments and missing pools are rejected.
  assert (CephRbdPlacementFromOsdMap (
            NULL, OsdMapLength, 4, &Placement) == EFI_INVALID_PARAMETER);
  assert (CephRbdPlacementFromOsdMap (
            OsdMap, OsdMapLength, 0, &Placement) == EFI_INVALID_PARAMETER);
  assert (CephRbdPlacementFromOsdMap (
            OsdMap, OsdMapLength, 999, &Placement) == EFI_NOT_FOUND);
  assert (CephRbdPlacementFromOsdMap (
            OsdMap, OsdMapLength / 2, 4, &Placement) != EFI_SUCCESS);

  assert (CephRbdPlacementFromOsdMap (
            OsdMap, OsdMapLength, 4, &Placement) == EFI_SUCCESS);
  assert (Placement.Valid);
  assert (Placement.Epoch == 3762);
  assert (Placement.PoolId == 4);
  assert (Placement.Type == 1);
  assert (Placement.Size == 3);
  assert (Placement.CrushRule == 0);
  assert (Placement.ObjectHash == CEPH_RBD_STR_HASH_RJENKINS);
  assert (Placement.PgNum == 32);
  assert (Placement.PgpNum == 32);
  assert ((Placement.Flags & CEPH_RBD_POOL_FLAG_HASHPSPOOL) != 0);

  for (I = 0; I < sizeof (Cases) / sizeof (Cases[0]); I++) {
    assert (CephRbdPlacementMapObject (
              &Placement, Cases[I].Name, strlen (Cases[I].Name),
              &Pg, &Pps, OsdIds, CEPH_RBD_CRUSH_MAX_RESULT,
              &OsdCount) == EFI_SUCCESS);
    assert (Pg == Cases[I].Pg);
    assert (OsdCount == 3);
    for (J = 0; J < 3; J++) {
      assert (OsdIds[J] == Cases[I].Osd[J]);
    }
    (VOID)Pps;
  }

  // Optional outputs and insufficient capacity behave as documented.
  assert (CephRbdPlacementMapObject (
            &Placement, "foo", 3, NULL, NULL,
            OsdIds, CEPH_RBD_CRUSH_MAX_RESULT, &OsdCount) == EFI_SUCCESS);
  assert (CephRbdPlacementMapObject (
            &Placement, "foo", 3, NULL, NULL,
            OsdIds, 2, &OsdCount) == EFI_INVALID_PARAMETER);
  assert (CephRbdPlacementMapObject (
            NULL, "foo", 3, NULL, NULL,
            OsdIds, CEPH_RBD_CRUSH_MAX_RESULT, &OsdCount) ==
          EFI_INVALID_PARAMETER);

  // Block I/O exposes the same placement through an open device.
  memset (&Device, 0, sizeof (Device));
  assert (CephRbdBlockIoGetObjectPlacement (
            &Device, "foo", 3, NULL, NULL,
            OsdIds, CEPH_RBD_CRUSH_MAX_RESULT, &OsdCount) ==
          EFI_INVALID_PARAMETER);
  Device.Ready = TRUE;
  Device.Placement = Placement;
  assert (CephRbdBlockIoGetObjectPlacement (
            &Device, "foo", 3, &Pg, &Pps,
            OsdIds, CEPH_RBD_CRUSH_MAX_RESULT, &OsdCount) == EFI_SUCCESS);
  assert (Pg == 6);
  assert (OsdCount == 3);
  assert (OsdIds[0] == 0 && OsdIds[1] == 1 && OsdIds[2] == 2);

  free (OsdMap);
}
