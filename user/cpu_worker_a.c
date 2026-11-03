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
    print_str("[CPU WORKER A] Started on CPU ");
    print_num(cpu);
    print_str(" (PID ");
    print_num((uint32_t)sys_getpid());
    print_str(")...\n");

    volatile uint64_t acc = 0xAAAAAAAA55555555ULL;
    for (uint64_t i = 1; i <= 20000000ULL; i++) {
        acc = (acc * 6364136223846793005ULL) + i;
        if (i % 5000000ULL == 0) {
            uint32_t cur_cpu = sys_getcpuid();
            print_str("[CPU WORKER A] Iteration ");
            print_num((uint32_t)(i / 1000000ULL));
            print_str("M on CPU ");
            print_num(cur_cpu);
            print_str("\n");
        }
    }

    if (acc == 0) {
        print_str("[CPU WORKER A] Error: Unexpected zero accumulator!\n");
        return 1;
    }

    print_str("[CPU WORKER A] Completed CPU-bound workload successfully.\n");
    return 0;
}

