# Phase AK Audit — AstraOS Process Management, ELF Loader & User-Space Isolation Hardening

## 1. Executive Summary & Baseline State

Phase AK audits and hardens AstraOS process management, ELF64 executable loading, Ring 3 isolation, process termination, resource ownership, and scheduler interactions.

The primary objective is to guarantee that multiple user-space applications can execute independently without corrupting kernel memory, leaking physical frames or page tables, hijacking other processes' resources (descriptors/windows), or compromising kernel stability when faulting.

### Rule 0 Git Status
- Git status was verified prior to modifications.
- Existing subsystem implementations (AstraFS, block driver, VFS, initramfs, cryptography, networking, window manager) are intact and operational.
- All 16 QEMU configurations and VirtualBox 7 EFI boot sessions passed completely in Phase AJ.

---

## 2. Process & ELF Subsystem Architecture Audit

### 2.1 Complete Process Lifecycle
The current lifecycle traces as follows:
```
ELF binary (VFS/memory)
  │
  ▼
elf_validate() / elf_validate_reader()
  - Validates magic (\x7fELF), class (ELFCLASS64), data (LSB), machine (EM_X86_64), type (ET_EXEC).
  - Validates e_phoff bounds, p_filesz <= p_memsz, lower canonical user VA bounds (0x200000..0x700000000000).
  - Enforces W^X (PF_W && PF_X rejected) and segment non-overlap.
  - Ensures e_entry falls in an executable PT_LOAD segment.
  │
  ▼
process_create_elf_reader()
  - Allocates process_t slot (up to PROCESS_MAX_PROCESSES = 16).
  - Calls vmm_create_process_space(): allocates PML4 root, zeroes user half (0..255), copies kernel half (256..511).
  - For each PT_LOAD segment: allocates physical frames, zeroes them (guaranteeing .bss = 0), copies file bytes, maps via vmm_map_page().
  - Allocates 4 pages (16 KiB) user stack at 0x0000700000000000 (with NO_EXEC), leaving 0x00006ffffff00000 unmapped as guard page.
  - Creates underlying execution thread with process_entry_trampoline.
  │
  ▼
process_entry_trampoline()
  - Updates TSS RSP0 to thread->stack_top.
  - Loads CR3 with proc->pml4_phys.
  - Calls user_jump_to_ring3(code_va, user_stack_top).
  │
  ▼
user_jump_to_ring3()
  - Loads user data segments (0x1B).
  - Sets up iretq frame (SS=0x1B, RSP=user_rsp, RFLAGS with IF=1, CS=0x23, RIP=code_va).
  - Clears all GP registers (%rax..%r15) to eliminate kernel state leakage.
  - Executes iretq into Ring 3 (CPL=3).
  │
  ▼
Syscall Execution (int $0x80)
  - Enters syscall_stub_128 (IDT vector 128, DPL=3, gate 0xEE).
  - CPU switches to TSS RSP0 kernel stack.
  - syscall_dispatcher enables interrupts (sti()), validates user pointers via user_ptr_valid(), executes requested syscall.
  - Disables interrupts (cli()), restores registers, and executes iretq back to Ring 3.
  │
  ▼
Termination & Reclamation (sys_exit or hardware fault)
  - process_exit() reclaims owned GUI windows (wm_destroy_all_for_pid).
  - Switches CR3 to kernel PML4.
  - Frees all mapped physical frames in proc->mapped_pas.
  - Destroys user page table structures via vmm_destroy_process_space().
  - Sets state to PROCESS_DEAD.
  - Calls thread_exit() to reclaim kernel execution thread.
```

---

## 3. Findings & Identified Gaps

### 3.1 ELF Loader Security
1. **Degenerate Segments (`p_memsz == 0`)**:
   `elf_validate_reader()` currently does not reject segments with `p_memsz == 0`. A zero-length segment should be flagged and rejected as `ELF_ERR_INVALID_SEGMENT`.
2. **Alignment Validation (`p_align`)**:
   Currently only 4096-byte alignment `(p_vaddr & 0xFFF) == (p_offset & 0xFFF)` is verified. If `p_align > 1`, ELF64 specification requires `p_align` to be a power of two and `(p_vaddr % p_align) == (p_offset % p_align)`.
3. **Cumulative Allocation Limit**:
   An ELF specifying hundreds of megabytes of memory across segments could trigger out-of-memory or excessive allocation before hitting page capacity limits. A bounded check (`total_memsz <= 64 MiB`) is required.
4. **Host Test Coverage**:
   ELF loader validation is currently tested primarily in in-kernel self-tests. A dedicated standalone host test suite (`tests/test_elf_host.c` / `tests/test_elf_host.py`) is needed to stress all malformed ELF edge cases deterministically.

### 3.2 File Descriptor Ownership & Process Isolation
1. **Unowned File Descriptors**:
   In `kernel/fs/vfs.c`, `vfs_file_t` currently contains `{ bool in_use; vfs_node_t *node; size_t offset; u32 flags; }` with **no `owner_pid` field**.
   Consequently, Process B can read, write, fsync, or close a file descriptor opened by Process A!
2. **Descriptor Leak on Process Exit**:
   When `process_exit()` is called, it destroys GUI windows (`wm_destroy_all_for_pid`) but **does not close open file descriptors**! Descriptors remain open in `g_file_table`, leaking table entries and leaving nodes open.
3. **Remediation**:
   - Add `pid_t owner_pid` to `vfs_file_t`.
   - Update `sys_open` to tag open descriptors with `proc->pid`.
   - Guard `sys_read`, `sys_write`, `sys_close`, `sys_fsync` with ownership checks: return `-EBADF` if caller PID does not match descriptor owner PID.
   - Implement `vfs_close_all_for_pid(pid_t pid)` and invoke it in `process_exit()`.

### 3.3 Process Waiting, Parent-Child Tracking & Exit Codes
1. **No `sys_waitpid`**:
   Currently, the kernel has no syscall for a process to wait for a child process. Parents must spin-yield on `proc->state != PROCESS_DEAD`.
2. **Exit Code & Status Propagation**:
   `process_t` holds `int exit_code` and `bool exited_from_fault`, but there is no syscall to retrieve the exit status of a child process.
3. **Remediation**:
   - Add `pid_t parent_pid` to `process_t`.
   - Add `sys_waitpid(pid_t pid, int *status_user, int options)` (syscall 36).
   - Maintain zombie state until reaped by parent or kernel reaper.

### 3.4 Adversarial Memory Isolation Traps
1. **Verification of Non-Execution (NX) Enforcement**:
   Must prove with an adversarial test that attempting to execute instructions on user stack pages or non-executable mmap pages triggers Vector 14 #PF with `Fetch=1` and is safely contained.
2. **Verification of Cross-Process Memory Isolation**:
   Must prove that Process B cannot read or write Process A's private user memory addresses even though they share the same numeric virtual addresses (e.g. `0x400000`), confirming distinct CR3 page table roots.
3. **Syscall Page Boundary Crossing Validation**:
   Must prove that a user buffer spanning two pages where the second page is unmapped or supervisor-only is caught by `user_ptr_valid()` and rejected with `-EFAULT` without triggering a kernel page fault trap.

---

## 4. Phase AK Implementation Plan & Tasks

1. **Task 1: ELF Loader Security & Host Test Suite**
   - Harden `kernel/process/elf.c`: reject `p_memsz == 0`, enforce `p_align` power-of-two and modulo alignment, enforce cumulative segment memory bounds.
   - Author `tests/test_elf_host.c` and `tests/test_elf_host.py` with 15+ deterministic malformed ELF tests.
2. **Task 2: Address Space & Memory Isolation Hardening**
   - Verify supervisor page user-bit protection.
   - Implement adversarial tests for:
     1. User reading supervisor memory.
     2. User writing supervisor memory.
     3. User executing non-executable page (NX check).
     4. User accessing another process's memory space.
     5. User accessing stack guard page.
     6. Page boundary crossing with unmapped tail page.
3. **Task 3: Process Lifecycle, Descriptor Ownership & Reclamation**
   - Add `pid_t owner_pid` to `vfs_file_t` in `kernel/fs/vfs.c`.
   - Implement `vfs_close_all_for_pid(pid_t pid)`.
   - Add descriptor ownership verification to `sys_read`, `sys_write`, `sys_close`, `sys_fsync`.
   - Add `pid_t parent_pid` and `sys_waitpid` to `process.c` / `syscall.c`.
4. **Task 4: Scheduler & Context Switch Verification**
   - Verify multi-process concurrent execution under timer preemption.
   - Verify CR3 switching and TSS RSP0 reloading on every context switch.
5. **Task 5: Syscall Boundary Hardening**
   - Enforce ownership and range checks on all syscalls.
   - Verify rejection of cross-process descriptor manipulation.
6. **Task 6: Three Independent Ring 3 Test Applications**
   - Create `normal_app` (`user/normal_app.c`).
   - Create `faulting_app` (`user/faulting_app.c`).
   - Create `worker_app` (`user/worker_app.c`).
7. **Task 7: Resource Accounting & Stress Testing**
   - Create bounded stress tests measuring PMM free pages before and after repeated process creation and exit cycles.
8. **Task 8: Complete Regression**
   - Run all host test suites (ELF, Block, AstraFS, Bookmarks, VFS, Networking, Fuzz, Crypto).
   - Run QEMU `nofb`, `normal`, and 16-config matrix.
   - Run VirtualBox 7 EFI headless test.
9. **Task 9: Documentation & Report**
   - Update `docs/architecture.md`, `docs/status.md`, and author `phase_ak_report.md`.

---

## 5. Acceptance Criteria

- [ ] All malformed ELFs (truncated, corrupted magic, 32-bit, big-endian, bad machine, bad type, overlapping segments, filesz > memsz, invalid addresses, W^X violations, bad entry points, zero memsz) rejected without panic or leaks.
- [ ] File descriptors have defined owner PIDs; cross-process access returns `-EBADF`; process exit reclaims all owned descriptors.
- [ ] User page faults (#PF Vector 14) on supervisor access, NX violation, and guard pages terminate the faulting process without crashing the kernel.
- [ ] Multiple user applications execute concurrently with independent CR3 address spaces and verified exit codes.
- [ ] PMM free frame count before and after process creation/destruction is identical (zero page leaks).
- [ ] All host unit tests, QEMU matrix, and VirtualBox 7 EFI boot verify 100% PASS.

