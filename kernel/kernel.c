#include <kernel/types.h>
#include <kernel/boot.h>
#include <kernel/serial.h>
#include <kernel/framebuffer.h>
#include <kernel/kprintf.h>
#include <kernel/panic.h>
#include <kernel/mem.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/heap.h>
#include <kernel/monitor.h>
#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/drivers/apic.h>
#include <kernel/sched.h>
#include <kernel/sync.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/vfs.h>
#include <kernel/gfx.h>
#include <kernel/input.h>
#include <kernel/wm.h>
#include <kernel/pci.h>
#include <kernel/e1000.h>
#include <kernel/net.h>
#include <kernel/rtc.h>
#include <kernel/crypto.h>
#include <kernel/tls.h>
#include <kernel/browser.h>

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

static phys_addr_t g_owned_pml4 = 0;
static virt_addr_t g_dedicated_stack_top = 0;
static virt_addr_t g_guard_page_va = 0;
__attribute__((noreturn)) void kernel_continuation(void);

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

    /* 9. Virtual Memory Manager (VMM) Initialization */
    kprintf("[BOOT] Initializing Virtual Memory Manager (VMM)...\n");
    int vmm_res = vmm_init(hhdm_resp->offset);
    if (vmm_res != VMM_SUCCESS) {
        panic("Failed to initialize VMM! Error code: %d", vmm_res);
    }
    kprintf("[BOOT] VMM initialized successfully.\n");

    /* 10. Run In-Kernel VMM Self-Test */
    if (!vmm_self_test(hhdm_resp->offset)) {
        panic("In-kernel VMM self-test failed!");
    }

    /* 11. Construct verified kernel-owned address space */
    kprintf("[BOOT] Constructing kernel-owned address space...\n");
    struct limine_kernel_address_response *kaddr_resp = boot_get_kernel_address_response();
    if (!kaddr_resp) {
        panic("Bootloader did not provide kernel address response!");
    }
    struct limine_framebuffer_response *fb_resp = boot_get_framebuffer_response();

    phys_addr_t owned_pml4 = 0;
    virt_addr_t dedicated_stack_top = 0;
    virt_addr_t guard_page_va = 0;

    int build_res = vmm_build_kernel_space(mmap_resp, kaddr_resp, fb_resp,
                                           &owned_pml4, &dedicated_stack_top, &guard_page_va);
    if (build_res != VMM_SUCCESS) {
        panic("Failed to build kernel-owned address space! Error code: %d", build_res);
    }

    /* 12. Enforce CR0.WP (Write Protect) so supervisor respects read-only pages */
    u64 cr0 = read_cr0();
    if (!(cr0 & (1ULL << 16))) {
        kprintf("[BOOT] Enabling CR0.WP (Supervisor Write Protect)...\n");
        write_cr0(cr0 | (1ULL << 16));
        cr0 = read_cr0();
    }
    kprintf("[BOOT] CR0.WP is %s (CR0: 0x%016llx).\n",
            (cr0 & (1ULL << 16)) ? "ACTIVE" : "INACTIVE", (unsigned long long)cr0);

    /* 13. Inspect CR4.PGE and CR4.PCIDE */
    u64 cr4 = read_cr4();
    kprintf("[BOOT] CPU Paging Features: CR4.PGE=%s, CR4.PCIDE=%s (CR4: 0x%016llx).\n",
            (cr4 & (1ULL << 7)) ? "ACTIVE" : "INACTIVE",
            (cr4 & (1ULL << 17)) ? "ACTIVE" : "INACTIVE",
            (unsigned long long)cr4);

    /* Stage test context in globals before activation */
    g_owned_pml4 = owned_pml4;
    g_dedicated_stack_top = dedicated_stack_top;
    g_guard_page_va = guard_page_va;

    /* Distinct pre-activation marker */
    kprintf("[VMM] Activating kernel-owned address space (PML4: 0x%016llx, Stack: 0x%016llx)...\n",
            (unsigned long long)owned_pml4, (unsigned long long)dedicated_stack_top);

    /* 14. Activate address space and switch stack */
    vmm_activate_space_and_switch_stack(owned_pml4, dedicated_stack_top, &kernel_continuation);

    /* Unreachable */
    halt_forever();
}

__attribute__((noreturn)) void kernel_continuation(void) {
    /* 1. Verify CR3 matches our owned root */
    u64 active_cr3 = read_cr3();
    if ((active_cr3 & CR3_ADDR_MASK) != g_owned_pml4) {
        panic("CR3 mismatch after activation! Expected: 0x%016llx, got: 0x%016llx",
              (unsigned long long)g_owned_pml4, (unsigned long long)active_cr3);
    }

    /* 2. Verify RSP is within dedicated stack */
    u64 active_rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(active_rsp));
    if (active_rsp < KERNEL_STACK_BOTTOM_VA || active_rsp > g_dedicated_stack_top) {
        panic("RSP outside dedicated stack! RSP: 0x%016llx (expected [%016llx..%016llx])",
              (unsigned long long)active_rsp,
              (unsigned long long)KERNEL_STACK_BOTTOM_VA,
              (unsigned long long)g_dedicated_stack_top);
    }

    /* 3. Verify dedicated stack memory operations */
    volatile u64 stack_canary = 0x55AA55AA12345678ULL;
    if (stack_canary != 0x55AA55AA12345678ULL) {
        panic("Stack memory verification failed on dedicated kernel stack!");
    }

    /* Distinct post-activation marker */
    kprintf("[VMM] Kernel-owned address space successfully activated.\n");
    kprintf("[VMM] Active CR3: 0x%016llx (Owned Root)\n", (unsigned long long)active_cr3);
    kprintf("[VMM] Dedicated Kernel Stack: RSP=0x%016llx (Top=0x%016llx, Guard=0x%016llx)\n",
            (unsigned long long)active_rsp,
            (unsigned long long)g_dedicated_stack_top,
            (unsigned long long)g_guard_page_va);
    kprintf("ASTRA_OWN_PAGING_OK\n\n");

    /* 4. Initialize Task State Segment (TSS) with Guarded Double-Fault IST1 Stack */
    tss_init(DOUBLE_FAULT_STACK_TOP_VA);

    /* Enable SSE instructions (CR0.EM=0, CR0.MP=1, CR4.OSFXSR=1, CR4.OSXMMEXCPT=1) */
    u64 cr0_sse = read_cr0();
    cr0_sse &= ~(1ULL << 2);
    cr0_sse |= (1ULL << 1);
    write_cr0(cr0_sse);

    u64 cr4_sse = read_cr4();
    cr4_sse |= (1ULL << 9) | (1ULL << 10);
    write_cr4(cr4_sse);

    /* 5. Check for destructive CPU permission tests */
    const char *cmdline = boot_get_cmdline();
    if (kstrstr_contains(cmdline, "test=df_fault")) {
        kprintf("\n[TEST] Triggering deliberate Double Fault (#DF Vector 8) via corrupted RSP...\n");
        __asm__ volatile (
            "mov $0, %%rsp\n\t"
            "ud2\n\t"
            : : : "memory"
        );
        panic("Double fault test failed to trigger #DF!");
    } else if (kstrstr_contains(cmdline, "test=ro_fault")) {
        kprintf("\n[TEST] Triggering deliberate Write to Read-Only Page (__rodata_start: 0x%016llx)...\n",
                (unsigned long long)__rodata_start);
        *(volatile u32 *)__rodata_start = 0xDEADBEEF;
        panic("Read-Only write did not trigger page fault! Memory protections not enforced!");
    } else if (kstrstr_contains(cmdline, "test=nx_fault")) {
        kprintf("\n[TEST] Setting up and triggering deliberate Execution from NX Page...\n");
        /* Map dedicated NX page at 0xffffffff80500000 */
        phys_addr_t nx_pa;
        int ares = pmm_alloc_page(&nx_pa);
        if (ares != PMM_SUCCESS) panic("Failed to allocate page for NX test!");
        u8 *nx_hhdm = (u8 *)(nx_pa + boot_get_hhdm_response()->offset);
        nx_hhdm[0] = 0xC3; /* RET instruction */
        int mres = vmm_map_page(g_owned_pml4, 0xffffffff80500000ULL, nx_pa, VMM_FLAG_READ | VMM_FLAG_NO_EXEC);
        if (mres != VMM_SUCCESS) panic("Failed to map NX test page: %d", mres);

        void (*nx_func)(void) = (void (*)(void))0xffffffff80500000ULL;
        nx_func();
        panic("Execution of NX page did not trigger page fault! NX protections not enforced!");
    } else if (kstrstr_contains(cmdline, "test=guard_fault")) {
        kprintf("\n[TEST] Triggering deliberate Access to Unmapped Guard Page (0x%016llx)...\n",
                (unsigned long long)g_guard_page_va);
        *(volatile u64 *)g_guard_page_va = 0xCAFEBABE12345678ULL;
        panic("Guard page access did not trigger page fault! Guard protection not enforced!");
    } else if (kstrstr_contains(cmdline, "test=heap_lower_guard")) {
        kprintf("\n[TEST] Triggering deliberate Access to Heap Lower Guard Page (0x%016llx)...\n",
                (unsigned long long)HEAP_LOWER_GUARD_VA);
        *(volatile u64 *)HEAP_LOWER_GUARD_VA = 0xDEADBEEFCAFE0001ULL;
        panic("Heap lower guard page access did not trigger page fault!");
    } else if (kstrstr_contains(cmdline, "test=heap_upper_guard")) {
        kprintf("\n[TEST] Triggering deliberate Access to Heap Upper Guard Page (0x%016llx)...\n",
                (unsigned long long)HEAP_UPPER_GUARD_VA);
        *(volatile u64 *)HEAP_UPPER_GUARD_VA = 0xDEADBEEFCAFE0002ULL;
        panic("Heap upper guard page access did not trigger page fault!");
    } else if (kstrstr_contains(cmdline, "test=heap_rollback")) {
        kprintf("\n[TEST] Testing Transactional Heap Initialization Rollback via Fault Injection...\n");
        struct pmm_stats st_before;
        pmm_get_stats(&st_before);

        /* Inject fault at 4th page allocation */
        heap_set_fault_injection(4);
        int res = heap_init();
        if (res == 0) {
            panic("heap_init succeeded despite fault injection!");
        }
        if (heap_is_initialized()) {
            panic("heap is marked initialized after rollback!");
        }

        struct pmm_stats st_after;
        pmm_get_stats(&st_after);
        if (st_before.free_pages != st_after.free_pages || st_before.allocated_pages != st_after.allocated_pages) {
            panic("PMM leak during heap rollback! Before: free=%llu, After: free=%llu",
                  (unsigned long long)st_before.free_pages, (unsigned long long)st_after.free_pages);
        }

        /* Clear fault injection */
        heap_set_fault_injection(-1);
        kprintf("[TEST] PASS: Transactional heap initialization cleanly rolled back all frames and mappings!\n");
        kprintf("ASTRA_HEAP_ROLLBACK_OK\n\n");
        halt_forever();
    }

    /* 5. Post-switch regression checks: PMM & VMM operations in owned space */
    kprintf("[POST-SWITCH] Verifying PMM allocation in owned address space...\n");
    phys_addr_t test_page = 0;
    if (pmm_alloc_page(&test_page) != PMM_SUCCESS) {
        panic("Post-switch PMM page allocation failed!");
    }
    if (pmm_free_page(test_page) != PMM_SUCCESS) {
        panic("Post-switch PMM page free failed!");
    }
    kprintf("[POST-SWITCH] PMM operations verified.\n");

    /* 6. Initialize and self-test Kernel Heap */
    kprintf("[HEAP] Initializing transactional kernel heap...\n");
    struct pmm_stats pmm_before_heap;
    pmm_get_stats(&pmm_before_heap);

    if (heap_init() != 0) {
        panic("Kernel heap initialization failed!");
    }

    struct pmm_stats pmm_after_heap_init;
    pmm_get_stats(&pmm_after_heap_init);
    kprintf("[HEAP] PMM frames used for fixed arena: %llu pages (expected %llu pages)\n",
            (unsigned long long)(pmm_before_heap.free_pages - pmm_after_heap_init.free_pages),
            (unsigned long long)HEAP_ARENA_PAGES);

    if (!heap_self_test()) {
        panic("Kernel heap self-test failed!");
    }

    /* Verify PMM usage did not change during ordinary heap allocation & free operations */
    struct pmm_stats pmm_after_heap_test;
    pmm_get_stats(&pmm_after_heap_test);
    if (pmm_after_heap_init.free_pages != pmm_after_heap_test.free_pages) {
        panic("PMM usage changed during heap operations! Fixed arena leaked PMM frames: init_free=%llu, after_test_free=%llu",
              (unsigned long long)pmm_after_heap_init.free_pages,
              (unsigned long long)pmm_after_heap_test.free_pages);
    }

    kprintf("ASTRA_HEAP_OK\n\n");
    heap_print_stats();

    /* 7. Initialize Local APIC and Timer */
    apic_init();

    /* 8. Enable interrupts and verify repeated timer ticks with EOI */
    kprintf("[APIC] Enabling interrupts (sti) to verify repeated APIC timer ticks...\n");
    sti();

    u64 start_ticks = apic_get_ticks();
    while (apic_get_ticks() < start_ticks + 20) {
        __asm__ volatile ("pause");
    }

    u64 verified_ticks = apic_get_ticks();
    kprintf("[APIC] Repeated timer delivery verified: %llu ticks successfully delivered and acknowledged!\n",
            (unsigned long long)verified_ticks);
    kprintf("ASTRA_APIC_TIMER_OK\n\n");

    /* 9. Initialize Cooperative Kernel Thread Scheduler */
    sched_init();

    /* 10. Check for destructive Thread Stack Guard page fault test (deliberate guard-page access) */
    if (kstrstr_contains(cmdline, "test=thread_guard")) {
        kprintf("\n[TEST] Creating worker thread to trigger deliberate Thread Stack Guard Page Access (#PF)...\n");
        void worker_guard_touch(void *arg);
        thread_t *tg = thread_create("t_guard", (void (*)(void *))worker_guard_touch, NULL);
        if (!tg) panic("Failed to create guard test thread!");
        while (1) {
            thread_yield();
        }
        panic("Worker thread did not trigger stack guard fault!");
    } else if (kstrstr_contains(cmdline, "test=mutex_recursive")) {
        kprintf("\n[TEST] Triggering deliberate recursive mutex lock panic...\n");
        mutex_t test_m;
        mutex_init(&test_m);
        mutex_lock(&test_m);
        mutex_lock(&test_m); /* Panics with recursive acquisition attempted */
        panic("Recursive mutex lock did not panic!");
    } else if (kstrstr_contains(cmdline, "test=mutex_non_owner")) {
        kprintf("\n[TEST] Triggering deliberate non-owner mutex unlock panic...\n");
        void worker_bad_unlock(void *arg);
        static mutex_t test_m_no;
        mutex_init(&test_m_no);
        mutex_lock(&test_m_no); /* Boot thread is owner */
        thread_t *tb = thread_create("t_bad_unlock", worker_bad_unlock, &test_m_no);
        if (!tb) panic("Failed to create worker for non-owner test!");
        while (1) {
            thread_yield();
        }
        panic("Non-owner mutex unlock did not panic!");
    } else if (kstrstr_contains(cmdline, "test=mutex_exit_owning")) {
        kprintf("\n[TEST] Triggering deliberate exit while owning mutex panic...\n");
        void worker_exit_owning(void *arg);
        thread_t *to = thread_create("t_exit_owning", worker_exit_owning, NULL);
        if (!to) panic("Failed to create worker for exit owning test!");
        while (1) {
            thread_yield();
        }
        panic("Exit while owning mutex did not panic!");
    }

    /* 11. Run Scheduler Deterministic Self-Tests (cooperative) */
    sched_run_self_tests();

    /* 12. Run Phase C: Timer-Driven Preemption Verification Tests */
    sched_run_preemption_tests();

    /* 13. Run Synchronization Primitives Self-Tests (Mutexes & Semaphores) */
    sched_run_sync_tests();

    /* 14. Run Phase E: User Mode, System Calls & Process Foundation Tests */
    process_subsystem_init();
    process_run_tests();

    /* 15. Run Phase G: Initramfs, VFS & Basic File Operations Tests */
    vfs_initramfs_run_tests();

    /* 16. Run Phase I: Graphical Desktop Foundation */
    kprintf("[BOOT] Initializing Phase I Graphics, Input, Window Manager & Desktop...\n");
    if (gfx_init() == 0) {
        if (!gfx_self_test()) {
            panic("Graphics subsystem self-test failed!");
        }
        input_init(1024, 768);
        ps2_init();
        if (!input_self_test()) {
            panic("Input subsystem self-test failed!");
        }
        wm_init();
        if (!wm_self_test()) {
            panic("Window Manager self-test failed!");
        }
        desktop_init();
        if (!desktop_self_test()) {
            panic("Desktop Shell self-test failed!");
        }
        if (!app_gui_self_test()) {
            panic("GUI Applications self-test failed!");
        }

        /* Launch the two real GUI applications on the active desktop */
        app_editor_launch("/hello.txt");
        app_filemgr_launch("/");

        /* Render initial graphical desktop session */
        desktop_render();

        /* 16b. Run Phase N: User-Space GUI Applications & Process Isolation Verification */
        process_gui_run_tests();
    } else {
        kprintf("[WARN] Framebuffer graphics not available; running in headless fallback mode.\n");
    }

    /* 17. Run Phase J: Network Driver, TCP/IP Stack & Internet Connectivity */
    kprintf("\n[BOOT] Initializing Phase J PCI, E1000 NIC & TCP/IP Network Stack...\n");
    pci_init();
    if (e1000_init()) {
        net_init();
        if (!net_self_test()) {
            panic("Network subsystem self-test failed!");
        }
    } else {
        kprintf("[WARN] Intel E1000 network adapter not found or failed to initialize.\n");
    }

    /* 18. Run Phase L: Cryptographic Subsystem & TLS 1.3 Engine */
    kprintf("\n[BOOT] Initializing Phase L Cryptography, Hardware Entropy & TLS 1.3...\n");
    rtc_init();
    if (!crypto_self_test()) {
        panic("Cryptographic subsystem self-test failed!");
    }
    if (!tls_self_test()) {
        panic("TLS 1.3 engine self-test failed!");
    }

    /* 19. Run Phase K: AstraBrowser Native Graphical Web Browser */
    kprintf("\n[BOOT] Initializing Phase K AstraBrowser Native Graphical Web Browser...\n");
    app_browser_init();
    if (!browser_self_test()) {
        panic("AstraBrowser self-test failed!");
    }

    /* 19b. Run Phase O: User-Space AstraBrowser & Memory Management Tests */
    process_browser_run_tests();

    /* Launch AstraBrowser on active desktop */
    if (gfx_get_backbuffer()) {
        app_browser_launch("file:///www/index.html");
        desktop_render();
    }

    /* 20. Launch Kernel Monitor */
    monitor_run();

    /* Safety fallback */
    halt_forever();
}

void worker_guard_touch(void *arg) {
    (void)arg;
    cli();
    thread_t *curr = thread_current();
    kprintf("[TEST] Worker thread (TID %u) touching its unmapped guard page at 0x%016llx...\n",
            curr->tid, (unsigned long long)curr->guard_va);
    *(volatile u64 *)curr->guard_va = 0xDEADBEEF12345678ULL;
    panic("Thread stack guard fault failed to trigger #PF!");
}

void worker_bad_unlock(void *arg) {
    mutex_t *m = (mutex_t *)arg;
    mutex_unlock(m); /* Panics: non-owner unlock */
}

static mutex_t g_exit_owning_mutex;

void worker_exit_owning(void *arg) {
    (void)arg;
    mutex_init(&g_exit_owning_mutex);
    mutex_lock(&g_exit_owning_mutex);
    thread_exit(); /* Panics: exit while owning mutex */
}
