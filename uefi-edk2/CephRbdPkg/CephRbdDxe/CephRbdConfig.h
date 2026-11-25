#ifndef CEPH_RBD_CONFIG_H_
#define CEPH_RBD_CONFIG_H_

#include <Uefi.h>

// Firmware configuration is a fixed-size, validated blob stored in a UEFI
// variable. Keeping the layout fixed avoids dynamic allocation in DXE and
// makes the same parser usable by host tests and by firmware.
#define CEPH_RBD_CONFIG_MAX_MONITORS     4
#define CEPH_RBD_CONFIG_MAX_IMAGE_NAME   96
#define CEPH_RBD_CONFIG_MAX_IDENTITY     64
#define CEPH_RBD_CONFIG_MAX_KEY          128
#define CEPH_RBD_CONFIG_FSID_SIZE        16

#define CEPH_RBD_CONFIG_SIGNATURE        0x52424443U  // 'CDBR'
#define CEPH_RBD_CONFIG_VERSION          1U

// The key field is an opaque credential reference (for example a protected
// variable name), not raw key material.
#define CEPH_RBD_CONFIG_FLAG_KEY_REFERENCE  0x00000001U
// Attempt to map the image directly from the DXE entry point. Without this
// flag the driver only publishes its Driver Binding protocol and waits for
// the UEFI driver model to select a controller.
#define CEPH_RBD_CONFIG_FLAG_START_ON_ENTRY 0x00000002U

typedef struct {
  EFI_IPv4_ADDRESS  Address;
  UINT16            Port;
  UINT16            Reserved;
} CEPH_RBD_MONITOR_ENTRY;

typedef struct {
  UINT32                  Signature;
  UINT32                  Version;
  UINT32                  Length;
  UINT32                  Flags;
  UINT8                   Fsid[CEPH_RBD_CONFIG_FSID_SIZE];
  UINT32                  PoolId;
  UINT32                  MonitorCount;
  CEPH_RBD_MONITOR_ENTRY  Monitors[CEPH_RBD_CONFIG_MAX_MONITORS];
  UINT32                  ImageNameLength;
  CHAR8                   ImageName[CEPH_RBD_CONFIG_MAX_IMAGE_NAME];
  UINT32                  IdentityLength;
  CHAR8                   Identity[CEPH_RBD_CONFIG_MAX_IDENTITY];
  UINT32                  KeyLength;
  UINT8                   Key[CEPH_RBD_CONFIG_MAX_KEY];
} CEPH_RBD_CONFIG;

extern EFI_GUID       gCephRbdConfigGuid;
extern CONST CHAR16   gCephRbdConfigVariableName[];

// Validate a fully populated configuration structure.
EFI_STATUS
CephRbdConfigValidate (
  IN CONST CEPH_RBD_CONFIG  *Config
  );

// Copy a raw variable blob into Config after checking its size and fields.
EFI_STATUS
CephRbdConfigParse (
  IN  CONST VOID       *Buffer,
  IN  UINTN             BufferLength,
  OUT CEPH_RBD_CONFIG   *Config
  );

// Load and validate the configuration from its UEFI variable. Returns
// EFI_NOT_FOUND when the variable is absent or gRT is unavailable.
EFI_STATUS
CephRbdConfigLoad (
  OUT CEPH_RBD_CONFIG  *Config
  );

#endif
