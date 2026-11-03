# Phase AP — Independent SMP Qualification, Test Integrity & Kernel Reliability Audit

## 1. Repository Baseline & Environment

- **Current Commit / Revision**: `a133b84cf42cb086689a1a270c44a2ce05a2bc09` (`main`)
- **Working Tree State**: Clean baseline on top of Phase AO changes.
- **Audit Date**: 2026-10-31
- **Subsystem Focus**: Symmetric Multiprocessing (SMP), Address-Space Lifecycle, Cross-CPU TLB Invalidation, Memory Reclamation, Lock Hierarchy, Test Harness Integrity, and Negative Testing.

---

## 2. Independent Classification of Existing Claims

To satisfy Rule 0, all documented claims from Phases AM, AN, and AO are categorized into five rigorous classifications:

| Capability / Subsystem | Claimed Status | Independent Audit Classification | Evidence / Source Location |
| :--- | :--- | :--- | :--- |
| **Limine MP AP Discovery** | APs boot via `goto_address = ap_entry_c` | **Source-code verified & Runtime tested** | `kernel/arch/x86_64/smp.c:283` |
| **Per-CPU GDT / TSS / IST1** | Independent TSS and IST1 per CPU | **Source-code verified & Runtime tested** | `kernel/arch/x86_64/gdt.c`, `smp.c:248` |
| **Local APIC Timer per CPU** | Periodic timer interrupt per CPU | **Source-code verified & Runtime tested** | `kernel/drivers/apic.c:218` |
| **Preemptive Context Switch** | Stack swap with `is_executing` flag | **Source-code verified & Stress tested** | `kernel/sched/switch.S`, `sched.c:873` |
| **Address-Space Activation Order** | `active_cpu_mask` set before CR3, cleared after | **Source-code verified (Defect Found)** | `kernel/sched/sched.c:181-200` — Insufficient alone against concurrent teardown! |
| **Process Teardown Quiescence** | Wait for `active_cpu_mask == 0` | **Source-code verified (Defect Found)** | `kernel/process/process.c:481` — Race window: new CPU can enter while space is dying! |
| **Monotonic TLB Shootdown** | Single-flight request with sequence check | **Source-code verified & Stress tested** | `kernel/arch/x86_64/smp.c:88-169` |
| **Whole-Space Remote CR3 Fallback** | Remote CPU running dying PML4 switches to kernel | **Source-code verified & Runtime tested** | `kernel/arch/x86_64/smp.c:70` |
| **Deferred Reclamation (`sched_reap`)**| Thread stack unmapped and freed safely | **Source-code verified & Stress tested** | `kernel/sched/sched.c:946` |
| **5-Level Acyclic Lock Hierarchy** | L1 $\to$ L2 $\to$ L3 $\to$ L4 $\to$ L5 leaf locks | **Source-code verified** | Inspected across all kernel subsystems |
| **Per-CPU Data via `IA32_GS_BASE`** | Hardware MSR 0xC0000101 self-pointer | **Source-code verified & Runtime tested** | `kernel/arch/x86_64/smp.c:186` |
| **Test Harness Multi-Core Claim** | Single-CPU runs treated cross-CPU as PASS | **Audit Defect Identified** | `tests/test_runner.py:1004` — Must report NOT APPLICABLE on 1-CPU |

---

## 3. Task 1 Audit: Address-Space Activation & Destruction Race Analysis

### 3.1 Source Inspection of Current Context Switch Protocol
In `kernel/sched/sched.c` (`sched_prepare_switch`):
```c
static inline void sched_prepare_switch(thread_t *prev, thread_t *next) {
    u32 cpu_id = g_smp_initialized ? smp_cur_cpu_id() : 0;
    bool diff_proc = (!prev || !prev->process || !next || !next->process || prev->process != next->process);

    /* 1. Mark next process active BEFORE loading CR3 */
    if (diff_proc && next && next->process) {
        __atomic_fetch_or(&next->process->active_cpu_mask, (1U << cpu_id), __ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
...
    /* 2. Switch hardware CR3 */
    phys_addr_t next_cr3 = (next && next->process && next->process->pml4_phys)
                           ? next->process->pml4_phys
                           : vmm_get_kernel_pml4();
    if (next_cr3 != 0 && (read_cr3() & CR3_ADDR_MASK) != (next_cr3 & CR3_ADDR_MASK)) {
        write_cr3(next_cr3);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* 3. Clear prev process active bit AFTER loading new CR3 */
    if (diff_proc && prev && prev->process) {
        __atomic_fetch_and(&prev->process->active_cpu_mask, ~(1U << cpu_id), __ATOMIC_SEQ_CST);
    }
}
```

### 3.2 Discovery of Concrete Teardown vs. Activation Race Condition
Investigating whether setting the incoming bit before `write_cr3()` and clearing outgoing after `write_cr3()` is sufficient reveals a critical race window during concurrent process destruction:

1. **The Interleaving**:
   - Suppose Process $P$ has thread $T_1$ running on CPU B and thread $T_2$ ready to run on CPU A.
   - CPU B invokes `process_exit(proc, 0)`.
   - CPU B sets CR3 to kernel PML4 and clears its bit in `proc->active_cpu_mask`.
   - CPU B checks `while (proc->active_cpu_mask != 0)`. Because CPU A has not scheduled $T_2$ yet, `proc->active_cpu_mask == 0`!
   - CPU B exits the wait loop and proceeds to reclaim physical frames (`pmm_free_page`) and destroy page tables (`vmm_destroy_process_space`).
   - Concurrently, CPU A's scheduler selects thread $T_2$.
   - CPU A calls `sched_prepare_switch(prev, T_2)`.
   - CPU A reads `next->process->pml4_phys` and sets its bit in `proc->active_cpu_mask`.
   - CPU A executes `write_cr3(next_cr3)`: **loading a PML4 that CPU B has already reclaimed or is in the middle of freeing!**
   - Result: CPU A executes with a corrupted or freed page table.
2. **Root Cause**:
   - `proc->state` remains `PROCESS_RUNNING` throughout address space teardown and is only changed to `PROCESS_DEAD` at the very end of `process_exit()`.
   - There is no intermediate `PROCESS_DYING` state.
   - `sched_prepare_switch()` does not inspect whether `next->process` is currently being destroyed or dying.
   - Two CPUs can call `process_exit()` concurrently on the same process, causing a double-free of page tables and user frames.

### 3.3 Target Fix for Task 1
1. Introduce state `PROCESS_DYING = 4` in `kernel/include/kernel/process.h`.
2. In `process_exit()`:
   - Acquire `g_proc_lock`.
   - Check if `proc->state >= PROCESS_DYING`. If so, another core is already executing teardown; release lock and exit thread cleanly.
   - Atomically transition `proc->state = PROCESS_DYING`.
   - Release `g_proc_lock`.
3. In `sched_prepare_switch()`:
   - If `next->process && next->process->state >= PROCESS_DYING`:
     - Do **not** load `next->process->pml4_phys`.
     - Load `vmm_get_kernel_pml4()` instead.
     - Do **not** set `active_cpu_mask` for the dying process.
4. In `process_exit()`:
   - With `PROCESS_DYING` set, no CPU can ever set its bit in `active_cpu_mask` again.
   - Wait for `active_cpu_mask == 0`.
   - Shootdown and destroy address space safely.
   - Transition to `proc->state = PROCESS_DEAD`.

---

## 4. Task 2 Audit: Cross-CPU TLB Shootdown Requests

### 4.1 Monotonic Sequence Tracking Invariants
In `kernel/arch/x86_64/smp.c`:
- Global request structure `g_tlb_req` is protected by `g_tlb_req.lock`.
- `g_tlb_seq` increments monotonically for each new request.
- Every CPU tracks `g_cpus[i].acked_tlb_seq`.
- Delayed or duplicate IPIs are safely identified and discarded via `if (g_cpus[my_cpu].acked_tlb_seq >= req_seq && req_seq != 0) return;`.

### 4.2 Potential Flaw Analysis & Verification
1. **Can request data be overwritten while remote CPUs process it?**
   - **No**: `g_tlb_req.lock` is held continuously from initial parameter setup until all target CPUs have updated `acked_tlb_seq >= cur_seq` and `target_mask` is cleared to 0. A subsequent caller cannot acquire `g_tlb_req.lock` or overwrite parameters until the current generation is completely finished.
2. **Can concurrent shootdown callers deadlock?**
   - **No**: While spinning on `spinlock_try_acquire(&g_tlb_req.lock)`, any core that is targeted by the in-flight request calls `smp_handle_tlb_shootdown()`, processing the pending invalidation with interrupts disabled and breaking circular wait.
3. **Whole-Space Remote Fallback**:
   - Verified that `smp_handle_tlb_shootdown()` checks `count == 0 && cur_cr3 == req_pml4` and immediately invokes `write_cr3(vmm_get_kernel_pml4())` prior to setting its sequence ACK.

---

## 5. Task 3 Audit: Physical Memory Reclamation Proof

### 5.1 Reclamation Sequence Audit
The required sequence is:
$$\text{PTE cleared (unmapped)} \implies \text{TLB Shootdown IPI broadcast} \implies \text{Remote ACKs verified} \implies \text{PMM frame freed}$$

In `kernel/mm/vmm.c` (`vmm_unmap_page`):
```c
int vmm_unmap_page(phys_addr_t pml4_phys, virt_addr_t va) {
    u64 irq_flags = spinlock_acquire_irqsave(&g_vmm_lock);
    int res = vmm_unmap_page_unlocked(pml4_phys, va);
    spinlock_release_irqrestore(&g_vmm_lock, irq_flags);

    if (res == VMM_SUCCESS) {
        smp_tlb_shootdown(pml4_phys, va, 1);
    }
    return res;
}
```
And in caller (e.g. `sched_reap`):
```c
vmm_unmap_page(pml4, va);
pmm_free_page(pa);
```
- The PTE is cleared under `g_vmm_lock`.
- `smp_tlb_shootdown` executes `invlpg` locally and across all online remote CPUs.
- Only after all ACKs arrive does `vmm_unmap_page` return `VMM_SUCCESS`.
- `pmm_free_page` is invoked strictly after invalidation completes.

### 5.2 Deterministic Verification Plan (Task 3)
A new test in `kernel/process/process.c`:
1. Allocate physical frame $F_1$.
2. Map $F_1$ at virtual address $V_{test}$ in kernel/shared space with write permissions.
3. Access and verify $F_1$ from CPU 0 and CPU 1.
4. Unmap $V_{test}$ on CPU 0 $\to$ triggers `smp_tlb_shootdown`.
5. Verify remote CPU 1 ACK.
6. Free $F_1$ and reallocate it for a new owner with a distinct signature.
7. Verify that the unmapped address $V_{test}$ can no longer access $F_1$ (#PF verified).

---

## 6. Task 4 Audit: Lock Ordering Hierarchy & Call Graph

### 6.1 Lock Order Graph
The documented 5-level hierarchy is:
$$\text{L1: } g\_vfs\_lock \prec \text{L2: } g\_fs\_lock \prec \text{L3: } g\_proc\_lock \prec \text{L4: } g\_vmm\_lock \prec \text{L5: Leaf Locks}$$
Leaf locks: `g_pmm_lock`, `g_sched_lock`, `g_sleep_lock`, `g_ata_lock`, `g_heap_lock`, `g_tlb_req.lock`.

### 6.2 Call Graph Inspection Results
1. `g_vfs_lock`: Held during path operations and mounts. Calls into filesystem drivers (`astrafs_write`) which acquire `g_astrafs_lock` (L1 $\to$ L2). Never acquires process or scheduler locks.
2. `g_fs_lock`: Held during AstraFS commits and extent modifications. Calls block layer `bdev->ops->write` which acquires `g_ata_lock` (L2 $\to$ L5). Never acquires VFS lock.
3. `g_proc_lock`: Held during process table updates, waitpid, PID allocation, thread dislinking. In Phase AO Test 5, verified L3 $\to$ L5 (`g_proc_lock` $\to$ `g_sched_lock`). Never held when acquiring L1 or L2.
4. `g_vmm_lock`: Held during page table tree walks and mapping insertions. In `vmm_map_page`, if a table page is required, allocates via `pmm_alloc_page()`, acquiring `g_pmm_lock` (L4 $\to$ L5). Never held when acquiring L1, L2, or L3.
5. **Leaf Locks (Level 5)**:
   - `g_pmm_lock`: Protects bitmap allocations. Pure leaf.
   - `g_sched_lock`: Protects ready queue and thread state transitions. Acquired with IF=0. Pure leaf.
   - `g_sleep_lock`: Protects sleeper delta queue. Pure leaf.
   - `g_ata_lock`: Protects ATA I/O ports. Pure leaf.
   - `g_tlb_req.lock`: Protects TLB shootdown request. Pure leaf.

**Conclusion**: The hierarchy is strictly acyclic. No inversions exist in production code paths.

---

## 7. Task 5 Audit: Test Harness Integrity

### 7.1 Identified Test Harness Flaws
1. **Single-CPU Cross-CPU False PASS**:
   - In `tests/test_runner.py:1004`:
     ```python
     if "ASTRA_SMP_PARALLEL_EXECUTION_OK" in line or "ASTRA_SMP_SINGLE_CPU_OK" in line:
         am_parallel_ok = True
     ```
   - In a 1-CPU run, the kernel emits `ASTRA_SMP_SINGLE_CPU_OK` and the test runner reports cross-CPU parallel execution as PASS!
   - **Correction**: On `--smp 1`, cross-CPU tests must be explicitly reported as `NOT APPLICABLE`, not `PASS`. Genuine SMP parallel execution requires `--smp >= 2`.
2. **Missing Marker Detection**:
   - `test_runner.py` verifies all required markers in `phase_ao_conditions` and boot prerequisites.
   - However, timeout handling must record the exact QEMU command line and distinguish clean shutdown from success.

---

## 8. Task 7 Audit: Negative Testing Plan

Eight deterministic negative fault-injection scenarios will be implemented and verified:

1. **Missing TLB ACK**: Invalidate with remote core forced not to ACK; verify `smp_tlb_shootdown` timeout detection.
2. **Delayed TLB ACK**: Inject 500-cycle delay on remote core ACK; verify initiator waits and completes correctly.
3. **Duplicate TLB ACK**: Remote core sends redundant ACK; verify monotonic sequence engine ignores duplicate.
4. **Invalid Target CPU**: Target mask set with offline CPU bit; verify safety and bounded timeout.
5. **Shootdown Timeout Diagnostic**: Injected indefinite stall; verify exact timeout panic diagnostic emission.
6. **Forced Allocation Failure**: Force `pmm_alloc_page()` failure; verify graceful `PMM_ERR_OUT_OF_MEM` return and caller rollback.
7. **Simulated Concurrent Teardown**: Two threads invoke `process_exit()` simultaneously; verify `PROCESS_DYING` single-winner execution and zero double-free.
8. **Lock Contention Under Interrupt**: Stress spinlock under high interrupt frequency; verify zero priority inversion or deadlock.

---

## 9. Next Steps
1. Implement Task 1 `PROCESS_DYING` atomic transition and `sched_prepare_switch()` activation defense.
2. Implement Task 3 deterministic physical frame reclamation test.
3. Implement Task 5 `test_runner.py` harness corrections (`--smp 1` reports cross-CPU tests as `NOT APPLICABLE`).
4. Implement Task 7 negative fault injection tests in `kernel/process/process.c`.
5. Run full qualification matrix across QEMU 1, 2, 4, 8 CPUs, nofb mode, 7 host test suites, and VirtualBox EFI.
6. Compile final `phase_ap_report.md`.

