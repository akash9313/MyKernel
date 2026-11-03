#include <kernel/net.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>

static inline char ascii_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static bool str_starts_with_ci(const char *str, const char *prefix) {
    if (!str || !prefix) return false;
    while (*prefix) {
        if (ascii_tolower(*str) != ascii_tolower(*prefix)) return false;
        str++;
        prefix++;
    }
    return true;
}

int http_parse_response(const char *raw, size_t raw_len, http_response_t *resp) {
    if (!raw || !resp || raw_len == 0) return -1;
    kmemset(resp, 0, sizeof(*resp));
    resp->content_length = -1;

    /* 1. Validate status line */
    if (raw_len < 12) {
        resp->status_code = 502;
        return -1;
    }

    if (!str_starts_with_ci(raw, "HTTP/")) {
        resp->status_code = 502;
        return -1;
    }

    size_t pos = 5;
    /* Skip version e.g. 1.0 or 1.1 */
    while (pos < raw_len && raw[pos] != ' ' && raw[pos] != '\t' && raw[pos] != '\r' && raw[pos] != '\n') {
        pos++;
    }
    while (pos < raw_len && (raw[pos] == ' ' || raw[pos] == '\t')) {
        pos++;
    }

    if (pos + 3 > raw_len) {
        resp->status_code = 502;
        return -1;
    }

    /* Status code must be 3 digits */
    if (raw[pos] < '0' || raw[pos] > '9' ||
        raw[pos+1] < '0' || raw[pos+1] > '9' ||
        raw[pos+2] < '0' || raw[pos+2] > '9') {
        resp->status_code = 502;
        return -1;
    }

    int code = (raw[pos] - '0') * 100 + (raw[pos+1] - '0') * 10 + (raw[pos+2] - '0');
    if (code < 100 || code > 599) {
        resp->status_code = 502;
        return -1;
    }

    /* Character following 3-digit status code must not be another digit */
    if (pos + 3 < raw_len && raw[pos+3] >= '0' && raw[pos+3] <= '9') {
        resp->status_code = 502;
        return -1;
    }
    resp->status_code = code;

    /* Skip rest of status line to CRLF or LF */
    while (pos < raw_len && raw[pos] != '\n') {
        pos++;
    }
    if (pos < raw_len && raw[pos] == '\n') {
        pos++;
    }
    size_t headers_start = pos;

    /* 2. Locate header boundary: \r\n\r\n or \n\n (bounded to 8192 bytes) */
    size_t max_scan = (raw_len < 8192) ? raw_len : 8192;
    size_t body_start = 0;
    size_t boundary_len = 0;

    for (size_t i = headers_start; i + 1 < max_scan; i++) {
        if (i + 3 < max_scan && raw[i] == '\r' && raw[i+1] == '\n' && raw[i+2] == '\r' && raw[i+3] == '\n') {
            body_start = i + 4;
            boundary_len = 4;
            break;
        } else if (raw[i] == '\n' && raw[i+1] == '\n') {
            body_start = i + 2;
            boundary_len = 2;
            break;
        }
    }

    if (body_start == 0) {
        /* No valid header terminator found */
        resp->status_code = 502;
        return -1;
    }

    resp->header_len = body_start;
    resp->body = raw + body_start;
    resp->body_len = raw_len - body_start;

    /* 3. Parse headers line by line */
    size_t cur = headers_start;
    while (cur < body_start - boundary_len) {
        /* Find end of line */
        size_t line_start = cur;
        while (cur < body_start - boundary_len && raw[cur] != '\r' && raw[cur] != '\n') {
            cur++;
        }
        size_t line_end = cur;
        while (cur < body_start - boundary_len && (raw[cur] == '\r' || raw[cur] == '\n')) {
            cur++;
        }

        /* Parse Header: Value */
        size_t colon = line_start;
        while (colon < line_end && raw[colon] != ':') {
            colon++;
        }
        if (colon >= line_end) continue; /* No colon separator */

        /* Header name */
        size_t hname_len = colon - line_start;
        const char *hname = raw + line_start;

        /* Value */
        size_t val_start = colon + 1;
        while (val_start < line_end && (raw[val_start] == ' ' || raw[val_start] == '\t')) {
            val_start++;
        }
        size_t val_end = line_end;
        while (val_end > val_start && (raw[val_end-1] == ' ' || raw[val_end-1] == '\t')) {
            val_end--;
        }
        size_t val_len = val_end - val_start;
        const char *val = raw + val_start;

        /* Check Content-Length */
        if (hname_len == 14 && str_starts_with_ci(hname, "content-length")) {
            if (val_len == 0) {
                resp->status_code = 502;
                return -1;
            }
            s64 parsed_cl = 0;
            for (size_t k = 0; k < val_len; k++) {
                if (val[k] < '0' || val[k] > '9') {
                    resp->status_code = 502;
                    return -1;
                }
                /* Integer overflow check */
                if (parsed_cl > (0x7FFFFFFFFFFFFFFFLL - (val[k] - '0')) / 10) {
                    resp->status_code = 502;
                    return -1;
                }
                parsed_cl = parsed_cl * 10 + (val[k] - '0');
            }

            if (resp->content_length >= 0) {
                /* Duplicate Content-Length */
                if (resp->content_length != parsed_cl) {
                    resp->has_duplicate_cl = true;
                    resp->status_code = 502;
                    return -1;
                }
            } else {
                resp->content_length = parsed_cl;
            }
        }
        /* Check Transfer-Encoding */
        else if (hname_len == 17 && str_starts_with_ci(hname, "transfer-encoding")) {
            for (size_t k = 0; k + 7 <= val_len; k++) {
                if (str_starts_with_ci(val + k, "chunked")) {
                    resp->is_chunked = true;
                    break;
                }
            }
        }
        /* Check Connection */
        else if (hname_len == 10 && str_starts_with_ci(hname, "connection")) {
            for (size_t k = 0; k + 5 <= val_len; k++) {
                if (str_starts_with_ci(val + k, "close")) {
                    resp->is_close = true;
                    break;
                }
            }
        }
        /* Check Location */
        else if (hname_len == 8 && str_starts_with_ci(hname, "location")) {
            size_t cplen = (val_len < sizeof(resp->location) - 1) ? val_len : (sizeof(resp->location) - 1);
            kmemcpy(resp->location, val, cplen);
            resp->location[cplen] = '\0';
        }
    }

    /* 4. Check for body truncation if Content-Length was provided and not chunked */
    if (!resp->is_chunked && resp->content_length >= 0) {
        if (resp->body_len < (size_t)resp->content_length) {
            resp->is_truncated = true;
        }
    }

    return 0;
}

int http_decode_chunked(const char *in, size_t in_len, char *out, size_t max_out) {
    if (!in || !out || max_out == 0) return -1;
    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < in_len) {
        /* Skip leading CRLF or LF if between chunks */
        while (in_pos < in_len && (in[in_pos] == '\r' || in[in_pos] == '\n')) {
            in_pos++;
        }
        if (in_pos >= in_len) break;

        /* Parse hex chunk size */
        size_t chunk_sz = 0;
        bool has_digits = false;
        while (in_pos < in_len) {
            char c = in[in_pos];
            if (c >= '0' && c <= '9') {
                if (chunk_sz > (SIZE_MAX - (size_t)(c - '0')) / 16) return -1;
                chunk_sz = chunk_sz * 16 + (size_t)(c - '0');
                has_digits = true;
            } else if (c >= 'a' && c <= 'f') {
                if (chunk_sz > (SIZE_MAX - (size_t)(c - 'a' + 10)) / 16) return -1;
                chunk_sz = chunk_sz * 16 + (size_t)(c - 'a' + 10);
                has_digits = true;
            } else if (c >= 'A' && c <= 'F') {
                if (chunk_sz > (SIZE_MAX - (size_t)(c - 'A' + 10)) / 16) return -1;
                chunk_sz = chunk_sz * 16 + (size_t)(c - 'A' + 10);
                has_digits = true;
            } else {
                break;
            }
            in_pos++;
        }

        if (!has_digits) {
            return -1; /* Malformed chunk size */
        }

        /* Skip chunk extension up to CRLF or LF */
        while (in_pos < in_len && in[in_pos] != '\n') {
            in_pos++;
        }
        if (in_pos < in_len && in[in_pos] == '\n') {
            in_pos++;
        }

        /* Check for terminal chunk (size 0) */
        if (chunk_sz == 0) {
            out[out_pos] = '\0';
            return (int)out_pos;
        }

        /* Check if chunk data is available in input */
        if (in_pos + chunk_sz > in_len) {
            return -1; /* Incomplete / truncated chunk data */
        }

        /* Copy chunk data to output buffer */
        size_t to_copy = chunk_sz;
        if (out_pos + to_copy >= max_out) {
            to_copy = max_out - 1 - out_pos;
        }
        if (to_copy > 0) {
            kmemcpy(out + out_pos, in + in_pos, to_copy);
            out_pos += to_copy;
        }
        in_pos += chunk_sz;

        /* Expect CRLF after chunk data */
        if (in_pos < in_len && in[in_pos] == '\r') in_pos++;
        if (in_pos < in_len && in[in_pos] == '\n') in_pos++;
    }

    out[out_pos] = '\0';
    return (int)out_pos;
}

int http_get(const char *host, const char *path, char *response_buf, size_t max_len, int *out_status_code) {
    if (!host || !response_buf || max_len == 0) return -1;
    if (!path) path = "/";

    ip_addr_t dest_ip = 0;
    if (!str_to_ip(host, &dest_ip)) {
        if (!dns_resolve(host, &dest_ip, 2000)) {
            kprintf("[HTTP ERROR] Failed to resolve host '%s'\n", host);
            return -2;
        }
    }

    int sock = tcp_socket_create();
    if (sock < 0) {
        kprintf("[HTTP ERROR] Failed to create TCP socket\n");
        return -3;
    }

    kprintf("[HTTP] Connecting to %s (Port 80)...\n", host);
    if (tcp_connect(sock, dest_ip, 80, 800) != 0) {
        kprintf("[HTTP ERROR] TCP connection to %s:80 failed\n", host);
        tcp_close(sock);
        return -4;
    }

    /* Build HTTP GET Request */
    char req[512];
    int req_len = 0;
    const char *p1 = "GET ";
    while (*p1) req[req_len++] = *p1++;
    const char *p2 = path;
    while (*p2) req[req_len++] = *p2++;
    const char *p3 = " HTTP/1.1\r\nHost: ";
    while (*p3) req[req_len++] = *p3++;
    const char *p4 = host;
    while (*p4) req[req_len++] = *p4++;
    const char *p5 = "\r\nUser-Agent: AstraOS-Desktop/1.0\r\nConnection: close\r\n\r\n";
    while (*p5) req[req_len++] = *p5++;
    req[req_len] = '\0';

    tcp_send(sock, req, (size_t)req_len);

    /* Read Response */
    size_t total_read = 0;
    while (total_read < max_len - 1) {
        int r = tcp_recv(sock, response_buf + total_read, max_len - 1 - total_read, 3000);
        if (r <= 0) break;
        total_read += (size_t)r;
    }
    response_buf[total_read] = '\0';

    tcp_close(sock);

    /* Parse HTTP response using hardened parser */
    http_response_t resp;
    int status = 0;
    if (http_parse_response(response_buf, total_read, &resp) == 0) {
        status = resp.status_code;
    } else {
        status = resp.status_code ? resp.status_code : 502;
    }

    if (out_status_code) {
        *out_status_code = status;
    }

    kprintf("[HTTP] GET %s -> Status: %d (%u bytes received)\n", path, status, (unsigned int)total_read);
    return (int)total_read;
}
