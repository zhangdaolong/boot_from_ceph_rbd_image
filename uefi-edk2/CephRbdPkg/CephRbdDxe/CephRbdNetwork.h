#ifndef CEPH_RBD_NETWORK_H_
#define CEPH_RBD_NETWORK_H_

#include <Uefi.h>

// Local IPv4 configuration for the Ceph RBD DXE driver.
//
// By default the driver lets the UEFI platform supply the station address
// (EFI_TCP4_ACCESS_POINT.UseDefaultAddress == TRUE), which in OVMF means the
// platform IP4 driver has already performed DHCP auto-configuration. Firmware
// builders that need a deterministic address can publish a separate
// CephRbdNetworkConfig UEFI variable holding a CEPH_RBD_NETWORK_CONFIG blob:
//
//   * CEPH_RBD_NETWORK_MODE_DHCP   -> keep UseDefaultAddress TRUE.
//   * CEPH_RBD_NETWORK_MODE_STATIC -> UseDefaultAddress FALSE with the
//                                    supplied station address / subnet mask.
//
// Keeping this in its own variable means the fixed-size CEPH_RBD_CONFIG blob
// and its generator (make-ceph-rbd-config.py) are unchanged.

#define CEPH_RBD_NETWORK_SIGNATURE    0x4352424EU  // 'CRBN'
#define CEPH_RBD_NETWORK_VERSION      1U

#define CEPH_RBD_NETWORK_MODE_DHCP    0U
#define CEPH_RBD_NETWORK_MODE_STATIC  1U

#define CEPH_RBD_NETWORK_FLAG_GATEWAY 0x00000001U  // GatewayAddress is valid

extern EFI_GUID      gCephRbdNetworkGuid;
extern CONST CHAR16  gCephRbdNetworkVariableName[];

typedef struct {
  UINT32            Signature;
  UINT32            Version;
  UINT32            Length;
  UINT32            Mode;
  UINT32            Flags;
  EFI_IPv4_ADDRESS  StationAddress;
  EFI_IPv4_ADDRESS  SubnetMask;
  EFI_IPv4_ADDRESS  GatewayAddress;
  EFI_IPv4_ADDRESS  DnsAddress;
} CEPH_RBD_NETWORK_CONFIG;

// Protocol-independent resolved local-IP configuration. Consumed by the TCP4
// layer to fill EFI_TCP4_ACCESS_POINT without this module depending on Tcp4.h.
typedef struct {
  BOOLEAN           UseDefaultAddress;
  EFI_IPv4_ADDRESS  StationAddress;
  EFI_IPv4_ADDRESS  SubnetMask;
} CEPH_RBD_LOCAL_IP_CONFIG;

EFI_STATUS
CephRbdNetworkValidate (
  IN CONST CEPH_RBD_NETWORK_CONFIG  *Config
  );

EFI_STATUS
CephRbdNetworkParse (
  IN  CONST VOID                *Buffer,
  IN  UINTN                     BufferLength,
  OUT CEPH_RBD_NETWORK_CONFIG   *Config
  );

// Load and validate the network configuration variable. Returns EFI_NOT_FOUND
// when the variable is absent or gRT is unavailable, in which case callers
// use the platform default (DHCP).
EFI_STATUS
CephRbdNetworkLoad (
  OUT CEPH_RBD_NETWORK_CONFIG  *Config
  );

// Resolve a network configuration into a protocol-independent local-IP block.
// Config == NULL, or Mode == DHCP, selects UseDefaultAddress TRUE.
EFI_STATUS
CephRbdNetworkResolveLocalIp (
  IN     CONST CEPH_RBD_NETWORK_CONFIG  *Config OPTIONAL,
  OUT    CEPH_RBD_LOCAL_IP_CONFIG       *LocalIp
  );

#endif
