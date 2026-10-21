#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define ASTRA_TYPES_DEFINED
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;

#include "../user/js_engine.h"

s64 sys_write(int fd, const void *buf, size_t count) {
    (void)fd; (void)buf; return count;
}

int main(void) {
    js_vm_t vm;
    js_init(&vm);
    js_value_t res;
    js_error_t err = js_eval(&vm, "$$$ syntax error", &res);
    printf("EVAL_RES=%d MSG=%s\n", (int)err, js_error_string(err));
    return 0;
}
