#include <kernel/net.h>
#include <kernel/e1000.h>
#include <kernel/mem.h>

int ethernet_send(const u8 *dst_mac, u16 ethertype, const void *payload, size_t len) {
    if (!dst_mac || !payload || len == 0 || len > 1500) return -1;

    net_if_t *netif = net_get_interface();
    if (!netif) return -2;

    static u8 packet[1514];
    eth_header_t *hdr = (eth_header_t *)packet;

    kmemcpy(hdr->dst_mac, dst_mac, 6);
    kmemcpy(hdr->src_mac, netif->mac, 6);
    hdr->ethertype = htons(ethertype);

    kmemcpy(packet + sizeof(eth_header_t), payload, len);

    size_t total_len = sizeof(eth_header_t) + len;
    /* Enforce minimum Ethernet frame size of 60 bytes (excluding FCS) */
    if (total_len < 60) {
        kmemset(packet + total_len, 0, 60 - total_len);
        total_len = 60;
    }

    return e1000_send(packet, total_len);
}

void ethernet_handle_packet(const void *data, size_t len) {
    if (!data || len < sizeof(eth_header_t)) return;

    const eth_header_t *hdr = (const eth_header_t *)data;
    net_if_t *netif = net_get_interface();
    if (!netif) return;

    /* Check destination MAC address: must match our MAC or broadcast FF:FF:FF:FF:FF:FF */
    bool match = true;
    bool broadcast = true;
    for (int i = 0; i < 6; i++) {
        if (hdr->dst_mac[i] != netif->mac[i]) match = false;
        if (hdr->dst_mac[i] != 0xFF) broadcast = false;
    }

    if (!match && !broadcast) {
        return; /* Filter out packets not addressed to us */
    }

    u16 ethertype = ntohs(hdr->ethertype);
    const u8 *payload = (const u8 *)data + sizeof(eth_header_t);
    size_t payload_len = len - sizeof(eth_header_t);

    if (ethertype == ETHERTYPE_ARP) {
        arp_handle_packet(payload, payload_len);
    } else if (ethertype == ETHERTYPE_IPV4) {
        ipv4_handle_packet(payload, payload_len);
    }
}
