#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "CephRbdRpc.h"
#include "CephRbdWire.h"

static void
WriteLe16 (UINT8 *Buffer, UINT16 Value)
{
  Buffer[0] = (UINT8)Value;
  Buffer[1] = (UINT8)(Value >> 8);
}

static void
WriteLe32 (UINT8 *Buffer, UINT32 Value)
{
  Buffer[0] = (UINT8)Value;
  Buffer[1] = (UINT8)(Value >> 8);
  Buffer[2] = (UINT8)(Value >> 16);
  Buffer[3] = (UINT8)(Value >> 24);
}

static void
WriteLe64 (UINT8 *Buffer, UINT64 Value)
{
  WriteLe32 (Buffer, (UINT32)Value);
  WriteLe32 (Buffer + 4, (UINT32)(Value >> 32));
}

static void
TestMonCodecs (VOID)
{
  CEPH_MON_REQUEST_HEADER Header = { 0, 3, 0x1122334455667788ULL };
  UINT8 Fsid[CEPH_FSID_SIZE];
  UINT8 Buffer[256];
  UINTN Length;
  CEPH_WIRE_CURSOR Cursor;
  UINT64 HaveVersion;
  UINT16 SessionMon;
  UINT64 SessionMonTid;
  UINT32 StartEpoch;
  UINT32 NumStrings;
  UINT32 StringLength;
  INT32 Result;
  CONST UINT8 *OutData;
  UINTN OutDataLength;

  memset (Fsid, 0x5a, sizeof (Fsid));
  assert(CephRpcEncodeMonGetMap (
           &Header, Fsid, 12, Buffer, sizeof (Buffer), &Length) ==
         EFI_SUCCESS);
  assert(Length == CEPH_MON_REQUEST_HEADER_SIZE + CEPH_FSID_SIZE +
         sizeof (UINT32));
  assert(CephWireInit (&Cursor, Buffer, Length) == EFI_SUCCESS);
  assert(CephWireReadLe64 (&Cursor, &HaveVersion) == EFI_SUCCESS);
  assert(HaveVersion == 0);
  assert(CephWireReadLe16 (&Cursor, &SessionMon) == EFI_SUCCESS);
  assert(SessionMon == 3);
  assert(CephWireReadLe64 (&Cursor, &SessionMonTid) == EFI_SUCCESS);
  assert(SessionMonTid == 0x1122334455667788ULL);
  assert(CephWireSkip (&Cursor, CEPH_FSID_SIZE) == EFI_SUCCESS);
  assert(CephWireReadLe32 (&Cursor, &StartEpoch) == EFI_SUCCESS);
  assert(StartEpoch == 12);

  // Regression: Ceph 22 PaxosServiceMessage::paxos_encode() writes a u64
  // version, a __s16 (not u32) session_mon and a u64 tid. A 20-byte header
  // shifts the fsid/command vector and makes the monitor drop the connection
  // with "failed to decode message of type 50 ... End of buffer".
  assert(CEPH_PAXOS_REQUEST_HEADER_SIZE == 18);
  assert(CephRpcEncodeMonCommand (
           &Header, Fsid, "osd map", 7, Buffer, sizeof (Buffer), &Length) ==
         EFI_SUCCESS);
  assert(Length == CEPH_PAXOS_REQUEST_HEADER_SIZE + CEPH_FSID_SIZE +
         4 + 4 + 7);
  assert(CephWireInit (&Cursor, Buffer, Length) == EFI_SUCCESS);
  assert(CephWireReadLe64 (&Cursor, &HaveVersion) == EFI_SUCCESS);
  assert(HaveVersion == 0);
  assert(CephWireReadLe16 (&Cursor, &SessionMon) == EFI_SUCCESS);
  assert(SessionMon == 3);
  assert(CephWireReadLe64 (&Cursor, &SessionMonTid) == EFI_SUCCESS);
  assert(SessionMonTid == 0x1122334455667788ULL);
  assert(CephWireSkip (&Cursor, CEPH_FSID_SIZE) == EFI_SUCCESS);
  assert(CephWireReadLe32 (&Cursor, &NumStrings) == EFI_SUCCESS);
  assert(NumStrings == 1);
  assert(CephWireReadLe32 (&Cursor, &StringLength) == EFI_SUCCESS);
  assert(StringLength == 7);
  assert(memcmp (Cursor.Data, "osd map", 7) == 0);

  // MMonCommandAck: paxos header (u64, __s16, u64), result s32, status
  // string, cmd vector.
  WriteLe64 (Buffer, 42);
  WriteLe16 (Buffer + 8, 0);
  WriteLe64 (Buffer + 10, 0);
  WriteLe32 (Buffer + 18, 0);
  WriteLe32 (Buffer + 22, 2);
  memcpy (Buffer + 26, "ok", 2);
  WriteLe32 (Buffer + 28, 0);
  assert(CephRpcDecodeMonCommandAck (
           Buffer, 32, &Result, &OutData, &OutDataLength) == EFI_SUCCESS);
  assert(Result == 0);
  assert(OutDataLength == 0);

  WriteLe32 (Buffer + 18, (UINT32)-22);
  assert(CephRpcDecodeMonCommandAck (
           Buffer, 32, &Result, NULL, NULL) == EFI_SUCCESS);
  assert(Result == -22);
}

static void
TestOsdMapEpoch (VOID)
{
  UINT8 Buffer[32] = { 0 };
  UINT64 Epoch;

  // Two ENCODE_START headers (6 bytes each), fsid (16 bytes), then epoch.
  Buffer[0] = 8;
  Buffer[1] = 7;
  WriteLe32 (Buffer + 2, 26);
  Buffer[6] = 10;
  Buffer[7] = 1;
  WriteLe32 (Buffer + 8, 20);
  WriteLe32 (Buffer + 28, 1234);
  assert(CephRpcDecodeOsdMapEpoch (Buffer, sizeof (Buffer), &Epoch) ==
         EFI_SUCCESS);
  assert(Epoch == 1234);

  Buffer[0] = 0;
  assert(CephRpcDecodeOsdMapEpoch (Buffer, sizeof (Buffer), &Epoch) ==
         EFI_UNSUPPORTED);
  Buffer[0] = 8;
  WriteLe32 (Buffer + 28, (UINT32)-1);
  assert(CephRpcDecodeOsdMapEpoch (Buffer, sizeof (Buffer), &Epoch) ==
         EFI_COMPROMISED_DATA);
  WriteLe32 (Buffer + 2, 4);
  WriteLe32 (Buffer + 28, 1234);
  assert(CephRpcDecodeOsdMapEpoch (Buffer, sizeof (Buffer), &Epoch) ==
         EFI_BAD_BUFFER_SIZE);
}

static void
TestOsdOpCodecs (VOID)
{
  UINT8 Front[2048];
  UINTN Length;
  CEPH_WIRE_CURSOR Cursor;
  UINT32 Value;
  UINT16 NumOps;
  CEPH_OSD_OP_REQUEST Op;
  UINT8 ReplyFront[512];
  UINT8 ReplyData[16];
  CEPH_OSD_OP_REPLY Reply;
  UINT8 ClassLen;

  memset (&Op, 0, sizeof (Op));
  Op.Op = CEPH_OSD_OP_READ;
  Op.Flags = CEPH_OSD_FLAG_READ;
  Op.Offset = 4096;
  Op.Length = 1024;

  assert(CephRpcEncodeOsdOp (
           3, 1, 42, 7, "rbd_data.abc.0000000000000000", 29,
           &Op, 1, Front, sizeof (Front), &Length,
           NULL, 0, NULL) == EFI_SUCCESS);
  assert(Length > CEPH_OSD_OP_SIZE);

  // Verify the legacy v1 MOSDOp header fields are encoded in order:
  // client_inc, old_pg_t{preferred,ps,pool}, su, osdmap_epoch, flags,
  // mtime, reassert, oid_len, snapid, snap_seq, num_snaps, num_ops,
  // ops[], oid bytes.
  assert(CephWireInit (&Cursor, Front, Length) == EFI_SUCCESS);
  assert(CephWireReadLe32 (&Cursor, &Value) == EFI_SUCCESS);   // client_inc
  assert(Value == 1);
  {
    UINT16 Preferred;
    UINT16 Ps;

    assert(CephWireReadLe16 (&Cursor, &Preferred) == EFI_SUCCESS);
    assert(Preferred == 0xffff);                 // -1
    assert(CephWireReadLe16 (&Cursor, &Ps) == EFI_SUCCESS);
    assert(Ps == 0);
    assert(CephWireReadLe32 (&Cursor, &Value) == EFI_SUCCESS); // pool
    assert(Value == 3);
  }
  assert(CephWireSkip (&Cursor, 4) == EFI_SUCCESS);           // su
  assert(CephWireReadLe32 (&Cursor, &Value) == EFI_SUCCESS);   // osdmap epoch
  assert(Value == 42);
  assert(CephWireReadLe32 (&Cursor, &Value) == EFI_SUCCESS);   // flags
  assert(Value == CEPH_OSD_FLAG_READ);
  assert(CephWireSkip (&Cursor, 4 + 4 + 4 + 8) == EFI_SUCCESS); // mtime,reassert
  assert(CephWireReadLe32 (&Cursor, &Value) == EFI_SUCCESS);   // oid length
  assert(Value == 29);
  assert(CephWireSkip (&Cursor, 8 + 8 + 4) == EFI_SUCCESS);    // snap,snapseq,n
  assert(CephWireReadLe16 (&Cursor, &NumOps) == EFI_SUCCESS);
  assert(NumOps == 1);
  assert(CephWireReadLe16 (&Cursor, (UINT16 *)&Value) == EFI_SUCCESS);
  assert(Value == CEPH_OSD_OP_READ);

  // Build an OSD reply front in the Ceph 22 HEAD_VERSION (v8) layout and
  // decode result and op return values. Layout (verified against Ceph 22):
  //   oid: u32 len + bytes
  //   pg_t: u8 v + u64 pool + u32 ps + i32 preferred
  //   s64 flags, i32 result
  //   eversion_t: u64 version + u32 epoch
  //   u32 osdmap_epoch, u32 num_ops, ceph_osd_op ops[], i32 retry_attempt,
  //   i32 rval[] (one per op).
  {
    UINTN  Offset;

    memset (ReplyFront, 0, sizeof (ReplyFront));
    Offset = 0;
    WriteLe32 (ReplyFront + Offset, 4);        // oid name length
    Offset += 4;
    memcpy (ReplyFront + Offset, "data", 4);   // oid name
    Offset += 4;
    ReplyFront[Offset++] = 1;                  // pg_t struct_v
    WriteLe64 (ReplyFront + Offset, 3); Offset += 8;   // pool
    WriteLe32 (ReplyFront + Offset, 6); Offset += 4;   // ps
    WriteLe32 (ReplyFront + Offset, 0xffffffff); Offset += 4; // preferred (-1)
    WriteLe64 (ReplyFront + Offset, 0); Offset += 8;   // flags (s64)
    WriteLe32 (ReplyFront + Offset, 0); Offset += 4;   // result
    WriteLe64 (ReplyFront + Offset, 0); Offset += 8;   // bad_replay version
    WriteLe32 (ReplyFront + Offset, 0); Offset += 4;   // bad_replay epoch
    WriteLe32 (ReplyFront + Offset, 42); Offset += 4;  // osdmap epoch
    WriteLe32 (ReplyFront + Offset, 1); Offset += 4;   // num_ops
    Offset += CEPH_OSD_OP_SIZE;                // op
    WriteLe32 (ReplyFront + Offset, 0); Offset += 4;   // retry_attempt
    WriteLe32 (ReplyFront + Offset, 0); Offset += 4;   // op rval
  }

  assert(CephRpcDecodeOsdOpReply (
           ReplyFront, sizeof (ReplyFront), ReplyData, sizeof (ReplyData),
           &Reply) == EFI_SUCCESS);
  assert(Reply.Result == 0);
  assert(Reply.NumOps == 1);
  assert(Reply.DataLength == sizeof (ReplyData));

  // CLS call op encoding path.
  memset (&Op, 0, sizeof (Op));
  Op.Op = CEPH_OSD_OP_CALL;
  Op.Flags = CEPH_OSD_FLAG_READ;
  strcpy ((char *)Op.ClassName, "rbd");
  Op.ClassNameLength = 3;
  strcpy ((char *)Op.MethodName, "get_header");
  Op.MethodNameLength = 10;
  Op.Indata = (CONST UINT8 *)"input";
  Op.IndataLength = 5;
  {
    UINT8 CallData[128];
    UINTN CallDataLength;

    assert(CephRpcEncodeOsdOp (
             3, 1, 42, 8, "rbd_header.abc", 14,
             &Op, 1, Front, sizeof (Front), &Length,
             CallData, sizeof (CallData), &CallDataLength) == EFI_SUCCESS);
    assert(CephWireInit (&Cursor, Front, Length) == EFI_SUCCESS);
    // 70 bytes of header through num_ops, then op u16 + flags u32.
    assert(CephWireSkip (&Cursor, 70 + 2 + 4) == EFI_SUCCESS);
    assert(CephWireReadU8 (&Cursor, &ClassLen) == EFI_SUCCESS);
    assert(ClassLen == 3);

    // Ceph carries class name, method name and in-data in the message data
    // buffer (concatenated), with the op's payload_len declaring the length.
    assert(CallDataLength == 3 + 10 + 5);
    assert(memcmp (CallData, "rbd", 3) == 0);
    assert(memcmp (CallData + 3, "get_header", 10) == 0);
    assert(memcmp (CallData + 13, "input", 5) == 0);
    // payload_len is the last le32 of the 38-byte ceph_osd_op. 70 bytes of
    // header + num_ops precede the op, payload_len sits at 70 + 34.
    {
      UINT32 PayloadLen;

      assert(CephWireInit (&Cursor, Front, Length) == EFI_SUCCESS);
      assert(CephWireSkip (&Cursor, 70 + 34) == EFI_SUCCESS);
      assert(CephWireReadLe32 (&Cursor, &PayloadLen) == EFI_SUCCESS);
      assert(PayloadLen == CallDataLength);
    }
    // cls.indata_len sits at op+9 (u16 op, u32 flags, u8 class_len,
    // u8 method_len, u8 argc). Ceph uses it to copy the caller's indata
    // out of the payload slice, so it must be 5 here -- not the sum of
    // class_len + method_len + indata_len (18).
    {
      UINT32 ClsIndataLen;

      assert(CephWireInit (&Cursor, Front, Length) == EFI_SUCCESS);
      assert(CephWireSkip (&Cursor, 70 + 9) == EFI_SUCCESS);
      assert(CephWireReadLe32 (&Cursor, &ClsIndataLen) == EFI_SUCCESS);
      assert(ClsIndataLen == 5);
    }
  }
}

typedef struct {
  EFI_STATUS  Status[4][4];
  UINTN       Calls[4];
} FAKE_FAILOVER;

static EFI_STATUS EFIAPI
FakeFailoverAttempt (
  IN VOID   *Context,
  IN UINTN  EndpointIndex
  )
{
  FAKE_FAILOVER  *Fake;
  EFI_STATUS      Status;
  UINTN           Call;

  if ((Context == NULL) || (EndpointIndex >= 4)) {
    return EFI_INVALID_PARAMETER;
  }

  Fake = Context;
  Call = Fake->Calls[EndpointIndex]++;
  Status = (Call < 4) ? Fake->Status[EndpointIndex][Call] : EFI_SUCCESS;
  return Status;
}

static void
TestMonMapEpoch (VOID)
{
  UINT8   Buffer[64] = { 0 };
  UINT64  EpochHolder = 0;

  // MMonMap front: u32 blob length, ENCODE_START(10,6) = {10,6,u32 len},
  // fsid (16 bytes), then signed epoch (le32).
  WriteLe32 (Buffer, 26);        // blob length
  Buffer[4] = 10;                // struct_v
  Buffer[5] = 6;                 // struct_compat
  WriteLe32 (Buffer + 6, 20);    // struct length (fsid + epoch)
  memset (Buffer + 10, 0x5a, 16);// fsid
  WriteLe32 (Buffer + 26, 451);  // epoch

  assert(CephRpcDecodeMonMapEpoch (Buffer, 30, NULL) == EFI_INVALID_PARAMETER);
  assert(CephRpcDecodeMonMapEpoch (Buffer, 30, &EpochHolder) == EFI_SUCCESS);
  assert(EpochHolder == 451);

  EpochHolder = 0;
  Buffer[4] = 0;
  assert(CephRpcDecodeMonMapEpoch (Buffer, 30, &EpochHolder) == EFI_UNSUPPORTED);
  Buffer[4] = 10;
  WriteLe32 (Buffer, 100);       // blob length exceeds buffer
  assert(CephRpcDecodeMonMapEpoch (Buffer, 30, &EpochHolder) == EFI_BAD_BUFFER_SIZE);
}

static void
TestRpcFailover (VOID)
{
  FAKE_FAILOVER  Fake;

  assert(CephRpcStatusIsRecoverable (EFI_TIMEOUT) == TRUE);
  assert(CephRpcStatusIsRecoverable (EFI_CONNECTION_FIN) == TRUE);
  assert(CephRpcStatusIsRecoverable (EFI_DEVICE_ERROR) == TRUE);
  assert(CephRpcStatusIsRecoverable (EFI_NOT_READY) == TRUE);
  assert(CephRpcStatusIsRecoverable (EFI_ACCESS_DENIED) == FALSE);
  assert(CephRpcStatusIsRecoverable (EFI_INVALID_PARAMETER) == FALSE);

  memset (&Fake, 0, sizeof (Fake));
  Fake.Status[0][0] = EFI_TIMEOUT;
  Fake.Status[0][1] = EFI_DEVICE_ERROR;
  assert(CephRpcRunWithFailover (
           1, 3, &Fake, FakeFailoverAttempt) == EFI_SUCCESS);
  assert(Fake.Calls[0] == 3);

  // A hard error on the first monitor must not block the second one.
  memset (&Fake, 0, sizeof (Fake));
  Fake.Status[0][0] = EFI_ACCESS_DENIED;
  assert(CephRpcRunWithFailover (
           2, 2, &Fake, FakeFailoverAttempt) == EFI_SUCCESS);
  assert(Fake.Calls[0] == 1);
  assert(Fake.Calls[1] == 1);

  // Exhausting every endpoint returns the last observed error.
  memset (&Fake, 0, sizeof (Fake));
  Fake.Status[0][0] = EFI_TIMEOUT;
  Fake.Status[0][1] = EFI_TIMEOUT;
  Fake.Status[1][0] = EFI_DEVICE_ERROR;
  Fake.Status[1][1] = EFI_TIMEOUT;
  assert(CephRpcRunWithFailover (
           2, 2, &Fake, FakeFailoverAttempt) == EFI_TIMEOUT);
  assert(Fake.Calls[0] == 2);
  assert(Fake.Calls[1] == 2);

  assert(CephRpcRunWithFailover (
           0, 2, &Fake, FakeFailoverAttempt) == EFI_INVALID_PARAMETER);
  assert(CephRpcRunWithFailover (
           1, 0, &Fake, FakeFailoverAttempt) == EFI_INVALID_PARAMETER);
  assert(CephRpcRunWithFailover (
           1, 2, &Fake, NULL) == EFI_INVALID_PARAMETER);
}

VOID
TestRpcCodecs (VOID)
{
  TestMonCodecs ();
  TestOsdMapEpoch ();
  TestMonMapEpoch ();
  TestOsdOpCodecs ();
  TestRpcFailover ();
  puts("RPC codec tests: passed");
}
