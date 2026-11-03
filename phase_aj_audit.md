# Phase AJ Audit & Verification Plan — AstraOS Realistic I/O Fault Injection, Concurrency Stress & Reliability Qualification

**Workspace:** `C:\Users\Akash\Documents\MyKernel`  
**Phase:** Phase AJ  
**Author:** Antigravity Team  
**Date:** October 30, 2026  

---

## 1. Executive Overview & Objectives

Phase AI hardened the AstraFS dual-set Copy-on-Write metadata protocol, signed generation rollover comparison, and basic ATA flush handling. However, Phase AI identified and documented specific test fidelity limitations:
1. **Mock Storage Fidelity**: The host block mock operated on a unified memory array (`s_mock_storage`) where sector writes became instantly persistent without distinguishing volatile controller write caches from non-volatile persistent storage. Consequently, unflushed dirty writes could survive simulated power loss in mock tests.
2. **ATA Flush Semantics**: The ATA driver selected flush commands without checking IDENTIFY DEVICE capability words (word 82 bit 12, word 83 bit 13), lacked checking for illegal `DRQ` assertion during non-data commands, did not check drive presence, and could not differentiate between unsupported commands and transient bus errors.
3. **Kernel Concurrency Testing**: While `g_astrafs_lock` provides mutual exclusion across AstraFS entry points, real multi-worker concurrency (concurrent reads, writes, open descriptors across renames, simultaneous `fsync` and `sync`) requires explicit verification under both multi-threaded host harnesses and guest kernel scheduler execution.
4. **End-to-End Browser Durability**: Browser bookmark tests previously verified ASTB serialization and host file replacement, but must now be qualified against the realistic volatile-cache/persistent-disk fault injection model.

Phase AJ systematically qualifies AstraOS storage reliability, fault tolerance, and concurrency invariants under realistic failure conditions.

---

## 2. Test Environment Taxonomy: Mocks vs. QEMU vs. VirtualBox

To maintain absolute documentation integrity, Phase AJ categorizes every test environment and its verified guarantees:

| Environment | Test Suites | Execution Model | Storage Guarantees Verified | Limitations |
|-------------|-------------|-----------------|-----------------------------|-------------|
| **Host Mock Harness** | `test_fs_host.c`<br>`test_block_host.c`<br>`test_bookmark_storage.c` | MinGW GCC C-based host DLL loaded by Python runner | Precise sector-level fault injection, volatile cache vs non-volatile disk separation, torn sector writes, boundary limits, ASTB checksum verification | CPU execution is host x86_64; I/O ports are mocked; does not test real hardware ATA PIO port timings or hypervisor disk backends |
| **QEMU Emulator** | `test_runner.py` (16 configs)<br>`normal` / `nofb` | 64-bit UEFI VM with IDE/ATA disk (`-drive file=disk_data.img,format=raw,if=ide,index=1`) | Real ATA PIO port I/O (`0x1F0`/`0x3F6`), 400 ns timing delays, APIC timer interrupts, paging, Ring 3 syscall traps, cold reboots | QEMU IDE emulation simulates typical hardware; writeback cache flushing depends on host OS sync semantics; does not simulate physical capacitor failure |
| **VirtualBox 7** | `test_vbox.py` (Headless EFI) | Oracle VirtualBox 7.0 EFI VM booting El Torito ISO with AHCI/IDE controllers | Real hypervisor EFI firmware execution, hardware ACPI, 86 kernel marker invariants | VirtualBox uses host-backed virtual disks; physical power loss cannot be induced directly |

---

## 3. Task 1 — ATA Flush Command Correctness Audit

### 3.1 IDENTIFY DEVICE Word Analysis
The ATA/ATAPI-6 and ATA8-ACS standards define command capability reporting in IDENTIFY DEVICE words:
- **Word 82 (Command sets supported)**:
  - Bit 14: 0, Bit 15: 1 (indicates word 82 is valid).
  - Bit 12: 1 = `FLUSH CACHE` (0xE7) supported.
- **Word 83 (Command sets supported)**:
  - Bit 14: 0, Bit 15: 1 (indicates word 83 is valid).
  - Bit 10: 1 = 48-bit Address feature set supported.
  - Bit 13: 1 = `FLUSH CACHE EXT` (0xEA) supported.
- **Word 86 (Command sets enabled)**: Bit 12: 1 = `FLUSH CACHE` enabled.
- **Word 87 (Command sets enabled)**: Bit 13: 1 = `FLUSH CACHE EXT` enabled.

### 3.2 ATA Flush Policy & Error Handling Matrix
1. **Device Presence**: If `!dev || !dev->priv`, return `BLOCK_ERR_INVALID_PARAM`. If `!d->present` or `dev->status != BLOCK_STATUS_READY`, return `BLOCK_ERR_NO_DEVICE`.
2. **Command Selection**:
   - If drive capacity $> 128\text{ GiB}$ ($> 0x0FFFFFFF$ sectors) and `d->flush_ext_supported`: emit `ATA_CMD_FLUSH_CACHE_EXT` (`0xEA`).
   - Else if `d->flush_cache_supported`: emit `ATA_CMD_FLUSH_CACHE` (`0xE7`).
   - If neither is supported: return `BLOCK_ERR_UNSUPPORTED` (never report false success).
3. **Execution & Timing**:
   - Select drive via `ATA_REG_DRIVE_HEAD`.
   - Wait for `BSY == 0` with bounded timeout (`ATA_TIMEOUT_CYCLES`).
   - Emit command byte to `ATA_REG_COMMAND`.
   - Enforce mandatory 400 ns bus assertion delay (`ata_delay_400ns`).
   - Wait for `BSY == 0` with bounded timeout.
4. **Status & Error Analysis**:
   - Flush is a **non-data command**. If `status & ATA_STATUS_DRQ` is set, the controller is in an illegal state; reject with `BLOCK_ERR_IO`.
   - If `status & (ATA_STATUS_ERR | ATA_STATUS_DF)`:
     - Read `ATA_REG_ERROR`.
     - If the command was `0xEA` and `error & ATA_ERR_ABRT` is set, and `d->flush_cache_supported` is true: fallback and retry with `0xE7`.
     - If `0xE7` also aborts, or if another error bit is set (`UNC`, `BBK`, `DF`), acknowledge the error register and return `BLOCK_ERR_IO`.
5. **AstraFS & VFS Propagation**:
   - In `astrafs_commit_metadata()` and `astrafs_sync()`, any non-zero return from `block_flush()` must immediately abort the commit, roll back in-memory changes, and propagate `VFS_ERR_IO` / `BLOCK_ERR_IO` to `sys_fsync()` and `sys_sync()`.

---

## 4. Task 2 — Realistic Disk Fault-Injection Model Design

### 4.1 Volatile Cache vs Persistent Storage Separation
To accurately evaluate crash consistency, the host test block device is restructured into two explicit physical tiers:
```text
+-------------------------------------------------------+
|                 AstraFS Filesystem                    |
+-------------------------------------------------------+
                           |
                     block_write()
                           v
+-------------------------------------------------------+
|             Mock Volatile Write Cache                 |
|  - s_volatile_cache[MOCK_TOTAL_SECTORS * 512]         |
|  - s_dirty_bitmap[MOCK_TOTAL_SECTORS / 8]             |
|  - Sector writes modify only cache & set dirty bit    |
+-------------------------------------------------------+
              |                               |
       power_loss()                     block_flush()
   (Cache discarded!)             (Dirty sectors copied)
              v                               v
+-------------------------------------------------------+
|             Mock Persistent Disk Storage              |
|  - s_persistent_storage[MOCK_TOTAL_SECTORS * 512]     |
|  - Non-volatile disk image surviving crashes          |
+-------------------------------------------------------+
```

### 4.2 Fault Injection Primitives
1. **Unflushed Power Loss (`mock_power_loss`)**:
   - Wipes `s_volatile_cache` by restoring it from `s_persistent_storage`.
   - Clears all bits in `s_dirty_bitmap`.
   - Wipes in-memory `g_astrafs` filesystem state.
   - Any writes not committed via `block_flush()` are permanently lost.
2. **Selective Flush Barrier**:
   - `block_flush()` inspects `s_dirty_bitmap`. Only marked sectors are copied to `s_persistent_storage`.
   - Configurable flush failure hook (`s_mock_fail_flush`): returns `BLOCK_ERR_IO` without copying dirty sectors.
3. **Half-Sector Torn Writes (`mock_inject_torn_write`)**:
   - Simulates write tearing by writing only 256 bytes (first half) to persistent storage, leaving the remaining 256 bytes intact or corrupt.
4. **Arbitrary Sector Failure (`mock_fail_sector`)**:
   - Allows designating specific LBAs that fail on write or read with `BLOCK_ERR_IO`.

---

## 5. Task 3 — Superblock & Metadata Recovery Qualification Matrix

The 10-checkpoint qualification matrix evaluated against the realistic cache-disk model:

| Checkpoint | Fault Injected | State in Persistent Storage | Expected Recovery Behavior | Durability Invariant |
|------------|----------------|-----------------------------|----------------------------|----------------------|
| **C1** | Crash during data extent write | Prior data intact; new extent unflushed | Remount recovers prior generation; file content unchanged | Zero data corruption |
| **C2** | Crash after data write, before pre-commit flush | New data only in volatile cache; lost on crash | Remount recovers prior generation | Zero extent leakage |
| **C3** | Crash during alternate inode table write | Inode table unflushed or partial in volatile cache | Alternate superblock has old generation; remount uses active set | Active set intact |
| **C4** | Crash during alternate bitmap write | Alternate bitmap unflushed in volatile cache | Alternate superblock not written; remount uses active set | Active set intact |
| **C5** | Pre-commit metadata flush failure | Flush returns error; commit aborted | In-memory commit halts; persistent storage untouched | Atomicity guaranteed |
| **C6** | Partial superblock write (Torn write, 256 bytes) | First 256 bytes committed, remainder old | Fletcher-32 checksum failure; fallback to other valid superblock | Checksum protection |
| **C7** | Alternate superblock written to volatile cache without flush | Superblock in volatile cache; lost on power loss | Persistent disk has old generation; remount recovers old generation | Flush required for commit |
| **C8** | Final cache flush failure | Superblock unflushed or partially committed | Remount recovers previous valid generation | No false durability |
| **C9** | Successful final flush | Both alternate tables & alternate superblock durable | Remount selects new generation; all new data accessible | Clean forward commit |
| **C10** | Newer generation has invalid extents / corrupt bitmap | New superblock valid checksum, but extents exceed disk bounds | `validate_metadata_integrity()` rejects Set; falls back to Set A | Defensive validation |

---

## 6. Tasks 4 & 5 — Kernel Concurrency, Lock Ordering & Interrupt Safety

### 6.1 Scheduler & Ring 3 Concurrency Model
1. **AstraOS Multitasking Support**:
   - The AstraOS scheduler supports multiple independent threads and user processes.
   - User processes run in Ring 3 with independent PML4 page tables (`pml4_phys`).
   - Both cooperative scheduling (`sys_yield`) and preemptive APIC timer scheduling (Vector 32) are fully supported.
2. **Lock Hierarchy & Serialization**:
   - Call hierarchy: $\text{VFS} \rightarrow \text{AstraFS} \rightarrow \text{Block Layer} \rightarrow \text{ATA Driver}$.
   - AstraFS uses a single global mutex: `g_astrafs_lock`.
   - Non-recursive: Recursive acquisition panics by design.
   - Non-barging: Waiters wait in FIFO order; mutex ownership is transferred directly.
   - Preemption safety: `mutex_lock` verifies `IF=1`, `preempt_count == 0`, and `!in_interrupt()`.
3. **Concurrency Stress Scenarios**:
   - Scenario 1: Independent worker threads reading and writing distinct files simultaneously.
   - Scenario 2: Two worker threads writing to the same file concurrently (verifying extent serialization, zero double allocations, and clean append/overwrite).
   - Scenario 3: Rename while descriptor is open (verifying descriptor continues reading original inode).
   - Scenario 4: Concurrent `fsync` and `sync` during active writes.
   - Scenario 5: Thread exit safety (verifying zero mutexes held upon exit).

---

## 7. Task 6 — End-to-End Browser Bookmark Failure Testing

The browser bookmark persistence protocol uses an atomic three-stage write pattern:
1. Serialize bookmark entries into ASTB binary format.
2. Open `/data/bookmarks.bin.tmp`, write binary stream, and call `sys_fsync()`.
3. Close descriptor and atomically rename `/data/bookmarks.bin.tmp` to `/data/bookmarks.bin`.

### Qualification Against Disk Fault Model
Inject failures at all 8 lifecycle boundaries:
1. Fail temporary file creation -> baseline bookmarks intact.
2. Fail partial temporary file write -> baseline bookmarks intact, stale `.tmp` safely ignored.
3. Power loss before `fsync` -> volatile cache discarded, baseline bookmarks intact.
4. `fsync` error returned -> save fails, baseline bookmarks intact.
5. Successful `fsync` -> data durable in `.tmp`.
6. Power loss during rename metadata update -> recovered disk retains either valid old or valid new file.
7. Power loss before final post-rename flush -> volatile cache discarded; previous stable generation recovered cleanly.
8. Complete commit and post-rename flush -> new bookmarks recovered with valid ASTB checksum and exact URLs/titles.

---

## 8. Tasks 7 & 8 — Performance, Resource Bounds & Full Regression Matrix

1. **Resource Bounds**:
   - Repeated Copy-on-Write overwrites (50+ cycles) must yield zero leaked sectors in the allocation bitmap.
   - File size bounds (4 MiB max) and total file count (64 files max) must be strictly enforced.
2. **Regression Matrix**:
   - Host test suites: AstraFS (26+ tests), Bookmark Storage (15+ tests), Block Device (11+ tests), VFS/Initramfs, Adversarial Networking, JavaScript Fuzz, Cryptography.
   - QEMU: 512M `nofb`, 512M `normal`, 16-configuration automated matrix (`test_runner.py --all`).
   - VirtualBox 7: Headless EFI automated test (`test_vbox.py`, 86 markers).

---

## 9. Conclusion

Phase AJ establishes realistic, hardware-grade fault injection and rigorous concurrency qualification. Following this audit plan, implementation begins with ATA flush correctness (Task 1), followed by the two-tier volatile-cache/persistent-storage fault model (Task 2 & 3), concurrency and synchronization verification (Tasks 4 & 5), and end-to-end browser qualification (Task 6).

