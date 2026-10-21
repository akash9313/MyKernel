#ifndef ASTRA_KERNEL_TLS_H
#define ASTRA_KERNEL_TLS_H

#include <kernel/types.h>
#include <kernel/crypto.h>

#define TLS_RECORD_MAX_LEN      16384
#define TLS_RECORD_HEADER_LEN   5
#define TLS_MAX_SERVER_NAME_LEN 64

typedef enum {
    TLS_STATE_CLOSED = 0,
    TLS_STATE_CLIENT_HELLO_SENT,
    TLS_STATE_SERVER_HELLO_RECEIVED,
    TLS_STATE_HANDSHAKE_KEYS_ESTABLISHED,
    TLS_STATE_AUTHENTICATED,
    TLS_STATE_CONNECTED,
    TLS_STATE_ERROR
} tls_state_t;

typedef enum {
    TLS_OK = 0,
    TLS_ERR_TCP_CONNECT,
    TLS_ERR_ENTROPY_FAILURE,
    TLS_ERR_HANDSHAKE_FAILED,
    TLS_ERR_RECORD_OVERFLOW,
    TLS_ERR_UNSUPPORTED_VERSION,
    TLS_ERR_UNSUPPORTED_CIPHER,
    TLS_ERR_CERT_INVALID,
    TLS_ERR_CERT_EXPIRED,
    TLS_ERR_CERT_HOSTNAME_MISMATCH,
    TLS_ERR_CERT_UNTRUSTED,
    TLS_ERR_DECRYPT_FAILED,
    TLS_ERR_MAC_MISMATCH,
    TLS_ERR_CLOSED
} tls_error_t;

typedef struct {
    int tcp_socket;
    tls_state_t state;
    tls_error_t last_error;
    char hostname[TLS_MAX_SERVER_NAME_LEN];
    u16 port;

    /* Handshake Keys & IVs */
    u8 client_hs_key[32];
    u8 client_hs_iv[12];
    u8 server_hs_key[32];
    u8 server_hs_iv[12];

    /* Application Traffic Keys & IVs */
    u8 client_app_key[32];
    u8 client_app_iv[12];
    u8 server_app_key[32];
    u8 server_app_iv[12];

    /* Sequence numbers for record AEAD nonces */
    u64 client_seq;
    u64 server_seq;

    /* Running transcript hash */
    sha256_ctx_t transcript;

    /* Server certificate validation info */
    x509_cert_t peer_cert;
    bool cert_verified;
} tls_session_t;

/* TLS Client API */
tls_error_t tls_init_session(tls_session_t *session, const char *hostname, u16 port);
tls_error_t tls_connect(tls_session_t *session, const char *hostname, u16 port);
int         tls_send(tls_session_t *session, const void *data, size_t len);
int         tls_recv(tls_session_t *session, void *buf, size_t max_len);
void        tls_close(tls_session_t *session);

/* Self-test and integration helpers */
bool tls_self_test(void);

#endif /* ASTRA_KERNEL_TLS_H */
