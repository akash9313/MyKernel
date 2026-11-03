#include <stdint.h>
#include <stddef.h>

extern long sys_write(int fd, const void *buf, size_t count);
extern long sys_getpid(void);
extern unsigned int sys_getcpuid(void);
extern long sys_sleep(unsigned int ms);
extern void sys_exit(int status) __attribute__((noreturn));

static void print_str(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);
}

static void print_num(uint32_t val) {
    char buf[16];
    int idx = 0;
    if (val == 0) {
        buf[idx++] = '0';
    } else {
        char temp[16];
        int t_idx = 0;
        while (val > 0) {
            temp[t_idx++] = (char)('0' + (val % 10));
            val /= 10;
        }
        while (t_idx > 0) {
            buf[idx++] = temp[--t_idx];
        }
    }
    buf[idx] = '\0';
    print_str(buf);
}

int main(void) {
    uint32_t cpu = sys_getcpuid();
    print_str("[BLOCKING WORKER] Started on CPU ");
    print_num(cpu);
    print_str(" (PID ");
    print_num((uint32_t)sys_getpid());
    print_str("); entering blocking sleep for 40ms...\n");

    /* Invokes sys_sleep which transitions this thread to THREAD_BLOCKED */
    long res = sys_sleep(40);
    if (res != 0) {
        print_str("[BLOCKING WORKER] sys_sleep returned error!\n");
        return 1;
    }

    uint32_t wake_cpu = sys_getcpuid();
    print_str("[BLOCKING WORKER] Awakened from sleep on CPU ");
    print_num(wake_cpu);
    print_str("; exiting with 0.\n");
    return 0;
}
