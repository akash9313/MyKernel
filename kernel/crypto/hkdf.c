#include <kernel/crypto.h>
#include <kernel/mem.h>

void hkdf_extract(const void *salt, size_t salt_len,
                  const void *ikm, size_t ikm_len,
                  u8 prk[SHA256_DIGEST_SIZE]) {
    static const u8 default_salt[SHA256_DIGEST_SIZE] = {0};
    if (!salt || salt_len == 0) {
        salt = default_salt;
        salt_len = SHA256_DIGEST_SIZE;
    }
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

bool hkdf_expand(const u8 prk[SHA256_DIGEST_SIZE],
                 const void *info, size_t info_len,
                 void *okm, size_t okm_len) {
    if (!prk || !okm || okm_len == 0) return false;
    if (okm_len > 255 * SHA256_DIGEST_SIZE) return false;

    u8 *out = (u8 *)okm;
    u8 T[SHA256_DIGEST_SIZE];
    size_t T_len = 0;
    u8 counter = 1;

    size_t generated = 0;
    while (generated < okm_len) {
        /* HMAC message is T_{i-1} || info || counter */
        u8 msg_buf[SHA256_DIGEST_SIZE + 256 + 1];
        size_t msg_len = 0;

        if (T_len > 0) {
            kmemcpy(&msg_buf[msg_len], T, T_len);
            msg_len += T_len;
        }
        if (info && info_len > 0) {
            if (info_len > 256) return false; /* Enforce sane bounds */
            kmemcpy(&msg_buf[msg_len], info, info_len);
            msg_len += info_len;
        }
        msg_buf[msg_len++] = counter;

        hmac_sha256(prk, SHA256_DIGEST_SIZE, msg_buf, msg_len, T);
        T_len = SHA256_DIGEST_SIZE;

        size_t to_copy = okm_len - generated;
        if (to_copy > SHA256_DIGEST_SIZE) {
            to_copy = SHA256_DIGEST_SIZE;
        }
        kmemcpy(&out[generated], T, to_copy);
        generated += to_copy;
        counter++;
    }

    return true;
}

static size_t k_strlen(const char *s) {
    size_t l = 0;
    while (s && s[l]) l++;
    return l;
}

bool hkdf_expand_label(const u8 secret[SHA256_DIGEST_SIZE],
                       const char *label,
                       const u8 *context, size_t context_len,
                       u16 length, u8 *out) {
    if (!secret || !label || !out) return false;
    size_t label_len = k_strlen(label);
    if (label_len + 6 > 255 || context_len > 255) return false;

    /* RFC 8446 Section 7.1:
     * struct {
     *     uint16 length = Length;
     *     opaque label<7..255> = "tls13 " + Label;
     *     opaque context<0..255> = Context;
     * } HkdfLabel;
     */
    u8 hkdf_label[2 + 1 + 6 + 64 + 1 + 64];
    size_t idx = 0;

    /* length (uint16 big-endian) */
    hkdf_label[idx++] = (u8)(length >> 8);
    hkdf_label[idx++] = (u8)(length & 0xFF);

    /* label (1-byte length prefix + "tls13 " + label) */
    u8 full_label_len = (u8)(6 + label_len);
    hkdf_label[idx++] = full_label_len;
    kmemcpy(&hkdf_label[idx], "tls13 ", 6);
    idx += 6;
    kmemcpy(&hkdf_label[idx], label, label_len);
    idx += label_len;

    /* context (1-byte length prefix + context) */
    hkdf_label[idx++] = (u8)context_len;
    if (context && context_len > 0) {
        kmemcpy(&hkdf_label[idx], context, context_len);
        idx += context_len;
    }

    return hkdf_expand(secret, hkdf_label, idx, out, length);
}
