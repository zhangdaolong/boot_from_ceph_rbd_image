#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include "CephRbdPlacement.h"
#include "CephRbdRpc.h"
#include "CephRbdWire.h"

// OSDMap::encode() wraps the map in ENCODE_START(8,7), then a client-usable
// ENCODE_START(10,1) whose payload is: fsid, epoch, created, modified, pools,
// pool_name, pool_max, flags, max_osd, osd_state, osd_weight, client_addrs,
// pg_temp, primary_temp, osd_primary_affinity, then the CRUSH bufferlist.
//
// We only decode the pools map and locate the CRUSH blob. Everything between
// is deliberately skipped instead of decoded: the pool's own ENCODE_START
// length lets us advance without knowing the rest of pg_pool_t, and the CRUSH
// blob is found by its length prefix plus magic and then fully validated by
// CephRbdCrushParseMap(). This keeps the decoder independent of the many
// variable-length OSD address structures in modern maps.

#define PLACEMENT_MAX_OSDMAP_VERSION  20
#define PLACEMENT_MAX_POOL_VERSION    40
#define PLACEMENT_MAX_POOLS           1024
#define PLACEMENT_MIN_CRUSH_BLOB      64

STATIC
EFI_STATUS
CephRbdPlacementReadLe32At (
  IN  CONST UINT8  *Base,
  IN  UINTN        BaseLength,
  IN  UINTN        Offset,
  OUT UINT32       *Value
  )
{
  if ((Base == NULL) || (Value == NULL) ||
      (Offset > BaseLength) || (BaseLength - Offset < 4))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  *Value = (UINT32)Base[Offset] |
           ((UINT32)Base[Offset + 1] << 8) |
           ((UINT32)Base[Offset + 2] << 16) |
           ((UINT32)Base[Offset + 3] << 24);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CephRbdPlacementReadEncodeStart (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  IN     UINT8             MaxVersion,
  OUT UINT32               *PayloadLength
  )
{
  EFI_STATUS  Status;
  UINT8       Version;
  UINT8       Compat;
  UINT32      Length;

  Status = CephWireReadU8 (Cursor, &Version);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (Cursor, &Compat);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (Cursor, &Length);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Version == 0) || (Version > MaxVersion)) {
    return EFI_UNSUPPORTED;
  }

  if (Length > Cursor->Remaining) {
    return EFI_BAD_BUFFER_SIZE;
  }

  (VOID)Compat;
  *PayloadLength = Length;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CephRbdPlacementDecodePool (
  IN OUT CEPH_WIRE_CURSOR   *Cursor,
  IN     INT32              PoolId,
  OUT    CEPH_RBD_OSD_POOL  *Pool,
  OUT BOOLEAN               *Matched
  )
{
  EFI_STATUS        Status;
  CEPH_WIRE_CURSOR  Payload;
  UINT64            EncodedId;
  UINT32            PayloadLength;
  UINT8             Type;
  UINT8             Size;
  UINT8             CrushRuleRaw;
  UINT8             ObjectHash;
  UINT32            PgNum;
  UINT32            PgpNum;
  UINT32            LastChange;
  UINT64            SnapSeq;
  UINT32            SnapEpoch;
  UINT32            SnapCount;
  UINT32            RemovedCount;
  UINT64            Auid;
  UINT32            Flags;

  *Matched = FALSE;

  Status = CephWireReadLe64 (Cursor, &EncodedId);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (EncodedId > 0x7fffffffU) {
    return EFI_UNSUPPORTED;
  }

  Status = CephRbdPlacementReadEncodeStart (
             Cursor, PLACEMENT_MAX_POOL_VERSION, &PayloadLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireInit (&Payload, Cursor->Data, PayloadLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // The leading pg_pool_t fields are stable across all supported struct_v.
  Status = CephWireReadU8 (&Payload, &Type);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (&Payload, &Size);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (&Payload, &CrushRuleRaw);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (&Payload, &ObjectHash);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Payload, &PgNum);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Payload, &PgpNum);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Payload, 8);  // lpg_num, lpgp_num
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Payload, &LastChange);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe64 (&Payload, &SnapSeq);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Payload, &SnapEpoch);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Payload, &SnapCount);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Payload, &RemovedCount);
  }

  // A non-empty managed-snapshot map has a variable-length snap_info_t value
  // that this minimal decoder does not understand. Report it explicitly
  // instead of reading pool flags from the wrong offset.
  if (!EFI_ERROR (Status) && (SnapCount != 0)) {
    return EFI_UNSUPPORTED;
  }

  if (!EFI_ERROR (Status) && (RemovedCount != 0)) {
    // interval_set<snapid_t>: u32 count then (u64 start, u64 len) per entry.
    // UINTN is 64-bit in UEFI so UINT32 * 16 cannot overflow here; the
    // bounds check inside CephWireSkip validates the actual skip.
    Status = CephWireSkip (&Payload, (UINTN)RemovedCount * 16);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe64 (&Payload, &Auid);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Payload, &Flags);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Advance the outer cursor past the complete pool, including any fields we
  // did not decode above.
  Status = CephWireSkip (Cursor, PayloadLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((UINT32)PoolId != (UINT32)EncodedId) {
    return EFI_SUCCESS;
  }

  Pool->Epoch = 0;
  Pool->PoolId = (INT32)EncodedId;
  Pool->Type = Type;
  Pool->Size = Size;
  Pool->CrushRule = (CrushRuleRaw & 0x80) != 0
                    ? (INT32)CrushRuleRaw - 256 : (INT32)CrushRuleRaw;
  Pool->ObjectHash = ObjectHash;
  Pool->PgNum = PgNum;
  Pool->PgpNum = PgpNum;
  Pool->Flags = Flags;
  (VOID)LastChange;
  (VOID)SnapSeq;
  (VOID)SnapEpoch;
  (VOID)Auid;
  *Matched = TRUE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CephRbdPlacementFindCrushMap (
  IN  CONST UINT8        *Base,
  IN  UINTN              BaseLength,
  IN  UINTN              StartOffset,
  OUT CEPH_RBD_CRUSH_MAP *Map
  )
{
  EFI_STATUS  Status;
  UINT32      Magic;
  UINT32      BlobLength;
  UINTN       Offset;

  if ((Base == NULL) || (Map == NULL) ||
      (StartOffset > BaseLength) || (BaseLength < 8))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (StartOffset > BaseLength - 8) {
    return EFI_NOT_FOUND;
  }

  for (Offset = StartOffset; Offset <= BaseLength - 8; Offset++) {
    Status = CephRbdPlacementReadLe32At (
               Base, BaseLength, Offset, &Magic
               );
    if (EFI_ERROR (Status) || (Magic != CEPH_RBD_CRUSH_MAGIC)) {
      continue;
    }

    if (Offset < 4) {
      continue;
    }

    Status = CephRbdPlacementReadLe32At (
               Base, BaseLength, Offset - 4, &BlobLength
               );
    if (EFI_ERROR (Status)) {
      continue;
    }

    if ((BlobLength < PLACEMENT_MIN_CRUSH_BLOB) ||
        (BlobLength > BaseLength - Offset))
    {
      continue;
    }

    // Full parse is the validation: a false magic match will not decode into
    // a usable CRUSH wrapper.
    Status = CephRbdCrushParseMap (Base + Offset, BlobLength, Map);
    if (!EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS
CephRbdPlacementFromOsdMap (
  IN  CONST VOID           *Buffer,
  IN  UINTN                BufferLength,
  IN  INT32                PoolId,
  OUT CEPH_RBD_PLACEMENT   *Placement
  )
{
  EFI_STATUS        Status;
  CEPH_WIRE_CURSOR  Cursor;
  CEPH_RBD_OSD_POOL Pool;
  UINT32            PayloadLength;
  UINT32            PoolCount;
  UINT32            Index;
  INT32             RawEpoch;
  BOOLEAN           Matched;
  CONST UINT8      *Base;
  UINTN             AfterPools;

  if ((Buffer == NULL) || (Placement == NULL) || (PoolId <= 0)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Placement, sizeof (*Placement));

  Status = CephWireInit (&Cursor, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdPlacementReadEncodeStart (
             &Cursor, PLACEMENT_MAX_OSDMAP_VERSION, &PayloadLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdPlacementReadEncodeStart (
             &Cursor, PLACEMENT_MAX_OSDMAP_VERSION, &PayloadLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // fsid, epoch, created, modified.
  Status = CephWireSkip (&Cursor, CEPH_FSID_SIZE);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, (UINT32 *)&RawEpoch);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, 8 + 8);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (RawEpoch < 0) {
    return EFI_COMPROMISED_DATA;
  }

  Status = CephWireReadLe32 (&Cursor, &PoolCount);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (PoolCount > PLACEMENT_MAX_POOLS) {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&Pool, sizeof (Pool));
  for (Index = 0; Index < PoolCount; Index++) {
    Status = CephRbdPlacementDecodePool (
               &Cursor, PoolId, &Pool, &Matched
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Matched) {
      break;
    }
  }

  if (!Matched) {
    return EFI_NOT_FOUND;
  }

  Base = (CONST UINT8 *)Buffer;
  AfterPools = (UINTN)(Cursor.Data - Base);

  Status = CephRbdPlacementFindCrushMap (
             Base, BufferLength, AfterPools, &Placement->Crush
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Placement->PoolId = Pool.PoolId;
  Placement->Type = Pool.Type;
  Placement->PgNum = Pool.PgNum;
  Placement->PgpNum = Pool.PgpNum;
  Placement->CrushRule = Pool.CrushRule;
  Placement->Size = Pool.Size;
  Placement->ObjectHash = Pool.ObjectHash;
  Placement->Flags = Pool.Flags;
  Placement->Epoch = (UINT64)RawEpoch;
  Placement->Valid = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdPlacementMapObject (
  IN  CONST CEPH_RBD_PLACEMENT  *Placement,
  IN  CONST CHAR8               *ObjectName,
  IN  UINTN                     ObjectNameLength,
  OUT UINT32                    *Pg,  OPTIONAL
  OUT UINT32                    *Pps,  OPTIONAL
  OUT INT32                     *OsdIds,
  IN  UINTN                     OsdIdsCapacity,
  OUT UINTN                     *OsdIdCount
  )
{
  EFI_STATUS  Status;
  UINT32      Hash;
  UINT32      PgValue;
  UINT32      PpsValue;

  if ((Placement == NULL) || !Placement->Valid ||
      (ObjectName == NULL) || (OsdIds == NULL) ||
      (OsdIdCount == NULL) || (OsdIdsCapacity == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Placement->ObjectHash != CEPH_RBD_STR_HASH_RJENKINS) {
    return EFI_UNSUPPORTED;
  }

  if ((Placement->PgNum == 0) || (Placement->PgpNum == 0) ||
      (Placement->Size == 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  Hash = CephRbdCrushStrHash (ObjectName, ObjectNameLength);
  PgValue = Hash % Placement->PgNum;

  if ((Placement->Flags & CEPH_RBD_POOL_FLAG_HASHPSPOOL) != 0) {
    // raw_pg_to_pps() with FLAG_HASHPSPOOL: crush_hash32_2(RJENKINS1,
    // pg.ps() % pgp_num, pool).
    PpsValue = CephRbdCrushHash32_2 (
                 CEPH_RBD_CRUSH_HASH_RJENKINS1,
                 PgValue % Placement->PgpNum,
                 (UINT32)Placement->PoolId
                 );
  } else {
    // Legacy: add the pool id rather than hashing it.
    PpsValue = (PgValue % Placement->PgpNum) + (UINT32)Placement->PoolId;
  }

  if (Pg != NULL) {
    *Pg = PgValue;
  }
  if (Pps != NULL) {
    *Pps = PpsValue;
  }

  Status = CephRbdCrushMapRule (
             &Placement->Crush,
             (UINT32)Placement->CrushRule,
             PpsValue,
             Placement->Size,
             NULL,
             OsdIds,
             OsdIdsCapacity,
             OsdIdCount
             );
  return Status;
}
