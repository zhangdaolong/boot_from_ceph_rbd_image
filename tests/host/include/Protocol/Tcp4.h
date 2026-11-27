#ifndef TEST_TCP4_H_
#define TEST_TCP4_H_

#include <Uefi.h>

extern EFI_GUID gEfiTcp4ServiceBindingProtocolGuid;
extern EFI_GUID gEfiTcp4ProtocolGuid;

typedef struct _EFI_TCP4_PROTOCOL EFI_TCP4_PROTOCOL;

typedef struct {
  BOOLEAN UseDefaultAddress;
  EFI_IPv4_ADDRESS StationAddress;
  EFI_IPv4_ADDRESS SubnetMask;
  UINT16 StationPort;
  EFI_IPv4_ADDRESS RemoteAddress;
  UINT16 RemotePort;
  BOOLEAN ActiveFlag;
} EFI_TCP4_ACCESS_POINT;

typedef struct {
  UINT32 ReceiveBufferSize;
  UINT32 SendBufferSize;
  UINT32 MaxSynBackLog;
  UINT32 ConnectionTimeout;
  UINT32 DataRetries;
  UINT32 FinTimeout;
  UINT32 TimeWaitTimeout;
  UINT32 KeepAliveProbes;
  UINT32 KeepAliveTime;
  UINT32 KeepAliveInterval;
  BOOLEAN EnableNagle;
  BOOLEAN EnableTimeStamp;
  BOOLEAN EnableWindowScaling;
  BOOLEAN EnableSelectiveAck;
  BOOLEAN EnablePathMtuDiscovery;
} EFI_TCP4_OPTION;

typedef struct {
  UINT8 TypeOfService;
  UINT8 TimeToLive;
  EFI_TCP4_ACCESS_POINT AccessPoint;
  EFI_TCP4_OPTION *ControlOption;
} EFI_TCP4_CONFIG_DATA;

typedef struct {
  EFI_EVENT Event;
  EFI_STATUS Status;
} EFI_TCP4_COMPLETION_TOKEN;

typedef struct {
  EFI_TCP4_COMPLETION_TOKEN CompletionToken;
} EFI_TCP4_CONNECTION_TOKEN;

typedef struct {
  UINT32 FragmentLength;
  VOID *FragmentBuffer;
} EFI_TCP4_FRAGMENT_DATA;

typedef struct {
  BOOLEAN UrgentFlag;
  UINT32 DataLength;
  UINT32 FragmentCount;
  EFI_TCP4_FRAGMENT_DATA FragmentTable[1];
} EFI_TCP4_RECEIVE_DATA;

typedef struct {
  BOOLEAN Push;
  BOOLEAN Urgent;
  UINT32 DataLength;
  UINT32 FragmentCount;
  EFI_TCP4_FRAGMENT_DATA FragmentTable[1];
} EFI_TCP4_TRANSMIT_DATA;

typedef struct {
  EFI_TCP4_COMPLETION_TOKEN CompletionToken;
  union {
    EFI_TCP4_RECEIVE_DATA *RxData;
    EFI_TCP4_TRANSMIT_DATA *TxData;
  } Packet;
} EFI_TCP4_IO_TOKEN;

typedef struct {
  EFI_TCP4_COMPLETION_TOKEN CompletionToken;
  BOOLEAN AbortOnClose;
} EFI_TCP4_CLOSE_TOKEN;

struct _EFI_TCP4_PROTOCOL {
  EFI_STATUS (EFIAPI *Configure)(
    EFI_TCP4_PROTOCOL *This,
    EFI_TCP4_CONFIG_DATA *TcpConfigData
    );
  EFI_STATUS (EFIAPI *Connect)(
    EFI_TCP4_PROTOCOL *This,
    EFI_TCP4_CONNECTION_TOKEN *ConnectionToken
    );
  EFI_STATUS (EFIAPI *Transmit)(
    EFI_TCP4_PROTOCOL *This,
    EFI_TCP4_IO_TOKEN *Token
    );
  EFI_STATUS (EFIAPI *Receive)(
    EFI_TCP4_PROTOCOL *This,
    EFI_TCP4_IO_TOKEN *Token
    );
  EFI_STATUS (EFIAPI *Close)(
    EFI_TCP4_PROTOCOL *This,
    EFI_TCP4_CLOSE_TOKEN *CloseToken
    );
  EFI_STATUS (EFIAPI *Cancel)(
    EFI_TCP4_PROTOCOL *This,
    EFI_TCP4_COMPLETION_TOKEN *Token
    );
  EFI_STATUS (EFIAPI *Poll)(
    EFI_TCP4_PROTOCOL *This
    );
};

#endif
