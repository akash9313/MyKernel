# AstraOS Boot & Memory Layout

## Virtual Address Space Layout (x86-64)

The kernel is placed in the negative 2 GiB range of the 64-bit virtual address space, adhering to the standard x86-64 higher-half model.

| Virtual Address Range                         | Purpose                                            | Permissions |
|-----------------------------------------------|----------------------------------------------------|-------------|
| `0x0000000000000000` .. `0x00007fffffffffff` | Reserved for future user-space address space       | Ring 3 (Unmapped) |
| `0xffff800000000000` .. `0xffff807fffffffff` | Higher-Half Direct Map (HHDM) of all physical RAM  | Kernel RW-  |
| `0xffffffff80200000` .. `0xffffffff80203000` | Kernel `.text` section (Code)                     | Kernel R-X  |
| `0xffffffff80203000` .. `0xffffffff80206000` | Kernel `.rodata` section (Constants & Font)       | Kernel R--  |
| `0xffffffff80206000` .. `0xffffffff80208000` | Kernel `.data`, `.limine_requests`, `.bss`         | Kernel RW-  |

## Audited Physical Memory Accounting

Physical address space reported by the firmware consists of both actual DRAM and memory-mapped I/O (MMIO) hardware windows. AstraOS strictly distinguishes between the two:

### 1. Installed System RAM
Calculated by summing only genuine physical RAM categories:
- **Usable RAM**: Memory explicitly flagged as `LIMINE_MEMMAP_USABLE`. Managed by the Physical Page Manager (PMM).
- **Bootloader Reclaimable**: Memory used by Limine structures, kept reserved during early boot.
- **Kernel / Modules**: Memory holding kernel ELF segments and boot modules.
- **ACPI Reclaimable / NVS**: ACPI descriptor tables and non-volatile configuration memory.
- **Framebuffer RAM**: Memory backing the display buffer.

### 2. Reserved & MMIO Windows
- High memory ranges above 4 GiB (such as `0x000000fd00000000` .. `0x000000ffffffffff`) represent PCI Express configuration space, APIC apertures, and device MMIO.
- **Rule**: MMIO address ranges are tracked as non-RAM and are strictly excluded from PMM bitmap sizing to avoid wasted metadata.

## Physical Memory Allocator (PMM) Layout
- Physical Page Zero (`0x00000000` .. `0x00000FFF`) is unconditionally excluded from management.
- For a standard 512 MiB QEMU UEFI boot:
  - Usable physical memory: ~504 MiB (129,201 pages across 11 descriptors).
  - PMM metadata (descriptors and bitmaps): 5 physical pages (~20 KiB), located at the start of region 0 (`0x1000`).
  - Available free memory for allocation: 129,196 pages (~504 MiB).
