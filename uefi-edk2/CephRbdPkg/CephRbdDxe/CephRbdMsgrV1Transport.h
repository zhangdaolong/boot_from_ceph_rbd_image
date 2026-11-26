#ifndef CEPH_RBD_MSGR_V1_TRANSPORT_H_
#define CEPH_RBD_MSGR_V1_TRANSPORT_H_

#include <Uefi.h>
#include "CephRbdMessenger.h"
#include "CephRbdTcp4.h"

EFI_STATUS
CephMsgrV1SendFrame (
  IN CEPH_RBD_TCP4_CONNECTION     *Connection,
  IN CONST CEPH_MSGR_V1_FRAME     *Frame,
  IN UINTN                        MaximumPayload,
  IN UINT32                       TimeoutSeconds,
  OUT VOID                        *FrameBuffer,
  IN UINTN                        FrameBufferCapacity,
  OUT UINTN                       *FrameLength
  );

EFI_STATUS
CephMsgrV1ReceiveFrame (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN UINTN                     MaximumPayload,
  IN UINTN                     FooterSize,
  IN UINT32                    TimeoutSeconds,
  OUT VOID                     *FrameBuffer,
  IN UINTN                     FrameBufferCapacity,
  OUT CEPH_MSGR_V1_FRAME       *Frame,
  OUT UINTN                    *FrameLength
  );

EFI_STATUS
CephMsgrV1ReceiveFrameAfterTag (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN UINT8                     Tag,
  IN UINTN                     MaximumPayload,
  IN UINTN                     FooterSize,
  IN UINT32                    TimeoutSeconds,
  OUT VOID                     *FrameBuffer,
  IN UINTN                     FrameBufferCapacity,
  OUT CEPH_MSGR_V1_FRAME       *Frame,
  OUT UINTN                    *FrameLength
  );

#endif
