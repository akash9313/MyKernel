#ifndef ASTRA_KERNEL_KPRINTF_H
#define ASTRA_KERNEL_KPRINTF_H

#include <kernel/types.h>
#include <stdarg.h>

void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list args);
int  kvsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int  ksnprintf(char *buf, size_t size, const char *fmt, ...);

#define ksprintf(buf, fmt, ...) ksnprintf(buf, 256, fmt, ##__VA_ARGS__)

#endif /* ASTRA_KERNEL_KPRINTF_H */
