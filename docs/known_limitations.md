# AstraOS v1.0 Known Limitations & Architectural Boundaries

## 1. Installation & Disk Management
- **No Bare-Metal Installer**: AstraOS operates strictly as a Live-Boot operating system. There is no automated or manual disk installer, partition editor, or raw disk installer CLI.
- **Boot Media Immutability**: The boot drive (`hd0`) is enforced read-only. System updates require writing a new ISO/ESP image.
- **Secondary Disk Required for Persistence**: Persistent data is supported exclusively on a separate secondary disk (`hd1`) formatted with AstraFS. If no secondary disk is detected, AstraOS operates in volatile RAMFS mode.

---

## 2. Firmware & Boot
- **No UEFI Secure Boot**: AstraOS binaries (`BOOTX64.EFI`, `kernel.elf`) are not signed with Microsoft Third-Party UEFI CA keys. Secure Boot must be disabled in firmware setup.
- **UEFI Only (No Legacy BIOS)**: AstraOS cannot boot on systems lacking 64-bit UEFI firmware or CSM configurations without UEFI support.

---

## 3. Storage & Peripherals
- **IDE/ATA PIO Mode Only**: AstraOS communicates with storage drives using standard IDE/ATA PIO ports (0x1F0, 0x170). There are no native PCI AHCI (SATA) or NVMe drivers.
- **Legacy PS/2 Input Emulation Required**: AstraOS interacts with keyboard and mouse via standard PS/2 I/O ports (0x60, 0x64). Systems with native USB controllers must provide legacy PS/2 emulation in firmware.
- **Wired Ethernet Only**: Network support is restricted to Intel 82540EM (E1000) PCI controllers. Wi-Fi and Bluetooth controllers are not supported.

---

## 4. Scheduling & Process Management
- **Maximum Process Limit**: The process table is bounded to 16 concurrent processes (`PROCESS_MAX_PROCESSES = 16`).
- **Maximum CPU Limit**: The SMP subsystem supports up to 8 logical CPU cores (`MAX_CPUS = 8`).
- **Single Process Quantum**: Preemptive context switching occurs via LAPIC timer ticks at 100 Hz. Complex interactive multi-threaded workloads may experience latency under extreme CPU saturation.

---

## 5. Web Browser & Rendering Engine
- **Subset HTML/CSS**: AstraBrowser supports standard HTML elements (`<h1>`-`<h6>`, `<p>`, `<a>`, `<button>`, `<input>`, `<img>`), inline/block layout, font styling, colors, and margins. Advanced CSS features (Flexbox, CSS Grid, animations, `@media` queries) are not supported.
- **Uncompressed BMP Images Only**: Image support is limited to 24bpp and 32bpp uncompressed BMP files. JPEG, PNG, and WebP decoding are not implemented.
- **Limited JavaScript Engine**: Embedded JavaScript engine supports basic expressions, variables, button click events, and DOM manipulation. Full ECMAScript 6+ standard compliance is not implemented.

