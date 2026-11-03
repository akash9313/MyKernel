# Phase AH — AstraOS Independent Storage Audit, Crash Recovery & Concurrency Verification Report

## Executive Summary

Phase AH conducted an independent, exhaustive audit of AstraOS's persistent storage subsystem following Phase AG. The audit rigorously investigated the invariants of **AstraFS v1**, the VFS abstraction, block/ATA drivers, the syscall layer (`SYS_FSYNC`, `SYS_SYNC`, `SYS_RENAME`), and user-space AstraBrowser bookmark persistence.

The audit identified and resolved real correctness, security, and recovery defects in the Copy-on-Write (CoW) commit lifecycle and namespace boundaries. All invariants were proven through deterministic host-side failure injection suites, QEMU multi-configuration matrices, and Oracle VirtualBox 7 EFI boot tests.

---

## 1. Key Audit Findings & Defect Resolutions

### Defect 1 (CRITICAL): Premature Extent Deallocation & In-Memory Desynchronization on Commit Failure
- **Location**: `kernel/fs/astrafs.c:788-807`
- **Root Cause**:
  In `astrafs_write()`, the old extent `old_lba` was marked free in `g_astrafs.bitmap` and `ino->start_lba` was reassigned to `new_lba` *before* invoking `astrafs_commit_metadata()`. If `astrafs_commit_metadata()` encountered an I/O or flush failure, the function returned `bytes_written` instead of the error code. The caller believed the write had succeeded, but in-memory metadata was desynchronized from the on-disk state. A subsequent file write could allocate `old_lba` from the free bitmap while the on-disk superblock still referenced it, causing catastrophic cross-file corruption and extent collisions.
- **Fix**:
  - Implemented transactional state snapshots (`old_lba`, `old_count`, `old_file_size`, `old_mtime`, `old_checksum`, `old_node_size`).
  - If `astrafs_commit_metadata()` returns an error:
    - `new_lba` is freed in `g_astrafs.bitmap`.
    - `old_lba` is re-marked as allocated in `g_astrafs.bitmap`.
    - All inode fields are reverted to their pre-write state.
    - The commit error code is returned directly to the caller.

### Defect 2 (HIGH): Missing Rollback in `astrafs_truncate` and `astrafs_rename`
- **Location**: `kernel/fs/astrafs.c:830-848` and `980-1008`
- **Root Cause**:
  - `astrafs_truncate(node, 0)` freed the extent and zeroed the inode before commit. If commit failed, the file was wiped in memory while persisting on disk.
  - `astrafs_rename()` zeroed the source inode and freed the destination extent before commit. If commit failed, the source was lost from memory while destination data was corrupted.
- **Fix**:
  Added transactional rollback snapshots to both functions: if `astrafs_commit_metadata()` returns non-zero, both inodes and bitmap allocations are restored to their exact pre-operation state before unlocking `g_astrafs_lock`.

### Defect 3 (MEDIUM): Cross-Filesystem Rename Asymmetry in VFS
- **Location**: `kernel/fs/vfs.c:526-545`
- **Root Cause**:
  `vfs_rename()` checked only whether `old_path` began with `/data/`. When `old_path` was a root/RAMFS path (e.g. `/etc/bookmarks.bin`) and `new_path` was `/data/bookmarks.bin`, the operation bypassed AstraFS and executed a RAMFS rename, polluting the `/data/` mountpoint with a volatile RAMFS file.
- **Fix**:
  Enforced strict bidirectional cross-filesystem rejection when AstraFS is mounted:
  ```c
  if (astrafs_is_mounted()) {
      bool old_is_data = (str_ncmp(old_path, "/data/", 6) == 0);
      bool new_is_data = (str_ncmp(new_path, "/data/", 6) == 0);
      if (old_is_data != new_is_data) {
          return VFS_ERR_UNSUPPORTED; /* Cannot rename cross-filesystem */
      }
      if (old_is_data && new_is_data) {
          return astrafs_rename(old_path + 6, new_path + 6);
      }
  }
  ```

### Defect 4 (CORRECTNESS): Syscall Dispatcher Interrupt State
- **Location**: `kernel/process/syscall.c:729-842`
- **Root Cause**:
  Vector 128 is an x86-64 Interrupt Gate (`0xEE`), which clears the Interrupt Flag (`IF=0`) upon trap from Ring 3. Invoking blocking operations (e.g. mutex acquisition or sleep) while `IF=0` violates kernel sync invariants.
- **Fix**:
  Enabled `sti()` at entry to `syscall_dispatcher` and restored `cli()` before returning via `iretq`. Confirmed that no syscall handlers execute while holding kernel spinlocks, allowing safe mutex sleeping and APIC timer tick delivery in process context.

---

## 2. Deterministic 10-Point Crash-Fault Matrix

AstraFS v1's dual-set Copy-on-Write commit protocol was tested across 10 deterministic failure injection points in `tests/test_fs_host.c` (Test 25):

| Checkpoint | Injection Point | Disk State on Interruption | Recovery Selection | Outcome |
|:---:|:---|:---|:---|:---|
| **C1** | Extent allocated in RAM; crash before disk write | No new sectors written | Prior Active Set (Gen N) | Intact pre-crash data; zero leaks |
| **C2** | Partial payload sector write to new extent | Partial new data at uncommitted LBA | Prior Active Set (Gen N) | Intact pre-crash data; zero leaks |
| **C3** | Full payload written before pre-commit flush | Full payload at uncommitted LBA | Prior Active Set (Gen N) | Intact pre-crash data; zero leaks |
| **C4** | Pre-commit payload flush completed | Flushed payload at uncommitted LBA | Prior Active Set (Gen N) | Intact pre-crash data; zero leaks |
| **C5** | Alternate inode table partially written/corrupted | Damaged alternate inode table | Prior Active Set (Gen N) | Alternate rejected; active intact |
| **C6** | Alternate inode written, alternate bitmap corrupt | Damaged alternate bitmap | Prior Active Set (Gen N) | Alternate rejected; active intact |
| **C7** | Alternate inode & bitmap written; superblock unwritten | Alternate superblock has old Gen N-1 | Prior Active Set (Gen N) | Active set chosen; pre-crash intact |
| **C8** | Alternate superblock partially written (bad checksum) | Checksum validation failure | Prior Active Set (Gen N) | Fallback to active set; pre-crash intact |
| **C9** | Alternate superblock fully written, before final flush | Valid alternate generation N+1 | Alternate Set (Gen N+1) | Roll forward to new committed data |
| **C10** | Final hardware cache flush succeeds | Both sets valid; alternate is Gen N+1 | Alternate Set (Gen N+1) | Durable byte-for-byte committed update |

**Result**: All 10 stages passed cleanly. Extent accounting (`sb.used_sectors == calculated_used_sectors`) was verified with zero leaked extents and zero double allocations.

---

## 3. Concurrency, Locking & Hardware Contract

1. **Kernel Mutex Discipline**:
   - `g_astrafs_lock` is acquired on entry to all public AstraFS routines and released on every exit path.
   - Internal helper routines (`astrafs_commit_metadata`, `bitmap_alloc_extent`, `bitmap_free_extent`) assume the lock is already held.
   - Non-recursive: Recursive calls trigger an immediate diagnosed panic.
2. **ATA Flush Cache Contract**:
   - `ata_flush()` sends ATA command `0xE7` (`FLUSH CACHE`) and polls status until `BSY` clears.
   - Hardware boundary: In virtualized environments (QEMU/VirtualBox), durability against host OS crash requires writeback cache flushing. AstraOS guarantees delivery of the ATA flush command to the controller.

---

## 4. Verification Evidence & Test Results

### 1. Host Unit Test Suites
- **`tests/test_fs_host.py`**: **PASS (25/25 tests)**
  - Test 1-23: Formatting, mounting, bounds, CoW overwrites, multi-sector extents, fsync, readdir.
  - Test 24: In-memory transactional rollback (write, truncate, rename).
  - Test 25: Deterministic 10-point crash-fault matrix (C1..C10).
- **`tests/test_bookmark_storage.py`**: **PASS (14/14 tests)**
  - Test 1-12: ASTB v1 binary format, Fletcher-32 checksums, 32-capacity bound, null-termination, non-destructive loading.
  - Test 13: Stale `.tmp` file defense during load.
  - Test 14: Atomic `.tmp` -> `fsync` -> `rename` failure immunity.
- **`tests/test_vfs_host.py`**: **PASS** (Synthetic initramfs, path lookup, RAMFS fallback, rename).
- **`tests/test_block_host.py`**: **PASS (11/11 tests)** (Device registration, LBA bounds, cache flush).
- **`tests/test_browser_networking.py`**: **PASS (17/17 tests)** (HTTP/HTTPS, chunked encoding, TLS 1.3).
- **`tests/test_crypto_host.py`**: **PASS** (SHA-256, HMAC, HKDF, ChaCha20-Poly1305, X25519, X.509).

### 2. QEMU Test Matrix
- **`nofb` Safe Console Mode (512M)**: **PASS**
  - Markers verified: `ASTRA_FS_OK`, `ASTRA_FS_PERSISTENCE_OK`, `ASTRA_FS_COLD_REBOOT_CONTENTS_VERIFIED`, `ASTRA_BROWSER_BOOKMARKS_COLD_REBOOT_OK`.
  - Serial monitor `fsinfo` verified: Set B, generation 128, 5 files, 24 sectors used.
- **`normal` GUI Mode (512M)**: **PASS**
  - Full GUI compositor, window manager, and user-space AstraBrowser.
  - Cold-reboot bookmarks loaded and verified.
- **Full 16-Configuration Matrix (`tests/test_runner.py --all`)**: **PASS (16/16)**
  - Tested across 128MB, 256MB, 512MB, 1024MB RAM configurations.
  - Tested across all diagnostic fault configurations (`test=exception`, `test=df_fault`, `ro_fault`, `nx_fault`, guard page checks).
  - Zero warnings, zero errors.

### 3. Oracle VirtualBox 7 EFI Hardware Test
- **`scripts/test_vbox.py`**: **PASS (86/86 markers)**
  - Headless EFI boot of `build/astraos.iso` with Intel AHCI SATA and E1000 NAT.
  - All 86 boot markers verified from `ASTRA_IDT_OK` through `ASTRA_BROWSER_ISOLATION_OK`.
  - Desktop screenshot captured cleanly at `build/vbox_desktop.png`.

---

## 5. Artifacts & Documentation Updated

1. **`phase_ah_audit.md`**: Detailed architectural audit document in the conversation artifact directory.
2. **`docs/architecture.md`**: Added Section 44 detailing the independent audit findings, transactional rollback architecture, cross-FS path validation, and the 10-point crash-fault matrix.
3. **`docs/status.md`**: Updated with Phase AH status rows for Transactional Rollback, Cross-FS Validation, Syscall Concurrency, 10-Point Crash Matrix, and Bookmark Failure Immunity.

