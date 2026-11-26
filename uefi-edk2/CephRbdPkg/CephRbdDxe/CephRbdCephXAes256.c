#include <Uefi.h>
#include <Library/BaseCryptLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include "CephRbdCephX.h"
#include "CephRbdCephXAes256.h"

// Ceph's CryptoAES256KRB5KeyHandler (auth/Crypto.cc) derives two usage-bound
// subkeys from the 32-byte principal key with HMAC-SHA384. The packed input is
//   u8  prefix[4] = {0,0,0,1}; u32 be usage; u8 type; u8 c = 0; u32 be bits.
// Ki uses type 0x55 and 24 bytes; Ke uses type 0xAA and 32 bytes.
STATIC
EFI_STATUS
CephXAes256Krb5DeriveKey (
  IN  CONST UINT8  *Secret,
  IN  UINTN        SecretLength,
  IN  UINT32       Usage,
  IN  UINT8        Type,
  IN  UINT32       KeyBits,
  OUT UINT8        *Derived,
  IN  UINTN        DerivedLength
  )
{
  UINT8   Plain[14];
  UINT8   Digest[48];
  VOID    *Context;
  BOOLEAN Result;

  if ((Secret == NULL) || (Derived == NULL) ||
      (SecretLength != CEPHX_AES256_KEY_SIZE) ||
      (DerivedLength == 0) || (DerivedLength > sizeof (Digest)) ||
      (KeyBits > MAX_UINT32))
  {
    return EFI_INVALID_PARAMETER;
  }

  Plain[0] = 0;
  Plain[1] = 0;
  Plain[2] = 0;
  Plain[3] = 1;
  Plain[4] = (UINT8)(Usage >> 24);
  Plain[5] = (UINT8)(Usage >> 16);
  Plain[6] = (UINT8)(Usage >> 8);
  Plain[7] = (UINT8)Usage;
  Plain[8] = Type;
  Plain[9] = 0;
  Plain[10] = (UINT8)(KeyBits >> 24);
  Plain[11] = (UINT8)(KeyBits >> 16);
  Plain[12] = (UINT8)(KeyBits >> 8);
  Plain[13] = (UINT8)KeyBits;

  Context = HmacSha384New ();
  if (Context == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Result = HmacSha384SetKey (Context, Secret, SecretLength);
  if (Result) {
    Result = HmacSha384Update (Context, Plain, sizeof (Plain));
  }
  if (Result) {
    Result = HmacSha384Final (Context, Digest);
  }
  HmacSha384Free (Context);
  ZeroMem (Plain, sizeof (Plain));
  if (!Result) {
    ZeroMem (Digest, sizeof (Digest));
    return EFI_UNSUPPORTED;
  }

  CopyMem (Derived, Digest, DerivedLength);
  ZeroMem (Digest, sizeof (Digest));
  return EFI_SUCCESS;
}

// Ceph uses OpenSSL's "AES-256-CBC-CTS" cipher with CTS_MODE=CS3, zero IV and
// no padding. In OpenSSL, EVP's "CS3" maps to CRYPTO_cts128_* (RFC 3962),
// which swaps the last two ciphertext blocks: for an input of 16*n + r bytes
// the output is C0..C(n-2) || T || stolen, where T = E(C(n-1) ^ partial_pt)
// and "stolen" is the first r bytes of C(n-1). These routines mirror
// OpenSSL's CRYPTO_cts128_encrypt_block/decrypt_block exactly.
STATIC
EFI_STATUS
CephXAes256CbcCts3Encrypt (
  IN  CONST UINT8  *Key,
  IN  UINTN        KeyLength,
  IN  CONST UINT8  *Input,
  IN  UINTN        InputLength,
  OUT UINT8        *Output
  )
{
  VOID    *Context;
  UINTN   ContextSize;
  UINTN   Full;
  UINTN   Residue;
  UINTN   Index;
  UINT8   Iv[16];
  UINT8   Block[16];
  UINT8   Tail[16];
  BOOLEAN Result;

  if (InputLength <= 16) {
    return EFI_INVALID_PARAMETER;
  }

  ContextSize = AesGetContextSize ();
  if (ContextSize == 0) {
    return EFI_UNSUPPORTED;
  }
  Context = AllocateZeroPool (ContextSize);
  if (Context == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  if (!AesInit (Context, Key, KeyLength * 8)) {
    ZeroMem (Context, ContextSize);
    FreePool (Context);
    return EFI_UNSUPPORTED;
  }

  ZeroMem (Iv, sizeof (Iv));
  Residue = InputLength & 15;
  if (Residue == 0) {
    Residue = 16;
  }
  Full = InputLength - Residue;
  Result = TRUE;

  // CBC over the complete blocks. AesInit() also installed the decrypt key,
  // so the same single-block context can be used for both directions.
  for (Index = 0; Index < Full; Index += 16) {
    Block[0] = Input[Index] ^ Iv[0];
    Block[1] = Input[Index + 1] ^ Iv[1];
    Block[2] = Input[Index + 2] ^ Iv[2];
    Block[3] = Input[Index + 3] ^ Iv[3];
    Block[4] = Input[Index + 4] ^ Iv[4];
    Block[5] = Input[Index + 5] ^ Iv[5];
    Block[6] = Input[Index + 6] ^ Iv[6];
    Block[7] = Input[Index + 7] ^ Iv[7];
    Block[8] = Input[Index + 8] ^ Iv[8];
    Block[9] = Input[Index + 9] ^ Iv[9];
    Block[10] = Input[Index + 10] ^ Iv[10];
    Block[11] = Input[Index + 11] ^ Iv[11];
    Block[12] = Input[Index + 12] ^ Iv[12];
    Block[13] = Input[Index + 13] ^ Iv[13];
    Block[14] = Input[Index + 14] ^ Iv[14];
    Block[15] = Input[Index + 15] ^ Iv[15];
    Result = AesEncrypt (Context, Block, Output + Index);
    if (!Result) {
      break;
    }
    CopyMem (Iv, Output + Index, sizeof (Iv));
  }

  if (!Result) {
    ZeroMem (Context, ContextSize);
    FreePool (Context);
    return EFI_UNSUPPORTED;
  }

  // RFC 3962 / CS3 ciphertext stealing: XOR the partial plaintext into the
  // running IV (= last full ciphertext block C(Full-16..Full-1)) and encrypt
  // it to produce T. Then copy the first Residue bytes of the last full
  // ciphertext block to Output[Full..] (the "stolen" tail) and overwrite the
  // last full ciphertext block with T.
  for (Index = 0; Index < Residue; Index++) {
    Iv[Index] ^= Input[Full + Index];
  }
  Result = AesEncrypt (Context, Iv, Tail);
  if (Result) {
    CopyMem (Output + Full, Output + Full - 16, Residue);
    CopyMem (Output + Full - 16, Tail, sizeof (Tail));
  }

  ZeroMem (Tail, sizeof (Tail));
  ZeroMem (Block, sizeof (Block));
  ZeroMem (Iv, sizeof (Iv));
  ZeroMem (Context, ContextSize);
  FreePool (Context);
  return Result ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
CephXAes256CbcCts3Decrypt (
  IN  CONST UINT8  *Key,
  IN  UINTN        KeyLength,
  IN  CONST UINT8  *Input,
  IN  UINTN        InputLength,
  OUT UINT8        *Output
  )
{
  VOID    *Context;
  UINTN   ContextSize;
  UINTN   Full;
  UINTN   Residue;
  UINTN   Index;
  UINT8   Iv[16];
  UINT8   Block[16];
  UINT8   Tail[16];
  UINT8   Saved[16];
  UINT8   Cipher;
  BOOLEAN Result;

  if (InputLength <= 16) {
    return EFI_INVALID_PARAMETER;
  }

  ContextSize = AesGetContextSize ();
  if (ContextSize == 0) {
    return EFI_UNSUPPORTED;
  }
  Context = AllocateZeroPool (ContextSize);
  if (Context == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  if (!AesInit (Context, Key, KeyLength * 8)) {
    ZeroMem (Context, ContextSize);
    FreePool (Context);
    return EFI_UNSUPPORTED;
  }

  ZeroMem (Iv, sizeof (Iv));
  Residue = InputLength & 15;
  if (Residue == 0) {
    Residue = 16;
  }
  Result = TRUE;

  // A CTS input may contain exactly the tail (16-byte full block + residue),
  // leaving zero leading CBC blocks. OpenSSL's CRYPTO_cts128_decrypt_block()
  // permits this; do not reject it here. InputLength > 16 is guaranteed above.
  Full = InputLength - 16 - Residue;

  for (Index = 0; Index < Full; Index += 16) {
    Result = AesDecrypt (Context, Input + Index, Block);
    if (!Result) {
      break;
    }
    Output[Index] = Block[0] ^ Iv[0];
    Output[Index + 1] = Block[1] ^ Iv[1];
    Output[Index + 2] = Block[2] ^ Iv[2];
    Output[Index + 3] = Block[3] ^ Iv[3];
    Output[Index + 4] = Block[4] ^ Iv[4];
    Output[Index + 5] = Block[5] ^ Iv[5];
    Output[Index + 6] = Block[6] ^ Iv[6];
    Output[Index + 7] = Block[7] ^ Iv[7];
    Output[Index + 8] = Block[8] ^ Iv[8];
    Output[Index + 9] = Block[9] ^ Iv[9];
    Output[Index + 10] = Block[10] ^ Iv[10];
    Output[Index + 11] = Block[11] ^ Iv[11];
    Output[Index + 12] = Block[12] ^ Iv[12];
    Output[Index + 13] = Block[13] ^ Iv[13];
    Output[Index + 14] = Block[14] ^ Iv[14];
    Output[Index + 15] = Block[15] ^ Iv[15];
    CopyMem (Iv, Input + Index, sizeof (Iv));
  }

  if (!Result) {
    ZeroMem (Context, ContextSize);
    FreePool (Context);
    return EFI_UNSUPPORTED;
  }

  // RFC 3962 / CS3 decrypt tail (mirrors CRYPTO_cts128_decrypt_block).
  // Layout after the CBC blocks: Input[Full .. Full+15] = T (full swapped
  // block), Input[Full+16 .. Full+16+Residue-1] = stolen bytes.
  //   tmp[16..31] = D_K(T)
  //   tmp[0..15]  = D_K(T); tmp[0..Residue-1] = stolen bytes
  //   tmp[0..15]  = D_K(tmp[0..15]) = Saved
  //   for n in [0,16): c=in[n]; out[n]=tmp[n]^iv[n]; iv[n]=c
  //   for n in [16,16+Res): out[n]=tmp[n]^in[n]
  // Note: unlike the NIST variant, cts128 does NOT overwrite tmp[n] with c,
  // so Saved[0..15] stays intact for the second loop. In OpenSSL tmp.c[16..31]
  // still holds D_K(T) (Tail), therefore tmp[n] for n>=16 is Tail[n-16], and
  // in[n] is the stolen ciphertext at Input[Full+n]. iv = last CBC ciphertext.
  Result = AesDecrypt (Context, Input + Full, Tail);
  if (!Result) {
    ZeroMem (Context, ContextSize);
    FreePool (Context);
    return EFI_UNSUPPORTED;
  }
  CopyMem (Block, Tail, sizeof (Block));
  CopyMem (Block, Input + Full + 16, Residue);
  Result = AesDecrypt (Context, Block, Saved);
  if (!Result) {
    ZeroMem (Context, ContextSize);
    FreePool (Context);
    return EFI_UNSUPPORTED;
  }

  for (Index = 0; Index < 16; Index++) {
    Cipher = Input[Full + Index];
    Output[Full + Index] = Saved[Index] ^ Iv[Index];
    Iv[Index] = Cipher;
  }
  for (; Index < 16 + Residue; Index++) {
    Output[Full + Index] = Tail[Index - 16] ^ Input[Full + Index];
  }

  ZeroMem (Saved, sizeof (Saved));
  ZeroMem (Tail, sizeof (Tail));
  ZeroMem (Block, sizeof (Block));
  ZeroMem (Iv, sizeof (Iv));
  ZeroMem (Context, ContextSize);
  FreePool (Context);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CephXAes256Krb5Hmac (
  IN  CONST UINT8  *Key,
  IN  UINTN        KeyLength,
  IN  CONST UINT8  *Data,
  IN  UINTN        DataLength,
  OUT UINT8        *Tag
  )
{
  UINT8   ZeroIv[16];
  VOID    *Context;
  UINT8   Digest[48];
  BOOLEAN Result;

  ZeroMem (ZeroIv, sizeof (ZeroIv));
  Context = HmacSha384New ();
  if (Context == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Result = HmacSha384SetKey (Context, Key, KeyLength);
  if (Result) {
    Result = HmacSha384Update (Context, ZeroIv, sizeof (ZeroIv));
  }
  if (Result && (DataLength > 0)) {
    Result = HmacSha384Update (Context, Data, DataLength);
  }
  if (Result) {
    Result = HmacSha384Final (Context, Digest);
  }
  HmacSha384Free (Context);
  if (!Result) {
    ZeroMem (Digest, sizeof (Digest));
    return EFI_UNSUPPORTED;
  }

  CopyMem (Tag, Digest, CEPHX_KRB5_HASH_SIZE);
  ZeroMem (Digest, sizeof (Digest));
  return EFI_SUCCESS;
}

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
  )
{
  EFI_STATUS     Status;
  UINT8          Ke[CEPHX_AES256_KEY_SIZE];
  UINT8          Ki[CEPHX_KRB5_HASH_SIZE];
  UINT8          Input[CEPHX_MAX_ENCRYPTED_PAYLOAD];
  UINT8          Tag[CEPHX_KRB5_HASH_SIZE];
  UINTN          InputLength;
  UINTN          Index;
  CONST UINT8    *Bytes;

  if ((Key == NULL) || (Plaintext == NULL) || (Buffer == NULL) ||
      (BufferLength == NULL) || (PlaintextLength == 0) ||
      (PlaintextLength > CEPHX_MAX_ENCRYPTED_PAYLOAD -
                         CEPHX_KRB5_CONFOUNDER_SIZE))
  {
    return EFI_INVALID_PARAMETER;
  }
  if (KeyLength != CEPHX_AES256_KEY_SIZE) {
    return EFI_UNSUPPORTED;
  }

  *BufferLength = 0;
  InputLength = CEPHX_KRB5_CONFOUNDER_SIZE + PlaintextLength;
  if (BufferCapacity < InputLength + CEPHX_KRB5_HASH_SIZE) {
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = CephXAes256Krb5DeriveKey (
             Key, KeyLength, Usage, 0xAA, 256, Ke, sizeof (Ke)
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  Status = CephXAes256Krb5DeriveKey (
             Key, KeyLength, Usage, 0x55, 192, Ki, sizeof (Ki)
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  // Ceph draws a random confounder. Firmware has no guaranteed entropy source
  // at this layer; derive a per-plaintext deterministic confounder from the
  // two subkeys. This is wire-compatible (the receiver never interprets the
  // confounder) but deployments with a hardware RNG should override it.
  Bytes = (CONST UINT8 *)Plaintext;
  for (Index = 0; Index < CEPHX_KRB5_CONFOUNDER_SIZE; Index++) {
    Input[Index] = (UINT8)(Ke[Index] ^ Ki[Index % sizeof (Ki)] ^
                           Bytes[Index % PlaintextLength]);
  }
  CopyMem (Input + CEPHX_KRB5_CONFOUNDER_SIZE, Plaintext, PlaintextLength);

  Status = CephXAes256CbcCts3Encrypt (
             Ke, sizeof (Ke), Input, InputLength, (UINT8 *)Buffer
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = CephXAes256Krb5Hmac (Ki, sizeof (Ki), Buffer, InputLength, Tag);
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  CopyMem ((UINT8 *)Buffer + InputLength, Tag, sizeof (Tag));
  *BufferLength = InputLength + sizeof (Tag);

Done:
  ZeroMem (Input, sizeof (Input));
  ZeroMem (Tag, sizeof (Tag));
  ZeroMem (Ke, sizeof (Ke));
  ZeroMem (Ki, sizeof (Ki));
  return Status;
}

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
  )
{
  EFI_STATUS Status;
  UINT8      Ke[CEPHX_AES256_KEY_SIZE];
  UINT8      Ki[CEPHX_KRB5_HASH_SIZE];
  UINT8      Plain[CEPHX_MAX_ENCRYPTED_PAYLOAD];
  UINT8      Tag[CEPHX_KRB5_HASH_SIZE];
  UINTN      CipherLength;
  UINTN      OutputLength;

  if ((Key == NULL) || (Ciphertext == NULL) || (Plaintext == NULL) ||
      (PlaintextLength == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }
  if (KeyLength != CEPHX_AES256_KEY_SIZE) {
    return EFI_UNSUPPORTED;
  }

  *PlaintextLength = 0;
  if (CiphertextLength < CEPHX_KRB5_CONFOUNDER_SIZE +
                          CEPHX_KRB5_HASH_SIZE + 1)
  {
    return EFI_BAD_BUFFER_SIZE;
  }
  CipherLength = CiphertextLength - CEPHX_KRB5_HASH_SIZE;
  if (CipherLength > sizeof (Plain)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = CephXAes256Krb5DeriveKey (
             Key, KeyLength, Usage, 0xAA, 256, Ke, sizeof (Ke)
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  Status = CephXAes256Krb5DeriveKey (
             Key, KeyLength, Usage, 0x55, 192, Ki, sizeof (Ki)
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = CephXAes256Krb5Hmac (
             Ki, sizeof (Ki), Ciphertext, CipherLength, Tag
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  if (CompareMem (
        Tag,
        (CONST UINT8 *)Ciphertext + CipherLength,
        sizeof (Tag)
        ) != 0)
  {
    Status = EFI_COMPROMISED_DATA;
    goto Done;
  }

  Status = CephXAes256CbcCts3Decrypt (
             Ke, sizeof (Ke), Ciphertext, CipherLength, Plain
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (CipherLength <= CEPHX_KRB5_CONFOUNDER_SIZE) {
    Status = EFI_COMPROMISED_DATA;
    goto Done;
  }
  OutputLength = CipherLength - CEPHX_KRB5_CONFOUNDER_SIZE;
  if (OutputLength > PlaintextCapacity) {
    Status = EFI_BUFFER_TOO_SMALL;
    goto Done;
  }
  CopyMem (Plaintext, Plain + CEPHX_KRB5_CONFOUNDER_SIZE, OutputLength);
  *PlaintextLength = OutputLength;

Done:
  ZeroMem (Plain, sizeof (Plain));
  ZeroMem (Tag, sizeof (Tag));
  ZeroMem (Ke, sizeof (Ke));
  ZeroMem (Ki, sizeof (Ki));
  return Status;
}
