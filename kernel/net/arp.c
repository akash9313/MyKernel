#include <kernel/net.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/arch/x86_64/cpu.h>

#define ARP_CACHE_SIZE 16

typedef struct arp_entry {
    ip_addr_t ip;
    u8 mac[6];
    bool valid;
} arp_entry_t;

static arp_entry_t g_arp_cache[ARP_CACHE_SIZE] = {0};
static int g_arp_next_replace = 0;

static const u8 g_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

bool arp_lookup(ip_addr_t ip, u8 *out_mac) {
    if (!out_mac) return false;

    if (ip == IP_ADDR_BROADCAST) {
        kmemcpy(out_mac, g_broadcast_mac, 6);
        return true;
    }

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            kmemcpy(out_mac, g_arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void arp_cache_insert(ip_addr_t ip, const u8 *mac) {
    if (!mac) return;

    /* Check if already present */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            kmemcpy(g_arp_cache[i].mac, mac, 6);
            return;
        }
    }

    /* Insert into next slot */
    int idx = g_arp_next_replace;
    g_arp_next_replace = (g_arp_next_replace + 1) % ARP_CACHE_SIZE;

    g_arp_cache[idx].ip = ip;
    kmemcpy(g_arp_cache[idx].mac, mac, 6);
    g_arp_cache[idx].valid = true;
}

bool arp_resolve(ip_addr_t ip, u8 *out_mac, u32 timeout_ms) {
    if (!out_mac) return false;

    if (arp_lookup(ip, out_mac)) {
        return true;
    }

    net_if_t *netif = net_get_interface();
    if (!netif) return false;

    /* Prepare ARP Request */
    arp_packet_t req;
    req.hw_type = htons(ARP_HW_ETHERNET);
    req.proto_type = htons(ARP_PROTO_IPV4);
    req.hw_size = 6;
    req.proto_size = 4;
    req.opcode = htons(ARP_OP_REQUEST);

    kmemcpy(req.sender_mac, netif->mac, 6);
    req.sender_ip = netif->ip;
    kmemset(req.target_mac, 0, 6);
    req.target_ip = ip;

    /* Broadcast ARP request */
    ethernet_send(g_broadcast_mac, ETHERTYPE_ARP, &req, sizeof(req));

    /* Poll network for reply */
    u32 loops = (timeout_ms > 0) ? (timeout_ms * 100) : 50000;
    while (loops--) {
        net_poll();
        if (arp_lookup(ip, out_mac)) {
            return true;
        }
        for (volatile int d = 0; d < 100; d++) io_wait();
    }

    return false;
}

void arp_handle_packet(const void *data, size_t len) {
    if (!data || len < sizeof(arp_packet_t)) return;

    const arp_packet_t *pkt = (const arp_packet_t *)data;
    if (ntohs(pkt->hw_type) != ARP_HW_ETHERNET || ntohs(pkt->proto_type) != ARP_PROTO_IPV4) {
        return;
    }

    net_if_t *netif = net_get_interface();
    if (!netif) return;

    /* Learn sender MAC address into cache */
    arp_cache_insert(pkt->sender_ip, pkt->sender_mac);

    u16 op = ntohs(pkt->opcode);
    if (op == ARP_OP_REQUEST && pkt->target_ip == netif->ip) {
        /* Send ARP Reply */
        arp_packet_t reply;
        reply.hw_type = htons(ARP_HW_ETHERNET);
        reply.proto_type = htons(ARP_PROTO_IPV4);
        reply.hw_size = 6;
        reply.proto_size = 4;
        reply.opcode = htons(ARP_OP_REPLY);

        kmemcpy(reply.sender_mac, netif->mac, 6);
        reply.sender_ip = netif->ip;
        kmemcpy(reply.target_mac, pkt->sender_mac, 6);
        reply.target_ip = pkt->sender_ip;

        ethernet_send(pkt->sender_mac, ETHERTYPE_ARP, &reply, sizeof(reply));
    }
}
