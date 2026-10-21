#include <kernel/net.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>

static u16 g_ipv4_packet_id = 1;

/* Declarations for protocol handlers */
void icmp_handle_packet(ip_addr_t src_ip, const void *data, size_t len);
void udp_handle_packet(ip_addr_t src_ip, const void *data, size_t len);
void tcp_handle_packet(ip_addr_t src_ip, const void *data, size_t len);

u16 ipv4_checksum(const void *data, size_t len) {
    const u16 *ptr = (const u16 *)data;
    u32 sum = 0;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    if (len == 1) {
        sum += *(const u8 *)ptr;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (u16)(~sum);
}

int ipv4_send(ip_addr_t dst_ip, u8 protocol, const void *payload, size_t len) {
    if (!payload || len > 1480) return -1;

    net_if_t *netif = net_get_interface();
    if (!netif) return -2;

    u8 dst_mac[6];
    if (dst_ip == IP_ADDR_BROADCAST) {
        kmemset(dst_mac, 0xFF, 6);
    } else {
        /* Subnet routing check */
        ip_addr_t next_hop = dst_ip;
        if ((dst_ip & netif->netmask) != (netif->ip & netif->netmask)) {
            next_hop = netif->gateway;
        }

        if (!arp_resolve(next_hop, dst_mac, 500)) {
            /* If ARP resolution fails, try broadcast as fallback */
            kmemset(dst_mac, 0xFF, 6);
        }
    }

    static u8 packet[1500];
    ipv4_header_t *hdr = (ipv4_header_t *)packet;

    hdr->version_ihl = 0x45; /* Version 4, Header length 5 (20 bytes) */
    hdr->tos = 0;
    hdr->total_len = htons((u16)(sizeof(ipv4_header_t) + len));
    hdr->id = htons(g_ipv4_packet_id++);
    hdr->flags_frag = htons(0x4000); /* Don't Fragment flag */
    hdr->ttl = 64;
    hdr->protocol = protocol;
    hdr->checksum = 0;
    hdr->src_ip = netif->ip;
    hdr->dst_ip = dst_ip;

    hdr->checksum = ipv4_checksum(hdr, sizeof(ipv4_header_t));

    kmemcpy(packet + sizeof(ipv4_header_t), payload, len);

    return ethernet_send(dst_mac, ETHERTYPE_IPV4, packet, sizeof(ipv4_header_t) + len);
}

void ipv4_handle_packet(const void *data, size_t len) {
    if (!data || len < sizeof(ipv4_header_t)) return;

    const ipv4_header_t *hdr = (const ipv4_header_t *)data;

    /* Verify IPv4 version */
    if ((hdr->version_ihl >> 4) != 4) return;

    size_t ihl = (hdr->version_ihl & 0x0F) * 4;
    if (ihl < sizeof(ipv4_header_t) || len < ihl) return;

    /* Verify Header Checksum */
    if (ipv4_checksum(hdr, ihl) != 0) {
        return; /* Corrupted packet dropped */
    }

    size_t total_len = ntohs(hdr->total_len);
    if (total_len > len) return; /* Incomplete packet */

    net_if_t *netif = net_get_interface();
    if (!netif) return;

    /* Check destination IP */
    if (hdr->dst_ip != netif->ip &&
        hdr->dst_ip != IP_ADDR_BROADCAST &&
        hdr->dst_ip != 0) {
        return; /* Not addressed to us */
    }

    const u8 *payload = (const u8 *)data + ihl;
    size_t payload_len = total_len - ihl;

    if (hdr->protocol == IP_PROTO_ICMP) {
        icmp_handle_packet(hdr->src_ip, payload, payload_len);
    } else if (hdr->protocol == IP_PROTO_UDP) {
        udp_handle_packet(hdr->src_ip, payload, payload_len);
    } else if (hdr->protocol == IP_PROTO_TCP) {
        tcp_handle_packet(hdr->src_ip, payload, payload_len);
    }
}
