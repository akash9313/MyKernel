# Phase AT — SMP Memory-Model Correctness, TLB Slot Lifetime & Remote Handler Safety
## Independent Pre-Implementation Audit & Verification Architecture

**Date:** October 31, 2026  
**Status:** Audit Complete — Pre-Implementation Baseline  
**Workspace:** `C:\Users\Akash\Documents\MyKernel`  
**Target:** AstraOS x86-64 Symmetric Multiprocessing (SMP) Kernel  
**Baseline HEAD:** `a133b84cf42cb086689a1a270c44a2ce05a2bc09`  

---

## 1. Executive Summary & Objective

In Phase AS, AstraOS transitioned from a single global shared TLB shootdown mailbox to a per-CPU distributed TLB request queue (`g_smp_tlb_queues[MAX_CPUS]`, each containing `SMP_TLB_SLOTS_PER_CPU = 4` slots). Phase AS successfully validated the queue lifecycle, basic monotonic generation counters, timed-out slot retention, and leaf PTE rollbacks under fault injection, reaching 134/134 verified markers in Oracle VirtualBox 7 EFI and 100% pass rates across QEMU 1, 2, 4, 8 cores and all 7 host test suites.

**Phase AT** focuses on deep correctness under the C memory model (C99/C11) and actual x86-64 hardware execution:
1. **Memory-Model Atomicity and Data-Race Elimination**: Eradicate all mixed atomic/non-atomic accesses to shared slot fields (`state`, `gen`, `target_mask`, `ack_mask`, `ack_gens`, `in_flight_mask`). Standardize all accesses using explicit compiler builtins (`__atomic_*`) with strict memory ordering (`__ATOMIC_SEQ_CST`, `__ATOMIC_ACQUIRE`, `__ATOMIC_RELEASE`).
2. **In-Flight Remote Handler Lifetime Tracking & Premature Slot Reclamation**: Prevent torn reads, data corruption, or stale invalidations when a delayed remote core is still reading slot parameters (`va`, `pml4_phys`, `page_count`) while an initiator times out or attempts to reclaim/reallocate the slot. Senders cannot reclaim or reuse a slot unless its state is `FREE` or `RETIRED` **and** its `in_flight_mask` is 0.
3. **Double-Generation Verification & Stale ACK Immunity**: Remote handlers must verify generation identity both *before* invalidation and *after* invalidation before committing acknowledgements.
4. **PTE_TRANSITION Containment & Concurrent Isolation**: Validate that the architectural software bit 9 (`PTE_TRANSITION`) safely traps #PF with Present=0 on unauthorized memory access, and protects leaf entries from conflicting concurrent modifications.
5. **Zero Frame Leakage & Qualification**: Guarantee exact physical frame reclamation across failure recovery cycles.

---

## 2. Source-Level Audit of Existing TLB Queue Implementation

An exhaustive audit of `kernel/arch/x86_64/smp.c`, `kernel/include/kernel/smp.h`, `kernel/mm/vmm.c`, and `kernel/sched/sched.c` revealed key areas requiring hardening:

### 2.1 Shared Slot State Access Inconsistencies (Data Races)
In Phase AS, several shared slot fields in `smp_tlb_slot_t` were accessed using bare assignment operators alongside `__atomic_*` builtins:
- In `smp_tlb_shootdown()`:
  - `slot->state = TLB_SLOT_FREE;` (Bare assignment)
  - `slot->gen = req_gen;` (Bare assignment)
  - `slot->ack_mask = 0;` (Bare assignment)
  - `slot->ack_gens[c] = 0;` (Bare assignment)
  - `slot->state = TLB_SLOT_ACTIVE;` (Bare assignment)
  - `slot->state = TLB_SLOT_TIMED_OUT;` (Bare assignment)
  - `slot->state = TLB_SLOT_RETIRED;` (Bare assignment)
- While in `smp_service_pending_tlb_requests()`:
  - `__atomic_load_n(&slot->state, __ATOMIC_SEQ_CST);`
  - `__atomic_load_n(&slot->gen, __ATOMIC_SEQ_CST);`
  - `__atomic_fetch_or(&slot->ack_mask, ...);`
  - `__atomic_store_n(&slot->ack_gens[my_cpu], ...);`

**Defect Analysis**: Under C99/C11 memory model semantics, mixed atomic and non-atomic operations on the same memory location constitute an undefined data race. Compilers are permitted to reorder bare stores past fences or emit non-atomic store sequences that could tear on 32-bit/64-bit boundaries.
**Remediation**: All shared slot fields MUST be accessed strictly using `__atomic_load_n`, `__atomic_store_n`, `__atomic_fetch_or`, and `__atomic_fetch_and` with explicit memory orderings (`__ATOMIC_SEQ_CST`).

### 2.2 Remote Handler Lifetime & In-Flight Parameter Protection
Consider the following race condition under high contention or interrupt latency:
1. Core 0 allocates slot 0, writes `va = 0x400000`, `gen = 42`, `state = ACTIVE`, broadcasts IPI.
2. Core 1 receives IPI, calls `smp_service_pending_tlb_requests(1)`, checks `target_mask & (1 << 1)`, sees `state == ACTIVE`.
3. Core 1 suffers an interrupt delay or bus latency before reading `slot->va`.
4. Core 0 reaches timeout loop limit, sets `state = TIMED_OUT`.
5. Suppose other cores finished, or Core 0 wraps around after draining and reallocates slot 0 for a new shootdown (`va = 0x700000`, `gen = 43`).
6. Core 1 resumes, reads `slot->va` (now `0x700000`), performs `invlpg(0x700000)` instead of `0x400000`, and sets `ack_mask` for gen 42 or 43!

**Remediation**:
1. Add `volatile u32 in_flight_mask;` to `smp_tlb_slot_t`.
2. Remote core sets `__atomic_fetch_or(&slot->in_flight_mask, (1U << my_cpu), __ATOMIC_SEQ_CST)` upon beginning to process a slot.
3. Remote core samples `gen_start = __atomic_load_n(&slot->gen, __ATOMIC_SEQ_CST)`.
4. Remote core reads `pml4_phys`, `va`, `page_count` and performs invalidation.
5. Remote core re-verifies `__atomic_load_n(&slot->gen, __ATOMIC_SEQ_CST) == gen_start` (Double Generation Verification).
6. If verified, updates `ack_gens` and `ack_mask`.
7. Remote core clears its bit: `__atomic_fetch_and(&slot->in_flight_mask, ~(1U << my_cpu), __ATOMIC_SEQ_CST)` on exit.
8. Sender candidate allocator checks:
   ```c
   tlb_slot_state_t st = __atomic_load_n(&slot->state, __ATOMIC_SEQ_CST);
   u32 in_flight = __atomic_load_n(&slot->in_flight_mask, __ATOMIC_SEQ_CST);
   if ((st == TLB_SLOT_FREE || st == TLB_SLOT_RETIRED) && in_flight == 0) {
       allocated_idx = cand;
       break;
   }
   ```
   A slot is NEVER reclaimed or overwritten while `in_flight_mask != 0`.

### 2.3 Linearized Slot State Machine
The lifecycle of a TLB slot follows a strictly linearized state progression:
```
  [ FREE ]
     |
     v (Allocator finds slot: state==FREE/RETIRED && in_flight_mask==0)
  [ ACTIVE ] (Parameters written, fences emitted, target_mask set, IPI sent)
     |
     +-------------------------------+
     | (All ACKs match gen)          | (Timeout reached before all ACKs)
     v                               v
  [ RETIRED ]                 [ TIMED_OUT ]
     ^                               |
     |                               | (Delayed receiver services slot & all ACKs arrive)
     +-------------------------------+
     |
     v (Sender reallocates when in_flight_mask == 0)
  [ FREE / ACTIVE ]
```

### 2.4 PTE_TRANSITION Architectural Software Bit (Bit 9)
- Bit 9 is ignored by the x86-64 MMU when `PTE_PRESENT` (bit 0) is 0.
- When `vmm_unmap_page()` sets `pt[pt_idx] = (old_pte & ~PTE_PRESENT) | PTE_TRANSITION;`, any CPU access to this virtual address will immediately trigger a Page Fault (`#PF`) with error code bit 0 (Present = 0).
- In user mode (`(cs & 3) == 3`), the `#PF` handler routes to `process_exit_from_fault()`, ensuring fault containment without system hang.
- In kernel mapping routines, `vmm_map_page()` checks `(pt[pt_idx] & (PTE_PRESENT | PTE_TRANSITION))` and returns `VMM_ERR_ALREADY_MAPPED`, preventing conflicting concurrent mappings into transitioning slots.

---

## 3. Implementation Plan & File Modifications

| File | Proposed Changes |
| :--- | :--- |
| `kernel/include/kernel/smp.h` | Add `volatile u32 in_flight_mask;` to `smp_tlb_slot_t`. Ensure full 64-bit alignment and clear field definitions. |
| `kernel/arch/x86_64/smp.c` | Standardize all slot reads and writes to `__atomic_*`. Implement in-flight reader tracking (`in_flight_mask`) and double generation checking in `smp_service_pending_tlb_requests()`. Enforce `in_flight_mask == 0` during slot selection in `smp_tlb_shootdown()`. |
| `kernel/process/process.c` | Implement `process_at_run_tests()` containing Tests 1 through 6 covering memory model atomics, handler lifetime, double-gen verification, lock-free reentrancy, PTE transition containment, and end-to-end stress recovery. |
| `kernel/include/kernel/process.h` | Declare `void process_at_run_tests(void);`. |
| `kernel/kernel.c` | Invoke `process_at_run_tests()` in kernel bootstrap sequence after Phase AS tests. |
| `tests/test_runner.py` | Add Phase AT test assertions for all 6 markers (total markers updated). |
| `scripts/test_vbox.py` | Update expected VirtualBox EFI test markers to 140 (134 + 6 Phase AT markers). |
| `docs/architecture.md` | Document Section 56 covering Phase AT memory model formalization and in-flight reader tracking. |
| `docs/status.md` | Update qualification matrix and status indicators. |

---

## 4. Phase AT Test Suite Specification

1. **TEST 1: Shared Memory Model Atomicity & Zero Data Race Qualification**
   - Marker: `ASTRA_AT_MEMORY_MODEL_ATOMICS_OK`
   - Verifies explicit atomic builtin usage across all slot state transitions and parameter publications.
2. **TEST 2: In-Flight Reader Lifetime & Premature Reuse Prevention**
   - Marker: `ASTRA_AT_REMOTE_HANDLER_LIFETIME_OK`
   - Verifies that a retired slot with active in-flight readers (`in_flight_mask != 0`) is strictly protected from reclamation or overwriting.
3. **TEST 3: Double-Generation Verification & Stale ACK Rejection**
   - Marker: `ASTRA_AT_DOUBLE_GEN_VERIFY_OK`
   - Verifies that post-invalidation generation validation detects torn/reused slots and suppresses spurious ACKs.
4. **TEST 4: Re-Entrant Lock-Free Servicing Invariance**
   - Marker: `ASTRA_AT_LOCKFREE_SERVICING_OK`
   - Verifies that `smp_service_pending_tlb_requests` executes lock-free without deadlocking even under re-entrant nested callers.
5. **TEST 5: PTE_TRANSITION Page Fault Containment & Concurrent Isolation**
   - Marker: `ASTRA_AT_PTE_TRANSITION_ISOLATION_OK`
   - Verifies architectural #PF containment on non-present transitioning PTEs and rejection of conflicting concurrent operations.
6. **TEST 6: End-to-End Multi-Core Memory Model Stress & Zero Frame Leak Recovery**
   - Marker: `ASTRA_AT_END_TO_END_MEM_MODEL_OK`
   - Stresses cross-core shootdown, fault injection timeout, delayed drainage, and asserts `pmm_stats.free_pages` zero leak across 3 cycles.

---

## 5. Verification Gate Criteria
- Zero compiler warnings and zero build errors under `-Wall -Wextra -Werror`.
- 100% pass across QEMU `-smp 1`, `-smp 2`, `-smp 4`, `-smp 8`, and `-smp 4` (`nofb`).
- 100% pass across all 7 host test suites.
- 140/140 markers verified under Oracle VirtualBox 7 EFI (`python scripts/test_vbox.py`).
- Complete documentation of architecture and status updates.
- Strictly complete Phase AT; do not start Phase AU.

