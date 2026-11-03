# Phase AX — Storage Integrity, Transaction Safety & Crash Recovery Audit

## 1. Executive Summary & Audit Scope

This document records the comprehensive, source-level storage integrity and crash recovery audit conducted for AstraOS as part of **Phase AX — Storage Integrity, Transaction Safety & Crash Recovery Qualification**.

The audit examined every component of the AstraOS storage stack:
1. **Block Layer & Device Drivers**: Parameter checking, bounds validation, integer overflow handling, ATA PIO commands (`0x20` READ, `0x30` WRITE), ATA cache flushing (`0xE7` FLUSH CACHE, `0xEA` FLUSH CACHE EXT), hardware fault flags (`ATA_STATUS_ERR`, `ATA_STATUS_DF`, `ATA_STATUS_DRQ`), device timeouts, and multi-core spinlock serialization (`g_ata_lock`).
2. **AstraFS v1 Metadata Architecture**: Dual-set alternating metadata layout (Superblock A/B, Inode Table A/B, Allocation Bitmap A/B), 128-byte packed inodes, 512-byte packed superblocks, Fletcher-32 checksums, and mount-time metadata validation (`validate_metadata_integrity`).
3. **Transaction & Crash Recovery Protocol**: Copy-On-Write extent allocation, payload pre-commit disk flushes, alternate metadata staging, generation incrementing, and power-loss recovery across all commit stages.
4. **Virtual File System (VFS) & RAMFS**: Canonical path validation, ASCII control character rejection, open file descriptor table (`g_file_table`), file descriptor ownership (`owner_pid`), mutable RAM-backed file structures, and cross-filesystem rename restrictions.
5. **Concurrency & Lock Hierarchy**: Thread safety across VFS (`g_vfs_lock`), AstraFS (`g_astrafs_lock` mutex), and ATA hardware (`g_ata_lock` spinlock), verifying strict adherence to the L1 -> L2 -> L5 acyclic lock hierarchy.
6. **Existing Fault-Injection Harnesses**: Evaluation of host test suites (`test_block_host.py`, `test_fs_host.py`, `test_bookmark_storage.py`) simulating volatile write caches, torn writes, out-of-bounds extents, and generation rollover.

---

## 2. Block Device & ATA Driver Reliability Audit

### 2.1 Parameter Checking and Bounds Protection (`kernel/drivers/block.c`)
- **Null & Status Checks**: `block_read()`, `block_write()`, and `block_flush()` verify `dev != NULL`, `dev->status == BLOCK_STATUS_READY`, and `buf != NULL`.
- **Zero-Count I/O**: `count == 0` returns `0` immediately without dispatching I/O requests.
- **Integer Overflow Protection**: Evaluates `lba + (u64)count < lba`, returning `BLOCK_ERR_OVERFLOW` if unsigned 64-bit integer addition wraps.
- **Out-of-Bounds Enforcement**: Evaluates `lba + (u64)count > dev->total_sectors`, returning `BLOCK_ERR_OUT_OF_BOUNDS`.
- **Media Protection**: Evaluates `dev->read_only || dev->is_boot_device`, returning `BLOCK_ERR_READ_ONLY` before any write attempt on protected devices.

### 2.2 ATA PIO Hardware Operation & Serialization (`kernel/drivers/ata.c`)
- **Concurrency Serialization**: All entry points (`ata_op_read_sectors`, `ata_op_write_sectors`, `ata_op_flush`) acquire `g_ata_lock` via `spinlock_acquire_irqsave(&g_ata_lock)` and release it via `spinlock_release_irqrestore(&g_ata_lock, flags)`. This prevents concurrent access to ATA I/O port registers across multi-core CPUs.
- **Bus Timing Delays**: After writing to drive selection (`ATA_REG_DRIVE_HEAD`) or command registers (`ATA_REG_COMMAND`), the driver performs `ata_delay_400ns()` by reading the alternate status register 4 times, fulfilling the ATA specification minimum bus stabilization delay.
- **Bounded Polling**: `ata_wait_bsy()` and `ata_wait_drq()` poll for up to `ATA_TIMEOUT_CYCLES` (200,000 iterations) with `io_wait()`. If `ATA_STATUS_BSY` does not clear, the function aborts with `BLOCK_ERR_TIMEOUT` without deadlocking the kernel.
- **Hardware Error Inspection**: On error, the status register flags `ATA_STATUS_ERR` and `ATA_STATUS_DF` (Drive Fault) are inspected. If either is asserted, `inb(ATA_REG_ERROR)` reads the controller error code and returns `BLOCK_ERR_IO`.

### 2.3 Hardware Cache Flush & Durability Guarantees
- **Flush Command Selection**:
  - For standard drives: issues `0xE7` (`ATA_CMD_FLUSH_CACHE`).
  - For 48-bit LBA drives exceeding 128 GiB with `flush_ext_supported`: issues `0xEA` (`ATA_CMD_FLUSH_CACHE_EXT`).
  - Automatic Fallback: If `0xEA` is aborted by the drive controller (`ATA_ERR_ABRT`) and the drive supports `0xE7`, the driver automatically falls back to `0xE7`.
- **Non-Data Command Invariant**: Because flush is a non-data ATA command, the controller must never assert `ATA_STATUS_DRQ`. The driver explicitly checks `if (status & ATA_STATUS_DRQ)`, treating anomalous DRQ assertions as `BLOCK_ERR_IO`.
- **Durability Guarantee**: AstraFS claims durability only after `block_flush()` completes successfully, ensuring all volatile drive cache contents are committed to non-volatile magnetic or solid-state media.

---

## 3. AstraFS Architecture & Metadata Integrity Audit

### 3.1 On-Disk Layout
- **LBA 0**: MBR / Partition Table Header.
- **LBA 1**: Superblock Set A (512 bytes).
- **LBA 2**: Superblock Set B (512 bytes).
- **LBA 16..31**: Inode Table Set A (16 sectors = 8,192 bytes = 64 inodes * 128 bytes).
- **LBA 32..47**: Inode Table Set B (16 sectors).
- **LBA 48..63**: Allocation Bitmap Set A (16 sectors = 8,192 bytes = 65,536 bits).
- **LBA 64..79**: Allocation Bitmap Set B (16 sectors).
- **LBA 128..N**: Data Extent Region.

### 3.2 Superblock & Inode Integrity
- **Superblock Header**:
  - `magic == 0x41534653` ("ASFS").
  - `version == 1`.
  - `generation`: Monotonic 64-bit sequence counter.
  - `sector_size == 512`.
  - `data_start_lba == 128`.
  - `file_count`: Active file tally ($\le 64$).
  - `used_sectors`: Total data sectors allocated.
  - `checksum`: Fletcher-32 computed over the first 508 bytes.
- **Inode Entry (`astrafs_inode_t`, 128 bytes)**:
  - `name`: Null-terminated string ($< 48$ characters).
  - `in_use`: 1 if active, 0 if free.
  - `file_size`: Byte size ($\le 4 \text{ MiB}$).
  - `start_lba`: Beginning sector in data region ($\ge 128$).
  - `sector_count`: Contiguous sector count ($\ge \lceil \text{file\_size} / 512 \rceil$).
  - `data_checksum`: Fletcher-32 of file payload.
  - `created_time`, `modified_time`: Commit generation numbers.

### 3.3 Mount-Time Metadata Consistency Verification (`validate_metadata_integrity`)
Before accepting a superblock as valid, `validate_metadata_integrity()` executes the following checks:
1. Superblock magic, version, sector size, and total sector bounds.
2. Superblock Fletcher-32 checksum recalculation and match.
3. Allocation bitmap reserved sectors ($0..127$) must all be set to 1.
4. Active file iteration across all 64 inodes:
   - File name length ($1 \le \text{len} < 48$).
   - File size bounds ($\le 4 \text{ MiB}$).
   - Sector count sufficiency ($\text{sector\_count} \ge \lceil \text{file\_size} / 512 \rceil$).
   - Extent boundary checks ($128 \le \text{start\_lba} \le \text{total\_sectors} - \text{sector\_count}$).
   - Extent overlap detection: Pairwise collision check against all previously validated active inodes.
   - Allocation bitmap verification: Ensures every sector within the extent is marked as 1 in the bitmap.
5. Inode count match: Verifies $\sum \text{in\_use} == \text{sb}->\text{file\_count}$.
6. Sector count match: Verifies $\sum \text{sector\_count} == \text{sb}->\text{used\_sectors}$.

---

## 4. Transaction Lifecycle & Crash Recovery Audit

### 4.1 Copy-On-Write (CoW) Data Modification Sequence
In `astrafs_write()`:
1. **Extent Allocation**: Allocates a fresh, contiguous extent `new_lba` from `g_astrafs.bitmap`.
2. **Payload Staging**:
   - Copies unmodified sectors from `old_lba` to `new_lba`.
   - Zero-fills any newly extended sectors.
   - Writes new data into `new_lba`.
3. **Pre-Commit Disk Flush**: Calls `block_flush(g_astrafs.dev)`. Ensures payload sectors are written to physical media *before* any metadata pointer is updated.
   - *Failure Recovery*: If flush fails, `new_lba` is freed from `g_astrafs.bitmap`, and the operation aborts without modifying in-memory inodes.
4. **Metadata Snapshot**: Takes a local snapshot of `old_lba`, `old_count`, `old_file_size`, `old_mtime`, `old_checksum`, and `old_node_size`.
5. **In-Memory Update**: Inode updated to reference `new_lba`; old extent freed in in-memory bitmap.
6. **Dual-Set Metadata Commit (`astrafs_commit_metadata`)**:
   - Staged write of Inode Table to alternate LBA (`alt_inode_lba`).
   - Staged write of Allocation Bitmap to alternate LBA (`alt_bitmap_lba`).
   - Intermediate Disk Flush (`block_flush`).
   - Alternate Superblock preparation (`generation = current + 1`, checksum recalculated).
   - Alternate Superblock write (`alt_sb_lba`).
   - Final Disk Flush (`block_flush`).
   - In-memory active set switch (`g_astrafs.active_set = alt_set`).
7. **Rollback on Commit Failure**: If any block write or flush fails during `astrafs_commit_metadata()`, the in-memory metadata is rolled back to the pre-transaction snapshot: `new_lba` is freed, `old_lba` is re-marked as used in the bitmap, and inode fields are restored.

### 4.2 Crash Interruption State Matrix
| Interruption Point | On-Disk State | Mount-Time Recovery Outcome | Consistency Guarantee |
|---|---|---|---|
| **Before write starts** | Set A active; disk untouched | Mounts Set A (Gen N) | Complete prior state intact |
| **During payload write** | Uncommitted sectors in data region | Mounts Set A (Gen N); incomplete extent ignored | Complete prior state intact |
| **During pre-commit flush** | Incomplete/unflushed data sectors | Mounts Set A (Gen N); uncommitted sectors ignored | Complete prior state intact |
| **During alternate inode write** | Incomplete Inode Table B on disk | Superblock B not committed; mounts Set A | Complete prior state intact |
| **During alternate bitmap write** | Incomplete Bitmap B on disk | Superblock B not committed; mounts Set A | Complete prior state intact |
| **During pre-SB flush** | Tables written, Superblock B not written | Mounts Set A (Gen N) | Complete prior state intact |
| **During Superblock write (Torn write)** | Corrupt Superblock B (first 256 bytes) | Checksum mismatch on Set B; mounts Set A | Complete prior state intact |
| **Before final flush (Volatile cache)** | Superblock B in cache, power loss | Cache lost; mounts durable Set A on disk | Complete prior state intact |
| **After final flush** | Superblock B durable (Gen N+1) | Both valid; selects Gen N+1 (Set B) | Complete new state committed |

---

## 5. Identified Deficiencies & Targeted Remediation Plan

### Finding AX-1: Unchecked Orphan Bits in Allocation Bitmap Validation
- **Component**: `kernel/fs/astrafs.c` (`validate_metadata_integrity`)
- **Severity**: High
- **Issue**: `validate_metadata_integrity()` iterates over active inodes and checks that each inode's extent sectors are marked as used in the bitmap. However, it does not scan the data region of the bitmap to verify that NO extraneous/orphan bits are marked as used. If a corrupted bitmap has orphan bits set in unreferenced data sectors, `calculated_used_sectors` would still match `sb->used_sectors`, allowing leaked blocks to persist silently.
- **Remediation**: Add a comprehensive bitmap audit loop that counts all set bits from `ASTRAFS_DATA_START_LBA` to `sb->total_sectors`. Assert that `bitmap_set_bits == calculated_used_sectors`. Any orphan bit will trigger validation failure and cause the mount logic to fall back to the intact alternate set.

### Finding AX-2: Open File Descriptor Invalidation During Atomic Rename/Replacement
- **Component**: `kernel/fs/vfs.c` (`vfs_rename`)
- **Severity**: Medium
- **Issue**: In `astrafs_rename()`, when `dst_existed == true`, the source inode and its VFS node `g_astrafs.vfs_nodes[src_slot]` are zeroed out while the file data is transferred to `dst_slot`. If a user process had opened the file under `old_path`, its file descriptor (`g_file_table[fd].node`) pointed to `&g_astrafs.vfs_nodes[src_slot]`. Following the rename, operations on `fd` would fail with `VFS_ERR_INVALID_HANDLE` because the node was zeroed.
- **Remediation**: In `vfs_rename()`, after a successful rename, iterate across `g_file_table`. Any open file descriptor whose `node` matches the source node before the rename is updated to point to the surviving destination node, preserving file descriptor validity across atomic replacement.

### Finding AX-3: Uninitialized Slack Bytes in Sparse File Writes Past EOF
- **Component**: `kernel/fs/astrafs.c` (`astrafs_write`)
- **Severity**: Medium
- **Issue**: When writing past the current file size (`offset > ino->file_size`), newly allocated sectors beyond the old sector count are zeroed out, but if `offset` falls within the last existing sector past `ino->file_size`, the gap between `ino->file_size` and `offset` is not explicitly zero-filled, potentially exposing uninitialized data from previous allocations.
- **Remediation**: In `astrafs_write()`, explicitly zero-fill any slack bytes in the existing sector between `ino->file_size` and `offset` before copying new payload bytes.

---

## 6. Qualification Strategy & Test Plan

1. Implement targeted hardening fixes in `kernel/fs/astrafs.c` and `kernel/fs/vfs.c`.
2. Implement 6 deterministic in-kernel qualification tests in `kernel/process/process.c` (`process_ax_run_tests`):
   - **Test AX-1**: Block device boundary, overflow, and write-protection defense (`ASTRA_AX_BLOCK_BOUNDS_OK`).
   - **Test AX-2**: Superblock, inode, and bitmap integrity validation (`ASTRA_AX_METADATA_INTEGRITY_OK`).
   - **Test AX-3**: Multi-stage crash recovery and dual-set rollback verification (`ASTRA_AX_CRASH_RECOVERY_OK`).
   - **Test AX-4**: Pre-commit flush and ATA cache synchronization defense (`ASTRA_AX_PRECOMMIT_FLUSH_OK`).
   - **Test AX-5**: Atomic rename replacement and open file descriptor preservation (`ASTRA_AX_RENAME_FD_PRESERVATION_OK`).
   - **Test AX-6**: Multi-worker concurrent file operations and lock hierarchy stress (`ASTRA_AX_CONCURRENT_LIFECYCLE_OK`).
3. Run the complete test qualification matrix:
   - QEMU SMP 1, 2, 4, 8, nofb.
   - 7 Host test suites.
   - Oracle VirtualBox 7 EFI hardware verification.
4. Total expected markers: **167** (161 prior + 6 Phase AX).

