# AstraOS v1.0 Hardware Compatibility Guide

## 1. Supported and Qualified Environments

AstraOS v1.0 hardware support claims are strictly evidence-based and limited to platforms and configurations independently qualified through deterministic test suites.

| Subsystem / Peripheral | Supported Specification | Tested Environments | Evidence Status |
|---|---|---|---|
| **CPU Architecture** | x86-64 (AMD64 / Intel 64) | 1, 2, 4, 8 cores under QEMU and VirtualBox 7 | **QUALIFIED** |
| **Firmware Interface** | UEFI 2.x 64-bit | EDK2 OVMF x86-64 & VirtualBox 7 EFI | **QUALIFIED** |
| **System Memory** | 256 MiB to 1024 MiB RAM | Tested across 128M, 256M, 512M, 1024M | **QUALIFIED** |
| **Display / Graphics** | UEFI GOP Linear Framebuffer | $1280 \times 800$ & $1024 \times 768$ (32 bpp ARGB) | **QUALIFIED** |
| **Interrupt Controller** | Local APIC + I/O APIC | Monotonic APIC timer & SMP IPI delivery | **QUALIFIED** |
| **Storage Controller** | IDE / ATA PIO (0x1F0, 0x170) | Primary Master (`hd0`), Secondary Master (`hd1`)| **QUALIFIED** |
| **Network Interface** | Intel 82540EM Gigabit Ethernet | QEMU E1000 & VirtualBox Intel PRO/1000 MT | **QUALIFIED** |
| **Input Devices** | Standard PS/2 Keyboard & Mouse | QEMU std PS/2 & VirtualBox PS/2 emulation | **QUALIFIED** |
| **Serial Diagnostics** | 16550 UART (COM1, 0x3F8) | 115200 baud, 8 data bits, no parity, 1 stop bit | **QUALIFIED** |

---

## 2. Unsupported and Unqualified Hardware

The following hardware interfaces and architectures are explicitly **NOT SUPPORTED** in AstraOS v1.0:

1. **Non-x86 Architectures**:
   - ARM64 (AArch64), RISC-V, and 32-bit x86 (IA-32) are not supported.
2. **UEFI Secure Boot**:
   - Secure Boot signature verification is not implemented. Firmware Secure Boot **must be disabled**.
3. **Legacy BIOS Boot**:
   - AstraOS does not support 16-bit real-mode or legacy CSM boot records. A 64-bit UEFI environment is required.
4. **Native AHCI (PCI SATA) & NVMe Storage**:
   - AstraOS currently communicates with disks via standard IDE/ATA PIO registers.
   - When running on pure AHCI or NVMe controllers without IDE emulation, AstraOS detects 0 block devices and safely operates in volatile RAMFS mode (`ASTRA_AY_STORAGE_DISCOVERY_OK`).
5. **USB Host Controllers (xHCI, EHCI, UHCI)**:
   - USB keyboards and mice require BIOS/firmware legacy PS/2 emulation. Native USB drivers are not implemented.
6. **Wireless Networking (Wi-Fi / Bluetooth)**:
   - Network connectivity is restricted to wired Ethernet via the Intel 82540EM (E1000) driver.
7. **Hardware GPU Acceleration (DirectX, OpenGL, Vulkan)**:
   - Graphics rendering is strictly software-based double buffering on top of the UEFI linear framebuffer.

