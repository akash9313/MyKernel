#include <kernel/panic.h>
#include <kernel/kprintf.h>
#include <kernel/arch/x86_64/cpu.h>
#include <stdarg.h>

void kernel_panic(const char *file, int line, const char *fmt, ...) {
    cli();

    kprintf("\n==================== [ KERNEL PANIC ] ====================\n");
    kprintf("Location: %s:%d\n", file, line);
    kprintf("Reason:   ");

    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);

    kprintf("\n");
    kprintf("CR2 (Fault Addr): 0x%016llx\n", (unsigned long long)read_cr2());
    kprintf("CR3 (Page Table): 0x%016llx\n", (unsigned long long)read_cr3());
    kprintf("System halted. Please reboot the machine.\n");
    kprintf("==========================================================\n");

    halt_forever();
}
