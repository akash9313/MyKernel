#include <stdint.h>
#include <stddef.h>

extern long sys_write(int fd, const void *buf, size_t count);
extern long sys_getpid(void);
extern void sys_exit(int status) __attribute__((noreturn));

static void print_str(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);
}

int main(void) {
    print_str("[FAULT APP] Starting deliberate supervisor memory access...\n");

    /* Deliberate access to supervisor higher-half virtual address */
    volatile uint64_t *bad_ptr = (volatile uint64_t *)0xffffffff80400000ULL;
    uint64_t val = *bad_ptr;
    (void)val;

    /* Should never reach here */
    print_str("[FAULT APP] ERROR: Supervisor access did not fault!\n");
    return 42;
}

