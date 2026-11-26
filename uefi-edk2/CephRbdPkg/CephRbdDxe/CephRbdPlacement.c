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

// Decode one entity_addr_t. Modern OSDMaps encode marker 1 followed by
// ENCODE_START(1,1), then type (u32), nonce (u32), elen (u32) and a
// sockaddr_storage prefix. For AF_INET the first two bytes of sa_data are
// the big-endian port and the next four are the IPv4 address in network byte
// order. Only the LEGACY (type 1) address is retained: it is the messenger v1
// endpoint, while type 2 is the msgr2 endpoint.
STATIC
EFI_STATUS
CephRbdPlacementDecodeEntityAddr (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  OUT UINT32               *Ipv4,
  OUT UINT16               *Port,
  OUT BOOLEAN              *IsLegacy
  )
{
  EFI_STATUS        Status;
  CEPH_WIRE_CURSOR  Payload;
  UINT32            StructLength;
  UINT32            Type;
  UINT32            Nonce;
  UINT32            Elen;
  UINT16            Family;
  UINT32            Address;
  UINT8             Marker;
  UINT8             PortHigh;
  UINT8             PortLow;

  *Ipv4 = 0;
  *Port = 0;
  *IsLegacy = FALSE;

  Status = CephWireReadU8 (Cursor, &Marker);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Marker == 0) {
    // Legacy addr_after_marker: u8 marker, u16 rest, u32 nonce, sockaddr_storage.
    Status = CephWireSkip (Cursor, sizeof (UINT16) + sizeof (UINT32) + 128);
    return Status;
  }

  if (Marker != 1) {
    return EFI_UNSUPPORTED;
  }

  Status = CephRbdPlacementReadEncodeStart (
             Cursor, PLACEMENT_MAX_OSDMAP_VERSION, &StructLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireInit (&Payload, Cursor->Data, StructLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireReadLe32 (&Payload, &Type);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Payload, &Nonce);  // nonce
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Payload, &Elen);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Elen >= sizeof (UINT16)) {
    // entity_addr_t::encode() appends ss_family as a 16-bit value (AF_INET
    // is 2) before the raw sa_data bytes. Read it as u16 directly; reading a
    // u32 here would leave the port bytes in the upper half of Family and
    // make the AF_INET comparison fail.
    Status = CephWireReadLe16 (&Payload, &Family);

    if (!EFI_ERROR (Status) && (Family == 2) && (Elen >= 8)) {
      // AF_INET: port (2 bytes, network order), IPv4 (4 bytes),
      // then remaining sa_data padding in sockaddr_in.
      Status = CephWireReadU8 (&Payload, &PortHigh);
      if (!EFI_ERROR (Status)) {
        Status = CephWireReadU8 (&Payload, &PortLow);
      }
      if (!EFI_ERROR (Status)) {
        *Port = (UINT16)((PortHigh << 8) | PortLow);
        Status = CephWireReadBytes (&Payload, &Address, sizeof (Address));
      }
      if (!EFI_ERROR (Status)) {
        *Ipv4 = Address;  // network byte order
        // Family(2) + port(2) + IPv4(4) = 8 bytes consumed from Elen.
        Status = CephWireSkip (&Payload, Elen - 8);
      }
    } else if (!EFI_ERROR (Status)) {
      Status = CephWireSkip (&Payload, Elen - sizeof (UINT16));
    }
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireSkip (Cursor, StructLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *IsLegacy = (Type == CEPH_RBD_ADDR_TYPE_LEGACY);
  return EFI_SUCCESS;
}

// Parse the fields between the pools map and the embedded CRUSH blob. This
// is where OSDMap::encode() places osd_addrs->client_addrs; retaining the
// messenger-v1 addresses lets the driver connect directly to the primary OSD.
STATIC
EFI_STATUS
CephRbdPlacementDecodeOsdAddrs (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  OUT CEPH_RBD_PLACEMENT   *Placement
  )
{
  EFI_STATUS  Status;
  UINT32      Count;
  UINT32      InnerCount;
  UINT32      Index;
  UINT32      OsdIndex;
  UINT32      StringLength;
  UINT64      PoolId;
  UINT8       Marker;
  UINT32      Ipv4;
  UINT16      Port;
  BOOLEAN     IsLegacy;

  // pool_name: map<int64_t, string>.
  Status = CephWireReadLe32 (Cursor, &Count);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Count > PLACEMENT_MAX_POOLS) {
    return EFI_UNSUPPORTED;
  }
  for (Index = 0; Index < Count; Index++) {
    Status = CephWireReadLe64 (Cursor, &PoolId);
    if (!EFI_ERROR (Status)) {
      Status = CephWireReadLe32 (Cursor, &StringLength);
    }
    if (!EFI_ERROR (Status) && (StringLength > Cursor->Remaining)) {
      return EFI_BAD_BUFFER_SIZE;
    }
    if (!EFI_ERROR (Status)) {
      Status = CephWireSkip (Cursor, StringLength);
    }
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  // pool_max, flags, max_osd.
  Status = CephWireSkip (Cursor, sizeof (UINT32) * 2);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (Cursor, &Count);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // osd_state and osd_weight: both vector<u32>.
  Status = CephWireReadLe32 (Cursor, &InnerCount);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (InnerCount > 4096) {
    return EFI_UNSUPPORTED;
  }
  Status = CephWireSkip (Cursor, (UINTN)InnerCount * sizeof (UINT32));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireReadLe32 (Cursor, &InnerCount);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (InnerCount > 4096) {
    return EFI_UNSUPPORTED;
  }
  Status = CephWireSkip (Cursor, (UINTN)InnerCount * sizeof (UINT32));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // client_addrs: vector<entity_addrvec_t>. One entry per OSD; a cluster may
  // have more OSDs than we can retain, so keep only the first
  // CEPH_RBD_MAX_OSDS but still walk all entries to keep the cursor aligned.
  Status = CephWireReadLe32 (Cursor, &Count);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Count > 4096) {
    return EFI_UNSUPPORTED;
  }

  Placement->OsdCount = 0;
  for (Index = 0; Index < Count; Index++) {
    Status = CephWireReadU8 (Cursor, &Marker);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (Marker != 2) {
      return EFI_UNSUPPORTED;
    }

    Status = CephWireReadLe32 (Cursor, &InnerCount);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (InnerCount > 8) {
      return EFI_UNSUPPORTED;
    }

    for (OsdIndex = 0; OsdIndex < InnerCount; OsdIndex++) {
      Status = CephRbdPlacementDecodeEntityAddr (
                 Cursor, &Ipv4, &Port, &IsLegacy
                 );
      if (EFI_ERROR (Status)) {
        return Status;
      }
      if (IsLegacy && (Port != 0) &&
          (Placement->OsdCount < CEPH_RBD_MAX_OSDS))
      {
        Placement->OsdAddr[Placement->OsdCount].OsdId = (INT32)Index;
        Placement->OsdAddr[Placement->OsdCount].Ipv4 = Ipv4;
        Placement->OsdAddr[Placement->OsdCount].Port = Port;
        Placement->OsdAddr[Placement->OsdCount].Valid = TRUE;
        Placement->OsdCount++;
      }
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdPlacementGetOsdAddress (
  IN  CONST CEPH_RBD_PLACEMENT  *Placement,
  IN  INT32                     OsdId,
  OUT UINT32                    *Ipv4,
  OUT UINT16                    *Port
  )
{
  UINT32  Index;

  if ((Placement == NULL) || (Ipv4 == NULL) || (Port == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < Placement->OsdCount; Index++) {
    if (Placement->OsdAddr[Index].Valid &&
        (Placement->OsdAddr[Index].OsdId == OsdId))
    {
      *Ipv4 = Placement->OsdAddr[Index].Ipv4;
      *Port = Placement->OsdAddr[Index].Port;
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
  BOOLEAN           Found;
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
  Found = FALSE;
  for (Index = 0; Index < PoolCount; Index++) {
    Status = CephRbdPlacementDecodePool (
               &Cursor, PoolId, &Pool, &Matched
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Matched) {
      Found = TRUE;
    }
  }

  if (!Found) {
    return EFI_NOT_FOUND;
  }

  Base = (CONST UINT8 *)Buffer;
  AfterPools = (UINTN)(Cursor.Data - Base);

  // The OSD address table lives between the pools map and the CRUSH blob.
  // Retaining it enables direct-to-OSD routing, but a failure here must not
  // invalidate the placement map that callers already rely on. The CRUSH
  // blob is located by magic from AfterPools regardless of this result.
  CephRbdPlacementDecodeOsdAddrs (&Cursor, Placement);

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
