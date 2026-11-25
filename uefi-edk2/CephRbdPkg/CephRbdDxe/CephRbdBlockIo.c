#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include "CephRbdBlockIo.h"
#include "CephRbdWire.h"

// CEPH_NOSNAP is (snapid_t)-2, not -1 (which is CEPH_SNAPDIR) in
// include/rados.h. snapid_t is an unsigned 64-bit wire value.
#define CEPH_RBD_SNAP_ID                 (MAX_UINT64 - 1)
#define CEPH_RBD_ID_OBJECT_PREFIX        "rbd_id."
#define CEPH_RBD_HEADER_OBJECT_PREFIX    "rbd_header."
#define CEPH_RBD_INCOMPATIBLE_FEATURES   (1ULL << 63)

STATIC
UINTN
CephRbdAsciiStrLen (
  IN CONST CHAR8  *String,
  IN UINTN        MaxLength
  )
{
  UINTN  Length;

  Length = 0;
  while ((Length < MaxLength) && (String[Length] != '\0')) {
    Length++;
  }

  return Length;
}

// A full OSDMap is larger than the per-call stack budget, so use static
// buffers; only one firmware driver instance is active at a time.
STATIC UINT8 mCephRbdOsdMapReply[CEPH_RPC_FRAME_MAX];
STATIC UINT8 mCephRbdOsdMapData[CEPH_RPC_FRAME_MAX];

// MON_GET_MAP returns a MonMap, not an OSDMap (verified on Ceph 22). The
// "osd getmap" monitor command returns the canonical OSDMap blob as reply
// data, which is exactly what CephRbdPlacementFromOsdMap decodes.
#define CEPH_RBD_GET_OSDMAP_COMMAND  "{\"prefix\":\"osd getmap\"}"

STATIC
EFI_STATUS
CephRbdBlockIoGetOsdMap (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN CONST UINT8                  *Fsid,
  IN INT32                        PoolId,
  OUT UINT64                      *Epoch,
  OUT CEPH_RBD_PLACEMENT          *Placement
  )
{
  EFI_STATUS              Status;
  CEPH_MON_REQUEST_HEADER Header;
  UINT8                   Request[CEPH_PAXOS_REQUEST_HEADER_SIZE +
                                  CEPH_FSID_SIZE + sizeof (UINT32) * 2 +
                                  sizeof (CEPH_RBD_GET_OSDMAP_COMMAND)];
  UINTN                   RequestLength;
  UINTN                   ReplyLength;
  UINTN                   DataLength;
  INT32                   Result;

  Header.HaveVersion = 0;
  Header.SessionMon = 0;
  Header.SessionMonTid = 0;

  Status = CephRpcEncodeMonCommand (
             &Header,
             Fsid,
             CEPH_RBD_GET_OSDMAP_COMMAND,
             sizeof (CEPH_RBD_GET_OSDMAP_COMMAND) - 1,
             Request,
             sizeof (Request),
             &RequestLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRpcCallAuto (
             Client,
             CEPH_MSG_MON_COMMAND,
             CEPH_MSG_MON_COMMAND_ACK,
             Request,
             RequestLength,
             NULL,
             0,
             mCephRbdOsdMapReply,
             sizeof (mCephRbdOsdMapReply),
             &ReplyLength,
             mCephRbdOsdMapData,
             sizeof (mCephRbdOsdMapData),
             &DataLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRpcDecodeMonCommandAck (
             mCephRbdOsdMapReply, ReplyLength, &Result, NULL, NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Result != 0) {
    return EFI_DEVICE_ERROR;
  }

  Status = CephRbdPlacementFromOsdMap (
             mCephRbdOsdMapData, DataLength, PoolId, Placement
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Epoch = Placement->Epoch;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdBlockIoDecodeHeader (
  IN CONST UINT8                 *Payload,
  IN UINTN                       PayloadLength,
  OUT CEPH_RBD_FORMAT2_METADATA  *Metadata
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT8             ObjectOrder;
  UINT64            ImageSize;

  if ((Payload == NULL) || (Metadata == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  // cls_client::get_size_finish() decodes order (u8) before size (u64);
  // there is no struct version wrapper on this reply.
  Status = CephWireInit (&Cursor, Payload, PayloadLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireReadU8 (&Cursor, &ObjectOrder);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe64 (&Cursor, &ImageSize);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((ObjectOrder == 0) || (ObjectOrder > CEPH_RBD_MAX_OBJECT_ORDER) ||
      (ImageSize == 0))
  {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (Metadata, sizeof (*Metadata));
  Metadata->ImageSize = ImageSize;
  Metadata->ObjectOrder = ObjectOrder;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CephRbdBlockIoDecodeU64Pair (
  IN CONST UINT8  *Payload,
  IN UINTN        PayloadLength,
  OUT UINT64      *First,
  OUT UINT64      *Second
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;

  Status = CephWireInit (&Cursor, Payload, PayloadLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireReadLe64 (&Cursor, First);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe64 (&Cursor, Second);
  }

  return Status;
}

STATIC
EFI_STATUS
CephRbdBlockIoDecodeString (
  IN CONST UINT8  *Payload,
  IN UINTN        PayloadLength,
  OUT CHAR8       *Buffer,
  IN UINTN        BufferCapacity
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT32            Length;

  Status = CephWireInit (&Cursor, Payload, PayloadLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireReadLe32 (&Cursor, &Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Length == 0) || (Length >= BufferCapacity)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = CephWireReadBytes (&Cursor, Buffer, Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Buffer[Length] = '\0';
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CephRbdBlockIoCallClassMethod (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN UINT32                       PoolId,
  IN UINT32                       OsdEpoch,
  IN CONST CHAR8                  *ObjectName,
  IN UINTN                        ObjectNameLength,
  IN CONST CHAR8                  *ClassName,
  IN CONST CHAR8                  *MethodName,
  IN CONST VOID                   *Indata  OPTIONAL,
  IN UINTN                        IndataLength,
  OUT UINT8                       *OutData,
  IN UINTN                        OutDataCapacity,
  OUT UINTN                       *OutDataLength
  )
{
  EFI_STATUS          Status;
  CEPH_OSD_OP_REQUEST Op;
  UINT8               Request[CEPH_RPC_MAX_METADATA];
  UINT8               ReplyFront[CEPH_RPC_MAX_METADATA];
  UINT8               ReplyData[CEPH_RPC_MAX_METADATA];
  UINTN               RequestLength;
  UINTN               ReplyFrontLength;
  UINTN               ReplyDataLength;
  UINTN               ClassNameLength;
  UINTN               MethodNameLength;
  CEPH_OSD_OP_REPLY   Reply;

  if ((Client == NULL) || (ObjectName == NULL) ||
      (ObjectNameLength == 0) || (ClassName == NULL) ||
      (MethodName == NULL) || (OutData == NULL) ||
      (OutDataLength == NULL) ||
      ((Indata == NULL) && (IndataLength != 0)))
  {
    return EFI_INVALID_PARAMETER;
  }

  ClassNameLength = 0;
  while ((ClassNameLength < CEPH_RPC_MAX_CLASS) &&
         (ClassName[ClassNameLength] != '\0'))
  {
    ClassNameLength++;
  }

  MethodNameLength = 0;
  while ((MethodNameLength < CEPH_RPC_MAX_METHOD) &&
         (MethodName[MethodNameLength] != '\0'))
  {
    MethodNameLength++;
  }

  if ((ClassNameLength == 0) || (ClassNameLength == CEPH_RPC_MAX_CLASS) ||
      (MethodNameLength == 0) || (MethodNameLength == CEPH_RPC_MAX_METHOD) ||
      (IndataLength > CEPH_RPC_MAX_INDATA))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&Op, sizeof (Op));
  Op.Op = CEPH_OSD_OP_CALL;
  Op.Flags = CEPH_OSD_FLAG_READ;
  CopyMem (Op.ClassName, ClassName, ClassNameLength);
  Op.ClassNameLength = ClassNameLength;
  CopyMem (Op.MethodName, MethodName, MethodNameLength);
  Op.MethodNameLength = MethodNameLength;
  Op.Indata = Indata;
  Op.IndataLength = IndataLength;

  Status = CephRpcEncodeOsdOp (
             PoolId,
             1,
             OsdEpoch,
             Client->Session.NextOutgoingSequence,
             ObjectName,
             ObjectNameLength,
             &Op,
             1,
             Request,
             sizeof (Request),
             &RequestLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRpcCallAuto (
             Client,
             CEPH_MSG_OSD_OP,
             CEPH_MSG_OSD_OPREPLY,
             Request,
             RequestLength,
             NULL,
             0,
             ReplyFront,
             sizeof (ReplyFront),
             &ReplyFrontLength,
             ReplyData,
             sizeof (ReplyData),
             &ReplyDataLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRpcDecodeOsdOpReply (
             ReplyFront,
             ReplyFrontLength,
             ReplyData,
             ReplyDataLength,
             &Reply
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Reply.Result != 0) {
    return EFI_DEVICE_ERROR;
  }

  if (Reply.DataLength > OutDataCapacity) {
    return EFI_BUFFER_TOO_SMALL;
  }

  if (Reply.DataLength != 0) {
    CopyMem (OutData, Reply.Data, Reply.DataLength);
  }

  *OutDataLength = Reply.DataLength;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CephRbdBlockIoBuildObjectName (
  IN CONST CHAR8  *Prefix,
  IN UINTN        PrefixLength,
  IN CONST CHAR8  *Name,
  OUT CHAR8       *ObjectName,
  IN UINTN        ObjectNameCapacity,
  OUT UINTN       *ObjectNameLength
  )
{
  if ((Prefix == NULL) || (Name == NULL) || (ObjectName == NULL) ||
      (ObjectNameLength == NULL) || (PrefixLength == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (PrefixLength + 1 >= ObjectNameCapacity) {
    return EFI_BAD_BUFFER_SIZE;
  }

  CopyMem (ObjectName, Prefix, PrefixLength);
  ObjectName[PrefixLength] = '.';
  *ObjectNameLength = CephRbdAsciiStrLen (Name, ObjectNameCapacity -
                                          PrefixLength - 1);
  if (*ObjectNameLength == 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  CopyMem (ObjectName + PrefixLength + 1, Name, *ObjectNameLength);
  ObjectName[PrefixLength + 1 + *ObjectNameLength] = '\0';
  *ObjectNameLength += PrefixLength + 1;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdBlockIoDiscover (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN UINT32                       PoolId,
  IN CONST CHAR8                  *ImageName,
  IN UINTN                        ImageNameLength,
  IN CONST UINT8                  *Fsid,
  OUT CEPH_RBD_BLOCK_DEVICE       *Device
  )
{
  EFI_STATUS                Status;
  UINT8                     Data[CEPH_RPC_MAX_METADATA];
  UINT8                     Indata[16];
  UINT8                     ObjectName[CEPH_RBD_MAX_OBJECT_NAME];
  UINT8                     ImageId[CEPH_RBD_MAX_IMAGE_NAME];
  UINT8                     ObjectPrefix[CEPH_RBD_OBJECT_PREFIX_MAX + 1];
  UINTN                     DataLength;
  UINTN                     ObjectNameLength;
  CEPH_RBD_FORMAT2_METADATA Metadata;
  UINT64                    OsdEpoch;
  UINT64                    Features;
  UINT64                    Incompatible;
  UINT64                    StripeUnit;
  UINT64                    StripeCount;
  CEPH_WIRE_WRITER          Writer;

  if ((Client == NULL) || (ImageName == NULL) ||
      (ImageNameLength == 0) ||
      (ImageNameLength >= CEPH_RBD_MAX_IMAGE_NAME) || (Fsid == NULL) ||
      (Device == NULL) || (PoolId == 0) ||
      (PoolId > CEPH_RBD_POOL_ID_MAX))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Device, sizeof (*Device));
  Device->PoolId = PoolId;
  Device->ClientIncarnation = 1;
  CopyMem (Device->ImageName, ImageName, ImageNameLength);
  Device->ImageName[ImageNameLength] = '\0';
  Device->ImageNameLength = ImageNameLength;
  CopyMem (Device->Fsid, Fsid, CEPH_FSID_SIZE);

  Status = CephRbdBlockIoGetOsdMap (
             Client, Fsid, (INT32)PoolId, &OsdEpoch, &Device->Placement
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Device->OsdEpoch = (UINT32)OsdEpoch;

  // Resolve the image id through rbd_id.<name>, then use rbd_header.<id>.
  Status = CephRbdBlockIoBuildObjectName (
             CEPH_RBD_ID_OBJECT_PREFIX,
             sizeof (CEPH_RBD_ID_OBJECT_PREFIX) - 1,
             (CONST CHAR8 *)ImageName,
             (CHAR8 *)ObjectName,
             sizeof (ObjectName),
             &ObjectNameLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdBlockIoCallClassMethod (
             Client,
             PoolId,
             Device->OsdEpoch,
             (CONST CHAR8 *)ObjectName,
             ObjectNameLength,
             "rbd",
             "get_id",
             NULL,
             0,
             Data,
             sizeof (Data),
             &DataLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdBlockIoDecodeString (
             Data,
             DataLength,
             (CHAR8 *)ImageId,
             sizeof (ImageId)
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdBlockIoBuildObjectName (
             CEPH_RBD_HEADER_OBJECT_PREFIX,
             sizeof (CEPH_RBD_HEADER_OBJECT_PREFIX) - 1,
             (CONST CHAR8 *)ImageId,
             (CHAR8 *)ObjectName,
             sizeof (ObjectName),
             &ObjectNameLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireWriterInit (&Writer, Indata, sizeof (Indata));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireWriteLe64 (&Writer, CEPH_RBD_SNAP_ID);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdBlockIoCallClassMethod (
             Client,
             PoolId,
             Device->OsdEpoch,
             (CONST CHAR8 *)ObjectName,
             ObjectNameLength,
             "rbd",
             "get_size",
             Indata,
             Writer.Written,
             Data,
             sizeof (Data),
             &DataLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdBlockIoDecodeHeader (Data, DataLength, &Metadata);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireWriteU8 (&Writer, 1);  // read_only
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdBlockIoCallClassMethod (
             Client,
             PoolId,
             Device->OsdEpoch,
             (CONST CHAR8 *)ObjectName,
             ObjectNameLength,
             "rbd",
             "get_features",
             Indata,
             Writer.Written,
             Data,
             sizeof (Data),
             &DataLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdBlockIoDecodeU64Pair (
             Data,
             DataLength,
             &Features,
             &Incompatible
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Incompatible & CEPH_RBD_INCOMPATIBLE_FEATURES) != 0) {
    return EFI_UNSUPPORTED;
  }

  Metadata.Features = Features;

  Status = CephRbdBlockIoCallClassMethod (
             Client,
             PoolId,
             Device->OsdEpoch,
             (CONST CHAR8 *)ObjectName,
             ObjectNameLength,
             "rbd",
             "get_object_prefix",
             NULL,
             0,
             Data,
             sizeof (Data),
             &DataLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdBlockIoDecodeString (
             Data,
             DataLength,
             (CHAR8 *)ObjectPrefix,
             sizeof (ObjectPrefix)
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Metadata.ObjectPrefix = (CONST CHAR8 *)ObjectPrefix;

  if ((Features & CEPH_RBD_FEATURE_STRIPINGV2) != 0) {
    Status = CephRbdBlockIoCallClassMethod (
               Client,
               PoolId,
               Device->OsdEpoch,
               (CONST CHAR8 *)ObjectName,
               ObjectNameLength,
               "rbd",
               "get_stripe_unit_count",
               NULL,
               0,
               Data,
               sizeof (Data),
               &DataLength
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = CephRbdBlockIoDecodeU64Pair (
               Data,
               DataLength,
               &StripeUnit,
               &StripeCount
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Metadata.StripeUnit = StripeUnit;
    if (StripeCount > MAX_UINT32) {
      return EFI_UNSUPPORTED;
    }

    Metadata.StripeCount = (UINT32)StripeCount;
  }

  Status = CephRbdFormat2Initialize (&Metadata, &Device->Image);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Device->Ready = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdBlockIoGetObjectPlacement (
  IN  CONST CEPH_RBD_BLOCK_DEVICE  *Device,
  IN  CONST CHAR8                  *ObjectName,
  IN  UINTN                        ObjectNameLength,
  OUT UINT32                       *Pg,  OPTIONAL
  OUT UINT32                       *Pps,  OPTIONAL
  OUT INT32                        *OsdIds,
  IN  UINTN                        OsdIdsCapacity,
  OUT UINTN                        *OsdIdCount
  )
{
  if ((Device == NULL) || !Device->Ready || (ObjectName == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  return CephRbdPlacementMapObject (
           &Device->Placement,
           ObjectName,
           ObjectNameLength,
           Pg,
           Pps,
           OsdIds,
           OsdIdsCapacity,
           OsdIdCount
           );
}

EFI_STATUS
CephRbdBlockIoReadBlocks (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN OUT CEPH_RBD_BLOCK_DEVICE    *Device,
  IN UINT64                       StartLba,
  IN UINTN                        NumberOfBlocks,
  OUT VOID                        *Buffer,
  IN UINTN                        BufferSize
  )
{
  EFI_STATUS         Status;
  CEPH_RBD_EXTENT    Extent;
  UINT64             ImageOffset;
  UINT64             Remaining;
  UINTN              Copied;
  UINTN              ObjectNameLength;
  UINTN              OsdCount;
  UINT32             Pg;
  UINT32             Pps;
  INT32              OsdIds[CEPH_RBD_CRUSH_MAX_RESULT];
  UINT8              ObjectName[CEPH_RBD_MAX_OBJECT_NAME];
  UINT8              Request[CEPH_RPC_MAX_METADATA];
  UINT8              Front[CEPH_RPC_MAX_METADATA];
  UINT8              Data[CEPH_MSGR_V1_RETRANSMIT_FRAME_MAX];
  UINTN              RequestLength;
  UINTN              FrontLength;
  UINTN              DataLength;
  CEPH_OSD_OP_REQUEST Op;
  CEPH_OSD_OP_REPLY   Reply;

  if ((Client == NULL) || (Device == NULL) || !Device->Ready ||
      (Buffer == NULL) || (NumberOfBlocks == 0) ||
      (NumberOfBlocks > CEPH_RBD_MAX_READ_BLOCKS))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (BufferSize < NumberOfBlocks * CEPH_RBD_BLOCK_SIZE) {
    return EFI_BUFFER_TOO_SMALL;
  }

  ImageOffset = StartLba * CEPH_RBD_BLOCK_SIZE;
  Remaining = (UINT64)NumberOfBlocks * CEPH_RBD_BLOCK_SIZE;
  if ((ImageOffset >= Device->Image.Layout.ImageSize) ||
      (Remaining > Device->Image.Layout.ImageSize - ImageOffset))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  Copied = 0;
  while (Remaining > 0) {
    Status = CephRbdMapExtent (
               &Device->Image.Layout,
               ImageOffset,
               Remaining,
               &Extent
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = CephRbdFormat2BuildObjectName (
               &Device->Image,
               Extent.ObjectNumber,
               (CHAR8 *)ObjectName,
               sizeof (ObjectName),
               &ObjectNameLength
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    // Resolve the object's acting set from the OSDMap. The primary is the
    // first OSD in the set. Routing the OSD op requires a per-OSD messenger
    // session with an OSD service ticket, which is a separate step; for now
    // the primary is recorded and the op uses the existing connected session.
    Status = CephRbdPlacementMapObject (
               &Device->Placement,
               (CONST CHAR8 *)ObjectName,
               ObjectNameLength,
               &Pg,
               &Pps,
               OsdIds,
               CEPH_RBD_CRUSH_MAX_RESULT,
               &OsdCount
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Device->PrimaryOsd = OsdIds[0];

    ZeroMem (&Op, sizeof (Op));
    Op.Op = CEPH_OSD_OP_READ;
    Op.Flags = CEPH_OSD_FLAG_READ |
               CEPH_OSD_OP_FLAG_FADVISE_READAHEAD;
    Op.Offset = Extent.ObjectOffset;
    Op.Length = Extent.Length;

    Status = CephRpcEncodeOsdOp (
               Device->PoolId,
               Device->ClientIncarnation,
               Device->OsdEpoch,
               Client->Session.NextOutgoingSequence,
               (CONST CHAR8 *)ObjectName,
               ObjectNameLength,
               &Op,
               1,
               Request,
               sizeof (Request),
               &RequestLength
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = CephRpcCallAuto (
               Client,
               CEPH_MSG_OSD_OP,
               CEPH_MSG_OSD_OPREPLY,
               Request,
               RequestLength,
               NULL,
               0,
               Front,
               sizeof (Front),
               &FrontLength,
               Data,
               sizeof (Data),
               &DataLength
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = CephRpcDecodeOsdOpReply (
               Front,
               FrontLength,
               Data,
               DataLength,
               &Reply
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Reply.Result != 0) {
      return EFI_DEVICE_ERROR;
    }

    if ((Extent.Length > Reply.DataLength) ||
        (Copied > MAX_UINTN - (UINTN)Extent.Length))
    {
      return EFI_COMPROMISED_DATA;
    }

    CopyMem (
      (UINT8 *)Buffer + Copied,
      Reply.Data,
      (UINTN)Extent.Length
      );
    Copied += (UINTN)Extent.Length;
    ImageOffset += Extent.Length;
    Remaining -= Extent.Length;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdBlockIoGetMediaLastLba (
  IN CONST CEPH_RBD_BLOCK_DEVICE  *Device,
  OUT UINT64                      *LastLba
  )
{
  if ((Device == NULL) || !Device->Ready || (LastLba == NULL) ||
      (Device->Image.Layout.ImageSize < CEPH_RBD_BLOCK_SIZE))
  {
    return EFI_INVALID_PARAMETER;
  }

  *LastLba = (Device->Image.Layout.ImageSize / CEPH_RBD_BLOCK_SIZE) - 1;
  return EFI_SUCCESS;
}
