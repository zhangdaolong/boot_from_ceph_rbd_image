#ifndef CEPH_RBD_TCP4_H_
#define CEPH_RBD_TCP4_H_

#include <Uefi.h>
#include <Protocol/ServiceBinding.h>
#include <Protocol/Tcp4.h>

typedef struct {
  EFI_SERVICE_BINDING_PROTOCOL  *ServiceBinding;
  EFI_HANDLE                    ImageHandle;
  EFI_HANDLE                    ControllerHandle;
  EFI_HANDLE                    ChildHandle;
  EFI_TCP4_PROTOCOL             *Tcp4;
  BOOLEAN                       ServiceBindingOpen;
  BOOLEAN                       ProtocolOpen;
  BOOLEAN                       Configured;
  BOOLEAN                       Connected;
} CEPH_RBD_TCP4_CONNECTION;

EFI_STATUS
CephRbdTcp4Connect (
  IN EFI_HANDLE                         ImageHandle,
  IN EFI_HANDLE                         ControllerHandle,
  IN CONST EFI_IPv4_ADDRESS             *RemoteAddress,
  IN UINT16                             RemotePort,
  IN UINT32                             TimeoutSeconds,
  OUT CEPH_RBD_TCP4_CONNECTION         *Connection
  );

EFI_STATUS
CephRbdTcp4Send (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN CONST VOID                *Buffer,
  IN UINTN                     BufferLength,
  IN UINT32                    TimeoutSeconds
  );

EFI_STATUS
CephRbdTcp4Receive (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  OUT VOID                    *Buffer,
  IN UINTN                     BufferCapacity,
  IN UINT32                    TimeoutSeconds,
  OUT UINTN                    *ReceivedLength
  );

EFI_STATUS
CephRbdTcp4Close (
  IN OUT CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN UINT32                        TimeoutSeconds
  );

#endif
