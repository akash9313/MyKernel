# AstraOS Kernel Architecture Notes

## Overview
AstraOS is an original, freestanding 64-bit operating system kernel written in C17 and x86-64 assembly. The design prioritizes correctness, debuggability, memory protection, and reproducible verification over premature complexity.

## Milestone 2 Slice: Physical Memory Management & Verified Diagnostics

### 1. Boot Protocol & Toolchain
- **Bootloader**: Limine (v8.x binary, protocol revision 3). Pinned commit assets tracked in `third_party/limine/`.
- **Authoritative Protocol Header**: Single consolidated copy at `third_party/limine/limine.h` (SHA-256: `f8b35287579dbe50e9d84cca7b558e39b2b64e499f9b37ef5ebe969c05104c33`).
- **Firmware**: UEFI x86-64 (tested on OVMF / EDK2).
- **Binary Format**: ELF64 executable statically linked at virtual base `0xffffffff80200000`.
- **Protocol Markers**:
  - `LIMINE_REQUESTS_START_MARKER`
  - `LIMINE_BASE_REVISION(3)`
  - `limine_framebuffer_request`
  - `limine_memmap_request`
  - `limine_hhdm_request`
  - `limine_kernel_file_request`
  - `LIMINE_REQUESTS_END_MARKER`

### 2. CPU Initialization & Segment Protection
- **GDT (Global Descriptor Table)**:
  - Selector `0x00`: Null descriptor.
  - Selector `0x08`: 64-bit Kernel Code segment (DPL=0, L=1, Readable, Present).
  - Selector `0x10`: 64-bit Kernel Data segment (DPL=0, Writable, Present).
  - Selector `0x18`: 64-bit User Data segment descriptor (DPL=3, Writable, Present).
  - Selector `0x20`: 64-bit User Code segment descriptor (DPL=3, L=1, Readable, Present).
  - *Architectural Status*: The CPU currently runs entirely in Ring 0. User segment descriptors are staged in the GDT for future user-space transitions, but Ring 3 execution, process isolation, TSS stack switching (`RSP0`), and syscall ABIs are deliberately uninitialized at this stage.
- **IDT (Interrupt Descriptor Table)**:
  - 256 gates total.
  - Vectors 0..31 populated with CPU exception handler stubs.
  - Strict System V AMD64 ABI compliance:
    - 16-byte stack alignment guaranteed prior to C function calls.
    - Direction Flag (`DF`) explicitly cleared via `cld` in `isr_common_stub`.
    - Hardware error codes accurately preserved (vectors 8, 10..14, 17, 21, 29, 30 push error code; others push dummy 0).
    - Full register frame (`RAX`..`R15`, `RIP`, `CS`, `RFLAGS`, `RSP`, `SS`, and `CR2` for Page Faults) logged on exception.
  - Verified via isolated deliberate `#UD` test mode (`test=exception`).

### 3. Diagnostics & Memory Accounting
- **COM1 UART Driver**: Base port `0x3F8`, 115200 baud, 8N1, FIFO enabled, loopback verified.
- **Audited Memory Map Accounting**:
  - Strict separation between actual physical RAM (Usable, Bootloader, Kernel, ACPI) and high MMIO/PCIe device apertures (e.g. `0xfd00000000`–`0xffffffffff`).
  - High MMIO space is never mislabeled as installed RAM.
  - Arithmetic overflow and zero-length bounds checks enforced.
- **kprintf**: Freestanding zero-heap formatter (`%s`, `%c`, `%d`, `%u`, `%x`, `%X`, `%p`, `%%`) teeing to serial and framebuffer.
- **Headless Fallback**: Safe, verified execution in serial-only mode when no graphical framebuffer exists (verified with `-vga none` and `nofb`).

### 4. Physical Page Manager (PMM)
- **Allocation Granularity**: 4096-byte physical pages (`PAGE_SIZE = 4096`).
- **Allocator Design**: Region-based bitmap allocator.
  - Instead of a monolithic bitmap covering sparse high MMIO apertures, each usable physical memory descriptor from the Limine map becomes an independent `struct pmm_region`.
  - Discards non-usable memory (reserved, bootloader-reclaimable, ACPI, framebuffer).
  - Upward alignment for region starts, downward alignment for region ends.
  - Physical Page Zero (`0x00000000`) is unconditionally reserved.
  - Metadata (region descriptors + bitmaps) is carved out of the first suitable usable memory region, mapped via the Higher-Half Direct Map (HHDM), and marked allocated before serving requests.
  - Metadata overhead is ~0.003% of managed RAM (~20 KiB for 512 MiB RAM).
- **Invariants & Error Trapping**:
  - Traps unaligned physical addresses (`PMM_ERR_UNALIGNED`).
  - Traps attempts to free page zero (`PMM_ERR_PAGE_ZERO`).
  - Traps addresses outside managed usable regions (`PMM_ERR_UNMANAGED`).
  - Traps double frees (`PMM_ERR_DOUBLE_FREE`).
  - Traps memory exhaustion (`PMM_ERR_NOMEM`).
- **Synchronization**: Single-core design; SMP locking will be introduced in Milestone 3.

### 5. Kernel Serial Monitor
- Interactive development shell over COM1 UART.
- Commands:
  - `help`: lists all available commands.
  - `version`: reports kernel version, architecture, and active subsystems.
  - `clear`: clears terminal and framebuffer.
  - `memmap`: displays physical memory map table with RAM vs MMIO breakdown.
  - `mem`: displays detailed PMM statistics and region-by-region allocation state.
  - `fault`: executes deliberate `ud2` instruction for interactive exception verification.
  - `halt`: cleanly halts CPU.
