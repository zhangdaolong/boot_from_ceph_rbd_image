#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "CephRbdSecretResolver.h"

// gCephRbdSecretGuid is generated from CephRbdPkg.dec by EDK II AutoGen in
// firmware builds and supplied by the host test file in host builds.

EFI_STATUS
CephRbdSecretResolve (
  IN     CONST CEPH_RBD_SECRET_RESOLVER_BACKEND  *Backend,
  IN     CONST CHAR8                            *Reference,
  IN     UINTN                                  ReferenceLength,
  OUT    UINT8                                  *Secret,
  IN     UINTN                                  SecretCapacity,
  OUT    UINTN                                  *SecretLength
  )
{
  EFI_STATUS  Status;

  if ((Backend == NULL) || (Backend->ResolveSecret == NULL) ||
      (Reference == NULL) || (ReferenceLength == 0) ||
      (Secret == NULL) || (SecretLength == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }
  if (ReferenceLength > CEPH_RBD_SECRET_MAX_REF_CHARS) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (SecretCapacity < 16) {
    return EFI_BUFFER_TOO_SMALL;
  }

  ZeroMem (Secret, SecretCapacity);
  *SecretLength = 0;

  Status = Backend->ResolveSecret (
                      Reference,
                      ReferenceLength,
                      Secret,
                      SecretCapacity,
                      SecretLength
                      );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // CephX accepts only AES-128 (16-byte) or AES-256-KRB5 (32-byte) secrets.
  if ((*SecretLength != 16) && (*SecretLength != 32)) {
    ZeroMem (Secret, SecretCapacity);
    *SecretLength = 0;
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

#ifndef CEPH_RBD_SECRET_RESOLVER_HOST_TEST

// Convert an ASCII reference string to a NUL-terminated CHAR16 variable name.
STATIC
EFI_STATUS
AsciiToVariableName (
  IN     CONST CHAR8  *Reference,
  IN     UINTN        ReferenceLength,
  OUT    CHAR16       *Name,
  IN     UINTN        NameCapacity
  )
{
  UINTN  Index;

  if (NameCapacity < (ReferenceLength + 1)) {
    return EFI_BUFFER_TOO_SMALL;
  }
  for (Index = 0; Index < ReferenceLength; Index++) {
    if ((Reference[Index] < 0x20) || (Reference[Index] > 0x7e)) {
      return EFI_INVALID_PARAMETER;
    }
    Name[Index] = (CHAR16)Reference[Index];
  }
  Name[ReferenceLength] = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
EfiResolveSecret (
  IN     CONST CHAR8  *Reference,
  IN     UINTN        ReferenceLength,
  OUT    UINT8        *Secret,
  IN     UINTN        SecretCapacity,
  OUT    UINTN        *SecretLength
  )
{
  EFI_STATUS  Status;
  CHAR16      Name[CEPH_RBD_SECRET_MAX_REF_CHARS + 1];
  UINTN       DataSize;

  Status = AsciiToVariableName (
             Reference, ReferenceLength, Name,
             sizeof (Name) / sizeof (Name[0])
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (gRT == NULL) {
    return EFI_NOT_FOUND;
  }

  DataSize = SecretCapacity;
  Status = gRT->GetVariable (
                  Name,
                  &gCephRbdSecretGuid,
                  NULL,
                  &DataSize,
                  Secret
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  *SecretLength = DataSize;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdSecretResolverGetEfiBackend (
  OUT CEPH_RBD_SECRET_RESOLVER_BACKEND  *Backend
  )
{
  if (Backend == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Backend, sizeof (*Backend));
  Backend->ResolveSecret = EfiResolveSecret;
  return EFI_SUCCESS;
}

#endif  // !CEPH_RBD_SECRET_RESOLVER_HOST_TEST
