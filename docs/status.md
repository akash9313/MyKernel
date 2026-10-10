# AstraOS Feature Status

## Status Legend
- **Implemented & Tested**: Functionality is implemented, compiled, and verified via automated host/QEMU integration tests.
- **Implemented**: Code exists and compiles, awaiting dedicated unit/stress test suite.
- **Incomplete / In Progress**: Scaffolded or partially implemented.
- **Planned**: Scheduled for upcoming development milestones.

| Subsystem | Feature | Status | Notes / Evidence |
|-----------|---------|--------|------------------|
| **Boot** | Limine Protocol v3 Handshake | **Implemented & Tested** | Verified base revision 3, responses validated |
| **Boot** | UEFI Booting with OVMF | **Implemented & Tested** | Tested on QEMU 9.2.0 + EDK2 across 128M, 512M, 1024M |
| **Boot** | Consolidated Limine Header | **Implemented & Tested** | Single authoritative `third_party/limine/limine.h` (SHA-256 pinned) |
| **CPU** | GDT 64-bit Descriptors | **Implemented & Tested** | Kernel (`0x08`/`0x10`) and user (`0x18`/`0x20`) descriptors staged; CPU in Ring 0 |
| **CPU** | IDT Exception Handling (0..31) | **Implemented & Tested** | Strict System V ABI alignment, `cld` flag clearing, 32 exception gates |
| **CPU** | Deliberate Exception Test (#UD) | **Implemented & Tested** | Isolated test via `test=exception` and `fault` monitor command; asserts Vector 6 |
| **Diagnostics** | COM1 Serial Port (UART 16550) | **Implemented & Tested** | 115200 8N1, FIFO enabled, loopback verified |
| **Diagnostics** | Zero-Heap kprintf Formatter | **Implemented & Tested** | Supports `%s`, `%c`, `%d`, `%u`, `%x`, `%p`, `%%` |
| **Diagnostics** | Kernel Panic & Safe Halt | **Implemented & Tested** | Logs location, fault registers (CR2, CR3), executes `cli; hlt` |
| **Display** | Framebuffer Driver & Font | **Implemented & Tested** | 1280x800 @ 32bpp, 8x16 font, color masks, vertical scroll |
| **Display** | Serial-Only Safe Fallback | **Implemented & Tested** | Tested via `-vga none` and `nofb` cmdline flag |
| **Display** | Visual Screenshot Verification | **Implemented & Tested** | Captured `build/framebuffer.png` via QEMU monitor screendump |
| **Memory** | Audited Memory Map Accounting | **Implemented & Tested** | Distinguishes RAM from MMIO apertures; overflow checks enforced |
| **Memory** | Physical Page Allocator (PMM) | **Implemented & Tested** | Region-based bitmap allocator; 4 KiB pages; page 0 reserved; metadata reserved |
| **Memory** | Host PMM Unit Test Suite | **Implemented & Tested** | 5 host tests: unaligned, sparse, overflow, exhaustion/recovery, invalid frees |
| **Memory** | In-Kernel PMM Self-Test | **Implemented & Tested** | Page cycling, HHDM write/read verification, double free traps; emits `ASTRA_PMM_OK` |
| **Monitor** | Kernel Serial Shell | **Implemented & Tested** | Supports `help`, `version`, `clear`, `memmap`, `mem`, `fault`, `halt` |
| **Testing** | Automated Multi-RAM Integration Suite | **Implemented & Tested** | `make test` runs host unit tests + QEMU (128M, 512M, 1024M, nofb, exception) |
| **Memory** | Virtual Memory Manager (VMM) | **Planned (Milestone 2B)** | 4-level page tables (PML4, PDPT, PD, PT), NX permissions, kernel mapping |
| **Memory** | Kernel Heap (`kmalloc`/`kfree`) | **Planned (Milestone 2B)** | Slab / free-list allocator on top of VMM |
| **Multitasking** | Local APIC / PIT Timer | **Planned (Milestone 3)** | Preemptive timer ticks |
| **Multitasking** | Kernel Threads & Scheduler | **Planned (Milestone 3)** | Context switching, round-robin scheduler |
| **User Space** | Ring 3 Switching & Syscalls | **Planned (Milestone 4)** | Syscall ABI, TSS, address space isolation |
| **Storage** | VFS & Ramdisk | **Planned (Milestone 5)** | File tables, initrd, native user shell |
| **Storage** | VirtIO-Block & Filesystems | **Planned (Milestone 6)** | Block drivers, Ext2/FAT32 |
| **GUI** | Compositor & Window Toolkit | **Planned (Milestone 8)** | Double-buffered software compositor, desktop |
