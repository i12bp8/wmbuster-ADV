// wM-Buster ADV — AES-128 (FIPS-197) with CBC, CTR and CMAC helpers.
// Compact implementation: S-boxes are computed at compile time.
// GPL-3.0
#include "wmbus/aes.h"

#include <string.h>

namespace wmb {

// ---------------------------------------------------------------------------
// Compile time S-box generation
// ---------------------------------------------------------------------------
static constexpr uint8_t xt(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0x00)); }

static constexpr uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

static constexpr uint8_t ginv(uint8_t a) {
    if (a == 0) return 0;
    // a^254
    uint8_t r = 1, base = a;
    int e = 254;
    while (e) {
        if (e & 1) r = gmul(r, base);
        base = gmul(base, base);
        e >>= 1;
    }
    return r;
}

static constexpr uint8_t rotl8(uint8_t x, int n) { return (uint8_t)((x << n) | (x >> (8 - n))); }

struct SBoxes {
    uint8_t s[256];
    uint8_t inv[256];
    constexpr SBoxes() : s(), inv() {
        for (int i = 0; i < 256; ++i) {
            uint8_t b = ginv((uint8_t)i);
            uint8_t v = (uint8_t)(b ^ rotl8(b, 1) ^ rotl8(b, 2) ^ rotl8(b, 3) ^ rotl8(b, 4) ^ 0x63);
            s[i] = v;
            inv[v] = (uint8_t)i;
        }
    }
};
static constexpr SBoxes SB{};

// ---------------------------------------------------------------------------
// Key schedule and block functions
// ---------------------------------------------------------------------------
void aes128_init(Aes128* ctx, const uint8_t key[16]) {
    uint8_t* rk = ctx->rk;
    memcpy(rk, key, 16);
    uint8_t rcon = 0x01;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
        if ((i % 16) == 0) {
            uint8_t tmp = t[0];
            t[0] = (uint8_t)(SB.s[t[1]] ^ rcon);
            t[1] = SB.s[t[2]];
            t[2] = SB.s[t[3]];
            t[3] = SB.s[tmp];
            rcon = xt(rcon);
        }
        for (int j = 0; j < 4; ++j) rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ t[j]);
    }
}

static inline void add_rk(uint8_t* s, const uint8_t* k) { for (int i = 0; i < 16; ++i) s[i] ^= k[i]; }

void aes128_encrypt_block(const Aes128* ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    add_rk(s, ctx->rk);
    for (int round = 1; round <= 10; ++round) {
        // SubBytes + ShiftRows
        uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                t[c * 4 + r] = SB.s[s[((c + r) & 3) * 4 + r]];
        if (round != 10) {
            // MixColumns
            for (int c = 0; c < 4; ++c) {
                uint8_t* col = t + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                col[0] = (uint8_t)(a0 ^ all ^ xt((uint8_t)(a0 ^ a1)));
                col[1] = (uint8_t)(a1 ^ all ^ xt((uint8_t)(a1 ^ a2)));
                col[2] = (uint8_t)(a2 ^ all ^ xt((uint8_t)(a2 ^ a3)));
                col[3] = (uint8_t)(a3 ^ all ^ xt((uint8_t)(a3 ^ a0)));
            }
        }
        memcpy(s, t, 16);
        add_rk(s, ctx->rk + round * 16);
    }
    memcpy(out, s, 16);
}

void aes128_decrypt_block(const Aes128* ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    add_rk(s, ctx->rk + 160);
    for (int round = 9; round >= 0; --round) {
        // InvShiftRows + InvSubBytes
        uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                t[((c + r) & 3) * 4 + r] = SB.inv[s[c * 4 + r]];
        add_rk(t, ctx->rk + round * 16);
        if (round != 0) {
            // InvMixColumns
            for (int c = 0; c < 4; ++c) {
                uint8_t* col = t + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (uint8_t)(gmul(a0, 0x0E) ^ gmul(a1, 0x0B) ^ gmul(a2, 0x0D) ^ gmul(a3, 0x09));
                col[1] = (uint8_t)(gmul(a0, 0x09) ^ gmul(a1, 0x0E) ^ gmul(a2, 0x0B) ^ gmul(a3, 0x0D));
                col[2] = (uint8_t)(gmul(a0, 0x0D) ^ gmul(a1, 0x09) ^ gmul(a2, 0x0E) ^ gmul(a3, 0x0B));
                col[3] = (uint8_t)(gmul(a0, 0x0B) ^ gmul(a1, 0x0D) ^ gmul(a2, 0x09) ^ gmul(a3, 0x0E));
            }
        }
        memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------
void aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                        const uint8_t* in, uint8_t* out, size_t len) {
    Aes128 ctx;
    aes128_init(&ctx, key);
    uint8_t prev[16], cur[16], blk[16];
    memcpy(prev, iv, 16);
    for (size_t off = 0; off + 16 <= len; off += 16) {
        memcpy(cur, in + off, 16);
        aes128_decrypt_block(&ctx, cur, blk);
        for (int i = 0; i < 16; ++i) out[off + i] = (uint8_t)(blk[i] ^ prev[i]);
        memcpy(prev, cur, 16);
    }
}

void aes128_ctr_crypt(const uint8_t key[16], const uint8_t iv[16],
                      const uint8_t* in, uint8_t* out, size_t len) {
    Aes128 ctx;
    aes128_init(&ctx, key);
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 16);
    for (size_t off = 0; off < len; off += 16) {
        aes128_encrypt_block(&ctx, ctr, ks);
        size_t n = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < n; ++i) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
        for (int i = 15; i >= 0; --i) {
            if (++ctr[i] != 0) break;
        }
    }
}

static void cmac_shift(const uint8_t in[16], uint8_t out[16]) {
    uint8_t carry = (in[0] & 0x80) ? 0x87 : 0x00;
    for (int i = 0; i < 15; ++i) out[i] = (uint8_t)((in[i] << 1) | (in[i + 1] >> 7));
    out[15] = (uint8_t)((in[15] << 1) ^ carry);
}

void aes128_cmac(const uint8_t key[16], const uint8_t* msg, size_t len, uint8_t mac[16]) {
    Aes128 ctx;
    aes128_init(&ctx, key);
    uint8_t zero[16] = { 0 }, l[16], k1[16], k2[16];
    aes128_encrypt_block(&ctx, zero, l);
    cmac_shift(l, k1);
    cmac_shift(k1, k2);

    size_t n = (len + 15) / 16;
    bool complete = (len > 0) && (len % 16 == 0);
    if (n == 0) n = 1;

    uint8_t x[16] = { 0 }, y[16], last[16];
    for (size_t b = 0; b + 1 < n; ++b) {
        for (int i = 0; i < 16; ++i) y[i] = (uint8_t)(x[i] ^ msg[b * 16 + i]);
        aes128_encrypt_block(&ctx, y, x);
    }
    size_t rem = len - (n - 1) * 16;
    if (complete) {
        for (int i = 0; i < 16; ++i) last[i] = (uint8_t)(msg[(n - 1) * 16 + i] ^ k1[i]);
    } else {
        for (size_t i = 0; i < 16; ++i) {
            uint8_t v = i < rem ? msg[(n - 1) * 16 + i] : (i == rem ? 0x80 : 0x00);
            last[i] = (uint8_t)(v ^ k2[i]);
        }
    }
    for (int i = 0; i < 16; ++i) y[i] = (uint8_t)(x[i] ^ last[i]);
    aes128_encrypt_block(&ctx, y, mac);
}

} // namespace wmb
