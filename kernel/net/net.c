#include <kernel/net.h>
#include <kernel/e1000.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>

static net_if_t g_net_if = {0};

void net_init(void) {
    kprintf("[NET] Initializing AstraOS Network Subsystem...\n");
    kmemset(&g_net_if, 0, sizeof(g_net_if));

    if (e1000_is_available()) {
        const u8 *mac = e1000_get_mac();
        kmemcpy(g_net_if.mac, mac, 6);
        g_net_if.link_up = e1000_is_link_up();

        /* Default fallback static IP in case DHCP is not yet negotiated */
        g_net_if.ip = IP_ADDR(10, 0, 2, 15);
        g_net_if.netmask = IP_ADDR(255, 255, 255, 0);
        g_net_if.gateway = IP_ADDR(10, 0, 2, 2);
        g_net_if.dns = IP_ADDR(10, 0, 2, 3);
        g_net_if.configured = false;

        char ip_str[32];
        ip_to_str(g_net_if.ip, ip_str, sizeof(ip_str));
        kprintf("[NET] Interface configured with MAC: %02x:%02x:%02x:%02x:%02x:%02x (Default IP: %s)\n",
                g_net_if.mac[0], g_net_if.mac[1], g_net_if.mac[2],
                g_net_if.mac[3], g_net_if.mac[4], g_net_if.mac[5], ip_str);
    } else {
        kprintf("[NET WARN] No network hardware available.\n");
    }
}

net_if_t *net_get_interface(void) {
    return &g_net_if;
}

void net_poll(void) {
    static u8 rx_buf[E1000_BUFFER_SIZE];
    int len = 0;

    /* Process all ready packets in E1000 RX ring */
    while ((len = e1000_receive(rx_buf, sizeof(rx_buf))) > 0) {
        ethernet_handle_packet(rx_buf, (size_t)len);
    }
}

void ip_to_str(ip_addr_t ip, char *buf, size_t max_len) {
    if (!buf || max_len < 16) return;

    u8 a = (u8)(ip & 0xFF);
    u8 b = (u8)((ip >> 8) & 0xFF);
    u8 c = (u8)((ip >> 16) & 0xFF);
    u8 d = (u8)((ip >> 24) & 0xFF);

    size_t pos = 0;
    u8 bytes[4] = {a, b, c, d};

    for (int i = 0; i < 4; i++) {
        u8 val = bytes[i];
        if (val >= 100) {
            buf[pos++] = '0' + (val / 100);
            val %= 100;
            buf[pos++] = '0' + (val / 10);
            val %= 10;
        } else if (val >= 10) {
            buf[pos++] = '0' + (val / 10);
            val %= 10;
        }
        buf[pos++] = '0' + val;

        if (i < 3) buf[pos++] = '.';
    }
    buf[pos] = '\0';
}

bool str_to_ip(const char *str, ip_addr_t *out_ip) {
    if (!str || !out_ip) return false;

    u32 parts[4] = {0};
    int part_idx = 0;

    while (*str) {
        if (*str >= '0' && *str <= '9') {
            parts[part_idx] = parts[part_idx] * 10 + (*str - '0');
            if (parts[part_idx] > 255) return false;
        } else if (*str == '.') {
            part_idx++;
            if (part_idx > 3) return false;
        } else {
            return false;
        }
        str++;
    }

    if (part_idx != 3) return false;

    *out_ip = IP_ADDR(parts[0], parts[1], parts[2], parts[3]);
    return true;
}
