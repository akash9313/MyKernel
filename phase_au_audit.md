# Phase AU Audit — Atomic Reader Registration, Slot Reclamation & SMP Race-Freedom Proof

**Date:** 2026-11-01  
**Phase:** AU  
**Status:** Pre-Implementation Architectural Correctness Audit  
**Workspace:** `C:\Users\Akash\Documents\MyKernel`  
**Git Baseline Commit:** `a133b84cf42cb086689a1a270c44a2ce05a2bc09`  

---

## 1. Executive Summary & Objective

In Phases AM through AT, AstraOS evolved from single-core cooperative execution to a multi-core Symmetric Multiprocessing (SMP) operating system with:
1. Application Processor (AP) bring-up via Limine SMP boot protocol.
2. Per-CPU Local APIC timer preemption and dedicated idle threads.
3. Lock-free per-CPU TLB shootdown request queues (`smp_cpu_tlb_queue_t`) with 8 fixed slots per core (`SMP_TLB_SLOTS_PER_CPU`).
4. Monotonic generational sequencing (`g_tlb_gen_counter`, `slot->gen`, `slot->ack_gens[MAX_CPUS]`).
5. Standardized C memory model atomic primitives (`__atomic_*` builtins with `__ATOMIC_SEQ_CST`).
6. Leaf page table entry transitional isolation (`PTE_TRANSITION`, bit 9) with atomic fault containment.

### The Core Phase AU Question
> **Can a sender ever reclaim or overwrite a TLB shootdown queue slot while a remote CPU is about to register itself as an in-flight reader?**

This audit proves the exact failure mode present in sequential inspection protocols, defines a formally verified atomic handshake protocol with store-load barrier symmetry (Dekker-style mutual exclusion), and establishes a deterministic multi-core qualification suite (Tests A–F) to mathematically and experimentally certify race freedom.

---

## 2. In-Depth Concurrency & Race-Window Analysis

### 2.1 The Phase AT Inspection Window
In Phase AT, remote TLB invalidation in `smp_service_pending_tlb_requests(my_cpu)` evaluated slots using the following sequence:

```c
/* --- Phase AT Implementation --- */
for (u32 idx = 0; idx < SMP_TLB_SLOTS_PER_CPU; idx++) {
    smp_tlb_slot_t *slot = &g_smp_tlb_queues[s].slots[idx];
    u32 targets = __atomic_load_n(&slot->target_mask, __ATOMIC_SEQ_CST);
    if ((targets & (1U << my_cpu)) == 0) continue;

    tlb_slot_state_t st = __atomic_load_n(&slot->state, __ATOMIC_SEQ_CST);
    if (st != TLB_SLOT_ACTIVE && st != TLB_SLOT_TIMED_OUT) continue;

    u32 acks = __atomic_load_n(&slot->ack_mask, __ATOMIC_SEQ_CST);
    if ((acks & (1U << my_cpu)) != 0) continue;

    /* RACE WINDOW OPENS HERE */
    // Receiver has determined that slot is ACTIVE and targets my_cpu.
    // Receiver experiences an interrupt, hardware cache stall, or execution delay.
    
    /* Registration occurs AFTER the initial decision to service the slot */
    __atomic_fetch_or(&slot->in_flight_mask, (1U << my_cpu), __ATOMIC_SEQ_CST);
    /* RACE WINDOW CLOSES HERE */
...
```

Concurrently, the sender CPU executes slot retirement and reclamation in `smp_tlb_shootdown()`:
```c
/* Sender scans for an eligible slot to reallocate */
for (u32 i = 0; i < SMP_TLB_SLOTS_PER_CPU; i++) {
    u32 cand = (queue->head + i) % SMP_TLB_SLOTS_PER_CPU;
    tlb_slot_state_t st = __atomic_load_n(&queue->slots[cand].state, __ATOMIC_SEQ_CST);
    u32 in_flight = __atomic_load_n(&queue->slots[cand].in_flight_mask, __ATOMIC_SEQ_CST);
    if ((st == TLB_SLOT_FREE || st == TLB_SLOT_RETIRED) && in_flight == 0) {
        allocated_idx = cand;
        break;
    }
}
```

### 2.2 Trace of the Race Window Interleaving
1. **Request $R_1$** (generation $G_1$, virtual address $V_1$) is dispatched by CPU 0 targeting CPU 1 and CPU 2.
2. CPU 1 inspects slot 0: reads `targets` (includes CPU 1), reads `state == ACTIVE`, reads `acks` (bit 1 is 0).
3. CPU 1 is delayed immediately prior to invoking `__atomic_fetch_or(&slot->in_flight_mask)`.
4. CPU 0 experiences a timeout on CPU 1 (with fault injection or high interrupt latency), transitions `state = TLB_SLOT_TIMED_OUT`.
5. Under high queue turnover or delayed drainage, CPU 0 prepares to reclaim candidate slots.
6. CPU 0 loads `state` of slot 0. If slot 0 was marked retired or reclaimed:
   - CPU 0 loads `slot->in_flight_mask`. Because CPU 1 has not yet executed `__atomic_fetch_or`, CPU 0 reads `in_flight_mask == 0`!
   - CPU 0 concludes slot 0 is free from remote readers.
   - CPU 0 re-initializes slot 0 for **Request $R_2$** (generation $G_2$, virtual address $V_2$, target mask $T_2$).
7. CPU 1 resumes: executes `__atomic_fetch_or(&slot->in_flight_mask)`.
8. CPU 1 now observes slot 0 in an inconsistent state:
   - If CPU 0 has partially overwritten the non-atomic struct fields (`slot->va`, `slot->pml4_phys`, `slot->page_count`), CPU 1 reads torn or mismatched addresses.
   - Even if CPU 1 re-checks `gen`, CPU 1 might either abort or service $R_2$ prematurely, while $R_1$'s TLB invalidation is silently lost on CPU 1!

---

## 3. The Atomic Handshake Protocol & Memory Model Proof

To eliminate this vulnerability with mathematical certainty, Phase AU introduces an **Atomic Pre-Registration Handshake** with Dekker-style sequential consistency symmetry.

### 3.1 Formal Protocol Definition

```
        RECEIVER (Remote CPU)                         SENDER (Initiating CPU)
        ---------------------                         -----------------------
                                                   [Slot Retirement / Scan]
[Fast Pre-Filter]                                  1. Store slot->state = RETIRED
If state != ACTIVE && != TIMED_OUT: Skip              (or hold lock on candidate queue)
If target_mask & my_cpu == 0: Skip                 2. Store slot->target_mask = 0
If ack_mask & my_cpu != 0: Skip                    3. Load slot->in_flight_mask (SEQ_CST)
                                                      |
[Step 1: Atomic Pre-Registration]                     |
Store-Load Barrier:                                   |
__atomic_fetch_or(&slot->in_flight_mask,              |
                  (1 << my_cpu), SEQ_CST)             |
       |                                              |
       v                                              |
[Step 2: Full Barrier State Validation]               |
Load slot->state (SEQ_CST)                            |
Load slot->target_mask (SEQ_CST)                      |
Load slot->ack_mask (SEQ_CST)                         |
       |                                              |
       +---> If state != ACTIVE && != TIMED_OUT,     |
       |     OR target_mask & my_cpu == 0,            |
       |     OR ack_mask & my_cpu != 0:               |
       |     [ABORT & ROLLBACK]                       |
       |     __atomic_fetch_and(&slot->in_flight_mask, |
       |                        ~(1 << my_cpu))       |
       |     Continue to next slot.                   |
       |                                              v
       v                                      [Reclamation Gate]
[Step 3: Linearized Safe Critical Section]     If state in {FREE, RETIRED}
- Slot protected against reclamation           AND in_flight_mask == 0:
- Execute invlpg / CR3 invalidation              --> Reclaim & Allocate
- Double-verify generation                     Else:
- Publish ack_gens[my_cpu] & ack_mask             --> DEFER / WAIT
- Clear in_flight_mask
```

### 3.2 Formal Sequential Consistency Proof
Let $S$ denote the sequentially consistent total order enforced by `__ATOMIC_SEQ_CST` across all CPUs on the x86-64 hardware bus (via `mfence` / locked instructions).

Define the operations:
- **$W_{reg}$**: Receiver's atomic write setting `slot->in_flight_mask |= (1 << my_cpu)`.
- **$R_{st}$**: Receiver's atomic read of `slot->state` and `slot->target_mask`.
- **$W_{ret}$**: Sender's atomic write setting `slot->state = RETIRED` and `slot->target_mask = 0`.
- **$R_{inf}$**: Sender's atomic read of `slot->in_flight_mask`.

By program order on the respective CPUs:
1. $W_{reg} <_S R_{st}$ (Receiver executes pre-registration before validating state).
2. $W_{ret} <_S R_{inf}$ (Sender executes retirement before scanning/reclaiming).

Now evaluate the interleaving in total order $S$:
- **Case 1: $W_{reg} <_S R_{inf}$**  
  The receiver's pre-registration precedes the sender's read of `in_flight_mask`.
  Therefore, $R_{inf}$ MUST read `in_flight_mask != 0`.
  The sender's reclamation condition `in_flight_mask == 0` evaluates to **FALSE**.
  **Conclusion:** The sender is strictly barred from reclaiming or overwriting the slot while the receiver is active.

- **Case 2: $R_{inf} <_S W_{reg}$**  
  The sender's read of `in_flight_mask` precedes the receiver's pre-registration.
  Since $W_{ret} <_S R_{inf}$ (program order) and $R_{inf} <_S W_{reg}$, transitivity requires:
  $$W_{ret} <_S W_{reg}$$
  Furthermore, since $W_{reg} <_S R_{st}$, transitivity requires:
  $$W_{ret} <_S R_{st}$$
  Therefore, $R_{st}$ MUST observe the sender's write $W_{ret}$ (`state = TLB_SLOT_RETIRED` or `target_mask = 0`).
  The receiver's validity check fails:
  $$\text{state} \notin \{\text{ACTIVE}, \text{TIMED\_OUT}\} \quad \lor \quad (\text{target\_mask} \ \& \ (1 \ll \text{my\_cpu})) == 0$$
  The receiver immediately triggers **ABORT & ROLLBACK**, clearing its bit from `in_flight_mask` without reading or invalidating parameters.
  **Conclusion:** The receiver accesses zero reused request parameters.

**Theorem (Atomic Handshake Invariance):**  
There exists no schedule under `__ATOMIC_SEQ_CST` in which the sender observes `in_flight_mask == 0` while the receiver observes `state == ACTIVE` on the same queue slot. Mutual exclusion between reader parameter access and sender slot reclamation is strictly complete. $\blacksquare$

---

## 4. Architectural Protection of `PTE_TRANSITION` (Bit 9)

In Phase AS and AT, leaf page table unmapping introduced provisional state marking via `PTE_TRANSITION` (`1ULL << 9`).

### 4.1 Invariants
1. **MMU Hardware Invariance**: On x86-64, bits 9..11 of page table entries are architecturally reserved for system software ("Available for system programmer's use"). Hardware MMUs ignore bit 9 when `PTE_PRESENT` is 0.
2. **Atomic Unmap Sequence**:
   - `pt[pt_idx] = (old_pte & ~PTE_PRESENT) | PTE_TRANSITION;`
   - Memory barrier.
   - `smp_tlb_shootdown()`.
   - On SUCCESS: `pt[pt_idx] = 0;` followed by `pmm_free_page()`.
   - On TIMEOUT / FAILURE: `pt[pt_idx]` is rolled back to `old_pte` if and only if `pt[pt_idx] == ((old_pte & ~PTE_PRESENT) | PTE_TRANSITION)`.
3. **Fault Containment**:
   - Any concurrent memory access to the page during shootdown traps to `#PF` with Error Code `Present=0`.
   - The page fault handler recognizes `PTE_PRESENT == 0`. Ring 3 access is safely routed to process termination (`process_exit_from_fault`) without kernel corruption.
   - Concurrent `vmm_map_page()` checks `(pt[pt_idx] & (PTE_PRESENT | PTE_TRANSITION))` and returns `VMM_ERR_ALREADY_MAPPED`, preventing double allocation.
   - Concurrent `vmm_unmap_page()` checks `pt[pt_idx] & PTE_PRESENT` and returns `VMM_ERR_NOT_MAPPED`, preventing duplicate unmapping.

---

## 5. Phase AU Deterministic Test Specification (Tests A–F)

Phase AU defines 6 rigorous deterministic tests to be implemented in `kernel/process/process.c`:

### Test A: Reader Pauses Before Registration
- **Marker**: `ASTRA_AU_READER_PAUSE_BEFORE_REG_OK`
- **Objective**: Verify that if a remote receiver pauses before setting `in_flight_mask`, and the sender retires the slot, the receiver cleanly detects retirement and aborts without accessing reused parameters.

### Test B: Reader Registers Before Retirement
- **Marker**: `ASTRA_AU_READER_REG_BEFORE_RETIRE_OK`
- **Objective**: Verify that if a reader pre-registers in `in_flight_mask`, the sender scanning for reusable slots sees `in_flight_mask != 0` and is completely blocked from reallocating the slot until the reader completes.

### Test C: Retirement Races with Registration
- **Marker**: `ASTRA_AU_RETIRE_RACE_REGISTRATION_OK`
- **Objective**: Verify strict binary mutual exclusion when retirement and registration execute concurrently. Exactly one side wins: either reader registers first (deferring sender reuse), or sender retires first (causing reader abort). Zero torn reads or parameter leaks.

### Test D: Delayed IPI After Timeout
- **Marker**: `ASTRA_AU_DELAYED_IPI_TIMEOUT_DRAIN_OK`
- **Objective**: Verify that when a sender marks a slot `TLB_SLOT_TIMED_OUT`, a late-arriving IPI handler pre-registers, safely invalidates using the timed-out request's parameters, transitions state to `TLB_SLOT_RETIRED`, and clears `in_flight_mask`.

### Test E: Slot Reuse Stress
- **Marker**: `ASTRA_AU_SLOT_REUSE_STRESS_OK`
- **Objective**: Execute multi-cycle rapid slot allocations, invalidations, retirements, and immediate reallocations under active multi-core execution to verify that generation counters and atomic handshakes prevent generational aliasing.

### Test F: Queue Exhaustion & Failure Recovery
- **Marker**: `ASTRA_AU_QUEUE_EXHAUSTION_OK`
- **Objective**: Fill all `SMP_TLB_SLOTS_PER_CPU` (8 slots) with active/in-flight requests. Verify that `smp_tlb_shootdown` cleanly detects queue saturation, rejects allocation without corrupting existing slots, and recovers immediately once slots drain.

---

## 6. Verification & Qualification Matrix

| Target | Configuration | Requirement |
|---|---|---|
| **QEMU Baseline** | `-smp 4` | All kernel subsystems, user processes, and 6 Phase AU tests pass |
| **QEMU Single-Core** | `-smp 1` | Zero single-core regressions, graceful single-core fallbacks |
| **QEMU Dual-Core** | `-smp 2` | Clean AP bring-up, 2-core shootdown and handshake verification |
| **QEMU Octa-Core** | `-smp 8` | High-concurrency 8-core AP execution, no bus deadlocks |
| **QEMU Headless** | `-smp 4 -display none` | Pure headless serial verification |
| **Host Unit Suites** | 7 suites (Crypto, VFS, Block, AstraFS, ELF, Bookmark, Browser) | 100% pass across all native unit tests |
| **Oracle VirtualBox 7** | EFI Multi-Core (`scripts/test_vbox.py`) | All **146 / 146** markers verified on bare-metal hypervisor |

---

## 7. Conclusion & Next Implementation Steps

The pre-registration atomic handshake provides a provably sound resolution to the remote reader race window.
Next implementation steps:
1. Update `kernel/arch/x86_64/smp.c` with the pre-registration handshake in `smp_service_pending_tlb_requests()`.
2. Implement Tests A through F in `kernel/process/process.c`.
3. Wire Phase AU tests into `kernel/include/kernel/process.h` and `kernel/kernel.c`.
4. Update `tests/test_runner.py` and `scripts/test_vbox.py` for all 6 markers (total target: 146 markers).
5. Execute the full multi-core qualification matrix.
6. Strictly finish Phase AU; do not begin Phase AV.

