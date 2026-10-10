#include <kernel/types.h>
#include <kernel/boot.h>
#include <kernel/serial.h>
#include <kernel/framebuffer.h>
#include <kernel/kprintf.h>
#include <kernel/panic.h>
#include <kernel/mem.h>
#include <kernel/pmm.h>
#include <kernel/monitor.h>
#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/arch/x86_64/cpu.h>

static bool kstrstr_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    for (size_t i = 0; haystack[i] != '\0'; i++) {
        size_t j = 0;
        while (needle[j] != '\0' && haystack[i + j] == needle[j]) {
            j++;
        }
        if (needle[j] == '\0') {
            return true;
        }
    }
    return false;
}

void kmain(void) {
    /* 1. Initialize COM1 Serial port */
    if (serial_init() != 0) {
        halt_forever();
    }

    /* 2. Validate Limine base revision */
    if (!boot_is_base_revision_supported()) {
        serial_puts("[PANIC] Limine base revision 3 is not supported by bootloader!\n");
        halt_forever();
    }

    serial_puts("\n\n======================================================\n");
    serial_puts("             AstraOS Kernel v0.2.0-dev                \n");
    serial_puts("        Architecture: x86-64 | Boot: UEFI/Limine       \n");
    serial_puts("======================================================\n\n");
    serial_puts("[BOOT] COM1 Serial initialized (115200 baud, 8N1).\n");

    /* 3. Initialize GDT & IDT */
    serial_puts("[BOOT] Initializing Global Descriptor Table (GDT)...\n");
    gdt_init();
    serial_puts("[BOOT] GDT reloaded (Kernel & user descriptors staged; CPU in Ring 0).\n");

    serial_puts("[BOOT] Initializing Interrupt Descriptor Table (IDT)...\n");
    idt_init();
    serial_puts("[BOOT] IDT loaded (CPU exception gates 0..31 registered).\n");

    /* Read boot command line if present */
    const char *cmdline = boot_get_cmdline();
    if (cmdline) {
        serial_puts("[BOOT] Kernel cmdline: '");
        serial_puts(cmdline);
        serial_puts("'\n");
    }

    /* Check for early deliberate exception test */
    if (kstrstr_contains(cmdline, "test=exception")) {
        serial_puts("\n[TEST] Deliberate CPU exception test triggered via cmdline (test=exception).\n");
        serial_puts("[TEST] Executing UD2 instruction...\n");
        __asm__ volatile ("ud2");
    }

    /* 4. Probe Framebuffer */
    bool force_nofb = kstrstr_contains(cmdline, "nofb");
    if (!force_nofb) {
        struct limine_framebuffer_response *fb_resp = boot_get_framebuffer_response();
        if (fb_resp && fb_resp->framebuffer_count > 0 && fb_resp->framebuffers[0]) {
            struct limine_framebuffer *fb = fb_resp->framebuffers[0];
            if (fb_init(fb) == 0) {
                u64 w, h;
                fb_get_resolution(&w, &h);
                kprintf("[BOOT] Framebuffer console initialized: %llux%llu (32bpp, pitch %llu bytes).\n",
                        (unsigned long long)w, (unsigned long long)h, (unsigned long long)fb->pitch);
            } else {
                serial_puts("[WARN] Framebuffer initialization failed. Falling back to serial-only console.\n");
            }
        } else {
            serial_puts("[BOOT] No framebuffer provided by bootloader. Operating in serial-only mode.\n");
        }
    } else {
        serial_puts("[BOOT] 'nofb' specified: operating in safe serial-only console mode.\n");
    }

    /* 5. Memory Map Processing */
    struct limine_memmap_response *mmap_resp = boot_get_memmap_response();
    if (!mmap_resp) {
        panic("Bootloader provided no memory map!");
    }
    memmap_init(mmap_resp);
    kprintf("[BOOT] Memory map received: %llu entries.\n", (unsigned long long)mmap_resp->entry_count);
    memmap_print();

    /* 6. HHDM (Higher-Half Direct Map) validation */
    struct limine_hhdm_response *hhdm_resp = boot_get_hhdm_response();
    if (!hhdm_resp) {
        panic("HHDM response not provided by bootloader!");
    }
    kprintf("[BOOT] HHDM offset: 0x%016llx\n", (unsigned long long)hhdm_resp->offset);

    kprintf("[BOOT] Core bootstrap completed.\n");
    kprintf("ASTRA_BOOT_OK\n\n");

    /* 7. Physical Memory Manager (PMM) Initialization */
    kprintf("[BOOT] Initializing Physical Page Manager (PMM)...\n");
    int pmm_res = pmm_init(mmap_resp, hhdm_resp->offset);
    if (pmm_res != PMM_SUCCESS) {
        panic("Failed to initialize PMM! Error code: %d", pmm_res);
    }
    kprintf("[BOOT] PMM initialized successfully.\n");
    pmm_print_stats();

    /* 8. Run In-Kernel PMM Self-Test */
    if (!pmm_self_test(hhdm_resp->offset)) {
        panic("In-kernel PMM self-test failed!");
    }
    kprintf("ASTRA_PMM_OK\n\n");

    /* 9. Launch Kernel Monitor */
    monitor_run();

    /* Safety fallback */
    halt_forever();
}
