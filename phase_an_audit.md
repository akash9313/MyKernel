# Phase AN — AstraOS SMP Memory Safety, TLB Shootdown & Concurrency Hardening: Architectural Audit

**Workspace:** `C:\Users\Akash\Documents\MyKernel`  
**Target Architecture:** x86-64 (UEFI / Limine Bootloader)  
**Date:** October 31, 2026  
**Status:** **AUDIT COMPLETE — CRITICAL CONCURRENCY AND TLB DEFECTS IDENTIFIED**

---

## 1. Executive Summary & Audit Scope

Phase AM established the foundation for Symmetric Multiprocessing (SMP) in AstraOS by discovering Application Processors (APs) via the Limine MP protocol, staging per-CPU GDT/TSS descriptors with isolated IST1 double fault stacks, starting Local APIC periodic timers, and adding coarse IRQ-safe spinlocks to the PMM, Heap, Process Table, and Scheduler queues.

However, an exhaustive, independent audit reveals that **cross-CPU memory safety, page table coherence, and driver concurrency are fundamentally incomplete under genuine multi-core execution**:
1. **Zero TLB Shootdown Capability**: There is no cross-CPU TLB invalidation mechanism. When pages are unmapped in `vmm_unmap_page()` or `sys_munmap()`, or when an address space is dismantled during `process_exit()`, only the local CPU's TLB or CR3 is invalidated. Remote CPUs continue to hold stale translations to freed physical frames.
2. **Missing VMM Page-Table Locking**: `kernel/mm/vmm.c` contains zero synchronization primitives. Concurrent mappings, unmappings, and address space creations from multiple cores race on PML4, PDPT, PD, and PT entries, leaking physical frames and risking page-table corruption.
3. **Embryonic Process Table Slot Clashing**: `process_create()` and `process_create_elf()` clear the process slot with `kmemset` outside `g_proc_lock`, resetting `proc->state` to `PROCESS_UNUSED` while address spaces are constructed, allowing concurrent calls to allocate and clobber the same slot.
4. **Unsynchronized ATA Block Driver I/O**: `kernel/drivers/ata.c` and `kernel/drivers/block.c` possess no locking. Concurrent I/O requests from multiple cores interleave `inb`/`outb`/`insw`/`outsw` instructions to the shared ATA legacy PIO ports (0x1F0..0x1F7), corrupting hardware status and sector data.
5. **Missing Local APIC IPI Infrastructure**: `kernel/drivers/apic.c` lacks Interrupt Command Register (ICR) programming (`APIC_REG_ICR_LOW` 0x300, `APIC_REG_ICR_HIGH` 0x310), making inter-processor interrupts impossible.

---

## 2. Detailed Findings by Subsystem

### Subsystem 1: Page Tables & TLB Coherence (Task 1)
- **Local Invalidation Only**:
  - `vmm_unmap_page()` does not execute `invlpg` locally and performs no cross-CPU invalidation.
  - `sys_munmap()` executes `invlpg(pva)` on the calling core only, then immediately invokes `pmm_free_page(info.paddr)`. If an AP previously executed in this address space, its hardware TLB continues to map `pva` to the freed physical frame.
  - `vmm_destroy_process_space()` reloads CR3 to `vmm_get_kernel_pml4()` on the local CPU only, leaving remote APs running with stale CR3 or cached translations.
- **Missing Architecture**:
  - No active CPU mask tracking which cores are currently running an address space (`active_cpu_mask`).
  - No TLB shootdown request descriptor or synchronization handshake.
  - No IPI handler for TLB shootdown.

### Subsystem 2: Process Lifecycle & Address-Space Teardown (Task 2)
- **Premature Physical Frame Reclamation**:
  - In `process_exit()`, `write_cr3(vmm_get_kernel_pml4())` is executed on the local core, and all physical frames in `proc->mapped_pas` are immediately freed to PMM.
  - If another CPU is executing a thread within the same process, or if an AP is in the middle of a context switch into that process, the address space is destroyed while still active on the remote core.
- **Process Slot Allocation Race**:
  - In `process_create()`:
    ```c
    proc->state = PROCESS_READY;
    spinlock_release_irqrestore(&g_proc_lock, proc_flags);
    kmemset(proc, 0, sizeof(process_t)); // Resets state to 0 (PROCESS_UNUSED)!
    ```
    Between `kmemset` and the subsequent `proc->state = PROCESS_READY`, the slot appears unused to concurrent callers on other cores.

### Subsystem 3: Allocator & VMM Concurrency (Task 3)
- **VMM State Corruption**:
  - `vmm_map_page()` allocates new intermediate page-table levels (PDPT, PD, PT) via `pmm_alloc_page()`. Without a lock, two cores mapping different virtual addresses under the same intermediate entry will simultaneously allocate distinct pages, and the second core will overwrite the first core's pointer, permanently leaking the first allocated page and dropping its mappings.
  - Intermediate table deallocation in `vmm_unmap_page()` (`pt_empty`, `pd_empty`, `pdpt_empty`) can free a table while another core is concurrently inserting a mapping into it.
- **PMM & Heap**:
  - PMM (`g_pmm_lock`) and Heap (`g_heap_lock`) are protected by spinlocks, but require comprehensive stress testing under concurrent multi-core pressure (1, 2, 4, 8 CPUs) to verify zero frame leakage, deadlock freedom, and invariant preservation.

### Subsystem 4: Scheduler & Execution Exclusivity (Task 4)
- **Thread Migration & Execution Exclusivity**:
  - Phase AM introduced `is_executing` CAS guards, but the interaction with `g_cpus[cpu_id].current_thread` and `smp_cur_cpu_id()` during AP preemption requires strict synchronization barriers to prevent subtle race windows where `thread_current()` momentarily observes a stale thread pointer.
  - Scheduler ready queue operations (`sched_enqueue_locked`, `sched_dequeue_locked`) and sleeper queue operations must maintain strict lock ordering: `g_sched_lock` vs `g_sleep_lock` vs subsystem locks.

### Subsystem 5: APIC, Interrupts & IPIs (Task 5)
- **Local APIC ICR Programming**:
  - Register definitions for ICR Low (0x0300) and ICR High (0x0310) are missing in `kernel/drivers/apic.c`.
  - Sending fixed IPIs requires waiting for the delivery status bit (bit 12) to clear, writing the destination APIC ID to ICR High (bits 24..31), and writing the vector with delivery mode to ICR Low.
  - IDT must register a dedicated interrupt gate for TLB shootdown (vector 251 / `0xFB`), and `interrupt_handler` must acknowledge the interrupt via `apic_eoi()` without recursion or deadlocks.

### Subsystem 6: Filesystem & ATA Driver Concurrency (Task 6)
- **Hardware I/O Port Contention**:
  - Legacy ATA PIO uses shared port ranges: Primary channel at 0x1F0..0x1F7, Control at 0x3F6.
  - Without driver-level serialization, concurrent disk reads, writes, or flushes from separate CPUs interleave commands, causing timeouts, data corruption, or drive faults.
  - An IRQ-safe spinlock (`g_ata_lock`) or block-layer mutex must serialize all command sequences, sector data transfers, and cache flushes.

---

## 3. Actionable Hardening Plan

```
+---------------------------------------------------------------------------------------+
|                                    Phase AN Hardening Plan                            |
+---------------------------------------------------------------------------------------+
|  1. Local APIC IPI & TLB Shootdown Infrastructure                                     |
|     - Add ICR register offsets & apic_send_ipi() / apic_send_ipi_all_ex_self()       |
|     - Assign IDT Vector 251 for TLB shootdown IPI (irq_stub_251)                       |
|     - Implement smp_tlb_shootdown(pml4, va, count) with atomic ack mask & timeout     |
|     - Update vmm_unmap_page, sys_munmap, vmm_destroy_process_space                    |
|     - Test: Invalidate page on CPU A and verify CPU B traps #PF (no stale TLB)        |
|     - Marker: ASTRA_SMP_TLB_SHOOTDOWN_OK                                              |
|                                                                                       |
|  2. Process Lifecycle & Teardown Hardening                                            |
|     - Add active_cpu_mask to process_t (set on sched switch, cleared on exit)         |
|     - Ensure process_exit waits for active_cpu_mask == 0 before freeing frames        |
|     - Initialize process slots in embryonic state while holding g_proc_lock           |
|     - Marker: ASTRA_SMP_PROCESS_TEARDOWN_OK                                           |
|                                                                                       |
|  3. VMM & Memory Allocator Concurrency                                                |
|     - Add g_vmm_lock (spinlock_t) to serialize all page table structure changes        |
|     - Implement concurrent multi-core stress test (alloc/free across 8 CPUs)          |
|     - Record initial, peak, and final free frames (verify 0 leaks)                     |
|     - Marker: ASTRA_SMP_MEMORY_STRESS_OK                                              |
|                                                                                       |
|  4. Scheduler & Synchronization Qualification                                         |
|     - Stress ready queue, sleeper queue, and thread migration under 8 CPUs            |
|     - Verify is_executing exclusivity and lock order fairness                         |
|     - Marker: ASTRA_SMP_SCHEDULER_STRESS_OK                                           |
|                                                                                       |
|  5. ATA Block Driver Serialization                                                    |
|     - Add g_ata_lock (irqsave spinlock) protecting all ATA port access & flushes      |
|     - Test concurrent disk writes/reads across multiple CPUs                          |
|     - Marker: ASTRA_SMP_STORAGE_STRESS_OK                                             |
|                                                                                       |
|  6. Matrix Verification & Qualification                                               |
|     - Full QEMU Matrix: 1, 2, 4, 8 CPUs in Normal and nofb modes                     |
|     - All 7 host unit test suites                                                     |
|     - VirtualBox EFI execution                                                        |
+---------------------------------------------------------------------------------------+
```

---

## 4. Verification Checkpoint

This audit document fulfills **RULE 0 — Independent Audit First**.  
Implementation of Tasks 1 through 7 will proceed according to the findings and architectural invariants identified above.

