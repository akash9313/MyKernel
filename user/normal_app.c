#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

extern long sys_write(int fd, const void *buf, size_t count);
extern long sys_yield(void);
extern long sys_getpid(void);
extern void sys_exit(int status) __attribute__((noreturn));
extern long sys_open(const char *path, int flags);
extern long sys_read(int fd, void *buf, size_t count);
extern long sys_close(int fd);

static void print_str(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    sys_write(1, s, len);
}

static void print_num(long val) {
    char buf[32];
    int idx = 0;
    if (val == 0) {
        print_str("0");
        return;
    }
    if (val < 0) {
        print_str("-");
        val = -val;
    }
    while (val > 0 && idx < (int)sizeof(buf) - 1) {
        buf[idx++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (int i = 0; i < idx / 2; i++) {
        char tmp = buf[i];
        buf[i] = buf[idx - 1 - i];
        buf[idx - 1 - i] = tmp;
    }
    buf[idx] = '\0';
    print_str(buf);
}

int main(void) {
    long pid = sys_getpid();
    print_str("[NORMAL APP] Started successfully! PID=");
    print_num(pid);
    print_str("\n");

    /* 1. Arithmetic computation: Sum of 1..100 = 5050 */
    long sum = 0;
    for (long i = 1; i <= 100; i++) {
        sum += i;
    }

    if (sum != 5050) {
        print_str("[NORMAL APP] Computation check failed!\n");
        return 1;
    }
    print_str("[NORMAL APP] Computation verified: sum(1..100) = ");
    print_num(sum);
    print_str("\n");

    /* 2. File descriptor operations and ownership */
    long fd = sys_open("/normal_out.txt", 0x202 /* O_CREAT | O_RDWR */);
    if (fd < 0) {
        fd = sys_open("/data/normal_out.txt", 0x202 /* O_CREAT | O_RDWR */);
    }
    if (fd >= 3) {
        const char *msg = "NORMAL_APP_PAYLOAD_PID_ISOLATION";
        size_t mlen = 32;
        sys_write((int)fd, msg, mlen);
        sys_close((int)fd);
        print_str("[NORMAL APP] File write and descriptor lifecycle verified.\n");
    }

    print_str("[NORMAL APP] Exiting with status 0.\n");
    return 0;
}
