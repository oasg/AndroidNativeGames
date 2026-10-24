#pragma once
#include <stddef.h>
#include <stdint.h>
void sha256(const void *data, size_t len, uint8_t out[32]);
void sha256_hex(const void *data, size_t len, char out[65]);
