#include <kernel/net.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>

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
    const char *p3 = " HTTP/1.0\r\nHost: ";
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

    /* Parse Status Code (e.g. "HTTP/1.1 200 OK") */
    int status = 0;
    if (total_read > 12 && response_buf[0] == 'H' && response_buf[1] == 'T' && response_buf[2] == 'T' && response_buf[3] == 'P') {
        const char *s = response_buf;
        while (*s && *s != ' ') s++;
        if (*s == ' ') {
            s++;
            if (*s >= '0' && *s <= '9') {
                status = (*s - '0') * 100 + (*(s+1) - '0') * 10 + (*(s+2) - '0');
            }
        }
    }

    if (out_status_code) {
        *out_status_code = status;
    }

    kprintf("[HTTP] GET %s -> Status: %d (%u bytes received)\n", path, status, (unsigned int)total_read);
    return (int)total_read;
}
