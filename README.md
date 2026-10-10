# AstraOS

AstraOS is an original 64-bit operating system kernel targeting the x86-64 architecture, booted via the modern Limine protocol under UEFI.

## Current State: Milestone 2 (Physical Page Allocator & Audited Foundation)
- **Bootloader**: Limine v8.x binary (UEFI x86-64) with protocol revision 3. Authoritative header pinned at `third_party/limine/limine.h`.
- **CPU Subsystems**: 64-bit GDT staged (Ring 0 active), 256-entry IDT with 32 CPU exception handlers, System V ABI compliance (16-byte stack alignment, `cld` direction-flag clearing, register preservation).
- **Exception Verification**: Isolated deliberate exception test mode (`test=exception` and `fault` monitor command) confirming `#UD` vector 6 and diagnostic crash dump.
- **Serial Diagnostics**: COM1 UART driver running at 115200 baud (8N1) with FIFO and hardware loopback test.
- **Console & Graphics**: 1280x800 32bpp framebuffer console with 8x16 bitmap font, smooth scrolling, and safe verified headless fallback (`nofb` and `-vga none`).
- **Memory Map Auditing**: Separates physical RAM (Usable, Bootloader, Kernel, ACPI) from device MMIO apertures.
- **Physical Page Allocator (PMM)**: Region-based 4096-byte bitmap allocator managing usable RAM ranges, reserving page 0 and metadata pages, trapping unaligned, unmanaged, and double frees.
- **Kernel Monitor**: Interactive serial monitor with commands: `help`, `version`, `clear`, `memmap`, `mem`, `fault`, `halt`.
- **Automated Verification**: Full automated test suite executing host-side unit tests and QEMU integration tests across 128 MiB, 512 MiB, and 1024 MiB RAM, headless mode, and deliberate exception testing.

## Prerequisites
The workspace includes pinned, self-contained toolchain binaries under `tools/`:
- **Clang 23.1.3** / **LLD 23.1.3** (`tools/llvm-toolchain/bin`)
- **QEMU 9.2.0** (`tools/qemu/qemu-system-x86_64.exe`)
- **EDK2 OVMF x86-64 UEFI Firmware** (`tools/qemu/share/edk2-x86_64-code.fd`)
- **Python 3.11+**
- **GNU Make** (`mingw32-make` or `make`)

To check environment health, run:
```bash
make doctor
```

## Available Commands

### 1. Check Toolchain Health
```bash
make doctor
```

### 2. Build Kernel & Stage Boot Image
```bash
make
```

### 3. Run Automated Tests
```bash
make test
```
Executes:
1. `make test-host`: Runs host-side PMM unit tests (unaligned regions, sparse high memory, overflow, exhaustion/recovery, double frees).
2. `make test-qemu`: Boots QEMU in UEFI mode across 128 MiB, 512 MiB, and 1024 MiB RAM, tests no-framebuffer mode, and validates deliberate exception diagnostics. Returns exit code 0 on complete pass.

### 4. Run Interactive System
```bash
make run
```
Boots AstraOS in QEMU with a graphical window for the framebuffer and standard I/O connected to the COM1 serial monitor.

To run headless in pure serial mode:
```bash
make run-headless
```

### 5. Debug with GDB
```bash
make debug
```
Starts QEMU paused (`-S -s`) listening for a GDB connection on `localhost:1234` with debug symbols preserved in `build/kernel.elf`.

### 6. Clean Build Artifacts
```bash
make clean
```

## License & Provenance
- Limine bootloader: BSD Zero Clause License (0BSD), pinned from `v8.x-binary` (see `third_party/LICENSE.limine`).
- VGA 8x16 console font: GNU General Public License v2 (GPL-2.0), sourced from Linux kernel `lib/fonts/font_8x16.c` (see `third_party/LICENSE.font`).
- AstraOS project license: Undecided and uncommitted.