#include <kernel/net.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/arch/x86_64/cpu.h>

typedef struct dns_header {
    u16 id;
    u16 flags;
    u16 qdcount;
    u16 ancount;
    u16 nscount;
    u16 arcount;
} __attribute__((packed)) dns_header_t;

static volatile bool g_dns_resolved = false;
static ip_addr_t g_dns_result_ip = 0;
static u16 g_dns_query_id = 0x51A2;
static u16 g_dns_client_port = 45678;

static bool k_strcmp_match(const char *s1, const char *s2) {
    if (!s1 || !s2) return false;
    while (*s1 && *s2) {
        if (*s1 != *s2) return false;
        s1++; s2++;
    }
    return (*s1 == '\0' && *s2 == '\0');
}

static const u8 *dns_skip_name(const u8 *ptr, const u8 *end, const u8 *base) {
    (void)base;
    while (ptr < end) {
        u8 len = *ptr;
        if (len == 0) {
            return ptr + 1;
        }
        if ((len & 0xC0) == 0xC0) {
            /* Compression pointer: 2 bytes total */
            return ptr + 2;
        }
        ptr += 1 + len;
    }
    return NULL;
}

static void dns_on_packet(ip_addr_t src_ip, u16 src_port, const void *payload, size_t len) {
    (void)src_ip;
    (void)src_port;

    if (!payload || len < sizeof(dns_header_t)) return;

    const dns_header_t *hdr = (const dns_header_t *)payload;
    if (ntohs(hdr->id) != g_dns_query_id) return;

    u16 ancount = ntohs(hdr->ancount);
    u16 qdcount = ntohs(hdr->qdcount);
    if (ancount == 0) return;

    const u8 *base = (const u8 *)payload;
    const u8 *end = base + len;
    const u8 *ptr = base + sizeof(dns_header_t);

    /* Skip Questions */
    for (int q = 0; q < qdcount; q++) {
        ptr = dns_skip_name(ptr, end, base);
        if (!ptr || ptr + 4 > end) return;
        ptr += 4; /* Skip QTYPE & QCLASS */
    }

    /* Parse Answers */
    for (int a = 0; a < ancount; a++) {
        ptr = dns_skip_name(ptr, end, base);
        if (!ptr || ptr + 10 > end) return;

        u16 type = ntohs(*(const u16 *)ptr);
        ptr += 2;
        u16 cls = ntohs(*(const u16 *)ptr);
        (void)cls;
        ptr += 2;
        /* skip TTL (4 bytes) */
        ptr += 4;
        u16 rdlen = ntohs(*(const u16 *)ptr);
        ptr += 2;

        if (ptr + rdlen > end) return;

        if (type == 1 && rdlen == 4) { /* Type A (IPv4 Address) */
            kmemcpy(&g_dns_result_ip, ptr, 4);
            g_dns_resolved = true;
            return;
        }
        ptr += rdlen;
    }
}

bool dns_resolve(const char *hostname, ip_addr_t *out_ip, u32 timeout_ms) {
    if (!hostname || !out_ip) return false;

    /* If already an IPv4 literal address like "10.0.2.2", parse directly */
    if (str_to_ip(hostname, out_ip)) {
        return true;
    }

    /* Static hosts table */
    if (k_strcmp_match(hostname, "test.astraos.local")) {
        *out_ip = IP_ADDR(10, 0, 2, 2);
        kprintf("[DNS] Resolved 'test.astraos.local' -> 10.0.2.2 (static hosts)\n");
        return true;
    }
    if (k_strcmp_match(hostname, "localhost")) {
        *out_ip = IP_ADDR(127, 0, 0, 1);
        return true;
    }

    net_if_t *netif = net_get_interface();
    if (!netif || netif->dns == 0) {
        kprintf("[DNS WARN] No DNS server configured.\n");
        return false;
    }

    g_dns_client_port++;
    if (g_dns_client_port > 60000) g_dns_client_port = 45000;
    udp_bind(g_dns_client_port, dns_on_packet);

    g_dns_resolved = false;
    g_dns_result_ip = 0;
    g_dns_query_id++;

    u8 packet[512];
    dns_header_t *hdr = (dns_header_t *)packet;
    hdr->id = htons(g_dns_query_id);
    hdr->flags = htons(0x0100); /* Standard query, recursion desired */
    hdr->qdcount = htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    /* Format QNAME: label length + characters */
    u8 *qname = packet + sizeof(dns_header_t);
    const char *src = hostname;
    u8 *dest = qname;

    while (*src) {
        const char *dot = src;
        while (*dot && *dot != '.') dot++;
        size_t label_len = dot - src;
        if (label_len > 63) {
            udp_unbind(g_dns_client_port);
            return false;
        }
        *dest++ = (u8)label_len;
        for (size_t l = 0; l < label_len; l++) {
            *dest++ = (u8)src[l];
        }
        if (*dot == '.') {
            src = dot + 1;
        } else {
            src = dot;
        }
    }
    *dest++ = 0; /* Terminating zero label */

    /* QTYPE = 1 (A), QCLASS = 1 (IN) */
    *(u16 *)dest = htons(1);
    dest += 2;
    *(u16 *)dest = htons(1);
    dest += 2;

    size_t query_len = dest - packet;

    /* Send DNS query */
    udp_send(netif->dns, g_dns_client_port, 53, packet, query_len);

    /* Poll for DNS reply */
    u32 loops = (timeout_ms > 0) ? (timeout_ms * 100) : 50000;
    while (loops-- && !g_dns_resolved) {
        net_poll();
        for (volatile int d = 0; d < 100; d++) io_wait();
    }

    udp_unbind(g_dns_client_port);

    if (g_dns_resolved) {
        *out_ip = g_dns_result_ip;
        char ip_str[32];
        ip_to_str(*out_ip, ip_str, sizeof(ip_str));
        kprintf("[DNS] Resolved '%s' -> %s\n", hostname, ip_str);
        return true;
    }

    kprintf("[DNS WARN] Hostname resolution timed out for '%s'.\n", hostname);
    return false;
}
