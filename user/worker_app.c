#include <stdint.h>
#include <stddef.h>

extern long sys_write(int fd, const void *buf, size_t count);
extern long sys_yield(void);
extern long sys_getpid(void);
extern void sys_exit(int status) __attribute__((noreturn));

static void print_str(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);
}

int main(void) {
    long pid = sys_getpid();
    print_str("[WORKER APP] Started background worker loop. PID=");
    char c = (char)('0' + (pid % 10));
    sys_write(1, &c, 1);
    print_str("\n");

    /* Perform 50 rounds of CPU work interleaved with sys_yield() */
    volatile long counter = 0;
    for (int round = 0; round < 50; round++) {
        for (int i = 0; i < 1000; i++) {
            counter += i;
        }
        sys_yield();
    }

    print_str("[WORKER APP] Completed 50 interleaved work cycles with status 0.\n");
    return 0;
}

