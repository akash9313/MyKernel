# AstraOS v1.0 Boot & Installation Guide

## 1. Operating System Model: Live-Boot Architecture

AstraOS is architected as an immutable **Live-Boot Operating System**:
- **Stateless Root Filesystem**: The operating system kernel, system utilities, desktop window manager, and web browser assets are bundled into an immutable `initramfs` loaded into memory by the UEFI bootloader and mounted at root (`/`).
- **Persistent Secondary Storage**: Persistent data, user documents, and browser bookmarks are stored on a dedicated secondary storage drive (`hd1`), formatted with AstraFS and mounted at `/data`.
- **Boot Media Protection**: The primary boot media (`hd0`) is strictly write-protected at the kernel block layer (`BLOCK_ERR_READ_ONLY`) to prevent accidental corruption of the boot partition.

> [!IMPORTANT]
> **Bare-Metal Disk Partitioner / Installer Status**: **NOT IMPLEMENTED**.  
> AstraOS does not currently provide an interactive or automated partition manager or disk installer CLI. Boot media cannot be installed or cloned onto bare-metal internal drives from a live session.

> [!WARNING]
> **Secure Boot Support**: **NOT IMPLEMENTED**.  
> AstraOS EFI binaries are not signed with Microsoft Third-Party UEFI CA keys. Secure Boot **must be disabled** in firmware setup.

---

## 2. Booting in QEMU

### 2.1 Standard Graphical Desktop (with Serial Monitor)
```powershell
make run
```
or manually:
```powershell
tools/qemu/qemu-system-x86_64.exe `
    -drive if=pflash,format=raw,unit=0,file=tools/qemu/share/edk2-x86_64-code.fd,readonly=on `
    -m 512M `
    -smp 4 `
    -drive file=build/astraos.iso,media=cdrom `
    -drive file=build/data_disk.img,format=raw,if=ide,index=1 `
    -netdev user,id=net0 -device e1000,netdev=net0 `
    -serial stdio
```

### 2.2 Headless Serial Console Mode (`nofb`)
```powershell
python tests/test_runner.py --smp 4 --mode nofb
```

---

## 3. Booting in Oracle VirtualBox 7

AstraOS is fully verified and qualified on Oracle VirtualBox 7 in 64-bit EFI mode.

### 3.1 Recommended VM Configuration
1. **OS Type**: `Other / Unknown (64-bit)`.
2. **System Setup**:
   - Enable **EFI** (check "Enable EFI (special OSes only)").
   - Base Memory: **512 MB** (minimum 256 MB).
   - Processors: **1 to 4 CPUs**.
3. **Storage Configuration**:
   - Controller: **AHCI (SATA)** or **IDE (PIIX4)**.
   - Optical Drive: Attach `build/AstraOS-v1.0.iso`.
   - Secondary Hard Disk (Optional Persistent Storage): Attach a secondary VDI/VMDK drive for AstraFS persistent storage.
4. **Network**:
   - Adapter 1: Attached to **NAT**.
   - Adapter Type: **Intel PRO/1000 MT Desktop (82540EM)**.
5. **Serial Ports**:
   - Enable Serial Port 1 (COM1, IRQ 4, I/O Port `0x3F8`).
   - Port Mode: **Raw File** (e.g. `vbox_serial.log`) or Disconnected.

### 3.2 Automated VirtualBox Testing
To run the automated VirtualBox test suite that manages VM creation, headless boot, marker verification, desktop screenshot capture, and cleanup:
```powershell
python scripts/test_vbox.py
```
To run the lifecycle and persistence test suite (cold boot, controlled shutdown, restart, and warm reset):
```powershell
python tests/test_vbox_lifecycle.py
```

