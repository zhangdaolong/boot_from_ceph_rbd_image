#ifndef CEPH_RBD_MSGR_V1_HANDSHAKE_H_
#define CEPH_RBD_MSGR_V1_HANDSHAKE_H_

#include <Uefi.h>
#include "CephRbdMessenger.h"
#include "CephRbdTcp4.h"

#define CEPH_MSGR_V1_BANNER_LENGTH              9
// u32 type, u32 nonce, and the 128-byte legacy sockaddr_storage wire value.
#define CEPH_MSGR_V1_ENTITY_ADDR_LENGTH         136
#define CEPH_MSGR_V1_ENTITY_ADDR_FAMILY_OFFSET    8
#define CEPH_MSGR_V1_ENTITY_ADDR_PORT_OFFSET     10
#define CEPH_MSGR_V1_ENTITY_ADDR_IP_OFFSET       12
#define CEPH_MSGR_V1_AF_INET                       2
#define CEPH_MSGR_V1_MAX_AUTHOR_REPLY           4096

typedef struct {
  CEPH_MSGR_V1_CONNECT_REPLY  ConnectReply;
  CEPH_MSGR_CONNECT_ACTION    Action;
  UINT32                      AuthorizerReplyLength;
  UINT8                       AuthorizerReply[CEPH_MSGR_V1_MAX_AUTHOR_REPLY];
  BOOLEAN                     Ready;
} CEPH_MSGR_V1_NEGOTIATION;

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
  );

#endif
