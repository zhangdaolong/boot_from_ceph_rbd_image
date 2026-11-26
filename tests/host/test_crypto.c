// The host stub intentionally uses the low-level AES/HMAC entry points that
// mirror EDK II's BaseCryptLib single-block API. Mark the OpenSSL 1.1 compat
// level so OpenSSL 3 headers do not emit deprecation warnings under -Werror.
#define OPENSSL_API_COMPAT 0x10100000L

#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <string.h>

#include <Library/BaseCryptLib.h>

typedef struct {
  UINT8   Key[32];
  UINTN   KeyBytes;
  AES_KEY EncryptKey;
  AES_KEY DecryptKey;
} TEST_AES_CONTEXT;

typedef struct {
  HMAC_CTX *Context;
} TEST_HMAC_CONTEXT;

UINTN
AesGetContextSize (
  VOID
  )
{
  return sizeof (TEST_AES_CONTEXT);
}

BOOLEAN
AesInit (
  OUT VOID        *AesContext,
  IN CONST UINT8  *Key,
  IN UINTN        KeyLength
  )
{
  TEST_AES_CONTEXT *Context;

  if ((AesContext == NULL) || (Key == NULL) ||
      ((KeyLength != 128) && (KeyLength != 192) && (KeyLength != 256)))
  {
    return FALSE;
  }

  Context = AesContext;
  memset (Context, 0, sizeof (*Context));
  Context->KeyBytes = KeyLength / 8;
  memcpy (Context->Key, Key, Context->KeyBytes);
  if ((AES_set_encrypt_key (Context->Key, (int)KeyLength,
                            &Context->EncryptKey) != 0) ||
      (AES_set_decrypt_key (Context->Key, (int)KeyLength,
                            &Context->DecryptKey) != 0))
  {
    return FALSE;
  }
  return TRUE;
}

BOOLEAN
AesCbcEncrypt (
  IN VOID         *AesContext,
  IN CONST UINT8  *Input,
  IN UINTN        InputSize,
  IN CONST UINT8  *Ivec,
  OUT UINT8       *Output
  )
{
  TEST_AES_CONTEXT *Context;
  EVP_CIPHER_CTX   *CipherContext;
  const EVP_CIPHER *Cipher;
  int              OutputLength;
  int              FinalLength;
  BOOLEAN          Result;

  if ((AesContext == NULL) || (Input == NULL) || (Ivec == NULL) ||
      (Output == NULL) || ((InputSize & 15) != 0) || (InputSize > INT_MAX))
  {
    return FALSE;
  }

  Context = AesContext;
  Cipher = (Context->KeyBytes == 32) ? EVP_aes_256_cbc () : EVP_aes_128_cbc ();
  CipherContext = EVP_CIPHER_CTX_new ();
  if (CipherContext == NULL) {
    return FALSE;
  }

  Result = (BOOLEAN)(
             (EVP_EncryptInit_ex (
                CipherContext, Cipher, NULL,
                Context->Key, Ivec) == 1) &&
             (EVP_CIPHER_CTX_set_padding (CipherContext, 0) == 1) &&
             (EVP_EncryptUpdate (
                CipherContext, Output, &OutputLength,
                Input, (int)InputSize) == 1) &&
             (EVP_EncryptFinal_ex (
                CipherContext, Output + OutputLength, &FinalLength) == 1) &&
             ((UINTN)(OutputLength + FinalLength) == InputSize));
  EVP_CIPHER_CTX_free (CipherContext);
  return Result;
}

BOOLEAN
AesCbcDecrypt (
  IN VOID         *AesContext,
  IN CONST UINT8  *Input,
  IN UINTN        InputSize,
  IN CONST UINT8  *Ivec,
  OUT UINT8       *Output
  )
{
  TEST_AES_CONTEXT *Context;
  EVP_CIPHER_CTX   *CipherContext;
  const EVP_CIPHER *Cipher;
  int              OutputLength;
  int              FinalLength;
  BOOLEAN          Result;

  if ((AesContext == NULL) || (Input == NULL) || (Ivec == NULL) ||
      (Output == NULL) || ((InputSize & 15) != 0) || (InputSize > INT_MAX))
  {
    return FALSE;
  }

  Context = AesContext;
  Cipher = (Context->KeyBytes == 32) ? EVP_aes_256_cbc () : EVP_aes_128_cbc ();
  CipherContext = EVP_CIPHER_CTX_new ();
  if (CipherContext == NULL) {
    return FALSE;
  }

  Result = (BOOLEAN)(
             (EVP_DecryptInit_ex (
                CipherContext, Cipher, NULL,
                Context->Key, Ivec) == 1) &&
             (EVP_CIPHER_CTX_set_padding (CipherContext, 0) == 1) &&
             (EVP_DecryptUpdate (
                CipherContext, Output, &OutputLength,
                Input, (int)InputSize) == 1) &&
             (EVP_DecryptFinal_ex (
                CipherContext, Output + OutputLength, &FinalLength) == 1) &&
             ((UINTN)(OutputLength + FinalLength) == InputSize));
  EVP_CIPHER_CTX_free (CipherContext);
  return Result;
}

BOOLEAN
AesEncrypt (
  IN VOID         *AesContext,
  IN CONST UINT8  *Input,
  OUT UINT8       *Output
  )
{
  TEST_AES_CONTEXT *Context;

  if ((AesContext == NULL) || (Input == NULL) || (Output == NULL)) {
    return FALSE;
  }
  Context = AesContext;
  AES_encrypt (Input, Output, &Context->EncryptKey);
  return TRUE;
}

BOOLEAN
AesDecrypt (
  IN VOID         *AesContext,
  IN CONST UINT8  *Input,
  OUT UINT8       *Output
  )
{
  TEST_AES_CONTEXT *Context;

  if ((AesContext == NULL) || (Input == NULL) || (Output == NULL)) {
    return FALSE;
  }
  Context = AesContext;
  AES_decrypt (Input, Output, &Context->DecryptKey);
  return TRUE;
}

VOID *
HmacSha384New (
  VOID
  )
{
  TEST_HMAC_CONTEXT *Context;

  Context = calloc (1, sizeof (*Context));
  if (Context == NULL) {
    return NULL;
  }
  Context->Context = HMAC_CTX_new ();
  if (Context->Context == NULL) {
    free (Context);
    return NULL;
  }
  return Context;
}

VOID
HmacSha384Free (
  IN VOID  *HmacSha384Ctx
  )
{
  TEST_HMAC_CONTEXT *Context;

  if (HmacSha384Ctx == NULL) {
    return;
  }
  Context = HmacSha384Ctx;
  if (Context->Context != NULL) {
    HMAC_CTX_free (Context->Context);
  }
  free (Context);
}

BOOLEAN
HmacSha384SetKey (
  OUT VOID        *HmacSha384Context,
  IN CONST UINT8  *Key,
  IN UINTN        KeySize
  )
{
  TEST_HMAC_CONTEXT *Context;

  if ((HmacSha384Context == NULL) || (Key == NULL) || (KeySize > INT_MAX)) {
    return FALSE;
  }
  Context = HmacSha384Context;
  return (BOOLEAN)(
           HMAC_Init_ex (
             Context->Context, Key, (int)KeySize, EVP_sha384 (), NULL
             ) == 1);
}

BOOLEAN
HmacSha384Update (
  IN OUT VOID        *HmacSha384Context,
  IN CONST VOID      *Data,
  IN UINTN           DataSize
  )
{
  TEST_HMAC_CONTEXT *Context;

  if ((HmacSha384Context == NULL) || (Data == NULL) || (DataSize > INT_MAX)) {
    return FALSE;
  }
  Context = HmacSha384Context;
  return (BOOLEAN)(
           HMAC_Update (Context->Context, Data, (int)DataSize) == 1);
}

BOOLEAN
HmacSha384Final (
  IN OUT VOID   *HmacSha384Context,
  OUT UINT8     *HmacValue
  )
{
  TEST_HMAC_CONTEXT *Context;
  UINTN             Length;

  if ((HmacSha384Context == NULL) || (HmacValue == NULL)) {
    return FALSE;
  }
  Context = HmacSha384Context;
  Length = 0;
  return (BOOLEAN)(
           (HMAC_Final (Context->Context, HmacValue,
                        (unsigned int *)&Length) == 1) &&
           (Length == 48));
}
