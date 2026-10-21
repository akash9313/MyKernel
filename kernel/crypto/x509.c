#include <kernel/crypto.h>
#include <kernel/mem.h>
#include <kernel/kprintf.h>

/* ASN.1 DER tags */
#define ASN1_BOOLEAN          0x01
#define ASN1_INTEGER          0x02
#define ASN1_BIT_STRING       0x03
#define ASN1_OCTET_STRING     0x04
#define ASN1_NULL             0x05
#define ASN1_OID              0x06
#define ASN1_UTF8_STRING      0x0C
#define ASN1_PRINTABLE_STRING 0x13
#define ASN1_IA5_STRING       0x16
#define ASN1_UTC_TIME         0x17
#define ASN1_GENERALIZED_TIME 0x18
#define ASN1_SEQUENCE         0x30
#define ASN1_SET              0x31

static bool asn1_get_tlv(const u8 *buf, size_t len, size_t *pos,
                         u8 *tag, const u8 **val, size_t *val_len) {
    if (!buf || !pos || *pos >= len) return false;

    *tag = buf[(*pos)++];
    if (*pos >= len) return false;

    size_t length = buf[(*pos)++];
    if (length & 0x80) {
        size_t num_bytes = length & 0x7F;
        if (num_bytes == 0 || num_bytes > 4 || *pos + num_bytes > len) {
            return false;
        }
        length = 0;
        for (size_t i = 0; i < num_bytes; i++) {
            length = (length << 8) | buf[(*pos)++];
        }
    }

    if (*pos + length > len) return false;

    *val = &buf[*pos];
    *val_len = length;
    *pos += length;
    return true;
}

static u64 parse_time_str(const u8 *val, size_t len, bool is_utc) {
    if (len < 12) return 0;
    u16 year;
    size_t idx = 0;
    if (is_utc) {
        u8 y = (val[0] - '0') * 10 + (val[1] - '0');
        year = (y >= 50) ? (1900 + y) : (2000 + y);
        idx = 2;
    } else {
        year = (val[0] - '0') * 1000 + (val[1] - '0') * 100 + (val[2] - '0') * 10 + (val[3] - '0');
        idx = 4;
    }

    u8 month = (val[idx] - '0') * 10 + (val[idx + 1] - '0'); idx += 2;
    u8 day   = (val[idx] - '0') * 10 + (val[idx + 1] - '0'); idx += 2;
    u8 hour  = (val[idx] - '0') * 10 + (val[idx + 1] - '0'); idx += 2;
    u8 min   = (val[idx] - '0') * 10 + (val[idx + 1] - '0'); idx += 2;
    u8 sec   = (val[idx] - '0') * 10 + (val[idx + 1] - '0');

    static const u16 days_before_month[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };

    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) return 0;

    u64 days = 365ULL * (year - 1970);
    for (u16 y = 1970; y < year; y++) {
        if (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) days++;
    }
    days += days_before_month[month - 1];
    if (month > 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
        days++;
    }
    days += (day - 1);

    return (days * 86400ULL) + ((u64)hour * 3600ULL) + ((u64)min * 60ULL) + sec;
}

static void parse_name(const u8 *val, size_t len, char *cn, size_t max_cn) {
    if (!val || len == 0 || !cn || max_cn == 0) return;
    cn[0] = '\0';

    /* Look for CN OID: 2.5.4.3 = 0x55, 0x04, 0x03 */
    static const u8 cn_oid[] = {0x55, 0x04, 0x03};
    for (size_t i = 0; i + 3 < len; i++) {
        if (val[i] == 0x06 && val[i+1] == 0x03 &&
            val[i+2] == cn_oid[0] && val[i+3] == cn_oid[1] && val[i+4] == cn_oid[2]) {
            /* Next element is the string (UTF8String 0x0C or PrintableString 0x13) */
            size_t str_pos = i + 5;
            if (str_pos + 2 < len) {
                u8 str_tag = val[str_pos++];
                size_t str_len = val[str_pos++];
                if (str_pos + str_len <= len &&
                    (str_tag == ASN1_UTF8_STRING || str_tag == ASN1_PRINTABLE_STRING || str_tag == ASN1_IA5_STRING)) {
                    size_t to_copy = (str_len < max_cn - 1) ? str_len : (max_cn - 1);
                    kmemcpy(cn, &val[str_pos], to_copy);
                    cn[to_copy] = '\0';
                    return;
                }
            }
        }
    }
}

static bool parse_rsa_pubkey(const u8 *spki, size_t spki_len, rsa_pubkey_t *key) {
    if (!spki || spki_len == 0 || !key) return false;
    kmemset(key, 0, sizeof(*key));

    /* Find BIT STRING (0x03) in SPKI */
    for (size_t i = 0; i + 2 < spki_len; i++) {
        if (spki[i] == ASN1_BIT_STRING) {
            size_t pos = i;
            u8 tag;
            const u8 *val;
            size_t val_len;
            if (asn1_get_tlv(spki, spki_len, &pos, &tag, &val, &val_len) && val_len > 1) {
                /* Skip unused bits byte (first byte) */
                const u8 *der = val + 1;
                size_t der_len = val_len - 1;

                /* Parse RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER } */
                size_t kpos = 0;
                u8 stag;
                const u8 *sval;
                size_t slen;
                if (asn1_get_tlv(der, der_len, &kpos, &stag, &sval, &slen) && stag == ASN1_SEQUENCE) {
                    size_t ipos = 0;
                    u8 mtag;
                    const u8 *mval;
                    size_t mlen;
                    /* Modulus */
                    if (asn1_get_tlv(sval, slen, &ipos, &mtag, &mval, &mlen) && mtag == ASN1_INTEGER) {
                        /* Strip leading zero byte if present */
                        if (mlen > 0 && mval[0] == 0x00) {
                            mval++;
                            mlen--;
                        }
                        if (mlen <= RSA_MAX_KEY_BYTES) {
                            kmemcpy(key->n, mval, mlen);
                            key->n_len = mlen;
                        }
                    }
                    /* Exponent */
                    u8 etag;
                    const u8 *eval;
                    size_t elen;
                    if (asn1_get_tlv(sval, slen, &ipos, &etag, &eval, &elen) && etag == ASN1_INTEGER) {
                        u32 e = 0;
                        for (size_t b = 0; b < elen; b++) {
                            e = (e << 8) | eval[b];
                        }
                        key->e = e;
                        return (key->n_len > 0 && key->e > 0);
                    }
                }
            }
        }
    }
    return false;
}

bool x509_parse_cert(const u8 *der, size_t len, x509_cert_t *cert) {
    if (!der || len == 0 || !cert) return false;
    kmemset(cert, 0, sizeof(*cert));

    size_t pos = 0;
    u8 tag;
    const u8 *val;
    size_t val_len;

    /* 1. Outer Certificate SEQUENCE */
    if (!asn1_get_tlv(der, len, &pos, &tag, &val, &val_len) || tag != ASN1_SEQUENCE) {
        return false;
    }

    /* 2. TBSCertificate SEQUENCE */
    size_t cpos = 0;
    const u8 *cval = val;
    size_t clen = val_len;

    const u8 *tbs_val;
    size_t tbs_len;
    if (!asn1_get_tlv(cval, clen, &cpos, &tag, &tbs_val, &tbs_len) || tag != ASN1_SEQUENCE) {
        return false;
    }
    size_t tbs_full_len = cpos;

    /* Compute SHA-256 hash of TBSCertificate for signature verification */
    sha256_hash(cval, tbs_full_len, cert->tbs_hash);

    /* Parse elements inside TBSCertificate */
    size_t tbspos = 0;
    u8 ttag;
    const u8 *elem;
    size_t elem_len;

    /* Optional Version [0] */
    if (tbspos < tbs_len && tbs_val[tbspos] == 0xA0) {
        asn1_get_tlv(tbs_val, tbs_len, &tbspos, &ttag, &elem, &elem_len);
    }

    /* Serial Number (INTEGER) */
    if (!asn1_get_tlv(tbs_val, tbs_len, &tbspos, &ttag, &elem, &elem_len) || ttag != ASN1_INTEGER) {
        return false;
    }

    /* Signature Algorithm */
    if (!asn1_get_tlv(tbs_val, tbs_len, &tbspos, &ttag, &elem, &elem_len) || ttag != ASN1_SEQUENCE) {
        return false;
    }

    /* Issuer Name */
    if (!asn1_get_tlv(tbs_val, tbs_len, &tbspos, &ttag, &elem, &elem_len) || ttag != ASN1_SEQUENCE) {
        return false;
    }
    parse_name(elem, elem_len, (char *)cert->issuer_cn, sizeof(cert->issuer_cn));

    /* Validity (SEQUENCE of notBefore, notAfter) */
    if (!asn1_get_tlv(tbs_val, tbs_len, &tbspos, &ttag, &elem, &elem_len) || ttag != ASN1_SEQUENCE) {
        return false;
    }
    size_t vpos = 0;
    u8 vtag;
    const u8 *vval;
    size_t vlen;
    if (asn1_get_tlv(elem, elem_len, &vpos, &vtag, &vval, &vlen)) {
        cert->not_before = parse_time_str(vval, vlen, vtag == ASN1_UTC_TIME);
    }
    if (asn1_get_tlv(elem, elem_len, &vpos, &vtag, &vval, &vlen)) {
        cert->not_after = parse_time_str(vval, vlen, vtag == ASN1_UTC_TIME);
    }

    /* Subject Name */
    if (!asn1_get_tlv(tbs_val, tbs_len, &tbspos, &ttag, &elem, &elem_len) || ttag != ASN1_SEQUENCE) {
        return false;
    }
    parse_name(elem, elem_len, cert->common_name, sizeof(cert->common_name));

    /* SubjectPublicKeyInfo */
    if (!asn1_get_tlv(tbs_val, tbs_len, &tbspos, &ttag, &elem, &elem_len) || ttag != ASN1_SEQUENCE) {
        return false;
    }
    parse_rsa_pubkey(elem, elem_len, &cert->pubkey);

    /* Look for Extensions [3] */
    while (tbspos < tbs_len) {
        if (!asn1_get_tlv(tbs_val, tbs_len, &tbspos, &ttag, &elem, &elem_len)) break;
        if (ttag == 0xA3) { /* [3] EXPLICIT Extensions */
            /* Scan for Subject Alternative Name (SAN) OID: 2.5.29.17 = 0x55, 0x1D, 0x11 */
            for (size_t s = 0; s + 3 < elem_len; s++) {
                if (elem[s] == 0x55 && elem[s+1] == 0x1D && elem[s+2] == 0x11) {
                    /* Locate dNSName (0x82) and iPAddress (0x87) tags */
                    for (size_t d = s + 3; d + 2 < elem_len; d++) {
                        if (elem[d] == 0x82 && cert->dns_count < 4) {
                            size_t dlen = elem[d+1];
                            if (d + 2 + dlen <= elem_len && dlen < 64) {
                                kmemcpy(cert->dns_names[cert->dns_count], &elem[d+2], dlen);
                                cert->dns_names[cert->dns_count][dlen] = '\0';
                                cert->dns_count++;
                                d += 1 + dlen;
                            }
                        } else if (elem[d] == 0x87 && cert->dns_count < 4) {
                            size_t dlen = elem[d+1];
                            if (d + 2 + dlen <= elem_len && dlen == 4) {
                                const u8 *ip = &elem[d+2];
                                char *dst = cert->dns_names[cert->dns_count];
                                int di = 0;
                                for (int b = 0; b < 4; b++) {
                                    u8 val = ip[b];
                                    if (val >= 100) {
                                        dst[di++] = '0' + (val / 100);
                                        dst[di++] = '0' + ((val / 10) % 10);
                                        dst[di++] = '0' + (val % 10);
                                    } else if (val >= 10) {
                                        dst[di++] = '0' + (val / 10);
                                        dst[di++] = '0' + (val % 10);
                                    } else {
                                        dst[di++] = '0' + val;
                                    }
                                    if (b < 3) dst[di++] = '.';
                                }
                                dst[di] = '\0';
                                cert->dns_count++;
                                d += 1 + dlen;
                            }
                        }
                    }
                }
            }
        }
    }

    /* 3. Outer SignatureAlgorithm */
    if (!asn1_get_tlv(cval, clen, &cpos, &tag, &val, &val_len) || tag != ASN1_SEQUENCE) {
        return false;
    }

    /* 4. Outer SignatureValue (BIT STRING) */
    if (!asn1_get_tlv(cval, clen, &cpos, &tag, &val, &val_len) || tag != ASN1_BIT_STRING || val_len <= 1) {
        return false;
    }
    /* Skip unused bits byte */
    const u8 *sig_bytes = val + 1;
    size_t sig_len = val_len - 1;
    if (sig_len <= RSA_MAX_KEY_BYTES) {
        kmemcpy(cert->sig, sig_bytes, sig_len);
        cert->sig_len = sig_len;
    }

    cert->is_valid_format = (cert->pubkey.n_len > 0 && cert->sig_len > 0);
    return cert->is_valid_format;
}

static bool str_equals_ci(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += ('a' - 'A');
        if (cb >= 'A' && cb <= 'Z') cb += ('a' - 'A');
        if (ca != cb) return false;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

cert_status_t x509_verify_cert(const x509_cert_t *cert,
                              const x509_cert_t *issuer_ca,
                              const char *hostname,
                              u64 current_time) {
    if (!cert || !cert->is_valid_format) {
        return CERT_ERR_MALFORMED;
    }

    /* 1. Time validity checks */
    if (current_time != 0) {
        if (cert->not_before != 0 && current_time < cert->not_before) {
            return CERT_ERR_NOT_YET_VALID;
        }
        if (cert->not_after != 0 && current_time > cert->not_after) {
            return CERT_ERR_EXPIRED;
        }
    }

    /* 2. Hostname validation */
    if (hostname && hostname[0] != '\0') {
        bool match = false;
        if (cert->dns_count > 0) {
            for (int i = 0; i < cert->dns_count; i++) {
                if (str_equals_ci(hostname, cert->dns_names[i])) {
                    match = true;
                    break;
                }
            }
        } else if (cert->common_name[0] != '\0') {
            if (str_equals_ci(hostname, cert->common_name)) {
                match = true;
            }
        }
        if (!match) {
            return CERT_ERR_HOSTNAME_MISMATCH;
        }
    }

    /* 3. Issuer CA Trust and Signature Verification */
    if (issuer_ca) {
        if (!issuer_ca->is_valid_format) {
            return CERT_ERR_UNTRUSTED_ISSUER;
        }

        /* Check issuer common name match */
        if (!str_equals_ci((const char *)cert->issuer_cn, issuer_ca->common_name)) {
            return CERT_ERR_UNTRUSTED_ISSUER;
        }

        /* Verify RSA signature over TBS hash */
        bool sig_ok = rsa_verify_pkcs1_v15_sha256(&issuer_ca->pubkey,
                                                 cert->tbs_hash, SHA256_DIGEST_SIZE,
                                                 cert->sig, cert->sig_len);
        if (!sig_ok) {
            /* Try RSA-PSS */
            sig_ok = rsa_verify_pss_sha256(&issuer_ca->pubkey,
                                          cert->tbs_hash, SHA256_DIGEST_SIZE,
                                          cert->sig, cert->sig_len);
        }

        if (!sig_ok) {
            return CERT_ERR_INVALID_SIGNATURE;
        }
    }

    return CERT_OK;
}
