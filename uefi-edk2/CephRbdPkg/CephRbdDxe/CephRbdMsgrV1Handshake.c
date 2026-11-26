#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include "CephRbdMessenger.h"
#include "CephRbdMsgrV1Handshake.h"
#include "CephRbdTcp4.h"

STATIC CONST UINT8 mCephMsgrV1Banner[] = "ceph v027";

STATIC
EFI_STATUS
CephMsgrV1ValidatePeerAddress (
  IN CONST UINT8           *Address,
  IN CONST EFI_IPv4_ADDRESS *ExpectedAddress,
  IN UINT16                ExpectedPort
  )
{
  UINT8  ExpectedPortHigh;
  UINT8  ExpectedPortLow;

  if ((Address[CEPH_MSGR_V1_ENTITY_ADDR_FAMILY_OFFSET] != 0) ||
      (Address[CEPH_MSGR_V1_ENTITY_ADDR_FAMILY_OFFSET + 1] !=
       CEPH_MSGR_V1_AF_INET))
  {
    return EFI_UNSUPPORTED;
  }

  ExpectedPortHigh = (UINT8)(ExpectedPort >> 8);
  ExpectedPortLow  = (UINT8)ExpectedPort;
  if ((Address[CEPH_MSGR_V1_ENTITY_ADDR_PORT_OFFSET] != ExpectedPortHigh) ||
      (Address[CEPH_MSGR_V1_ENTITY_ADDR_PORT_OFFSET + 1] != ExpectedPortLow) ||
      (CompareMem (
         Address + CEPH_MSGR_V1_ENTITY_ADDR_IP_OFFSET,
         ExpectedAddress,
         sizeof (*ExpectedAddress)
         ) != 0))
  {
    return EFI_ACCESS_DENIED;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephMsgrV1ExchangeBanners (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN CONST EFI_IPv4_ADDRESS    *RemoteAddress,
  IN UINT16                    RemotePort,
  IN UINT32                    TimeoutSeconds
  )
{
  EFI_STATUS  Status;
  UINT8       BannerAndAddresses[
                CEPH_MSGR_V1_BANNER_LENGTH +
                2 * CEPH_MSGR_V1_ENTITY_ADDR_LENGTH
                ];
  UINTN       AddressOffset;

  if ((Connection == NULL) || (RemoteAddress == NULL) ||
      (RemotePort == 0) || (TimeoutSeconds == 0) ||
      !Connection->Connected || (Connection->Tcp4 == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephRbdTcp4Send (
             Connection,
             mCephMsgrV1Banner,
             CEPH_MSGR_V1_BANNER_LENGTH,
             TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdTcp4ReceiveExact (
             Connection,
             BannerAndAddresses,
             sizeof (BannerAndAddresses),
             TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (CompareMem (
        BannerAndAddresses,
        mCephMsgrV1Banner,
        CEPH_MSGR_V1_BANNER_LENGTH
        ) != 0)
  {
    return EFI_COMPROMISED_DATA;
  }

  AddressOffset = CEPH_MSGR_V1_BANNER_LENGTH;
  Status = CephMsgrV1ValidatePeerAddress (
             BannerAndAddresses + AddressOffset,
             RemoteAddress,
             RemotePort
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Reflect the address the peer observed for this TCP connection.
  AddressOffset += CEPH_MSGR_V1_ENTITY_ADDR_LENGTH;
  return CephRbdTcp4Send (
           Connection,
           BannerAndAddresses + AddressOffset,
           CEPH_MSGR_V1_ENTITY_ADDR_LENGTH,
           TimeoutSeconds
           );
}

EFI_STATUS
CephMsgrV1SendConnectAndReadReply (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN UINT64                    SupportedFeatures,
  IN UINT32                    HostType,
  IN UINT32                    GlobalSequence,
  IN UINT32                    ConnectSequence,
  IN UINT32                    ProtocolVersion,
  IN UINT32                    AuthorizerProtocol,
  IN CONST VOID                *Authorizer,
  IN UINT32                    AuthorizerLength,
  IN UINT8                     Flags,
  IN UINT64                    RequiredFeatures,
  IN UINT32                    TimeoutSeconds,
  OUT CEPH_MSGR_V1_NEGOTIATION *Negotiation
  )
{
  EFI_STATUS  Status;
  UINT8       ConnectRequest[CEPH_MSGR_V1_CONNECT_SIZE + 4096];
  UINT8       ConnectReply[CEPH_MSGR_V1_CONNECT_REPLY_SIZE];
  UINTN       ConnectRequestLength;

  if ((Connection == NULL) || (TimeoutSeconds == 0) ||
      (Negotiation == NULL) || !Connection->Connected ||
      (Connection->Tcp4 == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Negotiation, sizeof (*Negotiation));
  Negotiation->SupportedFeatures = SupportedFeatures;

  Status = CephMsgrEncodeV1Connect (
             SupportedFeatures,
             HostType,
             GlobalSequence,
             ConnectSequence,
             ProtocolVersion,
             AuthorizerProtocol,
             Authorizer,
             AuthorizerLength,
             Flags,
             ConnectRequest,
             sizeof (ConnectRequest),
             &ConnectRequestLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdTcp4Send (
             Connection,
             ConnectRequest,
             ConnectRequestLength,
             TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdTcp4ReceiveExact (
             Connection,
             ConnectReply,
             sizeof (ConnectReply),
             TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephMsgrDecodeV1ConnectReply (
             ConnectReply,
             sizeof (ConnectReply),
             &Negotiation->ConnectReply
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Negotiation->ConnectReply.AuthorizerLength >
      sizeof (Negotiation->AuthorizerReply))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (Negotiation->ConnectReply.AuthorizerLength != 0) {
    Status = CephRbdTcp4ReceiveExact (
               Connection,
               Negotiation->AuthorizerReply,
               Negotiation->ConnectReply.AuthorizerLength,
               TimeoutSeconds
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Negotiation->AuthorizerReplyLength =
      Negotiation->ConnectReply.AuthorizerLength;
  }

  Status = CephMsgrClassifyV1ConnectReply (
             &Negotiation->ConnectReply,
             RequiredFeatures,
             ProtocolVersion,
             ConnectSequence,
             &Negotiation->Action
             );
  if (!EFI_ERROR (Status)) {
    Negotiation->Ready =
      (BOOLEAN)(Negotiation->Action == CephMsgrConnectReady);
  }

  return Status;
}

EFI_STATUS
CephMsgrV1ClientHandshake (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN CONST EFI_IPv4_ADDRESS    *RemoteAddress,
  IN UINT16                    RemotePort,
  IN UINT64                    SupportedFeatures,
  IN UINT32                    HostType,
  IN UINT32                    GlobalSequence,
  IN UINT32                    ConnectSequence,
  IN UINT32                    ProtocolVersion,
  IN UINT32                    AuthorizerProtocol,
  IN CONST VOID                *Authorizer,
  IN UINT32                    AuthorizerLength,
  IN UINT8                     Flags,
  IN UINT64                    RequiredFeatures,
  IN UINT32                    TimeoutSeconds,
  OUT CEPH_MSGR_V1_NEGOTIATION *Negotiation
  )
{
  EFI_STATUS  Status;

  Status = CephMsgrV1ExchangeBanners (
             Connection,
             RemoteAddress,
             RemotePort,
             TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return CephMsgrV1SendConnectAndReadReply (
           Connection,
           SupportedFeatures,
           HostType,
           GlobalSequence,
           ConnectSequence,
           ProtocolVersion,
           AuthorizerProtocol,
           Authorizer,
           AuthorizerLength,
           Flags,
           RequiredFeatures,
           TimeoutSeconds,
           Negotiation
           );
}
