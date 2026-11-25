#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "CephRbdRpc.h"
#include "CephRbdWire.h"

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

  assert(CephRpcEncodeMonCommand (
           &Header, Fsid, "osd map", 7, Buffer, sizeof (Buffer), &Length) ==
         EFI_SUCCESS);
  assert(Length == CEPH_MON_REQUEST_HEADER_SIZE + CEPH_FSID_SIZE +
         4 + 4 + 7);
  assert(CephWireInit (&Cursor, Buffer, Length) == EFI_SUCCESS);
  assert(CephWireSkip (&Cursor, CEPH_MON_REQUEST_HEADER_SIZE +
                      CEPH_FSID_SIZE) == EFI_SUCCESS);
  assert(CephWireReadLe32 (&Cursor, &NumStrings) == EFI_SUCCESS);
  assert(NumStrings == 1);
  assert(CephWireReadLe32 (&Cursor, &StringLength) == EFI_SUCCESS);
  assert(StringLength == 7);
  assert(memcmp (Cursor.Data, "osd map", 7) == 0);

  // MMonCommandAck: version u64, result s32, status string, cmd vector.
  WriteLe64 (Buffer, 42);
  WriteLe32 (Buffer + 8, 0);
  WriteLe32 (Buffer + 12, 2);
  memcpy (Buffer + 16, "ok", 2);
  WriteLe32 (Buffer + 18, 0);
  assert(CephRpcDecodeMonCommandAck (
           Buffer, 22, &Result, &OutData, &OutDataLength) == EFI_SUCCESS);
  assert(Result == 0);
  assert(OutDataLength == 0);

  WriteLe32 (Buffer + 8, (UINT32)-22);
  assert(CephRpcDecodeMonCommandAck (
           Buffer, 22, &Result, NULL, NULL) == EFI_SUCCESS);
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
           &Op, 1, Front, sizeof (Front), &Length) == EFI_SUCCESS);
  assert(Length > CEPH_OSD_OP_SIZE);

  // Verify the request header fields are encoded in order.
  assert(CephWireInit (&Cursor, Front, Length) == EFI_SUCCESS);
  assert(CephWireReadLe32 (&Cursor, &Value) == EFI_SUCCESS);   // client_inc
  assert(Value == 1);
  assert(CephWireReadLe32 (&Cursor, &Value) == EFI_SUCCESS);   // osdmap epoch
  assert(Value == 42);
  assert(CephWireReadLe32 (&Cursor, &Value) == EFI_SUCCESS);   // flags
  assert(Value == CEPH_OSD_FLAG_READ);
  assert(CephWireSkip (&Cursor, 4 + 4 + 4 + 8 + 8) == EFI_SUCCESS);
  assert(CephWireReadLe32 (&Cursor, &Value) == EFI_SUCCESS);   // key length
  assert(Value == 0);
  assert(CephWireSkip (&Cursor, 4 + 4) == EFI_SUCCESS);        // nspace, hash
  assert(CephWireReadLe32 (&Cursor, &Value) == EFI_SUCCESS);   // oid length
  assert(Value == 29);
  assert(CephWireSkip (&Cursor, 29) == EFI_SUCCESS);
  assert(CephWireReadLe16 (&Cursor, &NumOps) == EFI_SUCCESS);
  assert(NumOps == 1);
  assert(CephWireReadLe16 (&Cursor, (UINT16 *)&Value) == EFI_SUCCESS);
  assert(Value == CEPH_OSD_OP_READ);

  // Build an OSD reply front and decode result and op return values.
  memset (ReplyFront, 0, sizeof (ReplyFront));
  WriteLe32 (ReplyFront, 4);                  // oid name length
  memcpy (ReplyFront + 4, "data", 4);         // oid name
  // pgid(8) + flags(8) then result at offset 24.
  WriteLe32 (ReplyFront + 24, 0);             // result
  WriteLe32 (ReplyFront + 44, 1);             // num_ops
  WriteLe32 (ReplyFront + 48 + CEPH_OSD_OP_SIZE, 0);  // op rval

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
  assert(CephRpcEncodeOsdOp (
           3, 1, 42, 8, "rbd_header.abc", 14,
           &Op, 1, Front, sizeof (Front), &Length) == EFI_SUCCESS);
  assert(CephWireInit (&Cursor, Front, Length) == EFI_SUCCESS);
  // 72 bytes of header through num_ops, then op u16 + flags u32.
  assert(CephWireSkip (&Cursor, 72 + 2 + 4) == EFI_SUCCESS);
  assert(CephWireReadU8 (&Cursor, &ClassLen) == EFI_SUCCESS);
  assert(ClassLen == 3);
}

VOID
TestRpcCodecs (VOID)
{
  TestMonCodecs ();
  TestOsdMapEpoch ();
  TestOsdOpCodecs ();
  puts("RPC codec tests: passed");
}
