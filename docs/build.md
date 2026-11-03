# AstraOS v1.0 Build & Toolchain Guide

## 1. Prerequisites and Toolchain Requirements

AstraOS is engineered for reproducible, self-contained compilation on modern 64-bit host systems. The workspace includes or utilizes pinned toolchain binaries:

| Component | Pinned Version / Path | Purpose |
|---|---|---|
| **Clang C Compiler** | Clang 23.1.3 (`tools/llvm-toolchain/bin/clang.exe` or system LLVM) | Freestanding C17 kernel & user ELF compilation |
| **LLD Linker** | LLD 23.1.3 (`tools/llvm-toolchain/bin/ld.lld.exe` or system LLD) | Statically linking higher-half kernel ELF |
| **Python Runtime** | Python 3.11+ (`python.exe`) | Build orchestration, initramfs packing, ISO authoring |
| **QEMU Emulator** | QEMU 9.2.0 (`tools/qemu/qemu-system-x86_64.exe`) | Virtual hardware emulation & test runner |
| **EDK2 OVMF** | OVMF x86-64 UEFI Firmware (`tools/qemu/share/edk2-x86_64-code.fd`)| UEFI firmware bootloader environment |
| **Limine Bootloader** | Limine v8.x Binary (`third_party/limine/BOOTX64.EFI`) | UEFI Stage-1/2 protocol bootloader |
| **MinGW GCC** | MinGW-w64 GCC (`C:\mingw64\bin\gcc.exe`) | Compiling host-side test harnesses and mock DLLs |
| **Oracle VirtualBox** | VirtualBox 7.x (`C:\Program Files\Oracle\VirtualBox\VBoxManage.exe`) | Hardware virtualization qualification |

To verify toolchain readiness, execute:
```powershell
python scripts/doctor.py
```

---

## 2. Standard Build Pipeline

The authoritative build entry point is `scripts/build_kernel.py`:
```powershell
python scripts/build_kernel.py
```

This single command deterministically executes all stages of the build:
1. **User Application Build (`scripts/build_user.py`)**:
   - Compiles user programs (`browser`, `counter_gui`, `hello_gui`, `cpu_worker`, etc.) using freestanding Clang with user linker scripts.
   - Generates native ELF executables in `build/`.
   - Generates the embedded user header `kernel/include/kernel/user_app_elf.h`.
2. **Initramfs Generation (`scripts/build_initramfs.py`)**:
   - Packs root filesystem files, web assets (`/www`), SSL certificates (`/etc/ssl`), and binaries into `build/initramfs.bin` (`ASTRAFS1` format).
3. **Kernel Compilation & Linking (`scripts/build_kernel.py`)**:
   - Compiles all C and assembly kernel sources in `kernel/` with flags:
     `-target x86_64-unknown-none-elf -std=c17 -ffreestanding -fno-stack-protector -mno-red-zone -mcmodel=kernel -O2 -g`.
   - Links `build/kernel.elf` using `kernel/arch/x86_64/linker.ld`.
4. **EFI Staging (`scripts/prepare_esp.py`)**:
   - Populates `build/esp` with `\EFI\BOOT\BOOTX64.EFI`, `limine.conf`, `kernel.elf`, and `initramfs.bin`.
5. **Bootable ISO Generation (`scripts/build_iso.py`)**:
   - Formats a pure-Python FAT16 EFI System Partition image (16 MiB).
   - Constructs a compliant ISO 9660 + El Torito bootable CD/DVD image at `build/astraos.iso`.

---

## 3. Clean Build & Artifact Removal

To perform a clean build and purge all intermediate binaries safely:
```powershell
make clean
```
or via Python:
```powershell
python -c "import glob, os, shutil; [os.remove(f) for f in glob.glob('build/*.o') + glob.glob('build/*.elf') + glob.glob('build/*.iso') + glob.glob('build/*.bin') if os.path.exists(f)]; shutil.rmtree('build/esp', ignore_errors=True)"
```

---

## 4. Reproducible Build Verification

AstraOS enforces deterministic, bit-for-bit reproducible builds. Timestamps in FAT16 and ISO 9660 volume descriptors are pinned to constants.

To execute two independent clean builds and verify byte-identical outputs:
```powershell
python tests/test_reproducible_build.py
```
Expected output:
```
  initramfs.bin Match : YES (100% IDENTICAL)
  kernel.elf Match    : YES (100% IDENTICAL)
  astraos.iso Match   : YES (100% IDENTICAL)
  [SUCCESS] 100% BIT-FOR-BIT REPRODUCIBLE BUILDS CONFIRMED!
```

