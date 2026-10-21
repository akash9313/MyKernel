#include <kernel/net.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>

#define MAX_UDP_LISTENERS 16

typedef struct udp_listener {
    u16 port;
    udp_callback_t callback;
    bool active;
} udp_listener_t;

static udp_listener_t g_udp_listeners[MAX_UDP_LISTENERS] = {0};

bool udp_bind(u16 port, udp_callback_t cb) {
    if (port == 0 || !cb) return false;

    /* Check if already bound */
    for (int i = 0; i < MAX_UDP_LISTENERS; i++) {
        if (g_udp_listeners[i].active && g_udp_listeners[i].port == port) {
            g_udp_listeners[i].callback = cb;
            return true;
        }
    }

    /* Find free slot */
    for (int i = 0; i < MAX_UDP_LISTENERS; i++) {
        if (!g_udp_listeners[i].active) {
            g_udp_listeners[i].port = port;
            g_udp_listeners[i].callback = cb;
            g_udp_listeners[i].active = true;
            return true;
        }
    }
    return false;
}

void udp_unbind(u16 port) {
    for (int i = 0; i < MAX_UDP_LISTENERS; i++) {
        if (g_udp_listeners[i].active && g_udp_listeners[i].port == port) {
            g_udp_listeners[i].active = false;
            g_udp_listeners[i].callback = NULL;
            return;
        }
    }
}

int udp_send(ip_addr_t dst_ip, u16 src_port, u16 dst_port, const void *payload, size_t len) {
    if (!payload && len > 0) return -1;
    if (len > 1472) return -2;

    net_if_t *netif = net_get_interface();
    if (!netif) return -3;

    u8 packet[1500];
    udp_header_t *hdr = (udp_header_t *)packet;
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length = htons((u16)(sizeof(udp_header_t) + len));
    hdr->checksum = 0;

    if (payload && len > 0) {
        kmemcpy(packet + sizeof(udp_header_t), payload, len);
    }

    /* Compute UDP Checksum with IPv4 Pseudo-Header */
    struct {
        u32 src_ip;
        u32 dst_ip;
        u8  zero;
        u8  protocol;
        u16 udp_len;
    } __attribute__((packed)) pseudo;

    pseudo.src_ip = netif->ip;
    pseudo.dst_ip = dst_ip;
    pseudo.zero = 0;
    pseudo.protocol = IP_PROTO_UDP;
    pseudo.udp_len = hdr->length;

    u32 sum = 0;
    const u16 *ptr = (const u16 *)&pseudo;
    for (size_t i = 0; i < sizeof(pseudo) / 2; i++) {
        sum += *ptr++;
    }

    ptr = (const u16 *)packet;
    size_t total_udp_len = sizeof(udp_header_t) + len;
    size_t rem = total_udp_len;
    while (rem > 1) {
        sum += *ptr++;
        rem -= 2;
    }
    if (rem == 1) {
        sum += *(const u8 *)ptr;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    u16 csum = (u16)(~sum);
    hdr->checksum = (csum == 0) ? 0xFFFF : csum;

    return ipv4_send(dst_ip, IP_PROTO_UDP, packet, total_udp_len);
}

void udp_handle_packet(ip_addr_t src_ip, const void *data, size_t len) {
    if (!data || len < sizeof(udp_header_t)) return;

    const udp_header_t *hdr = (const udp_header_t *)data;
    u16 src_port = ntohs(hdr->src_port);
    u16 dst_port = ntohs(hdr->dst_port);
    u16 length = ntohs(hdr->length);

    if (length < sizeof(udp_header_t) || length > len) return;

    const u8 *payload = (const u8 *)data + sizeof(udp_header_t);
    size_t payload_len = length - sizeof(udp_header_t);

    for (int i = 0; i < MAX_UDP_LISTENERS; i++) {
        if (g_udp_listeners[i].active && g_udp_listeners[i].port == dst_port) {
            g_udp_listeners[i].callback(src_ip, src_port, payload, payload_len);
            break;
        }
    }
}
