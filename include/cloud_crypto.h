// Request-body encryption used by xBloom's account API (the app's own scheme,
// per xbloom-py): JSON -> 117-byte chunks -> RSA PKCS#1 v1.5 with xBloom's
// public key -> concatenated -> base64. Pure mbedTLS; unit-tested on a PC.
#pragma once
#include <stddef.h>
#include <string>

namespace cloudcrypto {

extern const char XBLOOM_PUBLIC_KEY_PEM[];

using Rng = int (*)(void *ctx, unsigned char *out, size_t len);

// key_pem must be NUL-terminated. Returns false on any mbedTLS error.
bool encrypt_body(const char *key_pem, const std::string &plain, std::string &out_b64, Rng rng, void *rng_ctx);

}  // namespace cloudcrypto
