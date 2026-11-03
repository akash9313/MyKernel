# Phase AQ — Adversarial SMP Concurrency, TLB Shootdown & Memory Reclamation Audit

## 1. Executive Summary & Repository Baseline

- **Current Commit / Revision**: `a133b84cf42cb086689a1a270c44a2ce05a2bc09`
- **Subsystem Focus**: Adversarial Concurrency, Cross-CPU TLB Shootdown Protocol, Physical Frame Reclamation Ordering, Address-Space Teardown vs. Activation Synchronization, Kernel Lock Graph Invariants, and Test Harness Qualification.
- **Audit Date**: 2026-10-31

Phase AP successfully established single-core vs. multi-core test distinctions and introduced the `PROCESS_DYING` state to prevent address-space teardown collisions. However, an adversarial concurrency audit reveals critical edge cases and protocol vulnerabilities that must be hardened and tested under multi-core load:
1. **TLB Shootdown Publication Race**: In `smp_tlb_shootdown()`, `g_tlb_req.seq = cur_seq` was assigned before virtual address and page count parameters, and `smp_handle_tlb_shootdown()` did not check `(g_tlb_req.target_mask & (1U << my_cpu))`. A delayed IPI on a remote core could read uninitialized or stale parameters and advance `acked_tlb_seq` prematurely.
2. **Ignored Shootdown Return Value in VMM**: `vmm_unmap_page()` discarded the boolean return of `smp_tlb_shootdown()`, unconditionally returning `VMM_SUCCESS`. If a shootdown timed out or failed, callers would proceed to free physical memory, violating the fundamental guarantee that frames are never reclaimed while stale TLB mappings remain.
3. **Double-Check Defense in Address-Space Switch**: If a thread was selected right as a process began exiting, re-checking `proc->state < PROCESS_DYING` after setting `active_cpu_mask` eliminates transient activation of dying page tables.
4. **Target Mask Normalization**: Out-of-bounds CPU bits or offline cores in shootdown masks must be filtered against `online_mask` to prevent false timeouts.

---

## 2. In-Depth Subsystem Call Path Tracing

### 2.1 Process Exit & Teardown Path
```
process_exit(proc, exit_code)
  |-- acquire g_proc_lock
  |-- check proc->state >= PROCESS_DYING -> if so, release lock, preempt_enable(), thread_exit() [SINGLE WINNER]
  |-- proc->state = PROCESS_DYING
  |-- release g_proc_lock
  |-- proc->active_cpu_mask &= ~(1ULL << cur_cpu)
  |-- write_cr3(vmm_get_kernel_pml4())
  |-- wait loop: while (proc->active_cpu_mask != 0) { sti(); pause(); }
  |-- vmm_destroy_process_space(proc->pml4_phys)
        |-- smp_tlb_shootdown(pml4_phys, 0, 0) [WHOLE-SPACE BROADCAST]
        |-- vmm_destroy_space(pml4_phys) [RECLAIM INTERMEDIATE TABLES]
  |-- reparent children to init under g_proc_lock
  |-- proc->state = PROCESS_DEAD
  |-- preempt_enable(); thread_exit();
```

### 2.2 Context Switch & Address-Space Selection Path
```
sched_yield() / timer interrupt
  |-- acquire g_sched_lock (IF=0)
  |-- select next runnable thread
  |-- release g_sched_lock
  |-- sched_prepare_switch(prev, next)
        |-- if diff_proc && next->process && next->process->state < PROCESS_DYING:
              __atomic_fetch_or(&next->process->active_cpu_mask, (1U << cpu_id))
              re-check next->process->state >= PROCESS_DYING:
                if dying: clear bit, target_cr3 = kernel_pml4
                else: target_cr3 = next->process->pml4_phys
        |-- write_cr3(target_cr3)
        |-- if diff_proc && prev->process:
              __atomic_fetch_and(&prev->process->active_cpu_mask, ~(1U << cpu_id))
  |-- thread_switch_asm(&prev->saved_rsp, next->saved_rsp, next, &prev->is_executing)
```

### 2.3 Page Unmapping, Shootdown & Frame Reclamation Path
```
vmm_unmap_page(pml4_phys, va)
  |-- acquire g_vmm_lock (IF=0)
  |-- clear PTE (PTE_PRESENT = 0)
  |-- release g_vmm_lock
  |-- smp_tlb_shootdown(pml4_phys, va, 1)
        |-- if single-CPU or smp not init: local invlpg; return true;
        |-- acquire g_tlb_req.lock (IF=0; service pending shootdowns while spinning)
        |-- write parameters: pml4_phys, va, count, ack_mask = 0
        |-- seq = ++g_tlb_seq; fence;
        |-- target_mask = target_mask; fence;
        |-- apic_send_ipi_all_ex_self(APIC_TLB_IPI_VECTOR)
        |-- wait for acked_tlb_seq >= cur_seq on all targets
        |-- clear target_mask = 0; release g_tlb_req.lock;
  |-- if !shootdown_ok: return VMM_ERR_SHOOTDOWN_FAILED;
  |-- return VMM_SUCCESS;
Caller (e.g. sched_reap / memory teardown):
  |-- if vmm_unmap_page == VMM_SUCCESS:
        pmm_free_page(pa)
```

---

## 3. Identified Concurrency Vulnerabilities & Fix Plan

### Vulnerability 1: Parameter Publication vs. ACK Race in Shootdown
- **Observation**: In `smp_tlb_shootdown`, `g_tlb_req.seq = cur_seq` was written before `pml4_phys`, `va`, and `page_count`. If a remote CPU took an interrupt or spin-service between line 134 and line 137, it saw `acked_tlb_seq < cur_seq`, processed with incomplete/stale parameters, and acknowledged.
- **Fix**:
  1. Set `g_tlb_req.pml4_phys`, `va`, `page_count`, `ack_mask` first.
  2. Set `g_tlb_req.seq = cur_seq` with `__atomic_thread_fence(__ATOMIC_SEQ_CST)`.
  3. Set `g_tlb_req.target_mask = target_mask` with `__atomic_thread_fence(__ATOMIC_SEQ_CST)`.
  4. In `smp_handle_tlb_shootdown()`, immediately return if `(g_tlb_req.target_mask & (1U << my_cpu)) == 0`.

### Vulnerability 2: Unchecked Shootdown Return in `vmm_unmap_page`
- **Observation**: `vmm_unmap_page` discarded `smp_tlb_shootdown`'s return value. If shootdown timed out, `vmm_unmap_page` returned 0, allowing the caller to free physical frames while other CPUs still held valid TLB mappings.
- **Fix**:
  1. Define `VMM_ERR_SHOOTDOWN_FAILED (-9)` in `kernel/include/kernel/vmm.h`.
  2. In `vmm_unmap_page`, check:
     ```c
     if (res == VMM_SUCCESS) {
         if (!smp_tlb_shootdown(pml4_phys, va, 1)) {
             return VMM_ERR_SHOOTDOWN_FAILED;
         }
     }
     ```
  3. Frame reclamation only proceeds if `res == VMM_SUCCESS`.

### Vulnerability 3: Address-Space Transient Activation Guard
- **Observation**: In `sched_prepare_switch()`, if `next->process->state < PROCESS_DYING` is read before a concurrent `process_exit()` sets `PROCESS_DYING`, `active_cpu_mask` could be set and `CR3` loaded just as the process begins teardown.
- **Fix**:
  Re-check `next->process->state >= PROCESS_DYING` immediately after setting `active_cpu_mask`. If the process transitioned to dying, back out: clear the bit and load `vmm_get_kernel_pml4()`.

### Vulnerability 4: Target Mask Normalization & Offline CPU Defense
- **Observation**: Out-of-bounds target masks or targeting offline CPUs could cause infinite wait loops or spurious panics.
- **Fix**:
  In `smp_tlb_shootdown`, compute `online_mask` and filter: `target_mask &= online_mask; target_mask &= ~(1U << cur_cpu);`. If 0, bypass IPI broadcast.

---

## 4. Test Harness Qualification Invariants

### 4.1 CPU Count Classification Table
| Configuration | Expected Online CPUs | Qualification Classification |
|---|---|---|
| QEMU 1 CPU | 1 | Single-core regression; cross-CPU parallel & IPI tests marked **NOT APPLICABLE** |
| QEMU 2 CPUs | 2 | Minimum cross-CPU IPI, dual-core concurrent Ring 3, shootdown qualification |
| QEMU 4 CPUs | 4 | Multi-core concurrency, scheduler stress, race interleaving qualification |
| QEMU 8 CPUs | 8 | Higher-contention stress, 8-core scheduler exclusivity qualification |
| Oracle VirtualBox | 1 | Single-core EFI qualification; report actual online CPU count (1 vCPU) |

### 4.2 Test Runner Integrity Checks
- Parse `[SMP] AP initialization complete. X/Y CPUs online.` to verify actual online count against requested count.
- Prevent stale logs by truncating log buffers at run start.
- Fail explicitly on kernel panics, hangs, or missing markers.

---

## 5. Phase AQ Proposed Test Markers

1. `ASTRA_AQ_CONCURRENT_SHOOTDOWN_OK`: Simultaneous multi-core shootdowns with monotonic sequence validation.
2. `ASTRA_AQ_TLB_FAULT_INJECTION_OK`: Fault injection tests (delayed ACK, duplicate ACK, missing ACK, invalid CPU target).
3. `ASTRA_AQ_FRAME_REUSE_ORDERING_OK`: Frame reclamation order (`Unmap -> Shootdown -> Required ACKs -> Free -> Reuse`) with prevention of premature reuse.
4. `ASTRA_AQ_ASPACE_ADVERSARIAL_OK`: Adversarial interleaving of process selection, exit, and CR3 switching.
5. `ASTRA_AQ_LOCK_DEADLOCK_AUDIT_OK`: Dynamic lock hierarchy validation and deadlock absence under high concurrency.
6. `ASTRA_AQ_HARNESS_CLASSIFICATION_OK`: Test harness CPU classification and online count integrity.

---

## 6. Audit Acceptance & Next Steps

Audit complete. Proceeding with targeted synchronization fixes and implementing adversarial Phase AQ verification tests.

