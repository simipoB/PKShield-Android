// Darwin stand-in (dm/android/compat): SHA-256 with CommonCrypto's names (compat.c)
#pragma once
#include <stdint.h>
#include <stddef.h>
#define CC_SHA256_DIGEST_LENGTH 32
typedef uint32_t CC_LONG;
typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; } CC_SHA256_CTX;
int CC_SHA256_Init(CC_SHA256_CTX *c);
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *p, CC_LONG n);
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c);
