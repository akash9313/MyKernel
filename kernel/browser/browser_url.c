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

static void k_strcpy_safe(char *dst, const char *src, size_t max) {
    if (!dst || max == 0) return;
    size_t i = 0;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void k_strcat_safe(char *dst, const char *src, size_t max) {
    if (!dst || max == 0) return;
    size_t dlen = k_strlen(dst);
    if (dlen >= max - 1) return;
    size_t i = 0;
    while (src && src[i] && (dlen + i) < max - 1) {
        dst[dlen + i] = src[i];
        i++;
    }
    dst[dlen + i] = '\0';
}

static void k_normalize_path(const char *in_path, char *out_path, size_t max_out) {
    if (!in_path || !out_path || max_out == 0) return;
    out_path[0] = '\0';
    if (max_out < 2) return;

    /* Segment-based normalization handling . and .. */
    char segs[16][64];
    int seg_count = 0;

    const char *p = in_path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char seg[64];
        size_t si = 0;
        while (*p && *p != '/' && si < sizeof(seg) - 1) {
            seg[si++] = *p++;
        }
        seg[si] = '\0';

        if (seg[0] == '.' && seg[1] == '\0') {
            /* Current directory: ignore */
            continue;
        } else if (seg[0] == '.' && seg[1] == '.' && seg[2] == '\0') {
            /* Parent directory */
            if (seg_count > 0) seg_count--;
        } else if (seg_count < 16) {
            k_strcpy_safe(segs[seg_count], seg, sizeof(segs[seg_count]));
            seg_count++;
        }
    }

    out_path[0] = '/';
    out_path[1] = '\0';
    for (int i = 0; i < seg_count; i++) {
        k_strcat_safe(out_path, segs[i], max_out);
        if (i < seg_count - 1) {
            k_strcat_safe(out_path, "/", max_out);
        }
    }
}

void browser_url_resolve(const char *base_url, const char *rel, char *out, size_t out_max) {
    if (!out || out_max == 0) return;
    out[0] = '\0';
    if (!rel || !rel[0]) {
        if (base_url) k_strcpy_safe(out, base_url, out_max);
        return;
    }

    /* 1. Fragment-only link: e.g. "#section" */
    if (rel[0] == '#') {
        if (base_url && base_url[0]) {
            k_strcpy_safe(out, base_url, out_max);
            for (int i = 0; out[i]; i++) {
                if (out[i] == '#') {
                    out[i] = '\0';
                    break;
                }
            }
            k_strcat_safe(out, rel, out_max);
        } else {
            k_strcpy_safe(out, rel, out_max);
        }
        return;
    }

    /* 2. Absolute URL (scheme present) */
    if (k_strncmp(rel, "http://", 7) == 0 ||
        k_strncmp(rel, "https://", 8) == 0 ||
        k_strncmp(rel, "file://", 7) == 0) {
        k_strcpy_safe(out, rel, out_max);
        return;
    }

    if (!base_url || !base_url[0]) {
        k_strcpy_safe(out, rel, out_max);
        return;
    }

    /* Separate fragment from rel if any */
    char rel_clean[256];
    char rel_frag[64];
    rel_clean[0] = '\0';
    rel_frag[0] = '\0';
    k_strcpy_safe(rel_clean, rel, sizeof(rel_clean));
    for (int i = 0; rel_clean[i]; i++) {
        if (rel_clean[i] == '#') {
            rel_clean[i] = '\0';
            k_strcpy_safe(rel_frag, &rel[i], sizeof(rel_frag));
            break;
        }
    }

    if (k_strncmp(base_url, "file://", 7) == 0) {
        char base_path[160];
        const char *bp = base_url + 7;
        while (*bp == '/') bp++; /* skip redundant slashes */
        base_path[0] = '/';
        k_strcpy_safe(base_path + 1, bp, sizeof(base_path) - 1);

        /* Strip base fragment */
        for (int i = 0; base_path[i]; i++) {
            if (base_path[i] == '#') { base_path[i] = '\0'; break; }
        }

        char combined[256];
        if (rel_clean[0] == '/') {
            k_strcpy_safe(combined, rel_clean, sizeof(combined));
        } else {
            int last_slash = -1;
            for (int i = 0; base_path[i]; i++) {
                if (base_path[i] == '/') last_slash = i;
            }
            if (last_slash >= 0) {
                base_path[last_slash + 1] = '\0';
                k_strcpy_safe(combined, base_path, sizeof(combined));
            } else {
                k_strcpy_safe(combined, "/", sizeof(combined));
            }
            k_strcat_safe(combined, rel_clean, sizeof(combined));
        }

        char norm[160];
        k_normalize_path(combined, norm, sizeof(norm));
        k_strcpy_safe(out, "file://", out_max);
        k_strcat_safe(out, norm, out_max);
        if (rel_frag[0]) k_strcat_safe(out, rel_frag, out_max);
        return;
    }

    if (k_strncmp(base_url, "http://", 7) == 0 || k_strncmp(base_url, "https://", 8) == 0) {
        browser_url_t bp;
        if (browser_url_parse(base_url, &bp)) {
            const char *scheme_str = (bp.scheme == URL_SCHEME_HTTPS) ? "https://" : "http://";
            char combined[256];
            if (rel_clean[0] == '/') {
                k_strcpy_safe(combined, rel_clean, sizeof(combined));
            } else {
                char base_dir[160];
                k_strcpy_safe(base_dir, bp.path, sizeof(base_dir));
                int last_slash = -1;
                for (int i = 0; base_dir[i]; i++) {
                    if (base_dir[i] == '/') last_slash = i;
                }
                if (last_slash >= 0) {
                    base_dir[last_slash + 1] = '\0';
                    k_strcpy_safe(combined, base_dir, sizeof(combined));
                } else {
                    k_strcpy_safe(combined, "/", sizeof(combined));
                }
                k_strcat_safe(combined, rel_clean, sizeof(combined));
            }

            char norm[160];
            k_normalize_path(combined, norm, sizeof(norm));

            k_strcpy_safe(out, scheme_str, out_max);
            k_strcat_safe(out, bp.host, out_max);
            if ((bp.scheme == URL_SCHEME_HTTP && bp.port != 80) ||
                (bp.scheme == URL_SCHEME_HTTPS && bp.port != 443)) {
                char pbuf[16];
                char ptmp[16];
                int pi = 0;
                u32 pv = bp.port;
                while (pv > 0) {
                    ptmp[pi++] = '0' + (pv % 10);
                    pv /= 10;
                }
                pbuf[0] = ':';
                int pj = 1;
                for (int k = pi - 1; k >= 0; k--) pbuf[pj++] = ptmp[k];
                pbuf[pj] = '\0';
                k_strcat_safe(out, pbuf, out_max);
            }
            k_strcat_safe(out, norm, out_max);
            if (rel_frag[0]) k_strcat_safe(out, rel_frag, out_max);
            return;
        }
    }

    k_strcpy_safe(out, rel, out_max);
}

static int process_http_response(const char *raw_resp, size_t total_recv,
                                 char *out_buf, size_t max_len,
                                 int *out_status, char *out_location, size_t loc_max) {
    if (!raw_resp || total_recv == 0 || !out_buf || max_len == 0) {
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

    http_response_t resp;
    int perr = http_parse_response(raw_resp, total_recv, &resp);

    if (out_location && loc_max > 0) {
        size_t lcopy = k_strlen(resp.location);
        if (lcopy >= loc_max) lcopy = loc_max - 1;
        kmemcpy(out_location, resp.location, lcopy);
        out_location[lcopy] = '\0';
    }

    if (perr != 0) {
        int status = resp.status_code ? resp.status_code : 502;
        if (out_status) *out_status = status;
        const char *err_msg = resp.has_duplicate_cl ?
            "Duplicate or conflicting Content-Length headers detected (framing security violation)." :
            "Malformed HTTP status line or headers could not be parsed.";

        char err_page[512];
        int elen = 0;
        const char *p1 = "<html><head><title>Bad Gateway</title></head><body><h1>502 Bad Gateway</h1><p>";
        while (*p1) err_page[elen++] = *p1++;
        const char *p2 = err_msg;
        while (*p2) err_page[elen++] = *p2++;
        const char *p3 = "</p><hr><p><a href=\"file:///www/index.html\">Return to Home</a></p></body></html>";
        while (*p3) err_page[elen++] = *p3++;
        err_page[elen] = '\0';

        size_t mlen = (size_t)elen;
        if (mlen >= max_len) mlen = max_len - 1;
        kmemcpy(out_buf, err_page, mlen);
        out_buf[mlen] = '\0';
        return (int)mlen;
    }

    if (resp.is_truncated) {
        if (out_status) *out_status = 502;
        const char *trunc_page = "<html><head><title>502 Truncated Body</title></head>"
                                 "<body><h1>502 Bad Gateway - Truncated Response</h1>"
                                 "<p>The connection closed before Content-Length bytes were received.</p></body></html>";
        size_t mlen = k_strlen(trunc_page);
        if (mlen >= max_len) mlen = max_len - 1;
        kmemcpy(out_buf, trunc_page, mlen);
        out_buf[mlen] = '\0';
        return (int)mlen;
    }

    if (out_status) *out_status = resp.status_code;

    /* Handle Chunked Transfer-Encoding */
    if (resp.is_chunked) {
        static char dechunk_buf[BROWSER_HTML_BUFFER_MAX];
        int dlen = http_decode_chunked(resp.body, resp.body_len, dechunk_buf, sizeof(dechunk_buf));
        if (dlen < 0) {
            if (out_status) *out_status = 502;
            const char *c_err = "<html><head><title>502 Malformed Chunked</title></head>"
                                "<body><h1>502 Bad Gateway</h1>"
                                "<p>Malformed or truncated chunked transfer encoding.</p></body></html>";
            size_t mlen = k_strlen(c_err);
            if (mlen >= max_len) mlen = max_len - 1;
            kmemcpy(out_buf, c_err, mlen);
            out_buf[mlen] = '\0';
            return (int)mlen;
        }

        size_t mlen = (size_t)dlen;
        if (mlen >= max_len) mlen = max_len - 1;
        kmemcpy(out_buf, dechunk_buf, mlen);
        out_buf[mlen] = '\0';
        return (int)mlen;
    }

    /* Normal body */
    size_t body_len = resp.body_len;
    if (body_len >= max_len) body_len = max_len - 1;
    kmemcpy(out_buf, resp.body, body_len);
    out_buf[body_len] = '\0';
    return (int)body_len;
}

/* Shared response buffer for HTTP/HTTPS download */
static char s_browser_raw_resp[BROWSER_HTML_BUFFER_MAX];

int browser_fetch_url(const browser_url_t *url, char *out_buf, size_t max_len, int *out_status) {
    if (!url || !out_buf || max_len < 32) {
        if (out_status) *out_status = 400;
        return -1;
    }

    browser_url_t cur_url = *url;
    int redirects = 0;

    while (redirects <= 5) {
        /* 1. Handle HTTPS (https://) */
        if (cur_url.scheme == URL_SCHEME_HTTPS) {
            static tls_session_t session;
            tls_error_t terr = tls_connect(&session, cur_url.host, cur_url.port);
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
            const char *r2 = cur_url.path;
            while (*r2) req[req_len++] = *r2++;
            const char *r3 = " HTTP/1.1\r\nHost: ";
            while (*r3) req[req_len++] = *r3++;
            const char *r4 = cur_url.host;
            while (*r4) req[req_len++] = *r4++;
            const char *r5 = "\r\nUser-Agent: AstraBrowser/1.0 (AstraOS x86-64; TLS 1.3)\r\nConnection: close\r\nAccept: text/html, */*\r\n\r\n";
            while (*r5) req[req_len++] = *r5++;
            req[req_len] = '\0';

            if (tls_send(&session, req, (size_t)req_len) < 0) {
                tls_close(&session);
                if (out_status) *out_status = 502;
                return -1;
            }

            /* Read HTTP response into shared buffer */
            size_t total_recv = 0;
            while (total_recv < sizeof(s_browser_raw_resp) - 1) {
                int r = tls_recv(&session, s_browser_raw_resp + total_recv, sizeof(s_browser_raw_resp) - 1 - total_recv);
                if (r <= 0) break;
                total_recv += (size_t)r;
            }
            s_browser_raw_resp[total_recv] = '\0';
            tls_close(&session);

            int status = 0;
            char location[256] = {0};
            int n = process_http_response(s_browser_raw_resp, total_recv, out_buf, max_len, &status, location, sizeof(location));
            if (out_status) *out_status = status;

            /* Check for HTTP Redirect (301, 302, 303, 307, 308) */
            if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                if (location[0] != '\0') {
                    redirects++;
                    if (redirects > 5) {
                        if (out_status) *out_status = 508;
                        const char *lmsg = "<html><body><h1>Redirect Loop Detected</h1><p>Too many redirects.</p></body></html>";
                        size_t lmlen = k_strlen(lmsg);
                        if (lmlen >= max_len) lmlen = max_len - 1;
                        kmemcpy(out_buf, lmsg, lmlen);
                        out_buf[lmlen] = '\0';
                        return (int)lmlen;
                    }
                    browser_url_t next_url;
                    char next_url_str[BROWSER_URL_MAX];
                    browser_url_resolve(cur_url.raw, location, next_url_str, sizeof(next_url_str));
                    if (!browser_url_parse(next_url_str, &next_url)) {
                        if (out_status) *out_status = 502;
                        const char *bmsg = "<html><body><h1>Invalid Redirect Target</h1></body></html>";
                        size_t bmlen = k_strlen(bmsg);
                        if (bmlen >= max_len) bmlen = max_len - 1;
                        kmemcpy(out_buf, bmsg, bmlen);
                        out_buf[bmlen] = '\0';
                        return (int)bmlen;
                    }
                    if (k_strncmp(next_url.path, cur_url.path, sizeof(cur_url.path)) == 0 &&
                        k_strncmp(next_url.host, cur_url.host, sizeof(cur_url.host)) == 0 &&
                        next_url.port == cur_url.port && next_url.scheme == cur_url.scheme) {
                        if (out_status) *out_status = 508;
                        const char *lmsg = "<html><body><h1>Redirect Loop Detected</h1><p>Self-referential redirect loop.</p></body></html>";
                        size_t lmlen = k_strlen(lmsg);
                        if (lmlen >= max_len) lmlen = max_len - 1;
                        kmemcpy(out_buf, lmsg, lmlen);
                        out_buf[lmlen] = '\0';
                        return (int)lmlen;
                    }
                    cur_url = next_url;
                    continue;
                }
            }
            return n;
        }

        /* 2. Handle Unknown Scheme */
        if (cur_url.scheme == URL_SCHEME_UNKNOWN) {
            if (out_status) *out_status = 400;
            const char *msg = "<html><head><title>AstraBrowser - Invalid URL</title></head>"
                              "<body><h1>Error: Unsupported URL Scheme</h1>"
                              "<p>Supported schemes are <b>http://</b>, <b>https://</b>, and <b>file://</b>.</p></body></html>";
            size_t mlen = k_strlen(msg);
            if (mlen >= max_len) mlen = max_len - 1;
            kmemcpy(out_buf, msg, mlen);
            out_buf[mlen] = '\0';
            return (int)mlen;
        }

        /* 3. Handle Local VFS (file://) */
        if (cur_url.scheme == URL_SCHEME_FILE) {
            int fd = vfs_open(cur_url.path, 0);
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
        if (cur_url.scheme == URL_SCHEME_HTTP) {
            ip_addr_t dest_ip = 0;
            if (!dns_resolve(cur_url.host, &dest_ip, 2000)) {
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

            if (tcp_connect(sock, dest_ip, cur_url.port, 1000) != 0) {
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
            const char *r2 = cur_url.path;
            while (*r2) req[req_len++] = *r2++;
            const char *r3 = " HTTP/1.1\r\nHost: ";
            while (*r3) req[req_len++] = *r3++;
            const char *r4 = cur_url.host;
            while (*r4) req[req_len++] = *r4++;
            const char *r5 = "\r\nUser-Agent: AstraBrowser/1.0 (AstraOS x86-64)\r\nConnection: close\r\nAccept: text/html, */*\r\n\r\n";
            while (*r5) req[req_len++] = *r5++;
            req[req_len] = '\0';

            tcp_send(sock, req, (size_t)req_len);

            /* Read HTTP response into shared buffer */
            size_t total_recv = 0;
            while (total_recv < sizeof(s_browser_raw_resp) - 1) {
                int r = tcp_recv(sock, s_browser_raw_resp + total_recv, sizeof(s_browser_raw_resp) - 1 - total_recv, 2500);
                if (r <= 0) break;
                total_recv += (size_t)r;
            }
            s_browser_raw_resp[total_recv] = '\0';
            tcp_close(sock);

            int status = 0;
            char location[256] = {0};
            int n = process_http_response(s_browser_raw_resp, total_recv, out_buf, max_len, &status, location, sizeof(location));
            if (out_status) *out_status = status;

            /* Check for HTTP Redirect (301, 302, 303, 307, 308) */
            if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                if (location[0] != '\0') {
                    redirects++;
                    if (redirects > 5) {
                        if (out_status) *out_status = 508;
                        const char *lmsg = "<html><body><h1>Redirect Loop Detected</h1><p>Too many redirects.</p></body></html>";
                        size_t lmlen = k_strlen(lmsg);
                        if (lmlen >= max_len) lmlen = max_len - 1;
                        kmemcpy(out_buf, lmsg, lmlen);
                        out_buf[lmlen] = '\0';
                        return (int)lmlen;
                    }
                    browser_url_t next_url;
                    char next_url_str[BROWSER_URL_MAX];
                    browser_url_resolve(cur_url.raw, location, next_url_str, sizeof(next_url_str));
                    if (!browser_url_parse(next_url_str, &next_url)) {
                        if (out_status) *out_status = 502;
                        const char *bmsg = "<html><body><h1>Invalid Redirect Target</h1></body></html>";
                        size_t bmlen = k_strlen(bmsg);
                        if (bmlen >= max_len) bmlen = max_len - 1;
                        kmemcpy(out_buf, bmsg, bmlen);
                        out_buf[bmlen] = '\0';
                        return (int)bmlen;
                    }
                    if (k_strncmp(next_url.path, cur_url.path, sizeof(cur_url.path)) == 0 &&
                        k_strncmp(next_url.host, cur_url.host, sizeof(cur_url.host)) == 0 &&
                        next_url.port == cur_url.port && next_url.scheme == cur_url.scheme) {
                        if (out_status) *out_status = 508;
                        const char *lmsg = "<html><body><h1>Redirect Loop Detected</h1><p>Self-referential redirect loop.</p></body></html>";
                        size_t lmlen = k_strlen(lmsg);
                        if (lmlen >= max_len) lmlen = max_len - 1;
                        kmemcpy(out_buf, lmsg, lmlen);
                        out_buf[lmlen] = '\0';
                        return (int)lmlen;
                    }
                    cur_url = next_url;
                    continue;
                }
            }
            return n;
        }

        break;
    }

    return -1;
}

