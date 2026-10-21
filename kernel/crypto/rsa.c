#include <kernel/crypto.h>
#include <kernel/mem.h>

#define BN_WORDS 34 /* 34 * 64 bits = 2176 bits, ample for 2048-bit RSA + carry */

typedef struct {
    u64 d[BN_WORDS];
} bignum_t;

typedef unsigned __int128 u128;

static void bn_from_bytes(bignum_t *r, const u8 *b, size_t len) {
    kmemset(r, 0, sizeof(*r));
    if (len > RSA_MAX_KEY_BYTES) len = RSA_MAX_KEY_BYTES;
    for (size_t i = 0; i < len; i++) {
        size_t byte_pos = len - 1 - i;
        size_t word_idx = i / 8;
        size_t shift = (i % 8) * 8;
        r->d[word_idx] |= ((u64)b[byte_pos]) << shift;
    }
}

static void bn_to_bytes(const bignum_t *a, u8 *b, size_t len) {
    kmemset(b, 0, len);
    for (size_t i = 0; i < len; i++) {
        size_t byte_pos = len - 1 - i;
        size_t word_idx = i / 8;
        size_t shift = (i % 8) * 8;
        if (word_idx < BN_WORDS) {
            b[byte_pos] = (u8)(a->d[word_idx] >> shift);
        }
    }
}

static int bn_cmp(const bignum_t *a, const bignum_t *b) {
    for (int i = BN_WORDS - 1; i >= 0; i--) {
        if (a->d[i] > b->d[i]) return 1;
        if (a->d[i] < b->d[i]) return -1;
    }
    return 0;
}

static void bn_sub(bignum_t *r, const bignum_t *a, const bignum_t *b) {
    u64 borrow = 0;
    for (int i = 0; i < BN_WORDS; i++) {
        u128 diff = (u128)a->d[i] - b->d[i] - borrow;
        r->d[i] = (u64)diff;
        borrow = (diff >> 64) ? 1 : 0;
    }
}

static void bn_lshift1(bignum_t *a) {
    u64 carry = 0;
    for (int i = 0; i < BN_WORDS; i++) {
        u64 next_carry = a->d[i] >> 63;
        a->d[i] = (a->d[i] << 1) | carry;
        carry = next_carry;
    }
}

static void bn_mul_mod(bignum_t *r, const bignum_t *a, const bignum_t *b, const bignum_t *n) {
    u64 prod[BN_WORDS * 2] = {0};
    for (int i = 0; i < BN_WORDS; i++) {
        if (a->d[i] == 0) continue;
        u64 carry = 0;
        for (int j = 0; j < BN_WORDS; j++) {
            u128 p = (u128)a->d[i] * b->d[j] + prod[i + j] + carry;
            prod[i + j] = (u64)p;
            carry = (u64)(p >> 64);
        }
        int k = i + BN_WORDS;
        while (carry > 0 && k < BN_WORDS * 2) {
            u128 p = (u128)prod[k] + carry;
            prod[k] = (u64)p;
            carry = (u64)(p >> 64);
            k++;
        }
    }

    /* Reduce product modulo n using bit-by-bit division */
    bignum_t rem = {0};
    for (int bit = (BN_WORDS * 2 * 64) - 1; bit >= 0; bit--) {
        bn_lshift1(&rem);
        size_t w_idx = bit / 64;
        size_t b_idx = bit % 64;
        if (prod[w_idx] & (1ULL << b_idx)) {
            rem.d[0] |= 1;
        }
        if (bn_cmp(&rem, n) >= 0) {
            bn_sub(&rem, &rem, n);
        }
    }
    *r = rem;
}

static void bn_mod_exp(bignum_t *r, const bignum_t *base, u32 exp, const bignum_t *n) {
    bignum_t res = {0};
    res.d[0] = 1;
    bignum_t b = *base;

    while (exp > 0) {
        if (exp & 1) {
            bn_mul_mod(&res, &res, &b, n);
        }
        if (exp > 1) {
            bn_mul_mod(&b, &b, &b, n);
        }
        exp >>= 1;
    }
    *r = res;
}

/* PKCS#1 v1.5 SHA-256 DigestInfo prefix */
static const u8 SHA256_DIGEST_INFO[19] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
    0x00, 0x04, 0x20
};

bool rsa_verify_pkcs1_v15_sha256(const rsa_pubkey_t *key,
                                 const u8 *hash, size_t hash_len,
                                 const u8 *sig, size_t sig_len) {
    if (!key || !hash || hash_len != 32 || !sig || sig_len != key->n_len) {
        return false;
    }
    if (key->n_len != 256 || key->e != 65537) {
        /* Support standard 2048-bit RSA with e=65537 */
        return false;
    }

    bignum_t n, s, m;
    bn_from_bytes(&n, key->n, key->n_len);
    bn_from_bytes(&s, sig, sig_len);

    if (bn_cmp(&s, &n) >= 0) return false;

    bn_mod_exp(&m, &s, key->e, &n);

    u8 em[RSA_MAX_KEY_BYTES];
    bn_to_bytes(&m, em, key->n_len);

    /* Verify PKCS#1 v1.5 padding:
     * 0x00 || 0x01 || PS (0xFF) || 0x00 || DigestInfo (19) || Hash (32)
     */
    if (em[0] != 0x00 || em[1] != 0x01) return false;

    size_t i = 2;
    while (i < key->n_len && em[i] == 0xFF) i++;

    if (i < 10 || i >= key->n_len || em[i] != 0x00) return false;
    i++; /* Skip 0x00 delimiter */

    if (key->n_len - i != sizeof(SHA256_DIGEST_INFO) + 32) return false;

    if (!crypto_memcmp_eq(&em[i], SHA256_DIGEST_INFO, sizeof(SHA256_DIGEST_INFO))) {
        return false;
    }
    i += sizeof(SHA256_DIGEST_INFO);

    return crypto_memcmp_eq(&em[i], hash, 32);
}

/* RFC 8017 Section 9.1.2: MGF1 with SHA-256 */
static void mgf1_sha256(const u8 *seed, size_t seed_len, u8 *mask, size_t mask_len) {
    u32 counter = 0;
    size_t out_len = 0;
    u8 in_buf[SHA256_DIGEST_SIZE + 4];
    kmemcpy(in_buf, seed, seed_len);

    while (out_len < mask_len) {
        in_buf[seed_len + 0] = (u8)(counter >> 24);
        in_buf[seed_len + 1] = (u8)(counter >> 16);
        in_buf[seed_len + 2] = (u8)(counter >> 8);
        in_buf[seed_len + 3] = (u8)(counter);

        u8 h[SHA256_DIGEST_SIZE];
        sha256_hash(in_buf, seed_len + 4, h);

        size_t to_copy = mask_len - out_len;
        if (to_copy > SHA256_DIGEST_SIZE) to_copy = SHA256_DIGEST_SIZE;
        kmemcpy(mask + out_len, h, to_copy);
        out_len += to_copy;
        counter++;
    }
}

bool rsa_verify_pss_sha256(const rsa_pubkey_t *key,
                           const u8 *hash, size_t hash_len,
                           const u8 *sig, size_t sig_len) {
    if (!key || !hash || hash_len != 32 || !sig || sig_len != key->n_len) {
        return false;
    }
    if (key->n_len != 256 || key->e != 65537) {
        return false;
    }

    bignum_t n, s, m;
    bn_from_bytes(&n, key->n, key->n_len);
    bn_from_bytes(&s, sig, sig_len);

    if (bn_cmp(&s, &n) >= 0) return false;

    bn_mod_exp(&m, &s, key->e, &n);

    u8 em[RSA_MAX_KEY_BYTES];
    bn_to_bytes(&m, em, key->n_len);

    /* 1. Trailer byte must be 0xBC */
    if (em[key->n_len - 1] != 0xBC) return false;

    /* emLen = 256, hLen = 32, sLen = 32 */
    size_t em_len = key->n_len;
    size_t h_len = 32;
    size_t s_len = 32;

    const u8 *H = &em[em_len - 1 - h_len];
    size_t db_len = em_len - 1 - h_len;

    /* Unmask DB */
    u8 db_mask[RSA_MAX_KEY_BYTES];
    mgf1_sha256(H, h_len, db_mask, db_len);

    u8 DB[RSA_MAX_KEY_BYTES];
    for (size_t j = 0; j < db_len; j++) {
        DB[j] = em[j] ^ db_mask[j];
    }

    /* Top bit of DB must be 0 for RSA-2048 */
    DB[0] &= 0x7F;

    /* Verify DB: 0x00 ... 0x01 || salt */
    size_t pad_len = db_len - s_len - 1;
    for (size_t j = 0; j < pad_len; j++) {
        if (DB[j] != 0x00) return false;
    }
    if (DB[pad_len] != 0x01) return false;

    const u8 *salt = &DB[pad_len + 1];

    /* Reconstruct M' = 8 zero bytes || mHash || salt */
    u8 m_prime[8 + 32 + 32];
    kmemset(m_prime, 0, 8);
    kmemcpy(m_prime + 8, hash, 32);
    kmemcpy(m_prime + 40, salt, s_len);

    u8 h_prime[SHA256_DIGEST_SIZE];
    sha256_hash(m_prime, sizeof(m_prime), h_prime);

    return crypto_memcmp_eq(h_prime, H, h_len);
}
