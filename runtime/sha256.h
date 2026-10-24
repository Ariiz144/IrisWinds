// Standard SHA-256 implementation (FIPS 180-4) for content-addressed build caches.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} Sha256Ctx;

void sha256_init(Sha256Ctx *ctx);
void sha256_update(Sha256Ctx *ctx, const void *data, size_t len);
void sha256_final(Sha256Ctx *ctx, uint8_t hash[32]);

// Hex conversion: writes 64 lowercase hex characters and a null terminator into out (must have size >= 65)
void sha256_to_hex(const uint8_t hash[32], char out[65]);

// Convenience helpers
bool sha256_file(const char *path, char out_hex[65]);
void sha256_buffer(const void *data, size_t len, char out_hex[65]);
void sha256_string(const char *str, char out_hex[65]);

#ifdef __cplusplus
}
#endif
