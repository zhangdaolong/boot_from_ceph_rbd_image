// Host-only socket implementation of the CephRbdTcp4 primitive. It lets the
// firmware messenger sources be exercised against a real Ceph v1 endpoint
// without changing the EDK II transport. Not part of the firmware build.
#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

static void
DumpBytes (const char *Label, const unsigned char *Data, size_t Length)
{
  size_t Index;

  if (getenv ("LIVE_DEBUG") == NULL) {
    return;
  }

  fprintf (stderr, "%s (%lu):", Label, (unsigned long)Length);
  for (Index = 0; Index < Length; Index++) {
    if ((Index % 16) == 0) {
      fprintf (stderr, "\n  ");
    }
    fprintf (stderr, "%02x ", Data[Index]);
  }

  fprintf (stderr, "\n");
}

#include <Uefi.h>
#include "CephRbdTcp4.h"

STATIC EFI_STATUS
SetTimeout (INT32 Fd, UINT32 TimeoutSeconds)
{
  struct timeval  Tv;

  Tv.tv_sec = (TimeoutSeconds != 0) ? (long)TimeoutSeconds : 5;
  Tv.tv_usec = 0;
  if ((setsockopt (Fd, SOL_SOCKET, SO_RCVTIMEO, &Tv, sizeof (Tv)) != 0) ||
      (setsockopt (Fd, SOL_SOCKET, SO_SNDTIMEO, &Tv, sizeof (Tv)) != 0))
  {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdTcp4Connect (
  IN EFI_HANDLE                         ImageHandle,
  IN EFI_HANDLE                         ControllerHandle,
  IN CONST EFI_IPv4_ADDRESS             *RemoteAddress,
  IN UINT16                             RemotePort,
  IN UINT32                             TimeoutSeconds,
  OUT CEPH_RBD_TCP4_CONNECTION           *Connection
  )
{
  INT32               Fd;
  struct sockaddr_in  Addr;
  EFI_STATUS          Status;

  (VOID)ImageHandle;
  (VOID)ControllerHandle;
  if ((RemoteAddress == NULL) || (Connection == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  memset (Connection, 0, sizeof (*Connection));
  Fd = socket (AF_INET, SOCK_STREAM, 0);
  if (Fd < 0) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = SetTimeout (Fd, TimeoutSeconds);
  if (EFI_ERROR (Status)) {
    close (Fd);
    return Status;
  }

  memset (&Addr, 0, sizeof (Addr));
  Addr.sin_family = AF_INET;
  Addr.sin_port = htons (RemotePort);
  memcpy (&Addr.sin_addr.s_addr, RemoteAddress->Addr, 4);
  if (connect (Fd, (struct sockaddr *)&Addr, sizeof (Addr)) != 0) {
    close (Fd);
    return EFI_CONNECTION_FIN;
  }

  Connection->Tcp4 = (EFI_TCP4_PROTOCOL *)(UINTN)Fd;
  Connection->Connected = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdTcp4Send (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN CONST VOID                *Buffer,
  IN UINTN                     BufferLength,
  IN UINT32                    TimeoutSeconds
  )
{
  INT32       Fd;
  CONST UINT8 *Data;
  UINTN       Left;
  ssize_t     Sent;

  if ((Connection == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Fd = (INT32)(UINTN)Connection->Tcp4;
  SetTimeout (Fd, TimeoutSeconds);
  DumpBytes ("SEND", Buffer, BufferLength);
  Data = Buffer;
  Left = BufferLength;
  while (Left != 0) {
    Sent = send (Fd, Data, Left, 0);
    if (Sent <= 0) {
      return (errno == EAGAIN || errno == EWOULDBLOCK) ?
             EFI_TIMEOUT : EFI_DEVICE_ERROR;
    }
    Data += Sent;
    Left -= (UINTN)Sent;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdTcp4Receive (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  OUT VOID                     *Buffer,
  IN UINTN                     BufferCapacity,
  IN UINT32                    TimeoutSeconds,
  OUT UINTN                    *ReceivedLength
  )
{
  INT32    Fd;
  ssize_t  Got;

  if ((Connection == NULL) || (Buffer == NULL) || (ReceivedLength == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Fd = (INT32)(UINTN)Connection->Tcp4;
  SetTimeout (Fd, TimeoutSeconds);
  Got = recv (Fd, Buffer, BufferCapacity, 0);
  if (Got < 0) {
    return (errno == EAGAIN || errno == EWOULDBLOCK) ?
           EFI_TIMEOUT : EFI_DEVICE_ERROR;
  }

  if (Got == 0) {
    return EFI_CONNECTION_FIN;
  }

  DumpBytes ("RECV", Buffer, (UINTN)Got);
  *ReceivedLength = (UINTN)Got;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdTcp4ReceiveExact (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  OUT VOID                     *Buffer,
  IN UINTN                     BufferLength,
  IN UINT32                    TimeoutSeconds
  )
{
  EFI_STATUS  Status;
  UINTN       Received;
  UINT8       *Data;

  if ((Connection == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Data = Buffer;
  while (BufferLength != 0) {
    Status = CephRbdTcp4Receive (
               Connection, Data, BufferLength, TimeoutSeconds, &Received
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Data += Received;
    BufferLength -= Received;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdTcp4Close (
  IN OUT CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN UINT32                        TimeoutSeconds
  )
{
  INT32  Fd;

  (VOID)TimeoutSeconds;
  if (Connection == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Connection->Tcp4 != NULL) {
    Fd = (INT32)(UINTN)Connection->Tcp4;
    close (Fd);
    Connection->Tcp4 = NULL;
  }

  Connection->Connected = FALSE;
  return EFI_SUCCESS;
}
