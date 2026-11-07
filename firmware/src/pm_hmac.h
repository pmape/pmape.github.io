#pragma once
#include <stddef.h>
#include <stdint.h>
/* HMAC-SHA256 (telefon doğrulaması için küçük, bağımsız uygulama) */
void pm_hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32]);
