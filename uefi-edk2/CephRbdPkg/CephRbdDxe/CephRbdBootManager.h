#ifndef CEPH_RBD_BOOT_MANAGER_H_
#define CEPH_RBD_BOOT_MANAGER_H_

#include <Uefi.h>

// ---------------------------------------------------------------------------
// Device path types
// ---------------------------------------------------------------------------
// CephRbdBootManager.c is compiled both inside EDK II and against the host
// test stubs. Define only the one piece of EDK II device-path infrastructure
// that the public API needs (EFI_DEVICE_PATH_PROTOCOL) when building on the
// host. Use the same struct tag as Protocol/DriverBinding.h's forward
// declaration so the two headers can coexist.
#ifdef CEPH_RBD_BOOT_MANAGER_HOST_TEST
typedef struct _EFI_DEVICE_PATH_PROTOCOL {
  UINT8  Type;
  UINT8  SubType;
  UINT8  Length[2];
} EFI_DEVICE_PATH_PROTOCOL;
#else
#include <Protocol/DevicePath.h>
#endif

#include "CephRbdGpt.h"

// Layout-compatible mirrors of MdePkg/Include/Protocol/DevicePath.h. They are
// given local names so this module never collides with the EDK II definitions
// while still producing byte-identical device paths. Packed so sizeof() is
// the on-wire size on every compiler (the encoder also uses explicit offsets).
#pragma pack(1)
typedef struct {
  EFI_DEVICE_PATH_PROTOCOL  Header;      // MEDIA_DEVICE_PATH / MEDIA_HARDDRIVE_DP / 42
  UINT32                    PartitionNumber;
  UINT64                    PartitionStart;
  UINT64                    PartitionSize;
  UINT8                     Signature[16];
  UINT8                     MBRType;
  UINT8                     SignatureType;
} CEPH_RBD_BM_HARDDRIVE_DEVICE_PATH;

typedef struct {
  EFI_DEVICE_PATH_PROTOCOL  Header;      // MEDIA_DEVICE_PATH / MEDIA_FILEPATH_DP
  CHAR16                    PathName[1];
} CEPH_RBD_BM_FILEPATH_DEVICE_PATH;

typedef struct {
  EFI_DEVICE_PATH_PROTOCOL  Header;      // END_DEVICE_PATH_TYPE / END_ENTIRE_DP_SUBTYPE / 4
} CEPH_RBD_BM_END_DEVICE_PATH;
#pragma pack()

#define CEPH_RBD_BM_MEDIA_DEVICE_PATH      0x01
#define CEPH_RBD_BM_MEDIA_HARDDRIVE_DP     0x01
#define CEPH_RBD_BM_MEDIA_FILEPATH_DP      0x04
#define CEPH_RBD_BM_END_DEVICE_PATH_TYPE   0x7f
#define CEPH_RBD_BM_END_DP_SUBTYPE         0xff
#define CEPH_RBD_BM_MBR_TYPE_GPT           0x02
#define CEPH_RBD_BM_SIGNATURE_TYPE_GUID    0x02
#define CEPH_RBD_BM_MAX_LOADER_PATH_CHARS  255

#ifndef LOAD_OPTION_ACTIVE
#define LOAD_OPTION_ACTIVE                 0x00000001
#endif
#ifndef EFI_VARIABLE_NON_VOLATILE
#define EFI_VARIABLE_NON_VOLATILE          0x00000001
#define EFI_VARIABLE_BOOTSERVICE_ACCESS    0x00000002
#define EFI_VARIABLE_RUNTIME_ACCESS        0x00000004
#endif

#define CEPH_RBD_BM_BOOT_VARIABLE_ATTRS    \
  (EFI_VARIABLE_NON_VOLATILE |            \
   EFI_VARIABLE_BOOTSERVICE_ACCESS |      \
   EFI_VARIABLE_RUNTIME_ACCESS)

// 0xffff means "pick the lowest free Boot#### index".
#define CEPH_RBD_BM_AUTO_BOOT_INDEX        0xffff

// ---------------------------------------------------------------------------
// Host-testable UEFI backend
// ---------------------------------------------------------------------------
// All firmware service access goes through this table. Production code
// obtains a table backed by gBS/gRT with CephRbdBootManagerGetEfiBackend();
// host tests fill the same table with their own mocks. Keeping the table
// here avoids any direct gBS->LoadImage / gRT->SetVariable references in the
// portable core (the host stubs do not declare those members).
typedef
EFI_STATUS
(EFIAPI *CEPH_RBD_BM_GET_VARIABLE) (
  IN     CONST CHAR16    *VariableName,
  IN     CONST EFI_GUID  *VendorGuid,
  OUT OPTIONAL UINT32    *Attributes,
  IN OUT UINTN           *DataSize,
  OUT OPTIONAL VOID      *Data
  );

typedef
EFI_STATUS
(EFIAPI *CEPH_RBD_BM_SET_VARIABLE) (
  IN CONST CHAR16    *VariableName,
  IN CONST EFI_GUID  *VendorGuid,
  IN UINT32           Attributes,
  IN UINTN            DataSize,
  IN CONST VOID      *Data       // DataSize == 0 && Data == NULL deletes
  );

typedef
VOID *
(EFIAPI *CEPH_RBD_BM_ALLOCATE_POOL) (
  IN UINTN  Size
  );

typedef
VOID *
(EFIAPI *CEPH_RBD_BM_ALLOCATE_ZERO_POOL) (
  IN UINTN  Size
  );

typedef
VOID
(EFIAPI *CEPH_RBD_BM_FREE_POOL) (
  IN VOID  *Buffer
  );

typedef
EFI_STATUS
(EFIAPI *CEPH_RBD_BM_LOAD_IMAGE) (
  IN     BOOLEAN                        BootPolicy,
  IN     EFI_HANDLE                     ParentImageHandle,
  IN     CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN     VOID                           *SourceBuffer OPTIONAL,
  IN     UINTN                          SourceSize,
  OUT    EFI_HANDLE                     *ImageHandle
  );

typedef
EFI_STATUS
(EFIAPI *CEPH_RBD_BM_START_IMAGE) (
  IN     EFI_HANDLE    ImageHandle,
  OUT OPTIONAL UINTN   *ExitDataSize,
  OUT OPTIONAL CHAR16  **ExitData
  );

typedef
EFI_STATUS
(EFIAPI *CEPH_RBD_BM_UNLOAD_IMAGE) (
  IN EFI_HANDLE  ImageHandle
  );

typedef struct {
  CEPH_RBD_BM_GET_VARIABLE       GetVariable;
  CEPH_RBD_BM_SET_VARIABLE       SetVariable;
  // Named Pool* rather than AllocatePool/FreePool because the host test
  // stubs define those as function-like macros.
  CEPH_RBD_BM_ALLOCATE_POOL      PoolAllocate;
  CEPH_RBD_BM_ALLOCATE_ZERO_POOL PoolAllocateZero;
  CEPH_RBD_BM_FREE_POOL          PoolFree;
  CEPH_RBD_BM_LOAD_IMAGE         LoadImage;
  CEPH_RBD_BM_START_IMAGE        StartImage;
  CEPH_RBD_BM_UNLOAD_IMAGE       UnloadImage;
} CEPH_RBD_BOOT_MANAGER_BACKEND;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Populate Backend with wrappers around the real gBS/gRT. Not compiled in
// host-test builds; use a mock table there instead.
#ifndef CEPH_RBD_BOOT_MANAGER_HOST_TEST
EFI_STATUS
CephRbdBootManagerGetEfiBackend (
  OUT CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend
  );
#endif

// Convert 16 GPT on-disk GUID bytes (mixed-endian) to an EFI_GUID. The GPT
// byte order is identical to the in-memory EFI_GUID layout on the target, so
// this is a straight 16-byte copy.
VOID
CephRbdBootManagerGuidFromGptBytes (
  IN     CONST UINT8  *GptGuidBytes,
  OUT    EFI_GUID     *Guid
  );

// Build HD(GPT) + FilePath + End for one loader on the ESP.
// PartitionStartLba/PartitionSizeLba are in 512-byte (BlockIo) units.
// LoaderPath == NULL selects \EFI\BOOT\BOOTX64.EFI via CephRbdGptBuildEspBootPath;
// a caller-supplied NUL-terminated CHAR16 string selects an alternate loader.
EFI_STATUS
CephRbdBootManagerBuildLoaderDevicePath (
  IN     CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN     UINT32                               PartitionNumber,
  IN     UINT64                               PartitionStartLba,
  IN     UINT64                               PartitionSizeLba,
  IN     CONST EFI_GUID                       *PartitionSignature,
  IN     CONST CHAR16                         *LoaderPath OPTIONAL,
  OUT    EFI_DEVICE_PATH_PROTOCOL             **DevicePath,
  OUT    UINTN                                *DevicePathSize
  );

EFI_STATUS
CephRbdBootManagerFreeDevicePath (
  IN     CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN     EFI_DEVICE_PATH_PROTOCOL             *DevicePath
  );

// Encode Attributes/FilePathListLength/Description/FilePathList/OptionalData.
EFI_STATUS
CephRbdBootManagerBuildLoadOption (
  IN     CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN     CONST CHAR16                         *Description OPTIONAL,
  IN     CONST EFI_DEVICE_PATH_PROTOCOL       *DevicePath,
  IN     UINTN                                DevicePathSize,
  IN     CONST VOID                           *OptionalData OPTIONAL,
  IN     UINTN                                OptionalDataSize,
  OUT    VOID                                 **LoadOption,
  OUT    UINTN                                *LoadOptionSize
  );

// Create or update one Boot#### option and register it in BootOrder.
// RequestedBootIndex == CEPH_RBD_BM_AUTO_BOOT_INDEX picks the lowest free
// index; otherwise the caller's index is used (an existing entry is updated).
EFI_STATUS
CephRbdBootManagerInstallLoadOption (
  IN     CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN     CONST CHAR16                         *Description OPTIONAL,
  IN     CONST EFI_DEVICE_PATH_PROTOCOL       *DevicePath,
  IN     UINTN                                DevicePathSize,
  IN     UINT16                               RequestedBootIndex,
  OUT OPTIONAL UINT16                         *BootIndex
  );

// Delete Boot#### and remove every occurrence from BootOrder. Idempotent.
EFI_STATUS
CephRbdBootManagerRemoveLoadOption (
  IN     CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN     UINT16                               BootIndex
  );

// Optional immediate handoff: LoadImage then StartImage, with UnloadImage
// on every path where an image handle was produced. Never called by
// CephRbdBootManagerHandoff() unless TryBoot is TRUE.
EFI_STATUS
CephRbdBootManagerTryBoot (
  IN CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN EFI_HANDLE                           ParentImageHandle,
  IN CONST EFI_DEVICE_PATH_PROTOCOL       *DevicePath
  );

// One-call handoff from a discovered ESP. Derives the partition signature
// from Esp->UniqueGuid, uses Esp->FirstLba/LastLba, installs the variable and
// optionally loads the image. Device-path/load-option allocations are freed
// before returning.
EFI_STATUS
CephRbdBootManagerHandoff (
  IN     CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN     CONST CEPH_RBD_GPT_PARTITION         *Esp,
  IN     UINT32                               PartitionNumber,
  IN     UINT16                               RequestedBootIndex,
  IN     BOOLEAN                              TryBoot,
  IN     EFI_HANDLE                           ParentImageHandle,
  OUT OPTIONAL UINT16                         *BootIndex
  );

#endif
