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

  RequiredLength = CEPH_MON_REQUEST_HEADER_SIZE + CEPH_FSID_SIZE +
                   sizeof (UINT32) + sizeof (UINT32) + CommandLength;
  *EncodedLength = 0;
  if (BufferLength < RequiredLength) {
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

  // MMonCommandAck::decode_payload: paxos_decode (version u64),
  // result (int32), rs (string), cmd (vector<string>).
  Status = CephWireSkip (&Cursor, sizeof (UINT64));
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

STATIC
EFI_STATUS
CephRpcWriteOsdOp (
  IN OUT CEPH_WIRE_WRITER       *Writer,
  IN CONST CEPH_OSD_OP_REQUEST  *Op
  )
{
  EFI_STATUS  Status;

  Status = CephWireWriteLe16 (Writer, Op->Op);
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (Writer, Op->Flags);
  }

  if (!EFI_ERROR (Status)) {
    if (Op->Op == CEPH_OSD_OP_CALL) {
      Status = CephWireWriteU8 (Writer, (UINT8)Op->ClassNameLength);
      if (!EFI_ERROR (Status)) {
        Status = CephWireWriteU8 (Writer, (UINT8)Op->MethodNameLength);
      }

      if (!EFI_ERROR (Status)) {
        Status = CephWireWriteU8 (Writer, 0);  // argc
      }

      if (!EFI_ERROR (Status)) {
        Status = CephWireWriteLe32 (
                   Writer,
                   (UINT32)Op->IndataLength
                   );
      }
    } else {
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
    Status = CephWireWriteLe32 (Writer, 0);  // payload_len
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
  OUT UINTN                         *EncodedLength
  )
{
  CEPH_WIRE_WRITER  Writer;
  EFI_STATUS        Status;
  UINTN             Index;
  UINTN             RequiredLength;
  UINT32            StripeUnit;

  if ((ObjectName == NULL) || (ObjectNameLength == 0) ||
      (ObjectNameLength > CEPH_RPC_MAX_COMMAND) || (Ops == NULL) ||
      (OpCount == 0) || (OpCount > 16) || (Buffer == NULL) ||
      (EncodedLength == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  // Fixed fields: client_inc, osdmap_epoch, flags, mtime(2), reassert(2),
  // locator pool/key/nspace/hash, oid length, oid, num_ops, snapid, snap_seq,
  // num_snaps, retry_attempt, features(2), reqid inc/tid.
  RequiredLength = sizeof (UINT32) * 3 + sizeof (UINT32) * 2 +
                   sizeof (UINT32) * 2 + sizeof (UINT64) * 2 +
                   sizeof (UINT32) * 4 + sizeof (UINT32) +
                   ObjectNameLength + sizeof (UINT16) +
                   sizeof (UINT64) * 2 + sizeof (UINT32) * 2 +
                   sizeof (UINT32) * 2 + sizeof (UINT32) + sizeof (UINT64) +
                   sizeof (UINT32);
  for (Index = 0; Index < OpCount; Index++) {
    RequiredLength += CEPH_OSD_OP_SIZE;
    if (Ops[Index].Op == CEPH_OSD_OP_CALL) {
      RequiredLength += Ops[Index].ClassNameLength +
                        Ops[Index].MethodNameLength +
                        Ops[Index].IndataLength;
    }
  }
  *EncodedLength = 0;
  if (BufferLength < RequiredLength) {
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = CephWireWriterInit (&Writer, Buffer, BufferLength);
  if (!EFI_ERROR (Status)) {
    // MOSDOp v6 encoding: client_inc, osdmap_epoch, flags, mtime,
    // reassert_version, object_locator, oid, num_ops, ops, snapid, snap_seq,
    // snaps, retry_attempt, features.
    Status = CephWireWriteLe32 (&Writer, ClientIncarnation);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, OsdmapEpoch);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, CEPH_OSD_FLAG_READ);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // mtime seconds
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // mtime nanoseconds
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // reassert epoch
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, 0);  // reassert version
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, PoolId);  // object locator pool
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // locator key length
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // locator nspace length
  }

  if (!EFI_ERROR (Status)) {
    StripeUnit = 0;
    Status = CephWireWriteLe32 (&Writer, StripeUnit);  // hash
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, (UINT32)ObjectNameLength);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteBytes (&Writer, ObjectName, ObjectNameLength);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe16 (&Writer, (UINT16)OpCount);
  }

  for (Index = 0; !EFI_ERROR (Status) && (Index < OpCount); Index++) {
    Status = CephRpcWriteOsdOp (&Writer, &Ops[Index]);
    if (!EFI_ERROR (Status) &&
        (Ops[Index].ClassNameLength != 0) &&
        (Ops[Index].MethodNameLength != 0))
    {
      Status = CephWireWriteBytes (
                 &Writer,
                 Ops[Index].ClassName,
                 Ops[Index].ClassNameLength
                 );
      if (!EFI_ERROR (Status)) {
        Status = CephWireWriteBytes (
                   &Writer,
                   Ops[Index].MethodName,
                   Ops[Index].MethodNameLength
                   );
      }

      if (!EFI_ERROR (Status) && (Ops[Index].IndataLength != 0)) {
        Status = CephWireWriteBytes (
                   &Writer,
                   Ops[Index].Indata,
                   Ops[Index].IndataLength
                   );
      }
    }
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, 0);  // snapid
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, 0);  // snap_seq
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // num_snaps
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // retry_attempt
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // features low
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // features high
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, (UINT32)ClientIncarnation);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, RequestId);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // reqid inc
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EncodedLength = Writer.Written;
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
  UINT8             StructVersion;
  UINT32            StringLength;
  UINT32            RawResult;
  UINT32            NumOps;
  UINT32            Index;
  INT32             Rval;

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

  // MOSDOpReply v6+: oid (string), pgid (u64), flags (s64), result (s32),
  // bad_replay_version (u32 u64), osdmap_epoch (u32), num_ops (u32).
  Status = CephWireReadLe32 (&Cursor, &StringLength);
  if (!EFI_ERROR (Status) && (StringLength > Cursor.Remaining)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, StringLength);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, sizeof (UINT64));  // pgid
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, sizeof (UINT64));  // flags
  }

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

  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, sizeof (UINT32));  // bad replay epoch
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, sizeof (UINT64));  // bad replay version
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, sizeof (UINT32));  // osdmap epoch
  }

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
  for (Index = 0; Index < NumOps; Index++) {
    Status = CephWireSkip (&Cursor, CEPH_OSD_OP_SIZE);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  // Each op carries an s32 return value in v4+ replies.
  for (Index = 0; !EFI_ERROR (Status) && (Index < NumOps); Index++) {
    Status = CephWireReadLe32 (&Cursor, (UINT32 *)&Rval);
    if (!EFI_ERROR (Status) && (Rval != 0)) {
      Reply->Result = Rval;
    }
  }

  (VOID)StructVersion;
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
  UINT8               FrameBuffer[CEPH_MSGR_V1_RETRANSMIT_FRAME_MAX];
  UINT8               ReplyBuffer[CEPH_MSGR_V1_RETRANSMIT_FRAME_MAX];
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

    if ((Incoming.TransactionId != RequestId) ||
        (Incoming.Type != ExpectedReplyType))
    {
      // Skip replies to other transactions (e.g. delayed OSD map updates).
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
