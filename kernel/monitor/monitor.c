#include <kernel/monitor.h>
#include <kernel/serial.h>
#include <kernel/framebuffer.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/pmm.h>
#include <kernel/arch/x86_64/cpu.h>

#define MONITOR_MAX_LINE 128

static int kstrcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static void cmd_help(void) {
    kprintf("\nAstraOS Kernel Monitor - Built-in Commands:\n");
    kprintf("  help     - Display this help menu\n");
    kprintf("  version  - Display kernel version and architecture\n");
    kprintf("  clear    - Clear framebuffer and serial screen\n");
    kprintf("  memmap   - Display boot physical memory map\n");
    kprintf("  mem      - Display physical page allocator (PMM) stats\n");
    kprintf("  fault    - Trigger a deliberate CPU exception (#UD) for diagnostic testing\n");
    kprintf("  halt     - Halt the processor cleanly\n\n");
}

static void cmd_version(void) {
    kprintf("\nAstraOS Kernel v0.2.0-dev\n");
    kprintf("Target: x86-64 freestanding (Limine protocol v3)\n");
    kprintf("Subsystems: Serial, Framebuffer, GDT, IDT, PMM Bitmap Allocator\n\n");
}

static void cmd_clear(void) {
    /* Send ANSI clear to serial terminal */
    serial_puts("\033[2J\033[H");
    /* Clear graphical framebuffer */
    fb_clear(FB_COLOR_BLACK);
}

static void cmd_memmap(void) {
    memmap_print();
}

static void cmd_mem(void) {
    pmm_print_stats();
}

static void cmd_fault(void) {
    kprintf("\n[DIAG] Triggering deliberate UD2 exception on operator request...\n");
    __asm__ volatile ("ud2");
}

static void cmd_halt(void) {
    kprintf("\nHalting system CPU on operator request...\n");
    kprintf("CPU halted. Power off or reset VM.\n");
    halt_forever();
}

static void monitor_execute(char *line) {
    /* Trim leading spaces */
    while (*line == ' ' || *line == '\t') {
        line++;
    }

    /* Trim trailing spaces */
    int len = 0;
    while (line[len]) len++;
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[len - 1] = '\0';
        len--;
    }

    if (len == 0) {
        return;
    }

    if (kstrcmp(line, "help") == 0) {
        cmd_help();
    } else if (kstrcmp(line, "version") == 0) {
        cmd_version();
    } else if (kstrcmp(line, "clear") == 0) {
        cmd_clear();
    } else if (kstrcmp(line, "memmap") == 0) {
        cmd_memmap();
    } else if (kstrcmp(line, "mem") == 0) {
        cmd_mem();
    } else if (kstrcmp(line, "fault") == 0) {
        cmd_fault();
    } else if (kstrcmp(line, "halt") == 0) {
        cmd_halt();
    } else {
        kprintf("Unknown command: '%s'. Type 'help' for available commands.\n", line);
    }
}

void monitor_run(void) {
    char buf[MONITOR_MAX_LINE];
    int pos = 0;

    kprintf("\nAstraOS Kernel Monitor (COM1 Serial)\n");
    kprintf("Type 'help' for available commands.\n\n");
    kprintf("AstraOS> ");

    for (;;) {
        char c = serial_getchar();

        if (c == '\r' || c == '\n') {
            serial_putchar('\n');
            fb_putchar('\n');
            buf[pos] = '\0';
            monitor_execute(buf);
            pos = 0;
            kprintf("AstraOS> ");
        } else if (c == '\b' || c == 0x7F) {
            if (pos > 0) {
                pos--;
                serial_puts("\b \b");
                fb_putchar('\b');
            }
        } else if (pos < MONITOR_MAX_LINE - 1 && c >= 32 && c <= 126) {
            buf[pos++] = c;
            serial_putchar(c);
            fb_putchar(c);
        }
    }
}
