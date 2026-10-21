#include <kernel/net.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/arch/x86_64/cpu.h>

#define DHCP_MAGIC_COOKIE 0x63825363

#define DHCP_OPT_PAD             0
#define DHCP_OPT_SUBNET_MASK     1
#define DHCP_OPT_ROUTER          3
#define DHCP_OPT_DNS             6
#define DHCP_OPT_REQUESTED_IP    50
#define DHCP_OPT_LEASE_TIME      51
#define DHCP_OPT_MESSAGE_TYPE    53
#define DHCP_OPT_SERVER_ID       54
#define DHCP_OPT_PARAM_REQUEST   55
#define DHCP_OPT_END             255

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

typedef struct dhcp_packet {
    u8  op;
    u8  htype;
    u8  hlen;
    u8  hops;
    u32 xid;
    u16 secs;
    u16 flags;
    u32 ciaddr;
    u32 yiaddr;
    u32 siaddr;
    u32 giaddr;
    u8  chaddr[16];
    u8  sname[64];
    u8  file[128];
    u32 magic;
    u8  options[308];
} __attribute__((packed)) dhcp_packet_t;

typedef enum {
    DHCP_IDLE = 0,
    DHCP_WAIT_OFFER,
    DHCP_WAIT_ACK,
    DHCP_BOUND
} dhcp_state_t;

static dhcp_state_t g_dhcp_state = DHCP_IDLE;
static u32 g_dhcp_xid = 0x3903F326;
static ip_addr_t g_offered_ip = 0;
static ip_addr_t g_server_id = 0;
static ip_addr_t g_netmask = 0;
static ip_addr_t g_gateway = 0;
static ip_addr_t g_dns = 0;

static void dhcp_on_packet(ip_addr_t src_ip, u16 src_port, const void *payload, size_t len) {
    (void)src_ip;
    (void)src_port;

    if (!payload || len < sizeof(dhcp_packet_t) - 308) return;

    const dhcp_packet_t *pkt = (const dhcp_packet_t *)payload;
    if (pkt->op != 2) return; /* Only process Boot Replies */
    if (pkt->xid != g_dhcp_xid) return;
    if (pkt->magic != htonl(DHCP_MAGIC_COOKIE)) return;

    /* Parse Options */
    const u8 *opts = pkt->options;
    size_t opt_len = len - (sizeof(dhcp_packet_t) - 308);

    u8 msg_type = 0;
    ip_addr_t srv_id = 0;
    ip_addr_t mask = 0;
    ip_addr_t router = 0;
    ip_addr_t dns = 0;

    size_t i = 0;
    while (i < opt_len) {
        u8 opt = opts[i++];
        if (opt == DHCP_OPT_END) break;
        if (opt == DHCP_OPT_PAD) continue;
        if (i >= opt_len) break;
        u8 length = opts[i++];
        if (i + length > opt_len) break;

        if (opt == DHCP_OPT_MESSAGE_TYPE && length >= 1) {
            msg_type = opts[i];
        } else if (opt == DHCP_OPT_SERVER_ID && length >= 4) {
            kmemcpy(&srv_id, &opts[i], 4);
        } else if (opt == DHCP_OPT_SUBNET_MASK && length >= 4) {
            kmemcpy(&mask, &opts[i], 4);
        } else if (opt == DHCP_OPT_ROUTER && length >= 4) {
            kmemcpy(&router, &opts[i], 4);
        } else if (opt == DHCP_OPT_DNS && length >= 4) {
            kmemcpy(&dns, &opts[i], 4);
        }
        i += length;
    }

    if (g_dhcp_state == DHCP_WAIT_OFFER && msg_type == DHCP_OFFER) {
        g_offered_ip = pkt->yiaddr;
        g_server_id = srv_id;
        g_netmask = mask;
        g_gateway = router;
        g_dns = dns;
        g_dhcp_state = DHCP_WAIT_ACK;
    } else if (g_dhcp_state == DHCP_WAIT_ACK && msg_type == DHCP_ACK) {
        if (pkt->yiaddr != 0) g_offered_ip = pkt->yiaddr;
        if (mask != 0) g_netmask = mask;
        if (router != 0) g_gateway = router;
        if (dns != 0) g_dns = dns;
        g_dhcp_state = DHCP_BOUND;
    }
}

bool dhcp_request_lease(u32 timeout_ms) {
    kprintf("[DHCP] Starting DHCP Discovery...\n");

    net_if_t *netif = net_get_interface();
    if (!netif) return false;

    udp_bind(68, dhcp_on_packet);

    g_dhcp_state = DHCP_WAIT_OFFER;
    g_dhcp_xid++;
    g_offered_ip = 0;
    g_server_id = 0;

    /* Build DHCPDISCOVER */
    dhcp_packet_t disc;
    kmemset(&disc, 0, sizeof(disc));
    disc.op = 1; /* BOOTREQUEST */
    disc.htype = 1;
    disc.hlen = 6;
    disc.xid = g_dhcp_xid;
    disc.flags = htons(0x8000); /* Broadcast */
    kmemcpy(disc.chaddr, netif->mac, 6);
    disc.magic = htonl(DHCP_MAGIC_COOKIE);

    int opt_pos = 0;
    disc.options[opt_pos++] = DHCP_OPT_MESSAGE_TYPE;
    disc.options[opt_pos++] = 1;
    disc.options[opt_pos++] = DHCP_DISCOVER;

    disc.options[opt_pos++] = DHCP_OPT_PARAM_REQUEST;
    disc.options[opt_pos++] = 3;
    disc.options[opt_pos++] = DHCP_OPT_SUBNET_MASK;
    disc.options[opt_pos++] = DHCP_OPT_ROUTER;
    disc.options[opt_pos++] = DHCP_OPT_DNS;

    disc.options[opt_pos++] = DHCP_OPT_END;

    /* Send DHCPDISCOVER to 255.255.255.255:67 from 0.0.0.0:68 */
    udp_send(IP_ADDR_BROADCAST, 68, 67, &disc, sizeof(dhcp_packet_t) - 308 + opt_pos);

    /* Wait for DHCPOFFER */
    u32 loops = (timeout_ms > 0) ? (timeout_ms * 100) : 50000;
    while (loops-- && g_dhcp_state == DHCP_WAIT_OFFER) {
        net_poll();
        for (volatile int d = 0; d < 100; d++) io_wait();
    }

    if (g_dhcp_state != DHCP_WAIT_ACK || g_offered_ip == 0) {
        kprintf("[DHCP WARN] Discovery timed out (No DHCP Server responded).\n");
        udp_unbind(68);
        return false;
    }

    char ip_str[32];
    ip_to_str(g_offered_ip, ip_str, sizeof(ip_str));
    kprintf("[DHCP] Received DHCPOFFER for IP: %s\n", ip_str);

    /* Build DHCPREQUEST */
    dhcp_packet_t req;
    kmemset(&req, 0, sizeof(req));
    req.op = 1;
    req.htype = 1;
    req.hlen = 6;
    req.xid = g_dhcp_xid;
    req.flags = htons(0x8000);
    kmemcpy(req.chaddr, netif->mac, 6);
    req.magic = htonl(DHCP_MAGIC_COOKIE);

    opt_pos = 0;
    req.options[opt_pos++] = DHCP_OPT_MESSAGE_TYPE;
    req.options[opt_pos++] = 1;
    req.options[opt_pos++] = DHCP_REQUEST;

    req.options[opt_pos++] = DHCP_OPT_REQUESTED_IP;
    req.options[opt_pos++] = 4;
    kmemcpy(&req.options[opt_pos], &g_offered_ip, 4);
    opt_pos += 4;

    if (g_server_id != 0) {
        req.options[opt_pos++] = DHCP_OPT_SERVER_ID;
        req.options[opt_pos++] = 4;
        kmemcpy(&req.options[opt_pos], &g_server_id, 4);
        opt_pos += 4;
    }

    req.options[opt_pos++] = DHCP_OPT_PARAM_REQUEST;
    req.options[opt_pos++] = 3;
    req.options[opt_pos++] = DHCP_OPT_SUBNET_MASK;
    req.options[opt_pos++] = DHCP_OPT_ROUTER;
    req.options[opt_pos++] = DHCP_OPT_DNS;

    req.options[opt_pos++] = DHCP_OPT_END;

    udp_send(IP_ADDR_BROADCAST, 68, 67, &req, sizeof(dhcp_packet_t) - 308 + opt_pos);

    /* Wait for DHCPACK */
    loops = (timeout_ms > 0) ? (timeout_ms * 100) : 50000;
    while (loops-- && g_dhcp_state != DHCP_BOUND) {
        net_poll();
        for (volatile int d = 0; d < 100; d++) io_wait();
    }

    udp_unbind(68);

    if (g_dhcp_state == DHCP_BOUND) {
        netif->ip = g_offered_ip;
        if (g_netmask != 0) netif->netmask = g_netmask;
        if (g_gateway != 0) netif->gateway = g_gateway;
        if (g_dns != 0) netif->dns = g_dns;
        netif->configured = true;

        char mask_str[32], gw_str[32], dns_str[32];
        ip_to_str(netif->netmask, mask_str, sizeof(mask_str));
        ip_to_str(netif->gateway, gw_str, sizeof(gw_str));
        ip_to_str(netif->dns, dns_str, sizeof(dns_str));

        kprintf("[DHCP] Lease Acquired Successfully:\n");
        kprintf("  IP Address:  %s\n", ip_str);
        kprintf("  Subnet Mask: %s\n", mask_str);
        kprintf("  Gateway:     %s\n", gw_str);
        kprintf("  DNS Server:  %s\n", dns_str);
        return true;
    }

    kprintf("[DHCP WARN] Request timed out without DHCPACK.\n");
    return false;
}
