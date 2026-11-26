#ifndef CEPH_RBD_CEPHX_AES256_H_
#define CEPH_RBD_CEPHX_AES256_H_

#include <Uefi.h>

// Ceph's CEPH_CRYPTO_AES256KRB5 (value 2) is not plain AES-256-CBC. It is the
// Kerberos-style AES-256-CTS-HMAC384-192 construction:
//
//   ciphertext = AES-256-CBC-CTS(Ke, confounder || plaintext) || HMAC
//   HMAC       = HMAC-SHA384(Ki, IV(16 zero bytes) || ciphertext)[0..23]
//
// Ke and Ki are derived from the 32-byte principal key with HMAC-SHA384 using
// the packed 14-byte "kx" structure from auth/Crypto.cc. CephX uses the
// CryptoKey default usage of 0 for all classic (messenger v1) encrypt/decrypt
// calls, so callers pass CEPHX_KRB5_USAGE_DEFAULT unless a protocol document
// states otherwise.
#define CEPHX_AES256_KEY_SIZE        32
#define CEPHX_KRB5_BLOCK_SIZE        16
#define CEPHX_KRB5_CONFOUNDER_SIZE   16
#define CEPHX_KRB5_HASH_SIZE         24
#define CEPHX_KRB5_USAGE_DEFAULT     0

// Encrypt plaintext with Ceph's AES256KRB5 handler. The output layout is
// AES-256-CBC-CTS(confounder || plaintext) followed by a 24-byte HMAC-SHA384
// tag. BufferCapacity must allow at least PlaintextLength + 16 + 24 bytes.
EFI_STATUS
CephXAes256Krb5Encrypt (
  IN  CONST UINT8  *Key,
  IN  UINTN        KeyLength,
  IN  UINT32       Usage,
  IN  CONST VOID   *Plaintext,
  IN  UINTN        PlaintextLength,
  OUT VOID         *Buffer,
  IN  UINTN        BufferCapacity,
  OUT UINTN        *BufferLength
  );

// Decrypt and authenticate a Ceph AES256KRB5 blob. Verifies the HMAC tag
// before decrypting, strips the 16-byte confounder and returns the plaintext.
EFI_STATUS
CephXAes256Krb5Decrypt (
  IN  CONST UINT8  *Key,
  IN  UINTN        KeyLength,
  IN  UINT32       Usage,
  IN  CONST VOID   *Ciphertext,
  IN  UINTN        CiphertextLength,
  OUT VOID         *Plaintext,
  IN  UINTN        PlaintextCapacity,
  OUT UINTN        *PlaintextLength
  );

#endif
