#include <kernel/serial.h>
#include <kernel/arch/x86_64/cpu.h>

#define UART_DATA(base)          ((base) + 0)
#define UART_IER(base)           ((base) + 1)
#define UART_BAUD_LO(base)       ((base) + 0)
#define UART_BAUD_HI(base)       ((base) + 1)
#define UART_FCR(base)           ((base) + 2)
#define UART_LCR(base)           ((base) + 3)
#define UART_MCR(base)           ((base) + 4)
#define UART_LSR(base)           ((base) + 5)
#define UART_MSR(base)           ((base) + 6)
#define UART_SCRATCH(base)       ((base) + 7)

#define LSR_DATA_READY           0x01
#define LSR_TRANSMITTER_EMPTY    0x20

static int serial_initialized = 0;

int serial_init(void) {
    u16 port = COM1_PORT;

    /* Disable all interrupts */
    outb(UART_IER(port), 0x00);

    /* Enable DLAB (set baud rate divisor) */
    outb(UART_LCR(port), 0x80);

    /* Set divisor to 1 (115200 baud) */
    outb(UART_BAUD_LO(port), 0x01);
    outb(UART_BAUD_HI(port), 0x00);

    /* 8 bits, no parity, one stop bit */
    outb(UART_LCR(port), 0x03);

    /* Enable FIFO, clear TX/RX queues, 14-byte threshold */
    outb(UART_FCR(port), 0xC7);

    /* Enable auxiliary output 2, RTS, DTR */
    outb(UART_MCR(port), 0x0B);

    /* Test serial chip with loopback mode */
    outb(UART_MCR(port), 0x1E);
    outb(UART_DATA(port), 0xAE);
    io_wait();

    bool loopback_ok = (inb(UART_DATA(port)) == 0xAE);
    (void)loopback_ok;

    /* Set normal operational mode (no loopback, IRQs enabled in MCR) */
    outb(UART_MCR(port), 0x0F);
    serial_initialized = 1;
    return 0;
}

static inline bool serial_is_transmit_empty(void) {
    return (inb(UART_LSR(COM1_PORT)) & LSR_TRANSMITTER_EMPTY) != 0;
}

void serial_putchar(char c) {
    if (!serial_initialized) {
        return;
    }

    /* LF to CRLF translation for serial terminals */
    if (c == '\n') {
        while (!serial_is_transmit_empty()) {
            io_wait();
        }
        outb(UART_DATA(COM1_PORT), '\r');
    }

    while (!serial_is_transmit_empty()) {
        io_wait();
    }
    outb(UART_DATA(COM1_PORT), (u8)c);
}

void serial_puts(const char *s) {
    if (!s) return;
    while (*s) {
        serial_putchar(*s++);
    }
}

bool serial_has_rx(void) {
    if (!serial_initialized) return false;
    return (inb(UART_LSR(COM1_PORT)) & LSR_DATA_READY) != 0;
}

char serial_getchar(void) {
    while (!serial_has_rx()) {
        io_wait();
    }
    return (char)inb(UART_DATA(COM1_PORT));
}
