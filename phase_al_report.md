# Phase AL Qualification Report — Preemptive Multitasking, Process Lifecycle & Scheduler Verification

**Author:** AstraOS Autonomous Qualification & Engineering Agent  
**Date:** October 31, 2026  
**Status:** **PASSED / COMPLETE**  
**Repository:** `C:\Users\Akash\Documents\MyKernel`

---

## 1. Executive Summary

Phase AL performed an exhaustive, independent audit and qualification of AstraOS preemptive multitasking, process lifecycle management, scheduler fairness, and multiprocessing hardware capabilities.

Prior phases established cooperative context switching, Ring 3 isolation, and ELF loading. However, true preemptive time-slicing of non-yielding user-space code, parent-child process synchronization semantics (`waitpid`), race-free address-space teardown, and SMP execution claims remained unverified.

Through hardware virtualization testing in **Oracle VirtualBox 7 (EFI)** and **QEMU**, host unit testing, and rigorous kernel concurrency stress, Phase AL resolved critical race conditions and verified all required properties:
1. **Ring 3 Timer-Driven Preemption**: A CPU-bound Ring 3 worker executing a tight loop without `sys_yield()` is preempted by APIC timer interrupts, allowing concurrent progress of monitoring processes (`ASTRA_PREEMPT_RING3_OK`).
2. **Multiple Independent User Processes**: 5 distinct native user applications execute concurrently with isolated virtual address spaces, separate CR3 page directories, independent stacks, and separate PIDs (`ASTRA_MULTI_PROCESS_OK`).
3. **Blocking and Wakeup State Machine**: Verified `RUNNING -> BLOCKED -> READY -> RUNNING` state transitions across timer queues (`sys_sleep`) and wait queues (`waitpid`), with zero lost wakeups or double-enqueue corruptions (`ASTRA_BLOCK_WAKEUP_OK`).
4. **`waitpid()` Parent-Child Lifecycle**: Verified authorization enforcement (non-parents rejected with `-ECHILD`), non-blocking checks (`WNOHANG`), zombie reaping, exit code propagation, and repeated wait defense (`ASTRA_WAITPID_LIFECYCLE_OK`).
5. **Atomic Process Teardown & Resource Reclamation**: 10 repeated lifecycle cycles under preemptive scheduling resulted in exactly **0 leaked physical frames** (PMM free frames before: 128,166, after: 128,166) (`ASTRA_PROCESS_CLEANUP_OK`).
6. **Scheduler Fairness & Stability**: Mixed concurrent workloads (CPU-bound worker, periodic monitor, sleeping worker, standard computation) ran to completion without starvation (`ASTRA_SCHED_FAIRNESS_OK`).
7. **SMP Hardware Capability & Reality Check**: Formally established that AstraOS executes strictly on CPU 0 (Bootstrap Processor). Secondary AP cores remain quiescent in firmware/BIOS halt. True multi-core SMP scheduling is explicitly classified as **NOT IMPLEMENTED** (`ASTRA_SMP_AUDIT_OK`).

---

## 2. Defects Identified and Resolved

During rigorous testing on real hypervisor hardware (Oracle VirtualBox 7 EFI), three critical kernel concurrency defects were identified and resolved:

### Defect 1: Embryonic Process Thread Runqueue Race
- **Symptom:** VirtualBox CPU exception `#PF` (Vector 14) during stress creation when `process_entry_trampoline` dereferenced `proc->thread->stack_top` at offset `0x30`.
- **Root Cause:** `thread_create()` added the newly allocated thread structure directly into the ready queue (`sched_enqueue(t)`) and re-enabled interrupts (`sti()`) before the caller (`process_create_elf_reader`) had assigned `proc->thread = t` or `t->process = proc`. In a preemptive hardware environment, a timer interrupt firing immediately after `sti()` scheduled the thread before its parent linkage and stack pointer were initialized.
- **Resolution:** Introduced `thread_create_process(name, entry, arg, proc)`, which atomically binds `t->process = proc` and `proc->thread = t` prior to inserting `t` into the ready queue. In addition, `process_entry_trampoline` was updated to read `thread_current()->stack_top`, eliminating the race window.

### Defect 2: Active Address Space Destruction in `process_exit`
- **Symptom:** Page fault in kernel space during `sys_write` (`CR3 = 0x5000`, `CR2 = 0x4010d6`) during rapid process exit and creation loops.
- **Root Cause:** When a process exited via `sys_exit`, the CPU remained in Ring 0 with CR3 pointing to `proc->pml4_phys`. Calling `vmm_destroy_process_space(proc->pml4_phys)` freed the active PML4 frames back to PMM while the hardware CR3 register and TLB still referenced them. Subsequent allocations reallocated those physical frames while still mapped.
- **Resolution:** In `process_exit()` and `process_rollback()`, CR3 is explicitly switched back to the stable kernel page directory (`write_cr3(vmm_get_kernel_pml4())`) before tearing down user page directories and freeing frames back to the physical memory manager. Furthermore, `preempt_disable()` is asserted across the entire teardown sequence.

### Defect 3: Process Slot Reuse Before Thread Execution Termination
- **Symptom:** Text interleaving and corrupted thread states (`stress_app` PID 33 and PID 34 overlapping).
- **Root Cause:** `process_exit()` marked `proc->state = PROCESS_ZOMBIE` at the start of exit teardown and signaled the parent before `thread_exit()` had context-switched away. The parent in `process_waitpid()` reaped the zombie by setting `proc->state = PROCESS_UNUSED`. The next loop iteration immediately allocated that slot via `process_create_elf_file`, zeroing the slot memory while the old thread was still executing in `process_exit()`.
- **Resolution:** Hardened `process_waitpid()` and `process_reap_zombies()` to enforce an invariant check: a process slot cannot be reaped until its underlying kernel execution thread has completely switched away and reached `THREAD_EXITED` (`if (p->thread && p->thread->state != THREAD_EXITED) { thread_yield(); continue; }`).

---

## 3. Detailed Verification Results by Task

### Task 1 — Timer-Driven Ring 3 Preemption
- **Test:** Boot thread launched `cpu_worker.elf` (a non-yielding arithmetic loop with zero voluntary yields) and `heartbeat.elf` (periodic progress reporter).
- **Outcome:** The APIC timer interrupted `cpu_worker.elf` and scheduled `heartbeat.elf`, which completed 10 progress ticks.
- **Marker:** `ASTRA_PREEMPT_RING3_OK` (Detected in QEMU `nofb`, QEMU `normal`, and VirtualBox 7).

### Task 2 — Multiple Independent User Processes
- **Test:** Concurrently launched 5 distinct user applications:
  1. `cpu_worker` (PID 24, CR3 `0x18e000`, Stack `0x700000004000`)
  2. `heartbeat` (PID 25, CR3 `0x680000`, Stack `0x700000004000`)
  3. `blocking_worker` (PID 26, CR3 `0x682000`, Stack `0x700000004000`)
  4. `faulting_app` (PID 27, CR3 `0x684000`, Stack `0x700000004000`)
  5. `normal_app` (PID 28, CR3 `0x686000`, Stack `0x700000004000`)
- **Outcome:** All 5 applications ran concurrently; `faulting_app` was safely caught on Vector 14 without crashing the kernel; all normal apps exited cleanly.
- **Marker:** `ASTRA_MULTI_PROCESS_OK` (Verified).

### Task 3 — Blocking, Waiting, and Wakeup Cycles
- **Test:** Launched `blocking_worker.elf` which invoked `sys_sleep(40)`. Monitored process state transitions: `RUNNING -> BLOCKED -> READY -> RUNNING`.
- **Outcome:** Process blocked on the APIC timer queue, woke up upon tick deadline expiration, and cleanly completed execution.
- **Marker:** `ASTRA_BLOCK_WAKEUP_OK` (Verified).

### Task 4 — Parent-Child Lifecycle & `waitpid()` Correctness
- **Test:**
  - Unauthorized caller wait: verified non-parent cannot wait on a process (`-ECHILD`).
  - Active child wait with `WNOHANG`: returned 0 while child was running.
  - Blocking wait: blocked until child exited and cleanly reaped zombie, returning PID and status 0.
  - Repeated wait: second wait on already reaped PID correctly returned `-ECHILD`.
- **Marker:** `ASTRA_WAITPID_LIFECYCLE_OK` (Verified).

### Task 5 — Process Cleanup & PMM Frame Accounting
- **Test:** 10 repeated cycles of process creation, execution, and reaping under preemptive scheduling.
- **Physical Memory Accounting:**
  - Free physical frames before test: `128,166` (500.64 MiB)
  - Free physical frames after test: `128,166` (500.64 MiB)
  - Leaked physical frames: **0**
- **Marker:** `ASTRA_PROCESS_CLEANUP_OK` (Verified).

### Task 6 — Scheduler Fairness & Stability
- **Test:** Mixed workload consisting of CPU-bound, periodic heartbeat, blocking sleeper, and standard application.
- **Outcome:** All 4 applications completed within 7 timer ticks without starvation.
- **Marker:** `ASTRA_SCHED_FAIRNESS_OK` (Verified).

### Task 7 — SMP Capability & Architectural Audit
- **Test:** Hardware CPUID and bootloader protocol inspection.
- **CPUID Output:** Host package reports logical cores via CPUID function 1.
- **Limine Boot State:** Limine SMP request is absent; bootloader halts all Application Processors (APs) at firmware initialization and hands control solely to the Bootstrap Processor (BSP, CPU 0).
- **Runqueue Topology:** Global single-core linked list without cross-core locking or inter-processor interrupts (IPIs).
- **Formal Status:** AstraOS executes exclusively on a single CPU. Multi-core SMP scheduling is formally **NOT IMPLEMENTED**.
- **Marker:** `ASTRA_SMP_AUDIT_OK` (Verified).

---

## 4. Test Suite Execution Matrix

| Test Suite | Environment | Scope | Result |
| :--- | :--- | :--- | :--- |
| `test_elf_host.py` | Host Mingw64 / GCC | 16 ELF64 Loader Security & Validation Tests | **PASS (16/16)** |
| `test_block_host.py` | Host Mingw64 / GCC | 20 Block Device & ATA Flush Tests | **PASS (20/20)** |
| `test_fs_host.py` | Host Mingw64 / GCC | 27 AstraFS Filesystem & CoW Tests | **PASS (27/27)** |
| `test_bookmark_storage.py` | Host Mingw64 / GCC | 16 Browser Bookmark Persistence Tests | **PASS (16/16)** |
| `test_vfs_host.py` | Host Mingw64 / GCC | 6 VFS & Mutable RAMFS Unit Tests | **PASS (6/6)** |
| `test_browser_networking.py` | Python / Local Server | 17 Adversarial HTTP/HTTPS Browser Tests | **PASS (17/17)** |
| `test_js_fuzz.py` | Host Python | 500 Deterministic JS Fuzz Iterations | **PASS (500/500)** |
| `test_crypto_host.py` | Host LLVM Clang | Full Cryptographic Subsystem Self-Test | **PASS (6/6)** |
| `test_deadline_host.py` | Host Python | 4 Deadline Arithmetic Boundary Tests | **PASS (4/4)** |
| `test_heap_host.py` | Host Mingw64 / GCC | 9 Kernel Heap & Corruption Defense Tests | **PASS (9/9)** |
| `test_sync_host.py` | Host Mingw64 / GCC | 11 Mutex, Semaphore & IRQ Primitives | **PASS (11/11)** |
| `test_pmm_host.py` | Host Python | 5 Physical Memory Manager Unit Tests | **PASS (5/5)** |
| QEMU nofb Mode | QEMU x86_64 UEFI | Safe Serial Console, All 93 Markers | **PASS** |
| QEMU normal Mode | QEMU x86_64 UEFI | Full GUI Desktop, Browser, All 93 Markers | **PASS** |
| VirtualBox 7 Headless | Oracle VirtualBox 7 EFI | Real Hypervisor Hardware, All 93 Markers | **PASS (93/93)** |

---

## 5. Architectural Distinction: Cooperative vs. Preemptive vs. SMP

To ensure strict engineering precision and preserve documentation integrity:
- **Cooperative Multitasking:** Implemented and operational since early phases. Threads voluntarily relinquish the CPU via `thread_yield()` or blocking primitives (`event_wait`, `mutex_lock`).
- **Timer-Driven Preemptive Multitasking:** Fully qualified and verified in Phase AL. The APIC timer interrupt forces execution out of CPU-bound Ring 3 user code and switches to other ready threads without user consent or voluntary yields.
- **Symmetric Multiprocessing (SMP):** **NOT IMPLEMENTED.** Although multi-core virtual machines boot because firmware leaves secondary cores in idle halt loops, the AstraOS kernel runs exclusively on CPU 0. No multi-core parallel execution takes place.

---

## 6. Conclusion & Phase Gate

Phase AL requirements have been satisfied in full. Every marker from `ASTRA_PREEMPT_RING3_OK` to `ASTRA_SMP_AUDIT_OK` has been verified under continuous integration testing in both QEMU and Oracle VirtualBox 7.

In accordance with user instructions, Phase AM has not been started.

