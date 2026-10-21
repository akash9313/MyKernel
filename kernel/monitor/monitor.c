#include <kernel/monitor.h>
#include <kernel/serial.h>
#include <kernel/framebuffer.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/heap.h>
#include <kernel/drivers/apic.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/vfs.h>
#include <kernel/wm.h>
#include <kernel/input.h>
#include <kernel/gfx.h>
#include <kernel/net.h>
#include <kernel/browser.h>

#define MONITOR_MAX_LINE 128

static int kstrcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static int kstrncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static bool parse_hex(const char *str, u64 *out_val) {
    if (!str || !*str) return false;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }
    if (!*str) return false;

    u64 val = 0;
    while (*str) {
        char c = *str;
        u64 digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;

        val = (val << 4) | digit;
        str++;
    }
    *out_val = val;
    return true;
}

static void cmd_ifconfig(void) {
    net_if_t *nif = net_get_interface();
    if (!nif) {
        kprintf("No network interface active.\n");
        return;
    }
    char ip_str[32], mask_str[32], gw_str[32], dns_str[32];
    ip_to_str(nif->ip, ip_str, sizeof(ip_str));
    ip_to_str(nif->netmask, mask_str, sizeof(mask_str));
    ip_to_str(nif->gateway, gw_str, sizeof(gw_str));
    ip_to_str(nif->dns, dns_str, sizeof(dns_str));

    kprintf("\neth0 (Intel 82540EM Gigabit Ethernet):\n");
    kprintf("  HWaddr:   %02x:%02x:%02x:%02x:%02x:%02x\n",
            nif->mac[0], nif->mac[1], nif->mac[2],
            nif->mac[3], nif->mac[4], nif->mac[5]);
    kprintf("  inet:     %s\n", ip_str);
    kprintf("  netmask:  %s\n", mask_str);
    kprintf("  gateway:  %s\n", gw_str);
    kprintf("  dns:      %s\n", dns_str);
    kprintf("  status:   %s, %s\n\n",
            nif->link_up ? "LINK UP" : "LINK DOWN",
            nif->configured ? "CONFIGURED" : "UNCONFIGURED");
}

static void cmd_ping(const char *arg) {
    while (*arg == ' ' || *arg == '\t') arg++;
    if (*arg == '\0') {
        kprintf("Usage: ping <ip-address>\n");
        return;
    }
    ip_addr_t target = 0;
    if (!str_to_ip(arg, &target)) {
        if (!dns_resolve(arg, &target, 2000)) {
            kprintf("ping: unknown host '%s'\n", arg);
            return;
        }
    }
    char ip_str[32];
    ip_to_str(target, ip_str, sizeof(ip_str));
    kprintf("PING %s (%s): 32 data bytes\n", arg, ip_str);
    for (int i = 0; i < 4; i++) {
        int rtt = icmp_ping(target, 1000);
        if (rtt >= 0) {
            kprintf("32 bytes from %s: icmp_seq=%d rtt=%d ms\n", ip_str, i + 1, rtt);
        } else {
            kprintf("Request timeout for icmp_seq %d\n", i + 1);
        }
    }
    kprintf("\n");
}

static void cmd_nslookup(const char *arg) {
    while (*arg == ' ' || *arg == '\t') arg++;
    if (*arg == '\0') {
        kprintf("Usage: nslookup <hostname>\n");
        return;
    }
    ip_addr_t out_ip = 0;
    if (dns_resolve(arg, &out_ip, 2000)) {
        char ip_str[32];
        ip_to_str(out_ip, ip_str, sizeof(ip_str));
        kprintf("Name:    %s\nAddress: %s\n\n", arg, ip_str);
    } else {
        kprintf("** server can't find %s: NXDOMAIN / Timeout\n\n", arg);
    }
}

static void cmd_http(const char *arg) {
    while (*arg == ' ' || *arg == '\t') arg++;
    if (*arg == '\0') {
        kprintf("Usage: http <host> [path]\n");
        return;
    }
    char host[64];
    size_t hi = 0;
    while (*arg && *arg != ' ' && *arg != '\t' && hi < sizeof(host) - 1) {
        host[hi++] = *arg++;
    }
    host[hi] = '\0';
    while (*arg == ' ' || *arg == '\t') arg++;
    const char *path = (*arg == '\0') ? "/" : arg;

    kprintf("Connecting to http://%s%s...\n", host, path);
    static char resp_buf[1024];
    int status = 0;
    int len = http_get(host, path, resp_buf, sizeof(resp_buf) - 1, &status);
    if (len >= 0) {
        resp_buf[len] = '\0';
        kprintf("HTTP Response Status: %d\n", status);
        kprintf("Response Data (%d bytes):\n%s\n\n", len, resp_buf);
    } else {
        kprintf("HTTP request failed: error %d\n", len);
    }
}

static void cmd_dhcp(void) {
    kprintf("Sending DHCP Discover...\n");
    if (dhcp_request_lease(3000)) {
        kprintf("DHCP lease acquired successfully!\n");
        cmd_ifconfig();
    } else {
        kprintf("DHCP lease request timed out.\n");
    }
}

static void cmd_browser(const char *arg) {
    while (*arg == ' ') arg++;
    const char *url = (*arg != '\0') ? arg : "file:///www/index.html";
    kprintf("Launching AstraBrowser: %s\n", url);
    app_browser_launch(url);
    if (gfx_is_available()) {
        desktop_render();
    }
}

static void cmd_help(void) {
    kprintf("\nAstraOS Kernel Monitor - Built-in Commands:\n");
    kprintf("  help      - Display this help menu\n");
    kprintf("  version   - Display kernel version and architecture\n");
    kprintf("  clear     - Clear framebuffer and serial screen\n");
    kprintf("  memmap    - Display boot physical memory map\n");
    kprintf("  mem       - Display physical page allocator (PMM) stats\n");
    kprintf("  heap      - Display kernel heap allocator stats\n");
    kprintf("  ticks     - Display Local APIC timer tick statistics\n");
    kprintf("  vmap <va> - Inspect active page mapping for a virtual address (hex)\n");
    kprintf("  ls [dir]  - List files and directories in VFS path\n");
    kprintf("  cat <file>- Print content of a regular file\n");
    kprintf("  ifconfig  - Display network interface configuration and link status\n");
    kprintf("  ping <ip> - Send ICMP Echo requests to target address\n");
    kprintf("  nslookup <host> - Resolve hostname using configured DNS server\n");
    kprintf("  http <host> [path] - Send HTTP 1.0 GET request and display response\n");
    kprintf("  dhcp      - Trigger DHCP discovery and configure IP stack\n");
    kprintf("  browser [url] - Launch or navigate AstraBrowser web browser\n");
    kprintf("  fault     - Trigger a deliberate CPU exception (#UD) for diagnostic testing\n");
    kprintf("  halt      - Halt the processor cleanly\n\n");
}

static void cmd_ticks(void) {
    u64 t = apic_get_ticks();
    kprintf("\n=================== APIC Timer Status ===================\n");
    kprintf("Periodic Timer Vector: %u\n", APIC_TIMER_VECTOR);
    kprintf("APIC Timer Ticks:      %llu\n", (unsigned long long)t);
    kprintf("Delivery Mode:         Periodic with EOI acknowledgment\n");
    kprintf("=========================================================\n\n");
}

static void cmd_version(void) {
    kprintf("\nAstraOS Kernel v0.2.0-dev\n");
    kprintf("Target: x86-64 freestanding (Limine protocol v3)\n");
    kprintf("Subsystems: Serial, Framebuffer, GDT, IDT, PMM, VMM, Heap, TSS, APIC\n\n");
}

static void cmd_clear(void) {
    /* Send ANSI clear to serial terminal */
    serial_puts("\033[2J\033[H");
    /* Clear graphical framebuffer */
    fb_clear(FB_COLOR_BLACK);
}

static void cmd_memmap(void) {
    memmap_print();
}

static void cmd_mem(void) {
    pmm_print_stats();
}

static void cmd_vmap(const char *arg) {
    while (*arg == ' ' || *arg == '\t') arg++;
    if (*arg == '\0') {
        kprintf("Usage: vmap <hex-virtual-address>\n");
        kprintf("Example: vmap 0xffffffff80200000\n");
        return;
    }

    u64 va = 0;
    if (!parse_hex(arg, &va)) {
        kprintf("Error: Invalid hexadecimal address '%s'\n", arg);
        return;
    }

    if (!vmm_is_canonical((virt_addr_t)va)) {
        kprintf("Address 0x%016llx is NON-CANONICAL on x86-64!\n", (unsigned long long)va);
        return;
    }

    struct vmm_mapping map;
    int res = vmm_walk_active((virt_addr_t)va, &map);
    if (res != VMM_SUCCESS) {
        kprintf("Error inspecting active page table (error %d)\n", res);
        return;
    }

    kprintf("\n================ Active Page Translation ================\n");
    kprintf("Virtual Address:     0x%016llx\n", (unsigned long long)va);
    if (!map.present) {
        kprintf("Mapping Presence:    NOT PRESENT (Unmapped)\n");
        kprintf("=========================================================\n\n");
        return;
    }

    const char *sz_str = "4 KiB";
    if (map.page_size == 1024ULL * 1024ULL * 1024ULL) sz_str = "1 GiB";
    else if (map.page_size == 2ULL * 1024ULL * 1024ULL) sz_str = "2 MiB";

    const char *lvl_str = "Level 1 (PT)";
    if (map.level == 2) lvl_str = "Level 2 (PD)";
    else if (map.level == 3) lvl_str = "Level 3 (PDPT)";

    kprintf("Mapping Presence:    PRESENT\n");
    kprintf("Physical Address:    0x%016llx\n", (unsigned long long)map.paddr);
    kprintf("Page Size:           %s (%llu bytes)\n", sz_str, (unsigned long long)map.page_size);
    kprintf("Paging Level:        %s\n", lvl_str);
    kprintf("Effective Perms:     %s | %s | %s\n",
            map.user ? "User (Ring 3)" : "Supervisor (Ring 0)",
            map.writable ? "Read/Write" : "Read-Only",
            map.no_exec ? "NX (No-Execute)" : "Executable (X)");
    kprintf("High-Level Flags:    0x%x (R%s%s%s)\n",
            map.flags,
            map.writable ? "W" : "",
            map.user ? "U" : "",
            map.no_exec ? " NX" : " X");
    kprintf("=========================================================\n\n");
}

static void cmd_ls(const char *arg) {
    while (*arg == ' ' || *arg == '\t') arg++;
    const char *path = (*arg == '\0') ? "/" : arg;

    kprintf("\nDirectory listing for '%s':\n", path);
    kprintf("%s\t%s\t%s\n", "TYPE", "SIZE", "NAME");
    kprintf("----------------------------------------\n");

    vfs_dirent_t dent;
    size_t idx = 0;
    int count = 0;
    while (vfs_readdir(path, idx, &dent) == VFS_OK) {
        const char *tstr = (dent.type == VFS_NODE_DIR) ? "<DIR>" : "<FILE>";
        kprintf("%s\t%u bytes\t%s\n", tstr, (unsigned)dent.size, dent.name);
        count++;
        idx++;
    }
    kprintf("----------------------------------------\n");
    kprintf("Total: %d entries\n\n", count);
}

static void cmd_cat(const char *arg) {
    while (*arg == ' ' || *arg == '\t') arg++;
    if (*arg == '\0') {
        kprintf("Usage: cat <file-path>\n");
        return;
    }

    int fd = vfs_open(arg, VFS_O_RDONLY);
    if (fd < 0) {
        kprintf("cat: cannot open '%s': %s (err %d)\n", arg, vfs_strerror(fd), fd);
        return;
    }

    vfs_stat_t st;
    int sres = vfs_fstat(fd, &st);
    if (sres != VFS_OK) {
        kprintf("cat: failed to stat '%s': %s\n", arg, vfs_strerror(sres));
        vfs_close(fd);
        return;
    }

    if (st.type == VFS_NODE_DIR) {
        kprintf("cat: '%s': Is a directory\n", arg);
        vfs_close(fd);
        return;
    }

    char buf[128];
    ssize_t n;
    while ((n = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        kprintf("%s", buf);
    }
    vfs_close(fd);
    kprintf("\n");
}

static void cmd_fault(void) {
    kprintf("\n[DIAG] Triggering deliberate UD2 exception on operator request...\n");
    __asm__ volatile ("ud2");
}

static void cmd_halt(void) {
    kprintf("\nHalting system CPU on operator request...\n");
    kprintf("CPU halted. Power off or reset VM.\n");
    halt_forever();
}

static void monitor_execute(char *line) {
    /* Trim leading spaces */
    while (*line == ' ' || *line == '\t') {
        line++;
    }

    /* Trim trailing spaces */
    int len = 0;
    while (line[len]) len++;
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[len - 1] = '\0';
        len--;
    }

    if (len == 0) {
        return;
    }

    if (kstrcmp(line, "help") == 0) {
        cmd_help();
    } else if (kstrcmp(line, "version") == 0) {
        cmd_version();
    } else if (kstrcmp(line, "clear") == 0) {
        cmd_clear();
    } else if (kstrcmp(line, "memmap") == 0) {
        cmd_memmap();
    } else if (kstrcmp(line, "mem") == 0) {
        cmd_mem();
    } else if (kstrcmp(line, "heap") == 0) {
        heap_print_stats();
    } else if (kstrcmp(line, "ticks") == 0) {
        cmd_ticks();
    } else if (kstrncmp(line, "vmap", 4) == 0 && (line[4] == ' ' || line[4] == '\0')) {
        cmd_vmap(line + 4);
    } else if (kstrncmp(line, "ls", 2) == 0 && (line[2] == ' ' || line[2] == '\0')) {
        cmd_ls(line + 2);
    } else if (kstrncmp(line, "cat", 3) == 0 && (line[3] == ' ' || line[3] == '\0')) {
        cmd_cat(line + 3);
    } else if (kstrcmp(line, "ifconfig") == 0) {
        cmd_ifconfig();
    } else if (kstrncmp(line, "ping", 4) == 0 && (line[4] == ' ' || line[4] == '\0')) {
        cmd_ping(line + 4);
    } else if (kstrncmp(line, "nslookup", 8) == 0 && (line[8] == ' ' || line[8] == '\0')) {
        cmd_nslookup(line + 8);
    } else if (kstrncmp(line, "http", 4) == 0 && (line[4] == ' ' || line[4] == '\0')) {
        cmd_http(line + 4);
    } else if (kstrcmp(line, "dhcp") == 0) {
        cmd_dhcp();
    } else if (kstrncmp(line, "browser", 7) == 0 && (line[7] == ' ' || line[7] == '\0')) {
        cmd_browser(line + 7);
    } else if (kstrcmp(line, "desktop") == 0) {
        if (gfx_is_available()) {
            desktop_render();
            kprintf("Desktop presentation updated.\n");
        } else {
            kprintf("Graphics subsystem not available.\n");
        }
    } else if (kstrcmp(line, "fault") == 0) {
        cmd_fault();
    } else if (kstrcmp(line, "halt") == 0) {
        cmd_halt();
    } else {
        kprintf("Unknown command: '%s'. Type 'help' for available commands.\n", line);
    }
}

void monitor_run(void) {
    char buf[MONITOR_MAX_LINE];
    int pos = 0;

    kprintf("\nAstraOS Kernel Monitor (COM1 Serial)\n");
    kprintf("Type 'help' for available commands.\n\n");
    kprintf("AstraOS>\n");

    bool last_was_cr = false;
    for (;;) {
        /* 1. Poll PS/2 keyboard/mouse hardware */
        ps2_poll();

        /* 1b. Poll network packet reception */
        net_poll();

        /* 2. Process all pending input events through GUI desktop session */
        input_event_t ev;
        bool need_render = false;
        while (input_pop_event(&ev)) {
            desktop_handle_event(&ev);
            need_render = true;
        }
        if (need_render && !serial_has_rx()) {
            desktop_render();
        }

        /* 3. Check for COM1 serial port character */
        if (!serial_has_rx()) {
            io_wait();
            continue;
        }

        char c = serial_getchar();

        if (c == '\r') {
            serial_putchar('\n');
            fb_putchar('\n');
            buf[pos] = '\0';
            if (pos > 0) {
                monitor_execute(buf);
                pos = 0;
            }
            kprintf("AstraOS>\n");
            last_was_cr = true;
        } else if (c == '\n') {
            if (last_was_cr) {
                last_was_cr = false;
                continue;
            }
            serial_putchar('\n');
            fb_putchar('\n');
            buf[pos] = '\0';
            if (pos > 0) {
                monitor_execute(buf);
                pos = 0;
            }
            kprintf("AstraOS>\n");
            last_was_cr = false;
        } else if (c == '\b' || c == 0x7F) {
            last_was_cr = false;
            if (pos > 0) {
                pos--;
                serial_puts("\b \b");
                fb_putchar('\b');
            }
        } else if (pos < MONITOR_MAX_LINE - 1 && c >= 32 && c <= 126) {
            last_was_cr = false;
            buf[pos++] = c;
            serial_putchar(c);
            fb_putchar(c);
        }
    }
}
