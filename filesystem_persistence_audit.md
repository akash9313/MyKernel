# AstraOS — Filesystem & Storage Persistence Comprehensive Audit

**Date:** 2026-11-17  
**Project:** AstraOS Operating System (x86_64)  
**Location:** `C:\Users\Akash\Documents\MyKernel`

---

## 1. Storage Backend Architecture by Directory

| Directory Path | Storage Backend | Durability / Lifecycle | Real Write Support | Underlying Constraints & Capacities |
| :--- | :--- | :--- | :--- | :--- |
| `/` (root) | Initramfs / VFS Root | Read-only in-memory archive unpacked from Limine boot module | Read-only (writes shadowed to RAMFS) | Fixed boot image (~8.3 MiB) |
| `/bin` | Initramfs / ELF Binaries | Read-only system binaries compiled at build time | Read-only | Immutable system partition |
| `/assets` | Initramfs / System Assets | Read-only raw wallpaper assets (`cosmic_vista.raw`, 8.29 MiB) | Read-only | Immutable system partition |
| `/etc` | Initramfs / System Config | Read-only base configs and SSL certificates (`/etc/ssl/certs/ca.crt`) | Read-only | Immutable system partition |
| `/www` | Initramfs / Web Assets | Read-only HTML, CSS, JS test pages for AstraBrowser | Read-only | Immutable system partition |
| `/data` (top-level) | **AstraFS v1** on Block Device (`hd0` VirtualBox / `hd1` QEMU) | **Persistent Block Storage** (Dual-Set A/B transactional commit, Fletcher-32 checksums, LBA 128+) | **Read / Write / Create / Unlink / Rename** | Max 64 files/dirs, 48-char names, up to 65,536 sectors (32 MiB) |
| `/data/<subdirs>` (e.g. `/data/docs`, `/data/home/akash`) | **AstraFS v1 Hierarchical** | **Persistent Block Storage** (Committed to disk with `ASTRAFS_FLAG_DIR`) | **Read / Write / Create / Rmdir / Rename** | Preserved across cold restart & power cycle |
| Paths outside `/data` | **RAMFS (`g_mutable_files`)** | **Volatile (In-Memory)** — Disappears upon reboot | Read / Write | 32 mutable slots, 16 KiB per file max |

---

## 2. Root Cause Audit: Defects Diagnosed & Repaired

### Defect 1: VirtualBox Primary Master `hd0` Block Driver Misclassification
- **Diagnosis:**
  In `kernel/drivers/ata.c`, `c=0, d=0` was flagged as `is_boot_device = true` and `read_only = true`. This logic assumed that `c=0, d=0` is always QEMU's read-only FAT16 ESP. However, in VirtualBox EFI, the primary master disk (`IDE-0-0`) is `astraoss.vdi` (`VBOX HARDDISK`), while the ATAPI CD-ROM (`IDE-1-0`) is `astraos.iso`.
- **Consequence:**
  VirtualBox `hd0` was blocked from writing with `BLOCK_ERR_READ_ONLY`, forcing AstraOS to fall back to volatile RAMFS.
- **Resolution:**
  `ata.c` now differentiates QEMU boot ESP from VirtualBox virtual disk:
  ```c
  bool is_qemu_boot = (c == 0 && d == 0 && ata_str_contains(drv->model, "QEMU"));
  drv->is_boot_device = is_qemu_boot;
  drv->read_only = is_qemu_boot;
  ```
  VirtualBox `hd0` is recognized as writable persistent storage.

### Defect 2: Dual-Pass Drive Discovery & AstraFS Mount Binding
- **Diagnosis:**
  In `kernel/kernel.c`, storage mount was hardcoded to `block_get_device_by_name("hd1")`. If `hd1` was absent (VirtualBox only configures `hd0`), the persistent mount was aborted.
- **Resolution:**
  Implemented dual-pass drive discovery in `kernel.c`:
  Pass 1 scans all discovered block devices for an existing valid AstraFS superblock set (A or B).
  Pass 2 falls back to the first available non-boot, writable block device. In VirtualBox it binds `hd0`, in QEMU it binds `hd1`.

### Defect 3: AstraFS Volume Size Overflowing 16-Sector Allocation Bitmap
- **Diagnosis:**
  In `kernel/include/kernel/astrafs.h`, `ASTRAFS_BITMAP_SECTORS` is fixed at 16 sectors ($16 \times 512 = 8,192\text{ bytes} = 65,536\text{ bits}$).
  In VirtualBox, `dev->total_sectors` on `hd0` is 4,194,304 sectors (2048 MB).
  When formatting or validating, loops iterated up to `dev->total_sectors`, reading out of bounds past the 8,192-byte bitmap buffer, reading corrupt bits and causing `validate_metadata_integrity()` to fail.
- **Resolution:**
  Added `#define ASTRAFS_MAX_SECTORS (ASTRAFS_BITMAP_SECTORS * ASTRAFS_SECTOR_SIZE * 8)`.
  All bitmap checks, allocations, and superblock fields are strictly bounded by `ASTRAFS_MAX_SECTORS` (65,536 sectors = 32 MiB). VirtualBox volumes format and mount cleanly.

### Defect 4: Subdirectory Creation and File Routing Bypassed into RAMFS
- **Diagnosis:**
  In `kernel/fs/vfs.c`, `vfs_open` inspected path for `/` after `/data/` (`has_sub`). If true, it bypassed AstraFS and routed to RAMFS `g_mutable_files`. Furthermore, `vfs_mkdir` had no backend routing to AstraFS.
- **Resolution:**
  - Implemented `astrafs_mkdir`, `astrafs_rmdir`, and `astrafs_unlink`.
  - Added `#define ASTRAFS_FLAG_DIR 0x80000000U`.
  - Removed `has_sub` bypass in `vfs_open`.
  - Routed `vfs_mkdir`, `vfs_rmdir`, `vfs_unlink`, and `vfs_rename` directly to AstraFS for `/data/...`.

### Defect 5: PS/2 Mouse Driver Packet Framing Desynchronization
- **Diagnosis:**
  In `kernel/drivers/ps2.c`, when `data & 0xC0` occurred during byte 0, the driver discarded the byte without resetting `g_mouse_cycle = 0`. This caused all subsequent 3-byte packets to be parsed with a 1- or 2-byte framing phase shift, causing cursor freezes ("stuck in center").
- **Resolution:**
  Removed the premature drop, clamped coordinate deltas to $[-127, +127]$, and tracked button transitions for `MOUSE_BTN_RIGHT` and `MOUSE_BTN_MIDDLE`.

### Defect 6: Memory Quota & Headroom Exhaustion
- **Diagnosis:**
  The kernel heap arena is fixed at 64 KiB (`HEAP_ARENA_SIZE`). Allocating large GUI buffers on the heap exhausted `kmalloc`, causing panic in multi-core tests.
- **Resolution:**
  All tab buffers in AstraEdit and item buffers in AstraFiles are dynamically allocated from physical page frames via `pmm_alloc_contiguous()` and mapped through Limine HHDM, using zero bytes of the 64 KiB kernel heap and keeping kernel ELF headroom $> 55\text{ KiB}$.
