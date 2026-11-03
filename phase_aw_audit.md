# Phase AW — Kernel Security Audit & Vulnerability Analysis

## 1. Executive Summary & Audit Scope

This document records the comprehensive, source-level security audit conducted for AstraOS as part of **Phase AW — Kernel Security Audit & Vulnerability Hardening**.

The audit examined every major kernel subsystem:
1. **Memory Management & Paging**: VMM page table construction, permissions, unmapping, TLB shootdown synchronization, PMM frame allocation/freeing, heap allocators (`kmalloc`, `kfree`, `krealloc`).
2. **User/Kernel Privilege Boundaries**: Ring 0 vs Ring 3 execution, DPL checks, segment registers, page table User/Supervisor bits (`PTE_USER`), CR3 isolation, CPU #PF and exception containment.
3. **System Calls & Input Validation**: Vector 128 interrupt gate (0x80), `user_ptr_valid()`, canonical address enforcement, integer overflow Traps, descriptor ownership, handle validation.
4. **ELF64 Loader & Process Creation**: Malformed headers, program header counts/offsets, segment bounds, non-zero `memsz`, W^X violations, canonical bounds, entry point validation.
5. **Scheduler & Process Lifecycle**: Multi-core preemption, state transitions, atomic teardown, zombie reaping, physical frame reclamation.
6. **Filesystems & Storage**: VFS syntax validation, path traversal defense, ramfs bounds, AstraFS Copy-On-Write dual-set atomicity, ATA flush commands.
7. **Networking & TLS**: Packet size validation, IPv4/UDP/TCP header decoding, socket table lifecycle and ownership, TLS 1.3 handshake, X.509 certificate and hostname verification.

---

## 2. Subsystem Audit Findings

### 2.1 Memory Management & Paging
- **PMM**: Tracks page double-free (`PMM_ERR_DOUBLE_FREE`), unaligned addresses (`PMM_ERR_UNALIGNED`), page zero (`PMM_ERR_PAGE_ZERO`), unmanaged MMIO (`PMM_ERR_UNMANAGED`), and reserved metadata protection (`PMM_ERR_RESERVED`).
- **VMM**: Enforces canonical 48-bit address validation, page directory tree allocation, leaf entry `PTE_TRANSITION` (bit 9) marking during unmapping, and synchronized cross-CPU TLB shootdowns with dynamic rollback on timeout.
- **Kernel Heap**: Verifies block magic (`HEAP_BLOCK_MAGIC`), arena bounds, alignment, coalescing integrity, and double-free trapping.
- **Paging Permissions**: User pages marked with `PTE_USER`; supervisor pages lack `PTE_USER` (hardware MMU causes Vector 14 #PF on Ring 3 access); user stack has non-executable bit (`PTE_NO_EXEC`) preventing stack execution; guard page at stack bottom remains unmapped.

### 2.2 System Calls & Privilege Separation
- **Syscall Gate**: IDT Vector 128 configured with DPL=3, gate type 0xEE.
- **Pointer Validation**: `user_ptr_valid()` verifies:
  1. Pointer is non-null.
  2. Length addition does not overflow 64-bit integer (`end >= start`).
  3. Start and end addresses reside within canonical user space ($< 0x0000800000000000ULL$).
  4. Every 4 KiB page in the buffer range is mapped, present in CR3, and has `PTE_USER` (and `PTE_WRITE` if write access is required).
- **Identified Defect AW-1 (High)**: `sys_munmap` took user `length` without upper bound check or overflow verification on `length + PAGE_SIZE - 1`. If `length` wrapped 64-bit integer, `page_count` underflowed to 0 or wrapped address space, bypassing canonical bound checks.
- **Identified Defect AW-2 (High)**: In `sys_write`, `sys_read`, `sys_close`, `sys_fsync`, ownership check evaluated `if (owner != 0) { if (cur_pid != owner) return -EBADF; }`. If `owner == 0` (kernel-opened descriptor), user processes (`cur_pid != 0`) bypassed the check and could read, write, fsync, or close kernel file descriptors.
- **Identified Defect AW-3 (Medium)**: In `sys_gui_*` syscalls, `win->owner_pid != 0` check allowed user processes to manipulate or destroy window 0 (the desktop/system window).
- **Identified Defect AW-4 (High)**: `sys_net_fetch` used an un-synchronized `static char fetch_buf[BROWSER_HTML_BUFFER_MAX]` shared across all CPUs and threads, causing race conditions and buffer corruption under concurrent calls.

### 2.3 Network Stack & Sockets
- **Identified Defect AW-5 (High)**: `tcp_socket_t` lacked an `owner_pid` field. Any user process could send, receive, connect, or close any socket in the system. Furthermore, `process_exit()` did not reclaim open sockets, allowing unprivileged processes to permanently exhaust `TCP_MAX_SOCKETS` (DoS).
- **Identified Defect AW-6 (High)**: In `kernel/net/ipv4.c` (`ipv4_handle_packet`), `size_t payload_len = total_len - ihl;` was calculated after checking `total_len > len`, but WITHOUT checking `total_len < ihl`. A malformed packet with `total_len < ihl` triggered an integer underflow, passing an astronomical payload length ($\approx 2^{64}$) to protocol handlers.
- **Identified Defect AW-7 (Medium)**: In `kernel/net/arp.c` (`arp_handle_packet`), `hw_size == 6` and `proto_size == 4` were not verified, allowing malformed ARP packets with mismatched address lengths to pass through.

### 2.4 VFS & Filesystem Security
- **Identified Defect AW-8 (Medium)**: In `kernel/fs/vfs.c` (`validate_path_syntax`), path validation checked for `//`, `/./`, `/../`, but did not reject ASCII non-printable control characters ($< 32$ or $127$), allowing potential log injection or delimiter confusion.
- **AstraFS**: Dual-set metadata (A & B) with alternating commit, Fletcher-32 checksums, Copy-On-Write extents, and pre-commit ATA cache flushes verified crash-resilient and tamper-resistant.
- **RamFS**: Enforces allocation bounds (`VFS_MAX_MUTABLE_FILES = 16`, `VFS_MAX_TOTAL_MUTABLE_ALLOC = 2 MiB`), file size bounds, and integer overflow checks.

### 2.5 ELF64 Loader
- `elf_validate_reader` enforces:
  - Header validation: Magic, 64-bit ELF, Little-Endian, static ET_EXEC, x86-64 machine.
  - Bounds & Counts: `e_phnum` bounded ($1 \le e_phnum \le 64$), `e_phoff` multiplication and bounds checked against file size.
  - Segment Validation: `p_memsz > 0`, `p_filesz <= p_memsz`, cumulative size $\le 64$ MiB.
  - Canonical Space: $0x00200000ULL \le p_vaddr < 0x0000700000000000ULL$.
  - Alignment: Power of two, page offset congruence (`p_vaddr % 4096 == p_offset % 4096`).
  - Strict W^X: Simultaneous `PF_W` and `PF_X` strictly rejected.
  - Overlap: Overlapping virtual memory extents between segments detected and rejected.
  - Entry Point: Must reside inside an executable (`PF_X`) loadable segment.

---

## 3. Vulnerability Summary Table

| Finding ID | Component | Severity | Vulnerability Description | Remediation Plan |
|---|---|---|---|---|
| **AW-1** | `kernel/process/syscall.c` | **High** | Integer overflow in `sys_munmap` length calculation | Bound `length <= 16 * 1024 * 1024` and reject overflow |
| **AW-2** | `kernel/process/syscall.c` | **High** | Kernel file descriptor access by user mode when `owner == 0` | Enforce `if (cur_pid != 0 && cur_pid != owner) return -EBADF;` |
| **AW-3** | `kernel/process/syscall.c` | **Medium** | System/desktop window manipulation when `owner_pid == 0` | Enforce `if (proc && proc->pid != 0 && win->owner_pid != proc->pid) return -EBADF;` |
| **AW-4** | `kernel/process/syscall.c` | **High** | Data race on shared static buffer in `sys_net_fetch` | Protect `fetch_buf` with dedicated spinlock `g_net_fetch_lock` |
| **AW-5** | `kernel/net/tcp.c` | **High** | Missing socket `owner_pid` and socket leak on process exit | Add `owner_pid`, check in syscalls, add `tcp_close_all_for_pid()` on exit |
| **AW-6** | `kernel/net/ipv4.c` | **High** | Integer underflow in IPv4 payload length computation (`total_len < ihl`) | Enforce `if (total_len < ihl \|\| total_len > len) return;` |
| **AW-7** | `kernel/net/arp.c` | **Medium** | Missing `hw_size` and `proto_size` validation in ARP handler | Verify `if (pkt->hw_size != 6 \|\| pkt->proto_size != 4) return;` |
| **AW-8** | `kernel/fs/vfs.c` | **Medium** | ASCII control character injection in VFS path validation | Reject characters with ASCII value $< 32$ or $== 127$ |

---

## 4. Test Strategy
1. **Targeted Security Regression Tests**:
   - Implement deterministic tests in `kernel/process/process.c` verifying each vulnerability fix:
     - Test AW-1: `sys_munmap` integer overflow rejection (`ASTRA_AW_MUNMAP_OVERFLOW_DEFENSE_OK`).
     - Test AW-2: Kernel file descriptor cross-privilege access rejection (`ASTRA_AW_FD_CROSS_PRIV_DEFENSE_OK`).
     - Test AW-3: Window manager cross-privilege hijacking rejection (`ASTRA_AW_GUI_CROSS_PRIV_DEFENSE_OK`).
     - Test AW-4: Concurrent `sys_net_fetch` thread safety & synchronization (`ASTRA_AW_NET_FETCH_CONCURRENCY_OK`).
     - Test AW-5: TCP socket ownership isolation & process exit cleanup (`ASTRA_AW_SOCKET_OWNER_CLEANUP_OK`).
     - Test AW-6: IPv4 packet length underflow defense (`ASTRA_AW_IPV4_UNDERFLOW_DEFENSE_OK`).
     - Test AW-7: ARP packet header size validation (`ASTRA_AW_ARP_SIZE_VALIDATION_OK`).
     - Test AW-8: Path traversal and control character injection rejection (`ASTRA_AW_PATH_SECURITY_DEFENSE_OK`).
2. **Full Qualification Matrix**:
   - QEMU SMP 1, 2, 4, 8, nofb.
   - 7 host test suites.
   - Oracle VirtualBox 7 EFI verification.

