# Phase AV Audit — Independent SMP Memory-Model Audit & TLB Slot Reclamation Verification

**Date:** 2026-11-01  
**Phase:** AV  
**Status:** Pre-Implementation Architectural & Concurrency Audit  
**Workspace:** `C:\Users\Akash\Documents\MyKernel`  
**Git Baseline Commit:** `a133b84cf42cb086689a1a270c44a2ce05a2bc09`  

---

## 1. Executive Summary & Objective

Phase AV conducts an adversarial, source-level and runtime verification of AstraOS's multi-core TLB slot reclamation and remote reader registration protocols introduced in Phases AT and AU.

Treating all previous PASS markers as preliminary evidence, this audit investigates whether:
1. The Phase AU atomic pre-registration handshake is provably race-free under all possible multi-core schedules.
2. The sender and receiver state transitions exhaustively resolve to either **Outcome A (Reader wins)** or **Outcome B (Sender wins)** without intermediate torn or corrupted states.
3. Concurrency hazards—such as Generation ABA, delayed IPI drainage, duplicate registrations, queue saturation, and multi-sender allocation—are completely contained.
4. Leaf page table entry transitional isolation (`PTE_TRANSITION`, bit 9) safely coordinates with hardware fault handling, unmap rollbacks, and frame reclamation.

---

## 2. Source-Level Tracing of the 7 Operations

In `kernel/arch/x86_64/smp.c` and `kernel/include/kernel/smp.h`, each core possesses an independent request queue `g_smp_tlb_queues[cur_cpu]` with 4 slots (`SMP_TLB_SLOTS_PER_CPU`).

The 7 primary operations operate across two distinct CPU roles:

### 2.1 Reader Operations (`smp_service_pending_tlb_requests(my_cpu)`)

1. **Reader Registration (Operation 1)**:
   - Line 87: `__atomic_fetch_or(&slot->in_flight_mask, (1U << my_cpu), __ATOMIC_SEQ_CST);`
   - *Linearization Point of Reader Registration*: An atomic read-modify-write emitting `lock bts` or `lock or` on x86-64. Enforces a full store-load hardware bus barrier.

2. **Reader State Validation (Operation 2)**:
   - Lines 90-93: Loads `st = slot->state`, `targets = slot->target_mask`, `acks = slot->ack_mask`, and `req_gen = slot->gen` under `__ATOMIC_SEQ_CST`.
   - Lines 95-101: If `st` is neither `ACTIVE` nor `TIMED_OUT`, or `targets` lacks `(1 << my_cpu)`, or `acks` already contains `(1 << my_cpu)`:
     - Reader immediately executes abort and rollback:
       `__atomic_fetch_and(&slot->in_flight_mask, ~(1U << my_cpu), __ATOMIC_SEQ_CST);`
       and skips to the next slot.

3. **Reader Payload Access (Operation 3)**:
   - Lines 104-107: Reads `req_pml4 = slot->pml4_phys`, `va = slot->va`, `count = slot->page_count`.
   - Executes hardware invalidation instructions (`invlpg va + p*4096` or CR3 reload).
   - Guaranteed protected against concurrent mutation because sender cannot reclaim a slot while `in_flight_mask` is non-zero.

4. **Reader Deregistration & ACK Publication (Operation 4)**:
   - Lines 126-133: Re-checks generation: `post_gen = slot->gen`.
   - If `post_gen == req_gen`:
     - Writes `slot->ack_gens[my_cpu] = req_gen;` (dedicated per-CPU slot entry; zero false sharing).
     - Issues release fence `__atomic_thread_fence(__ATOMIC_SEQ_CST)`.
     - Atomically sets ACK bit: `__atomic_fetch_or(&slot->ack_mask, (1U << my_cpu), __ATOMIC_SEQ_CST)`.
   - Line 142: Unconditionally executes deregistration:
     `__atomic_fetch_and(&slot->in_flight_mask, ~(1U << my_cpu), __ATOMIC_SEQ_CST);`

### 2.2 Sender Operations (`smp_tlb_shootdown()`)

5. **Sender Slot Retirement (Operation 5)**:
   - Lines 278-279:
     ```c
     __atomic_store_n(&slot->state, TLB_SLOT_RETIRED, __ATOMIC_SEQ_CST);
     __atomic_store_n(&slot->target_mask, 0, __ATOMIC_SEQ_CST);
     ```
   - *Linearization Point of Sender Retirement*: The atomic store setting `slot->state = TLB_SLOT_RETIRED` followed by zeroing `slot->target_mask`.

6. **Sender In-Flight Reader Check (Operation 6)**:
   - Lines 184-189:
     ```c
     tlb_slot_state_t st = __atomic_load_n(&slot->state, __ATOMIC_SEQ_CST);
     u32 in_flight = __atomic_load_n(&slot->in_flight_mask, __ATOMIC_SEQ_CST);
     if ((st == TLB_SLOT_FREE || st == TLB_SLOT_RETIRED) && in_flight == 0) { ... }
     ```
   - The sender examines whether `in_flight == 0`. If any reader is in operations 1 through 4, `in_flight != 0`, and the sender is strictly barred from reclaiming or overwriting this candidate slot.

7. **Slot Reclamation and Reuse (Operation 7)**:
   - Lines 214-230:
     - Marks `slot->state = TLB_SLOT_FREE;`
     - Advances monotonic 64-bit generator: `req_gen = ++g_tlb_gen_counter;`
     - Writes `slot->gen = req_gen`, `sender_cpu`, `pml4_phys`, `va`, `page_count`.
     - Zeroes `slot->ack_mask = 0;` and all `slot->ack_gens[c] = 0;`.
     - Issues release fence `__atomic_thread_fence(__ATOMIC_RELEASE);`.
     - Publishes state: `__atomic_store_n(&slot->state, TLB_SLOT_ACTIVE, __ATOMIC_RELEASE);`.
     - Publishes targets: `__atomic_store_n(&slot->target_mask, target_mask, __ATOMIC_SEQ_CST);`.
     - Broadcasts IPI 251.

---

## 3. Formal Verification of the Atomic Handshake (Outcomes A & B)

We evaluate whether the competition between a remote reader attempting registration and a sender attempting retirement/reclamation is exhaustively partitioned into two deterministic outcomes.

### 3.1 Theorem: Exhaustive Binary Mutual Exclusion
Let $S$ denote the total order enforced by sequentially consistent atomic operations (`__ATOMIC_SEQ_CST`):
- $W_{reg}$: Reader's atomic store-load write: `in_flight_mask |= (1 << my_cpu)`.
- $R_{st}$: Reader's atomic load of `state` and `target_mask`.
- $W_{ret}$: Sender's atomic store: `state = RETIRED` and `target_mask = 0`.
- $R_{inf}$: Sender's atomic load: `in_flight_mask`.

By program order on the hardware execution streams:
$$W_{reg} <_S R_{st} \quad \text{and} \quad W_{ret} <_S R_{inf}$$

Evaluating all possible relations between $W_{reg}$ and $R_{inf}$ in total order $S$:

#### Outcome A — Reader Wins ($W_{reg} <_S R_{inf}$)
1. The reader's atomic write $W_{reg}$ takes effect before the sender's read $R_{inf}$.
2. In total order $S$, $R_{inf}$ MUST observe the bit written by $W_{reg}$ (`in_flight_mask != 0`).
3. When the sender evaluates `(st == FREE || st == RETIRED) && in_flight == 0`, the predicate evaluates to **FALSE**.
4. The sender CANNOT select, reclaim, or overwrite the candidate slot.
5. The reader enters Operation 3 (payload access) with absolute memory safety. The payload words cannot be modified by the sender until the reader reaches Operation 4 and clears `in_flight_mask`.

#### Outcome B — Sender Wins ($R_{inf} <_S W_{reg}$)
1. The sender's read $R_{inf}$ precedes the reader's atomic write $W_{reg}$.
2. Because program order mandates $W_{ret} <_S R_{inf}$, transitivity across total order $S$ requires:
   $$W_{ret} <_S R_{inf} <_S W_{reg} <_S R_{st} \implies W_{ret} <_S R_{st}$$
3. Therefore, the reader's load $R_{st}$ MUST observe the sender's write $W_{ret}$ (`state = TLB_SLOT_RETIRED` and `target_mask = 0`).
4. In Operation 2, the reader's condition:
   $$(st \neq \text{ACTIVE} \land st \neq \text{TIMED\_OUT}) \lor (targets \ \& \ (1 \ll \text{my\_cpu})) == 0$$
   evaluates to **TRUE**.
5. The reader immediately executes **Abort and Rollback**, clearing its bit from `in_flight_mask`.
6. The reader does NOT access payload words (`pml4_phys`, `va`, `page_count`) and does NOT publish an ACK.

#### Exhaustiveness
Because total order $S$ is a strict total order over all sequentially consistent operations on the hardware bus, either $W_{reg} <_S R_{inf}$ or $R_{inf} <_S W_{reg}$. There is no third interleaving. Neither outcome permits simultaneous reader payload access and sender slot reclamation. $\blacksquare$

---

## 4. Comprehensive Audit of Concurrency Hazards

### 4.1 Discovery: Stale Local Variable Race During Timeout Servicing
**Vulnerability Identified**:
In `kernel/arch/x86_64/smp.c` line 135:
```c
if (st == TLB_SLOT_TIMED_OUT && (new_acks & targets) == targets) {
    __atomic_store_n(&slot->state, TLB_SLOT_RETIRED, __ATOMIC_SEQ_CST);
    __atomic_store_n(&slot->target_mask, 0, __ATOMIC_SEQ_CST);
}
```
`st` is a local variable sampled in Step 2 (`st = slot->state`).
If the reader begins invalidation when the slot is `TLB_SLOT_ACTIVE`, and the sender times out *during* the reader's hardware invalidation loop (transitioning the slot to `TLB_SLOT_TIMED_OUT`), the reader finishes invalidating and sets `new_acks`. However, because the reader checks the stale local variable `st == TLB_SLOT_TIMED_OUT` (which was `ACTIVE` when sampled), the reader skips retiring the slot!
Since the sender already timed out and released its lock, the slot could remain indefinitely in `TLB_SLOT_TIMED_OUT` despite all ACKs having arrived!

**Targeted Fix**:
1. In `smp_service_pending_tlb_requests()`, re-read the CURRENT state using `__atomic_load_n(&slot->state, __ATOMIC_SEQ_CST)` before evaluating the timeout auto-retirement condition.
2. In `smp_tlb_shootdown()`, during candidate scanning, if a slot is in `TLB_SLOT_TIMED_OUT` with `in_flight_mask == 0` and all targets have acknowledged (`(acks & targets) == targets`), dynamically retire the slot to `TLB_SLOT_RETIRED`.

### 4.2 Generation ABA Hazards
- `gen` is a 64-bit monotonically increasing counter (`g_tlb_gen_counter`).
- At 10,000,000 shootdowns per second, 64-bit counter wraparound requires 58,494 years. Wraparound to a previously seen generation is impossible.
- Double-generation verification (`slot->gen` pre-check vs post-check) guarantees that if a slot were reallocated while a reader paused, the reader detects `post_gen != req_gen` and refuses to record an ACK.

### 4.3 Bitmask and Multi-Core Scaling
- `MAX_CPUS` is 16. `target_mask`, `ack_mask`, and `in_flight_mask` are `u32` bitmasks.
- Shifting `(1U << my_cpu)` is defined and strictly bounds within 16 bits. Zero arithmetic or bitmask overflow.

### 4.4 Multi-Sender Isolation
- Each CPU initiates shootdowns strictly through its own private queue `g_smp_tlb_queues[cur_cpu]`, protected by `g_smp_tlb_queues[cur_cpu].lock`.
- No CPU ever allocates from or advances the head of another CPU's queue. Cross-sender queue corruption is impossible.

---

## 5. PTE_TRANSITION and VMM Integration

1. **Leaf Transition State**:
   - `vmm_unmap_page()` atomically sets leaf PTE to `(old_pte & ~PTE_PRESENT) | PTE_TRANSITION`.
   - Hardware MMU treats the page as Not-Present (bit 9 ignored by CPU translation).
2. **Page Fault Containment**:
   - Access to a transitioning page traps to `#PF` (Vector 14, Error Code Present=0).
   - In user mode, `#PF` safely routes to `process_exit_from_fault()`, isolating the fault without kernel corruption.
3. **Rollback & Frame Retention Invariants**:
   - If shootdown times out or fails, `vmm_unmap_page()` rolls back `pt[pt_idx] = old_pte;` if and only if `pt[pt_idx]` is still in transition state.
   - Physical frame is strictly retained (`pmm_free_page` is skipped).
   - Conflicting `vmm_map_page` calls are rejected with `VMM_ERR_ALREADY_MAPPED`.
   - Conflicting `vmm_unmap_page` calls are rejected with `VMM_ERR_NOT_MAPPED`.

---

## 6. Deterministic Adversarial Test Plan (Tests A–G)

To be implemented in `kernel/process/process.c` (`process_av_run_tests()`):

1. **Test A (Registration vs Retirement)**: Reader pauses before pre-registration; sender closes and reclaims; reader resumes and cleanly aborts without accessing payload (`ASTRA_AV_REG_VS_RETIRE_OK`).
2. **Test B (Registered Reader vs Retirement)**: Reader pre-registers; sender candidate scan is strictly blocked from selecting slot until reader exits (`ASTRA_AV_READER_BLOCKS_REUSE_OK`).
3. **Test C (Simultaneous Registration & Retirement)**: 100 simultaneous competing race iterations; verify strict binary outcome (Reader Wins or Sender Wins) with zero intermediate states (`ASTRA_AV_SIMULTANEOUS_RACE_OK`).
4. **Test D (Generation ABA Protection)**: Reader expects $G_1$; slot is forcefully retired and reused for $G_2$; reader resumes and detects `post_gen != req_gen`; ACK is cleanly suppressed (`ASTRA_AV_GENERATION_ABA_OK`).
5. **Test E (Delayed IPI & Timeout Recovery)**: Shootdown times out under injected dropped ACK; delayed receiver arrives, pre-registers, validates `TIMED_OUT`, auto-retires slot, and clears in-flight bits (`ASTRA_AV_DELAYED_IPI_TIMEOUT_OK`).
6. **Test F (Concurrent Senders Across Cores)**: Multiple CPUs allocate, retire, and service shootdowns simultaneously; verify independent per-CPU queue isolation and zero overlapping allocations (`ASTRA_AV_CONCURRENT_SENDERS_OK`).
7. **Test G (Queue Exhaustion & Dynamic Recovery)**: All 4 slots filled with active requests; candidate scan cleanly rejects allocation; slot release triggers immediate recovery (`ASTRA_AV_QUEUE_EXHAUSTION_OK`).

---

## 7. Next Implementation Steps

1. Apply the dual-safety fix in `kernel/arch/x86_64/smp.c` for `cur_st` in `smp_service_pending_tlb_requests` and dynamic candidate reclamation in `smp_tlb_shootdown`.
2. Implement Tests A through G in `kernel/process/process.c`.
3. Wire `process_av_run_tests()` into `kernel/include/kernel/process.h` and `kernel/kernel.c`.
4. Update `tests/test_runner.py` and `scripts/test_vbox.py` for all 7 Phase AV markers (total target: 153 markers).
5. Execute the full qualification matrix (QEMU 1, 2, 4, 8, nofb, 7 host suites, VirtualBox 7 EFI).
6. Document findings in `phase_av_report.md`, `docs/architecture.md`, and `docs/status.md`.
7. Strictly maintain boundary: Phase AW will NOT be started.

