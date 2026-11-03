# Phase AM Architectural Audit: SMP Bring-Up, AP Initialization & Multi-Core Scheduler Safety

**Workspace:** `C:\Users\Akash\Documents\MyKernel`  
**Phase:** AM — Symmetric Multiprocessing (SMP) Bring-Up  
**Date:** October 31, 2026  
**Status:** AUDIT COMPLETE — IMPLEMENTATION READY  

---

## 1. Executive Summary & Objective

In Phase AL, AstraOS successfully qualified timer-driven preemptive multitasking on the Bootstrap Processor (BSP), demonstrating involuntary preemption of CPU-bound Ring 3 processes, multi-process address-space isolation, blocking/wakeup queues, and parent-child lifecycle semantics. However, the Phase AL audit explicitly determined that Symmetric Multiprocessing (SMP) was **NOT IMPLEMENTED**: secondary Application Processors (APs) remained halted in firmware idle loops, and all kernel and user threads executed exclusively on CPU 0.

The objective of **Phase AM** is to implement genuine Symmetric Multiprocessing (SMP) in AstraOS. This entails:
1. Discovering and initializing secondary Application Processors (APs) via the 64-bit Limine bootloader protocol.
2. Establishing independent per-CPU execution contexts (per-CPU GDT, TSS, kernel interrupt stacks, IST1 double-fault stacks, and Local APIC configurations).
3. Implementing a concurrency-safe scheduler architecture that prevents duplicate thread execution, protects shared runqueues, and maintains per-CPU current thread state.
4. Providing independent Local APIC timer interrupts and preemption on each active CPU core.
5. Upgrading kernel synchronization primitives from single-core interrupt masking (`cli`/`sti`) to true SMP spinlocks with bounded lock contention across PMM, Heap, Process Table, VFS, and Mutex subsystems.
6. Demonstrating genuine concurrent Ring 3 parallel execution with deterministic overlap measurement and emitting `ASTRA_SMP_PARALLEL_EXECUTION_OK`.
7. Ensuring process lifecycle safety, address-space destruction safety, and CPU failure resilience across 1, 2, 4, and 8 CPU topologies.

---

## 2. Audit of Existing Single-Core Architecture

A thorough inspection of the AstraOS codebase reveals the specific subsystems that currently assume single-core BSP-only execution:

### 2.1 Bootloader & SMP Discovery
* **File:** `kernel/boot/limine_reqs.c`
* **Current State:** Contains requests for Base Revision, Framebuffer, Memory Map, HHDM, Kernel File, Kernel Address, and Modules. **The Limine SMP request (`LIMINE_SMP_REQUEST`) is completely absent.**
* **Consequence:** The Limine bootloader boots the BSP (CPU 0) and leaves all AP cores indefinitely suspended in firmware halt loops. The kernel possesses zero information regarding the number of physical/logical cores, APIC IDs, or processor topology.

### 2.2 Descriptor Tables (GDT, TSS) & Interrupt Stacks
* **File:** `kernel/arch/x86_64/gdt.c`, `kernel/arch/x86_64/gdt.h`
* **Current State:** Defines a single global GDT (`static struct gdt_entry g_gdt[7]`) and a single Task State Segment (`static struct tss g_tss`).
* **Hazard:** In x86_64, the Task Register (TR) references the TSS descriptor in the GDT. When an interrupt or syscall occurs from Ring 3 to Ring 0, the CPU hardware automatically loads `%rsp` from `TSS.rsp0`. In addition, Double Fault (`#DF`, Vector 8) loads `%rsp` from `TSS.ist1`. If multiple CPUs share the same TSS or GDT, privilege transitions on Core 0 and Core 1 simultaneously will overwrite `g_tss.rsp0` and corrupt kernel stack frames, leading to immediate system crash (`#DF` or `#GP`).
* **Resolution Requirement:** Each CPU core must possess an independent TSS structure, independent `ist1` stack, and its own GDT with a dedicated TSS descriptor loaded via `ltr`.

### 2.3 Interrupt Descriptor Table (IDT)
* **File:** `kernel/arch/x86_64/idt.c`, `kernel/arch/x86_64/idt.h`
* **Current State:** A single global 256-entry table (`static struct idt_entry g_idt[256]`) initialized during BSP boot.
* **SMP Safety:** The IDT contains read-only gate descriptors pointing to common interrupt stubs (`isr_stub_*`, `irq_stub_*`, `syscall_entry`). Each CPU core only needs to load the IDT register via `lidt` with `&g_idtr`. The IDT itself can safely be shared read-only across all CPU cores.

### 2.4 Local APIC & Timers
* **File:** `kernel/drivers/apic.c`, `kernel/drivers/apic.h`
* **Current State:** Maps Local APIC registers at `APIC_VIRT_BASE` (`0xffffffff80700000ULL`). `apic_init()` configures the BSP Local APIC, enables Spurious Interrupt Vector 255, and sets up Periodic Timer Vector 32.
* **Hazard:** In xAPIC mode, all CPUs access their respective core-local APIC registers via the identical physical MMIO address (0xFEE00000), mapped at `APIC_VIRT_BASE`. However, `apic_init()` is called only once by the BSP in `kmain()`. Secondary APs boot with their Local APIC disabled or unconfigured, receiving no timer ticks and incapable of preemption.
* **Resolution Requirement:** Each AP must independently initialize its Local APIC registers (SVR, TPR, LVT Timer, Timer Divide, Initial Count) upon startup.

### 2.5 Scheduler State & Thread Pointer
* **File:** `kernel/sched/sched.c`, `kernel/sched/switch.S`
* **Current State:**
  - `thread_t *g_current_thread`: A single global pointer to the currently running thread.
  - `switch.S` contains: `movq %rdx, g_current_thread(%rip)`.
  - `thread_current()` returns `g_current_thread`.
* **Hazard:** If CPU 0 runs Thread A and CPU 1 runs Thread B, updating `g_current_thread` will cause one core to overwrite the other core's active thread pointer. System calls, page faults, and context switches on CPU 0 would then operate on Thread B's memory and stack.
* **Resolution Requirement:** `current_thread` must be stored in a per-CPU control block (`g_cpus[cpu_id].current_thread`). `thread_current()` must resolve the calling CPU's identity (via Local APIC ID lookup or per-CPU base) and return the caller CPU's thread.

### 2.6 Scheduler Ready Queue & Concurrency
* **File:** `kernel/sched/sched.c`
* **Current State:** `g_ready_head` and `g_ready_tail` form a global FIFO queue. Critical sections in `sched_enqueue()`, `sched_dequeue()`, and `thread_yield()` are guarded solely by local interrupt disabling (`cli()` / `sti()`).
* **Hazard:** On multi-core SMP, `cli()` only masks interrupts on the local core. Another core running concurrently will execute `sched_dequeue()` or `sched_enqueue()` simultaneously, resulting in linked-list pointer corruption, lost threads, or two CPUs selecting and executing the identical thread.
* **Resolution Requirement:** A global scheduler spinlock (`spinlock_t g_sched_lock`) must serialize all ready-queue manipulations. Furthermore, each thread must feature an atomic execution flag (`volatile u32 is_executing`) to prevent another core from restoring a thread's stack before the relinquishing core has finished saving registers in `thread_switch_asm`.

### 2.7 Memory Allocators (PMM & Heap)
* **File:** `kernel/mm/pmm.c`, `kernel/mm/heap.c`
* **Current State:**
  - `pmm_alloc_page()` and `pmm_free_page()` use `u64 flags = irq_save(); ... irq_restore(flags);`.
  - `kmalloc()` and `kfree()` use `preempt_disable(); ... preempt_enable();`.
* **Hazard:** Neither `irq_save()` nor `preempt_disable()` provides cross-core mutual exclusion. Simultaneous allocations from threads executing on different CPUs will corrupt the PMM physical frame bitmap and the heap free-list links (`next_free`, `prev_free`, block capacity splits).
* **Resolution Requirement:** Introduce dedicated spinlocks: `spinlock_t g_pmm_lock` for physical frame management and `spinlock_t g_heap_lock` for the kernel heap allocator.

### 2.8 Process Table & Process Lifecycle
* **File:** `kernel/process/process.c`
* **Current State:** `g_process_table[PROCESS_MAX_PROCESSES]` is accessed without locking. Finding unused slots (`PROCESS_UNUSED`), setting `proc->state`, updating `parent_pid`, and zombie reaping have no synchronization across cores.
* **Resolution Requirement:** Introduce `spinlock_t g_proc_lock` to serialize process table searches, state transitions, PID assignments, and child exit signaling.

### 2.9 Mutex & Semaphore Synchronization
* **File:** `kernel/sched/sync.c`
* **Current State:** Uses `SYNC_ENTER_CRITICAL` (`pushfq; popq %0; cli`).
* **Hazard:** In SMP, two threads on different CPUs calling `mutex_lock()` concurrently will race on `m->owner == NULL`, both claiming ownership simultaneously.
* **Resolution Requirement:** Incorporate spinlock protection into `mutex_t` and `semaphore_t` to guard ownership handoff and waitqueue updates.

---

## 3. AP Startup Feasibility Analysis

### 3.1 Limine SMP Protocol
Limine provides a standardized, reliable multi-processor boot protocol for x86_64:
1. When `struct limine_smp_request smp_request` is registered in the `.limine_requests` section, Limine firmware boots all available Application Processors into 64-bit long mode.
2. Limine configures each AP with:
   - Identical virtual memory mapping (kernel PML4 and HHDM).
   - A dedicated 64 KiB stack allocated per AP.
   - Paging enabled, interrupts disabled (`cli`).
   - A tight firmware polling loop monitoring `info->goto_address`.
3. The response structure `struct limine_smp_response` supplies:
   - `cpu_count`: Total number of logical cores detected.
   - `bsp_lapic_id`: Hardware Local APIC ID of the Bootstrap Processor.
   - `cpus`: Array of pointers to `struct limine_smp_info`, containing `processor_id`, `lapic_id`, and `goto_address`.
4. When the BSP is ready to release an AP, it writes the address of the AP startup routine:
   ```c
   info->goto_address = ap_early_entry;
   ```
   accompanied by a memory barrier (`__sync_synchronize()`). The AP breaks out of its firmware spinloop and branches immediately to `ap_early_entry(info)`.

### 3.2 Handshake & Synchronization Protocol
To prevent race conditions during boot:
1. The BSP initializes all core data structures, memory managers, spinlocks, and per-CPU state arrays before releasing any APs.
2. The BSP emits the marker:
   `ASTRA_SMP_BSP_READY`
3. For each secondary AP `i` (where `info->lapic_id != bsp_lapic_id`):
   - The BSP configures AP `i`'s stack, TSS, and GDT.
   - The BSP sets `info->goto_address = ap_early_entry`.
   - The AP executes `ap_early_entry`: loads GDT, loads TR, loads IDT, initializes its Local APIC, configures its Local APIC timer, creates its idle thread, sets its `online = true` flag, and emits:
     `ASTRA_SMP_AP_STARTED`
   - The BSP polls `g_cpus[i].online` with a calibrated bounded timeout (50 ms). If the AP fails to set `online`, the BSP logs a non-fatal warning, marks the CPU offline, and proceeds without hanging.
4. Once all responsive APs have completed startup, the BSP verifies online CPU count and emits:
   `ASTRA_SMP_ALL_CPUS_READY`

---

## 4. Missing SMP Components Inventory

| Component | Current State | Required SMP Architecture |
|---|---|---|
| **Limine SMP Request** | Absent in `limine_reqs.c` | Add `limine_smp_request` with Base Revision 3 compatibility |
| **SMP Subsystem (`smp.c` / `smp.h`)** | Not implemented | Implement CPU discovery, AP bring-up, topology mapping, and status tracking |
| **Per-CPU State Structure** | Single global variables | Define `struct cpu_state g_cpus[MAX_CPUS]` with per-CPU current/idle threads, preempt count, tick count |
| **Per-CPU GDT & TSS** | Single global GDT/TSS | Implement `g_gdt[MAX_CPUS]` and `g_tss[MAX_CPUS]` with independent `rsp0` and `ist1` stacks |
| **Spinlock Primitives** | No spinlocks exist in codebase | Implement `spinlock_t` with `acquire`, `release`, `try_acquire`, and `irqsave` variants |
| **PMM Spinlock** | Uses `irq_save()` (`cli`) | Guard all page frame allocations/frees with `g_pmm_lock` |
| **Heap Spinlock** | Uses `preempt_disable()` | Guard arena block splitting/coalescing with `g_heap_lock` |
| **Process Table Spinlock** | Unsynchronized global array | Guard `g_process_table`, PID allocation, and state transitions with `g_proc_lock` |
| **Scheduler Runqueue Lock** | Uses `cli()` | Guard `g_ready_head`/`g_ready_tail` with `g_sched_lock` |
| **Cross-Core Execution Safety** | None (threads switch blindly) | Add atomic `is_executing` flag to `thread_t` to prevent concurrent stack corruption |
| **Per-CPU APIC Timers** | BSP-only timer initialization | Configure Local APIC timer independently on every started AP |
| **Per-CPU Idle Threads** | Single global `g_idle_thread` | Create dedicated `idle_thread` per CPU that executes `sti; hlt` when no threads are ready |
| **Parallel Execution Workload** | Single-core tests only | Author Ring 3 `cpu_worker_a`, `cpu_worker_b`, `heartbeat`, and `blocking_worker` with overlap detection |

---

## 5. Shared-State Concurrency & Race Risk Analysis

Seven primary shared data structures require rigorous synchronization to avoid data corruption, deadlocks, and use-after-free conditions under multi-core SMP:

### 5.1 Lock Ordering Hierarchy
To guarantee zero deadlocks across all subsystems, lock acquisitions must strictly adhere to the following global hierarchy:

`g_proc_lock` -> `g_sched_lock` -> `g_heap_lock` -> `g_pmm_lock`

* **Rule 1:** A spinlock must **never** be held while calling a blocking function (`thread_yield`, `mutex_lock`, `sys_sleep`).
* **Rule 2:** High-level locks (e.g. `g_proc_lock`) may acquire lower-level locks (e.g. `g_sched_lock` or `g_heap_lock`), but never the reverse.
* **Rule 3:** The PMM lock (`g_pmm_lock`) is the leaf lock; no other lock may be acquired while holding `g_pmm_lock`.
* **Rule 4:** All spinlock acquisitions from thread context must disable local interrupts (`spinlock_acquire_irqsave`) to prevent deadlock if an interrupt handler attempts to acquire the identical lock on the same CPU.

---

## 6. Proposed Implementation Sequence

### Phase 1: Limine SMP Request & Core Discovery (Task 1)
- Add `struct limine_smp_request` in `kernel/boot/limine_reqs.c` and accessor in `kernel/include/kernel/boot.h`.
- Create `kernel/include/kernel/smp.h` and `kernel/arch/x86_64/smp.c`.
- Parse `limine_smp_response`: enumerate discovered CPUs, validate APIC IDs, and identify BSP.
- Emit `ASTRA_SMP_BSP_READY`.

### Phase 2: Per-CPU Hardware Structures & AP Bring-Up (Task 1 & Task 3)
- Allocate per-CPU stacks (16 KiB kernel stack, 16 KiB IST1 double-fault stack per CPU).
- Initialize per-CPU GDT entries and TSS instances with valid `ist1` and `rsp0`.
- Implement `ap_early_entry()`: set CR3 to kernel PML4, load GDT (`lgdt`), load TR (`ltr`), load IDT (`lidt`).
- Initialize Local APIC on each AP: configure SVR (Vector 255), TPR, and LVT Timer (Vector 32, divide 16, periodic).
- Emit `ASTRA_SMP_AP_STARTED` from each AP.
- BSP verifies all responsive APs and emits `ASTRA_SMP_ALL_CPUS_READY`.

### Phase 3: SMP Spinlock Primitives (Task 4)
- Implement `spinlock_t` in `kernel/include/kernel/sync.h` using `__atomic_test_and_set` with `PAUSE` instructions and acquire/release semantics.
- Provide `spinlock_acquire`, `spinlock_release`, `spinlock_try_acquire`, `spinlock_acquire_irqsave`, and `spinlock_release_irqrestore`.
- Integrate spinlocks into `kernel/mm/pmm.c` (`g_pmm_lock`), `kernel/mm/heap.c` (`g_heap_lock`), and `kernel/process/process.c` (`g_proc_lock`).
- Upgrade `mutex_t` and `semaphore_t` internal queue transitions to use spinlocks.

### Phase 4: Per-CPU Scheduler Architecture (Task 2)
- Define `cpu_state_t g_cpus[MAX_CPUS]` with per-CPU `current_thread`, `idle_thread`, `preempt_count`, `need_resched`, and `timer_ticks`.
- Implement `smp_cur_cpu_id()` using Local APIC ID mapping table.
- Update `thread_current()` to return `g_cpus[smp_cur_cpu_id()].current_thread`.
- Add `g_sched_lock` to serialize ready queue operations in `kernel/sched/sched.c`.
- Add `volatile u32 is_executing` in `thread_t` and update `switch.S` / `sched_prepare_switch` to prevent cross-CPU thread collision.
- Create dedicated `idle_thread` per CPU.

### Phase 5: Genuine Parallel Execution Ring 3 Test (Task 5)
- Author Ring 3 applications: `cpu_worker_a.c` and `cpu_worker_b.c` (CPU-bound loops that never yield), `heartbeat.c`, and `blocking_worker.c`.
- Implement `sys_getcpuid()` (syscall 11) returning `smp_cur_cpu_id()`.
- Add atomic progress and overlap counters in shared kernel memory.
- Verify that Worker A and Worker B execute concurrently on distinct CPUs with overlapping timestamps.
- Emit `ASTRA_SMP_PARALLEL_EXECUTION_OK`.

### Phase 6: Process Lifecycle & Failure Safety (Task 6 & Task 7)
- Verify concurrent process creation, termination, zombie reaping, and `waitpid` under SMP.
- Implement address-space safety: verify no core is executing with `CR3 == proc->pml4_phys` before freeing page tables.
- Handle non-responsive APs gracefully with bounded timeouts.

### Phase 7: Regression & Qualification (Task 8 & Task 9)
- Run QEMU matrix: 1, 2, 4, 8 CPUs in both `normal` and `nofb` modes.
- Run all 12 host unit test suites (`test_elf_host`, `test_block_host`, `test_fs_host`, `test_vfs_host`, etc.).
- Run Oracle VirtualBox 7 EFI test.
- Author `phase_am_report.md` with full status matrix.

---

## 7. Explicit Architectural Boundaries & Unsupported Features

To ensure uncompromising stability and documentation integrity, the following capabilities are explicitly classified as out-of-scope or unsupported for Phase AM:

1. **Inter-Processor Interrupt (IPI) Broadcast TLB Shootdown:**
   AstraOS will enforce process address-space destruction safety by verifying that no CPU is currently running inside the dying process's PML4 (and switching CR3 back to kernel PML4 prior to freeing). Complex IPI-driven TLB shootdown across running threads of the *same* multi-threaded process is deferred, as user processes in AstraOS are currently single-threaded.
2. **Dynamic Work-Stealing / Load-Balancing Heuristics:**
   A centralized ready queue protected by `g_sched_lock` ensures fair, starvation-free scheduling across all active CPUs. Distributed per-CPU queues with complex work-stealing algorithms will not be implemented prematurely.
3. **CPU Hot-Plugging:**
   CPU topology is fixed at boot time as reported by Limine. Dynamic addition or removal of physical CPUs at runtime is not supported.

---

## 8. Audit Conclusion

The prerequisites for Symmetric Multiprocessing in AstraOS have been thoroughly evaluated. The Limine bootloader protocol natively supports multi-processor startup to 64-bit long mode. By introducing per-CPU hardware structures (GDT, TSS, stacks), core-local APIC timers, atomic thread execution locks, and kernel spinlocks across PMM, Heap, and Scheduler subsystems, AstraOS can achieve true, robust, parallel execution.

Rule 0 audit requirements are satisfied in full. Implementation may proceed.

