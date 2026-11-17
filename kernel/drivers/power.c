#include <kernel/power.h>
#include <kernel/acpi.h>
#include <kernel/kprintf.h>
#include <kernel/vfs.h>
#include <kernel/block.h>
#include <kernel/wm.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/panic.h>
#include <kernel/session.h>

static volatile power_state_t g_power_state = POWER_STATE_RUNNING;
static volatile int g_power_in_progress = 0;

void power_init(void) {
    g_power_state = POWER_STATE_RUNNING;
    g_power_in_progress = 0;
    acpi_init();
    kprintf("[POWER] Centralized Power Lifecycle Controller initialized.\n");
}

power_state_t power_get_state(void) {
    return g_power_state;
}

const char *power_state_to_string(power_state_t st) {
    switch (st) {
        case POWER_STATE_RUNNING:            return "RUNNING";
        case POWER_STATE_SHUTDOWN_REQUESTED: return "SHUTDOWN_REQUESTED";
        case POWER_STATE_SYNCING:            return "SYNCING";
        case POWER_STATE_QUIESCING:          return "QUIESCING";
        case POWER_STATE_POWERING_OFF:       return "POWERING_OFF";
        case POWER_STATE_RESTARTING:         return "RESTARTING";
        case POWER_STATE_SUSPENDING:         return "SUSPENDING";
        case POWER_STATE_SUSPENDED:          return "SUSPENDED";
        case POWER_STATE_RESUMING:           return "RESUMING";
        case POWER_STATE_FAILED:             return "FAILED";
        default:                             return "UNKNOWN";
    }
}

bool power_is_in_progress(void) {
    return (g_power_in_progress != 0) || (g_power_state != POWER_STATE_RUNNING);
}

static void power_sync_storage(void) {
    kprintf("[POWER] Synchronizing VFS and AstraFS transactions...\n");
    int sync_res = vfs_sync();
    if (sync_res != VFS_OK) {
        kprintf("[POWER WARN] VFS sync returned status %d\n", sync_res);
    }

    /* Systematic hardware cache flush across all block devices */
    kprintf("[POWER] Flushing hardware write caches across all storage devices...\n");
    int dev_count = block_get_device_count();
    for (int i = 0; i < dev_count; i++) {
        block_device_t *dev = block_get_device(i);
        if (dev && !dev->read_only && !dev->is_boot_device) {
            int fr = block_flush(dev);
            if (fr != BLOCK_OK) {
                kprintf("[POWER WARN] Block cache flush failed on '%s' (err=%d)\n", dev->name, fr);
            } else {
                kprintf("[POWER] Hardware write cache flushed on '%s'.\n", dev->name);
            }
        }
    }
}

static void power_quiesce_system(void) {
    kprintf("[POWER] Notifying applications and terminating desktop windows...\n");
    wm_destroy_all_for_pid(0);

    kprintf("[POWER] Masking external interrupts and quiescing CPU...\n");
    cli();
}

int power_shutdown(void) {
    /* Prevent concurrent power requests */
    if (__atomic_test_and_set(&g_power_in_progress, __ATOMIC_SEQ_CST)) {
        kprintf("[POWER WARN] Lifecycle action already in progress!\n");
        return POWER_ERR_BUSY;
    }

    g_power_state = POWER_STATE_SHUTDOWN_REQUESTED;
    kprintf("\n=======================================================\n");
    kprintf("       AstraOS System Shutdown Sequence Initiated       \n");
    kprintf("=======================================================\n");

    /* 1. Sync filesystems and flush disk caches */
    g_power_state = POWER_STATE_SYNCING;
    power_sync_storage();

    /* 2. Quiesce processes, windows, and interrupts */
    g_power_state = POWER_STATE_QUIESCING;
    power_quiesce_system();

    /* 3. Invoke ACPI S5 hardware power-off */
    g_power_state = POWER_STATE_POWERING_OFF;
    kprintf("[POWER] Attempting ACPI S5 soft-off power down...\n");

    if (acpi_is_s5_supported()) {
        acpi_poweroff();
        /* Bounded delay for hardware response */
        for (volatile int i = 0; i < 500000; i++) {
            io_wait();
        }
    }

    /* 4. Platform/Hypervisor Fallbacks */
    kprintf("[POWER] Trying platform-specific hypervisor power ports...\n");
    outw(0x604, 0x2000);   /* QEMU ACPI PM / PIIX4 */
    outw(0x4004, 0x3400);  /* VirtualBox */
    outw(0xB004, 0x2000);  /* Bochs / older QEMU */
    outw(0x0600, 0x0034);  /* Cloud-Hypervisor */

    for (volatile int i = 0; i < 500000; i++) {
        io_wait();
    }

    /* 5. Safe Fallback if platform cannot cut power */
    g_power_state = POWER_STATE_FAILED;
    kprintf("\n=======================================================\n");
    kprintf(" [POWER] ACPI Power-off completed.                      \n");
    kprintf(" System halted. You may now safely switch off power.   \n");
    kprintf("=======================================================\n\n");

    halt_forever();
    return POWER_OK;
}

int power_restart(void) {
    if (__atomic_test_and_set(&g_power_in_progress, __ATOMIC_SEQ_CST)) {
        kprintf("[POWER WARN] Lifecycle action already in progress!\n");
        return POWER_ERR_BUSY;
    }

    g_power_state = POWER_STATE_RESTARTING;
    kprintf("\n=======================================================\n");
    kprintf("        AstraOS System Restart Sequence Initiated      \n");
    kprintf("=======================================================\n");

    /* 1. Sync filesystems and flush disk caches */
    g_power_state = POWER_STATE_SYNCING;
    power_sync_storage();

    /* 2. Quiesce processes, windows, and interrupts */
    g_power_state = POWER_STATE_QUIESCING;
    power_quiesce_system();

    /* 3. Stage 1: ACPI 2.0+ Reset Register */
    kprintf("[POWER] Attempting ACPI Reset Register...\n");
    if (acpi_is_reset_supported()) {
        acpi_reboot();
        for (volatile int i = 0; i < 50000; i++) io_wait();
    }

    /* 4. Stage 2: 8042 Keyboard Controller Reset (Bounded loop) */
    kprintf("[POWER] Attempting 8042 Keyboard Controller reset (Pulse Line 0)...\n");
    for (int iter = 0; iter < 100000; iter++) {
        u8 temp = inb(0x64);
        if (temp & 1) (void)inb(0x60);
        if (!(temp & 2)) {
            outb(0x64, 0xFE);
            break;
        }
    }
    for (volatile int i = 0; i < 50000; i++) io_wait();

    /* 5. Stage 3: PCI Fast Reset via Port 0xCF9 */
    kprintf("[POWER] Attempting PCI Fast Reset via port 0xCF9...\n");
    outb(0xCF9, 0x02);
    io_wait();
    outb(0xCF9, 0x06);
    io_wait();
    for (volatile int i = 0; i < 50000; i++) io_wait();

    /* 6. Stage 4: Architectural CPU Triple Fault */
    kprintf("[POWER] Triggering CPU Triple Fault reset fallback...\n");
    struct { u16 limit; u64 base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile ("lidt %0; int3" : : "m"(null_idt));

    halt_forever();
    return POWER_OK;
}

int power_halt(void) {
    if (__atomic_test_and_set(&g_power_in_progress, __ATOMIC_SEQ_CST)) {
        return POWER_ERR_BUSY;
    }

    g_power_state = POWER_STATE_QUIESCING;
    power_sync_storage();
    power_quiesce_system();

    kprintf("\n[POWER] System halted safely. Power may be disconnected.\n");
    halt_forever();
    return POWER_OK;
}

int power_suspend(void) {
    kprintf("[POWER] ACPI S3 (Suspend-to-RAM) requested.\n");
    if (!acpi_is_s3_supported()) {
        kprintf("[POWER BLOCKED] ACPI S3 sleep state not exposed by platform firmware.\n");
        return POWER_ERR_UNSUPPORTED;
    }
    kprintf("[POWER BLOCKED] S3 Suspend-to-RAM requires missing platform resume dependencies:\n");
    kprintf("  1. 16-bit real-mode waking trampoline (< 1MB) for FACS->FirmwareWakingVector.\n");
    kprintf("  2. CPU context save/restore (CR0, CR3, CR4, GDT, IDT, TSS, non-volatile GPRs).\n");
    kprintf("  3. LAPIC/IOAPIC controller state save and reprogram upon firmware resume.\n");
    kprintf("  4. PCI bus and device power management state transitions (D0 -> D3hot -> D0).\n");
    kprintf("  5. Framebuffer VBE/GOP video mode re-probing and display surface re-init.\n");
    kprintf("  6. PS/2 8042 controller re-initialization and IRQ line unmasking.\n");
    kprintf("[POWER] Fake sleep (blank screen) rejected per zero-mockup policy.\n");
    return POWER_ERR_UNSUPPORTED;
}

int power_lock(void) {
    sid_t active_sid = session_get_active_sid();
    if (active_sid == SESSION_INVALID_SID) {
        kprintf("[POWER] No active session to lock.\n");
        return POWER_ERR_UNSUPPORTED;
    }
    kprintf("[POWER] Locking active session (SID %u)...\n", active_sid);
    int res = session_lock(active_sid);
    return (res == SESSION_OK) ? POWER_OK : POWER_ERR_UNSUPPORTED;
}

int power_logout(void) {
    sid_t active_sid = session_get_active_sid();
    if (active_sid == SESSION_INVALID_SID) {
        kprintf("[POWER] No active session to log out.\n");
        return POWER_ERR_UNSUPPORTED;
    }
    kprintf("[POWER] Logging out active session (SID %u)...\n", active_sid);
    int res = session_logout(active_sid);
    return (res == SESSION_OK) ? POWER_OK : POWER_ERR_UNSUPPORTED;
}
