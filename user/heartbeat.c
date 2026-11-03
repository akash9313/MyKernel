#include <stdint.h>
#include <stddef.h>

extern long sys_write(int fd, const void *buf, size_t count);
extern long sys_getpid(void);
extern unsigned int sys_getcpuid(void);
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
    print_str("[HEARTBEAT] Progress monitor started on CPU ");
    print_num(cpu);
    print_str(" (PID ");
    print_num((uint32_t)sys_getpid());
    print_str(")...\n");

    for (int tick = 1; tick <= 10; tick++) {
        uint32_t cur_cpu = sys_getcpuid();
        print_str("[HEARTBEAT] Progress tick ");
        char c = (char)('0' + (tick % 10));
        sys_write(1, &c, 1);
        print_str(" on CPU ");
        print_num(cur_cpu);
        print_str("\n");

        /* Bounded compute interval between heartbeat ticks */
        volatile uint64_t delay = 0;
        for (uint64_t i = 0; i < 500000ULL; i++) {
            delay += i;
        }
    }

    print_str("[HEARTBEAT] Finished 10 heartbeat progress ticks.\n");
    return 0;
}
