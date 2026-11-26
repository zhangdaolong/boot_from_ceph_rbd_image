#ifndef CEPH_RBD_SECRET_RESOLVER_H_
#define CEPH_RBD_SECRET_RESOLVER_H_

#include <Uefi.h>

// Platform secret resolver for CEPH_RBD_CONFIG_FLAG_KEY_REFERENCE.
//
// When the KEY_REFERENCE flag is set, CEPH_RBD_CONFIG.Key holds an opaque
// ASCII reference string instead of raw key material. This module resolves
// that reference into raw AES key bytes (16 for AES-128, 32 for AES-256-KRB5).
//
// The reference is interpreted as the name of a UEFI variable stored under
// gCephRbdSecretGuid whose data is the raw key bytes. This gives platforms a
// single, well-defined protected-storage hook: firmware can back the variable
// with an authenticated variable, an TPM-sealed store, or a management-engine
// protected region. A generic DXE driver cannot know platform-specific secret
// storage, so all access is routed through a backend table that host tests can
// mock and platforms can replace.

#define CEPH_RBD_SECRET_MAX_KEY_SIZE   32
#define CEPH_RBD_SECRET_MAX_REF_CHARS  128

extern EFI_GUID  gCephRbdSecretGuid;

typedef
EFI_STATUS
(EFIAPI *CEPH_RBD_RESOLVE_SECRET) (
  IN     CONST CHAR8  *Reference,
  IN     UINTN        ReferenceLength,
  OUT    UINT8        *Secret,
  IN     UINTN        SecretCapacity,
  OUT    UINTN        *SecretLength
  );

typedef struct {
  CEPH_RBD_RESOLVE_SECRET  ResolveSecret;
} CEPH_RBD_SECRET_RESOLVER_BACKEND;

// Populate Backend with the default gRT-backed resolver. Not compiled in host
// tests (whose stubs do not expose all runtime services); host tests provide
// their own backend table.
#ifndef CEPH_RBD_SECRET_RESOLVER_HOST_TEST
EFI_STATUS
CephRbdSecretResolverGetEfiBackend (
  OUT CEPH_RBD_SECRET_RESOLVER_BACKEND  *Backend
  );
#endif

// Resolve a key reference into raw key bytes. Validates the result is either
// 16 or 32 bytes. Reference must be a non-empty ASCII string.
EFI_STATUS
CephRbdSecretResolve (
  IN     CONST CEPH_RBD_SECRET_RESOLVER_BACKEND  *Backend,
  IN     CONST CHAR8                            *Reference,
  IN     UINTN                                  ReferenceLength,
  OUT    UINT8                                  *Secret,
  IN     UINTN                                  SecretCapacity,
  OUT    UINTN                                  *SecretLength
  );

#endif
