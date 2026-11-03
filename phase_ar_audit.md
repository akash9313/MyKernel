# Phase AR — Independent TLB Protocol Audit, Request Lifetime Safety & SMP Proof Audit

## 1. Baseline Audit & Repository State

- **Current HEAD Commit**: `a133b84cf42cb086689a1a270c44a2ce05a2bc09`
- **Workspace**: `C:\Users\Akash\Documents\MyKernel`
- **Audit Target**: Phase AQ cross-CPU TLB shootdown engine, request publication ordering, delayed IPI lifetime safety, timeout semantics, and frame reclamation verification.
- **Rule 0 Compliance**: All uncommitted changes from prior phases (Phases A through AQ) are preserved.

---

## 2. Source-Level Inspection of Current Implementations

### 2.1 Current `smp_tlb_shootdown()` (`kernel/arch/x86_64/smp.c:122-213`)
```c
bool smp_tlb_shootdown(phys_addr_t pml4_phys, virt_addr_t va, size_t page_count) {
    /* 1. Local invalidation on calling core */
    if (page_count == 0) write_cr3(read_cr3());
    else { for (size_t i = 0; i < page_count; i++) invlpg(va + i * 4096); }

    if (!g_smp_initialized || g_cpu_count <= 1) return true;

    /* 2. Compute target mask */
    ...
    /* 3. Acquire g_tlb_req.lock while servicing pending shootdowns */
    while (!spinlock_try_acquire(&g_tlb_req.lock)) {
        if ((__atomic_load_n(&g_tlb_req.target_mask, __ATOMIC_SEQ_CST) & (1U << cur_cpu)) &&
            g_cpus[cur_cpu].acked_tlb_seq < __atomic_load_n(&g_tlb_req.seq, __ATOMIC_SEQ_CST)) {
            smp_handle_tlb_shootdown();
        }
        cpu_pause();
    }

    /* 4. Publish parameters into single global g_tlb_req */
    u64 cur_seq = ++g_tlb_seq;
    g_tlb_req.pml4_phys = pml4_phys;
    g_tlb_req.va = va;
    g_tlb_req.page_count = page_count;
    g_tlb_req.ack_mask = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_tlb_req.seq = cur_seq;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_tlb_req.target_mask = target_mask;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* 5. Broadcast IPI 251 */
    apic_send_ipi_all_ex_self(APIC_TLB_IPI_VECTOR);

    /* 6. Wait for all target CPUs to advance acked_tlb_seq >= cur_seq */
    ...
}
```

### 2.2 Current `smp_handle_tlb_shootdown()` (`kernel/arch/x86_64/smp.c:69-120`)
```c
void smp_handle_tlb_shootdown(void) {
    u32 my_cpu = smp_cur_cpu_id();
    g_cpus[my_cpu].tlb_ipi_count++;

    u32 target = __atomic_load_n(&g_tlb_req.target_mask, __ATOMIC_SEQ_CST);
    if ((target & (1U << my_cpu)) == 0) return;

    u64 req_seq = __atomic_load_n(&g_tlb_req.seq, __ATOMIC_SEQ_CST);
    if (g_cpus[my_cpu].acked_tlb_seq >= req_seq && req_seq != 0) return;

    /* Invalidate based on g_tlb_req parameters */
    ...
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&g_cpus[my_cpu].acked_tlb_seq, req_seq, __ATOMIC_SEQ_CST);
    __atomic_fetch_or(&g_tlb_req.ack_mask, (1U << my_cpu), __ATOMIC_SEQ_CST);
}
```

---

## 3. Critical Audit — Concurrent Request Publication (Section 2 Questions)

1. **Can two CPUs call `smp_tlb_shootdown()` simultaneously?**
   - Yes. Multiple threads on different cores can invoke `smp_tlb_shootdown()`.
2. **Do they write to the same global request descriptors?**
   - Yes, all CPUs write to the single global `g_tlb_req`.
3. **Can one sender overwrite another sender's address, page range, sequence, or target mask?**
   - While `g_tlb_req.lock` is held, concurrent writers are serialized. However, on timeout or completion, `g_tlb_req.lock` is released immediately. If sender A times out, releases the lock, and sender B acquires the lock, sender B overwrites all fields of `g_tlb_req` while a delayed receiver for sender A is still in flight.
4. **Is there a request ownership mechanism?**
   - No. There is only a single global descriptor owned solely by whoever currently holds `g_tlb_req.lock`.
5. **Is there a lock, atomic state machine, per-CPU request slot, or queue?**
   - Only a single spinlock `g_tlb_req.lock`. There are no per-CPU request slots, no request queue, and no generation state machine.
6. **Can a receiver observe a mixed request containing fields from different senders?**
   - Yes. If sender A times out, releases the lock, and sender B immediately begins writing `g_tlb_req`, a delayed receiver servicing the old IPI from sender A can read `g_tlb_req` concurrently while sender B is writing fields, observing mixed parameters.
7. **Can two senders wait on the same acknowledgement state?**
   - Senders serialize on `g_tlb_req.lock`, so two senders do not wait simultaneously on the current lock. However, if sender A times out and exits, and sender B starts, delayed ACKs intended for sender A can write into sender B's `ack_mask`.
8. **Does the implementation correctly handle overlapping target masks?**
   - Within a single lock acquisition, target mask is private to that request. But across consecutive requests, overlapping masks allow stale IPIs to acknowledge unrelated requests.

---

## 4. Request Lifetime and Delayed IPI Safety (Section 3 Questions)

### The Stale IPI / Shared Storage Race:
1. Sender CPU 0 publishes Request A (`seq = 1`, `va = 0x1000`, `targets = {CPU 1}`).
2. Receiver CPU 1 is delayed (e.g., interrupts masked, hypervisor pause, or fault injection).
3. Sender CPU 0 times out after 5,000,000 iterations. It sets `target_mask = 0`, releases `g_tlb_req.lock`, and returns `false`.
4. Sender CPU 2 acquires `g_tlb_req.lock` and publishes Request B (`seq = 2`, `va = 0x5000`, `targets = {CPU 0, CPU 1}`).
5. Receiver CPU 1 finally unmasks interrupts and processes the queued IPI for Request A.
6. In `smp_handle_tlb_shootdown()`, CPU 1 reads `g_tlb_req.target_mask` (which now has bit 1 from Request B) and `g_tlb_req.seq` (which is now 2).
7. CPU 1 compares its `acked_tlb_seq` (0) against `req_seq` (2). Because `0 < 2`, CPU 1 invalidates `0x5000` (Request B's address) and ACKs `seq = 2`.
8. **Result**:
   - Request A's address `0x1000` was **NEVER** invalidated on CPU 1.
   - Request B's `ack_mask` was updated by an IPI belonging to Request A.
   - If Request B did not target CPU 1, CPU 1 would have skipped it entirely, leaving `0x1000` stale forever in CPU 1's TLB.
   - Global monotonic counters do **NOT** prevent this lifetime violation.

---

## 5. Timeout, Failure Semantics & Frame Reclamation Audit (Sections 4 & 5)

### 5.1 Audit of Callers of `vmm_unmap_page()`
We audited every caller of `vmm_unmap_page()` across the codebase:
1. `kernel/process/syscall.c` (`sys_munmap`, lines 710-713):
   - **Defect**: The return value of `vmm_unmap_page()` is ignored. Even if shootdown returns `VMM_ERR_SHOOTDOWN_FAILED`, it immediately calls `pmm_free_page(info.paddr)`.
2. `kernel/process/syscall.c` (`sys_mmap` rollback, lines 656-658 and 672-674):
   - **Defect**: On allocation failure during multi-page mmap, rollback unmaps prior pages but ignores the return value of `vmm_unmap_page()`, immediately calling `pmm_free_page()`.
3. `kernel/sched/sched.c` (`sched_reap`, lines 996-1004):
   - **Defect**: Unmaps stack pages of reaped threads but ignores the return value of `vmm_unmap_page()`, proceeding directly to `pmm_free_page(t->stack_pas[p])`.
4. `kernel/sched/sched.c` (kthread stack allocation rollback, lines 1152 and 1168):
   - **Defect**: Ignores `vmm_unmap_page()` return value and frees frames immediately.
5. `kernel/process/process.c` (`process_exit`, lines 498-508):
   - **Defect**: Calls `smp_tlb_shootdown(proc->pml4_phys, 0, 0)` but ignores the boolean return value. Even on timeout, it proceeds to free all mapped physical frames with `pmm_free_page()` and destroys page tables with `vmm_destroy_process_space()`.

---

## 6. Architecture for Phase AR: Generation-Aware Per-CPU Request Queues

To provide an incontrovertible SMP proof and eliminate all request lifetime and publication races, we implement:

### 6.1 Independent Per-CPU Request Queues
```c
#define SMP_TLB_SLOTS_PER_CPU 4

typedef enum {
    TLB_SLOT_FREE = 0,
    TLB_SLOT_ACTIVE = 1,
    TLB_SLOT_TIMED_OUT = 2,
    TLB_SLOT_RETIRED = 3
} tlb_slot_state_t;

typedef struct {
    volatile u64 gen;                 /* Monotonic request generation ID */
    u32 sender_cpu;                   /* Initiator CPU */
    phys_addr_t pml4_phys;            /* Target PML4 (or 0 for kernel/all) */
    virt_addr_t va;                   /* Target VA */
    size_t page_count;                /* Page count (0 = whole space) */
    volatile u32 target_mask;         /* Bitmask of target CPUs */
    volatile u32 ack_mask;            /* Bitmask of completed ACK CPUs */
    volatile tlb_slot_state_t state;  /* Current slot state */
} smp_tlb_slot_t;

typedef struct {
    spinlock_t lock;                  /* Serializes sender allocations on this CPU */
    u32 head;                         /* Next slot index to allocate */
    smp_tlb_slot_t slots[SMP_TLB_SLOTS_PER_CPU];
} smp_cpu_tlb_queue_t;

extern smp_cpu_tlb_queue_t g_smp_tlb_queues[MAX_CPUS];
```

### 6.2 Key Architectural Invariants
1. **Concurrent Initiator Independence**: CPU $i$ writes only to `g_smp_tlb_queues[i]`. Two CPUs never write to the same request descriptor.
2. **Generational Request Identity**: Every request receives a globally unique monotonic `gen`. Receivers check generation equality, preventing stale ACKs.
3. **Retain-Until-Drained**: Timed-out slots transition to `TLB_SLOT_TIMED_OUT` and are NOT overwritten until the delayed receiver drains and ACKs them.
4. **Mandatory Frame Retention on Shootdown Failure**: Every caller of `vmm_unmap_page()` and `smp_tlb_shootdown()` checks the return code. If shootdown fails/times out, the physical frame is **NEVER** returned to the PMM free pool, completely eliminating stale TLB translation use-after-free.

---

## 7. Deterministic Phase AR In-Kernel Test Markers

1. `ASTRA_AR_CONCURRENT_INITIATORS_OK`: Independent concurrent shootdown initiators execute without lock contention or parameter corruption.
2. `ASTRA_AR_DELAYED_IPI_LIFETIME_OK`: Delayed IPI arriving after newer request publication invalidates the correct address and does not ACK the newer request.
3. `ASTRA_AR_STALE_ACK_REJECTION_OK`: Stale or duplicate ACKs from prior generations are safely rejected without corrupting active requests.
4. `ASTRA_AR_TIMEOUT_FRAME_RETENTION_OK`: Timed-out requests strictly retain physical frames and refuse reclamation into PMM.
5. `ASTRA_AR_UNMAP_CALLER_SAFETY_OK`: All callers of `vmm_unmap_page()` handle `VMM_ERR_SHOOTDOWN_FAILED` and refuse to free unacknowledged frames.
6. `ASTRA_AR_END_TO_END_PROOF_OK`: Complete end-to-end multi-core verification across QEMU (1, 2, 4, 8 cores) and VirtualBox 7 EFI.

