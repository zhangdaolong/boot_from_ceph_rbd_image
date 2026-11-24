#include <Uefi.h>
#include "CephRbdMessenger.h"
#include "CephRbdWire.h"

STATIC
UINT16
CephMsgrReadLe16 (
  IN CONST UINT8  *Data
  )
{
  return (UINT16)Data[0] | ((UINT16)Data[1] << 8);
}

STATIC
UINT32
CephMsgrReadLe32 (
  IN CONST UINT8  *Data
  )
{
  return (UINT32)Data[0]
         | ((UINT32)Data[1] << 8)
         | ((UINT32)Data[2] << 16)
         | ((UINT32)Data[3] << 24);
}

STATIC
UINT64
CephMsgrReadLe64 (
  IN CONST UINT8  *Data
  )
{
  return ((UINT64)CephMsgrReadLe32 (Data + 4) << 32)
         | CephMsgrReadLe32 (Data);
}

UINT32
CephMsgrCrc32c (
  IN CONST VOID  *Data,
  IN UINTN       Length
  )
{
  CONST UINT8  *Bytes;
  UINT32       Crc;
  UINTN        Index;
  UINTN        Bit;

  if ((Data == NULL) && (Length != 0)) {
    return 0;
  }

  Bytes = (CONST UINT8 *)Data;
  Crc   = 0;
  for (Index = 0; Index < Length; Index++) {
    Crc ^= Bytes[Index];
    for (Bit = 0; Bit < 8; Bit++) {
      Crc = (Crc >> 1) ^ ((Crc & 1) ? 0x82F63B78U : 0);
    }
  }

  return Crc;
}

EFI_STATUS
CephMsgrDecodeV1Frame (
  IN CONST VOID          *Buffer,
  IN UINTN               BufferLength,
  IN UINTN               MaximumPayload,
  OUT CEPH_MSGR_V1_FRAME *Frame
  )
{
  CONST UINT8  *Bytes;
  CONST UINT8  *Header;
  CONST UINT8  *Footer;
  UINT32       FrontLength;
  UINT32       MiddleLength;
  UINT32       DataLength;
  UINT32       PayloadLength;
  UINTN        ExpectedLength;
  UINTN        Offset;
  UINT8        FooterFlags;

  if ((Buffer == NULL) || (Frame == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (BufferLength <
      (1 + CEPH_MSGR_V1_HEADER_SIZE + CEPH_MSGR_V1_FOOTER_SIZE))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  Bytes = (CONST UINT8 *)Buffer;
  if (Bytes[0] != CEPH_MSGR_V1_TAG_MESSAGE) {
    return EFI_UNSUPPORTED;
  }

  Header       = Bytes + 1;
  FrontLength  = CephMsgrReadLe32 (Header + 22);
  MiddleLength = CephMsgrReadLe32 (Header + 26);
  DataLength   = CephMsgrReadLe32 (Header + 30);

  if ((FrontLength > MAX_UINT32 - MiddleLength) ||
      (FrontLength + MiddleLength > MAX_UINT32 - DataLength))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  PayloadLength = FrontLength + MiddleLength + DataLength;
  if (PayloadLength > MaximumPayload) {
    return EFI_BAD_BUFFER_SIZE;
  }

  ExpectedLength = 1 + CEPH_MSGR_V1_HEADER_SIZE + (UINTN)PayloadLength +
                   CEPH_MSGR_V1_FOOTER_SIZE;
  if (BufferLength != ExpectedLength) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (CephMsgrReadLe32 (Header + 49) !=
      CephMsgrCrc32c (Header, 49))
  {
    return EFI_CRC_ERROR;
  }

  Offset = 1 + CEPH_MSGR_V1_HEADER_SIZE;
  Footer = Bytes + Offset + PayloadLength;
  FooterFlags = Footer[20];
  if ((FooterFlags & CEPH_MSGR_V1_FOOTER_COMPLETE) == 0) {
    return EFI_ABORTED;
  }

  if ((FooterFlags & ~(CEPH_MSGR_V1_FOOTER_COMPLETE |
                       CEPH_MSGR_V1_FOOTER_NOCRC |
                       CEPH_MSGR_V1_FOOTER_SIGNED)) != 0)
  {
    return EFI_UNSUPPORTED;
  }

  if ((FooterFlags & CEPH_MSGR_V1_FOOTER_SIGNED) != 0) {
    return EFI_UNSUPPORTED;
  }

  if ((CephMsgrReadLe32 (Footer) !=
       CephMsgrCrc32c (Bytes + Offset, FrontLength)) ||
      (CephMsgrReadLe32 (Footer + 4) !=
       CephMsgrCrc32c (Bytes + Offset + FrontLength, MiddleLength)))
  {
    return EFI_CRC_ERROR;
  }

  if (((FooterFlags & CEPH_MSGR_V1_FOOTER_NOCRC) == 0) &&
      (CephMsgrReadLe32 (Footer + 8) !=
       CephMsgrCrc32c (Bytes + Offset + FrontLength + MiddleLength, DataLength)))
  {
    return EFI_CRC_ERROR;
  }

  Frame->Sequence       = CephMsgrReadLe64 (Header);
  Frame->TransactionId  = CephMsgrReadLe64 (Header + 8);
  Frame->Type            = CephMsgrReadLe16 (Header + 16);
  Frame->Priority        = CephMsgrReadLe16 (Header + 18);
  Frame->Version         = CephMsgrReadLe16 (Header + 20);
  Frame->FrontLength     = FrontLength;
  Frame->MiddleLength    = MiddleLength;
  Frame->DataLength      = DataLength;
  Frame->DataOffset      = CephMsgrReadLe16 (Header + 34);
  Frame->Front           = Bytes + Offset;
  Frame->Middle          = Frame->Front + FrontLength;
  Frame->Data            = Frame->Middle + MiddleLength;
  Frame->FooterFlags     = FooterFlags;
  return EFI_SUCCESS;
}

EFI_STATUS
CephMsgrEncodeV1Connect (
  IN UINT64       SupportedFeatures,
  IN UINT32       HostType,
  IN UINT32       GlobalSequence,
  IN UINT32       ConnectSequence,
  IN UINT32       ProtocolVersion,
  IN UINT32       AuthorizerProtocol,
  IN CONST VOID   *Authorizer,
  IN UINT32       AuthorizerLength,
  IN UINT8        Flags,
  OUT VOID        *Buffer,
  IN UINTN        BufferLength,
  OUT UINTN       *EncodedLength
  )
{
  CEPH_WIRE_WRITER  Writer;
  EFI_STATUS        Status;
  UINTN             RequiredLength;

  if ((Buffer == NULL) || (EncodedLength == NULL) ||
      ((Authorizer == NULL) && (AuthorizerLength != 0)) ||
      (HostType == 0) || (ProtocolVersion == 0) ||
      (AuthorizerLength > 4096))
  {
    return EFI_INVALID_PARAMETER;
  }

  *EncodedLength = 0;
  RequiredLength = CEPH_MSGR_V1_CONNECT_SIZE + AuthorizerLength;
  if (BufferLength < RequiredLength) {
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = CephWireWriterInit (&Writer, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireWriteLe64 (&Writer, SupportedFeatures);
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, HostType);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, GlobalSequence);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, ConnectSequence);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, ProtocolVersion);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, AuthorizerProtocol);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, AuthorizerLength);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteU8 (&Writer, Flags);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteBytes (&Writer, Authorizer, AuthorizerLength);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EncodedLength = Writer.Written;
  return EFI_SUCCESS;
}

EFI_STATUS
CephMsgrDecodeV1ConnectReply (
  IN CONST VOID                   *Buffer,
  IN UINTN                        BufferLength,
  OUT CEPH_MSGR_V1_CONNECT_REPLY  *Reply
  )
{
  CONST UINT8  *Bytes;

  if ((Buffer == NULL) || (Reply == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (BufferLength != CEPH_MSGR_V1_CONNECT_REPLY_SIZE) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Bytes = (CONST UINT8 *)Buffer;
  Reply->Tag              = Bytes[0];
  Reply->Features         = CephMsgrReadLe64 (Bytes + 1);
  Reply->GlobalSequence   = CephMsgrReadLe32 (Bytes + 9);
  Reply->ConnectSequence  = CephMsgrReadLe32 (Bytes + 13);
  Reply->ProtocolVersion  = CephMsgrReadLe32 (Bytes + 17);
  Reply->AuthorizerLength = CephMsgrReadLe32 (Bytes + 21);
  Reply->Flags            = Bytes[25];
  if (Reply->AuthorizerLength > 4096) {
    return EFI_BAD_BUFFER_SIZE;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephMsgrClassifyV1ConnectReply (
  IN CONST CEPH_MSGR_V1_CONNECT_REPLY  *Reply,
  IN UINT64                            RequiredFeatures,
  IN UINT32                            ExpectedProtocolVersion,
  IN UINT32                            CurrentConnectSequence,
  OUT CEPH_MSGR_CONNECT_ACTION         *Action
  )
{
  if ((Reply == NULL) || (Action == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Reply->AuthorizerLength > 4096) {
    return EFI_BAD_BUFFER_SIZE;
  }

  switch (Reply->Tag) {
    case CEPH_MSGR_TAG_READY:
      if ((Reply->ProtocolVersion != ExpectedProtocolVersion) ||
          ((Reply->Features & RequiredFeatures) != RequiredFeatures) ||
          (CurrentConnectSequence == MAX_UINT32) ||
          (Reply->ConnectSequence != CurrentConnectSequence + 1))
      {
        *Action = CephMsgrConnectRejected;
        return EFI_UNSUPPORTED;
      }

      *Action = CephMsgrConnectReady;
      return EFI_SUCCESS;

    case CEPH_MSGR_TAG_RETRY_SESSION:
      if (Reply->ConnectSequence <= CurrentConnectSequence) {
        *Action = CephMsgrConnectRejected;
        return EFI_COMPROMISED_DATA;
      }

      *Action = CephMsgrConnectRetrySession;
      return EFI_SUCCESS;

    case CEPH_MSGR_TAG_RETRY_GLOBAL:
      *Action = CephMsgrConnectRetryGlobal;
      return EFI_SUCCESS;

    case CEPH_MSGR_TAG_RESETSESSION:
      *Action = CephMsgrConnectResetSession;
      return EFI_SUCCESS;

    case CEPH_MSGR_TAG_WAIT:
      *Action = CephMsgrConnectWait;
      return EFI_SUCCESS;

    case CEPH_MSGR_TAG_SEQ:
      *Action = CephMsgrConnectAckSequence;
      return EFI_SUCCESS;

    case CEPH_MSGR_TAG_CHALLENGE_AUTH:
      *Action = CephMsgrConnectNeedAuthorizerChallenge;
      return EFI_SUCCESS;

    case CEPH_MSGR_TAG_FEATURES:
    case CEPH_MSGR_TAG_BADPROTOVER:
    case CEPH_MSGR_TAG_BADAUTHORIZER:
    case CEPH_MSGR_TAG_CLOSE:
      *Action = CephMsgrConnectRejected;
      return EFI_ACCESS_DENIED;

    default:
      *Action = CephMsgrConnectRejected;
      return EFI_UNSUPPORTED;
  }
}
