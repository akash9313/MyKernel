#include <kernel/mem.h>
#include <kernel/kprintf.h>

static struct limine_memmap_response *g_mmap = NULL;
static u64 g_usable_bytes = 0;
static u64 g_bootloader_bytes = 0;
static u64 g_kernel_bytes = 0;
static u64 g_acpi_bytes = 0;
static u64 g_framebuffer_bytes = 0;
static u64 g_reserved_bytes = 0;

static const char *mem_type_to_str(u64 type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:
            return "Usable RAM";
        case LIMINE_MEMMAP_RESERVED:
            return "Reserved";
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            return "ACPI Reclaimable";
        case LIMINE_MEMMAP_ACPI_NVS:
            return "ACPI NVS";
        case LIMINE_MEMMAP_BAD_MEMORY:
            return "Bad Memory";
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            return "Bootloader Reclaim";
#ifdef LIMINE_MEMMAP_EXECUTABLE_AND_MODULES
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
            return "Kernel / Modules";
#elif defined(LIMINE_MEMMAP_KERNEL_AND_MODULES)
        case LIMINE_MEMMAP_KERNEL_AND_MODULES:
            return "Kernel / Modules";
#else
        case 6:
            return "Kernel / Modules";
#endif
        case LIMINE_MEMMAP_FRAMEBUFFER:
            return "Framebuffer";
        default:
            return "Unknown";
    }
}

void memmap_init(struct limine_memmap_response *mmap) {
    g_mmap = mmap;
    g_usable_bytes = 0;
    g_bootloader_bytes = 0;
    g_kernel_bytes = 0;
    g_acpi_bytes = 0;
    g_framebuffer_bytes = 0;
    g_reserved_bytes = 0;

    if (!mmap) return;

    for (u64 i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry *e = mmap->entries[i];
        if (!e) continue;

        /* Check for arithmetic overflow in memory descriptor */
        if (e->base + e->length < e->base) {
            continue;
        }

        switch (e->type) {
            case LIMINE_MEMMAP_USABLE:
                g_usable_bytes += e->length;
                break;
            case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
                g_bootloader_bytes += e->length;
                break;
            case 6: /* Kernel / Modules */
                g_kernel_bytes += e->length;
                break;
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            case LIMINE_MEMMAP_ACPI_NVS:
                g_acpi_bytes += e->length;
                break;
            case LIMINE_MEMMAP_FRAMEBUFFER:
                g_framebuffer_bytes += e->length;
                break;
            case LIMINE_MEMMAP_RESERVED:
            case LIMINE_MEMMAP_BAD_MEMORY:
            default:
                g_reserved_bytes += e->length;
                break;
        }
    }
}

u64 memmap_get_total_usable(void) {
    return g_usable_bytes;
}

u64 memmap_get_total_memory(void) {
    /* Sum of actual RAM assets (excluding MMIO and reserved device apertures) */
    return g_usable_bytes + g_bootloader_bytes + g_kernel_bytes + g_acpi_bytes + g_framebuffer_bytes;
}

void memmap_print(void) {
    if (!g_mmap) {
        kprintf("[MM] Memory map not provided by bootloader!\n");
        return;
    }

    kprintf("\n======================= Physical Memory Map =======================\n");
    kprintf("  Idx   Physical Start      .. Physical End           Size (KiB)  Type\n");
    kprintf("-------------------------------------------------------------------\n");

    for (u64 i = 0; i < g_mmap->entry_count; i++) {
        struct limine_memmap_entry *e = g_mmap->entries[i];
        if (!e) continue;

        u64 end = (e->length > 0) ? (e->base + e->length - 1) : e->base;
        u64 size_kib = e->length / 1024;
        kprintf("  [%02llu] 0x%016llx .. 0x%016llx  (%8llu KiB)  %s\n",
                (unsigned long long)i,
                (unsigned long long)e->base,
                (unsigned long long)end,
                (unsigned long long)size_kib,
                mem_type_to_str(e->type));
    }

    u64 ram_total = memmap_get_total_memory();

    kprintf("-------------------------------------------------------------------\n");
    kprintf("Installed System RAM: %llu MiB (%llu KiB)\n",
            (unsigned long long)(ram_total / (1024 * 1024)),
            (unsigned long long)(ram_total / 1024));
    kprintf("  - Usable RAM:       %llu MiB (%llu KiB)\n",
            (unsigned long long)(g_usable_bytes / (1024 * 1024)),
            (unsigned long long)(g_usable_bytes / 1024));
    kprintf("  - Bootloader/Kernel:%llu KiB\n",
            (unsigned long long)((g_bootloader_bytes + g_kernel_bytes) / 1024));
    kprintf("  - ACPI / NVS:       %llu KiB\n",
            (unsigned long long)(g_acpi_bytes / 1024));
    kprintf("  - Framebuffer:      %llu KiB\n",
            (unsigned long long)(g_framebuffer_bytes / 1024));
    kprintf("Reserved/MMIO Space:  %llu MiB (%llu KiB)\n",
            (unsigned long long)(g_reserved_bytes / (1024 * 1024)),
            (unsigned long long)(g_reserved_bytes / 1024));
    kprintf("===================================================================\n\n");
}
