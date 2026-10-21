#include <kernel/browser.h>
#include <kernel/net.h>
#include <kernel/tls.h>
#include <kernel/vfs.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>

static int k_strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) return (int)((unsigned char)s1[i] - (unsigned char)s2[i]);
        if (s1[i] == '\0') break;
    }
    return 0;
}

static size_t k_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

bool browser_url_parse(const char *raw, browser_url_t *out) {
    if (!raw || !out) return false;
    kmemset(out, 0, sizeof(browser_url_t));

    size_t raw_len = k_strlen(raw);
    if (raw_len == 0 || raw_len >= BROWSER_URL_MAX) return false;
    for (size_t i = 0; i < raw_len; i++) out->raw[i] = raw[i];
    out->raw[raw_len] = '\0';

    const char *p = raw;

    /* Check Scheme */
    if (k_strncmp(p, "http://", 7) == 0) {
        out->scheme = URL_SCHEME_HTTP;
        p += 7;
    } else if (k_strncmp(p, "https://", 8) == 0) {
        out->scheme = URL_SCHEME_HTTPS;
        p += 8;
    } else if (k_strncmp(p, "file://", 7) == 0) {
        out->scheme = URL_SCHEME_FILE;
        p += 7;
        /* Host is localhost */
        out->host[0] = 'l'; out->host[1] = 'o'; out->host[2] = 'c';
        out->host[3] = 'a'; out->host[4] = 'l'; out->host[5] = 'h';
        out->host[6] = 'o'; out->host[7] = 's'; out->host[8] = 't';
        out->host[9] = '\0';
        out->port = 0;

        /* If path is file:///path, p starts with '/' */
        if (*p == '/') {
            size_t pi = 0;
            while (*p && pi < sizeof(out->path) - 1) {
                out->path[pi++] = *p++;
            }
            out->path[pi] = '\0';
        } else {
            /* file://path -> ensure leading slash */
            out->path[0] = '/';
            size_t pi = 1;
            while (*p && pi < sizeof(out->path) - 1) {
                out->path[pi++] = *p++;
            }
            out->path[pi] = '\0';
        }
        return true;
    } else if (p[0] == '/') {
        /* Direct local path */
        out->scheme = URL_SCHEME_FILE;
        size_t pi = 0;
        while (*p && pi < sizeof(out->path) - 1) {
            out->path[pi++] = *p++;
        }
        out->path[pi] = '\0';
        return true;
    } else {
        out->scheme = URL_SCHEME_UNKNOWN;
        return false;
    }

    /* Parse HTTP Host and Port */
    size_t hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < sizeof(out->host) - 1) {
        out->host[hi++] = *p++;
    }
    out->host[hi] = '\0';
    if (hi == 0) return false;

    out->port = (out->scheme == URL_SCHEME_HTTPS) ? 443 : 80;
    if (*p == ':') {
        p++;
        u32 prt = 0;
        while (*p >= '0' && *p <= '9') {
            prt = prt * 10 + (*p - '0');
            p++;
        }
        if (prt > 0 && prt <= 65535) {
            out->port = (u16)prt;
        }
    }

    /* Parse Path */
    if (*p == '/') {
        size_t pi = 0;
        while (*p && pi < sizeof(out->path) - 1) {
            out->path[pi++] = *p++;
        }
        out->path[pi] = '\0';
    } else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }

    return true;
}

int browser_fetch_url(const browser_url_t *url, char *out_buf, size_t max_len, int *out_status) {
    if (!url || !out_buf || max_len < 32) {
        if (out_status) *out_status = 400;
        return -1;
    }

    /* 1. Handle HTTPS (https://) */
    if (url->scheme == URL_SCHEME_HTTPS) {
        static tls_session_t session;
        tls_error_t terr = tls_connect(&session, url->host, url->port);
        if (terr != TLS_OK) {
            if (out_status) {
                if (terr == TLS_ERR_CERT_INVALID || terr == TLS_ERR_CERT_EXPIRED ||
                    terr == TLS_ERR_CERT_HOSTNAME_MISMATCH || terr == TLS_ERR_CERT_UNTRUSTED) {
                    *out_status = 495; /* SSL Certificate Error */
                } else {
                    *out_status = 525; /* SSL Handshake Failed */
                }
            }
            const char *err_reason = "TLS Connection Failed";
            if (terr == TLS_ERR_CERT_EXPIRED) err_reason = "Certificate Expired";
            else if (terr == TLS_ERR_CERT_HOSTNAME_MISMATCH) err_reason = "Certificate Hostname Mismatch";
            else if (terr == TLS_ERR_CERT_UNTRUSTED) err_reason = "Untrusted Certificate Authority";
            else if (terr == TLS_ERR_ENTROPY_FAILURE) err_reason = "Insecure Hardware Entropy (RDRAND Required)";
            else if (terr == TLS_ERR_TCP_CONNECT) err_reason = "TCP Connection Refused or Timed Out";

            char err_page[512];
            int elen = 0;
            const char *ep1 = "<html><head><title>AstraBrowser - Security Error</title></head><body><h1>Secure Connection Failed</h1><p>Reason: <b>";
            while (*ep1) err_page[elen++] = *ep1++;
            const char *ep2 = err_reason;
            while (*ep2) err_page[elen++] = *ep2++;
            const char *ep3 = "</b></p><hr><p><a href=\"file:///www/index.html\">Return to Home</a></p></body></html>";
            while (*ep3) err_page[elen++] = *ep3++;
            err_page[elen] = '\0';

            size_t mlen = (size_t)elen;
            if (mlen >= max_len) mlen = max_len - 1;
            kmemcpy(out_buf, err_page, mlen);
            out_buf[mlen] = '\0';
            return (int)mlen;
        }

        /* Build HTTP/1.1 GET Request */
        char req[512];
        int req_len = 0;
        const char *r1 = "GET ";
        while (*r1) req[req_len++] = *r1++;
        const char *r2 = url->path;
        while (*r2) req[req_len++] = *r2++;
        const char *r3 = " HTTP/1.1\r\nHost: ";
        while (*r3) req[req_len++] = *r3++;
        const char *r4 = url->host;
        while (*r4) req[req_len++] = *r4++;
        const char *r5 = "\r\nUser-Agent: AstraBrowser/1.0 (AstraOS x86-64; TLS 1.3)\r\nConnection: close\r\nAccept: text/html, */*\r\n\r\n";
        while (*r5) req[req_len++] = *r5++;
        req[req_len] = '\0';

        if (tls_send(&session, req, (size_t)req_len) < 0) {
            tls_close(&session);
            if (out_status) *out_status = 502;
            return -1;
        }

        /* Read HTTP response into static buffer */
        static char raw_resp[BROWSER_HTML_BUFFER_MAX];
        size_t total_recv = 0;
        while (total_recv < sizeof(raw_resp) - 1) {
            int r = tls_recv(&session, raw_resp + total_recv, sizeof(raw_resp) - 1 - total_recv);
            if (r <= 0) break;
            total_recv += (size_t)r;
        }
        raw_resp[total_recv] = '\0';
        tls_close(&session);

        if (total_recv == 0) {
            if (out_status) *out_status = 502;
            const char *msg = "<html><head><title>Empty HTTPS Response</title></head>"
                              "<body><h1>Empty Response</h1>"
                              "<p>The HTTPS server closed the connection without returning data.</p></body></html>";
            size_t mlen = k_strlen(msg);
            if (mlen >= max_len) mlen = max_len - 1;
            kmemcpy(out_buf, msg, mlen);
            out_buf[mlen] = '\0';
            return (int)mlen;
        }

        /* Parse HTTP Status Line */
        int status = 200;
        if (total_recv > 12 && raw_resp[0] == 'H' && raw_resp[1] == 'T' && raw_resp[2] == 'T' && raw_resp[3] == 'P') {
            const char *sp = raw_resp;
            while (*sp && *sp != ' ') sp++;
            if (*sp == ' ') {
                sp++;
                if (*sp >= '0' && *sp <= '9') {
                    status = (*sp - '0') * 100 + (*(sp+1) - '0') * 10 + (*(sp+2) - '0');
                }
            }
        }
        if (out_status) *out_status = status;

        /* Extract body after \r\n\r\n */
        char *body = NULL;
        for (size_t i = 0; i + 3 < total_recv; i++) {
            if (raw_resp[i] == '\r' && raw_resp[i+1] == '\n' &&
                raw_resp[i+2] == '\r' && raw_resp[i+3] == '\n') {
                body = &raw_resp[i+4];
                break;
            }
        }
        if (!body) body = raw_resp;

        size_t body_len = k_strlen(body);
        if (body_len >= max_len) body_len = max_len - 1;
        kmemcpy(out_buf, body, body_len);
        out_buf[body_len] = '\0';
        return (int)body_len;
    }

    /* 2. Handle Unknown Scheme */
    if (url->scheme == URL_SCHEME_UNKNOWN) {
        if (out_status) *out_status = 400;
        const char *msg = "<html><head><title>AstraBrowser - Invalid URL</title></head>"
                          "<body><h1>Error: Unsupported URL Scheme</h1>"
                          "<p>Supported schemes are <b>http://</b> and <b>file://</b>.</p></body></html>";
        size_t mlen = k_strlen(msg);
        if (mlen >= max_len) mlen = max_len - 1;
        kmemcpy(out_buf, msg, mlen);
        out_buf[mlen] = '\0';
        return (int)mlen;
    }

    /* 3. Handle Local VFS (file://) */
    if (url->scheme == URL_SCHEME_FILE) {
        int fd = vfs_open(url->path, 0);
        if (fd < 0) {
            if (out_status) *out_status = 404;
            const char *msg = "<html><head><title>404 File Not Found</title></head>"
                              "<body><h1>404 - File Not Found</h1>"
                              "<p>The requested VFS path does not exist on the AstraOS initramfs.</p>"
                              "<hr><p><a href=\"file:///www/index.html\">Return to AstraBrowser Home</a></p></body></html>";
            size_t mlen = k_strlen(msg);
            if (mlen >= max_len) mlen = max_len - 1;
            kmemcpy(out_buf, msg, mlen);
            out_buf[mlen] = '\0';
            return (int)mlen;
        }

        ssize_t nread = vfs_read(fd, out_buf, max_len - 1);
        vfs_close(fd);

        if (nread < 0) {
            if (out_status) *out_status = 500;
            out_buf[0] = '\0';
            return -1;
        }

        out_buf[nread] = '\0';
        if (out_status) *out_status = 200;
        return (int)nread;
    }

    /* 4. Handle HTTP (http://) */
    if (url->scheme == URL_SCHEME_HTTP) {
        ip_addr_t dest_ip = 0;
        if (!dns_resolve(url->host, &dest_ip, 2000)) {
            if (out_status) *out_status = 502;
            const char *msg = "<html><head><title>DNS Resolution Error</title></head>"
                              "<body><h1>DNS Resolution Error</h1>"
                              "<p>Could not resolve the specified hostname.</p></body></html>";
            size_t mlen = k_strlen(msg);
            if (mlen >= max_len) mlen = max_len - 1;
            kmemcpy(out_buf, msg, mlen);
            out_buf[mlen] = '\0';
            return (int)mlen;
        }

        int sock = tcp_socket_create();
        if (sock < 0) {
            if (out_status) *out_status = 500;
            return -1;
        }

        if (tcp_connect(sock, dest_ip, url->port, 1000) != 0) {
            tcp_close(sock);
            if (out_status) *out_status = 504;
            const char *msg = "<html><head><title>Connection Failed</title></head>"
                              "<body><h1>Connection Failed</h1>"
                              "<p>TCP connection to the remote host timed out or was refused.</p></body></html>";
            size_t mlen = k_strlen(msg);
            if (mlen >= max_len) mlen = max_len - 1;
            kmemcpy(out_buf, msg, mlen);
            out_buf[mlen] = '\0';
            return (int)mlen;
        }

        /* Build HTTP/1.1 GET Request */
        char req[512];
        int req_len = 0;
        const char *r1 = "GET ";
        while (*r1) req[req_len++] = *r1++;
        const char *r2 = url->path;
        while (*r2) req[req_len++] = *r2++;
        const char *r3 = " HTTP/1.1\r\nHost: ";
        while (*r3) req[req_len++] = *r3++;
        const char *r4 = url->host;
        while (*r4) req[req_len++] = *r4++;
        const char *r5 = "\r\nUser-Agent: AstraBrowser/1.0 (AstraOS x86-64)\r\nConnection: close\r\nAccept: text/html, */*\r\n\r\n";
        while (*r5) req[req_len++] = *r5++;
        req[req_len] = '\0';

        tcp_send(sock, req, (size_t)req_len);

        /* Read HTTP response into temporary buffer */
        static char raw_resp[BROWSER_HTML_BUFFER_MAX];
        size_t total_recv = 0;
        while (total_recv < sizeof(raw_resp) - 1) {
            int r = tcp_recv(sock, raw_resp + total_recv, sizeof(raw_resp) - 1 - total_recv, 2500);
            if (r <= 0) break;
            total_recv += (size_t)r;
        }
        raw_resp[total_recv] = '\0';
        tcp_close(sock);

        if (total_recv == 0) {
            if (out_status) *out_status = 502;
            const char *msg = "<html><head><title>Empty Response</title></head>"
                              "<body><h1>Empty Response</h1>"
                              "<p>The server closed the connection without returning data.</p></body></html>";
            size_t mlen = k_strlen(msg);
            if (mlen >= max_len) mlen = max_len - 1;
            kmemcpy(out_buf, msg, mlen);
            out_buf[mlen] = '\0';
            return (int)mlen;
        }

        /* Parse HTTP Status Line */
        int status = 200;
        if (total_recv > 12 && raw_resp[0] == 'H' && raw_resp[1] == 'T' && raw_resp[2] == 'T' && raw_resp[3] == 'P') {
            const char *sp = raw_resp;
            while (*sp && *sp != ' ') sp++;
            if (*sp == ' ') {
                sp++;
                if (*sp >= '0' && *sp <= '9') {
                    status = (*sp - '0') * 100 + (*(sp+1) - '0') * 10 + (*(sp+2) - '0');
                }
            }
        }
        if (out_status) *out_status = status;

        /* Extract body after \r\n\r\n */
        char *body = NULL;
        for (size_t i = 0; i + 3 < total_recv; i++) {
            if (raw_resp[i] == '\r' && raw_resp[i+1] == '\n' &&
                raw_resp[i+2] == '\r' && raw_resp[i+3] == '\n') {
                body = &raw_resp[i+4];
                break;
            }
        }

        if (!body) {
            /* No header boundary found, treat entire buffer as content */
            body = raw_resp;
        }

        size_t body_len = k_strlen(body);
        if (body_len >= max_len) body_len = max_len - 1;
        kmemcpy(out_buf, body, body_len);
        out_buf[body_len] = '\0';
        return (int)body_len;
    }

    return -1;
}
