// AES-256-KRB5 CephX tests. The expected values are computed independently
// with OpenSSL's "AES-256-CBC-CTS" cipher in CS3 mode plus HMAC-SHA384 using
// the same key-derivation structure as Ceph's CryptoAES256KRB5KeyHandler.
#define OPENSSL_API_COMPAT 0x10100000L

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/params.h>
#include <openssl/core_names.h>

#include "CephRbdCephX.h"
#include "CephRbdCephXAes256.h"

static void
WriteBe32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

// Ceph auth/Crypto.cc calc_kx(): HMAC-SHA384(secret, packed 14-byte struct)
// truncated to k bytes.
static void
DeriveKey(const uint8_t *secret, uint32_t usage, uint8_t type,
          uint32_t bits, uint8_t *out, uint32_t out_len)
{
  uint8_t plain[14];
  uint8_t digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;

  plain[0] = 0; plain[1] = 0; plain[2] = 0; plain[3] = 1;
  WriteBe32(plain + 4, usage);
  plain[8] = type;
  plain[9] = 0;
  WriteBe32(plain + 10, bits);
  assert(HMAC(EVP_sha384(), secret, 32, plain, sizeof(plain),
              digest, &len) != NULL);
  assert(len == 48);
  memcpy(out, digest, out_len);
}

static void
CtsInit(EVP_CIPHER_CTX **ctx, const uint8_t *key, int enc)
{
  const EVP_CIPHER *cipher = EVP_CIPHER_fetch(NULL, "AES-256-CBC-CTS", NULL);
  OSSL_PARAM params[2];
  uint8_t iv[16] = {0};

  assert(cipher != NULL);
  *ctx = EVP_CIPHER_CTX_new();
  assert(*ctx != NULL);
  params[0] = OSSL_PARAM_construct_utf8_string(OSSL_CIPHER_PARAM_CTS_MODE,
                                               (char *)"CS3", 0);
  params[1] = OSSL_PARAM_construct_end();
  if (enc) {
    assert(EVP_EncryptInit_ex2(*ctx, cipher, key, iv, params) == 1);
  } else {
    assert(EVP_DecryptInit_ex2(*ctx, cipher, key, iv, params) == 1);
  }
  EVP_CIPHER_free((EVP_CIPHER *)cipher);
}

// Build the exact wire blob Ceph emits for usage 0 with a fixed confounder.
static size_t
OpenSslEncrypt(const uint8_t *secret, const uint8_t *pt, size_t pt_len,
               uint8_t *out)
{
  uint8_t ke[32], ki[24], conf[16], input[512];
  uint8_t iv[16] = {0}, digest[EVP_MAX_MD_SIZE];
  unsigned int hlen = 0;
  EVP_CIPHER_CTX *ctx = NULL;
  int n = 0, total = 0;
  size_t cipher_len;

  DeriveKey(secret, 0, 0xAA, 256, ke, sizeof(ke));
  DeriveKey(secret, 0, 0x55, 192, ki, sizeof(ki));
  memset(conf, 0xA5, sizeof(conf));
  memcpy(input, conf, sizeof(conf));
  memcpy(input + sizeof(conf), pt, pt_len);

  CtsInit(&ctx, ke, 1);
  assert(EVP_EncryptUpdate(ctx, out, &n, input,
                           (int)(sizeof(conf) + pt_len)) == 1);
  total = n;
  assert(EVP_EncryptFinal_ex(ctx, out + total, &n) == 1);
  total += n;
  EVP_CIPHER_CTX_free(ctx);
  cipher_len = (size_t)total;

  assert(HMAC(EVP_sha384(), ki, sizeof(ki), iv, sizeof(iv),
              digest, &hlen) != NULL);
  assert(HMAC(EVP_sha384(), ki, sizeof(ki), out, (int)cipher_len,
              digest, &hlen) != NULL);
  // The two HMAC calls above are separate; recompute over IV||ciphertext.
  {
    uint8_t source[512];
    memcpy(source, iv, sizeof(iv));
    memcpy(source + sizeof(iv), out, cipher_len);
    assert(HMAC(EVP_sha384(), ki, sizeof(ki), source,
                (int)(sizeof(iv) + cipher_len), digest, &hlen) != NULL);
  }
  assert(hlen == 48);
  memcpy(out + cipher_len, digest, 24);
  return cipher_len + 24;
}

static void
OpenSslDecrypt(const uint8_t *secret, const uint8_t *blob, size_t blob_len,
               uint8_t *out, size_t *out_len)
{
  uint8_t ke[32], ki[24], digest[EVP_MAX_MD_SIZE];
  uint8_t iv[16] = {0}, source[512];
  unsigned int hlen = 0;
  EVP_CIPHER_CTX *ctx = NULL;
  int n = 0, total = 0;
  size_t cipher_len = blob_len - 24;

  DeriveKey(secret, 0, 0xAA, 256, ke, sizeof(ke));
  DeriveKey(secret, 0, 0x55, 192, ki, sizeof(ki));
  memcpy(source, iv, sizeof(iv));
  memcpy(source + sizeof(iv), blob, cipher_len);
  assert(HMAC(EVP_sha384(), ki, sizeof(ki), source,
              (int)(sizeof(iv) + cipher_len), digest, &hlen) != NULL);
  assert(hlen == 48 && memcmp(digest, blob + cipher_len, 24) == 0);

  CtsInit(&ctx, ke, 0);
  assert(EVP_DecryptUpdate(ctx, out, &n, blob, (int)cipher_len) == 1);
  total = n;
  assert(EVP_DecryptFinal_ex(ctx, out + total, &n) == 1);
  total += n;
  EVP_CIPHER_CTX_free(ctx);
  assert((size_t)total > 16);
  memmove(out, out + 16, (size_t)total - 16);
  *out_len = (size_t)total - 16;
}

static void
TestAes256Primitive(void)
{
  uint8_t key[32];
  uint8_t pt[37];
  uint8_t expected[512], actual[512], plain[512];
  size_t expected_len, actual_len, plain_len;
  UINTN firmware_len;
  UINTN i;

  for (i = 0; i < sizeof(key); i++) {
    key[i] = (uint8_t)(i * 7 + 3);
  }
  for (i = 0; i < sizeof(pt); i++) {
    pt[i] = (uint8_t)(0x60 + i);
  }

  // OpenSSL -> firmware: validates KDF, CS3 CTS and HMAC verification.
  expected_len = OpenSslEncrypt(key, pt, sizeof(pt), expected);
  assert(CephXAes256Krb5Decrypt(key, sizeof(key), 0, expected, expected_len,
                                plain, sizeof(plain), &firmware_len) ==
         EFI_SUCCESS);
  assert(firmware_len == sizeof(pt));
  assert(memcmp(plain, pt, sizeof(pt)) == 0);

  // Firmware -> OpenSSL: validates our CTS encrypt and tag generation.
  assert(CephXAes256Krb5Encrypt(key, sizeof(key), 0, pt, sizeof(pt),
                                actual, sizeof(actual), &firmware_len) ==
         EFI_SUCCESS);
  actual_len = firmware_len;
  OpenSslDecrypt(key, actual, actual_len, plain, &plain_len);
  assert(plain_len == sizeof(pt) && memcmp(plain, pt, sizeof(pt)) == 0);

  // A flipped HMAC byte must be rejected.
  expected[expected_len - 1] ^= 1;
  assert(CephXAes256Krb5Decrypt(key, sizeof(key), 0, expected, expected_len,
                                plain, sizeof(plain), &firmware_len) ==
         EFI_COMPROMISED_DATA);

  // Block-aligned and short payloads exercise both CS3 branches.
  {
    uint8_t short_pt[3] = {1, 2, 3};
    uint8_t aligned_pt[16];
    memset(aligned_pt, 0x5a, sizeof(aligned_pt));
    assert(CephXAes256Krb5Encrypt(key, sizeof(key), 0, short_pt,
                                  sizeof(short_pt), actual, sizeof(actual),
                                  &firmware_len) == EFI_SUCCESS);
    assert(CephXAes256Krb5Decrypt(key, sizeof(key), 0, actual, firmware_len,
                                  plain, sizeof(plain), &firmware_len) ==
           EFI_SUCCESS);
    assert(firmware_len == sizeof(short_pt) &&
           memcmp(plain, short_pt, sizeof(short_pt)) == 0);
    assert(CephXAes256Krb5Encrypt(key, sizeof(key), 0, aligned_pt,
                                  sizeof(aligned_pt), actual, sizeof(actual),
                                  &firmware_len) == EFI_SUCCESS);
    assert(CephXAes256Krb5Decrypt(key, sizeof(key), 0, actual, firmware_len,
                                  plain, sizeof(plain), &firmware_len) ==
           EFI_SUCCESS);
    assert(firmware_len == sizeof(aligned_pt) &&
           memcmp(plain, aligned_pt, sizeof(aligned_pt)) == 0);
  }
}

static void
TestAes256Payload(void)
{
  uint8_t key[CEPHX_AES256_KEY_SIZE];
  uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  uint8_t encoded[256], decoded[64];
  UINTN encoded_len, decoded_len;
  UINTN i;

  for (i = 0; i < sizeof(key); i++) {
    key[i] = (uint8_t)(i + 1);
  }

  // A 32-byte secret must now drive the shared CephX encode_encrypt path.
  assert(CephXEncryptAes128Payload(key, sizeof(key), payload, sizeof(payload),
                                   encoded, sizeof(encoded), &encoded_len) ==
         EFI_SUCCESS);
  assert(CephXDecryptAes128Payload(key, sizeof(key), encoded, encoded_len,
                                   decoded, sizeof(decoded), &decoded_len) ==
         EFI_SUCCESS);
  assert(decoded_len == sizeof(payload) &&
         memcmp(decoded, payload, sizeof(payload)) == 0);

  // Wrong 32-byte key is rejected by the HMAC tag.
  key[0] ^= 1;
  assert(CephXDecryptAes128Payload(key, sizeof(key), encoded, encoded_len,
                                   decoded, sizeof(decoded), &decoded_len) ==
         EFI_COMPROMISED_DATA);
}

void
TestAes256Krb5(void)
{
  TestAes256Primitive();
  TestAes256Payload();
  puts("aes256-krb5 tests: passed");
}
