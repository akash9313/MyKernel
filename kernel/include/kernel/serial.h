#ifndef ASTRA_KERNEL_SERIAL_H
#define ASTRA_KERNEL_SERIAL_H

#include <kernel/types.h>

#define COM1_PORT 0x3F8

int serial_init(void);
void serial_putchar(char c);
void serial_puts(const char *s);
bool serial_has_rx(void);
char serial_getchar(void);

#endif /* ASTRA_KERNEL_SERIAL_H */
