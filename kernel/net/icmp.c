#include <kernel/net.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/arch/x86_64/cpu.h>

static volatile bool g_ping_received = false;
static volatile u16 g_ping_received_seq = 0;
static u16 g_ping_seq = 1;

void icmp_handle_packet(ip_addr_t src_ip, const void *data, size_t len) {
    if (!data || len < sizeof(icmp_header_t)) return;

    const icmp_header_t *hdr = (const icmp_header_t *)data;

    if (hdr->type == ICMP_TYPE_ECHO_REQUEST && hdr->code == 0) {
        /* Send ICMP Echo Reply */
        u8 reply[1500];
        if (len > sizeof(reply)) return;

        kmemcpy(reply, data, len);
        icmp_header_t *r_hdr = (icmp_header_t *)reply;
        r_hdr->type = ICMP_TYPE_ECHO_REPLY;
        r_hdr->code = 0;
        r_hdr->checksum = 0;
        r_hdr->checksum = ipv4_checksum(reply, len);

        ipv4_send(src_ip, IP_PROTO_ICMP, reply, len);
    } else if (hdr->type == ICMP_TYPE_ECHO_REPLY && hdr->code == 0) {
        g_ping_received = true;
        g_ping_received_seq = ntohs(hdr->sequence);
    }
}

int icmp_send_echo(ip_addr_t dst_ip, u16 id, u16 seq, const void *payload, size_t len) {
    u8 packet[1500];
    if (sizeof(icmp_header_t) + len > sizeof(packet)) return -1;

    icmp_header_t *hdr = (icmp_header_t *)packet;
    hdr->type = ICMP_TYPE_ECHO_REQUEST;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->id = htons(id);
    hdr->sequence = htons(seq);

    if (payload && len > 0) {
        kmemcpy(packet + sizeof(icmp_header_t), payload, len);
    }

    hdr->checksum = ipv4_checksum(packet, sizeof(icmp_header_t) + len);

    return ipv4_send(dst_ip, IP_PROTO_ICMP, packet, sizeof(icmp_header_t) + len);
}

int icmp_ping(ip_addr_t dst_ip, u32 timeout_ms) {
    u16 seq = g_ping_seq++;
    g_ping_received = false;
    g_ping_received_seq = 0;

    static const char ping_msg[] = "AstraOS Ping Probe";
    icmp_send_echo(dst_ip, 0xA571, seq, ping_msg, sizeof(ping_msg));

    u32 loops = (timeout_ms > 0) ? (timeout_ms * 100) : 50000;
    while (loops--) {
        net_poll();
        if (g_ping_received && g_ping_received_seq == seq) {
            return 0; /* Ping success */
        }
        for (volatile int d = 0; d < 100; d++) io_wait();
    }

    return -1; /* Timeout */
}
