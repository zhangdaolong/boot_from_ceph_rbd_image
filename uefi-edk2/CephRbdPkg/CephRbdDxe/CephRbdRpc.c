#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include "CephRbdRpc.h"
#include "CephRbdWire.h"

EFI_STATUS
CephRpcEncodeMonGetMap (
  IN CONST CEPH_MON_REQUEST_HEADER  *Header,
  IN CONST UINT8                    *Fsid,
  IN UINT32                         StartEpoch,
  OUT VOID                          *Buffer,
  IN UINTN                          BufferLength,
  OUT UINTN                         *EncodedLength
  )
{
  CEPH_WIRE_WRITER  Writer;
  EFI_STATUS        Status;

  if ((Header == NULL) || (Fsid == NULL) || (Buffer == NULL) ||
      (EncodedLength == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *EncodedLength = 0;
  if (BufferLength < (CEPH_MON_REQUEST_HEADER_SIZE + CEPH_FSID_SIZE +
                      sizeof (StartEpoch)))
  {
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = CephWireWriterInit (&Writer, Buffer, BufferLength);
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, Header->HaveVersion);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe16 (&Writer, Header->SessionMon);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, Header->SessionMonTid);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteBytes (&Writer, Fsid, CEPH_FSID_SIZE);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, StartEpoch);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EncodedLength = Writer.Written;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRpcEncodeMonCommand (
  IN CONST CEPH_MON_REQUEST_HEADER  *Header,
  IN CONST UINT8                    *Fsid,
  IN CONST CHAR8                    *Command,
  IN UINTN                          CommandLength,
  OUT VOID                          *Buffer,
  IN UINTN                          BufferLength,
  OUT UINTN                         *EncodedLength
  )
{
  CEPH_WIRE_WRITER  Writer;
  EFI_STATUS        Status;
  UINTN             RequiredLength;

  if ((Header == NULL) || (Fsid == NULL) || (Command == NULL) ||
      (Buffer == NULL) || (EncodedLength == NULL) ||
      (CommandLength == 0) || (CommandLength > CEPH_RPC_MAX_COMMAND))
  {
    return EFI_INVALID_PARAMETER;
  }

  RequiredLength = CEPH_PAXOS_REQUEST_HEADER_SIZE + CEPH_FSID_SIZE +
                   sizeof (UINT32) + sizeof (UINT32) + CommandLength;
  *EncodedLength = 0;
  if (BufferLength < RequiredLength) {
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = CephWireWriterInit (&Writer, Buffer, BufferLength);
  // MMonCommand::encode_payload() starts with paxos_encode(): version (u64),
  // deprecated_session_mon (__s16), deprecated_session_mon_tid (u64). Using a
  // 32-bit session_mon field here shifts the fsid and the command vector by
  // two bytes; the monitor then reads a bogus vector size and drops the
  // connection with "failed to decode message of type 50 ... End of buffer".
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, Header->HaveVersion);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe16 (&Writer, (UINT16)Header->SessionMon);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, Header->SessionMonTid);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteBytes (&Writer, Fsid, CEPH_FSID_SIZE);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 1);  // num_strs
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, (UINT32)CommandLength);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteBytes (&Writer, Command, CommandLength);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EncodedLength = Writer.Written;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRpcDecodeMonCommandAck (
  IN CONST VOID  *Buffer,
  IN UINTN       BufferLength,
  OUT INT32      *Result,
  OUT CONST UINT8  **OutData,  OPTIONAL
  OUT UINTN      *OutDataLength  OPTIONAL
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT32            RawResult;
  UINT32            StringLength;
  UINT32            VectorCount;
  UINT32            Index;

  if ((Buffer == NULL) || (Result == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((OutData != NULL) && (OutDataLength == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephWireInit (&Cursor, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // MMonCommandAck::decode_payload: paxos_decode (u64 version, __s16
  // session_mon, u64 tid), result (int32), rs (string), cmd (vector<string>).
  Status = CephWireSkip (&Cursor, CEPH_PAXOS_REQUEST_HEADER_SIZE);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &RawResult);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Result = (INT32)RawResult;
  if (OutData != NULL) {
    *OutData = NULL;
  }

  if (OutDataLength != NULL) {
    *OutDataLength = 0;
  }

  // Skip the human-readable status string.
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &StringLength);
  }

  if (!EFI_ERROR (Status) && (StringLength > Cursor.Remaining)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, StringLength);
  }

  // Skip cmd vector; each element is a length-prefixed string.
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &VectorCount);
  }

  if (!EFI_ERROR (Status) && (VectorCount > 1024)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  for (Index = 0; !EFI_ERROR (Status) && (Index < VectorCount); Index++) {
    Status = CephWireReadLe32 (&Cursor, &StringLength);
    if (!EFI_ERROR (Status) && (StringLength > Cursor.Remaining)) {
      return EFI_BAD_BUFFER_SIZE;
    }

    if (!EFI_ERROR (Status)) {
      Status = CephWireSkip (&Cursor, StringLength);
    }
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((OutData != NULL) && (Cursor.Remaining != 0)) {
    *OutData = Cursor.Data;
    *OutDataLength = Cursor.Remaining;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephRpcDecodeOsdMapEpoch (
  IN CONST VOID  *Buffer,
  IN UINTN       BufferLength,
  OUT UINT64     *Epoch
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT8             OuterVersion;
  UINT8             InnerVersion;
  UINT32            StructLength;
  INT32             RawEpoch;

  if ((Buffer == NULL) || (Epoch == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephWireInit (&Cursor, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // OSDMap::encode() wraps the map in two ENCODE_START records. Each record is
  // struct_v (u8), struct_compat (u8) and struct_len (le32). The inner payload
  // starts with fsid (16 bytes) and only then the epoch (signed le32).
  Status = CephWireReadU8 (&Cursor, &OuterVersion);
  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, 1);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &StructLength);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((OuterVersion < 1) || (OuterVersion > 20)) {
    return EFI_UNSUPPORTED;
  }

  if (StructLength < (6 + CEPH_FSID_SIZE + sizeof (RawEpoch))) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = CephWireReadU8 (&Cursor, &InnerVersion);
  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, 1);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &StructLength);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((InnerVersion < 1) || (InnerVersion > 20)) {
    return EFI_UNSUPPORTED;
  }

  if (StructLength < (CEPH_FSID_SIZE + sizeof (RawEpoch))) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = CephWireSkip (&Cursor, CEPH_FSID_SIZE);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, (UINT32 *)&RawEpoch);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (RawEpoch < 0) {
    return EFI_COMPROMISED_DATA;
  }

  *Epoch = (UINT64)RawEpoch;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRpcDecodeMonMapEpoch (
  IN CONST VOID  *Buffer,
  IN UINTN       BufferLength,
  OUT UINT64     *Epoch
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT32            BlobLength;
  UINT8             StructVersion;
  UINT8             CompatVersion;
  UINT32            StructLength;
  INT32             RawEpoch;

  if ((Buffer == NULL) || (Epoch == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephWireInit (&Cursor, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // MMonMap front is a single encoded bufferlist: u32 byte count, then the
  // MonMap::encode() payload with ENCODE_START(10,6), fsid and epoch.
  Status = CephWireReadLe32 (&Cursor, &BlobLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((BlobLength == 0) || (BlobLength > Cursor.Remaining)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = CephWireReadU8 (&Cursor, &StructVersion);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (&Cursor, &CompatVersion);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &StructLength);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((StructVersion < 1) || (StructVersion > 20)) {
    return EFI_UNSUPPORTED;
  }

  if (StructLength < (CEPH_FSID_SIZE + sizeof (RawEpoch))) {
    return EFI_BAD_BUFFER_SIZE;
  }
  (VOID)CompatVersion;

  Status = CephWireSkip (&Cursor, CEPH_FSID_SIZE);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, (UINT32 *)&RawEpoch);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (RawEpoch < 0) {
    return EFI_COMPROMISED_DATA;
  }

  *Epoch = (UINT64)RawEpoch;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CephRpcWriteOsdOp (
  IN OUT CEPH_WIRE_WRITER       *Writer,
  IN CONST CEPH_OSD_OP_REQUEST  *Op,
  IN UINT32                     PayloadLength
  )
{
  EFI_STATUS  Status;

  // struct ceph_osd_op is a fixed 38-byte packed wire structure:
  //   u16 op, u32 flags, 28-byte union, u32 payload_len.
  Status = CephWireWriteLe16 (Writer, Op->Op);
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (Writer, Op->Flags);
  }

  if (!EFI_ERROR (Status)) {
    if (Op->Op == CEPH_OSD_OP_CALL) {
      // cls union: u8 class_len, u8 method_len, u8 argc, le32 indata_len.
      // The class/method/indata bytes themselves live in the message data
      // buffer; here we only declare their lengths (Ceph merge...in_data).
      Status = CephWireWriteU8 (Writer, (UINT8)Op->ClassNameLength);
      if (!EFI_ERROR (Status)) {
        Status = CephWireWriteU8 (Writer, (UINT8)Op->MethodNameLength);
      }
      if (!EFI_ERROR (Status)) {
        Status = CephWireWriteU8 (Writer, 0);  // argc
      }
      if (!EFI_ERROR (Status)) {
        // cls.indata_len is the length of the caller's indata ONLY. The class
        // and method names are accounted for by class_len/method_len. Ceph's
        // PrimaryLogPG::do_osd_op() does bp.copy(class_len) + bp.copy(method_len)
        // + bp.copy(indata_len) from the (payload_len-sized) data slice; setting
        // this to the sum makes the OSD read past the end and return -EINVAL.
        Status = CephWireWriteLe32 (Writer, (UINT32)Op->IndataLength);
      }
      // The union is a fixed 28 bytes (sized by the extent variant); the cls
      // variant uses only 7. Pad the remaining 21 bytes so payload_len lands
      // at the correct 34-byte offset and the op occupies exactly 38 bytes.
      {
        UINTN  PadIndex;

        for (PadIndex = 0; !EFI_ERROR (Status) && (PadIndex < 21); PadIndex++) {
          Status = CephWireWriteU8 (Writer, 0);
        }
      }
    } else {
      // extent union: le64 offset, length, truncate_size; le32 truncate_seq.
      Status = CephWireWriteLe64 (Writer, Op->Offset);
      if (!EFI_ERROR (Status)) {
        Status = CephWireWriteLe64 (Writer, Op->Length);
      }
      if (!EFI_ERROR (Status)) {
        Status = CephWireWriteLe64 (Writer, 0);  // truncate_size
      }
      if (!EFI_ERROR (Status)) {
        Status = CephWireWriteLe32 (Writer, 0);  // truncate_seq
      }
    }
  }

  if (!EFI_ERROR (Status)) {
    // Number of bytes this op consumes in the message data buffer. Ceph's
    // split_osd_op_vector_in_data() uses it to slice ops[i].indata.
    Status = CephWireWriteLe32 (Writer, PayloadLength);
  }

  return Status;
}

EFI_STATUS
CephRpcEncodeOsdOp (
  IN UINT32                         PoolId,
  IN UINT32                         ClientIncarnation,
  IN UINT32                         OsdmapEpoch,
  IN UINT64                         RequestId,
  IN CONST CHAR8                    *ObjectName,
  IN UINTN                          ObjectNameLength,
  IN CONST CEPH_OSD_OP_REQUEST      *Ops,
  IN UINTN                          OpCount,
  OUT VOID                          *Buffer,
  IN UINTN                          BufferLength,
  OUT UINTN                         *EncodedLength,
  OUT VOID                          *Data  OPTIONAL,
  IN UINTN                          DataCapacity  OPTIONAL,
  OUT UINTN                         *DataLength  OPTIONAL
  )
{
  CEPH_WIRE_WRITER   Writer;
  CEPH_WIRE_WRITER   DataWriter;
  EFI_STATUS         Status;
  UINTN              Index;
  UINTN              RequiredLength;
  UINTN              RequiredDataLength;
  BOOLEAN            HasCall;

  if ((ObjectName == NULL) || (ObjectNameLength == 0) ||
      (ObjectNameLength > CEPH_RPC_MAX_COMMAND) || (Ops == NULL) ||
      (OpCount == 0) || (OpCount > 16) || (Buffer == NULL) ||
      (EncodedLength == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  // The legacy MOSDOp layout has no explicit reqid; the OSD derives it from
  // client_inc, so RequestId is intentionally unused here.
  (VOID)RequestId;

  // MOSDOp "old" encoding (header.version < 2 in Ceph 22 decode_payload):
  //   u32 client_inc
  //   old_pg_t (ceph_pg raw, 8 bytes): u16 preferred, u16 ps, u32 pool
  //     (the OSD recomputes ps from the oid hash; pool must be correct)
  //   u32 su = 0
  //   u32 osdmap_epoch
  //   u32 flags
  //   ceph_timespec mtime: u32 sec + u32 nsec
  //   eversion_t reassert_version: u32 epoch + u64 version
  //   u32 oid_len
  //   u64 snapid
  //   u64 snap_seq
  //   u32 num_snaps
  //   u16 num_ops
  //   ceph_osd_op ops[]
  //   oid bytes (nohead)
  //   snaps (nohead, none)
  RequiredLength = sizeof (UINT32) * 2 + sizeof (UINT32) * 2 +
                   sizeof (UINT32) * 3 + sizeof (UINT32) * 2 +
                   sizeof (UINT64) * 2 + sizeof (UINT32) +
                   sizeof (UINT64) * 2 + sizeof (UINT32) +
                   sizeof (UINT16) +
                   OpCount * CEPH_OSD_OP_SIZE +
                   ObjectNameLength;

  // Ceph carries CALL class/method/indata bytes in the message data buffer,
  // not in the front. Compute that data length and validate the caller-supplied
  // data buffer when there is at least one CALL op.
  RequiredDataLength = 0;
  HasCall = FALSE;
  for (Index = 0; Index < OpCount; Index++) {
    if (Ops[Index].Op == CEPH_OSD_OP_CALL) {
      HasCall = TRUE;
      RequiredDataLength += Ops[Index].ClassNameLength +
                            Ops[Index].MethodNameLength +
                            Ops[Index].IndataLength;
    }
  }
  if (HasCall && ((Data == NULL) || (DataLength == NULL) ||
                  (DataCapacity < RequiredDataLength)))
  {
    return EFI_BUFFER_TOO_SMALL;
  }

  *EncodedLength = 0;
  if (DataLength != NULL) {
    *DataLength = 0;
  }
  if (BufferLength < RequiredLength) {
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = CephWireWriterInit (&Writer, Buffer, BufferLength);
  if (!EFI_ERROR (Status) && HasCall) {
    Status = CephWireWriterInit (&DataWriter, Data, DataCapacity);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, ClientIncarnation);
  }
  // old_pg_t is ceph_pg: u16 preferred (-1), u16 ps, u32 pool. The OSD
  // recomputes ps from the oid hash, so only the pool field is significant.
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe16 (&Writer, 0xffff);  // preferred = -1
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe16 (&Writer, 0);  // ps
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, PoolId);  // pool
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // su
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, OsdmapEpoch);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, CEPH_OSD_FLAG_READ);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // mtime sec
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // mtime nsec
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // reassert epoch
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, 0);  // reassert version
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, (UINT32)ObjectNameLength);
  }
  if (!EFI_ERROR (Status)) {
    // sobject_t snapid: CEPH_NOSNAP reads.
    Status = CephWireWriteLe64 (&Writer, MAX_UINT64 - 1);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, 0);  // snap_seq
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // num_snaps
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe16 (&Writer, (UINT16)OpCount);
  }

  for (Index = 0; !EFI_ERROR (Status) && (Index < OpCount); Index++) {
    UINT32  PayloadLength;

    PayloadLength = 0;
    if (Ops[Index].Op == CEPH_OSD_OP_CALL) {
      // Ceph's OSDOp::merge_osd_op_vector_in_data() concatenates the class
      // name, method name and in-data into one data-region buffer per op and
      // records its length in payload_len. The OSD splits it back using the
      // cls.class_len/method_len/indata_len fields encoded in the front.
      PayloadLength = (UINT32)(Ops[Index].ClassNameLength +
                               Ops[Index].MethodNameLength +
                               Ops[Index].IndataLength);
      if (!EFI_ERROR (Status) && (Ops[Index].ClassNameLength != 0)) {
        Status = CephWireWriteBytes (
                   &DataWriter, Ops[Index].ClassName, Ops[Index].ClassNameLength
                   );
      }
      if (!EFI_ERROR (Status) && (Ops[Index].MethodNameLength != 0)) {
        Status = CephWireWriteBytes (
                   &DataWriter, Ops[Index].MethodName, Ops[Index].MethodNameLength
                   );
      }
      if (!EFI_ERROR (Status) && (Ops[Index].IndataLength != 0)) {
        Status = CephWireWriteBytes (
                   &DataWriter, Ops[Index].Indata, Ops[Index].IndataLength
                   );
      }
    }

    if (!EFI_ERROR (Status)) {
      Status = CephRpcWriteOsdOp (&Writer, &Ops[Index], PayloadLength);
    }
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteBytes (&Writer, ObjectName, ObjectNameLength);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EncodedLength = Writer.Written;
  if (DataLength != NULL) {
    *DataLength = HasCall ? DataWriter.Written : 0;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
CephRpcDecodeOsdOpReply (
  IN CONST VOID          *Front,
  IN UINTN               FrontLength,
  IN CONST VOID          *Data,
  IN UINTN               DataLength,
  OUT CEPH_OSD_OP_REPLY  *Reply
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT32            StringLength;
  UINT32            RawResult;
  UINT32            NumOps;
  UINT32            Index;
  INT32             Rval;
  UINT8             StructVersion;
  UINT64            Pool;
  UINT32            Ps;
  UINT32            Preferred;

  if ((Front == NULL) || (Reply == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Reply->Result = 0;
  Reply->NumOps = 0;
  Reply->Data = Data;
  Reply->DataLength = DataLength;

  Status = CephWireInit (&Cursor, Front, FrontLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // MOSDOpReply HEAD_VERSION (v8) encode_payload() order (Ceph 22):
  //   object_t oid         : u32 len + name bytes
  //   pg_t pgid            : u8 struct_v(=1) + u64 pool + u32 ps + i32 preferred
  //   s64 flags            : 8 bytes
  //   errorcode32_t result : i32
  //   eversion_t bad_replay: u64 version + u32 epoch (raw struct append)
  //   u32 osdmap_epoch
  //   u32 num_ops
  //   ceph_osd_op ops[]    : 38 bytes each
  //   i32 retry_attempt
  //   i32 rval[]           : one per op (v4+)
  //   eversion_t replay_version, u64 user_version, u8 do_redirect, trace
  //
  // We only need oid/pgid/flags/result/num_ops/rvals. The probe's READ op
  // uses a single op and the actual returned bytes live in the message data
  // buffer (already captured by CephRpcCall), not in the front.
  Status = CephWireReadLe32 (&Cursor, &StringLength);
  if (!EFI_ERROR (Status) && (StringLength > Cursor.Remaining)) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, StringLength);
  }

  // pg_t::encode: u8 v, u64 pool, u32 ps, i32 preferred(-1).
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (&Cursor, &StructVersion);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe64 (&Cursor, &Pool);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &Ps);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &Preferred);
  }

  // int64_t flags.
  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, sizeof (UINT64));
  }

  // result (errorcode32_t is a packed i32).
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &RawResult);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Reply->Result = (INT32)RawResult;
  if (Reply->Result != 0) {
    return EFI_SUCCESS;
  }

  // eversion_t bad_replay_version: raw append of version_t(u64) + epoch_t(u32).
  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, sizeof (UINT64) + sizeof (UINT32));
  }
  // u32 osdmap_epoch.
  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, sizeof (UINT32));
  }
  // u32 num_ops.
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &NumOps);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (NumOps > 16) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Reply->NumOps = NumOps;
  for (Index = 0; !EFI_ERROR (Status) && (Index < NumOps); Index++) {
    Status = CephWireSkip (&Cursor, CEPH_OSD_OP_SIZE);
  }
  // i32 retry_attempt (v3+).
  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, sizeof (UINT32));
  }
  // i32 rval per op (v4+); a nonzero per-op error becomes the reply result.
  for (Index = 0; !EFI_ERROR (Status) && (Index < NumOps); Index++) {
    Status = CephWireReadLe32 (&Cursor, (UINT32 *)&Rval);
    if (!EFI_ERROR (Status) && (Rval != 0)) {
      Reply->Result = Rval;
    }
  }

  (VOID)StructVersion;
  (VOID)Pool;
  (VOID)Ps;
  (VOID)Preferred;
  return Status;
}

EFI_STATUS
CephRpcCall (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN UINT16                       RequestType,
  IN UINT16                       ExpectedReplyType,
  IN CONST VOID                   *Front,
  IN UINTN                        FrontLength,
  IN CONST VOID                   *Data,  OPTIONAL
  IN UINTN                        DataLength,
  OUT VOID                        *ReplyFront,
  IN UINTN                        ReplyFrontCapacity,
  OUT UINTN                       *ReplyFrontLength,
  OUT VOID                        *ReplyData,  OPTIONAL
  IN UINTN                        ReplyDataCapacity,
  OUT UINTN                       *ReplyDataLength  OPTIONAL
  )
{
  EFI_STATUS          Status;
  CEPH_MSGR_V1_FRAME  Outgoing;
  CEPH_MSGR_V1_FRAME  Incoming;
  UINT8               FrameBuffer[CEPH_RPC_FRAME_MAX];
  UINT8               ReplyBuffer[CEPH_RPC_FRAME_MAX];
  UINTN               FrameLength;
  UINTN               ReplyLength;
  UINT64              RequestId;
  UINT32              Retry;

  if ((Client == NULL) || (Front == NULL) || (ReplyFront == NULL) ||
      (ReplyFrontLength == NULL) ||
      ((Data == NULL) && (DataLength != 0)))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&Outgoing, sizeof (Outgoing));
  RequestId = Client->Session.NextOutgoingSequence;
  Outgoing.Type = RequestType;
  Outgoing.Priority = 64;
  Outgoing.Version = 1;
  Outgoing.SourceType = CEPH_ENTITY_TYPE_CLIENT;
  Outgoing.SourceNumber = RequestId;
  Outgoing.CompatVersion = 1;
  Outgoing.TransactionId = RequestId;
  Outgoing.FrontLength = (UINT32)FrontLength;
  Outgoing.Front = Front;
  Outgoing.DataLength = (UINT32)DataLength;
  Outgoing.Data = Data;

  Status = CephRbdMsgrV1ClientSendTracked (
             Client,
             &Outgoing,
             FrameBuffer,
             sizeof (FrameBuffer),
             &FrameLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Retry = 0; Retry <= Client->Retransmit.MaxRetries; Retry++) {
    Status = CephRbdMsgrV1ClientReceiveMessage (
               Client,
               ReplyBuffer,
               sizeof (ReplyBuffer),
               &Incoming,
               &ReplyLength
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    // A Ceph monitor assigns its own transaction id to reply messages and does
    // not echo the request's header tid (verified against MAuthReply on Ceph
    // 22). Matching requests and replies synchronously therefore has to rely
    // on the message type only; the driver issues one RPC at a time.
    if (Incoming.Type != ExpectedReplyType) {
      // A monitor assigns its own tid to replies; one RPC is issued at a time,
      // so unrelated control messages are simply skipped here.
      continue;
    }

    if (Incoming.FrontLength > ReplyFrontCapacity) {
      return EFI_BUFFER_TOO_SMALL;
    }

    CopyMem (ReplyFront, Incoming.Front, Incoming.FrontLength);
    *ReplyFrontLength = Incoming.FrontLength;

    if ((ReplyData != NULL) || (ReplyDataLength != NULL)) {
      if (Incoming.DataLength > ReplyDataCapacity) {
        return EFI_BUFFER_TOO_SMALL;
      }

      if (Incoming.DataLength != 0) {
        CopyMem (ReplyData, Incoming.Data, Incoming.DataLength);
      }

      if (ReplyDataLength != NULL) {
        *ReplyDataLength = Incoming.DataLength;
      }
    }

    // Receiving the reply proves the request reached and was processed by the
    // peer. Release it from the retransmit queue here instead of relying on a
    // CEPH_MSGR_TAG_ACK, which OSD endpoints (lossy policy) never send. Doing
    // this on reply receipt also avoids queue exhaustion during long reads.
    CephMsgrV1RetransmitQueueAcknowledge (
      &Client->Retransmit,
      RequestId,
      NULL
      );

    return EFI_SUCCESS;
  }

  return EFI_TIMEOUT;
}

BOOLEAN
CephRpcStatusIsRecoverable (
  IN EFI_STATUS  Status
  )
{
  switch (Status) {
    case EFI_TIMEOUT:
    case EFI_CONNECTION_FIN:
    case EFI_DEVICE_ERROR:
    case EFI_NOT_READY:
      return TRUE;
    default:
      return FALSE;
  }
}

EFI_STATUS
CephRpcRunWithFailover (
  IN UINTN            EndpointCount,
  IN UINTN            MaxAttemptsPerEndpoint,
  IN VOID             *Context,
  IN CEPH_RPC_ATTEMPT  Attempt
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  LastStatus;
  UINTN       EndpointIndex;
  UINTN       AttemptIndex;

  if ((EndpointCount == 0) || (MaxAttemptsPerEndpoint == 0) ||
      (Attempt == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  LastStatus = EFI_TIMEOUT;
  for (EndpointIndex = 0; EndpointIndex < EndpointCount; EndpointIndex++) {
    for (AttemptIndex = 0; AttemptIndex < MaxAttemptsPerEndpoint;
         AttemptIndex++)
    {
      Status = Attempt (Context, EndpointIndex);
      if (!EFI_ERROR (Status)) {
        return EFI_SUCCESS;
      }

      LastStatus = Status;
      if (!CephRpcStatusIsRecoverable (Status)) {
        // A local/protocol error will not be fixed by retrying this endpoint.
        break;
      }
    }
  }

  return LastStatus;
}

STATIC
EFI_STATUS
EFIAPI
CephRpcRecoveryAttempt (
  IN VOID   *Context,
  IN UINTN  EndpointIndex
  )
{
  CEPH_RPC_RECOVERY_CONTEXT  *Recovery;
  EFI_STATUS                 Status;

  (VOID)EndpointIndex;
  if (Context == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Recovery = Context;
  Recovery->AttemptIndex++;
  if ((Recovery->AttemptIndex > 1) ||
      (Recovery->Client == NULL) ||
      (!Recovery->Client->Session.Ready))
  {
    if (Recovery->Client == NULL) {
      return EFI_INVALID_PARAMETER;
    }

    if (Recovery->Client->Session.Ready) {
      Status = CephRbdMsgrV1ClientReconnect (
                 Recovery->Config,
                 Recovery->Client
                 );
    } else {
      Status = CephRbdMsgrV1ClientConnect (
                 Recovery->Config,
                 Recovery->Client
                 );
    }

    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return CephRpcCall (
           Recovery->Client,
           Recovery->RequestType,
           Recovery->ExpectedReplyType,
           Recovery->Front,
           Recovery->FrontLength,
           Recovery->Data,
           Recovery->DataLength,
           Recovery->ReplyFront,
           Recovery->ReplyFrontCapacity,
           Recovery->ReplyFrontLength,
           Recovery->ReplyData,
           Recovery->ReplyDataCapacity,
           Recovery->ReplyDataLength
           );
}

EFI_STATUS
CephRpcCallWithRecovery (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT               *Client,
  IN     CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG *Config,
  IN     UINT16                                 RequestType,
  IN     UINT16                                 ExpectedReplyType,
  IN     CONST VOID                             *Front,
  IN     UINTN                                  FrontLength,
  IN     CONST VOID                             *Data,  OPTIONAL
  IN     UINTN                                  DataLength,
  OUT    VOID                                   *ReplyFront,
  IN     UINTN                                  ReplyFrontCapacity,
  OUT    UINTN                                  *ReplyFrontLength,
  OUT    VOID                                   *ReplyData,  OPTIONAL
  IN     UINTN                                  ReplyDataCapacity,
  OUT    UINTN                                  *ReplyDataLength  OPTIONAL
  )
{
  CEPH_RPC_RECOVERY_CONTEXT  Recovery;

  if ((Client == NULL) || (Config == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&Recovery, sizeof (Recovery));
  Recovery.Client = Client;
  Recovery.Config = Config;
  Recovery.RequestType = RequestType;
  Recovery.ExpectedReplyType = ExpectedReplyType;
  Recovery.Front = Front;
  Recovery.FrontLength = FrontLength;
  Recovery.Data = Data;
  Recovery.DataLength = DataLength;
  Recovery.ReplyFront = ReplyFront;
  Recovery.ReplyFrontCapacity = ReplyFrontCapacity;
  Recovery.ReplyFrontLength = ReplyFrontLength;
  Recovery.ReplyData = ReplyData;
  Recovery.ReplyDataCapacity = ReplyDataCapacity;
  Recovery.ReplyDataLength = ReplyDataLength;

  return CephRpcRunWithFailover (
           1,
           CEPH_RPC_MAX_RECOVERY_ATTEMPTS,
           &Recovery,
           CephRpcRecoveryAttempt
           );
}

EFI_STATUS
CephRpcCallAuto (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN UINT16                       RequestType,
  IN UINT16                       ExpectedReplyType,
  IN CONST VOID                   *Front,
  IN UINTN                        FrontLength,
  IN CONST VOID                   *Data,  OPTIONAL
  IN UINTN                        DataLength,
  OUT VOID                        *ReplyFront,
  IN UINTN                        ReplyFrontCapacity,
  OUT UINTN                       *ReplyFrontLength,
  OUT VOID                        *ReplyData,  OPTIONAL
  IN UINTN                        ReplyDataCapacity,
  OUT UINTN                       *ReplyDataLength  OPTIONAL
  )
{
  if ((Client == NULL) || (Client->RecoveryConfig == NULL)) {
    return CephRpcCall (
             Client,
             RequestType,
             ExpectedReplyType,
             Front,
             FrontLength,
             Data,
             DataLength,
             ReplyFront,
             ReplyFrontCapacity,
             ReplyFrontLength,
             ReplyData,
             ReplyDataCapacity,
             ReplyDataLength
             );
  }

  return CephRpcCallWithRecovery (
           Client,
           Client->RecoveryConfig,
           RequestType,
           ExpectedReplyType,
           Front,
           FrontLength,
           Data,
           DataLength,
           ReplyFront,
           ReplyFrontCapacity,
           ReplyFrontLength,
           ReplyData,
           ReplyDataCapacity,
           ReplyDataLength
           );
}
