# AstraOS Boot & Memory Layout

## Virtual Address Space Layout (x86-64)

The kernel operates in a verified 4-level paging mode (`PML4` -> `PDPT` -> `PD` -> `PT`).
All initial mappings are supervisor-only (`User=0`).
W^X (Write XOR Execute) protections are enforced across both higher-half link addresses and HHDM physical aliases.

| Virtual Address Range                         | Purpose                                              | Permissions / Hardware Flags |
|-----------------------------------------------|------------------------------------------------------|------------------------------|
| `0x0000000000000000` .. `0x00007fffffffffff` | Reserved for future user-space address space         | Unmapped (Supervisor-only)   |
| `0xffff800000000000` .. `0xffff8000xxxxxxxx` | Higher-Half Direct Map (HHDM) of required RAM        | Supervisor, W^X Enforced     |
| - *HHDM RAM / Bootloader Reclaimable*         | Usable RAM, Limine response structures, boot stack   | Supervisor, RW-, NX=1        |
| - *HHDM Kernel Text Alias*                    | Physical pages backing `.text`                       | Supervisor, R--, NX=1        |
| - *HHDM Kernel Rodata Alias*                  | Physical pages backing `.rodata`                     | Supervisor, R--, NX=1        |
| - *HHDM Framebuffer Aperture*                 | Display scanout buffer (cache attributes preserved)  | Supervisor, RW-, NX=1        |
| `0xffffffff80200000` .. `0xffffffff80207000` | Kernel `.text` section (Executable code)             | Supervisor, R-X, NX=0        |
| `0xffffffff80207000` .. `0xffffffff8020c000` | Kernel `.rodata` section (Read-only data, font)      | Supervisor, R--, NX=1        |
| `0xffffffff8020c000` .. `0xffffffff8020f000` | Kernel `.data` & `.bss` (Globals, GDT, IDT)          | Supervisor, RW-, NX=1        |
| `0xffffffff80400000` .. `0xffffffff80401000` | Dedicated Kernel Stack Guard Page (4 KiB)            | **Deliberately Unmapped (P=0)**|
| `0xffffffff80401000` .. `0xffffffff80405000` | Dedicated Kernel Stack (16 KiB, 4 PMM pages)         | Supervisor, RW-, NX=1        |

---

## Live Mapping & Descriptor Inventory

Prior to activation of the kernel-owned address space, every live pointer, descriptor, and data structure has an explicit lifetime and mapping policy:

1. **Kernel Image Sections**:
   - Physical placement obtained from Limine `kernel_address_response->physical_base` (never assumed as `VA - HHDM_OFFSET`).
   - `.text`: Virtual `0xffffffff80200000` mapped to physical base, Read-Only, Executable (`VMM_FLAG_READ`).
   - `.rodata`: Virtual `0xffffffff80207000` mapped Read-Only, Non-Executable (`VMM_FLAG_READ | VMM_FLAG_NO_EXEC`).
   - `.data` & `.bss`: Virtual `0xffffffff8020c000` mapped Read-Write, Non-Executable (`VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC`).
   - Section boundaries explicitly aligned to 4096 bytes via linker script.
2. **Current & Dedicated Stacks**:
   - **Bootloader Stack**: Backed by `LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE` memory in HHDM. Kept mapped during transition; never freed.
   - **Dedicated Stack**: 4 physical pages allocated from PMM, mapped at `0xffffffff80401000` .. `0xffffffff80405000`.
   - **Guard Page**: Address `0xffffffff80400000` left completely absent from page tables.
3. **Descriptor Tables**:
   - GDT (`g_gdt`) and IDT (`g_idt`): Statically allocated in kernel `.bss`/`.data` at link addresses `0xffffffff8020xxxx`.
   - `GDTR` and `IDTR` base pointers reference these higher-half link addresses.
4. **PMM Metadata**:
   - Descriptors and bitmap buffers allocated in usable physical RAM, accessed via HHDM at `g_hhdm_offset + phys`.
   - Covered by HHDM mapping for `LIMINE_MEMMAP_USABLE`.
5. **Page-Table Pages**:
   - Root `PML4`, `PDPT`, `PD`, and `PT` frames allocated from PMM (`USABLE` RAM) and zeroed.
   - Accessed during setup and modifications via HHDM.
6. **Retained Boot Structures**:
   - Limine memory map entries, framebuffer descriptor, kernel file command line string.
   - Located in `LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE` memory, preserved and mapped Read-Write, Non-Executable in HHDM.
7. **Framebuffer**:
   - Display buffer mapped in HHDM.
   - Caching attributes (`PWT`, `PCD`, `PAT`) inspected from active bootloader translation and preserved without conflicting aliases.
8. **High MMIO & Unused Physical Ranges**:
   - Non-RAM ranges (such as PCIe aperture at `0xfd00000000`) are strictly omitted from HHDM.

---

## CPU Protections & Activation Mechanism

1. **EFER.NXE**:
   - Checked via CPUID extended leaf `0x80000001` (EDX bit 20).
   - `IA32_EFER.NXE` (bit 11) enabled before loading page-table entries containing bit 63 (`PTE_NX`).
2. **CR0.WP (Supervisor Write Protect)**:
   - CR0 bit 16 enabled (`write_cr0(read_cr0() | (1ULL << 16))`).
   - Guarantees supervisor mode (Ring 0) respects read-only page table permissions, causing `#PF` on attempted kernel writes to `.rodata`.
3. **TLB & Translation Invalidation**:
   - If `CR4.PGE` (bit 7) is active, clearing `CR4.PGE` invalidates all global TLB entries.
   - Loading `CR3` invalidates non-global translations.
   - `CR4.PGE` is restored immediately following the root switch.
4. **Assembly Transition (`vmm_switch.S`)**:
   - Assembly function `vmm_activate_space_and_switch_stack(owned_cr3, stack_top, &continuation)`:
     - Disables interrupts (`cli`).
     - Toggles `CR4.PGE` if active to invalidate global translations.
     - Loads new `CR3`.
     - Switches `RSP` to dedicated stack top (`0xffffffff80405000`).
     - Aligns `RSP` to 16 bytes, zeroes `RBP`, pushes simulated return frame (`%rsp % 16 == 8`).
     - Jumps directly to continuation entry point without returning to old stack frame.
