#ifndef ASTRA_KERNEL_PANIC_H
#define ASTRA_KERNEL_PANIC_H

#include <kernel/types.h>

#define panic(msg, ...) kernel_panic(__FILE__, __LINE__, msg, ##__VA_ARGS__)

void kernel_panic(const char *file, int line, const char *fmt, ...);

#endif /* ASTRA_KERNEL_PANIC_H */
