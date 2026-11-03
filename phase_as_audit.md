# Phase AS Audit — TLB Queue Lifecycle Verification, Atomicity & Failure-Recovery Qualification

**Workspace:** `C:\Users\Akash\Documents\MyKernel`  
**Date:** 2026-10-31  
**HEAD Commit:** `a133b84cf42cb086689a1a270c44a2ce05a2bc09`  
**Status:** Audit in Progress — Prior to Code Modifications.

---

## 1. Baseline Repository & Git State

### 1.1 Git Commit & Status Record
- **HEAD Commit**: `a133b84cf42cb086689a1a270c44a2ce05a2bc09`
- **Git Status**: Clean working baseline with Phase AR deliverables intact (`phase_ar_audit.md`, `phase_ar_report.md`, Section 54 in `docs/architecture.md`, `docs/status.md`).
- **Diff Baseline**: Preserving all existing features, test suites, ATA/AstraFS drivers, SMP infrastructure, and VirtualBox EFI configurations.

---

## 2. Inventory of Current TLB Shootdown & Queue Primitives

### 2.1 Structs and Global Variables
| Symbol | File | Purpose & Current Usage |
|---|---|---|
| `SMP_TLB_SLOTS_PER_CPU` | `kernel/include/kernel/smp.h:9` | Fixed queue depth: 4 slots per CPU |
| `tlb_slot_state_t` | `kernel/include/kernel/smp.h:13` | Slot states: `TLB_SLOT_FREE`, `TLB_SLOT_ACTIVE`, `TLB_SLOT_TIMED_OUT`, `TLB_SLOT_RETIRED` |
| `smp_tlb_slot_t` | `kernel/include/kernel/smp.h:20` | Request descriptor containing `gen`, `sender_cpu`, `pml4_phys`, `va`, `page_count`, `target_mask`, `ack_mask`, `state` |
| `smp_cpu_tlb_queue_t` | `kernel/include/kernel/smp.h:31` | Per-CPU queue containing private `spinlock_t lock`, `u32 head`, and `slots[4]` |
| `g_smp_tlb_queues[MAX_CPUS]` | `kernel/arch/x86_64/smp.c:29` | Global array of per-CPU queues |
| `g_tlb_gen_counter` | `kernel/arch/x86_64/smp.c:30` | Monotonic 64-bit sequence counter incremented on each shootdown |
| `smp_service_pending_tlb_requests()` | `kernel/arch/x86_64/smp.c:50` | Non-blocking incoming request scanner called by APs on IPI and by senders in wait loops |
| `smp_handle_tlb_shootdown()` | `kernel/arch/x86_64/smp.c:121` | IDT Vector 251 ISR entry point |
| `smp_tlb_shootdown()` | `kernel/arch/x86_64/smp.c:127` | Primary cross-CPU invalidation initiator |

---

## 3. Critical Architectural & Concurrency Defect Analysis

A rigorous line-by-line inspection of Phase AR implementations reveals five key vulnerabilities that must be hardened in Phase AS:

### 3.1 Defect 1: Timed-Out Slot Overwrite on Wraparound
- **Location**: `kernel/arch/x86_64/smp.c:156-166`
- **Code Trace**:
  ```c
  u32 slot_idx = g_smp_tlb_queues[cur_cpu].head % SMP_TLB_SLOTS_PER_CPU;
  smp_tlb_slot_t *slot = &g_smp_tlb_queues[cur_cpu].slots[slot_idx];

  u32 drain_timeout = 1000000ULL;
  while (slot->state == TLB_SLOT_ACTIVE && drain_timeout > 0) { ... }
  ```
- **Vulnerability**: If `smp_tlb_shootdown()` times out, the slot enters `TLB_SLOT_TIMED_OUT`. If the sender submits 4 subsequent shootdowns, `head % 4` circles back to this slot.
- Because the loop only checks `slot->state == TLB_SLOT_ACTIVE`, it **does not wait** if the slot is `TLB_SLOT_TIMED_OUT`!
- The sender immediately overwrites `slot->gen`, `slot->pml4_phys`, `slot->va`, and `slot->target_mask` while a delayed remote AP may still be in the middle of reading or servicing the timed-out request!
- **Resolution**:
  1. A slot is occupied if `state != TLB_SLOT_FREE && state != TLB_SLOT_RETIRED`.
  2. Implement an explicit slot allocator that scans all 4 slots in the queue for a free/retired slot.
  3. If all 4 slots are occupied, wait boundedly.
  4. If all slots remain occupied after drain timeout (queue full), explicitly return an error (`false`), rather than overwriting an occupied or timed-out slot.

### 3.2 Defect 2: Lack of Explicit Generation-Matched ACK Identity
- **Location**: `kernel/arch/x86_64/smp.c:79-110`
- **Vulnerability**: Receivers record completion by executing:
  ```c
  __atomic_fetch_or(&slot->ack_mask, (1U << my_cpu), __ATOMIC_SEQ_CST);
  ```
  The sender checks `(slot->ack_mask & target_mask) == target_mask`.
  However, `ack_mask` is merely a bitmask. It does not carry the generation ID from the receiver's perspective.
  If an ACK arrives from an unexpected CPU, or from a delayed receiver for a slot that was somehow recycled, the sender cannot mathematically prove that this ACK corresponds to *this* exact request generation.
- **Resolution**:
  Add `volatile u64 ack_gens[MAX_CPUS]` to `smp_tlb_slot_t`.
  When receiver CPU $K$ services generation $G$:
  - Receivers store `__atomic_store_n(&slot->ack_gens[my_cpu], req_gen, __ATOMIC_SEQ_CST)`.
  - Then update `slot->ack_mask`.
  - The sender validates that `slot->ack_gens[K] == req_gen` for every target $K$, guaranteeing the invariant: **An ACK must identify exactly one request generation and one target CPU.**

### 3.3 Defect 3: Blind Leaf PTE Rollback vs Concurrent Modification Race
- **Location**: `kernel/mm/vmm.c:470-478`
- **Code Trace**:
  ```c
  u64 old_pte = pt[pt_idx];
  pt[pt_idx] = 0;
  spinlock_release_irqrestore(&g_vmm_lock, flags);

  if (!smp_tlb_shootdown(pml4_phys, va, 1)) {
      flags = spinlock_acquire_irqsave(&g_vmm_lock);
      pt[pt_idx] = old_pte;
      spinlock_release_irqrestore(&g_vmm_lock, flags);
      return VMM_ERR_SHOOTDOWN_FAILED;
  }
  ```
- **Vulnerability**: While `g_vmm_lock` is released during `smp_tlb_shootdown()`, another CPU or thread could invoke `vmm_map_page()` on the same `va`. Because `pt[pt_idx] == 0`, `vmm_map_page()` succeeds and installs a new PTE.
  When shootdown fails/times out, line 477 blindly executes `pt[pt_idx] = old_pte;`, completely clobbering the newer mapping!
- **Resolution**:
  Utilize architectural x86-64 software bits:
  - Define `#define PTE_TRANSITION (1ULL << 9)` (bit 9 is designated for OS use by x86-64 MMU architecture; ignored by CPU when `PTE_PRESENT` is 0).
  - Provisionally set `pt[pt_idx] = (old_pte & ~PTE_PRESENT) | PTE_TRANSITION;`.
  - `vmm_map_page()` checks `(pt[pt_idx] & (PTE_PRESENT | PTE_TRANSITION))` and rejects with `VMM_ERR_ALREADY_MAPPED`.
  - Concurrent `vmm_unmap_page()` checks `!(pt[pt_idx] & PTE_PRESENT)` and rejects with `VMM_ERR_NOT_MAPPED`.
  - On shootdown success: set `pt[pt_idx] = 0;` and prune empty tables.
  - On shootdown failure: under `g_vmm_lock`, verify `if (pt[pt_idx] == ((old_pte & ~PTE_PRESENT) | PTE_TRANSITION))` before restoring `pt[pt_idx] = old_pte;`. If altered concurrently, preserve the newer state.

### 3.4 Defect 4: Queue-Full Exhaustion & Recovery Protocol
- If all 4 slots become `TLB_SLOT_TIMED_OUT` (e.g. under permanent missing ACK injection), subsequent unmaps must fail safely without corruption.
- Furthermore, when delayed receivers eventually drain their requests, slots must transition `TLB_SLOT_TIMED_OUT -> TLB_SLOT_RETIRED -> TLB_SLOT_FREE`, restoring full queue capacity without requiring a reboot.

---

## 4. State Machine Specification

```
         +-----------------------------------------------------------+
         |                                                           |
         v                                                           | (All targets ACK)
   +-----------+   Allocate Slot   +-------------+  Wait / Timeout   +----------------+
   |   FREE    | ----------------> |   ACTIVE    | ----------------> |   TIMED_OUT    |
   +-----------+  (write params,   | (Published) |                   +----------------+
         ^        fence, target)   +-------------+                           |
         |                                |                                  |
         | (Allocated)                    | (All targets ACK)                | (Delayed receivers
         |                                v                                  |  complete)
   +-----------+                   +-------------+                           v
   |  RETIRED  | <---------------- |  COMPLETED  | <-------------------------+
   +-----------+  (Drain / Free)   +-------------+
```

1. **FREE (0)**: Unallocated slot available for any new shootdown.
2. **ACTIVE (1)**: Parameters and generation published; `target_mask` populated; remote cores actively invalidating.
3. **TIMED_OUT (2)**: Sender exceeded timeout deadline and returned `false` to caller. Slot is retained; cannot be recycled while remote cores still process it.
4. **RETIRED (3)**: All targets in `target_mask` have acknowledged (either during active wait or delayed drainage). Safe for sender to reclaim as FREE.

---

## 5. Lock Ownership & Re-Entrant Servicing Invariant Proof

- `smp_service_pending_tlb_requests(my_cpu)` is called from two contexts:
  1. Vector 251 IPI interrupt handler (`smp_handle_tlb_shootdown`).
  2. Sender spin-wait loop in `smp_tlb_shootdown()`, while holding `g_smp_tlb_queues[sender_cpu].lock`.
- **Proof of Zero Deadlock & Zero Re-Entrant Lock Acquisition**:
  - `smp_service_pending_tlb_requests()` accesses remote queues `g_smp_tlb_queues[s]` (where $s \neq my\_cpu$) using strictly lock-free atomic primitives (`__ATOMIC_SEQ_CST`).
  - It **never** acquires `g_smp_tlb_queues[s].lock`.
  - It **never** acquires `g_vmm_lock`, `g_pmm_lock`, or `g_sched_lock`.
  - It does **not** allocate memory, perform I/O, or execute blocking loops.
  - Therefore, nested invocation inside a sender wait loop holding `g_smp_tlb_queues[sender_cpu].lock` is provably deadlock-free and cannot create circular wait conditions between CPUs.

---

## 6. Phase AS Test Strategy & Required Markers

We will implement deterministic tests verifying:
1. `ASTRA_AS_QUEUE_LIFECYCLE_OK`: 4 simultaneous senders, 4 slots occupied, queue full rejection, slot reuse, and head/tail wraparound.
2. `ASTRA_AS_GENERATION_ACK_ATOMICS_OK`: Monotonic generation matching, duplicate ACK rejection, stale ACK rejection, unexpected CPU rejection, and wraparound safety.
3. `ASTRA_AS_TIMEDOUT_SLOT_DRAIN_OK`: Timed-out slot retention, delayed receiver drainage to `TLB_SLOT_RETIRED`, and recovery without reboot.
4. `ASTRA_AS_REENTRANT_SERVICING_OK`: Lock-free re-entrant servicing during wait loops without lock inversion or recursive stalls.
5. `ASTRA_AS_PTE_ROLLBACK_CONCURRENCY_OK`: `PTE_TRANSITION` flag protection, rollback on timeout without clobbering concurrent mappings, and rejection of concurrent unmaps.
6. `ASTRA_AS_END_TO_END_FAILURE_QUAL_OK`: Multi-core concurrent mapping/unmapping under injected queue full, dropped ACK, delayed IPI, and address-space destruction with zero frame leaks.

