// argv[1] = PEM file; stdin = plaintext; prints base64 body.
#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include "cloud_crypto.h"
static int rng(void *, unsigned char *out, size_t n) { for (size_t i = 0; i < n; ++i) out[i] = (unsigned char)(rand() % 255 + 1); return 0; }
int main(int argc, char **argv) {
  std::ifstream f(argv[1]); std::stringstream ss; ss << f.rdbuf(); std::string pem = ss.str();
  std::string in((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  std::string out;
  if (!cloudcrypto::encrypt_body(argc > 2 ? cloudcrypto::XBLOOM_PUBLIC_KEY_PEM : pem.c_str(), in, out, rng, nullptr)) { printf("FAIL\n"); return 1; }
  printf("%s", out.c_str());
}
