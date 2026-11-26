#ifndef CEPH_STAGE2_NETWORK_H_
#define CEPH_STAGE2_NETWORK_H_

#include <stddef.h>
#include <stdint.h>

#include "pxe_undi.h"

/*
 * Network transport used by stage 2 to reach a Ceph monitor. When no !PXE
 * structure is found the loader leaves this transport unavailable and keeps
 * its existing disk-based behaviour.
 */
typedef struct {
  PXE_UNDI_SEND_FRAME Send;
  PXE_UNDI_RECEIVE_FRAME Receive;
  void *Context;
} CEPH_BIOS_NETWORK;

/* Stage 2 network status codes, separate from the disk-path CEPH_BIOS_*. */
#define CEPH_BIOS_NETWORK_NOT_AVAILABLE (-100)

/*
 * Try to bring up a PXE/UNDI network transport. Returns CEPH_BIOS_SUCCESS on
 * success, CEPH_BIOS_NETWORK_NOT_AVAILABLE when PXE/UNDI is absent.
 */
int
CephBiosStage2InitNetwork(
  const PXE_UNDI_PLATFORM *Platform,
  CEPH_BIOS_NETWORK *Network
  );

/* Tear down the transport opened by CephBiosStage2InitNetwork. */
int
CephBiosStage2ShutdownNetwork(
  CEPH_BIOS_NETWORK *Network
  );

/* Send one prebuilt Ethernet frame through the stage 2 transport. */
int
CephBiosStage2SendFrame(
  CEPH_BIOS_NETWORK *Network,
  const uint8_t *Frame,
  size_t Length
  );

/* Receive one Ethernet frame through the stage 2 transport. */
int
CephBiosStage2ReceiveFrame(
  CEPH_BIOS_NETWORK *Network,
  uint8_t *Frame,
  size_t *Length
  );

#endif
