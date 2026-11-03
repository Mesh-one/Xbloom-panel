#include "cloud_crypto.h"

#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <string.h>

#include <vector>

namespace cloudcrypto {

// The 1024-bit public key every xBloom client uses (from xbloom-py's cloud.py).
const char XBLOOM_PUBLIC_KEY_PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQC4LF40GZ72SdhMyl765K/i4nY5\n"
    "CPcHz2Q1IKWKZ9S79xmK7G8pUhbVf4EZLvnNF1+9IvOFQUKV5Z7ZNNviqSpnql9t\n"
    "AT+8+J/He0R7pcirvVSxgdr2i9V/C/gmqAEZ5qVTzRnd3uWdFoKzPdEBxP0IporJ\n"
    "1VBbCv90yBSOhVxO+QIDAQAB\n"
    "-----END PUBLIC KEY-----\n";

static const size_t CHUNK = 117;  // PKCS#1 v1.5 limit for a 1024-bit key

bool encrypt_body(const char *key_pem, const std::string &plain, std::string &out_b64, Rng rng, void *rng_ctx) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  bool ok = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)key_pem, strlen(key_pem) + 1) == 0;
  std::vector<unsigned char> cipher;
  const size_t block = ok ? mbedtls_pk_get_len(&pk) : 0;
  for (size_t off = 0; ok && off < plain.size(); off += CHUNK) {
    const size_t n = plain.size() - off < CHUNK ? plain.size() - off : CHUNK;
    size_t olen = 0;
    const size_t at = cipher.size();
    cipher.resize(at + block);
    ok = mbedtls_pk_encrypt(&pk, (const unsigned char *)plain.data() + off, n, cipher.data() + at, &olen, block, rng,
                            rng_ctx) == 0 &&
         olen == block;
  }
  mbedtls_pk_free(&pk);
  if (!ok) return false;
  size_t need = 0;
  mbedtls_base64_encode(nullptr, 0, &need, cipher.data(), cipher.size());
  out_b64.assign(need, '\0');
  size_t written = 0;
  if (mbedtls_base64_encode((unsigned char *)&out_b64[0], need, &written, cipher.data(), cipher.size()) != 0)
    return false;
  out_b64.resize(written);
  return true;
}

}  // namespace cloudcrypto
