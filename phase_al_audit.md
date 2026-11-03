# Phase AL Audit — AstraOS Preemptive Multitasking, Process Lifecycle & Scheduler Qualification

**Workspace:** `C:\Users\Akash\Documents\MyKernel`  
**Date:** October 31, 2026  
**Status:** AUDIT COMPLETE — IMPLEMENTATION & QUALIFICATION IN PROGRESS  

---

## 1. Executive Summary & Objective

The objective of **Phase AL** is to perform an independent audit and hardening of the AstraOS multitasking subsystem, timer-driven preemptive scheduling, process lifecycle, synchronization primitives, parent-child process management (`waitpid`), and scheduler stability under concurrent workloads.

Prior phases (notably Phase AK) proved hardware address-space isolation, non-executable user stacks, unmapped guard pages, supervisor write protection trapping (#PF Vector 14), and VFS descriptor ownership across independent processes. However, Phase AK's multi-process test executed user applications that either exited immediately or cooperatively yielded via `sys_yield()`.

Phase AL must decisively answer and demonstrate:
1. **Can a CPU-bound Ring 3 application that *never* yields be preempted by the timer interrupt to allow other processes to make concurrent progress?**
2. **Do blocking operations (`sys_sleep`, `waitpid`) properly transition processes between `RUNNING → BLOCKED → READY → RUNNING` without busy-spin polling?**
3. **Is `waitpid()` parent-child authorization strictly enforced, preserving exit statuses, reaping zombies exactly once, and handling early/delayed child exits?**
4. **Is SMP genuinely supported, or do multiple-core configurations merely boot on the Bootstrap Processor (BSP) while secondary cores idle?**

---

## 2. Architecture & Execution Path Audit

### 2.1 Timer Interrupt Path & IRQ Preemption Hook

The APIC timer interrupt path was traced through the kernel:
1. **Hardware Trigger:** Local APIC timer in periodic mode fires on Vector 32 (`APIC_TIMER_VECTOR`).
2. **Hardware State Push:** If interrupting Ring 3 (CPL=3), the CPU automatically switches to the kernel stack pointer stored in `TSS.rsp0` (`proc->thread->stack_top`) and pushes:
   - `SS` (User Data Selector `0x1B`)
   - `RSP` (User Stack Pointer)
   - `RFLAGS` (User RFLAGS with `IF=1`)
   - `CS` (User Code Selector `0x23`)
   - `RIP` (Interrupted User Instruction Pointer)
3. **Assembly Stub (`kernel/arch/x86_64/idt_asm.S`):**
   - `IRQ_STUB 32` pushes error code `0` and vector `32`.
   - `irq_common_stub` pushes all 15 general-purpose registers (`rax, rbx, rcx, rdx, rsi, rdi, rbp, r8..r15`).
   - Invokes `interrupt_handler(struct interrupt_frame *frame)`.
4. **Interrupt Handler (`kernel/arch/x86_64/idt.c`):**
   - Increments `g_interrupt_depth` and `g_irq_depth`.
   - On Vector 32: invokes `apic_timer_handler()`.
   - Inside `apic_timer_handler()`:
     - Increments `g_apic_ticks`.
     - Calls `sched_timer_tick(g_apic_ticks)`.
     - `sched_timer_tick` wakes any expired sleepers in `g_sleep_head` (moving them to `THREAD_READY` and enqueueing them onto `g_ready_head`).
     - If `sched_has_ready_threads()`: sets `g_current_thread->need_resched = true`.
     - Sends `apic_eoi()`.
   - Decrements `g_irq_depth` and `g_interrupt_depth` back to 0.
5. **IRQ Exit Preemption Hook (`sched_try_preempt_irq_exit`):**
   - In `irq_common_stub`, immediately following `interrupt_handler`, the stub passes the interrupted context's `RFLAGS` from the stack frame (`152(%rsp)`) to `sched_try_preempt_irq_exit(u64 interrupted_rflags)`.
   - Preemption Safety Guard verifies:
     - `g_sched_initialized && g_current_thread != NULL`
     - `g_interrupt_depth == 0` (guarantees we are not in a nested interrupt)
     - `preempt_count() == 0` (guarantees no preemption-disabled kernel spinlock or critical section is held)
     - `(interrupted_rflags & (1ULL << 9)) != 0` (guarantees interrupted context had `IF=1`)
     - `g_current_thread != &g_idle_thread`
     - `sched_has_ready_threads() == true`
     - `need_resched() == true`
   - If all conditions hold:
     - Re-enqueues `prev` as `THREAD_READY`.
     - Dequeues `next` from `g_ready_head` as `THREAD_RUNNING`.
     - Calls `sched_prepare_switch(next)` to update `TSS.rsp0` and switch `CR3` if `next->process->pml4_phys != 0`.
     - Calls `thread_switch_asm(&prev->saved_rsp, next->saved_rsp, next)` to swap stacks and restore incoming registers.

### 2.2 Address Space Symmetry & Context Switch Safety

- Every process address space created by `vmm_create_process_space` copies PML4 entries 256..511 directly from the kernel PML4 root.
- All kernel code, HHDM direct map, and thread kernel stacks allocated in the `0xffffffff80800000` region are mapped identically across all page directories.
- Switching `CR3` during `sched_prepare_switch` preserves the executing kernel stack pointer and kernel code pages without faulting.
- Restoring a preempted thread pops the callee-saved registers in `thread_switch_asm`, returns to `sched_try_preempt_irq_exit`, returns to `irq_common_stub`, pops all 15 GP registers, and executes `iretq`, perfectly restoring the user process's RIP, RSP, and RFLAGS.

---

## 3. Scheduler Evaluation: Cooperative vs. Preemptive vs. SMP

| Dimension | Architectural Finding | Audit Status |
|---|---|---|
| **Kernel Thread Preemption** | Tested in Phase C (`sched_run_preemption_tests`, Test 11). Spinners without yields are preempted by timer ticks. | **VERIFIED** |
| **Ring 3 User Preemption** | IRQ exit hook checks `interrupted_rflags & IF`. Ring 3 always runs with `IF=1`. When a CPU-bound Ring 3 process runs without yielding, timer ticks flag `need_resched`, triggering preemption in `irq_common_stub`. | **PATH VERIFIED (Needs Ring 3 App Proof)** |
| **SMP Concurrent Scheduling** | The Limine boot configuration in `kernel/boot/limine_reqs.c` does **not** register a `limine_smp_request`. No APIC INIT/STARTUP IPI sequence is implemented. AstraOS runs strictly on the Bootstrap Processor (BSP, CPU 0). Multi-core QEMU flags (`-smp 2`, `-smp 4`) boot because secondary cores remain sleeping in UEFI/firmware idle. True multi-core parallel scheduling is **NOT IMPLEMENTED**. | **ONE CPU ONLY (SMP BLOCKED/UNSUPPORTED)** |

---

## 4. Audit Findings & Identified Defects

### Defect 1: `process_waitpid` Busy-Spin Polling Instead of True Blocking
- **Current Behavior:** In `kernel/process/process.c`, when a parent calls `process_waitpid` on a child that has not yet exited, it executes `thread_yield()` in a tight loop.
- **Problem:** This keeps the parent thread `THREAD_READY` / `THREAD_RUNNING`, burning CPU cycles and failing the Task 3 requirement:
  `RUNNING → BLOCKED → READY → RUNNING` for a process waiting on a child.
- **Remedy:** Equip `process_t` with an event primitive (`event_t child_exit_ev`). When `process_waitpid` finds no dead child, it blocks via `event_wait(&curr->child_exit_ev)`. When any child calls `process_exit()`, it signals its parent's `child_exit_ev`, transitioning the parent from `THREAD_BLOCKED` to `THREAD_READY`.

### Defect 2: Missing Parent Authorization Check on Positive PID Wait
- **Current Behavior:** In `process_waitpid`:
  ```c
  if (pid > 0 && p->pid != pid) continue;
  if (pid == (pid_t)-1 && parent != 0 && p->parent_pid != parent) continue;
  ```
- **Problem:** When `pid > 0`, it does *not* verify that `p->parent_pid == parent`. A process could call `waitpid(target_pid)` on a process that belongs to another parent and reap it.
- **Remedy:** Strictly verify `if (pid > 0) { if (p->pid != pid) continue; if (parent != 0 && p->parent_pid != parent) return -ECHILD; }`.

### Defect 3: Invalid PID Input Handling
- **Current Behavior:** If `pid < -1` or `pid == 0`, `process_waitpid` treats the process matching condition as true for all processes because both `if (pid > 0)` and `if (pid == -1)` are skipped.
- **Remedy:** Reject `pid < -1` or `pid == 0` immediately with `-EINVAL` / `-ECHILD`.

### Defect 4: Premature Process State Transition in `process_exit`
- **Current Behavior:** In `process_exit()`, `proc->state` is set to `PROCESS_ZOMBIE`, and then immediately to `PROCESS_DEAD`.
- **Problem:** If `process_reap_zombies()` is called before the parent reaps the child, the child's slot is cleared to `PROCESS_UNUSED` and the exit code is lost.
- **Remedy:** Keep child in `PROCESS_ZOMBIE` until explicitly reaped by its parent via `process_waitpid()`. If the parent exits first (orphaned child), reparent all children to PID 0 (kernel/init) so that system cleanup can reclaim them.

### Defect 5: Missing User-Space Syscall Stubs
- **Current Behavior:** `user/user_entry.S` lacks assembly wrappers for `sys_waitpid` (SYS 36) and `sys_sleep` (SYS 20).
- **Remedy:** Add `sys_waitpid` and `sys_sleep` wrappers in `user/user_entry.S` so Ring 3 applications can invoke them directly.

---

## 5. Ring 3 Application Workloads for Qualification

To satisfy Tasks 1 and 2, five independent user ELF applications will be built and packaged in `/bin/` in `build/initramfs.bin`:

1. **`cpu_worker.elf` (`user/cpu_worker.c`):**
   - Executes a bounded, CPU-intensive arithmetic loop ($5 \times 10^7$ iterations).
   - Deliberately does **not** call `sys_yield()`.
   - Exits cleanly with status 0 upon completion.

2. **`heartbeat.elf` (`user/heartbeat.c`):**
   - Loops through 10 iterations, printing progress markers (`[HEARTBEAT] Tick N`) and briefly sleeping or computing.
   - Proves preemptive concurrency: must interleave its output while `cpu_worker` is running without yielding.

3. **`blocking_worker.elf` (`user/blocking_worker.c`):**
   - Spawns/calls `sys_sleep(50)` (SYS 20) and waits for a child process via `sys_waitpid(child_pid, &status, 0)` (SYS 36).
   - Validates that the process blocks (`PROCESS_BLOCKED` / `THREAD_BLOCKED`) and yields the CPU until the sleep deadline or child exit event occurs.

4. **`faulting_app.elf` (`user/faulting_app.c`):**
   - Deliberately attempts to read higher-half kernel memory (`0xffffffff80400000`).
   - Trapped by `#PF` Vector 14; kernel safely terminates it with status -11 while normal applications continue running.

5. **`normal_app.elf` (`user/normal_app.c`):**
   - Arithmetic verification (sum 1..100 = 5050), VFS descriptor write/close lifecycle, exits with status 0.

---

## 6. Acceptance Criteria

- [ ] **Task 1: Preemption Acceptance Criterion:** While `cpu_worker.elf` is running in a tight CPU-bound loop without `sys_yield()`, `heartbeat.elf` must make continuous forward progress and emit all 10 heartbeat ticks.
- [ ] **Task 2: Independent Process Execution:** 5 distinct ELF applications run with separate PIDs, distinct PML4s, independent user stacks, and isolated memory.
- [ ] **Task 3: Blocking & Wakeup State Transitions:** Verify `RUNNING → BLOCKED → READY → RUNNING` on both `sys_sleep()` and `sys_waitpid()` with zero lost wakeups and zero deadlocks.
- [ ] **Task 4: `waitpid()` Correctness:**
  - Unauthorized parent waiting on another process's child rejected with `-ECHILD`.
  - Child exit before parent wait: exit status preserved in zombie, reaped exactly once.
  - Parent wait before child exit: parent blocked on `child_exit_ev`, awakened on exit.
  - Repeated wait on reaped child returns `-ECHILD`.
  - Orphaned children reparented without leaking slots.
- [ ] **Task 5: Process Cleanup & Frame Accounting:** 10 repeated create/destroy cycles under preemptive scheduling with zero leaked PMM frames (`before == after`).
- [ ] **Task 6: Scheduler Fairness & Stability:** Bounded stress tests report context switch counts, tick counts, and zero starvation incidents.
- [ ] **Task 7: SMP Status:** Document single-CPU execution (BSP only) honestly and accurately; mark SMP scheduling as NOT IMPLEMENTED.
- [ ] **Task 8: Regression Matrix:** All 8 host test suites pass; QEMU `nofb` passes; QEMU `normal` passes; full 16-config matrix passes; VirtualBox 7 EFI headless passes.

