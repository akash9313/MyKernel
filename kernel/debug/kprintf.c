#include <kernel/kprintf.h>
#include <kernel/serial.h>
#include <kernel/framebuffer.h>
#include <kernel/sync.h>

static spinlock_t g_kprintf_lock = SPINLOCK_INIT;

static void out_char(char c) {
    serial_putchar(c);
    fb_putchar(c);
}

static void print_str(const char *s) {
    if (!s) s = "(null)";
    while (*s) {
        out_char(*s++);
    }
}

static void print_uint(u64 val, int base, bool uppercase, int min_width, char pad_char) {
    char buf[65];
    int pos = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0) {
        buf[pos++] = '0';
    } else {
        while (val > 0) {
            buf[pos++] = digits[val % base];
            val /= base;
        }
    }

    while (pos < min_width && pos < 64) {
        buf[pos++] = pad_char;
    }

    for (int i = pos - 1; i >= 0; i--) {
        out_char(buf[i]);
    }
}

static void print_int(s64 val) {
    if (val < 0) {
        out_char('-');
        print_uint((u64)(-val), 10, false, 0, ' ');
    } else {
        print_uint((u64)val, 10, false, 0, ' ');
    }
}

void kvprintf(const char *fmt, va_list args) {
    if (!fmt) return;
    u64 flags = spinlock_acquire_irqsave(&g_kprintf_lock);

    for (size_t i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            out_char(fmt[i]);
            continue;
        }

        i++;
        if (fmt[i] == '\0') break;

        char pad_char = ' ';
        int width = 0;

        if (fmt[i] == '0') {
            pad_char = '0';
            i++;
        }

        while (fmt[i] >= '0' && fmt[i] <= '9') {
            width = width * 10 + (fmt[i] - '0');
            i++;
        }

        bool is_long = false;
        if (fmt[i] == 'l') {
            is_long = true;
            i++;
            if (fmt[i] == 'l') {
                i++;
            }
        }

        switch (fmt[i]) {
            case 'c': {
                char c = (char)va_arg(args, int);
                out_char(c);
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                print_str(s);
                break;
            }
            case 'd':
            case 'i': {
                s64 v = is_long ? va_arg(args, s64) : (s64)va_arg(args, int);
                print_int(v);
                break;
            }
            case 'u': {
                u64 v = is_long ? va_arg(args, u64) : (u64)va_arg(args, unsigned int);
                print_uint(v, 10, false, width, pad_char);
                break;
            }
            case 'x': {
                u64 v = is_long ? va_arg(args, u64) : (u64)va_arg(args, unsigned int);
                print_uint(v, 16, false, width, pad_char);
                break;
            }
            case 'X': {
                u64 v = is_long ? va_arg(args, u64) : (u64)va_arg(args, unsigned int);
                print_uint(v, 16, true, width, pad_char);
                break;
            }
            case 'p': {
                u64 ptr = (u64)va_arg(args, void *);
                print_str("0x");
                print_uint(ptr, 16, false, 16, '0');
                break;
            }
            case '%': {
                out_char('%');
                break;
            }
            default: {
                out_char('%');
                out_char(fmt[i]);
                break;
            }
        }
    }
    spinlock_release_irqrestore(&g_kprintf_lock, flags);
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
}

typedef struct {
    char *buf;
    size_t size;
    size_t pos;
} sn_ctx_t;

static void sn_putc(sn_ctx_t *ctx, char c) {
    if (ctx->pos + 1 < ctx->size) {
        ctx->buf[ctx->pos++] = c;
    }
}

static void sn_puts(sn_ctx_t *ctx, const char *s) {
    if (!s) s = "(null)";
    while (*s && ctx->pos + 1 < ctx->size) {
        ctx->buf[ctx->pos++] = *s++;
    }
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    if (!buf || size == 0) return 0;
    sn_ctx_t ctx = { .buf = buf, .size = size, .pos = 0 };

    for (size_t i = 0; fmt[i] != '\0'; i++) {
        if (fmt[i] != '%') {
            sn_putc(&ctx, fmt[i]);
            continue;
        }

        i++;
        if (fmt[i] == '\0') break;

        char pad_char = ' ';
        int width = 0;
        bool left_align = false;
        if (fmt[i] == '-') {
            left_align = true;
            i++;
        }
        if (fmt[i] == '0') {
            pad_char = '0';
            i++;
        }

        while (fmt[i] >= '0' && fmt[i] <= '9') {
            width = width * 10 + (fmt[i] - '0');
            i++;
        }

        bool is_long = false;
        if (fmt[i] == 'l') {
            is_long = true;
            i++;
            if (fmt[i] == 'l') i++;
        }

        switch (fmt[i]) {
            case 'c': {
                char c = (char)va_arg(args, int);
                sn_putc(&ctx, c);
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                size_t slen = 0;
                while (s[slen]) slen++;
                if (!left_align && width > (int)slen) {
                    for (int p = 0; p < width - (int)slen; p++) sn_putc(&ctx, pad_char);
                }
                sn_puts(&ctx, s);
                if (left_align && width > (int)slen) {
                    for (int p = 0; p < width - (int)slen; p++) sn_putc(&ctx, ' ');
                }
                break;
            }
            case 'd':
            case 'i': {
                s64 v = is_long ? va_arg(args, s64) : (s64)va_arg(args, int);
                char num_buf[32];
                int np = 0;
                bool neg = false;
                if (v < 0) { neg = true; v = -v; }
                if (v == 0) num_buf[np++] = '0';
                else {
                    while (v > 0) { num_buf[np++] = '0' + (v % 10); v /= 10; }
                }
                if (neg) num_buf[np++] = '-';
                int total_len = np;
                if (!left_align && width > total_len) {
                    for (int p = 0; p < width - total_len; p++) sn_putc(&ctx, pad_char);
                }
                for (int p = np - 1; p >= 0; p--) sn_putc(&ctx, num_buf[p]);
                if (left_align && width > total_len) {
                    for (int p = 0; p < width - total_len; p++) sn_putc(&ctx, ' ');
                }
                break;
            }
            case 'u': {
                u64 v = is_long ? va_arg(args, u64) : (u64)va_arg(args, unsigned int);
                char num_buf[32];
                int np = 0;
                if (v == 0) num_buf[np++] = '0';
                else {
                    while (v > 0) { num_buf[np++] = '0' + (v % 10); v /= 10; }
                }
                int total_len = np;
                if (!left_align && width > total_len) {
                    for (int p = 0; p < width - total_len; p++) sn_putc(&ctx, pad_char);
                }
                for (int p = np - 1; p >= 0; p--) sn_putc(&ctx, num_buf[p]);
                if (left_align && width > total_len) {
                    for (int p = 0; p < width - total_len; p++) sn_putc(&ctx, ' ');
                }
                break;
            }
            case 'x': {
                u64 v = is_long ? va_arg(args, u64) : (u64)va_arg(args, unsigned int);
                const char *hex = "0123456789abcdef";
                char num_buf[32];
                int np = 0;
                if (v == 0) num_buf[np++] = '0';
                else {
                    while (v > 0) { num_buf[np++] = hex[v % 16]; v /= 16; }
                }
                int total_len = np;
                if (!left_align && width > total_len) {
                    for (int p = 0; p < width - total_len; p++) sn_putc(&ctx, pad_char);
                }
                for (int p = np - 1; p >= 0; p--) sn_putc(&ctx, num_buf[p]);
                if (left_align && width > total_len) {
                    for (int p = 0; p < width - total_len; p++) sn_putc(&ctx, ' ');
                }
                break;
            }
            case '%': {
                sn_putc(&ctx, '%');
                break;
            }
            default: {
                sn_putc(&ctx, '%');
                sn_putc(&ctx, fmt[i]);
                break;
            }
        }
    }

    ctx.buf[ctx.pos] = '\0';
    return (int)ctx.pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = kvsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}
