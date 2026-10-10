# AstraOS Staged Roadmap

The following phased roadmap outlines the architectural progression of AstraOS from bare-metal bootstrap to a full preemptive multitasking system with CLI and GUI environments.

### Milestone 1: Boot & Diagnostics Slice (Current - Verified)
- [x] Reproducible x86-64 ELF64 kernel build with pinned cross-toolchain.
- [x] UEFI Limine bootloader protocol v3 integration.
- [x] COM1 serial initialization (115200 8N1) and early boot logging.
- [x] GDT reloading with 64-bit kernel and user selectors.
- [x] IDT initialization with 32 CPU exception vectors and register diagnostic dumps.
- [x] 1280x800 Framebuffer console with 8x16 font rendering, color masking, and scroll.
- [x] Graceful fallback to serial-only console if graphics are absent.
- [x] Boot memory map validation and physical memory reporting.
- [x] Zero-heap kernel panic handler with controlled CPU halt.
- [x] Interactive Kernel Monitor over serial (`help`, `version`, `clear`, `memmap`, `halt`).
- [x] Automated integration test verifying `ASTRA_BOOT_OK` and monitor interaction in QEMU.
- [x] Real graphical framebuffer screenshot capture and verification.

### Milestone 2: Memory Management & Kernel Heap
- [ ] Physical Memory Allocator (Bitmap / Buddy allocator) managing usable RAM pages.
- [ ] Virtual Memory Management: 4-level paging (PML4, PDPT, PD, PT), recursive/HHDM mapping.
- [ ] Kernel heap allocator (`kmalloc`, `kfree`) with slab/free-list implementation.
- [ ] Page-fault handler with stack guard pages and memory invariants.

### Milestone 3: Interrupts, Timers & Kernel Multitasking
- [ ] Local APIC & IO-APIC initialization (deprecating legacy 8259 PIC).
- [ ] APIC Timer / PIT calibration for preemptive time-slicing.
- [ ] Kernel thread structure (`kthread`), thread control blocks (TCBs).
- [ ] Context switching in assembly (`push`/`pop` callee-saved registers).
- [ ] Cooperative and preemptive round-robin scheduler.
- [ ] Synchronization primitives (spinlocks, mutexes, semaphores).

### Milestone 4: User Mode & System Calls
- [ ] TSS (Task State Segment) with `RSP0` kernel stack per thread.
- [ ] Separate user address spaces with isolated page tables.
- [ ] User mode switch via `iretq` or `sysretq`.
- [ ] `syscall` / `sysret` instruction setup (EFER, STAR, LSTAR, SFMASK MSRs).
- [ ] Syscall dispatcher ABI with copy-in / copy-out pointer validation.
- [ ] Basic syscalls: `sys_write`, `sys_read`, `sys_exit`, `sys_yield`.

### Milestone 5: VFS, Initial Ramdisk & Shell
- [ ] Virtual Filesystem (VFS) abstractions: `vnode`, `mount`, `file_operations`.
- [ ] Initramfs / TAR / CPIO boot module loading via Limine.
- [ ] File descriptors table per process (`stdin`, `stdout`, `stderr`).
- [ ] Anonymous pipes and IPC.
- [ ] Native user-space shell binary executed from ramdisk.

### Milestone 6: Storage & Drivers
- [ ] PCI configuration space enumeration.
- [ ] VirtIO-Block device driver for disk I/O.
- [ ] Ext2 / FAT32 filesystem driver.
- [ ] Persistent storage mounting and data integrity tests.

### Milestone 7: Networking
- [ ] VirtIO-Net driver.
- [ ] Ethernet, ARP, IPv4, ICMP, and UDP/TCP stack.
- [ ] Socket interface syscalls (`socket`, `bind`, `sendto`, `recvfrom`).

### Milestone 8: Graphical Compositor & GUI Toolkit
- [ ] PS/2 and USB HID keyboard and mouse event processing.
- [ ] Double-buffered user-space software compositor.
- [ ] Window management, dirty rect redraws, clipping regions.
- [ ] Native GUI toolkit: text rendering, buttons, text inputs, window chrome.
- [ ] Terminal emulator application running the user shell.

### Milestone 9: Security, Permissions & Recovery
- [ ] Process privileges, UID/GID models, access control lists.
- [ ] Dynamic library loading (ELF shared objects).
- [ ] Crash dumps, kernel logging ring buffer, and recovery mode.
