#include <kernel/crypto.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/test_certs.h>

bool crypto_self_test(void) {
    kprintf("\n=======================================================\n");
    kprintf("        AstraOS Phase L: Cryptographic Subsystem Tests \n");
    kprintf("=======================================================\n");

    /* 1. SHA-256 Known-Answer Test (FIPS 180-4 'abc') */
    kprintf("[CRYPTO TEST] 1. Testing SHA-256 (FIPS 180-4 'abc')...\n");
    static const u8 expected_abc_hash[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    u8 digest[32];
    sha256_hash("abc", 3, digest);
    if (!crypto_memcmp_eq(digest, expected_abc_hash, 32)) {
        kprintf("[CRYPTO TEST FAIL] SHA-256 digest did not match expected vector!\n");
        return false;
    }
    kprintf("  PASS: SHA-256 known-answer vector verified.\n");

    /* 2. HMAC-SHA256 Known-Answer Test (RFC 4231 Test Case 1) */
    kprintf("[CRYPTO TEST] 2. Testing HMAC-SHA256 (RFC 4231 TC1)...\n");
    static const u8 expected_hmac[32] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7
    };
    static const u8 tc1_key[20] = {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b
    };
    u8 hmac_out[32];
    hmac_sha256(tc1_key, sizeof(tc1_key), "Hi There", 8, hmac_out);
    if (!crypto_memcmp_eq(hmac_out, expected_hmac, 32)) {
        kprintf("[CRYPTO TEST FAIL] HMAC-SHA256 did not match expected vector!\n");
        return false;
    }
    kprintf("  PASS: HMAC-SHA256 known-answer vector verified.\n");

    /* 3. HKDF-SHA256 Known-Answer Test (RFC 5869 Test Case 1) */
    kprintf("[CRYPTO TEST] 3. Testing HKDF-SHA256 Extract & Expand (RFC 5869)...\n");
    static const u8 hkdf_ikm[22] = {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b
    };
    static const u8 hkdf_salt[13] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c
    };
    static const u8 hkdf_info[10] = {
        0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
        0xf8, 0xf9
    };
    static const u8 expected_prk[32] = {
        0x07, 0x77, 0x09, 0x36, 0x2c, 0x2e, 0x32, 0xdf,
        0x0d, 0xdc, 0x3f, 0x0d, 0xc4, 0x7b, 0xba, 0x63,
        0x90, 0xb6, 0xc7, 0x3b, 0xb5, 0x0f, 0x9c, 0x31,
        0x22, 0xec, 0x84, 0x4a, 0xd7, 0xc2, 0xb3, 0xe5
    };
    u8 prk[32];
    hkdf_extract(hkdf_salt, sizeof(hkdf_salt), hkdf_ikm, sizeof(hkdf_ikm), prk);
    if (!crypto_memcmp_eq(prk, expected_prk, 32)) {
        kprintf("[CRYPTO TEST FAIL] HKDF Extract PRK mismatch!\n");
        return false;
    }
    u8 okm[42];
    if (!hkdf_expand(prk, hkdf_info, sizeof(hkdf_info), okm, sizeof(okm))) {
        kprintf("[CRYPTO TEST FAIL] HKDF Expand failed!\n");
        return false;
    }
    static const u8 expected_okm_first8[8] = {
        0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a
    };
    if (!crypto_memcmp_eq(okm, expected_okm_first8, 8)) {
        kprintf("[CRYPTO TEST FAIL] HKDF Expand output mismatch!\n");
        return false;
    }
    kprintf("  PASS: HKDF Extract & Expand vectors verified.\n");

    /* 4. ChaCha20-Poly1305 AEAD Known-Answer Test (RFC 8439 Section 2.8.2) */
    kprintf("[CRYPTO TEST] 4. Testing ChaCha20-Poly1305 AEAD (RFC 8439)...\n");
    static const u8 aead_key[32] = {
        0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
        0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
        0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
        0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f
    };
    static const u8 aead_nonce[12] = {
        0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43,
        0x44, 0x45, 0x46, 0x47
    };
    static const u8 aead_aad[12] = {
        0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3,
        0xc4, 0xc5, 0xc6, 0xc7
    };
    const char *aead_plain = "Ladies and Gentlemen of the class of '99: If I could offer you "
                             "only one tip for the future, sunscreen would be it.";
    size_t plain_len = 114;

    u8 ciphertext[128];
    u8 tag[16];
    chacha20_poly1305_encrypt(aead_key, aead_nonce, aead_aad, sizeof(aead_aad),
                             aead_plain, plain_len, ciphertext, tag);

    static const u8 expected_tag[16] = {
        0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
        0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91
    };
    if (!crypto_memcmp_eq(tag, expected_tag, 16)) {
        kprintf("[CRYPTO TEST FAIL] ChaCha20-Poly1305 MAC tag mismatch!\n");
        return false;
    }

    /* Decryption & Authentication Verification */
    u8 decrypted[128] = {0};
    if (!chacha20_poly1305_decrypt(aead_key, aead_nonce, aead_aad, sizeof(aead_aad),
                                  ciphertext, plain_len, tag, decrypted) ||
        !crypto_memcmp_eq(decrypted, aead_plain, plain_len)) {
        kprintf("[CRYPTO TEST FAIL] ChaCha20-Poly1305 decrypt failed!\n");
        return false;
    }

    /* Negative Test: Tampered Ciphertext must fail authentication */
    ciphertext[0] ^= 0x01;
    if (chacha20_poly1305_decrypt(aead_key, aead_nonce, aead_aad, sizeof(aead_aad),
                                 ciphertext, plain_len, tag, decrypted)) {
        kprintf("[CRYPTO TEST FAIL] Tampered ciphertext was accepted!\n");
        return false;
    }
    ciphertext[0] ^= 0x01; /* Restore */
    kprintf("  PASS: ChaCha20-Poly1305 AEAD encrypt/decrypt & tamper rejection verified.\n");

    /* 5. X25519 Curve25519 Key Exchange Test (RFC 7748 Section 6.1) */
    kprintf("[CRYPTO TEST] 5. Testing X25519 Montgomery Curve (RFC 7748)...\n");
    static const u8 alice_priv[32] = {
        0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
        0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
        0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
        0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a
    };
    static const u8 expected_alice_pub[32] = {
        0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54,
        0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
        0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4,
        0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a
    };
    static const u8 basepoint[32] = {9};
    u8 alice_pub[32];
    x25519_scalarmult(alice_pub, alice_priv, basepoint);
    if (!crypto_memcmp_eq(alice_pub, expected_alice_pub, 32)) {
        kprintf("[CRYPTO TEST FAIL] X25519 public key derivation mismatch!\n");
        return false;
    }

    static const u8 bob_priv[32] = {
        0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b,
        0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
        0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd,
        0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb
    };
    static const u8 expected_bob_pub[32] = {
        0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4,
        0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
        0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d,
        0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f
    };
    u8 bob_pub[32];
    x25519_scalarmult(bob_pub, bob_priv, basepoint);
    if (!crypto_memcmp_eq(bob_pub, expected_bob_pub, 32)) {
        kprintf("[CRYPTO TEST FAIL] X25519 Bob public key mismatch!\n");
        return false;
    }

    u8 shared_alice[32];
    u8 shared_bob[32];
    x25519_scalarmult(shared_alice, alice_priv, bob_pub);
    x25519_scalarmult(shared_bob, bob_priv, alice_pub);

    static const u8 expected_shared[32] = {
        0x4a, 0x5d, 0x9d, 0x5b, 0xa4, 0xce, 0x2d, 0xe1,
        0x72, 0x8e, 0x3b, 0xf4, 0x80, 0x35, 0x0f, 0x25,
        0xe0, 0x7e, 0x21, 0xc9, 0x47, 0xd1, 0x9e, 0x33,
        0x76, 0xf0, 0x9b, 0x3c, 0x1e, 0x16, 0x17, 0x42
    };
    if (!crypto_memcmp_eq(shared_alice, expected_shared, 32) ||
        !crypto_memcmp_eq(shared_bob, expected_shared, 32)) {
        kprintf("[CRYPTO TEST FAIL] X25519 shared secret mismatch!\n");
        return false;
    }
    kprintf("  PASS: X25519 Diffie-Hellman key exchange verified.\n");

    /* 6. X.509 Certificate Parser & Validation Test with Real X.509 DER Certs */
    kprintf("[CRYPTO TEST] 6. Testing X.509 Certificate Parsing & Signature Verification...\n");
    x509_cert_t parsed_ca;
    if (!x509_parse_cert(test_ca_der, TEST_CA_DER_LEN, &parsed_ca)) {
        kprintf("[CRYPTO TEST FAIL] Failed to parse Root CA X.509 certificate!\n");
        return false;
    }

    x509_cert_t parsed_server;
    if (!x509_parse_cert(test_server_der, TEST_SERVER_DER_LEN, &parsed_server)) {
        kprintf("[CRYPTO TEST FAIL] Failed to parse Server X.509 certificate!\n");
        return false;
    }

    /* Check parsed properties */
    if (parsed_server.dns_count == 0 || parsed_server.not_before == 0 || parsed_server.not_after == 0) {
        kprintf("[CRYPTO TEST FAIL] Parsed server cert missing DNS SAN or validity period!\n");
        return false;
    }

    /* Valid verification at a time between not_before and not_after */
    u64 test_valid_time = parsed_server.not_before + 3600;
    cert_status_t st = x509_verify_cert(&parsed_server, &parsed_ca, "test.astraos.local", test_valid_time);
    if (st != CERT_OK) {
        kprintf("[CRYPTO TEST FAIL] Valid server cert failed verification against Root CA (st=%d)!\n", (int)st);
        return false;
    }

    /* Also verify matching SAN: 'localhost' */
    if (x509_verify_cert(&parsed_server, &parsed_ca, "localhost", test_valid_time) != CERT_OK) {
        kprintf("[CRYPTO TEST FAIL] Valid SAN 'localhost' was not accepted!\n");
        return false;
    }

    /* Negative Test 1: Expired certificate (time past not_after) */
    if (x509_verify_cert(&parsed_server, &parsed_ca, "test.astraos.local", parsed_server.not_after + 86400) != CERT_ERR_EXPIRED) {
        kprintf("[CRYPTO TEST FAIL] Expired certificate was not rejected!\n");
        return false;
    }

    /* Negative Test 2: Hostname mismatch */
    if (x509_verify_cert(&parsed_server, &parsed_ca, "unrelated.domain.com", test_valid_time) != CERT_ERR_HOSTNAME_MISMATCH) {
        kprintf("[CRYPTO TEST FAIL] Hostname mismatch was not rejected!\n");
        return false;
    }

    /* Negative Test 3: Untrusted issuer */
    x509_cert_t untrusted_ca = parsed_ca;
    kmemcpy(untrusted_ca.common_name, "Rogue Untrusted CA", 19);
    if (x509_verify_cert(&parsed_server, &untrusted_ca, "test.astraos.local", test_valid_time) != CERT_ERR_UNTRUSTED_ISSUER) {
        kprintf("[CRYPTO TEST FAIL] Untrusted issuer was not rejected!\n");
        return false;
    }

    /* Negative Test 4: Corrupted signature */
    x509_cert_t bad_sig_cert = parsed_server;
    bad_sig_cert.sig[10] ^= 0x55;
    if (x509_verify_cert(&bad_sig_cert, &parsed_ca, "test.astraos.local", test_valid_time) != CERT_ERR_INVALID_SIGNATURE) {
        kprintf("[CRYPTO TEST FAIL] Corrupted certificate signature was not rejected!\n");
        return false;
    }

    kprintf("  PASS: Real OpenSSL X.509 DER certs, SANs, RSA signatures & negative rejection verified.\n");
    kprintf("ASTRA_TLS_CERT_VALIDATION_OK\n\n");

    kprintf("[CRYPTO TEST] All Cryptographic Subsystem tests passed successfully!\n");
    return true;
}
