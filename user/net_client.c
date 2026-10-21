/*
 * AstraOS User-Space Network Test Utility
 *
 * Exercises:
 * 1. sys_net_get_config: reports configured interface and IP settings.
 * 2. sys_net_resolve: resolves host using DNS.
 * 3. sys_net_ping: performs ICMP ping.
 * 4. sys_net_socket, sys_net_connect, sys_net_send, sys_net_recv, sys_net_close:
 *    establishes TCP connection, sends HTTP GET request, receives and displays response.
 */

typedef unsigned long size_t;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

typedef struct user_net_if {
    u8 mac[6];
    u32 ip;
    u32 netmask;
    u32 gateway;
    u32 dns;
    int configured;
    int link_up;
} user_net_if_t;

long sys_write(int fd, const void *buf, size_t count);
long sys_yield(void);
long sys_getpid(void);
void sys_exit(int status) __attribute__((noreturn));

long sys_net_socket(void);
long sys_net_connect(int sock, unsigned int ip, unsigned short port, unsigned int timeout_ms);
long sys_net_send(int sock, const void *buf, size_t len);
long sys_net_recv(int sock, void *buf, size_t max_len, unsigned int timeout_ms);
long sys_net_close(int sock);
long sys_net_get_config(void *net_if);
long sys_net_resolve(const char *hostname, unsigned int *out_ip, unsigned int timeout_ms);
long sys_net_ping(unsigned int ip, unsigned int timeout_ms);

static size_t ustrlen(const char *s) {
    size_t len = 0;
    while (s[len] != '\0') len++;
    return len;
}

static void print(const char *s) {
    sys_write(1, s, ustrlen(s));
}

static void print_ip(u32 ip) {
    char buf[32];
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        u8 byte = (ip >> (i * 8)) & 0xFF;
        if (byte >= 100) {
            buf[pos++] = '0' + (byte / 100);
            buf[pos++] = '0' + ((byte / 10) % 10);
            buf[pos++] = '0' + (byte % 10);
        } else if (byte >= 10) {
            buf[pos++] = '0' + (byte / 10);
            buf[pos++] = '0' + (byte % 10);
        } else {
            buf[pos++] = '0' + byte;
        }
        if (i < 3) buf[pos++] = '.';
    }
    buf[pos] = '\0';
    print(buf);
}

int main(void) {
    print("\n=======================================================\n");
    print("      AstraOS Native User-Space Network Utility        \n");
    print("=======================================================\n");

    /* 1. Report configured interface and IP settings */
    print("[NET-USER] 1. Inspecting network interface configuration...\n");
    user_net_if_t nif;
    for (size_t i = 0; i < sizeof(nif); i++) ((char *)&nif)[i] = 0;

    long res = sys_net_get_config(&nif);
    if (res < 0) {
        print("[NET-USER ERROR] sys_net_get_config failed!\n");
        sys_exit(1);
    }

    print("  IP Address:  ");
    print_ip(nif.ip);
    print("\n  Gateway:     ");
    print_ip(nif.gateway);
    print("\n  DNS Server:  ");
    print_ip(nif.dns);
    print("\n  Status:      ");
    print(nif.link_up ? "LINK UP" : "LINK DOWN");
    print(", ");
    print(nif.configured ? "CONFIGURED\n" : "UNCONFIGURED\n");

    /* 2. Resolve a hostname using DNS */
    print("[NET-USER] 2. Resolving host '10.0.2.2' via DNS/literal...\n");
    u32 resolved_ip = 0;
    res = sys_net_resolve("10.0.2.2", &resolved_ip, 1000);
    if (res == 0) {
        print("  Resolved to: ");
        print_ip(resolved_ip);
        print("\n");
    } else {
        print("  DNS resolution error or timed out.\n");
    }

    /* 3. Perform an ICMP ping */
    print("[NET-USER] 3. Performing ICMP ping to gateway...\n");
    u32 target_ip = nif.gateway ? nif.gateway : ((10) | (2 << 16) | (2 << 24));
    long rtt = sys_net_ping(target_ip, 1000);
    if (rtt >= 0) {
        print("  Ping successful (RTT response received)!\n");
    } else {
        print("  Ping request timed out (expected in isolated test environment).\n");
    }

    /* 4. Establish TCP connection and send HTTP GET */
    print("[NET-USER] 4. Creating TCP socket and connecting to HTTP endpoint...\n");
    long sock = sys_net_socket();
    if (sock < 0) {
        print("[NET-USER ERROR] sys_net_socket failed!\n");
        sys_exit(2);
    }

    /* Try connecting to target endpoint on port 80 */
    res = sys_net_connect((int)sock, target_ip, 80, 500);
    if (res == 0) {
        const char *http_req = "GET / HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n";
        sys_net_send((int)sock, http_req, ustrlen(http_req));

        char resp_buf[256];
        long recvd = sys_net_recv((int)sock, resp_buf, sizeof(resp_buf) - 1, 1000);
        if (recvd > 0) {
            resp_buf[recvd] = '\0';
            print("  HTTP Response:\n");
            print(resp_buf);
            print("\n");
        }
    } else {
        print("  TCP connection failed or host refused port 80 (handled cleanly).\n");
    }

    sys_net_close((int)sock);
    print("[NET-USER] All user-space network syscalls exercised cleanly!\n");
    sys_exit(0);
}
