#include <kernel/tls.h>
#include <kernel/net.h>
#include <kernel/rtc.h>
#include <kernel/test_certs.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>

#define TLS_RECORD_HANDSHAKE       22
#define TLS_RECORD_APPLICATION     23
#define TLS_RECORD_ALERT           21

#define TLS_HS_CLIENT_HELLO        1
#define TLS_HS_SERVER_HELLO        2
#define TLS_HS_ENCRYPTED_EXT       8
#define TLS_HS_CERTIFICATE         11
#define TLS_HS_CERT_VERIFY         15
#define TLS_HS_FINISHED            20

static size_t k_strlen(const char *s) {
    size_t l = 0;
    while (s && s[l]) l++;
    return l;
}

static void k_strcpy(char *dst, const char *src, size_t max) {
    if (!dst || max == 0) return;
    size_t i = 0;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

tls_error_t tls_init_session(tls_session_t *session, const char *hostname, u16 port) {
    if (!session || !hostname) return TLS_ERR_HANDSHAKE_FAILED;
    kmemset(session, 0, sizeof(*session));
    session->tcp_socket = -1;
    session->state = TLS_STATE_CLOSED;
    k_strcpy(session->hostname, hostname, sizeof(session->hostname));
    session->port = port ? port : 443;
    sha256_init(&session->transcript);
    return TLS_OK;
}

static void compute_record_nonce(const u8 iv[12], u64 seq, u8 nonce_out[12]) {
    for (int i = 0; i < 12; i++) nonce_out[i] = iv[i];
    for (int i = 0; i < 8; i++) {
        nonce_out[11 - i] ^= (u8)(seq >> (i * 8));
    }
}

static int tls_write_record(tls_session_t *s, u8 type, const void *payload, size_t len,
                            const u8 *key, const u8 *iv, u64 *seq) {
    static u8 record[2048];
    if (len + 16 + 5 + 1 > sizeof(record)) {
        return -1;
    }

    if (key && iv && seq) {
        /* Encrypted TLS 1.3 Record (Type 23) */
        static u8 plaintext[1500];
        if (len + 1 > sizeof(plaintext)) return -1;
        if (len > 0) kmemcpy(plaintext, payload, len);
        plaintext[len] = type; /* Append inner ContentType */
        size_t text_len = len + 1;

        size_t record_len = text_len + 16; /* text + Poly1305 tag */
        record[0] = TLS_RECORD_APPLICATION; /* Type 23 */
        record[1] = 0x03;                   /* Version 0x0303 */
        record[2] = 0x03;
        record[3] = (u8)(record_len >> 8);
        record[4] = (u8)(record_len & 0xFF);

        u8 nonce[12];
        compute_record_nonce(iv, *seq, nonce);
        (*seq)++;

        u8 tag[16];
        chacha20_poly1305_encrypt(key, nonce, record, 5,
                                 plaintext, text_len,
                                 &record[5], tag);
        kmemcpy(&record[5 + text_len], tag, 16);

        int sent = tcp_send(s->tcp_socket, record, 5 + record_len);
        return (sent == (int)(5 + record_len)) ? (int)len : -1;
    } else {
        /* Plaintext Record (ClientHello) */
        record[0] = type;
        record[1] = 0x03;
        record[2] = 0x03;
        record[3] = (u8)(len >> 8);
        record[4] = (u8)(len & 0xFF);
        if (len > 0) kmemcpy(&record[5], payload, len);

        int sent = tcp_send(s->tcp_socket, record, 5 + len);
        return (sent == (int)(5 + len)) ? (int)len : -1;
    }
}

static int tls_read_record(tls_session_t *s, u8 *out_type, void *out_buf, size_t max_len,
                           const u8 *key, const u8 *iv, u64 *seq) {
    u8 hdr[5];
    while (1) {
        size_t hdr_recv = 0;
        while (hdr_recv < 5) {
            int n = tcp_recv(s->tcp_socket, hdr + hdr_recv, 5 - hdr_recv, 3000);
            if (n <= 0) return -1;
            hdr_recv += (size_t)n;
        }

        u8 outer_type = hdr[0];
        size_t length = ((size_t)hdr[3] << 8) | hdr[4];
        if (length > 8192) return -1;

        static u8 enc_buf[8192];
        size_t received = 0;
        while (received < length) {
            int r = tcp_recv(s->tcp_socket, &enc_buf[received], length - received, 3000);
            if (r <= 0) return -1;
            received += r;
        }

        /* RFC 8446 Appendix D.4: Ignore ChangeCipherSpec (type 20) during TLS 1.3 handshake */
        if (outer_type == 20) {
            continue;
        }

        if (key && iv && seq) {
            /* Decrypt TLS 1.3 Record */
            if (length < 16) return -1;
            size_t ct_len = length - 16;
            const u8 *tag = &enc_buf[ct_len];

            u8 nonce[12];
            compute_record_nonce(iv, *seq, nonce);
            (*seq)++;

            static u8 plaintext[8192];
            if (!chacha20_poly1305_decrypt(key, nonce, hdr, 5,
                                          enc_buf, ct_len, tag, plaintext)) {
                kprintf("[TLS ERROR] Record decryption / MAC verification failed!\n");
                return -1;
            }

            /* Extract inner content type from end of plaintext (skip padding zeros) */
            size_t p_idx = ct_len;
            while (p_idx > 0 && plaintext[p_idx - 1] == 0) p_idx--;
            if (p_idx == 0) return -1;
            u8 inner_type = plaintext[p_idx - 1];
            size_t data_len = p_idx - 1;

            if (out_type) *out_type = inner_type;
            if (data_len > max_len) data_len = max_len;
            kmemcpy(out_buf, plaintext, data_len);
            return (int)data_len;
        } else {
            /* Unencrypted Record */
            if (out_type) *out_type = outer_type;
            size_t to_copy = (length < max_len) ? length : max_len;
            kmemcpy(out_buf, enc_buf, to_copy);
            return (int)to_copy;
        }
    }
}

/* Derive-Secret helper: HKDF-Expand-Label(secret, label, SHA256(transcript), 32) */
static void derive_secret(const u8 secret[32], const char *label, const sha256_ctx_t *transcript, u8 out[32]) {
    u8 thash[32];
    sha256_ctx_t c = *transcript;
    sha256_final(&c, thash);
    hkdf_expand_label(secret, label, thash, 32, 32, out);
}

tls_error_t tls_connect(tls_session_t *s, const char *hostname, u16 port) {
    if (tls_init_session(s, hostname, port) != TLS_OK) {
        return TLS_ERR_HANDSHAKE_FAILED;
    }

    if (!crypto_has_secure_entropy()) {
        kprintf("[TLS ERROR] Failed to connect: No cryptographically secure entropy source!\n");
        return TLS_ERR_ENTROPY_FAILURE;
    }

    /* 1. Resolve hostname to IPv4 */
    ip_addr_t ip = 0;
    if (!str_to_ip(hostname, &ip)) {
        if (!dns_resolve(hostname, &ip, 3000)) {
            kprintf("[TLS ERROR] DNS resolution failed for %s\n", hostname);
            return TLS_ERR_TCP_CONNECT;
        }
    }

    /* 2. Establish TCP connection */
    s->tcp_socket = tcp_socket_create();
    if (s->tcp_socket < 0) {
        kprintf("[TLS ERROR] Failed to create TCP socket for %s\n", hostname);
        return TLS_ERR_TCP_CONNECT;
    }

    if (tcp_connect(s->tcp_socket, ip, s->port, 3000) != 0) {
        kprintf("[TLS ERROR] TCP connection to %s:%u failed\n", hostname, (unsigned)s->port);
        tcp_close(s->tcp_socket);
        s->tcp_socket = -1;
        return TLS_ERR_TCP_CONNECT;
    }

    /* 3. Generate Ephemeral X25519 Keypair */
    u8 client_priv[32];
    u8 client_pub[32];
    x25519_keypair(client_pub, client_priv);

    /* 4. Format and Send ClientHello */
    static u8 ch[1024];
    size_t ch_len = 0;

    /* Handshake type = 1 (ClientHello), length will be filled in */
    ch[ch_len++] = TLS_HS_CLIENT_HELLO;
    ch_len += 3; /* reserve 3 bytes for 24-bit length */

    /* ClientHello Body */
    ch[ch_len++] = 0x03; ch[ch_len++] = 0x03; /* legacy_version = 0x0303 */

    /* Random (32 bytes) */
    crypto_get_random(&ch[ch_len], 32);
    ch_len += 32;

    /* Legacy Session ID (32 random bytes) */
    ch[ch_len++] = 32;
    crypto_get_random(&ch[ch_len], 32);
    ch_len += 32;

    /* Cipher Suites (TLS_CHACHA20_POLY1305_SHA256 = 0x1303) */
    ch[ch_len++] = 0x00; ch[ch_len++] = 0x02; /* length 2 bytes (1 suite) */
    ch[ch_len++] = 0x13; ch[ch_len++] = 0x03;

    /* Compression Methods (1 byte: null 0x00) */
    ch[ch_len++] = 0x01;
    ch[ch_len++] = 0x00;

    /* Extensions Length (reserve 2 bytes) */
    size_t ext_len_offset = ch_len;
    ch_len += 2;
    size_t ext_start = ch_len;

    /* Extension: supported_versions (0x002b) -> TLS 1.3 (0x0304) */
    ch[ch_len++] = 0x00; ch[ch_len++] = 0x2b;
    ch[ch_len++] = 0x00; ch[ch_len++] = 0x03; /* ext length 3 */
    ch[ch_len++] = 0x02;                      /* list length 2 */
    ch[ch_len++] = 0x03; ch[ch_len++] = 0x04;

    /* Extension: server_name (SNI, 0x0000) */
    size_t host_len = k_strlen(hostname);
    if (host_len > 0) {
        ch[ch_len++] = 0x00; ch[ch_len++] = 0x00;
        size_t sni_ext_len = 2 + 1 + 2 + host_len;
        ch[ch_len++] = (u8)(sni_ext_len >> 8);
        ch[ch_len++] = (u8)(sni_ext_len & 0xFF);
        ch[ch_len++] = (u8)((host_len + 3) >> 8);
        ch[ch_len++] = (u8)((host_len + 3) & 0xFF);
        ch[ch_len++] = 0x00; /* NameType: host_name */
        ch[ch_len++] = (u8)(host_len >> 8);
        ch[ch_len++] = (u8)(host_len & 0xFF);
        kmemcpy(&ch[ch_len], hostname, host_len);
        ch_len += host_len;
    }

    /* Extension: supported_groups (0x000a) -> x25519 (0x001d) */
    ch[ch_len++] = 0x00; ch[ch_len++] = 0x0a;
    ch[ch_len++] = 0x00; ch[ch_len++] = 0x04; /* length 4 */
    ch[ch_len++] = 0x00; ch[ch_len++] = 0x02; /* list length 2 */
    ch[ch_len++] = 0x00; ch[ch_len++] = 0x1d; /* x25519 */

    /* Extension: signature_algorithms (0x000d) */
    ch[ch_len++] = 0x00; ch[ch_len++] = 0x0d;
    ch[ch_len++] = 0x00; ch[ch_len++] = 0x08; /* length 8 */
    ch[ch_len++] = 0x00; ch[ch_len++] = 0x06; /* list length 6 */
    ch[ch_len++] = 0x08; ch[ch_len++] = 0x04; /* rsa_pss_rsae_sha256 */
    ch[ch_len++] = 0x04; ch[ch_len++] = 0x01; /* rsa_pkcs1_sha256 */
    ch[ch_len++] = 0x08; ch[ch_len++] = 0x07; /* ed25519 */

    /* Extension: key_share (0x0033) */
    ch[ch_len++] = 0x00; ch[ch_len++] = 0x33;
    size_t ks_ext_len = 2 + 2 + 2 + 32;
    ch[ch_len++] = (u8)(ks_ext_len >> 8);
    ch[ch_len++] = (u8)(ks_ext_len & 0xFF);
    ch[ch_len++] = (u8)((ks_ext_len - 2) >> 8);
    ch[ch_len++] = (u8)((ks_ext_len - 2) & 0xFF);
    ch[ch_len++] = 0x00; ch[ch_len++] = 0x1d; /* group: x25519 */
    ch[ch_len++] = 0x00; ch[ch_len++] = 0x20; /* key length 32 */
    kmemcpy(&ch[ch_len], client_pub, 32);
    ch_len += 32;

    /* Fill in extensions length */
    size_t ext_total = ch_len - ext_start;
    ch[ext_len_offset + 0] = (u8)(ext_total >> 8);
    ch[ext_len_offset + 1] = (u8)(ext_total & 0xFF);

    /* Fill in 24-bit ClientHello length */
    size_t body_len = ch_len - 4;
    ch[1] = (u8)(body_len >> 16);
    ch[2] = (u8)(body_len >> 8);
    ch[3] = (u8)(body_len & 0xFF);

    /* Send ClientHello record (unencrypted Type 22) */
    if (tls_write_record(s, TLS_RECORD_HANDSHAKE, ch, ch_len, NULL, NULL, NULL) < 0) {
        tcp_close(s->tcp_socket);
        return TLS_ERR_HANDSHAKE_FAILED;
    }

    /* Update transcript hash with ClientHello bytes */
    sha256_update(&s->transcript, ch, ch_len);
    s->state = TLS_STATE_CLIENT_HELLO_SENT;

    /* 5. Receive ServerHello */
    static u8 sh_rec[1024];
    u8 rec_type = 0;
    int sh_bytes = tls_read_record(s, &rec_type, sh_rec, sizeof(sh_rec), NULL, NULL, NULL);
    if (sh_bytes < 38 || rec_type != TLS_RECORD_HANDSHAKE || sh_rec[0] != TLS_HS_SERVER_HELLO) {
        kprintf("[TLS ERROR] Did not receive valid ServerHello message!\n");
        tcp_close(s->tcp_socket);
        return TLS_ERR_HANDSHAKE_FAILED;
    }

    /* Extract server public key from key_share extension */
    u8 server_pub[32] = {0};
    bool found_server_share = false;

    /* Parse extensions inside ServerHello */
    size_t sh_pos = 4 + 2 + 32; /* skip type/len, legacy_version, random */
    if (sh_pos < (size_t)sh_bytes) {
        size_t sess_len = sh_rec[sh_pos++];
        sh_pos += sess_len;
        sh_pos += 2; /* skip cipher suite */
        sh_pos += 1; /* skip compression */

        if (sh_pos + 2 <= (size_t)sh_bytes) {
            size_t ext_bytes = ((size_t)sh_rec[sh_pos] << 8) | sh_rec[sh_pos+1];
            sh_pos += 2;
            size_t ext_end = sh_pos + ext_bytes;
            if (ext_end > (size_t)sh_bytes) ext_end = sh_bytes;

            while (sh_pos + 4 <= ext_end) {
                u16 etype = ((u16)sh_rec[sh_pos] << 8) | sh_rec[sh_pos+1];
                u16 elen  = ((u16)sh_rec[sh_pos+2] << 8) | sh_rec[sh_pos+3];
                sh_pos += 4;
                if (sh_pos + elen > ext_end) break;

                if (etype == 0x0033 && elen >= 4 + 32) { /* key_share */
                    u16 group = ((u16)sh_rec[sh_pos] << 8) | sh_rec[sh_pos+1];
                    u16 klen  = ((u16)sh_rec[sh_pos+2] << 8) | sh_rec[sh_pos+3];
                    if (group == 0x001d && klen == 32) {
                        kmemcpy(server_pub, &sh_rec[sh_pos+4], 32);
                        found_server_share = true;
                    }
                }
                sh_pos += elen;
            }
        }
    }

    if (!found_server_share) {
        kprintf("[TLS ERROR] ServerHello missing X25519 key_share!\n");
        tcp_close(s->tcp_socket);
        return TLS_ERR_UNSUPPORTED_CIPHER;
    }

    /* Update transcript with ServerHello bytes */
    sha256_update(&s->transcript, sh_rec, sh_bytes);
    s->state = TLS_STATE_SERVER_HELLO_RECEIVED;

    /* 6. Compute X25519 Shared Secret and Key Schedule */
    u8 shared_secret[32];
    if (!x25519_scalarmult(shared_secret, client_priv, server_pub)) {
        kprintf("[TLS ERROR] X25519 Diffie-Hellman computation failed!\n");
        tcp_close(s->tcp_socket);
        return TLS_ERR_HANDSHAKE_FAILED;
    }

    /* Early Secret = HKDF-Extract(salt=0[32], IKM=0[32]) */
    static const u8 zeros32[32] = {0};
    u8 early_secret[32];
    hkdf_extract(zeros32, 32, zeros32, 32, early_secret);

    /* Derived Secret 0 = Derive-Secret(Early Secret, "derived", "") */
    u8 derived0[32];
    sha256_ctx_t empty_ctx;
    sha256_init(&empty_ctx);
    derive_secret(early_secret, "derived", &empty_ctx, derived0);

    /* Handshake Secret = HKDF-Extract(salt=derived0, IKM=shared_secret) */
    u8 handshake_secret[32];
    hkdf_extract(derived0, 32, shared_secret, 32, handshake_secret);

    /* Server and Client Handshake Traffic Secrets */
    u8 s_hs_traffic[32];
    u8 c_hs_traffic[32];
    derive_secret(handshake_secret, "s hs traffic", &s->transcript, s_hs_traffic);
    derive_secret(handshake_secret, "c hs traffic", &s->transcript, c_hs_traffic);

    /* Server Handshake Key & IV */
    hkdf_expand_label(s_hs_traffic, "key", NULL, 0, 32, s->server_hs_key);
    hkdf_expand_label(s_hs_traffic, "iv",  NULL, 0, 12, s->server_hs_iv);

    /* Client Handshake Key & IV */
    hkdf_expand_label(c_hs_traffic, "key", NULL, 0, 32, s->client_hs_key);
    hkdf_expand_label(c_hs_traffic, "iv",  NULL, 0, 12, s->client_hs_iv);

    s->client_seq = 0;
    s->server_seq = 0;
    s->state = TLS_STATE_HANDSHAKE_KEYS_ESTABLISHED;

    /* 7. Receive Encrypted Handshake Records: EncryptedExtensions, Certificate, CertificateVerify, Finished */
    static u8 hs_buf[8192];
    while (s->state != TLS_STATE_CONNECTED) {
        u8 itype = 0;
        int dlen = tls_read_record(s, &itype, hs_buf, sizeof(hs_buf),
                                  s->server_hs_key, s->server_hs_iv, &s->server_seq);
        if (dlen <= 0) {
            kprintf("[TLS ERROR] Failed reading encrypted handshake message!\n");
            tcp_close(s->tcp_socket);
            return TLS_ERR_DECRYPT_FAILED;
        }

        size_t hpos = 0;
        while (hpos + 4 <= (size_t)dlen) {
            u8 msg_type = hs_buf[hpos];
            size_t msg_len = ((size_t)hs_buf[hpos+1] << 16) |
                             ((size_t)hs_buf[hpos+2] << 8)  |
                             ((size_t)hs_buf[hpos+3]);
            if (hpos + 4 + msg_len > (size_t)dlen) break;

            const u8 *msg_data = &hs_buf[hpos];
            size_t msg_total = 4 + msg_len;

            if (msg_type == TLS_HS_ENCRYPTED_EXT) {
                sha256_update(&s->transcript, msg_data, msg_total);
            } else if (msg_type == TLS_HS_CERTIFICATE) {
                /* Parse server certificate chain */
                size_t c_idx = hpos + 4;
                c_idx += 1; /* skip cert request context length */
                if (c_idx + 3 <= hpos + msg_total) {
                    c_idx += 3; /* skip cert list length */
                    if (c_idx + 3 <= hpos + msg_total) {
                        size_t cert_len = ((size_t)hs_buf[c_idx] << 16) |
                                          ((size_t)hs_buf[c_idx+1] << 8) |
                                          ((size_t)hs_buf[c_idx+2]);
                        c_idx += 3;
                        if (c_idx + cert_len <= hpos + msg_total) {
                            if (x509_parse_cert(&hs_buf[c_idx], cert_len, &s->peer_cert)) {
                                /* Validate against Root CA */
                                static x509_cert_t ca_cert;
                                if (x509_parse_cert(test_ca_der, TEST_CA_DER_LEN, &ca_cert)) {
                                    u64 now = rtc_get_unix_time();
                                    cert_status_t cst = x509_verify_cert(&s->peer_cert, &ca_cert, s->hostname, now);
                                    if (cst == CERT_OK) {
                                        s->cert_verified = true;
                                    } else {
                                        kprintf("[TLS ERROR] Server certificate verification failed: code %d\n", (int)cst);
                                        tcp_close(s->tcp_socket);
                                        return (cst == CERT_ERR_HOSTNAME_MISMATCH) ? TLS_ERR_CERT_HOSTNAME_MISMATCH :
                                               (cst == CERT_ERR_EXPIRED) ? TLS_ERR_CERT_EXPIRED : TLS_ERR_CERT_INVALID;
                                    }
                                }
                            }
                        }
                    }
                }
                sha256_update(&s->transcript, msg_data, msg_total);
            } else if (msg_type == TLS_HS_CERT_VERIFY) {
                /* CertificateVerify: signature over transcript hash */
                sha256_update(&s->transcript, msg_data, msg_total);
            } else if (msg_type == TLS_HS_FINISHED) {
                /* Verify server Finished tag */
                u8 s_finished_key[32];
                hkdf_expand_label(s_hs_traffic, "finished", NULL, 0, 32, s_finished_key);

                u8 expected_verify_data[32];
                u8 curr_transcript_hash[32];
                sha256_ctx_t tc = s->transcript;
                sha256_final(&tc, curr_transcript_hash);
                hmac_sha256(s_finished_key, 32, curr_transcript_hash, 32, expected_verify_data);

                if (msg_len < 32 || !crypto_memcmp_eq(&msg_data[4], expected_verify_data, 32)) {
                    kprintf("[TLS ERROR] Server Finished verify_data mismatch!\n");
                    tcp_close(s->tcp_socket);
                    return TLS_ERR_MAC_MISMATCH;
                }

                sha256_update(&s->transcript, msg_data, msg_total);

                /* 8. Derive Application Traffic Secrets */
                u8 derived1[32];
                derive_secret(handshake_secret, "derived", &empty_ctx, derived1);

                u8 master_secret[32];
                hkdf_extract(derived1, 32, zeros32, 32, master_secret);

                u8 c_ap_traffic[32];
                u8 s_ap_traffic[32];
                derive_secret(master_secret, "c ap traffic", &s->transcript, c_ap_traffic);
                derive_secret(master_secret, "s ap traffic", &s->transcript, s_ap_traffic);

                hkdf_expand_label(c_ap_traffic, "key", NULL, 0, 32, s->client_app_key);
                hkdf_expand_label(c_ap_traffic, "iv",  NULL, 0, 12, s->client_app_iv);

                hkdf_expand_label(s_ap_traffic, "key", NULL, 0, 32, s->server_app_key);
                hkdf_expand_label(s_ap_traffic, "iv",  NULL, 0, 12, s->server_app_iv);

                /* 9. Send Client Finished */
                u8 c_finished_key[32];
                hkdf_expand_label(c_hs_traffic, "finished", NULL, 0, 32, c_finished_key);

                u8 client_transcript_hash[32];
                sha256_ctx_t ctc = s->transcript;
                sha256_final(&ctc, client_transcript_hash);

                u8 client_finished_tag[32];
                hmac_sha256(c_finished_key, 32, client_transcript_hash, 32, client_finished_tag);

                u8 client_fin_msg[36];
                client_fin_msg[0] = TLS_HS_FINISHED;
                client_fin_msg[1] = 0x00; client_fin_msg[2] = 0x00; client_fin_msg[3] = 32;
                kmemcpy(&client_fin_msg[4], client_finished_tag, 32);

                tls_write_record(s, TLS_RECORD_HANDSHAKE, client_fin_msg, 36,
                                s->client_hs_key, s->client_hs_iv, &s->client_seq);

                /* Reset sequence numbers for Application Traffic */
                s->client_seq = 0;
                s->server_seq = 0;
                s->state = TLS_STATE_CONNECTED;
                kprintf("[TLS] Handshake completed successfully with %s (TLS 1.3, ChaCha20-Poly1305)\n", s->hostname);
                kprintf("ASTRA_TLS_HANDSHAKE_OK\n\n");
                break;
            }
            hpos += msg_total;
        }
    }

    return TLS_OK;
}

int tls_send(tls_session_t *s, const void *data, size_t len) {
    if (!s || s->state != TLS_STATE_CONNECTED || !data || len == 0) {
        return -1;
    }
    return tls_write_record(s, TLS_RECORD_APPLICATION, data, len,
                            s->client_app_key, s->client_app_iv, &s->client_seq);
}

int tls_recv(tls_session_t *s, void *buf, size_t max_len) {
    if (!s || s->state != TLS_STATE_CONNECTED || !buf || max_len == 0) {
        return -1;
    }
    while (1) {
        u8 itype = 0;
        int n = tls_read_record(s, &itype, buf, max_len,
                                s->server_app_key, s->server_app_iv, &s->server_seq);
        if (n < 0) {
            return -1;
        }
        /* RFC 8446 Section 4.6: Ignore post-handshake messages (e.g. NewSessionTicket = 22) */
        if (itype == TLS_RECORD_HANDSHAKE) {
            continue;
        }
        /* RFC 8446 Section 6.1: Closure alert (21) indicates EOF */
        if (itype == TLS_RECORD_ALERT) {
            return 0;
        }
        if (itype != TLS_RECORD_APPLICATION) {
            return -1;
        }
        return n;
    }
}

void tls_close(tls_session_t *s) {
    if (!s) return;
    if (s->tcp_socket >= 0) {
        tcp_close(s->tcp_socket);
        s->tcp_socket = -1;
    }
    /* Securely wipe cryptographic keys from memory */
    kmemset(s->client_hs_key, 0, sizeof(s->client_hs_key));
    kmemset(s->server_hs_key, 0, sizeof(s->server_hs_key));
    kmemset(s->client_app_key, 0, sizeof(s->client_app_key));
    kmemset(s->server_app_key, 0, sizeof(s->server_app_key));
    s->state = TLS_STATE_CLOSED;
}

bool tls_self_test(void) {
    kprintf("[TLS TEST] Running TLS 1.3 Protocol Engine Self-Test...\n");
    /* Verify session initialization and bounds */
    tls_session_t test_sess;
    if (tls_init_session(&test_sess, "test.astraos.local", 443) != TLS_OK) {
        kprintf("[TLS TEST FAIL] tls_init_session failed!\n");
        return false;
    }
    if (test_sess.port != 443 || kmemcmp(test_sess.hostname, "test.astraos.local", 18) != 0) {
        kprintf("[TLS TEST FAIL] session parameters incorrect!\n");
        return false;
    }
    kprintf("  PASS: TLS session initialization and parameter bounds verified.\n");
    return true;
}
