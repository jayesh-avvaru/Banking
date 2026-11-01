#include "auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------- minimal SHA-256 (FIPS 180-4) ---------- */
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void compress(uint32_t h[8], const uint8_t b[64])
{
    uint32_t w[64], a, bb, c, d, e, f, g, hh;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[4*i] << 24 | (uint32_t)b[4*i+1] << 16 | (uint32_t)b[4*i+2] << 8 | b[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = h[0]; bb = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t maj = (a & bb) ^ (a & c) ^ (bb & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256(const uint8_t *msg, size_t len, uint8_t out[32]);   /* exported for self-test */
void sha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t i;
    for (i = 0; i + 64 <= len; i += 64) compress(h, msg + i);

    uint8_t tail[128];
    memset(tail, 0, sizeof tail);
    size_t rem = len - i;
    memcpy(tail, msg + i, rem);
    tail[rem] = 0x80;
    size_t tl = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; j++) tail[tl - 1 - j] = (uint8_t)(bits >> (8 * j));
    compress(h, tail);
    if (tl == 128) compress(h, tail + 64);

    for (int j = 0; j < 8; j++) {
        out[4*j]   = (uint8_t)(h[j] >> 24);
        out[4*j+1] = (uint8_t)(h[j] >> 16);
        out[4*j+2] = (uint8_t)(h[j] >> 8);
        out[4*j+3] = (uint8_t)h[j];
    }
}

/* ---------- PIN hashing ---------- */
#define SALT_HEX   16
#define PIN_ROUNDS 5000

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char *hx = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2*i]   = hx[in[i] >> 4];
        out[2*i+1] = hx[in[i] & 15];
    }
    out[2*n] = '\0';
}

static void derive(const char *salt_hex, const char *pin, char out_hex[65])
{
    uint8_t buf[SALT_HEX + 64], h[32], t[32 + SALT_HEX];
    size_t pl = strlen(pin);
    if (pl > 64) pl = 64;
    memcpy(buf, salt_hex, SALT_HEX);
    memcpy(buf + SALT_HEX, pin, pl);
    sha256(buf, SALT_HEX + pl, h);
    for (int i = 1; i < PIN_ROUNDS; i++) {
        memcpy(t, h, 32);
        memcpy(t + 32, salt_hex, SALT_HEX);
        sha256(t, sizeof t, h);
    }
    to_hex(h, 32, out_hex);
}

void auth_hash_pin(const char *pin, char out[HASH_LEN])
{
    uint8_t salt[8];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(salt, 1, sizeof salt, f) != sizeof salt) {
        uint64_t x = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
        memcpy(salt, &x, sizeof salt);          /* weak fallback, should not happen */
    }
    if (f) fclose(f);

    char salt_hex[SALT_HEX + 1], hash_hex[65];
    to_hex(salt, sizeof salt, salt_hex);
    derive(salt_hex, pin, hash_hex);
    snprintf(out, HASH_LEN, "%s$%s", salt_hex, hash_hex);
}

int auth_verify_pin(const char *pin, const char *stored)
{
    if (strlen(stored) != SALT_HEX + 1 + 64 || stored[SALT_HEX] != '$') return 0;
    char salt_hex[SALT_HEX + 1], hash_hex[65];
    memcpy(salt_hex, stored, SALT_HEX);
    salt_hex[SALT_HEX] = '\0';
    derive(salt_hex, pin, hash_hex);

    unsigned diff = 0;                           /* constant-time compare */
    for (int i = 0; i < 64; i++) diff |= (unsigned char)(hash_hex[i] ^ stored[SALT_HEX + 1 + i]);
    return diff == 0;
}
