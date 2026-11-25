#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CephRbdCrush.h"

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
TestCrushPlacement (void)
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

  CEPH_RBD_CRUSH_MAP  Map;
  uint8_t             *Blob;
  size_t              BlobLength;
  INT32               OsdIds[CEPH_RBD_CRUSH_MAX_RESULT];
  UINTN               OsdCount;
  UINT32              Hash;
  UINT32              Pg;
  UINT32              Pps;
  size_t              I;
  UINTN               J;

  Blob = ReadVector ("crushmap-embedded.bin", &BlobLength);
  assert (CephRbdCrushParseMap (Blob, BlobLength, &Map) == EFI_SUCCESS);
  assert (Map.BucketCount == 4);
  assert (Map.RuleCount == 1);
  assert (Map.ChooseTotalTries == 50);
  assert (Map.ChooseleafVaryR == 1);
  assert (Map.StrawCalcVersion == 1);
  assert (Map.AllowedBucketAlgs == 0x36);

  // A truncated map must be rejected rather than read out of bounds.
  assert (CephRbdCrushParseMap (Blob, BlobLength / 2, &Map) != EFI_SUCCESS);
  assert (CephRbdCrushParseMap (NULL, BlobLength, &Map) == EFI_INVALID_PARAMETER);

  // A failed parse leaves the output zeroed, so restore the valid map.
  assert (CephRbdCrushParseMap (Blob, BlobLength, &Map) == EFI_SUCCESS);

  for (I = 0; I < sizeof (Cases) / sizeof (Cases[0]); I++) {
    Hash = CephRbdCrushStrHash (Cases[I].Name, strlen (Cases[I].Name));
    Pg = Hash % 32;
    assert (Pg == Cases[I].Pg);

    // Pool 4 has FLAG_HASHPSPOOL; Ceph hashes (pg, pool) for the seed.
    Pps = CephRbdCrushHash32_2 (CEPH_RBD_CRUSH_HASH_RJENKINS1, Pg, 4);
    assert (CephRbdCrushMapRule (
              &Map, 0, Pps, 3, NULL, OsdIds,
              CEPH_RBD_CRUSH_MAX_RESULT, &OsdCount) == EFI_SUCCESS);
    assert (OsdCount == 3);
    for (J = 0; J < 3; J++) {
      assert (OsdIds[J] == Cases[I].Osd[J]);
    }
  }

  assert (CephRbdCrushMapRule (
            &Map, 99, 1, 3, NULL, OsdIds,
            CEPH_RBD_CRUSH_MAX_RESULT, &OsdCount) == EFI_NOT_FOUND);
  assert (CephRbdCrushMapRule (
            &Map, 0, 1, 3, NULL, OsdIds, 2, &OsdCount) == EFI_INVALID_PARAMETER);

  free (Blob);
}
