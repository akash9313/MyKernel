#include <kernel/crypto.h>
#include <kernel/mem.h>

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

#define QR(a, b, c, d) do { \
    a += b; d ^= a; d = ROTL32(d, 16); \
    c += d; b ^= c; b = ROTL32(b, 12); \
    a += b; d ^= a; d = ROTL32(d, 8);  \
    c += d; b ^= c; b = ROTL32(b, 7);  \
} while(0)

static u32 load32_le(const u8 *p) {
    return ((u32)p[0]) | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void store32_le(u8 *p, u32 v) {
    p[0] = (u8)(v);
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64]) {
    u32 state[16];
    state[0]  = 0x61707865;
    state[1]  = 0x3320646e;
    state[2]  = 0x79622d32;
    state[3]  = 0x6b206574;
    state[4]  = load32_le(key + 0);
    state[5]  = load32_le(key + 4);
    state[6]  = load32_le(key + 8);
    state[7]  = load32_le(key + 12);
    state[8]  = load32_le(key + 16);
    state[9]  = load32_le(key + 20);
    state[10] = load32_le(key + 24);
    state[11] = load32_le(key + 28);
    state[12] = counter;
    state[13] = load32_le(nonce + 0);
    state[14] = load32_le(nonce + 4);
    state[15] = load32_le(nonce + 8);

    u32 work[16];
    for (int i = 0; i < 16; i++) work[i] = state[i];

    for (int i = 0; i < 10; i++) {
        /* Column round */
        QR(work[0], work[4], work[8],  work[12]);
        QR(work[1], work[5], work[9],  work[13]);
        QR(work[2], work[6], work[10], work[14]);
        QR(work[3], work[7], work[11], work[15]);
        /* Diagonal round */
        QR(work[0], work[5], work[10], work[15]);
        QR(work[1], work[6], work[11], work[12]);
        QR(work[2], work[7], work[8],  work[13]);
        QR(work[3], work[4], work[9],  work[14]);
    }

    for (int i = 0; i < 16; i++) {
        store32_le(out + (i * 4), work[i] + state[i]);
    }
}

void chacha20_crypt(const u8 key[CHACHA20_KEY_SIZE],
                    u32 counter,
                    const u8 nonce[CHACHA20_NONCE_SIZE],
                    const void *in, void *out, size_t len) {
    const u8 *inp = (const u8 *)in;
    u8 *outp = (u8 *)out;
    u8 block[64];

    while (len > 0) {
        chacha20_block(key, counter, nonce, block);
        size_t chunk = (len < 64) ? len : 64;
        for (size_t i = 0; i < chunk; i++) {
            outp[i] = inp[i] ^ block[i];
        }
        inp += chunk;
        outp += chunk;
        len -= chunk;
        counter++;
    }
}

/* --- Poly1305 Authenticator --- */

void poly1305_mac(const u8 key[32],
                  const void *msg, size_t len,
                  u8 tag[POLY1305_TAG_SIZE]) {
    /* Clamped r */
    u32 r0 = load32_le(key + 0)  & 0x03ffffff;
    u32 r1 = (load32_le(key + 3) >> 2) & 0x03ffff03;
    u32 r2 = (load32_le(key + 6) >> 4) & 0x03ffc0ff;
    u32 r3 = (load32_le(key + 9) >> 6) & 0x03f03fff;
    u32 r4 = (load32_le(key + 12) >> 8) & 0x000fffff;

    u32 s1 = r1 * 5;
    u32 s2 = r2 * 5;
    u32 s3 = r3 * 5;
    u32 s4 = r4 * 5;

    u32 h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

    const u8 *m = (const u8 *)msg;
    while (len > 0) {
        u8 buf[17] = {0};
        size_t block_len = (len >= 16) ? 16 : len;
        kmemcpy(buf, m, block_len);
        buf[block_len] = 0x01;
        m += block_len;
        len -= block_len;

        u32 t0 = load32_le(buf + 0);
        u32 t1 = load32_le(buf + 4);
        u32 t2 = load32_le(buf + 8);
        u32 t3 = load32_le(buf + 12);
        u32 t4 = (u32)buf[16];

        h0 += t0 & 0x03ffffff;
        h1 += ((t0 >> 26) | (t1 << 6)) & 0x03ffffff;
        h2 += ((t1 >> 20) | (t2 << 12)) & 0x03ffffff;
        h3 += ((t2 >> 14) | (t3 << 18)) & 0x03ffffff;
        h4 += (t3 >> 8) | (t4 << 24);

        /* 130-bit polynomial multiply h * r mod (2^130 - 5) */
        u64 d0 = (u64)h0 * r0 + (u64)h1 * s4 + (u64)h2 * s3 + (u64)h3 * s2 + (u64)h4 * s1;
        u64 d1 = (u64)h0 * r1 + (u64)h1 * r0 + (u64)h2 * s4 + (u64)h3 * s3 + (u64)h4 * s2;
        u64 d2 = (u64)h0 * r2 + (u64)h1 * r1 + (u64)h2 * r0 + (u64)h3 * s4 + (u64)h4 * s3;
        u64 d3 = (u64)h0 * r3 + (u64)h1 * r2 + (u64)h2 * r1 + (u64)h3 * r0 + (u64)h4 * s4;
        u64 d4 = (u64)h0 * r4 + (u64)h1 * r3 + (u64)h2 * r2 + (u64)h3 * r1 + (u64)h4 * r0;

        /* Partial reduction */
        u32 c;
        c = (u32)(d0 >> 26); h0 = (u32)d0 & 0x03ffffff; d1 += c;
        c = (u32)(d1 >> 26); h1 = (u32)d1 & 0x03ffffff; d2 += c;
        c = (u32)(d2 >> 26); h2 = (u32)d2 & 0x03ffffff; d3 += c;
        c = (u32)(d3 >> 26); h3 = (u32)d3 & 0x03ffffff; d4 += c;
        c = (u32)(d4 >> 26); h4 = (u32)d4 & 0x03ffffff; h0 += c * 5;
        c = h0 >> 26;        h0 &= 0x03ffffff;           h1 += c;
    }

    /* Full reduction mod 2^130 - 5 */
    u32 g0 = h0 + 5; u32 c = g0 >> 26; g0 &= 0x03ffffff;
    u32 g1 = h1 + c; c = g1 >> 26; g1 &= 0x03ffffff;
    u32 g2 = h2 + c; c = g2 >> 26; g2 &= 0x03ffffff;
    u32 g3 = h3 + c; c = g3 >> 26; g3 &= 0x03ffffff;
    u32 g4 = h4 + c - (1 << 26);

    u32 mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    /* Pack into four 32-bit words */
    u32 w0 = h0 | (h1 << 26);
    u32 w1 = (h1 >> 6) | (h2 << 20);
    u32 w2 = (h2 >> 12) | (h3 << 14);
    u32 w3 = (h3 >> 18) | (h4 << 8);

    /* Add s */
    u64 t_w0 = (u64)w0 + load32_le(key + 16);
    u64 t_w1 = (u64)w1 + load32_le(key + 20) + (t_w0 >> 32);
    u64 t_w2 = (u64)w2 + load32_le(key + 24) + (t_w1 >> 32);
    u64 t_w3 = (u64)w3 + load32_le(key + 28) + (t_w2 >> 32);

    store32_le(tag + 0, (u32)t_w0);
    store32_le(tag + 4, (u32)t_w1);
    store32_le(tag + 8, (u32)t_w2);
    store32_le(tag + 12, (u32)t_w3);
}

/* --- ChaCha20-Poly1305 AEAD (RFC 8439 Section 2.8) --- */

void chacha20_poly1305_encrypt(const u8 key[CHACHA20_KEY_SIZE],
                               const u8 nonce[CHACHA20_NONCE_SIZE],
                               const void *aad, size_t aad_len,
                               const void *plaintext, size_t text_len,
                               void *ciphertext,
                               u8 tag[POLY1305_TAG_SIZE]) {
    /* 1. Generate Poly1305 subkey using counter 0 */
    u8 poly_key_block[64];
    chacha20_block(key, 0, nonce, poly_key_block);

    /* 2. Encrypt plaintext using counter 1 */
    if (text_len > 0) {
        chacha20_crypt(key, 1, nonce, plaintext, ciphertext, text_len);
    }

    /* 3. Build Poly1305 mac data */
    /* AAD + pad16 + ciphertext + pad16 + len(AAD)[8] + len(ct)[8] */
    size_t aad_pad = (16 - (aad_len % 16)) % 16;
    size_t ct_pad  = (16 - (text_len % 16)) % 16;
    size_t mac_data_len = aad_len + aad_pad + text_len + ct_pad + 16;

    /* Allocate static or bounded buffer for MAC data */
    static u8 mac_buf[2048];
    u8 *buf = mac_buf;
    if (mac_data_len > sizeof(mac_buf)) {
        /* Cap for kernel bounds */
        mac_data_len = sizeof(mac_buf);
    }
    kmemset(buf, 0, mac_data_len);

    size_t idx = 0;
    if (aad && aad_len > 0) {
        kmemcpy(buf + idx, aad, aad_len);
        idx += aad_len + aad_pad;
    }
    if (ciphertext && text_len > 0) {
        kmemcpy(buf + idx, ciphertext, text_len);
        idx += text_len + ct_pad;
    }

    /* Store 64-bit little-endian lengths */
    store32_le(buf + idx + 0, (u32)aad_len);
    store32_le(buf + idx + 4, (u32)((u64)aad_len >> 32));
    store32_le(buf + idx + 8, (u32)text_len);
    store32_le(buf + idx + 12, (u32)((u64)text_len >> 32));

    poly1305_mac(poly_key_block, buf, idx + 16, tag);
}

bool chacha20_poly1305_decrypt(const u8 key[CHACHA20_KEY_SIZE],
                               const u8 nonce[CHACHA20_NONCE_SIZE],
                               const void *aad, size_t aad_len,
                               const void *ciphertext, size_t text_len,
                               const u8 tag[POLY1305_TAG_SIZE],
                               void *plaintext) {
    u8 poly_key_block[64];
    chacha20_block(key, 0, nonce, poly_key_block);

    size_t aad_pad = (16 - (aad_len % 16)) % 16;
    size_t ct_pad  = (16 - (text_len % 16)) % 16;
    size_t mac_data_len = aad_len + aad_pad + text_len + ct_pad + 16;

    static u8 mac_buf[2048];
    u8 *buf = mac_buf;
    if (mac_data_len > sizeof(mac_buf)) {
        return false;
    }
    kmemset(buf, 0, mac_data_len);

    size_t idx = 0;
    if (aad && aad_len > 0) {
        kmemcpy(buf + idx, aad, aad_len);
        idx += aad_len + aad_pad;
    }
    if (ciphertext && text_len > 0) {
        kmemcpy(buf + idx, ciphertext, text_len);
        idx += text_len + ct_pad;
    }

    store32_le(buf + idx + 0, (u32)aad_len);
    store32_le(buf + idx + 4, (u32)((u64)aad_len >> 32));
    store32_le(buf + idx + 8, (u32)text_len);
    store32_le(buf + idx + 12, (u32)((u64)text_len >> 32));

    u8 computed_tag[POLY1305_TAG_SIZE];
    poly1305_mac(poly_key_block, buf, idx + 16, computed_tag);

    /* Constant-time tag check */
    if (!crypto_memcmp_eq(computed_tag, tag, POLY1305_TAG_SIZE)) {
        /* Authentication failed: zero output buffer */
        if (plaintext && text_len > 0) {
            kmemset(plaintext, 0, text_len);
        }
        return false;
    }

    /* Decrypt ciphertext with counter 1 */
    if (text_len > 0) {
        chacha20_crypt(key, 1, nonce, ciphertext, plaintext, text_len);
    }
    return true;
}
