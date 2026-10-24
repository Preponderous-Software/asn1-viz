/* SHA-256 (FIPS 180-4), for the value digest in asn1-layout/1. */
#ifndef ASN1VIZ_SHA256_H
#define ASN1VIZ_SHA256_H
#include <stddef.h>
#include <stdint.h>
void asn1viz_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
#endif
