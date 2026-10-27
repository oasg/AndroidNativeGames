#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t length;
    uint8_t block[64];
    size_t used;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);

void sha256(const void *data, size_t len, uint8_t out[32]);
void sha256_hex(const void *data, size_t len, char out[65]);
