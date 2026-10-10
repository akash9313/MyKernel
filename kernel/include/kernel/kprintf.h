#ifndef ASTRA_KERNEL_KPRINTF_H
#define ASTRA_KERNEL_KPRINTF_H

#include <kernel/types.h>
#include <stdarg.h>

void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list args);

#endif /* ASTRA_KERNEL_KPRINTF_H */
