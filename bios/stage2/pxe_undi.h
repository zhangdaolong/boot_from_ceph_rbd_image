#ifndef PXE_UNDI_H_
#define PXE_UNDI_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Legacy BIOS PXE 2.1 / UNDI access layer.
 *
 * This header describes the real-mode !PXE structure, the PXENV_UNDI_*
 * parameter blocks and a small platform abstraction so that the far-call ABI
 * can be exercised by host-side tests with a mock UNDI implementation.
 *
 * Status values returned by this layer. They are deliberately distinct from
 * the CEPH_BIOS_* stage 2 codes.
 */
enum {
  PXE_UNDI_SUCCESS = 0,
  PXE_UNDI_INVALID_ARGUMENT = -1,
  PXE_UNDI_NOT_FOUND = -2,
  PXE_UNDI_BAD_CHECKSUM = -3,
  PXE_UNDI_BAD_SIGNATURE = -4,
  PXE_UNDI_NOT_READY = -5,
  PXE_UNDI_CALL_FAILED = -6,
  PXE_UNDI_UNSUPPORTED = -7,
  PXE_UNDI_BUFFER_TOO_SMALL = -8,
  PXE_UNDI_NO_PACKET = -9,
  PXE_UNDI_NO_NETWORK = -10
};

/* PXE 2.1 UNDI API function codes (PXENV_UNDI_*). */
#define PXENV_UNDI_STARTUP              0x0001u
#define PXENV_UNDI_CLEANUP              0x0002u
#define PXENV_UNDI_INITIALIZE           0x0003u
#define PXENV_UNDI_RESET_ADAPTER        0x0004u
#define PXENV_UNDI_SHUTDOWN             0x0005u
#define PXENV_UNDI_OPEN                 0x0006u
#define PXENV_UNDI_CLOSE                0x0007u
#define PXENV_UNDI_TRANSMIT             0x0008u
#define PXENV_UNDI_SET_MCAST_ADDRESS    0x0009u
#define PXENV_UNDI_SET_STATION_ADDRESS  0x000au
#define PXENV_UNDI_SET_PACKET_FILTER    0x000bu
#define PXENV_UNDI_GET_INFORMATION      0x000cu
#define PXENV_UNDI_GET_STATISTICS       0x000du
#define PXENV_UNDI_CLEAR_STATISTICS     0x000eu
#define PXENV_UNDI_INITIATE_DIAGS       0x000fu
#define PXENV_UNDI_FORCE_INTERRUPT      0x0010u
#define PXENV_UNDI_GET_MCAST_ADDRESS    0x0011u
#define PXENV_UNDI_GET_NIC_TYPE         0x0012u
#define PXENV_UNDI_GET_IFACE_INFO       0x0013u
#define PXENV_UNDI_ISR                  0x0014u
#define PXENV_UNDI_GET_STATE            0x0015u

/*
 * PXE 2.1 has no standalone receive function; received frames are drained
 * with PXENV_UNDI_ISR. PXENV_UNDI_RECEIVE is this layer's logical name for
 * that operation, used to keep the transport surface symmetric with
 * PXENV_UNDI_TRANSMIT.
 */
#define PXENV_UNDI_RECEIVE              PXENV_UNDI_ISR

/* Receive packet filter flags (PXENV_UNDI_OPEN / SET_PACKET_FILTER). */
#define PXE_UNDI_FLTR_DIRECTED          0x0001u
#define PXE_UNDI_FLTR_BROADCAST         0x0002u
#define PXE_UNDI_FLTR_PROMISCUOUS       0x0004u
#define PXE_UNDI_FLTR_SOURCE_ROUTE      0x0008u

/* Transmit flags. The frame buffer already carries the media header. */
#define PXE_UNDI_XMT_DESTADDR           0x0000u
#define PXE_UNDI_XMT_BROADCAST          0x0001u

/* Transmit protocol values. P_UNKNOWN means the media header is prebuilt. */
#define PXE_UNDI_PROTO_UNKNOWN          0x00u
#define PXE_UNDI_PROTO_IP               0x01u
#define PXE_UNDI_PROTO_ARP              0x02u
#define PXE_UNDI_PROTO_RARP             0x03u

/* UNDI ISR function flags. */
#define PXE_UNDI_ISR_START              0x0000u
#define PXE_UNDI_ISR_GET_NEXT           0x0001u

/* NIC type codes returned by PXENV_UNDI_GET_NIC_TYPE. */
#define PXE_UNDI_NIC_PCI                0x02u
#define PXE_UNDI_NIC_PNP                0x03u
#define PXE_UNDI_NIC_CARDBUS            0x04u

#define PXE_UNDI_MAX_MCAST_ADDR         8u
#define PXE_UNDI_MAX_DATA_BLOCKS        8u
#define PXE_UNDI_MAC_SIZE               6u
#define PXE_UNDI_ETHERNET_MTU           1500u

#if defined(__GNUC__) || defined(__clang__)
#define PXE_UNDI_PACKED __attribute__((packed))
#else
#define PXE_UNDI_PACKED
#endif

/* A real-mode 16:16 far pointer as stored in PXE parameter blocks. */
typedef struct {
  uint16_t Offset;
  uint16_t Segment;
} PXE_UNDI_PACKED PXE_UNDI_FAR_PTR;

/*
 * The !PXE structure as consumed by a legacy 16-bit NBP: signature, length,
 * checksum, revision, selectors and the real-mode PXEnv entry point. The
 * checksum is the byte sum over StructLength bytes and must be zero.
 */
typedef struct {
  uint8_t Signature[4];
  uint8_t StructLength;
  uint8_t Checksum;
  uint8_t Revision;
  uint8_t Reserved1;
  uint16_t UndiCIDataSelector;
  uint16_t PXEnvSelector;
  uint16_t StackSize;
  uint16_t StackSeg;
  uint16_t UndiCodeSeg;
  uint16_t UndiCodeOffset;
  uint16_t UndiDataSeg;
  uint16_t PXEnvCodeSeg;
  uint16_t PXEnvCodeOffset;
  uint16_t Reserved2[3];
} PXE_UNDI_PACKED PXE_UNDI_PXE_STRUCT;

#define PXE_UNDI_PXE_SIGNATURE_0        '!'
#define PXE_UNDI_PXE_SIGNATURE_1        'P'
#define PXE_UNDI_PXE_SIGNATURE_2        'X'
#define PXE_UNDI_PXE_SIGNATURE_3        'E'
#define PXE_UNDI_PXE_MIN_LENGTH         8u
#define PXE_UNDI_PXE_MAX_LENGTH         64u

/* PCI/CardBus NIC description returned by PXENV_UNDI_GET_NIC_TYPE. */
typedef struct {
  uint16_t VendorId;
  uint16_t DeviceId;
  uint8_t BaseClass;
  uint8_t SubClass;
  uint8_t ProgIntf;
  uint8_t Revision;
  uint16_t BusDevFunc;
  uint16_t SubVendorId;
  uint16_t SubDeviceId;
} PXE_UNDI_PACKED PXE_UNDI_PCI_NIC_INFO;

/* ISAPnP NIC description returned by PXENV_UNDI_GET_NIC_TYPE. */
typedef struct {
  uint32_t EisaDevId;
  uint8_t BaseClass;
  uint8_t SubClass;
  uint8_t ProgIntf;
  uint16_t CardSelectNumber;
} PXE_UNDI_PACKED PXE_UNDI_PNP_NIC_INFO;

/* PXENV_UNDI_GET_NIC_TYPE parameter block. */
typedef struct {
  uint16_t Status;
  uint8_t NicType;
  PXE_UNDI_PCI_NIC_INFO Pci;
  PXE_UNDI_PNP_NIC_INFO Pnp;
} PXE_UNDI_PACKED PXENV_UNDI_GET_NIC_TYPE_PARAMS;

/* PXENV_UNDI_GET_INFORMATION parameter block. */
typedef struct {
  uint16_t Status;
  uint16_t BaseIo;
  uint16_t IntNumber;
  uint16_t MaxTranUnit;
  uint16_t HwType;
  uint16_t HwAddrLen;
  uint8_t CurrentNodeAddress[PXE_UNDI_MAC_SIZE];
  uint8_t PermNodeAddress[PXE_UNDI_MAC_SIZE];
  uint16_t RomAddress;
  uint16_t RxBufCount;
  uint16_t TxBufCount;
} PXE_UNDI_PACKED PXENV_UNDI_GET_INFORMATION_PARAMS;

/* PXENV_UNDI_STARTUP / CLOSE / SHUTDOWN parameter block. */
typedef struct {
  uint16_t Status;
} PXE_UNDI_PACKED PXENV_UNDI_SIMPLE_PARAMS;

/* PXENV_UNDI_INITIALIZE parameter block. */
typedef struct {
  uint16_t Status;
  PXE_UNDI_FAR_PTR ProtocolIni;
  uint8_t Reserved[8];
} PXE_UNDI_PACKED PXENV_UNDI_INITIALIZE_PARAMS;

/* PXENV_UNDI_SET_STATION_ADDRESS parameter block. */
typedef struct {
  uint16_t Status;
  uint8_t StationAddress[PXE_UNDI_MAC_SIZE];
} PXE_UNDI_PACKED PXENV_UNDI_SET_STATION_ADDRESS_PARAMS;

/* Multicast address list used by OPEN and SET_MCAST_ADDRESS. */
typedef struct {
  uint16_t MCastAddrCount;
  uint8_t McastAddr[PXE_UNDI_MAX_MCAST_ADDR][PXE_UNDI_MAC_SIZE];
} PXE_UNDI_PACKED PXENV_UNDI_MCAST_ADDRESS;

/* PXENV_UNDI_OPEN parameter block. */
typedef struct {
  uint16_t Status;
  uint16_t OpenFlag;
  uint16_t PktFilter;
  PXENV_UNDI_MCAST_ADDRESS Multicast;
} PXE_UNDI_PACKED PXENV_UNDI_OPEN_PARAMS;

/* One transmit data block inside the TBD. */
typedef struct {
  uint8_t TDPtrType;
  uint8_t TDRsvdByte;
  uint16_t TDDataLen;
  PXE_UNDI_FAR_PTR TDDataPtr;
} PXE_UNDI_PACKED PXENV_UNDI_TBD_DATA_BLOCK;

/* Transmit Buffer Descriptor referenced by PXENV_UNDI_TRANSMIT. */
typedef struct {
  uint16_t ImmedLength;
  PXE_UNDI_FAR_PTR Xmit;
  uint16_t DataBlkCount;
  PXENV_UNDI_TBD_DATA_BLOCK DataBlock[PXE_UNDI_MAX_DATA_BLOCKS];
} PXE_UNDI_PACKED PXENV_UNDI_TBD;

/* PXENV_UNDI_TRANSMIT parameter block. */
typedef struct {
  uint16_t Status;
  uint8_t Protocol;
  uint8_t XmitFlag;
  PXE_UNDI_FAR_PTR DestAddr;
  PXE_UNDI_FAR_PTR TBD;
  uint32_t Reserved;
} PXE_UNDI_PACKED PXENV_UNDI_TRANSMIT_PARAMS;

/* PXENV_UNDI_ISR (logical PXENV_UNDI_RECEIVE) parameter block. */
typedef struct {
  uint16_t Status;
  uint16_t FuncFlag;
  PXE_UNDI_FAR_PTR Frame;
  uint16_t FrameLength;
  uint16_t ProtocolType;
  uint16_t PacketType;
  uint16_t FrameAccepted;
} PXE_UNDI_PACKED PXENV_UNDI_ISR_PARAMS;

/* Ping-pong buffer used by the mock receive path. */
typedef struct {
  uint8_t Bytes[PXE_UNDI_ETHERNET_MTU + 14u];
  size_t Length;
} PXE_UNDI_FRAME_BUFFER;

/*
 * Platform abstraction for the real-mode far-call ABI.
 *
 * ReadBiosMemory: read physical memory (the BIOS ROM area) into Buffer.
 * CallApi:        perform the !PXE far call with an opcode and parameter
 *                 block. Returns the PXE status word; when ApiExit is not
 *                 NULL it also receives the 16-bit API exit value.
 * MakeFarPointer / FarPointerToHost: translate between host addresses and
 *                 the 16:16 far pointers stored in UNDI parameter blocks.
 */
typedef struct {
  void *Context;
  int (*ReadBiosMemory)(void *Context, uint32_t Address, void *Buffer,
                        size_t Length);
  uint16_t (*CallApi)(void *Context, uint16_t Opcode, void *ParamBlock,
                      uint16_t ParamBlockSize, uint16_t *ApiExit);
  PXE_UNDI_FAR_PTR (*MakeFarPointer)(void *Context, const void *HostAddress);
  void *(*FarPointerToHost)(void *Context, PXE_UNDI_FAR_PTR FarPointer);
} PXE_UNDI_PLATFORM;

/* Runtime state produced by discovery and consumed by every UNDI call. */
typedef struct {
  PXE_UNDI_PLATFORM Platform;
  PXE_UNDI_PXE_STRUCT Pxe;
  uint32_t PhysicalAddress;
  PXE_UNDI_FAR_PTR EntryPoint;
  int Ready;
} PXE_UNDI_CONTEXT;

/* BIOS ROM scan window (segments 0xC000-0xFFFF, covering the 0xF000 area). */
#define PXE_UNDI_BIOS_ROM_START         0x000c0000u
#define PXE_UNDI_BIOS_ROM_END           0x00100000u

/* USB-style link layer transport handed to the stage 2 loader. */
typedef int (*PXE_UNDI_SEND_FRAME)(void *Context, const uint8_t *Frame,
                                   size_t Length);
typedef int (*PXE_UNDI_RECEIVE_FRAME)(void *Context, uint8_t *Frame,
                                      size_t *Length);

/*
 * Table of PXE/UNDI operations. Defined as zeroed in ceph_stage2.c and
 * populated by PxeUndiInstallNetOps(), which keeps the stage 2 core free of
 * link-time references to this module.
 */
typedef struct {
  int (*Discover)(const PXE_UNDI_PLATFORM *Platform, PXE_UNDI_CONTEXT *Context);
  int (*Startup)(PXE_UNDI_CONTEXT *Context);
  int (*Initialize)(PXE_UNDI_CONTEXT *Context, const void *ProtocolIni);
  int (*GetNicType)(PXE_UNDI_CONTEXT *Context,
                    PXENV_UNDI_GET_NIC_TYPE_PARAMS *Out);
  int (*GetInformation)(PXE_UNDI_CONTEXT *Context,
                        PXENV_UNDI_GET_INFORMATION_PARAMS *Out);
  int (*SetStationAddress)(PXE_UNDI_CONTEXT *Context, const uint8_t Mac[6]);
  int (*Open)(PXE_UNDI_CONTEXT *Context, uint16_t Filter);
  int (*Close)(PXE_UNDI_CONTEXT *Context);
  int (*Shutdown)(PXE_UNDI_CONTEXT *Context);
  int (*SetPacketFilter)(PXE_UNDI_CONTEXT *Context, uint16_t Filter);
  int (*SetMulticast)(PXE_UNDI_CONTEXT *Context,
                      const uint8_t (*MacList)[6], unsigned Count);
  PXE_UNDI_SEND_FRAME SendFrame;
  PXE_UNDI_RECEIVE_FRAME ReceiveFrame;
} PXE_UNDI_NET_OPS;

extern PXE_UNDI_NET_OPS gCephBiosPxeUndiOps;

/* Discovery and validation. */
int PxeUndiDiscover(const PXE_UNDI_PLATFORM *Platform,
                    PXE_UNDI_CONTEXT *Context);
int PxeUndiValidateChecksum(const PXE_UNDI_PXE_STRUCT *Pxe);

/* PXENV_UNDI_* wrappers. */
int PxeUndiStartup(PXE_UNDI_CONTEXT *Context);
int PxeUndiInitialize(PXE_UNDI_CONTEXT *Context, const void *ProtocolIni);
int PxeUndiGetNicType(PXE_UNDI_CONTEXT *Context,
                      PXENV_UNDI_GET_NIC_TYPE_PARAMS *Out);
int PxeUndiGetInformation(PXE_UNDI_CONTEXT *Context,
                          PXENV_UNDI_GET_INFORMATION_PARAMS *Out);
int PxeUndiSetStationAddress(PXE_UNDI_CONTEXT *Context, const uint8_t Mac[6]);
int PxeUndiOpen(PXE_UNDI_CONTEXT *Context, uint16_t Filter);
int PxeUndiClose(PXE_UNDI_CONTEXT *Context);
int PxeUndiShutdown(PXE_UNDI_CONTEXT *Context);
int PxeUndiSetPacketFilter(PXE_UNDI_CONTEXT *Context, uint16_t Filter);
int PxeUndiSetMulticast(PXE_UNDI_CONTEXT *Context,
                        const uint8_t (*MacList)[6], unsigned Count);

/* Ethernet frame transport. */
int PxeUndiSendFrame(void *Context, const uint8_t *Frame, size_t Length);
int PxeUndiReceiveFrame(void *Context, uint8_t *Frame, size_t *Length);

/* Register the operations table into the stage 2 core. */
void PxeUndiInstallNetOps(void);

#endif
