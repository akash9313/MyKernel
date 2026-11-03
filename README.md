# AstraOS

AstraOS is an original, 64-bit operating system kernel targeting the x86-64 architecture, booted via the modern Limine protocol under UEFI.

AstraOS features a higher-half kernel architecture, 4-level paging with strict W^X enforcement, multi-core symmetric multiprocessing (SMP) with decentralized lock-free TLB shootdown request queues, a priority-aware preemptive round-robin scheduler, a dual-set crash-consistent filesystem (AstraFS), Intel E1000 Gigabit Ethernet networking, an embedded TLS 1.3 cryptographic stack, a double-buffered desktop window manager, and a native web browser (AstraBrowser).

---

## 1. Operating System Architecture & Highlights

- **64-Bit Higher-Half Kernel**: Located at `0xffffffff80200000` with direct-mapped physical memory aperture at `0xffff800000000000`.
- **UEFI Boot Integration**: Booted via Limine protocol v3 under 64-bit UEFI firmware (`BOOTX64.EFI`).
- **Memory Management**:
  - Region-based Physical Memory Manager (PMM) bitmap allocator managing all usable physical RAM.
  - Virtual Memory Manager (VMM) with independent 4-level paging, supervisor isolation, and user mmap support (`0x700000000000`..`0x7fffffffffff`).
  - Dynamic slab and block kernel heap with boundary guards.
- **Symmetric Multiprocessing (SMP)**:
  - Supports up to 8 CPU cores (`MAX_CPUS = 8`) brought online via ACPI MADT and INIT-SIPI-SIPI AP sequencing.
  - Per-CPU data isolation via `GS_BASE` MSR (`0xC0000101`).
  - Decentralized 8-slot per-CPU TLB shootdown request queues with lock-free remote servicing and Dekker store-load barriers (`mfence`).
- **Preemptive Multitasking**:
  - Priority-aware round-robin scheduler with voluntary yield, sleep, and LAPIC timer IRQ exit preemption hooks.
  - IRQ-safe spinlocks, sleeping mutexes with wait-queue handoff, and counting semaphores.
- **VFS & Crash-Consistent Storage**:
  - Hierarchical mount table routing immutable `initramfs` at `/`, mutable RAMFS at `/tmp`, and persistent `AstraFS` at `/data`.
  - AstraFS: Dual alternating metadata sets (Set A & Set B) with 5-stage atomic commit, pre-commit ATA cache flushes, power-loss crash consistency, and open descriptor preservation across atomic renames.
  - Block layer sector boundary enforcement and read-only boot media protection (`BLOCK_ERR_READ_ONLY`).
- **Networking & Cryptography**:
  - Intel 82540EM (E1000) PCI Gigabit Ethernet driver.
  - Network stack: Ethernet II, ARP, IPv4, ICMP Echo, UDP, DHCP, DNS, and TCP.
  - Cryptographic suite: SHA-256, HMAC, HKDF, ChaCha20-Poly1305 AEAD, X25519 Montgomery Diffie-Hellman, RSA-2048/4096 signature verification, and X.509 DER certificate parser.
  - TLS 1.3 Client: Full TLS 1.3 handshake with cipher suite ChaCha20-Poly1305 and Root CA verification.
- **Desktop Environment & Applications**:
  - 32-bit ARGB double-buffered graphics engine ($1280 \times 800$ and $1024 \times 768$).
  - Window manager with damage rect clipping, titlebar controls, text editor, file manager, and system monitor.
  - Native Graphical Terminal Application (`app_terminal.c`): 80x24 character grid, 256-line scrollback buffer, history navigation, dark theme, PageUp/PageDown scrolling.
  - Interactive Shell Engine (`shell.c`): Quoting (`'...'`, `"..."`), variable expansion (`$VAR`, `$?`), path normalization, PATH directory lookup (`/bin`, `/usr/bin`, `/pkg/bin`), output redirection (`>`, `>>`).
  - Native Package Manager (`pkg.c`): Cryptographically signed `.apkg` packages with RSA-2048 PKCS#1 v1.5 and SHA-256 verification, transactional staging in RAMFS, atomic commits via `vfs_rename()`, and clean dependency-aware uninstallation via `vfs_unlink()`.
  - Zero Autostart Policy: Clean desktop startup with zero unrequested windows; terminal launches strictly on user action.
  - AstraBrowser: In-kernel HTML/CSS tokenizer, block/inline layout engine, HTTP/HTTPS fetch, multi-tab browsing, and persistent bookmarks stored to AstraFS.

---

## 2. Storage Model & Hardware Compatibility

> [!NOTE]
> **Live-Boot Operating System**: AstraOS operates as a stateless Live-Boot OS. The primary operating system, root filesystem, system utilities, and web assets are bundled into an immutable `initramfs` mounted at `/`.
>
> **Persistent Storage**: Supported via secondary storage drives (`hd1`) formatted with AstraFS and mounted at `/data`.
>
> **Bare-Metal Disk Partitioner / Installer**: **NOT IMPLEMENTED**. AstraOS does not partition raw disks or perform bare-metal installations from the live session.
>
> **Secure Boot**: **NOT IMPLEMENTED**. Secure Boot must be **disabled** in UEFI firmware setup.

For exhaustive hardware support details, consult [docs/hardware_compatibility.md](docs/hardware_compatibility.md).

---

## 3. Toolchain & Prerequisites

The workspace includes pinned toolchain binaries under `tools/`:
- **Clang 23.1.3** / **LLD 23.1.3** (`tools/llvm-toolchain/bin`)
- **QEMU 9.2.0** (`tools/qemu/qemu-system-x86_64.exe`)
- **EDK2 OVMF x86-64 UEFI Firmware** (`tools/qemu/share/edk2-x86_64-code.fd`)
- **Python 3.11+**
- **Oracle VirtualBox 7** (Optional, for hardware virtualization qualification)

To check toolchain health:
```powershell
python scripts/doctor.py
```

---

## 4. Quickstart Commands

### Build Kernel & Bootable ISO
```powershell
python scripts/build_kernel.py
```
Outputs:
- Kernel ELF: `build/kernel.elf`
- Bootable ISO: `build/astraos.iso` and `build/AstraOS-v1.0.iso`

### Run in QEMU (Interactive Desktop + Serial Monitor)
```powershell
make run
```

### Run in QEMU Headless Mode (`nofb`)
```powershell
python tests/test_runner.py --smp 4 --mode nofb
```

### Run in Oracle VirtualBox 7 EFI
```powershell
python scripts/test_vbox.py
```

### Run Full Test Suite
```powershell
# 1. Host unit tests (Crypto, VFS, Block, AstraFS, ELF, Bookmarks, Networking)
python tests/test_crypto_host.py; python tests/test_vfs_host.py; python tests/test_block_host.py; python tests/test_fs_host.py; python tests/test_elf_host.py; python tests/test_bookmark_storage.py; python tests/test_browser_networking.py

# 2. Boot failure recovery tests (5 failure scenarios)
python tests/test_boot_failures.py

# 3. Clean build and bit-for-bit reproducibility test
python tests/test_reproducible_build.py

# 4. VirtualBox 7 EFI lifecycle and persistent storage test
python tests/test_vbox_lifecycle.py

# 5. QEMU multi-core qualification matrix (SMP 1, 2, 4, 8)
python tests/test_runner.py --smp 4
```

---

## 5. Documentation Directory

- [docs/architecture.md](docs/architecture.md) — Comprehensive technical architecture specification (Sections 1–62).
- [docs/status.md](docs/status.md) — Subsystem qualification matrix and marker status.
- [docs/build.md](docs/build.md) — Build system, toolchain dependencies, and clean build instructions.
- [docs/boot.md](docs/boot.md) — Boot instructions for QEMU and Oracle VirtualBox 7.
- [docs/hardware_compatibility.md](docs/hardware_compatibility.md) — Qualified platforms, peripherals, and unsupported hardware.
- [docs/security.md](docs/security.md) — Privilege isolation, memory protection, and network security.
- [docs/known_limitations.md](docs/known_limitations.md) — Architectural limitations and boundaries.
- [docs/release_notes.md](docs/release_notes.md) — Version 1.0 release notes and verification summary.

---

## 6. License & Provenance

- **Limine Bootloader**: BSD Zero Clause License (0BSD), pinned from `v8.x-binary` (see `third_party/LICENSE.limine`).
- **VGA 8x16 Console Font**: GNU General Public License v2 (GPL-2.0), sourced from Linux kernel `lib/fonts/font_8x16.c` (see `third_party/LICENSE.font`).
- **AstraOS Core Kernel & Userspace**: Copyright (c) 2026 AstraOS Project Contributors.
