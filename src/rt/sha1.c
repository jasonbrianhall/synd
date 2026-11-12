/* sha1.c - SHA-1 (FIPS 180-1) for the executable check; written for this project. */
#include "sha1.h"

#include <stdio.h>
#include <string.h>

static uint32_t rol(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80], a, b, c, d, e, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) |
               p[4 * i + 3];
    for (; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = h[0];
    b = h[1];
    c = h[2];
    d = h[3];
    e = h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void sha1_hex(const void *data, size_t n, char out[41])
{
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const uint8_t *p = (const uint8_t *)data;
    uint8_t tail[128];
    size_t i, rest;
    uint64_t bits = (uint64_t)n * 8u;
    for (i = 0; i + 64 <= n; i += 64) block(h, p + i);
    rest = n - i;
    memset(tail, 0, sizeof tail);
    memcpy(tail, p + i, rest);
    tail[rest] = 0x80;
    {
        size_t len = rest + 1 + 8 <= 64 ? 64 : 128;
        int k;
        for (k = 0; k < 8; k++) tail[len - 1 - k] = (uint8_t)(bits >> (8 * k));
        block(h, tail);
        if (len == 128) block(h, tail + 64);
    }
    for (i = 0; i < 5; i++) sprintf(out + 8 * i, "%08x", h[i]);
    out[40] = '\0';
}
