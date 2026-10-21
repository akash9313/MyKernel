/*
 * AstraOS Native User Program (ELF64 Freestanding)
 *
 * Requirements:
 * 1. Start from ELF entry point in CPL3.
 * 2. Call getpid and verify that it receives a valid PID.
 * 3. Use write to print a recognizable message.
 * 4. Exercise zero-initialized data (.bss).
 * 5. Call yield.
 * 6. Exit with a known status using exit syscall.
 */

typedef unsigned long size_t;

long sys_write(int fd, const void *buf, size_t count);
long sys_yield(void);
long sys_getpid(void);
void sys_exit(int status) __attribute__((noreturn));

/* Global initialized data (.data / .rodata) */
static const char g_banner[] = "AstraOS Native ELF: Executing in Ring 3 CPL=3!\n";
static const char g_bss_ok[] = "AstraOS Native ELF: .bss zero-initialization verified!\n";
static const char g_done[]   = "AstraOS Native ELF: Syscalls verified, calling exit(0)...\n";

/* Global uninitialized data (.bss) */
static volatile int g_bss_canary_1;
static volatile int g_bss_canary_2;

static size_t kstrlen(const char *s) {
    size_t len = 0;
    while (s[len] != '\0') len++;
    return len;
}

int main(void) {
    /* 1. Verify getpid */
    long pid = sys_getpid();
    if (pid <= 0) {
        sys_exit(101); /* Error: invalid PID */
    }

    /* 2. Verify .bss was zero-initialized */
    if (g_bss_canary_1 != 0 || g_bss_canary_2 != 0) {
        sys_exit(102); /* Error: .bss not zeroed */
    }

    /* Modify .bss variables and verify write/read */
    g_bss_canary_1 = 0x12345678;
    g_bss_canary_2 = 0x55AA33CC;
    if (g_bss_canary_1 != 0x12345678 || g_bss_canary_2 != 0x55AA33CC) {
        sys_exit(103); /* Error: .bss memory corruption */
    }

    /* 3. Use write syscall to output banners */
    sys_write(1, g_banner, kstrlen(g_banner));
    sys_write(1, g_bss_ok, kstrlen(g_bss_ok));

    /* 4. Voluntary yield */
    long y = sys_yield();
    if (y != 0) {
        sys_exit(104); /* Error: yield failed */
    }

    sys_write(1, g_done, kstrlen(g_done));

    /* 5. Exit cleanly with status 0 */
    sys_exit(0);
    return 0;
}
