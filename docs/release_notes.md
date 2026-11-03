# AstraOS v1.0 Release Notes

**Release Version:** 1.0.0  
**Release Date:** 2026-11-03  
**Architecture:** x86-64 (AMD64 / Intel 64)  
**Boot Interface:** 64-bit UEFI via Limine Protocol v3  
**Repository State:** Mainline Head (`a133b84cf42cb086689a1a270c44a2ce05a2bc09`)  

---

## 1. Overview

AstraOS v1.0 represents the culmination of a rigorous 26-phase engineering roadmap, transitioning AstraOS from an audited bare-metal microkernel foundation into a fully integrated, multi-core, preemptive operating system featuring graphical window management, networking, cryptographic security, persistent storage, and an in-kernel web browsing stack.

---

## 2. Key Subsystem Features

### 2.1 Kernel Core & Architecture
- **Higher-Half Kernel Layout**: Relocated to `0xffffffff80200000` with direct-map physical memory window at `0xffff800000000000`.
- **4-Level Paging (VMM)**: Kernel constructs its own independent PML4 page tables, enforces W^X permissions (RX code, RW+NX data), and protects user addresses below `0x0000800000000000`.
- **Physical Memory Manager (PMM)**: Dynamic region-based 4096-byte bitmap allocator managing all usable physical RAM with zero page 0 leakage.
- **Slab & Block Heap**: Dynamic kernel heap allocator with boundary overflow and zero-byte protection.
- **CPU Privilege Separation**: Ring 0 kernel supervisor mode strictly isolated from Ring 3 user applications. Syscalls serviced via `int 0x80`.

### 2.2 SMP & Multi-Core Architecture
- **Symmetric Multiprocessing (SMP)**: Dynamic enumeration and bringup of up to 8 CPU cores via ACPI MADT and INIT-SIPI-SIPI AP sequence.
- **Decentralized TLB Shootdown Queues**: Per-CPU 8-slot TLB request queues eliminating global lock contention under multi-core concurrency.
- **Lock-Free Reader Servicing & Dekker Barrier**: Provably race-free atomic reader registration handshake with Dekker store-load memory barriers (`mfence`), double generation validation, and timeout auto-drainage.
- **Per-CPU Data Isolation**: `GS_BASE` MSR (`0xC0000101`) maps isolated `cpu_state_t` structures per core.

### 2.3 Preemptive Multitasking & Synchronization
- **Preemptive Scheduler**: Priority-aware round-robin scheduler with LAPIC timer preemption hooks at IRQ exit.
- **Synchronization Primitives**: IRQ-safe spinlocks (`spinlock_acquire_irqsave`), sleeping mutexes with wait-queue handoff, counting semaphores, and atomic reference counters.

### 2.4 Storage & Filesystem
- **Virtual Filesystem (VFS)**: Unified hierarchical mount table routing read-only `initramfs` at `/`, volatile RAMFS at `/tmp`, and persistent `AstraFS` at `/data`.
- **AstraFS Persistent Filesystem**: Dual alternating metadata sets (Set A and Set B) with 5-stage atomic commit, pre-commit ATA cache flushes, crash consistency across power loss, and open file descriptor preservation across atomic renames.
- **Block Layer Protection**: Sector bounds enforcement, 64-bit integer overflow trapping, and boot media write protection (`BLOCK_ERR_READ_ONLY`).

### 2.5 Networking & Cryptography
- **Intel 82540EM (E1000) NIC Driver**: Full PCI discovery, transmit/receive ring buffers, interrupt handling.
- **Network Stack**: Ethernet II, ARP, IPv4 with length underflow guards, ICMP Echo, UDP, DHCP client, DNS resolver, and TCP state machine.
- **Cryptographic Primitives**: SHA-256, HMAC, HKDF, ChaCha20-Poly1305 AEAD, X25519 Montgomery Diffie-Hellman, RSA-2048/4096 signature verification, and X.509 DER certificate parser.
- **TLS 1.3 Client**: Modern TLS 1.3 client with ChaCha20-Poly1305 and Root CA signature validation.

### 2.6 Framebuffer Graphics, GUI & AstraBrowser
- **Graphics Engine**: 32-bit ARGB double buffering, smooth scrolling, and font rendering.
- **Window Manager**: Desktop environment with movable windows, dirty rect damage tracking, focus management, text editor, file manager, and system monitor.
- **AstraBrowser**: HTML/CSS parser, layout engine, HTTP/HTTPS web fetch, multi-tab browsing, and persistent bookmark storage.

---

## 3. Verification & Qualification Summary

Every subsystem was qualified against actual virtualization engines without mocks or stubs:
- **Oracle VirtualBox 7 EFI**: **173 / 173 markers verified** (100% PASS).
- **VirtualBox Lifecycle Suite**: Cold boot, controlled shutdown, restart, and warm reset verified.
- **QEMU Quad-Core SMP (`-smp 4`)**: **173 / 173 markers verified** (100% PASS).
- **QEMU Single-Core Regression (`-smp 1`)**: **173 / 173 markers verified** (100% PASS).
- **QEMU Dual-Core Minimum SMP (`-smp 2`)**: **173 / 173 markers verified** (100% PASS).
- **QEMU 8-Core High Contention (`-smp 8`)**: **173 / 173 markers verified** (100% PASS).
- **QEMU Headless Serial (`--mode nofb`)**: **173 / 173 markers verified** (100% PASS).
- **Boot Failure Recovery Suite**: 5/5 failure scenarios passed (missing kernel, corrupted kernel, invalid config, missing secondary storage, CPU exception dump).
- **All 7 Host Test Suites**: **100% PASS** (Crypto, VFS, Block, AstraFS, ELF, Bookmarks, Browser Networking).
- **Reproducible Build Verification**: **100% bit-for-bit identical outputs** confirmed across independent clean builds.

---

## 4. Release Artifacts & SHA-256 Checksums

| File | Size (Bytes) | SHA-256 Checksum |
|---|---|---|
| `AstraOS-v1.0.iso` | 18,716,672 | `cc2c6d9bcc34731a26fdf2cd537ac3d4b6e332ef3071edac79eccd38b162214d` |
| `kernel.elf` | 1,588,720 | `72e420429a3c27d1f80383db4fb8fe5037bd243b03e870174b37d345cf293b0a` |
| `initramfs.bin` | 301,008 | `bed056bcb9df13eea8ee0310437cfa3c340f72af3ec6d73d5457dbae51bfe9c6` |

