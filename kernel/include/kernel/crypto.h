#ifndef ASTRA_KERNEL_CRYPTO_H
#define ASTRA_KERNEL_CRYPTO_H

#include <kernel/types.h>

#define SHA256_DIGEST_SIZE  32
#define SHA256_BLOCK_SIZE   64
#define CHACHA20_KEY_SIZE   32
#define CHACHA20_NONCE_SIZE 12
#define POLY1305_TAG_SIZE   16
#define X25519_KEY_SIZE     32
#define RSA_MAX_KEY_BITS    2048
#define RSA_MAX_KEY_BYTES   (RSA_MAX_KEY_BITS / 8)

/* --- Cryptographic Randomness & Entropy --- */
bool crypto_has_secure_entropy(void);
bool crypto_get_random(void *buf, size_t len);

/* --- SHA-256 --- */
typedef struct {
    u32 state[8];
    u64 count;
    u8 buffer[SHA256_BLOCK_SIZE];
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, u8 digest[SHA256_DIGEST_SIZE]);
void sha256_hash(const void *data, size_t len, u8 digest[SHA256_DIGEST_SIZE]);

/* --- HMAC-SHA256 --- */
void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 u8 out[SHA256_DIGEST_SIZE]);

/* --- HKDF-SHA256 (RFC 5869 & RFC 8446) --- */
void hkdf_extract(const void *salt, size_t salt_len,
                  const void *ikm, size_t ikm_len,
                  u8 prk[SHA256_DIGEST_SIZE]);

bool hkdf_expand(const u8 prk[SHA256_DIGEST_SIZE],
                 const void *info, size_t info_len,
                 void *okm, size_t okm_len);

bool hkdf_expand_label(const u8 secret[SHA256_DIGEST_SIZE],
                       const char *label,
                       const u8 *context, size_t context_len,
                       u16 length, u8 *out);

/* --- ChaCha20-Poly1305 (RFC 8439) --- */
void chacha20_crypt(const u8 key[CHACHA20_KEY_SIZE],
                    u32 counter,
                    const u8 nonce[CHACHA20_NONCE_SIZE],
                    const void *in, void *out, size_t len);

void poly1305_mac(const u8 key[32],
                  const void *msg, size_t len,
                  u8 tag[POLY1305_TAG_SIZE]);

void chacha20_poly1305_encrypt(const u8 key[CHACHA20_KEY_SIZE],
                               const u8 nonce[CHACHA20_NONCE_SIZE],
                               const void *aad, size_t aad_len,
                               const void *plaintext, size_t text_len,
                               void *ciphertext,
                               u8 tag[POLY1305_TAG_SIZE]);

bool chacha20_poly1305_decrypt(const u8 key[CHACHA20_KEY_SIZE],
                               const u8 nonce[CHACHA20_NONCE_SIZE],
                               const void *aad, size_t aad_len,
                               const void *ciphertext, size_t text_len,
                               const u8 tag[POLY1305_TAG_SIZE],
                               void *plaintext);

/* --- X25519 (RFC 7748) --- */
void x25519_keypair(u8 public_key[X25519_KEY_SIZE], u8 private_key[X25519_KEY_SIZE]);
bool x25519_scalarmult(u8 shared_secret[X25519_KEY_SIZE],
                       const u8 private_key[X25519_KEY_SIZE],
                       const u8 peer_public_key[X25519_KEY_SIZE]);

/* --- RSA Signature Verification (RFC 8017 PKCS#1 v1.5 & PSS with SHA-256) --- */
typedef struct {
    u8 n[RSA_MAX_KEY_BYTES];
    size_t n_len;
    u32 e;
} rsa_pubkey_t;

bool rsa_verify_pkcs1_v15_sha256(const rsa_pubkey_t *key,
                                 const u8 *hash, size_t hash_len,
                                 const u8 *sig, size_t sig_len);

bool rsa_verify_pss_sha256(const rsa_pubkey_t *key,
                           const u8 *hash, size_t hash_len,
                           const u8 *sig, size_t sig_len);

/* --- X.509 Certificate Parser & Validator --- */
typedef struct {
    char common_name[64];
    char dns_names[4][64];
    int dns_count;
    u64 not_before;   /* Unix timestamp */
    u64 not_after;    /* Unix timestamp */
    rsa_pubkey_t pubkey;
    u8 issuer_cn[64];
    u8 tbs_hash[SHA256_DIGEST_SIZE];
    u8 sig[RSA_MAX_KEY_BYTES];
    size_t sig_len;
    u32 sig_algo;
    bool is_valid_format;
} x509_cert_t;

typedef enum {
    CERT_OK = 0,
    CERT_ERR_MALFORMED,
    CERT_ERR_EXPIRED,
    CERT_ERR_NOT_YET_VALID,
    CERT_ERR_HOSTNAME_MISMATCH,
    CERT_ERR_UNTRUSTED_ISSUER,
    CERT_ERR_INVALID_SIGNATURE,
    CERT_ERR_CLOCK_UNAVAILABLE
} cert_status_t;

bool x509_parse_cert(const u8 *der, size_t len, x509_cert_t *cert);
cert_status_t x509_verify_cert(const x509_cert_t *cert,
                              const x509_cert_t *issuer_ca,
                              const char *hostname,
                              u64 current_time);

/* Constant-time memory comparison */
bool crypto_memcmp_eq(const void *a, const void *b, size_t len);

/* Self-test suite */
bool crypto_self_test(void);

#endif /* ASTRA_KERNEL_CRYPTO_H */
