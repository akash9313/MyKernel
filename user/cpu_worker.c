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
    print_str("[CPU WORKER] Started busy loop without sys_yield()...\n");

    /*
     * Tight CPU-bound arithmetic loop without any cooperative yields.
     * Only timer-driven interrupt preemption will allow other threads to run.
     */
    volatile uint64_t acc = 0x12345678ULL;
    for (uint64_t i = 1; i <= 30000000ULL; i++) {
        acc = (acc * 6364136223846793005ULL) + i;
    }

    if (acc == 0) {
        print_str("[CPU WORKER] Error: Unexpected zero accumulator!\n");
        return 1;
    }

    print_str("[CPU WORKER] Completed CPU-bound workload without cooperative yielding.\n");
    return 0;
}

