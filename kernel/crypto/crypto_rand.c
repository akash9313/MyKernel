#include <kernel/crypto.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/kprintf.h>

static int g_has_rdrand = -1;

bool crypto_has_secure_entropy(void) {
    if (g_has_rdrand != -1) {
        return (g_has_rdrand == 1);
    }

    u32 eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);

    /* Leaf 1, ECX bit 30 = RDRAND feature flag */
    if ((ecx & (1U << 30)) != 0) {
        /* Run a live verification test of rdrand */
        u64 val = 0;
        u8 ok = 0;
        for (int i = 0; i < 10; i++) {
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
            if (ok) break;
        }

        if (ok) {
            g_has_rdrand = 1;
            kprintf("[CRYPTO] Hardware RDRAND entropy source detected and active.\n");
            return true;
        }
    }

    g_has_rdrand = 0;
    kprintf("[CRYPTO WARN] Hardware RDRAND entropy source not available!\n");
    return false;
}

bool crypto_get_random(void *buf, size_t len) {
    if (!buf || len == 0) return false;

    if (!crypto_has_secure_entropy()) {
        /* Fail closed: reject generating insecure random data */
        return false;
    }

    u8 *p = (u8 *)buf;
    while (len >= 8) {
        u64 val = 0;
        u8 ok = 0;
        for (int retry = 0; retry < 10; retry++) {
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
            if (ok) break;
        }
        if (!ok) return false;

        for (int i = 0; i < 8; i++) {
            p[i] = (u8)(val >> (i * 8));
        }
        p += 8;
        len -= 8;
    }

    if (len > 0) {
        u64 val = 0;
        u8 ok = 0;
        for (int retry = 0; retry < 10; retry++) {
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
            if (ok) break;
        }
        if (!ok) return false;

        for (size_t i = 0; i < len; i++) {
            p[i] = (u8)(val >> (i * 8));
        }
    }

    return true;
}

bool crypto_memcmp_eq(const void *a, const void *b, size_t len) {
    if (!a || !b) return false;
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;
    u8 diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (pa[i] ^ pb[i]);
    }
    return (diff == 0);
}
