#include <kernel/net.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/arch/x86_64/cpu.h>

static tcp_socket_t g_tcp_sockets[TCP_MAX_SOCKETS] = {0};
static u16 g_tcp_next_port = 50000;

static u16 tcp_checksum(ip_addr_t src_ip, ip_addr_t dst_ip, const tcp_header_t *hdr, const void *payload, size_t payload_len) {
    struct {
        u32 src_ip;
        u32 dst_ip;
        u8  zero;
        u8  protocol;
        u16 tcp_len;
    } __attribute__((packed)) pseudo;

    size_t total_tcp_len = sizeof(tcp_header_t) + payload_len;

    pseudo.src_ip = src_ip;
    pseudo.dst_ip = dst_ip;
    pseudo.zero = 0;
    pseudo.protocol = IP_PROTO_TCP;
    pseudo.tcp_len = htons((u16)total_tcp_len);

    u32 sum = 0;
    const u16 *ptr = (const u16 *)&pseudo;
    for (size_t i = 0; i < sizeof(pseudo) / 2; i++) {
        sum += *ptr++;
    }

    ptr = (const u16 *)hdr;
    for (size_t i = 0; i < sizeof(tcp_header_t) / 2; i++) {
        sum += *ptr++;
    }

    if (payload && payload_len > 0) {
        ptr = (const u16 *)payload;
        size_t rem = payload_len;
        while (rem > 1) {
            sum += *ptr++;
            rem -= 2;
        }
        if (rem == 1) {
            sum += *(const u8 *)ptr;
        }
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    u16 csum = (u16)(~sum);
    return (csum == 0) ? 0xFFFF : csum;
}

static int tcp_send_packet(tcp_socket_t *sock, u8 flags, const void *payload, size_t payload_len) {
    if (!sock) return -1;
    net_if_t *netif = net_get_interface();
    if (!netif) return -2;

    static u8 packet[1500];
    tcp_header_t *hdr = (tcp_header_t *)packet;
    kmemset(hdr, 0, sizeof(tcp_header_t));

    hdr->src_port = htons(sock->local_port);
    hdr->dst_port = htons(sock->remote_port);
    hdr->seq_num = htonl(sock->local_seq);
    hdr->ack_num = htonl(sock->remote_seq);
    hdr->data_offset = (sizeof(tcp_header_t) / 4) << 4; /* 5 dwords = 20 bytes */
    hdr->flags = flags;
    hdr->window_size = htons(8192);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    if (payload && payload_len > 0) {
        kmemcpy(packet + sizeof(tcp_header_t), payload, payload_len);
    }

    hdr->checksum = tcp_checksum(netif->ip, sock->remote_ip, hdr, payload, payload_len);

    size_t total_len = sizeof(tcp_header_t) + payload_len;
    return ipv4_send(sock->remote_ip, IP_PROTO_TCP, packet, total_len);
}

int tcp_socket_create(void) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!g_tcp_sockets[i].in_use) {
            tcp_socket_t *s = &g_tcp_sockets[i];
            kmemset(s, 0, sizeof(tcp_socket_t));
            s->id = i + 1;
            s->in_use = true;
            s->state = TCP_STATE_CLOSED;
            s->local_port = g_tcp_next_port++;
            if (g_tcp_next_port > 65000) g_tcp_next_port = 50000;
            return s->id;
        }
    }
    return -1;
}

static tcp_socket_t *tcp_get_socket(int id) {
    if (id <= 0 || id > TCP_MAX_SOCKETS) return NULL;
    tcp_socket_t *s = &g_tcp_sockets[id - 1];
    if (!s->in_use) return NULL;
    return s;
}

int tcp_connect(int sock_id, ip_addr_t remote_ip, u16 remote_port, u32 timeout_ms) {
    tcp_socket_t *s = tcp_get_socket(sock_id);
    if (!s || remote_ip == 0 || remote_port == 0) return -1;

    s->remote_ip = remote_ip;
    s->remote_port = remote_port;
    s->local_seq = 0x24681357 + (u32)sock_id;
    s->remote_seq = 0;
    s->rx_head = 0;
    s->rx_tail = 0;
    s->rx_count = 0;
    s->state = TCP_STATE_SYN_SENT;

    /* Send SYN */
    tcp_send_packet(s, TCP_FLAG_SYN, NULL, 0);

    /* Wait for connection establishment */
    u32 loops = (timeout_ms > 0) ? (timeout_ms * 100) : 50000;
    while (loops-- && s->state == TCP_STATE_SYN_SENT) {
        net_poll();
        for (volatile int d = 0; d < 100; d++) io_wait();
    }

    if (s->state == TCP_STATE_ESTABLISHED) {
        return 0; /* Connected */
    }

    s->state = TCP_STATE_CLOSED;
    return -2; /* Connection timed out */
}

int tcp_send(int sock_id, const void *data, size_t len) {
    tcp_socket_t *s = tcp_get_socket(sock_id);
    if (!s || s->state != TCP_STATE_ESTABLISHED) return -1;
    if (!data || len == 0) return 0;

    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > 1400) chunk = 1400;

        int res = tcp_send_packet(s, TCP_FLAG_ACK | TCP_FLAG_PSH, (const u8 *)data + sent, chunk);
        if (res < 0) return (sent > 0) ? (int)sent : res;

        s->local_seq += chunk;
        sent += chunk;
    }

    return (int)sent;
}

int tcp_recv(int sock_id, void *buf, size_t max_len, u32 timeout_ms) {
    tcp_socket_t *s = tcp_get_socket(sock_id);
    if (!s || !buf || max_len == 0) return -1;

    u32 loops = (timeout_ms > 0) ? (timeout_ms * 100) : 50000;
    while (loops-- && s->rx_count == 0 && s->state == TCP_STATE_ESTABLISHED) {
        net_poll();
        for (volatile int d = 0; d < 100; d++) io_wait();
    }

    if (s->rx_count == 0) {
        if (s->state != TCP_STATE_ESTABLISHED) return 0; /* EOF / Closed */
        return -2; /* Timeout */
    }

    size_t to_read = (max_len < s->rx_count) ? max_len : s->rx_count;
    u8 *out = (u8 *)buf;

    for (size_t i = 0; i < to_read; i++) {
        out[i] = s->rx_buffer[s->rx_head];
        s->rx_head = (s->rx_head + 1) % TCP_RX_BUFFER_SIZE;
    }
    s->rx_count -= to_read;

    return (int)to_read;
}

int tcp_close(int sock_id) {
    tcp_socket_t *s = tcp_get_socket(sock_id);
    if (!s) return -1;

    if (s->state == TCP_STATE_ESTABLISHED) {
        s->state = TCP_STATE_FIN_WAIT_1;
        tcp_send_packet(s, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        s->local_seq++;

        /* Poll briefly for remote ACK */
        u32 loops = 10000;
        while (loops-- && s->state != TCP_STATE_CLOSED && s->state != TCP_STATE_TIME_WAIT) {
            net_poll();
            for (volatile int d = 0; d < 50; d++) io_wait();
        }
    }

    s->in_use = false;
    s->state = TCP_STATE_CLOSED;
    return 0;
}

void tcp_handle_packet(ip_addr_t src_ip, const void *data, size_t len) {
    if (!data || len < sizeof(tcp_header_t)) return;

    const tcp_header_t *hdr = (const tcp_header_t *)data;
    u16 src_port = ntohs(hdr->src_port);
    u16 dst_port = ntohs(hdr->dst_port);
    u32 seq = ntohl(hdr->seq_num);
    u32 ack = ntohl(hdr->ack_num);
    u8 flags = hdr->flags;

    size_t data_offset = ((hdr->data_offset >> 4) & 0x0F) * 4;
    if (data_offset < sizeof(tcp_header_t) || len < data_offset) return;

    const u8 *payload = (const u8 *)data + data_offset;
    size_t payload_len = len - data_offset;

    /* Find matching socket */
    tcp_socket_t *s = NULL;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (g_tcp_sockets[i].in_use &&
            g_tcp_sockets[i].local_port == dst_port &&
            (g_tcp_sockets[i].remote_port == src_port || g_tcp_sockets[i].remote_port == 0) &&
            (g_tcp_sockets[i].remote_ip == src_ip || g_tcp_sockets[i].remote_ip == 0)) {
            s = &g_tcp_sockets[i];
            break;
        }
    }

    if (!s) return;

    /* Process according to TCP state */
    if (s->state == TCP_STATE_SYN_SENT) {
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
            s->remote_seq = seq + 1;
            s->local_seq = ack;
            s->state = TCP_STATE_ESTABLISHED;
            /* Send final ACK of 3-way handshake */
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
        } else if (flags & TCP_FLAG_RST) {
            s->state = TCP_STATE_CLOSED;
        }
    } else if (s->state == TCP_STATE_ESTABLISHED) {
        if (flags & TCP_FLAG_RST) {
            s->state = TCP_STATE_CLOSED;
            return;
        }

        if (payload_len > 0) {
            /* Append payload into socket's receive buffer */
            for (size_t i = 0; i < payload_len; i++) {
                if (s->rx_count < TCP_RX_BUFFER_SIZE) {
                    s->rx_buffer[s->rx_tail] = payload[i];
                    s->rx_tail = (s->rx_tail + 1) % TCP_RX_BUFFER_SIZE;
                    s->rx_count++;
                }
            }
            s->remote_seq += payload_len;
            /* Send ACK acknowledging data */
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
        }

        if (flags & TCP_FLAG_FIN) {
            s->remote_seq++;
            s->state = TCP_STATE_CLOSE_WAIT;
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
        }
    } else if (s->state == TCP_STATE_FIN_WAIT_1) {
        if (flags & TCP_FLAG_ACK) {
            s->state = TCP_STATE_TIME_WAIT;
        }
        if (flags & TCP_FLAG_FIN) {
            s->remote_seq++;
            tcp_send_packet(s, TCP_FLAG_ACK, NULL, 0);
            s->state = TCP_STATE_CLOSED;
        }
    }
}
