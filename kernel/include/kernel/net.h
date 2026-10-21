#ifndef ASTRA_KERNEL_NET_H
#define ASTRA_KERNEL_NET_H

#include <kernel/types.h>

/* Byte Order Macros */
static inline u16 htons(u16 v) {
    return (u16)((v << 8) | (v >> 8));
}
static inline u16 ntohs(u16 v) {
    return htons(v);
}
static inline u32 htonl(u32 v) {
    return ((v & 0x000000FF) << 24) |
           ((v & 0x0000FF00) << 8)  |
           ((v & 0x00FF0000) >> 8)  |
           ((v & 0xFF000000) >> 24);
}
static inline u32 ntohl(u32 v) {
    return htonl(v);
}

/* IPv4 & MAC Types */
typedef u32 ip_addr_t;

#define IP_ADDR(a, b, c, d) \
    ((u32)(((u32)(a) & 0xFF) | (((u32)(b) & 0xFF) << 8) | (((u32)(c) & 0xFF) << 16) | (((u32)(d) & 0xFF) << 24)))

#define IP_ADDR_ANY         0x00000000
#define IP_ADDR_BROADCAST   0xFFFFFFFF

typedef struct mac_addr {
    u8 addr[6];
} mac_addr_t;

/* Ethernet Header */
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

typedef struct eth_header {
    u8 dst_mac[6];
    u8 src_mac[6];
    u16 ethertype;
} __attribute__((packed)) eth_header_t;

/* ARP Header */
#define ARP_HW_ETHERNET 0x0001
#define ARP_PROTO_IPV4  0x0800
#define ARP_OP_REQUEST  0x0001
#define ARP_OP_REPLY    0x0002

typedef struct arp_packet {
    u16 hw_type;
    u16 proto_type;
    u8  hw_size;
    u8  proto_size;
    u16 opcode;
    u8  sender_mac[6];
    u32 sender_ip;
    u8  target_mac[6];
    u32 target_ip;
} __attribute__((packed)) arp_packet_t;

/* IPv4 Header */
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

typedef struct ipv4_header {
    u8  version_ihl;
    u8  tos;
    u16 total_len;
    u16 id;
    u16 flags_frag;
    u8  ttl;
    u8  protocol;
    u16 checksum;
    u32 src_ip;
    u32 dst_ip;
} __attribute__((packed)) ipv4_header_t;

/* ICMP Header */
#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

typedef struct icmp_header {
    u8  type;
    u8  code;
    u16 checksum;
    u16 id;
    u16 sequence;
} __attribute__((packed)) icmp_header_t;

/* UDP Header */
typedef struct udp_header {
    u16 src_port;
    u16 dst_port;
    u16 length;
    u16 checksum;
} __attribute__((packed)) udp_header_t;

/* TCP Header */
#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

typedef struct tcp_header {
    u16 src_port;
    u16 dst_port;
    u32 seq_num;
    u32 ack_num;
    u8  data_offset; /* Higher 4 bits: header length in 32-bit words */
    u8  flags;
    u16 window_size;
    u16 checksum;
    u16 urgent_ptr;
} __attribute__((packed)) tcp_header_t;

/* Network Interface Configuration */
typedef struct net_if {
    u8 mac[6];
    ip_addr_t ip;
    ip_addr_t netmask;
    ip_addr_t gateway;
    ip_addr_t dns;
    bool configured;
    bool link_up;
} net_if_t;

/* Core Network Stack API */
void net_init(void);
net_if_t *net_get_interface(void);
void net_poll(void);

/* Address conversion */
void ip_to_str(ip_addr_t ip, char *buf, size_t max_len);
bool str_to_ip(const char *str, ip_addr_t *out_ip);

/* Ethernet & ARP */
int ethernet_send(const u8 *dst_mac, u16 ethertype, const void *payload, size_t len);
void ethernet_handle_packet(const void *data, size_t len);

bool arp_lookup(ip_addr_t ip, u8 *out_mac);
bool arp_resolve(ip_addr_t ip, u8 *out_mac, u32 timeout_ms);
void arp_handle_packet(const void *data, size_t len);

/* IPv4 */
u16 ipv4_checksum(const void *data, size_t len);
int ipv4_send(ip_addr_t dst_ip, u8 protocol, const void *payload, size_t len);
void ipv4_handle_packet(const void *data, size_t len);

/* ICMP */
int icmp_send_echo(ip_addr_t dst_ip, u16 id, u16 seq, const void *payload, size_t len);
int icmp_ping(ip_addr_t dst_ip, u32 timeout_ms);

/* UDP */
typedef void (*udp_callback_t)(ip_addr_t src_ip, u16 src_port, const void *payload, size_t len);
bool udp_bind(u16 port, udp_callback_t cb);
void udp_unbind(u16 port);
int udp_send(ip_addr_t dst_ip, u16 src_port, u16 dst_port, const void *payload, size_t len);

/* DHCP Client */
bool dhcp_request_lease(u32 timeout_ms);

/* DNS Client */
bool dns_resolve(const char *hostname, ip_addr_t *out_ip, u32 timeout_ms);

/* TCP Sockets */
typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_SYN_SENT,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT
} tcp_state_t;

#define TCP_RX_BUFFER_SIZE 16384
#define TCP_MAX_SOCKETS    16

typedef struct tcp_socket {
    int id;
    bool in_use;
    tcp_state_t state;
    ip_addr_t remote_ip;
    u16 local_port;
    u16 remote_port;
    u32 local_seq;
    u32 remote_seq;
    u32 remote_ack;

    /* Receive buffer (circular) */
    u8 rx_buffer[TCP_RX_BUFFER_SIZE];
    size_t rx_head;
    size_t rx_tail;
    size_t rx_count;

    u64 last_activity_tick;
} tcp_socket_t;

int tcp_socket_create(void);
int tcp_connect(int sock_id, ip_addr_t remote_ip, u16 remote_port, u32 timeout_ms);
int tcp_send(int sock_id, const void *data, size_t len);
int tcp_recv(int sock_id, void *buf, size_t max_len, u32 timeout_ms);
int tcp_close(int sock_id);

/* HTTP Client */
int http_get(const char *host, const char *path, char *response_buf, size_t max_len, int *out_status_code);

/* Automated Self-Tests */
bool net_self_test(void);

#endif /* ASTRA_KERNEL_NET_H */
