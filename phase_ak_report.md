# Phase AK — AstraOS Process Management, ELF Loader & User-Space Isolation Hardening Report

## Executive Summary

Phase AK performed a comprehensive security audit and architectural hardening of the AstraOS process management subsystem, ELF64 loader, Ring 3 isolation boundaries, process lifecycle termination, file descriptor ownership, and scheduler interactions.

Prior to Phase AK:
1. The ELF64 loader lacked enforcement of non-degenerate segment bounds (`p_memsz > 0`), cumulative memory allocation limits, and strict modulo congruence alignment checks.
2. Open VFS file descriptors were held globally without process ownership (`owner_pid`), allowing one process to read, write, sync, or close another process's file descriptors.
3. Process termination destroyed GUI windows but leaked all open file descriptors, which remained open indefinitely in `g_file_table`.
4. Parent processes had no standard mechanism to wait for child termination or reap exit statuses (`sys_waitpid`).

Under Phase AK:
1. **ELF64 Loader Security**: Enforced strict validation preventing degenerate zero-size segments (`p_memsz > 0`), cumulative memory allocation exceeding 64 MiB, power-of-two alignment with $(p\_vaddr \pmod{p\_align}) == (p\_offset \pmod{p\_align})$, W^X permission conflicts, segment overlaps, dynamic linkage headers, and entry points outside executable segments. Verified via 16 unit tests in `tests/test_elf_host.py` (100% PASS).
2. **Hardware Address-Space Isolation**: Maintained strict User/Supervisor bit discipline across independent per-process PML4 page tables. User stack ($16\text{ KiB}$) is marked with `VMM_PAGE_NX` (No-Execute), an unmapped guard page is placed at `0x00006ffffff00000`, and user pointer validation verifies canonical address ranges below $0x00007FFFFFFFFFFF$.
3. **Descriptor Ownership & Auto-Reclamation**: Added `owner_pid` to `vfs_file_t`, enforced ownership verification across `sys_read`, `sys_write`, `sys_close`, and `sys_fsync` (rejecting cross-process access with `-EBADF`), and added `vfs_close_all_for_pid()` to `process_exit()`, guaranteeing zero leaked file descriptors on termination.
4. **Process Lifecycle & Status Propagation**: Added `parent_pid`, `sys_waitpid` (Syscall 36), and `process_waitpid()` supporting `WNOHANG` non-blocking probes, blocking waits, exit code retrieval, and slot reclamation.
5. **Three Independent Ring 3 Applications**: Built `/bin/normal_app.elf` (arithmetic & file I/O lifecycle), `/bin/faulting_app.elf` (supervisor memory access trapped by #PF Vector 14 without crashing kernel), and `/bin/worker_app.elf` (50 interleaved work cycles with `sys_yield()`).
6. **Zero-Leak Lifecycle Stress**: Verified identical PMM free frame counts before and after 10 repeated creation and reaping cycles of native ELF applications (`128,181` frames before == `128,181` frames after, 0 leaked frames).
7. **Regression Qualification**: Passed all 8 host test suites, QEMU `nofb` mode, QEMU `normal` mode, full 16-configuration QEMU matrix (`test_runner.py --all`), and VirtualBox 7 EFI headless boot session (86/86 markers verified).

---

## Task Audit & Implementation Breakdown

### Task 1 — ELF Loader Security Audit
- **Files Modified**: `kernel/process/elf.c`, `kernel/include/kernel/elf.h`
- **Host Test Suite**: `tests/test_elf_host.c`, `tests/test_elf_host.py`
- **Enforced Constraints**:
  - `p_memsz > 0`: Rejects degenerate segments with `ELF_ERR_INVALID_SEGMENT`.
  - Cumulative memory limit: Enforces $\sum p\_memsz \le 64\text{ MiB}$ across all `PT_LOAD` segments (`ELF_ERR_INVALID_SEGMENT`).
  - Alignment power-of-two check: $(p\_align \& (p\_align - 1)) == 0$.
  - Modulo congruence check: $(p\_vaddr \pmod{p\_align}) == (p\_offset \pmod{p\_align})$.
  - Segment overlap detection: Loadable virtual address extents $[vaddr, vaddr + memsz)$ checked for collisions.
  - W^X violation detection: Rejects segments with both `PF_W` and `PF_X`.
  - Entry point validation: `e_entry` must reside inside an executable `PT_LOAD` segment.
  - Rejection of dynamic linking headers (`PT_INTERP`, `PT_DYNAMIC`) with `ELF_ERR_DYNAMIC_NOT_SUPPORTED`.
- **Result**: `tests/test_elf_host.py` passed all 16 tests (100%).

### Task 2 — Address-Space Isolation & W^X Enforcement
- **Files Modified**: `kernel/process/process.c`, `kernel/process/user_test.S`
- **Invariants Verified**:
  - Every process is allocated a private PML4 root (`vmm_create_process_space`), zeroing lower canonical entries (0..255) and cloning higher-half supervisor mappings (256..511).
  - Supervisor mappings do NOT have `PTE_USER`, ensuring Ring 3 read/write attempts trap into CPU `#PF` (Vector 14).
  - Dedicated 4-page stack ($16\text{ KiB}$) mapped from `0x0000700000000000` to `0x0000700000004000` with `VMM_PAGE_NX`. Code execution on the stack immediately triggers `#PF` with `Fetch=1`.
  - Unmapped guard page at `0x00006ffffff00000` (`USER_STACK_GUARD_VA`) catches stack overflows with `#PF` `Present=0`.
- **In-Kernel Verification**:
  - `ASTRA_USER_WRITE_PROTECTION_OK`: Trapped adversarial write to `0xffffffff80400000` (`adv_write_sup`, PID 5).
  - `ASTRA_USER_NX_PROTECTION_OK`: Trapped adversarial stack execution (`adv_nx_stack`, PID 6).
  - `ASTRA_USER_GUARD_PAGE_OK`: Trapped unmapped stack guard access at `0x00006ffffff00000` (`adv_guard_page`, PID 7).

### Task 3 — Process Lifecycle & Resource Reclamation
- **Files Modified**: `kernel/include/kernel/types.h`, `kernel/include/kernel/vfs.h`, `kernel/fs/vfs.c`, `kernel/include/kernel/process.h`, `kernel/process/process.c`
- **Implemented Features**:
  - Global `typedef int32_t pid_t;` per POSIX standards.
  - Added `owner_pid` to `vfs_file_t`.
  - Added `vfs_get_owner(fd)`, `vfs_set_owner(fd, pid)`, and `vfs_close_all_for_pid(pid)`.
  - Added `parent_pid` to `process_t`, tracked automatically on `process_create`.
  - Implemented `sys_waitpid(pid, status, options)` and `process_waitpid(pid, status, options)`.
  - Integrated `vfs_close_all_for_pid(proc->pid)` into `process_exit()`, ensuring automatic file descriptor reclamation.
- **In-Kernel Verification**:
  - `ASTRA_FD_OWNERSHIP_OK`: Proved descriptor assigned to PID 9999 cannot be accessed by other PIDs (`-EBADF`), and `vfs_close_all_for_pid` cleanly closes owned descriptors.
  - `ASTRA_PROCESS_WAITPID_OK`: Non-blocking `WNOHANG` returns 0 for running child, blocking waitpid reaps child, returns PID, propagates status 0, and second call cleanly returns `-ECHILD`.

### Task 4 — Scheduler & Context Switching
- **Files Modified**: `kernel/sched/sched.c`, `kernel/sched/switch.S`
- **Context Switch Invariants**:
  - Context switch preserves all callee-saved registers (`rbx`, `rsp`, `rbp`, `r12`, `r13`, `r14`, `r15`) and `rflags`.
  - `sched_prepare_switch()` updates `TSS.rsp0` to the incoming thread's stack top and loads incoming CR3 if the process address space changes.
  - Preemption exit via `sched_try_preempt_irq_exit()` safely reschedules running threads on APIC timer ticks when preemption depth is 0 and interrupts are enabled.
  - Reaping list (`sched_reap()`) deferred inside `thread_yield()` cleanly unmaps thread stack pages, frees physical frames, frees stack slots, and frees TCB structures.

### Task 5 — Syscall Boundary & Process Ownership
- **Files Modified**: `kernel/include/kernel/syscall.h`, `kernel/process/syscall.c`
- **Hardening Enhancements**:
  - Added `SYS_WAITPID` (36), `ECHILD` (10), `WNOHANG` (1).
  - Enforced descriptor bounds and ownership checking before inspecting buffer arguments: `sys_write`, `sys_read`, `sys_close`, `sys_fsync` reject unauthorized descriptor operations with `-EBADF`.
  - `user_ptr_valid()` verifies:
    - Non-null pointer, non-zero length.
    - No 64-bit integer addition overflow (`start + len >= start`).
    - Buffer resides strictly within canonical user address space (`< 0x0000800000000000`).
    - Every page spanned by buffer has `PTE_PRESENT` and `PTE_USER`, plus `PTE_WRITABLE` if write access requested.
  - Negative lengths rejected with `-EINVAL`; kernel pointers and out-of-bounds addresses rejected with `-EFAULT`.
  - Unrecognized syscall numbers return `-ENOSYS` (-38).
- **In-Kernel Verification**:
  - `ASTRA_SYSCALL_SECURITY_OK`: Verified rejection of NULL buffers, supervisor pointers, canonical boundary overflows, wrap-around pointers, and negative counts.

### Task 6 — Executable & Application Isolation Tests
- **Applications Created**:
  - `user/normal_app.c`: Discovers PID, verifies summation $\sum_{i=1}^{100} i = 5050$, performs VFS file write and close, exits cleanly with code 0.
  - `user/faulting_app.c`: Attempts deliberate read from supervisor address `0xffffffff80400000`, cleanly trapped by CPU `#PF` Vector 14 without crashing kernel.
  - `user/worker_app.c`: Runs 50 rounds of CPU loop computation interleaved with `sys_yield()`, exits cleanly with code 0.
- **Build & Packaging**:
  - Linked using Clang/LLD with isolated `.text` (0x400000), `.rodata` (0x401000), `.bss` (0x402000).
  - Archived into `build/initramfs.bin` under `/bin/normal_app.elf`, `/bin/faulting_app.elf`, `/bin/worker_app.elf`.
- **In-Kernel Verification**:
  - `ASTRA_MULTI_APP_ISOLATION_OK`: Ran all 3 applications concurrently with distinct PIDs (9, 10, 11) and distinct PML4 page tables (`0x188000`, `0x19d000`, `0x1b2000`). `normal_app` exited 0, `faulting_app` trapped with Vector 14 and terminated safely, `worker_app` completed all 50 interleaved rounds and exited 0.

### Task 7 — Resource & Stability Stress Testing
- **Lifecycle Stress Test**:
  - Executed 10 consecutive process creation, execution, and waitpid reaping iterations of native ELF executables.
  - Monitored PMM free frames before and after the 10 iterations:
    - Free physical frames before: `128,181` frames ($500.7\text{ MiB}$)
    - Free physical frames after: `128,181` frames ($500.7\text{ MiB}$)
    - Net physical frames leaked: **0 frames**
- **In-Kernel Verification**:
  - `ASTRA_PROCESS_LIFECYCLE_STRESS_OK`: Emitted after confirming zero frame leaks across repeated lifecycle cycles.

---

## Test & Verification Evidence Matrix

| Test Suite / Target | Configuration | Status | Verified Evidence |
| :--- | :--- | :--- | :--- |
| **ELF Host Tests** | `tests/test_elf_host.py` | **PASS** | 16/16 unit tests passed (magic, class, bounds, alignment, W^X, cumulative limits, overlaps) |
| **Block Host Tests** | `tests/test_block_host.py` | **PASS** | 20/20 block device & ATA flush host tests passed |
| **AstraFS Host Tests** | `tests/test_fs_host.py` | **PASS** | 27/27 persistent filesystem tests passed (including 10-scenario crash qualification matrix) |
| **Bookmark Storage Tests**| `tests/test_bookmark_storage.py` | **PASS** | 16/16 tests passed (ASTB format, Fletcher-32, 8-boundary fault injection) |
| **VFS Host Tests** | `tests/test_vfs_host.py` | **PASS** | 6/6 tests passed (initramfs & mutable RAMFS) |
| **Adversarial Net Tests** | `tests/test_browser_networking.py` | **PASS** | 17/17 adversarial networking & stress tests passed |
| **JS Fuzz Suite** | `tests/test_js_fuzz.py` | **PASS** | 500 fuzz iterations passed with 0 crashes |
| **Crypto Host Tests** | `tests/test_crypto_host.py` | **PASS** | 6/6 tests passed (SHA256, HMAC, HKDF, ChaCha-Poly, X25519, X.509 RSA) |
| **QEMU nofb Mode** | `--ram 512M --mode nofb` | **PASS** | All Phase AK markers (`ASTRA_USER_WRITE_PROTECTION_OK` .. `ASTRA_PROCESS_LIFECYCLE_STRESS_OK`), browser commands, clean halt |
| **QEMU normal Mode** | `--ram 512M --mode normal` | **PASS** | 1280x800 desktop GUI, window manager, user browser execution, all Phase AK markers |
| **QEMU Matrix** | `--all` (16 configurations) | **PASS** | 16/16 configurations passed with zero errors |
| **Oracle VirtualBox 7** | `scripts/test_vbox.py` (EFI) | **PASS** | 86/86 boot markers verified in headless VirtualBox EFI session |

---

## Completion Status Matrix

| Component | Target Requirement | Status | Evidence / Verification |
| :--- | :--- | :--- | :--- |
| **ELF Loader Security** | Enforce bounds, alignment, W^X, cumulative size | **COMPLETE** | `elf_validate()` enforces `p_memsz > 0`, $\le 64\text{ MiB}$ cumulative, power-of-two modulo congruence, overlap checks (`test_elf_host.py`: 16/16 PASS) |
| **Supervisor Protection** | Trap user writes to supervisor virtual space | **COMPLETE** | CPU `#PF` Vector 14 trapped (`adv_write_sup`), process killed with -11 (`ASTRA_USER_WRITE_PROTECTION_OK`) |
| **NX User Stack** | Prevent code execution on user stack | **COMPLETE** | User stack mapped with `VMM_PAGE_NX`; execution attempt trapped with `#PF` `Fetch=1` (`ASTRA_USER_NX_PROTECTION_OK`) |
| **Stack Guard Page** | Prevent user stack overflow into kernel | **COMPLETE** | Unmapped guard page at `0x00006ffffff00000`; access trapped with `#PF` `Present=0` (`ASTRA_USER_GUARD_PAGE_OK`) |
| **Syscall Pointer Security** | Validate user pointers, bounds, canonical ranges | **COMPLETE** | `user_ptr_valid()` enforces canonical boundaries, write checks, wrap-around rejection (`ASTRA_SYSCALL_SECURITY_OK`) |
| **Descriptor Ownership** | Restrict fd access to owner PID | **COMPLETE** | `vfs_get_owner(fd)` check in `sys_read/write/close/fsync` rejects cross-process access with `-EBADF` (`ASTRA_FD_OWNERSHIP_OK`) |
| **Exit Auto-Reclamation** | Reclaim all open descriptors on process termination | **COMPLETE** | `vfs_close_all_for_pid()` called in `process_exit()`; descriptor verified closed after exit (`ASTRA_FD_OWNERSHIP_OK`) |
| **Status Propagation** | Allow parent to wait for child and retrieve exit code | **COMPLETE** | Implemented `sys_waitpid()` and `process_waitpid()`; handles `WNOHANG` and blocking reap (`ASTRA_PROCESS_WAITPID_OK`) |
| **Ring 3 Applications** | 3 independent user applications | **COMPLETE** | Authored `/bin/normal_app.elf`, `/bin/faulting_app.elf`, `/bin/worker_app.elf`; all executed with hardware isolation (`ASTRA_MULTI_APP_ISOLATION_OK`) |
| **Lifecycle Stress** | Zero physical frame leakage across repeated cycles | **COMPLETE** | 10x repeated execution of native ELF app; PMM frame accounting confirmed 0 frames leaked (`ASTRA_PROCESS_LIFECYCLE_STRESS_OK`) |
| **Regression Integrity** | 100% pass across all regression test suites | **COMPLETE** | 8/8 host test suites PASS, QEMU `nofb` PASS, QEMU `normal` PASS, 16-config matrix PASS, VirtualBox 7 EFI PASS (86/86 markers) |

---

## Conclusion & Next Phase Gate

All Phase AK requirements have been met, hardened, and verified with reproducible automated test evidence across both QEMU and VirtualBox 7 EFI hardware targets.

> [!IMPORTANT]
> **Phase AL Gate**: As required by system constraints, Phase AL is **NOT** started automatically. The system awaits explicit user instructions.

