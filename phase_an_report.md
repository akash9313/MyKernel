# Phase AN Engineering Report — AstraOS SMP Memory Safety, TLB Shootdown & Concurrency Hardening

**Date:** October 31, 2026  
**Status:** COMPLETE  
**Workspace:** `C:\Users\Akash\Documents\MyKernel`

---

## 1. Executive Summary

Phase AN independently audited and hardened the Symmetric Multiprocessing (SMP) subsystem introduced in Phase AM. Phase AM successfully achieved multi-core bring-up and parallel user-space execution across 1, 2, 4, and 8 CPUs. However, the Phase AN audit identified critical memory-safety and synchronization hazards under multi-core concurrency:
- **Lack of Cross-CPU TLB Invalidation**: Page table unmaps and remappings on one CPU were not propagated to remote CPUs, causing stale TLB translations and potential use-after-free vulnerabilities.
- **Unsynchronized Page Table Modifications**: VMM operations (`vmm_map_page`, `vmm_unmap_page`, page directory allocation) lacked mutual exclusion across cores.
- **Process Teardown Races**: Physical memory frames and page directory tables could be freed while remote APs were still executing with the victim process's `CR3` active.
- **Context Switch Stack Races**: The `is_executing` flag in `switch.S` was cleared prior to switching the hardware stack pointer (`%rsp`), creating a window where another core could pick up the thread and corrupt its active kernel stack.
- **ATA Hardware Driver Interleaving**: Concurrent file operations on multiple CPUs issued interleaved commands to shared IDE I/O ports (`0x1F0`–`0x1F7`).
- **High-Frequency MMIO Contention**: Reading the Local APIC ID from MMIO (`0xFFFFFFFFB0000020`) on every spinlock, preemption count, and `thread_current()` check introduced severe MMIO bus stalls under multi-core loads.

Phase AN completely resolved all these issues. AstraOS now features a hardware-accelerated Local APIC IPI TLB shootdown engine (IDT Vector 251), page table synchronization with `g_vmm_lock`, per-process `active_cpu_mask` quiescence tracking during teardown, context switch stack isolation, hardware-based per-CPU state via `IA32_GS_BASE` (MSR `0xC0000101`), and ATA hardware serialization (`g_ata_lock`).

All five Phase AN qualification markers passed deterministically across the full test matrix (QEMU `-smp 1, 2, 4, 8` in `normal` and `nofb` modes, 7 host test suites, and Oracle VirtualBox 7 EFI).

---

## 2. Key Architecture & Implementation Details

### 2.1 Local APIC IPI TLB Shootdown Protocol (Vector 251)
- **IDT Vector & Handler**:
  - IDT vector 251 (`APIC_TLB_IPI_VECTOR`) configured as a 64-bit Interrupt Gate (DPL=0, IST=0).
  - Low-level assembly stub `tlb_ipi_stub` in `kernel/arch/x86_64/idt_asm.S` preserves caller registers and invokes `smp_handle_tlb_shootdown()`, followed by Local APIC EOI (`apic_eoi()`) and `iretq`.
- **IPI Transmission**:
  - `apic_send_ipi_raw(dest_lapic_id, vector)` and `apic_broadcast_ipi(vector)` write to `APIC_REG_ICR_HIGH` and `APIC_REG_ICR_LOW` (`0x00080000 | vector` for All-Excluding-Self shorthand).
- **Deadlock-Free Request Protocol (`smp_tlb_shootdown`)**:
  - State struct `g_tlb_req` maintains `lock`, `target_mask`, `cr3`, `va`, and `ack_count`.
  - **In-Flight Servicing**: When CPU A holds `g_tlb_req.lock` and sends an IPI to CPU B while CPU B is trying to acquire `g_tlb_req.lock` with IRQs disabled (`spinlock_acquire_irqsave`), CPU B cannot service vector 251 directly. To eliminate this lock convoy/deadlock, `smp_tlb_shootdown()` inspects `(g_tlb_req.target_mask & (1U << cur_cpu))` and directly executes `smp_handle_tlb_shootdown()` while spinning on `spinlock_try_acquire(&g_tlb_req.lock)`.

### 2.2 VMM Synchronization & Page Table Protection
- **`g_vmm_lock` Spinlock**:
  - All page table allocation, mapping, and unmapping (`vmm_map_page`, `vmm_unmap_page`, `vmm_alloc_page_table`) serialize under `g_vmm_lock`.
  - Leaf PTEs are zeroed under lock; `g_vmm_lock` is released *before* calling `smp_tlb_shootdown()` to prevent priority inversions and deadlock with remote CPUs servicing shootdowns.

### 2.3 Process Address-Space Teardown & Quiescence Protocol
- **`proc->active_cpu_mask`**:
  - Atomic bitmask recording every CPU where `proc->pml4_phys` is active in `CR3`.
  - Updated upon context switch when loading a user PML4 or reverting to the kernel PML4.
- **Teardown Quiescence**:
  - During process destruction/reaping, the kernel checks `proc->active_cpu_mask`. If any remote CPU still has the process active, the reaper sends a TLB shootdown for `proc->pml4_phys` and spins until `proc->active_cpu_mask == 0` before freeing physical memory frames or page directory tables.

### 2.4 Hardware-Accelerated Per-CPU State (`IA32_GS_BASE`)
- **MSR `0xC0000101`**:
  - Set during early startup for BSP and all APs to point directly to `&g_cpus[cpu_id]`.
  - Offset 0 of `cpu_state_t` holds `self`.
  - `smp_cur_cpu()` resolves the current core in a single assembly instruction: `movq %gs:0, %rax`.
  - Eliminates MMIO bus contention on `APIC_REG_ID`.
  - Segment register preservation: User transitions (`user_jump_to_ring3`) preserve `%gs` instead of reloading it with user data segment selectors, maintaining per-CPU base integrity across syscalls and interrupts.

### 2.5 Low-Level Context Switch Stack Exclusivity (`switch.S`)
- **Stack Collision Elimination**:
  - In `switch.S`, `prev->is_executing = 0` is cleared strictly *after* switching the hardware stack pointer (`movq %rsi, %rsp`).
  - Remote cores searching the ready queue will never resume `prev` while the retiring core's stack frame is still being manipulated.
  - Multi-core clobber of global `g_current_thread` removed in favor of per-CPU `current_thread` pointers.

### 2.6 ATA Hardware Driver Serialization
- **`g_ata_lock` Spinlock**:
  - All ATA/IDE sector reads, writes, and cache flushes in `kernel/drivers/ata.c` serialize under `g_ata_lock` with `irqsave`.
  - Protects command and control registers from multi-core command interleaving.

---

## 3. Phase AN Qualification Markers

| Marker | Verification Subject | Result |
| :--- | :--- | :--- |
| `ASTRA_SMP_TLB_SHOOTDOWN_OK` | Cross-CPU TLB invalidation via Local APIC IPI 251 broadcast and acknowledgement | **PASS** |
| `ASTRA_SMP_PROCESS_TEARDOWN_OK` | Process termination, `active_cpu_mask` quiescence, and safe page table reclamation | **PASS** |
| `ASTRA_SMP_MEMORY_STRESS_OK` | Multi-core concurrent PMM frame allocations and zero physical memory leaks | **PASS** |
| `ASTRA_SMP_SCHEDULER_STRESS_OK` | Multi-core preemption exclusivity, zero stack collision, and balanced thread dispatch | **PASS** |
| `ASTRA_SMP_STORAGE_STRESS_OK` | Concurrent ATA disk writes and metadata updates under `g_ata_lock` without corruption | **PASS** |

---

## 4. Test Matrix & Verification Results

### 4.1 QEMU Multi-Core Integration Matrix
All runs executed with `--ram 512M` and the complete test suite (Boot, PMM, VMM, TSS, IDT, Heap, APIC, Context Switch, Cooperative Sched, Thread Lifecycle, Wait Queues, Events, Sleep, Idle Wake, Preemption Guard, Critical Sections, Preemption, Mutexes, Semaphores, User Mode, Syscalls, ELF Loader, VFS/Initramfs, Ring 3 Preemption, Multi-Process Concurrency, Blocking/Wakeup, waitpid Lifecycle, Process Cleanup, Scheduler Fairness, SMP Audit, SMP Parallel Execution, SMP Multi-Core Lifecycle, Framebuffer, Graphics, Input, Window Manager, Desktop, GUI Apps, User GUI, Multi-Process GUI, GUI Isolation, PCI Net Device, E1000 Driver, ARP, IPv4, DHCP, DNS, TCP, HTTP Client, Block Device, AstraFS, Cold Reboot Persistence, Cryptography, TLS 1.3, CSS Engine, Browser Render, Browser HTTP, Mmap, Browser Images, Ext CSS, Forms, Navigation, Scrolling, JS Feasibility/Runtime/Isolation/Hardening, DOM Bridge/Events, Interactive JS, Adversarial JS, Browser Security, Browser Network Isolation, HTTP Hardening, Browser UX, Browser Reliability, Tabs & Bookmarks, User Browser Exec, Browser Isolation, and all 5 Phase AN SMP markers):

- **`-smp 1 --mode normal`**: **PASS** (100% of all test markers).
- **`-smp 2 --mode normal`**: **PASS** (100% of all test markers, 2 CPUs online).
- **`-smp 4 --mode normal`**: **PASS** (100% of all test markers, 4 CPUs online).
- **`-smp 8 --mode normal`**: **PASS** (100% of all test markers, 8 CPUs online, monitor commands verified).
- **`-smp 4 --mode nofb`**: **PASS** (100% of all headless test markers, 4 CPUs online).

### 4.2 Host Unit Test Suites
- `tests/test_crypto_host.py`: **PASS** (SHA-256, HMAC, HKDF, ChaCha20-Poly1305, X25519, X.509 cert validation).
- `tests/test_vfs_host.py`: **PASS** (Archive validation, lookup, pread, stat, readdir, mutable RAMFS).
- `tests/test_block_host.py`: **PASS** (20/20 block device & ATA flush host tests).
- `tests/test_fs_host.py`: **PASS** (27/27 AstraFS persistent filesystem host tests).
- `tests/test_elf_host.py`: **PASS** (16/16 ELF64 security, bounds, alignment, W^X, and allocation tests).
- `tests/test_bookmark_storage.py`: **PASS** (16/16 browser bookmark persistence tests).
- `tests/test_browser_networking.py`: **PASS** (17/17 adversarial networking and stress tests).

### 4.3 Oracle VirtualBox 7 EFI Boot
- `python scripts/test_vbox.py`: **PASS**
  - **93 / 93** boot markers verified in headless EFI VM session (`ASTRA_BOOT_OK` through `ASTRA_BROWSER_ISOLATION_OK`).
  - Desktop screenshot captured successfully.

---

## 5. Artifacts and Documentation Integrity
- `phase_an_audit.md`: Audited all Phase AM implementations and identified cross-CPU memory hazards and synchronization requirements.
- `docs/architecture.md`: Updated with Section 50 detailing SMP Memory Safety, TLB Shootdown protocol (Vector 251), page table synchronization (`g_vmm_lock`), process address-space teardown protocol (`active_cpu_mask`), hardware `IA32_GS_BASE` per-CPU lookup, and ATA storage serialization (`g_ata_lock`).
- `docs/status.md`: Updated with Phase AN status, architecture entries, and test matrix results.
- `phase_an_report.md`: Persisted in workspace root and copied to artifacts directory.

