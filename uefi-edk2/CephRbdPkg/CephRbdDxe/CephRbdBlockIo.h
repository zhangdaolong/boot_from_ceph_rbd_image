#ifndef CEPH_RBD_BLOCK_IO_H_
#define CEPH_RBD_BLOCK_IO_H_

#include <Uefi.h>
#include "CephRbdFormat2.h"
#include "CephRbdMsgrV1Recovery.h"
#include "CephRbdRpc.h"
#include "CephRbdPlacement.h"
#include "CephRbdOsdClient.h"

#define CEPH_RBD_BLOCK_SIZE                  512
#define CEPH_RBD_MAX_READ_BLOCKS             ((CEPH_MSGR_V1_RETRANSMIT_FRAME_MAX - \
                                               CEPH_MSGR_V1_HEADER_SIZE - \
                                               CEPH_MSGR_V1_FOOTER_SIZE - 256) / \
                                              CEPH_RBD_BLOCK_SIZE)
#define CEPH_RBD_POOL_ID_MAX                 1024
#define CEPH_RBD_MAX_IMAGE_NAME              128
#define CEPH_RBD_MAX_OBJECT_NAME             CEPH_RBD_OBJECT_NAME_MAX
#define CEPH_RBD_HEADER_INFO_SIZE            64

typedef struct {
  UINT32                     PoolId;
  CHAR8                      ImageName[CEPH_RBD_MAX_IMAGE_NAME];
  UINTN                      ImageNameLength;
  UINT8                      Fsid[CEPH_FSID_SIZE];
  UINT64                     GlobalId;
  UINT32                     ClientIncarnation;
  UINT32                     OsdEpoch;
  CEPH_RBD_FORMAT2_IMAGE     Image;
  CEPH_RBD_PLACEMENT         Placement;
  INT32                      PrimaryOsd;
  BOOLEAN                    Ready;
} CEPH_RBD_BLOCK_DEVICE;

// Map a data object of an open image to its acting OSD set. Exposed so the
// driver and tests can verify placement independently of a live OSD session.
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
  );

// Discover image metadata through monitor/OSD RPCs. The caller supplies a
// live, authenticated messenger client. Only format 2 images with supported
// features are accepted.
EFI_STATUS
CephRbdBlockIoDiscover (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN     CEPH_RBD_OSD_CLIENT_POOL *OsdPool,
  IN UINT32                       PoolId,
  IN CONST CHAR8                  *ImageName,
  IN UINTN                        ImageNameLength,
  IN CONST UINT8                  *Fsid,
  OUT CEPH_RBD_BLOCK_DEVICE       *Device
  );

// Read one or more blocks from the image. Each extent is mapped to its acting
// OSD set; the read is sent over an authenticated session to that object's
// primary OSD (cached in OsdPool), not over the monitor connection.
EFI_STATUS
CephRbdBlockIoReadBlocks (
  IN     CEPH_RBD_OSD_CLIENT_POOL *OsdPool,
  IN OUT CEPH_RBD_BLOCK_DEVICE    *Device,
  IN UINT64                       StartLba,
  IN UINTN                        NumberOfBlocks,
  OUT VOID                        *Buffer,
  IN UINTN                        BufferSize
  );

// Return the media block count for the discovered image.
EFI_STATUS
CephRbdBlockIoGetMediaLastLba (
  IN CONST CEPH_RBD_BLOCK_DEVICE  *Device,
  OUT UINT64                      *LastLba
  );

// Decode the rbd/get_size class-method reply (order u8, size u64). Other
// metadata fields are zeroed and filled by separate class-method calls.
EFI_STATUS
CephRbdBlockIoDecodeHeader (
  IN CONST UINT8                 *Payload,
  IN UINTN                       PayloadLength,
  OUT CEPH_RBD_FORMAT2_METADATA  *Metadata
  );

#endif
