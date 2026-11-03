# AstraOS Boot Performance Baseline Measurement & Root-Cause Investigation

**Phase 1 Deliverable**  
**Date:** 2026-11-03  
**Target Platform:** Oracle VirtualBox 7.0 (EFI Enabled, 1 vCPU, 512 MB RAM, IntelAHCI SATA, E1000 NAT)  
**Host Environment:** Windows x86_64, VBoxManage CLI  
**Baseline Test Script:** `tests/measure_boot_timing.py`  
**Raw Trace File:** `boot_timing.log`

---

## 1. Executive Summary

A rigorous, end-to-end empirical timing measurement of AstraOS v1.0 was conducted using Oracle VirtualBox 7 with UEFI firmware. High-resolution timestamps (`time.perf_counter()`, sub-millisecond precision) were captured from the moment `VBoxManage startvm --type headless` was dispatched until the kernel monitor prompt was published.

### Key Metrics
- **Total VM Start to Interactive Usable Desktop:** **418,159.7 ms** (**418.16 seconds / 6 minutes 58 seconds**)
- **VirtualBox UEFI Firmware & Limine Handshake:** 18,068.8 ms (18.07 s)
- **Kernel Hardware & Subsystem Bootstrap:** 15,178.3 ms (15.18 s)
- **In-Kernel Cumulative Test Suites (Phases C through AY, L, K, O):** **384,912.6 ms** (**384.91 s / 92.05% of total boot time**)

---

## 2. 16-Milestone Empirical Timing Breakdown

The table below records the exact elapsed time from VM start ($T_0$) to the detection of each milestone marker on COM1 serial, along with the duration ($\Delta$) between milestones:

| Milestone Index | Milestone Description | Marker / Trigger String | Elapsed (ms) | Stage Delta (ms) | % of Total |
|:---:|:---|:---|---:|---:|---:|
| **1** | **VM Start** | `VBoxManage startvm` dispatch | 0.0 | 0.0 | 0.00% |
| **2** | **Bootloader Entry** | Limine EFI bootloader banner | 18,068.8 | 18,068.8 | 4.32% |
| **3** | **Kernel Entry** | `AstraOS Kernel v1.0.0` banner | 18,068.8 | 0.0 | 0.00% |
| **4** | **Physical Memory (PMM)** | `ASTRA_PMM_OK` | 22,729.3 | 4,660.5 | 1.11% |
| **5** | **Virtual Memory (VMM)** | `ASTRA_OWN_PAGING_OK` | 26,683.6 | 3,954.3 | 0.95% |
| **6** | **ACPI & Hardware Discovery** | `[BOOT] GDT reloaded` | 18,068.8 | *(concurrent)* | — |
| **7** | **Interrupts & APIC Timer** | `ASTRA_APIC_TIMER_OK` | 30,419.9 | 3,736.3 | 0.89% |
| **8** | **SMP Bring-Up** | `ASTRA_SMP_SINGLE_CPU_OK` | 31,328.4 | 908.5 | 0.22% |
| **9** | **Scheduler Ready** | `ASTRA_COOP_SCHED_OK` | 33,247.1 | 1,918.7 | 0.46% |
| **10** | **VFS Mount Completion** | `ASTRA_VFS_LOOKUP_OK` | 64,416.0 | 31,168.9 | 7.45% |
| **11** | **GUI Subsystem Init** | `ASTRA_GRAPHICS_OK` | 151,175.6 | 86,759.6 | 20.75% |
| **12** | **Window Manager Ready** | `ASTRA_WINDOW_MANAGER_OK` | 157,026.7 | 5,851.1 | 1.40% |
| **13** | **Desktop Creation** | `ASTRA_DESKTOP_OK` | 158,147.4 | 1,120.7 | 0.27% |
| **14** | **Network Initialization** | `ASTRA_NET_DRIVER_OK` | 174,867.5 | 16,720.1 | 4.00% |
| **15** | **Storage Initialization** | `ASTRA_BLOCK_DEVICE_OK` | 244,249.1 | 69,381.6 | 16.59% |
| **16** | **Desktop Fully Interactive** | `AstraOS Kernel Monitor` / Input Loop | 418,159.7 | 173,910.6 | 41.59% |

*(Note: Milestones are sorted by chronological occurrence in the baseline kernel).*

---

## 3. Top 5 Startup Bottlenecks Identified

From the measured empirical data, the five most expensive operations during boot are:

1. **SMP, TLB Concurrency & Memory Reclamation Qualification (Phases AN through AW): 132.8 seconds**  
   - 10 distinct multi-core stress suites run in sequence on CPU 0.
   - Includes simulated delayed IPI timeouts, queue exhaustion loops, page table transition stress, and lock hierarchy checks.

2. **Preemptive Multitasking & User-Space Process Stress (Phases AK, AL, AM): 86.8 seconds**  
   - Spawns over 50 Ring 3 processes sequentially (`normal_app`, `cpu_worker_a`, `cpu_worker_b`, `heartbeat`, `blocking_worker`, `smp_stress`).
   - In VirtualBox without nested hardware virtualization, each Ring 3 entry, page fault interception, and context switch incurs expensive software emulation overhead.

3. **Storage, Filesystem Integrity & Fault-Injection Tests (Phases AD, AX, AY, AstraFS): 71.4 seconds**  
   - ATA PIO polling loops (`ata_wait_bsy`, `ata_wait_drq`) with 200,000 cycle limits.
   - Simulated crash recovery, precommit journal flushes, and repeated sector verification.

4. **Network Protocol Stack & Timeout Loops (Phase J): 52.6 seconds**  
   - DHCP discovery, DNS resolution, TCP three-way handshake, and HTTP fetch loops.
   - Polling loops in `dhcp.c`, `dns.c`, and `tcp.c` executed `for (volatile int d = 0; d < 100; d++) io_wait();`. Each `io_wait()` performs `outb(0x80, 0)`, which triggers a VM exit in VirtualBox. With 50,000 loops, this caused 5,000,000 VM exits per timeout.

5. **Browser Engine, DOM, JS Runtime & User Browser Tests (Phases K & O): 41.3 seconds**  
   - Full CSS parser cascade tests, HTML tokenizer, layout engine, and two user-space browser process executions (PID 74 and PID 75).

---

## 4. Root-Cause Analysis of the 5 Primary Problems

### Problem 1: VirtualBox Startup Takes 5–6 Minutes
- **Root Cause:** All cumulative qualification test suites (Phases C through AY, Crypto, TLS, Browser) are hardcoded directly inside `kernel/kernel.c` (`kmain()` and `kernel_continuation()`). Under VirtualBox on a single core with software CPU emulation, running 75 user processes, RSA-2048 modular exponentiations, and million-iteration VM-exit I/O polling loops consumes ~385 seconds.

### Problem 2: Desktop Appears in the Middle of Boot Execution
- **Root Cause:** In `kernel/kernel.c`, GUI and Desktop initialization occurs at Step 16 (lines 466–497, ~158s), halfway through boot. Following Step 16, Steps 17 (Network), 17b (Block/ATA), 17b2–17b5 (Phases AN–AW), 17c (AstraFS), 17d (Phase AX), 17e (Phase AY), 18 (Crypto/TLS), and 19 (Browser) execute for another 260 seconds while the desktop is frozen on screen.

### Problem 3: Final Desktop is Sometimes Incomplete or Partially Initialized
- **Root Cause:** The desktop was initialized before network, block storage, and user sessions were established. Mid-boot test processes (`hello_gui`, `counter_gui`, `user_browser`) opened and closed test windows directly on the active desktop, modifying the window list and focus state during boot.

### Problem 4: Applications Open Automatically Without User Request
- **Root Cause:** `kernel/kernel.c` explicitly invoked:
  - Line 489: `app_editor_launch("/hello.txt");`
  - Line 490: `app_filemgr_launch("/");`
  - Line 684: `app_browser_launch("file:///www/index.html");`
  These applications were unconditionally launched during kernel boot.

### Problem 5: Desktop/Session Startup is Inconsistent
- **Root Cause:** Lack of a synchronized lifecycle state machine. `desktop_render()` was called repeatedly (in `desktop_self_test()`, in Step 16, and in Step 19), redrawing before all drivers and input event queues were stable.

---

## 5. Optimization & Stabilization Strategy (Phases 2–5)

1. **Boot Path Separation (Interactive vs Qualification Mode):**
   - In normal boot (default for user and performance target), execute only essential hardware and OS bring-up: GDT -> IDT -> PMM -> VMM -> TSS -> Heap -> APIC/Timer -> Sched -> SMP -> PCI -> E1000 (async/non-blocking) -> Block/ATA -> VFS -> AstraFS -> GUI -> WM -> Desktop -> Input Loop.
   - When cmdline contains `test` or `test=all` (used by automated regression test runners), run the qualification suites.
2. **Eliminate Bounded I/O Polling Overhead:**
   - In ATA, DHCP, DNS, and TCP, replace `io_wait()` busy loops with APIC timer tick based delays (`thread_sleep_ticks` or APIC tick differential) to avoid millions of VirtualBox VM exits.
3. **Formal Desktop Lifecycle State Machine:**
   - Enforce: `BOOTING` (0) -> `KERNEL_READY` (1) -> `GUI_INITIALIZING` (2) -> `DESKTOP_READY` (3).
   - Ensure `gfx_init()` and `wm_init()` run exactly once.
   - Ensure input events are processed and verified before emitting `ASTRA_DESKTOP_READY_OK`.
4. **Clean Default User Session:**
   - Remove automatic calls to `app_editor_launch`, `app_filemgr_launch`, and `app_browser_launch`.
   - Desktop displays only background wallpaper, taskbar, start launcher, clock/RAM monitor, and cursor.
   - Applications launch exclusively upon explicit user click or action.

