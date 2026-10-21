#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

#include <kernel/mem.h>
#include <kernel/crypto.h>

/* Include implementations directly for host testing */
#include "../kernel/crypto/crypto_rand.c"
#include "../kernel/crypto/sha256.c"
#include "../kernel/crypto/hkdf.c"
#include "../kernel/crypto/chacha20_poly1305.c"
#include "../kernel/crypto/x25519.c"
#include "../kernel/crypto/rsa.c"
#include "../kernel/crypto/x509.c"
#include "../kernel/crypto/crypto_test.c"

#ifdef _WIN32
__declspec(dllexport)
#endif
int run_crypto_tests(void) {
    printf("[HOST TEST] Running AstraOS Cryptographic Test Suite...\n");
    if (!crypto_self_test()) {
        printf("[FAIL] Cryptographic self-test failed!\n");
        return 1;
    }
    printf("[SUCCESS] All host cryptographic tests passed!\n");
    return 0;
}
