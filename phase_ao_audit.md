# Phase AO Audit — AstraOS SMP Correctness Proof, Adversarial Testing & Failure Recovery

**Date:** October 31, 2026  
**Status:** AUDIT COMPLETED (RULE 0)  
**Workspace:** `C:\Users\Akash\Documents\MyKernel`

---

## 1. Executive Summary

Phases AM and AN introduced Symmetric Multiprocessing (SMP) bring-up and fundamental memory safety mechanisms (Local APIC IPI TLB shootdown at vector 251, `active_cpu_mask` tracking, and `switch.S` context-switch stack exclusivity). 

This Phase AO audit performs an exhaustive concurrency and memory-safety analysis across all multi-core call paths:
1. Address space activation, switching, and `proc->active_cpu_mask` synchronization.
2. Concurrent TLB shootdown protocol serialization, request generation numbering, and stale acknowledgement prevention.
3. Physical memory reclamation sequencing vs remote TLB invalidation.
4. Process termination, zombie reaping, and thread deferred reclamation.
5. Global lock ordering, acquisition hierarchies, and deadlock cycle detection.
6. Per-CPU state isolation via `IA32_GS_BASE` (MSR `0xC0000101`).
7. Concurrent memory allocator and storage device fault injection.

---

## 2. Identified Concurrency Defects & Vulnerabilities

### Issue 1: Deactivation Ordering Inversion in `sched_prepare_switch()`
- **Shared State**: `prev->process->active_cpu_mask` and hardware `CR3`.
- **Competing CPU Operations**:
  - CPU A runs `sched_prepare_switch(prev, next)` switching away from `prev->process`.
  - CPU B runs `process_exit()` or `vmm_destroy_process_space()` tearing down `prev->process`.
- **Race / Invalid State**:
  In `sched_prepare_switch()`:
  ```c
  if (prev && prev->process) {
      __atomic_fetch_and(&prev->process->active_cpu_mask, ~(1U << cpu_id), __ATOMIC_SEQ_CST);
  }
  ...
  if (next_cr3 != 0 && (read_cr3() & CR3_ADDR_MASK) != (next_cr3 & CR3_ADDR_MASK)) {
      write_cr3(next_cr3);
  }
  ```
  `active_cpu_mask` has bit `cpu_id` cleared *before* `write_cr3(next_cr3)` is executed. CPU B observes `active_cpu_mask == 0`, concludes no CPUs are using `prev->process->pml4_phys`, sends a shootdown, and immediately frees page tables with `pmm_free_page()`. Meanwhile, CPU A is still executing with `prev->process->pml4_phys` active in its hardware `CR3` until `write_cr3` completes.
- **Expected Invariant**: A CPU's bit in `proc->active_cpu_mask` must remain set for as long as that CPU's hardware `CR3` holds `proc->pml4_phys`.
- **Proposed Correction**: Invert the deactivation sequence:
  1. If `prev->process != next->process`: set `next->process->active_cpu_mask |= (1 << cpu_id)`.
  2. Execute `write_cr3(next_cr3)`.
  3. Clear `prev->process->active_cpu_mask &= ~(1 << cpu_id)`.
- **Reproducible Test**: Multi-core stress test where CPU A rapidly context-switches in/out of an address space while CPU B polls `active_cpu_mask` and validates that `read_cr3()` matches.

---

### Issue 2: Transient Zeroing of `active_cpu_mask` on Intra-Process Thread Switches
- **Shared State**: `proc->active_cpu_mask`.
- **Competing CPU Operations**:
  - CPU A switches between two threads belonging to the same process `P` (`prev->process == next->process`).
  - CPU B checks `proc->active_cpu_mask` during teardown or memory inspection.
- **Race / Invalid State**:
  `sched_prepare_switch()` unconditionally cleared bit `cpu_id` on `prev->process`, then re-set bit `cpu_id` on `next->process`. For the duration of several instructions between the clear and set, `active_cpu_mask` momentarily dropped bit `cpu_id`. If `P` was a single-threaded process or only active on CPU A, `active_cpu_mask` dropped to 0, falsely signalling quiescence.
- **Expected Invariant**: Intra-process thread switches must never clear the CPU's bit in `active_cpu_mask`.
- **Proposed Correction**: Guard mask updates with `if (prev->process != next->process)`.

---

### Issue 3: Whole-Address-Space Shootdown Stale CR3 Reload on Remote Cores
- **Shared State**: Hardware `CR3` on remote CPUs during `smp_tlb_shootdown(pml4_phys, 0, 0)`.
- **Competing CPU Operations**:
  - CPU A destroys a process address space (`vmm_destroy_process_space` / `process_exit`).
  - CPU B executes `smp_handle_tlb_shootdown()` for `page_count == 0`.
- **Race / Invalid State**:
  In `smp_handle_tlb_shootdown()`:
  ```c
  if (req_pml4 == 0 || cur_cr3 == req_pml4 || va >= 0xFFFF800000000000ULL) {
      if (count == 0) {
          write_cr3(read_cr3());
      }
  }
  ```
  When `req_pml4 == cur_cr3` and `count == 0`, CPU B reloads its current CR3. It flushes the TLB, but its hardware `CR3` remains pointing to `req_pml4`! CPU A receives the ACK, assumes CPU B is no longer referencing `req_pml4`, and proceeds to free all page directory tables and the PML4 page. CPU B continues executing with a dangling, freed page table root.
- **Expected Invariant**: When an address space is destroyed, remote CPUs whose `CR3` matches the target PML4 must immediately switch their hardware `CR3` to `vmm_get_kernel_pml4()` before acknowledging the shootdown.
- **Proposed Correction**: In `smp_handle_tlb_shootdown()`, if `req_pml4 != 0 && count == 0 && cur_cr3 == req_pml4`, switch to `write_cr3(vmm_get_kernel_pml4())` and atomically clear `(1 << my_cpu)` from `active_cpu_mask`.

---

### Issue 4: Missing Generation Sequence in `g_tlb_req` (Stale / Duplicate Acknowledgements)
- **Shared State**: `smp_tlb_req_t g_tlb_req`.
- **Competing CPU Operations**:
  - CPU A completes shootdown 1, releases `g_tlb_req.lock`.
  - CPU B acquires `g_tlb_req.lock` for shootdown 2.
  - CPU C experiences interrupt latency or is servicing shootdown 1 while shootdown 2 is posted.
- **Race / Invalid State**:
  Without an explicit monotonic generation sequence:
  1. CPU C can read parameters from shootdown 2 while intending to handle shootdown 1.
  2. CPU C can acknowledge shootdown 2 prematurely before invalidating shootdown 2's virtual range.
  3. CPU B can observe a delayed ACK from shootdown 1 and retire shootdown 2 prematurely.
- **Expected Invariant**: Every TLB shootdown request must possess a unique, monotonic 64-bit sequence number (`seq`). Remote CPUs must record their acknowledged sequence number, and the initiator must strictly wait for all target CPUs to reach `ack_seq >= req_seq`.
- **Proposed Correction**:
  - Add `volatile u64 seq` to `g_tlb_req`.
  - Add `volatile u64 acked_seq` to per-CPU state `cpu_state_t`.
  - Remote CPUs invalidate memory and atomically store `cpu->acked_tlb_seq = req_seq`.
  - Initiator spins until `cpu->acked_tlb_seq >= req_seq` for all target CPUs.

---

### Issue 5: Silent Pseudo-Success on TLB Shootdown Timeout
- **Shared State**: `smp_tlb_shootdown()` return status and subsequent `pmm_free_page()` calls.
- **Competing CPU Operations**:
  - CPU A initiates a shootdown. Remote CPU B fails to acknowledge before timeout (due to hardware hang, infinite spin, or interrupt storm).
- **Race / Invalid State**:
  `smp_tlb_shootdown()` had a `void` return type and a 1,000,000-cycle loop. When the loop expired without all ACKs, it silently exited, incremented `g_tlb_shootdown_count`, and returned. The caller immediately freed physical memory frames, exposing un-invalidated stale translations to new memory allocations.
- **Expected Invariant**: A TLB shootdown timeout must never result in silent continuation or physical memory reclamation.
- **Proposed Correction**:
  - `smp_tlb_shootdown()` must return `bool` indicating success or failure.
  - If a required CPU fails to acknowledge within the deadline, log the unacknowledged CPU mask and diagnostic panic or abort memory reclamation.

---

### Issue 6: Dangling Pointer / Use-After-Free of `p->thread` After `sched_reap()`
- **Shared State**: `proc->thread` and `sched_reap()` / `process_reap_zombies()`.
- **Competing CPU Operations**:
  - CPU A runs `sched_reap()`, which unmaps stack pages and calls `kfree(t)`.
  - CPU B runs `process_waitpid()` or `process_reap_zombies()`.
- **Race / Invalid State**:
  In `process_waitpid()` and `process_reap_zombies()`:
  ```c
  if (p->thread && (p->thread->state != THREAD_EXITED || p->thread->is_executing)) {
      continue;
  }
  ```
  `sched_reap()` called `kfree(t)` on the thread without zeroing `proc->thread`. `p->thread` became a dangling pointer. When CPU B accessed `p->thread->state`, it read freed/reallocated heap memory, potentially hanging or crashing.
- **Expected Invariant**: When a thread is reaped, its pointer in its owning process must be atomically set to `NULL` under `g_proc_lock`.
- **Proposed Correction**:
  - In `sched_reap()`, before `kfree(t)`: if `t->process`, acquire `g_proc_lock`, set `t->process->thread = NULL`, and release `g_proc_lock`.
  - In `process_waitpid()` and `process_reap_zombies()`: check `if (p->thread != NULL)` under `g_proc_lock`.

---

### Issue 7: Global Lock Acquisition Hierarchy & Deadlock Inventory
- **Audit Target**: Establish total acquisition order for all kernel synchronization primitives.
- **Locks in Scope**:
  1. `g_astrafs_lock` (`mutex_t`, sleepable, non-IRQ)
  2. `g_proc_lock` (`spinlock_t`, irqsave)
  3. `g_vmm_lock` (`spinlock_t`, irqsave)
  4. `g_pmm_lock` (`spinlock_t`, irqsave, leaf)
  5. `g_heap_lock` (`spinlock_t`, irqsave, leaf)
  6. `g_sched_lock` (`spinlock_t`, irqsave)
  7. `g_sleep_lock` (`spinlock_t`, irqsave)
  8. `g_tlb_req.lock` (`spinlock_t`, irqsave)
  9. `g_ata_lock` (`spinlock_t`, irqsave, leaf)
  10. `g_kprintf_lock` (`spinlock_t`, irqsave, leaf)
- **Required Order**:
  `g_astrafs_lock` $\to$ `g_ata_lock`  
  `g_proc_lock` $\to$ `event_t.lock` / `g_sched_lock`  
  `g_vmm_lock` $\to$ `g_pmm_lock`  
  `g_sleep_lock` / `mutex_t.lock` / `semaphore_t.lock` / `event_t.lock` $\to$ `g_sched_lock`  
  `g_vmm_lock` is released *before* acquiring `g_tlb_req.lock`.  
  No lock may be acquired while holding a leaf lock (`g_pmm_lock`, `g_heap_lock`, `g_ata_lock`, `g_kprintf_lock`).

---

## 3. Plan of Implementation & Qualification Markers

| Task | Scope & Objective | Qualification Marker |
| :--- | :--- | :--- |
| **Task 1** | Order CR3 switch before clearing `active_cpu_mask`; eliminate intra-process transient drops. | `ASTRA_SMP_ASPACE_ACTIVATION_RACE_OK` |
| **Task 2** | Monotonic 64-bit generation sequence (`seq`/`ack_seq`) in `g_tlb_req`; non-silent timeout panic. | `ASTRA_SMP_CONCURRENT_TLB_OK` |
| **Task 3** | Multi-CPU stale mapping verification; remote CPU invalidation before physical frame reuse. | `ASTRA_SMP_STALE_TLB_REUSE_OK` |
| **Task 4** | Zero dangling `proc->thread` pointers in `sched_reap`; safe process teardown under SMP. | `ASTRA_SMP_DEFERRED_RECLAIM_OK` |
| **Task 5** | Complete lock inventory, lock-order graph verification, and deadlock detection. | `ASTRA_SMP_LOCK_ORDER_OK` |
| **Task 6** | `IA32_GS_BASE` per-CPU state audit, nested interrupt safety, and register isolation. | `ASTRA_SMP_PERCPU_ISOLATION_OK` |
| **Task 7** | Concurrent allocator (PMM/heap) and ATA storage fault injection under multi-core load. | `ASTRA_SMP_ALLOCATOR_FAULT_TEST_OK`<br>`ASTRA_SMP_STORAGE_FAULT_TEST_OK` |
| **Task 8** | Deterministic adversarial stress tests across 2, 4, and 8 CPUs with bounded timeouts. | (Adversarial test harness) |
| **Task 9** | Full regression matrix across QEMU 1/2/4/8 CPUs (normal/nofb), host tests, and VirtualBox EFI. | Complete matrix pass |
| **Task 10** | Architecture documentation update (Section 51), status update, and `phase_ao_report.md`. | Final report |

---

## 4. Acceptance Criteria
- All 8 markers emitted and verified.
- Zero compiler warnings or errors (`-Wall -Wextra -Werror`).
- Zero physical memory leaks across repeated creation/destruction cycles.
- Zero data corruption under concurrent storage and allocator stress.
- 100% pass rate across QEMU `-smp 1, 2, 4, 8` (normal and nofb), all 7 host test suites, and VirtualBox 7 EFI.

