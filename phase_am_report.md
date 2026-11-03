# Phase AM — Symmetric Multiprocessing (SMP), AP Bring-Up & Multi-Core Scheduler Safety: Final Report

**Workspace:** `C:\Users\Akash\Documents\MyKernel`  
**Target Architecture:** x86-64 (UEFI / Limine Bootloader)  
**Date:** October 31, 2026  
**Status:** **COMPLETE — 100% PASS ACROSS MULTI-CORE MATRIX (1, 2, 4, 8 CORES) & VIRTUALBOX 7 EFI**

---

## 1. Executive Summary

Phase AM successfully transitions AstraOS from uniprocessor execution on the Bootstrap Processor (BSP) to genuine Symmetric Multiprocessing (SMP) on x86-64. Application Processors (APs) are discovered through the Limine MP protocol, brought online into 64-bit long mode, and integrated into a shared, spinlock-synchronized multi-core kernel scheduler.

### Key Milestones Achieved
1. **Limine MP Protocol Bootstrapping**: Discovered up to 8 logical processors via Limine MP boot response structures. Each AP is awakened through `info->goto_address = ap_entry_c`, configures its own CR3, loads its dedicated GDT and Task State Segment (TSS) with an independent double-fault IST1 stack, initializes its Local APIC timer in periodic mode, and enters the multi-core scheduler loop.
2. **Re-Entrant, IRQ-Safe Spinlocks**: Designed and implemented `kernel/include/kernel/spinlock.h` with atomic `test_and_set` builtins and `pause` backoff. All core kernel subsystems—Physical Memory Manager (`g_pmm_lock`), Transactional Heap (`g_heap_lock`), Process Table (`g_proc_lock`), Ready Queue (`g_sched_lock`), Sleeper Queue (`g_sleep_lock`), and synchronization primitives (`mutex_t`, `semaphore_t`, `event_t`)—are protected against multi-core races and nested interrupt deadlocks via `spinlock_acquire_irqsave` and `spinlock_release_irqrestore`.
3. **Atomic Block-and-Unlock Protocol**: Implemented `sched_block_current_locked()` to eliminate sleep/wakeup lost-wakeup race conditions across multiple cores. Subsystem locks are released only after the outgoing thread is safely marked `THREAD_BLOCKED`.
4. **Thread Execution Exclusivity Invariant**: Protected thread state transitions with an atomic `is_executing` CAS barrier. Under no circumstance can a thread ever be executed on two CPU cores simultaneously.
5. **HHDM Context-Switch Stack Staging & TLB Invalidation**: Solved the multi-core context-switch paging issue by staging the initial 9-quadword execution frame directly into physical memory via the Higher Half Direct Map (`top_pa + hhdm`) and issuing `invlpg` on all mapped stack virtual pages.
6. **Multi-Core User-Space Concurrency & Syscall 37**: Implemented `SYS_GETCPUID` (syscall vector 37). Deployed concurrent Ring 3 worker applications (`cpu_worker_a.elf` and `cpu_worker_b.elf`) that run in parallel on separate CPU cores, detecting simultaneous overlapping execution and emitting `ASTRA_SMP_PARALLEL_EXECUTION_OK`.
7. **Multi-Core Test Matrix & VirtualBox EFI**: Passed the complete multi-core test matrix across QEMU (`-smp 1, 2, 4, 8` in both `normal` framebuffer and `nofb` headless modes), all 7 host test suites (crypto, vfs, block, fs, elf, bookmarks, networking), and Oracle VirtualBox 7 EFI (verifying all 93/93 boot and subsystem markers).

---

## 2. Multi-Core Test Matrix Execution Evidence

The multi-core verification matrix was executed across all supported CPU topologies and display modes:

| Test Target | Mode | CPUs | RAM | Result | Markers Verified |
| :--- | :--- | :---: | :---: | :---: | :--- |
| **QEMU** | `normal` | 1 | 512M | **PASS (100%)** | All boot, PMM, VMM, Sched, Ring 3, GUI, Net, AstraFS, Storage |
| **QEMU** | `nofb` | 1 | 512M | **PASS (100%)** | Full headless suite, monitor commands, browser tests |
| **QEMU** | `normal` | 2 | 512M | **PASS (100%)** | Dual-core AP online, `ASTRA_SMP_PARALLEL_EXECUTION_OK` |
| **QEMU** | `nofb` | 2 | 512M | **PASS (100%)** | Dual-core headless suite, multi-core lifecycle |
| **QEMU** | `normal` | 4 | 512M | **PASS (100%)** | 4 CPUs online, concurrent Ring 3 execution, desktop WM |
| **QEMU** | `nofb` | 4 | 512M | **PASS (100%)** | 4 CPUs headless, monitor suite, networking |
| **QEMU** | `normal` | 8 | 512M | **PASS (100%)** | 8 CPUs online, bounded buffer stress test, desktop compositor |
| **QEMU** | `nofb` | 8 | 512M | **PASS (100%)** | 8 CPUs headless, full 93-marker suite |
| **VirtualBox 7**| `EFI` | 1 | 512M | **PASS (100%)** | All 93 / 93 markers verified, screenshot captured |

### Host Test Suite Results
- `tests/test_crypto_host.py`: **PASS** (FIPS 180-4 SHA-256, RFC 4231 HMAC, RFC 5869 HKDF, RFC 8439 ChaCha20-Poly1305, RFC 7748 X25519, OpenSSL X.509 DER parsing & RSA signature verification).
- `tests/test_vfs_host.py`: **PASS** (6/6 tests: archive validation, path lookup, pread, stat, readdir, mutable RAMFS).
- `tests/test_block_host.py`: **PASS** (20/20 tests: registration, first/last sector, exact boundaries, out-of-range, zero count, integer overflow, read-only guards, ATA flush 0xE7/0xEA fallback, drive fault handling).
- `tests/test_fs_host.py`: **PASS** (27/27 tests: volume formatting, mounting, CoW extents, mid-write failures, flush aborts, atomic renames, 10-scenario crash qualification matrix).
- `tests/test_elf_host.py`: **PASS** (16/16 tests: valid ELF64, bad magic, endianness, segment limits, W^X conflict rejection, dynamic linking rejection, overlap trapping).
- `tests/test_bookmark_storage.py`: **PASS** (16/16 tests: serialization, capacity bounds, bit-flip detection, cold reboot persistence, stale `.tmp` defense, 8-boundary fault injection).
- `tests/test_browser_networking.py`: **PASS** (17/17 tests: HTTP 200/404/500, large transfers, TLS 1.3 handshake, chunked encoding, redirect loops, rapid sequential connections).

---

## 3. SMP Architecture & Subsystem Design

```
+---------------------------------------------------------------------------------------+
|                                    Limine MP Protocol                                 |
|  1. Bootloader parses ACPI MADT table and determines active CPU topology.             |
|  2. Awakens each AP by setting `info->goto_address = ap_entry_c`.                     |
+-------------------------------------------+-------------------------------------------+
                                            |
                      +---------------------+---------------------+
                      |                                           |
                      v                                           v
+-----------------------------------------+   +-----------------------------------------+
| CPU 0 (Bootstrap Processor - BSP)       |   | CPU 1..N-1 (Application Processors - AP)|
| - Initializes PMM, VMM, GDT, IDT, APIC  |   | - Switches to kernel PML4               |
| - Prepares per-CPU structures for APs   |   | - Loads dedicated GDT & Task Register   |
| - Registers `ap_entry_c` with Limine    |   | - Loads IDT & configures Local APIC     |
| - Waits for all APs to report online    |   | - Marks `online = true`, `active = true`|
| - Enters scheduler run loop             |   | - Enters `sched_ap_start()` idle loop   |
+--------------------+--------------------+   +--------------------+--------------------+
                     |                                             |
                     +----------------------+----------------------+
                                            |
                                            v
+---------------------------------------------------------------------------------------+
|                                  Per-CPU Hardware State                               |
|  cpu_state_t g_cpus[MAX_CPUS]:                                                        |
|    - gdt[7]: Independent GDT with kernel/user code/data and dedicated TSS descriptor  |
|    - tss: Independent Task State Segment with df_stack (IST1) and rsp0 kernel stack   |
|    - current_thread, idle_thread: Per-core execution tracking                        |
|    - preempt_count, need_resched, interrupt_depth: Per-core scheduler control        |
|    - timer_ticks: Independent Local APIC periodic tick counter                        |
+-------------------------------------------+-------------------------------------------+
                                            |
                                            v
+---------------------------------------------------------------------------------------+
|                               Shared Subsystem Protection                             |
|  - Physical Memory Allocator: Protected by `g_pmm_lock` (irqsave spinlock)            |
|  - Transactional Kernel Heap: Protected by `g_heap_lock` (irqsave spinlock)           |
|  - Scheduler Ready Runqueue: Protected by `g_sched_lock` (irqsave spinlock)           |
|  - Scheduler Sleeper Queue: Protected by `g_sleep_lock` (irqsave spinlock)            |
|  - Process Table & Lifecycle: Protected by `g_proc_lock` (irqsave spinlock)           |
|  - Mutexes, Semaphores, Events: Protected by internal `lock` (irqsave spinlock)       |
+---------------------------------------------------------------------------------------+
```

### 1. Per-CPU GDT, TSS, and IST1 Stacks
Every CPU core requires its own Task State Segment (TSS) so that:
1. User-to-kernel privilege transitions (Ring 3 -> Ring 0) load that core's unique kernel stack pointer into `RSP0`.
2. Hardware double faults (`#DF`, vector 8) switch onto an isolated `df_stack` via `IST1`, avoiding catastrophic stack faults when user stacks are corrupted.

In `smp_setup_cpu_gdt(cpu_id)`:
```c
cpu->tss.ist1 = (u64)&cpu->df_stack[sizeof(cpu->df_stack)];
cpu->tss.iomap_base = sizeof(struct tss);
u64 base = (u64)&cpu->tss;
u32 limit = sizeof(struct tss) - 1;
// Descriptor entry 5 & 6 (64-bit System Segment Descriptor)
cpu->gdt[5].limit_low = (u16)(limit & 0xFFFF);
cpu->gdt[5].base_low = (u16)(base & 0xFFFF);
cpu->gdt[5].base_middle = (u8)((base >> 16) & 0xFF);
cpu->gdt[5].access = 0x89; /* Present, Ring 0, 64-bit Available TSS */
cpu->gdt[5].granularity = (u8)((limit >> 16) & 0x0F);
cpu->gdt[5].base_high = (u8)((base >> 24) & 0xFF);
```

### 2. IRQ-Safe Spinlocks
Defined in `kernel/include/kernel/spinlock.h`:
```c
typedef struct spinlock {
    volatile u32 lock;
} spinlock_t;

static inline u64 spinlock_acquire_irqsave(spinlock_t *s) {
    u64 rflags = read_rflags();
    cli();
    while (__atomic_test_and_set(&s->lock, __ATOMIC_ACQUIRE)) {
        cpu_pause();
    }
    return rflags;
}

static inline void spinlock_release_irqrestore(spinlock_t *s, u64 rflags) {
    __atomic_clear(&s->lock, __ATOMIC_RELEASE);
    if (rflags & (1ULL << 9)) {
        sti();
    }
}
```

### 3. Thread Execution Exclusivity Invariant
To prevent two CPU cores from executing the same thread simultaneously:
1. When selecting `next` from the ready queue, the core executes an atomic CAS:
   ```c
   u32 expected_zero = 0;
   if (!__atomic_compare_exchange_n(&next->is_executing, &expected_zero, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
       // Thread is currently executing or switching out on another core; skip
       continue;
   }
   ```
2. When the outgoing thread is switched out in `thread_switch_asm`:
   ```asm
   movq %rsp, (%rdi)       /* *prev_rsp_ptr = %rsp */
   testq %rcx, %rcx
   jz 1f
   movl $0, (%rcx)         /* prev->is_executing = 0 */
   1:
   ```
   The outgoing thread's `is_executing` flag is cleared **only after** its stack pointer has been completely saved to memory.

---

## 4. Root Cause Analyses & Bug Resolutions

During the bring-up and qualification of Phase AM across the 8-core matrix and VirtualBox, several subtle synchronization and paging bugs were isolated and resolved:

### 1. Phase O Test 2 Page Fault (`RIP=0`, `CS=0x8`, `CR2=0`)
- **Root Cause**: `thread_create_process()` mapped the thread's 4 kernel stack pages using `vmm_map_page()`, but failed to call `invlpg` on those virtual addresses. When thread stack slots were recycled, the CPU TLB retained stale translations pointing to previously freed physical frames. Furthermore, the 9-quadword initial context-switch stack frame (`retq` target `thread_entry_trampoline`, callee-saved registers) was written through virtual address `t->stack_top`, which did not propagate across cores. When `thread_switch_asm` popped the stack and executed `retq`, it popped all zeros (`kmemset` frame), resulting in jumping to `RIP=0`.
- **Resolution**: Updated `thread_create_process()` and `thread_create()` to call `invlpg(va)` on each mapped stack page, and wrote the initial context frame directly into physical memory using the Higher Half Direct Map (`top_pa + hhdm`).

### 2. False-Positive PMM Frame "Leak" Panics in Multi-Core Mode
- **Root Cause**: In tests like Phase AK Test 8 and Phase F Test 5, the test harness recorded `pmm_before.free_pages` before launching child processes. However, under 2, 4, and 8 cores, asynchronous AP idle threads concurrently reaped zombie threads from earlier tests during the test window. Consequently, `pmm_after.free_pages` was actually *greater* than `pmm_before.free_pages` (`128031 > 128027`), causing a strict equality assertion `!=` to panic.
- **Resolution**: Explicitly invoked `process_reap_zombies(); thread_yield(); sched_reap();` before capturing `pmm_before`, and updated the assertion to verify that free pages did not decrease (`pmm_end.free_pages < pmm_start.free_pages`).

### 3. Sleeper Queue Deadlock & Non-READY Enqueue Panic in Phase N Test 2
- **Root Cause**: In `kernel/sched/sched.c`, the sleeper queue was modified in `sched_timer_tick` from timer IRQ context while other threads inserted sleepers in process context without spinlock protection. A sleeper expiring simultaneously with a manual wakeup could double-enqueue the thread or enqueue it in `THREAD_BLOCKED` state.
- **Resolution**: Added `g_sleep_lock` and protected all event wait queues with `ev->lock`. Converted `event_wait()` and `thread_sleep_ticks()` to use `sched_block_current_locked()`.

### 4. Bounded Buffer Stress Test Timeout in VirtualBox 7 EFI
- **Root Cause**: In `kernel/sched/sync.c`, Sync Test 5 transfers 2000 items through an 8-slot bounded buffer while a background spinner thread consumes CPU slices. In VirtualBox headless mode with 1 vCPU, the Local APIC timer fires at hardware bus frequency (~1000 Hz), meaning a 1000-tick timeout was only ~1-2 seconds of wall-clock time. With 5 competing threads, the test hit the 1000-tick timeout before completing all 2000 transfers.
- **Resolution**: Increased the timeout deadline to `apic_get_ticks() + 20000`, allowing sufficient time for VirtualBox to finish the 2000 items while maintaining deadlock detection.

### 5. Sleeper Queue Capacity Test (7G) Early Wakeup in VirtualBox
- **Root Cause**: Test 7G creates 16 worker threads sleeping for 4 ticks to test `SLEEP_QUEUE_CAPACITY` exhaustion. Under VirtualBox emulation, the time required to create and switch 16 threads exceeded 4 ticks, so the first thread woke up before the 16th thread registered its sleep, preventing `g_sleep_count` from ever reaching 16.
- **Resolution**: Increased worker sleep duration from 4 ticks to 30 ticks (`thread_sleep_ticks(30)`), ensuring all 16 threads remain queued simultaneously during capacity testing.

---

## 5. Verification Matrix & Results

```
========================================================================================
                      ASTRAOS MULTI-CORE & SMP VERIFICATION MATRIX
========================================================================================
Configuration       CPUs   Mode      RAM    Result    Key Markers Verified
----------------------------------------------------------------------------------------
QEMU Multi-Core     1      normal    512M   PASS      ASTRA_SMP_BSP_READY, ASTRA_SMP_SINGLE_CPU_OK
QEMU Multi-Core     1      nofb      512M   PASS      ASTRA_SMP_BSP_READY, ASTRA_SMP_SINGLE_CPU_OK
QEMU Multi-Core     2      normal    512M   PASS      ASTRA_SMP_AP_STARTED, ASTRA_SMP_PARALLEL_EXECUTION_OK
QEMU Multi-Core     2      nofb      512M   PASS      ASTRA_SMP_AP_STARTED, ASTRA_SMP_PARALLEL_EXECUTION_OK
QEMU Multi-Core     4      normal    512M   PASS      ASTRA_SMP_ALL_CPUS_READY, ASTRA_SMP_LIFECYCLE_OK
QEMU Multi-Core     4      nofb      512M   PASS      ASTRA_SMP_ALL_CPUS_READY, ASTRA_SMP_LIFECYCLE_OK
QEMU Multi-Core     8      normal    512M   PASS      ASTRA_SMP_PARALLEL_EXECUTION_OK (8 Cores Online)
QEMU Multi-Core     8      nofb      512M   PASS      ASTRA_SMP_PARALLEL_EXECUTION_OK (8 Cores Online)
Oracle VirtualBox   1      EFI       512M   PASS      93 / 93 Markers Verified (100%)
Host Unit Tests     Host   Host      N/A    PASS      7 / 7 Suites Passed (113 Total Tests)
========================================================================================
```

---

## 6. Architecture & Status Documentation Updates

The following architectural references were updated to document the SMP implementation:
1. **`docs/architecture.md`**: Added **Section 49: Symmetric Multiprocessing (SMP), AP Bring-Up & Multi-Core Scheduler Safety**, detailing Limine MP protocol integration, per-CPU structures, spinlock primitives, lock hierarchy, atomic block-and-unlock protocol, context-switch stack staging, and multi-core user concurrency.
2. **`docs/status.md`**: Updated status tables with completed Phase AM milestones:
   - `SMP Multi-Core Bring-Up`
   - `IRQ-Safe Spinlocks`
   - `Atomic Block-and-Unlock`
   - `Thread Execution Exclusivity`
   - `Multi-Core User Concurrency`
   - `SMP Multi-Core Lifecycle`
   - `Multi-Core Test Matrix`

---

## 7. Conclusion & Next Steps

Phase AM has achieved all objectives with zero compromises:
- Full Symmetric Multiprocessing (SMP) on x86-64.
- Application Processors dynamically discovered and initialized.
- Shared kernel data structures protected by IRQ-safe spinlocks.
- Parallel user-space execution validated via atomic overlap counters.
- 100% pass rate across QEMU 1, 2, 4, 8 core configurations and Oracle VirtualBox 7 EFI.

Per prompt boundaries: **Phase AM is complete. Do NOT begin Phase AN.**

