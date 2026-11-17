# AstraOS — AstraFiles 3.0: Final Engineering & Verification Report

**Project:** AstraOS Kernel v1.0.0 (x86-64 UEFI/Limine)  
**System Milestone:** Production-Quality File Manager & Robust Storage Lifecycle  
**Primary Deliverable:** AstraFiles 3.0 Native Desktop Application (`kernel/gui/app_filemgr.c`)  
**Target Architecture:** x86-64 UEFI (Limine Bootloader v0.5)  
**Test Platform:** Oracle VirtualBox 7.x at $1920 \times 1080 \times 32\text{ bpp}$  
**Date:** November 17, 2026  
**Status:** **COMPLETE & FULLY VERIFIED (100% Pass)**

---

## 1. Executive Summary

This engineering project resolved the critical user-visible bug in AstraFiles where the **Delete button failed to reliably delete selected files or folders**. In addition to resolving all underlying deletion failure mechanisms, the application was comprehensively overhauled into **AstraFiles 3.0**, a professional desktop file manager adhering to the Cosmic Vista dark aesthetic.

The newly deployed architecture features:
1. **A Reliable Two-Pass Recursive Deletion Backend:** Completely eliminates readdir index-skipping and recursively deletes nested child folders and files with zero orphan entries.
2. **True Multi-Selection Engine:** Supports standard single click, `Ctrl+Click` toggle, `Shift+Click` range selection, and `Ctrl+A` select-all, with active checkbox indicators and visual row highlights.
3. **Cosmic Vista Dark Theme UI:** $780 \times 480\text{ px}$ responsive window with a top navigation bar (history, breadcrumbs, search filter), action toolbar with an ergonomic 68px Rose Delete button, category sidebar with Favorites and persistent AstraFS storage telemetry, and main file table with colored file-type badges (`[DIR]`, `[C]`, `[PY]`, `[SH]`, `[TXT]`, `[ELF]`, `[FILE]`).
4. **Context Menus:** 8 options on items, 6 options on blank canvas, fully clamped within window bounds with event consumption to prevent click-through hazards.
5. **Zero Heap Overhead:** Dynamic allocations utilize physical page frames (`pmm_alloc_contiguous()`), maintaining zero impact on the 64 KiB kernel heap and preserving **33.88 KiB** of headroom below `0xffffffff80400000` (exceeding the $> 20\text{ KiB}$ requirement).
6. **Empirical Verification in VirtualBox EFI:** Tested across 7 acceptance test scenarios, cold-reboot persistence, and full system regression test suite (100% pass rate).

---

## 2. Root Cause Analysis & Deletion Defect Resolution

A comprehensive audit of AstraFiles 2.0 uncovered seven distinct failure points that collectively caused deletion failures in the GUI:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              FAILURE CHAIN                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│ 1. Recursive Readdir Index-Skipping:                                        │
│    vfs_readdir(path, idx++, ...) while unlinking caused remaining entries  │
│    to shift forward, skipping every second file/folder. Non-empty folders   │
│    were never emptied, causing vfs_rmdir() to fail with VFS_ERR_BUSY (-16). │
├─────────────────────────────────────────────────────────────────────────────┤
│ 2. Toolbar Hitbox Truncation:                                               │
│    Hit-test bound for Delete button was 54px wide, while text label was     │
│    56px wide. Clicks on the right half of the button fell outside bounds.   │
├─────────────────────────────────────────────────────────────────────────────┤
│ 3. Click vs Double-Click Collisions:                                        │
│    Absence of an APIC tick interval threshold allowed rapid selection       │
│    clicks to mistakenly trigger open_item, navigating into the folder       │
│    instead of staging it for deletion.                                      │
├─────────────────────────────────────────────────────────────────────────────┤
│ 4. Single-Item vs Multi-Item Disconnect:                                    │
│    Delete action operated solely on selected_idx (single integer), ignoring │
│    multi-selection flags.                                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│ 5. Context Menu Dismissal Click-Through:                                    │
│    Dismissing context menus outside their bounding box failed to consume     │
│    the event, triggering unwanted clicks on the underlying canvas.          │
├─────────────────────────────────────────────────────────────────────────────┤
│ 6. Parent Directory '..' Deletion Hazard:                                   │
│    Clicking or selecting '..' could attempt vfs_unlink(".."), resulting in  │
│    critical VFS path validation failures.                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│ 7. Silent Failure Reporting:                                                │
│    GUI reported "Item deleted" even when underlying VFS returned errors,    │
│    masking persistent storage failures from user feedback.                  │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Complete Bug Report Reference
The detailed technical diagnosis is documented in:
[astrafiles_delete_bug_report.md](file:///c:/Users/Akash/Documents/MyKernel/astrafiles_delete_bug_report.md)

---

## 3. AstraFiles 3.0 Architecture & Design Specifications

### 3.1 Window Layout Hierarchy ($780 \times 480\text{ px}$)

AstraFiles 3.0 introduces a modular, ergonomic visual structure:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ AstraOS Files 3.0 (AstraFS)                                      [-] [□] [x] │
├─────────────────────────────────────────────────────────────────────────────┤
│ [<] [>] [^] [H] [R]  [Root > data > projects               ] [Search...   x] │ Top Nav Bar (34px)
├─────────────────────────────────────────────────────────────────────────────┤
│ [+ File] [+ Folder] [Cut] [Copy] [Paste] [Rename] [Delete] [Sort:N] [Select]│ Action Bar (32px)
├───────────────┬─────────────────────────────────────────────────────────────┤
│ FAVORITES     │ [ ] [DIR]  documents       Folder               --        ▲ │
│  Root (/)     │ [ ] [DIR]  projects        Folder               --        █ │
│  Storage      │ [x] [C  ]  main.c          C Source File        2.4 KB    █ │ Main File Table
│  Binaries     │ [x] [TXT]  notes.txt       Text Document        512 B     █ │ (Details/List)
│  Web (/www)   │ [ ] [PY ]  script.py       Python Script        1.1 KB    █ │
│  Config       │ [ ] [ELF]  app.elf         Executable Binary    14.2 KB   ▼ │
│               ├─────────────────────────────────────────────────────────────┤
│ STORAGE       │                                                             │
│  AstraFS ATA  │                                                             │
│  [██████░░░░] │                                                             │
│  31MB free    │                                                             │
├───────────────┴─────────────────────────────────────────────────────────────┤
│ 6 items (2 selected) | Ready. Right-click for options.  AstraFS: 31MB free  │ Status Bar (24px)
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 Key Subsystems Implemented

1. **Top Navigation Bar:**
   - History Navigation: Back `[<]`, Forward `[>]`, Up `[^]`, Home `[H]`, Refresh `[R]`.
   - Interactive Breadcrumb Bar: Parses path segments into clickable navigation targets (e.g., clicking `Root` jumps to `/`, clicking `data` jumps to `/data`).
   - Real-Time Search Filter: Filters displayed entries by case-insensitive substring matching with a dedicated `[x]` clear button.
2. **Action Toolbar:**
   - Modern pill buttons with dedicated color accents: `+ File` (Teal), `+ Folder` (Amber), `Delete` (Rose Red), `Sort:N/S/T` (Sky Blue), `View:D/I` (Indigo), `Select All`.
   - Generous 68px wide hitbox for `Delete` ($402 \le \text{rel\_x} \le 470$), eliminating miss-clicks.
3. **Category Sidebar:**
   - Quick Favorites: `Root (/)`, `Storage (/data)`, `Binaries (/bin)`, `Web (/www)`, `Config (/etc)`.
   - Live AstraFS Telemetry Card: Shows storage mount name, visual usage progress bar, and real available space computed directly from `vfs_statfs()`.
4. **Main File Table:**
   - Distinct file-type badges: `[DIR]` (Sky), `[C]` / `[C++]` (Emerald), `[PY]` (Amber), `[SH]` (Teal), `[TXT]` / `[MD]` (Slate), `[ELF]` (Purple), `[FILE]` (Blue).
   - Selection checkboxes and high-contrast cobalt blue row highlights.
   - Column headers: `NAME`, `TYPE`, `SIZE` with sorting by Name, Size, or File Type.
5. **Context Menu Subsystem:**
   - Item Context Menu: 8 standard desktop actions (`Open`, `Open With AstraEdit`, `Cut`, `Copy`, `Paste`, `Rename`, `Delete`, `Properties`).
   - Canvas Context Menu: 6 actions (`New File`, `New Folder`, `Paste`, `Select All`, `Refresh`, `Properties`).
   - Strict window-edge clamping prevents menu clipping off-screen.
   - Event consumption ensures clicking outside to dismiss never accidentally clicks underlying canvas items.
6. **Two-Pass Non-Skipping Recursive Deletion Backend:**
   - Pass 1 gathers child file/folder names into a temporary 4-page PMM contiguous buffer (`pmm_alloc_contiguous()`).
   - Pass 2 recursively deletes every child from leaf to root before unlinking the directory itself via `vfs_rmdir()`.
   - Never touches the 64 KiB kernel heap.
   - Root mounts (`/`, `/data`, `/bin`, `/etc`, etc.) and parent directory links (`..`) are strictly protected.

### Architecture Document Reference
The complete UI specification and component layout is documented in:
[astrafiles_3_ui_architecture.md](file:///c:/Users/Akash/Documents/MyKernel/astrafiles_3_ui_architecture.md)

---

## 4. Engineering Implementation & Headroom Management

### 4.1 Modified Kernel Source Modules

| Module | Primary Modification |
| :--- | :--- |
| `kernel/gui/app_filemgr.c` | Complete re-architecture of AstraFiles 3.0: two-pass recursive deletion backend, multi-selection engine, Cosmic Vista dark UI, context menus, search filter, PMM dynamic buffer management. |
| `kernel/include/kernel/input.h` | Added `u8 modifiers;` to `struct mouse` inside `input_event_t` union so mouse events cleanly carry keyboard modifiers (`Ctrl`, `Shift`, `Alt`). |
| `kernel/drivers/ps2.c` | Updated mouse event generation in PS/2 ISR to populate `ev.mouse.modifiers` from `g_shift`, `g_ctrl`, and `g_alt`. |
| `kernel/browser/browser_url.c` | Consolidated duplicate 16 KiB static buffers (`raw_resp`) into single shared file-scope buffer `s_browser_raw_resp`, saving **16,384 bytes in `.bss`**. |
| `kernel/monitor/monitor.c` | Added `click <x> <y> [btn] [mod]` and `guikey <key>` monitor commands for pixel-accurate desktop input injection during automated testing. |

### 4.2 Memory Headroom Analysis

AstraOS enforces a strict architectural boundary where Segment 02 (`.data` and `.bss`) must never encroach on the kernel heap starting at `0xffffffff80400000`, requiring $> 20\text{ KiB}$ of free headroom.

Using LLVM ReadELF (`llvm-readelf.exe -l build/kernel.elf`):

```
Program Headers:
  Type           VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  LOAD           0xffffffff80200000 0xffffffff80200000 0x09dc48 0x09dc48 R E 0x1000
  LOAD           0xffffffff8029e000 0xffffffff8029e000 0x034469 0x034469 R   0x1000
  LOAD           0xffffffff802d3000 0xffffffff802d3000 0x0003f0 0x124878 RW  0x1000
```

- **Segment 02 Virtual Base:** `0xffffffff802d3000`
- **Segment 02 Memory Size:** `0x124878`
- **Segment 02 Virtual End:** `0xffffffff802d3000 + 0x124878 = 0xffffffff803f7878`
- **Kernel Heap Boundary:** `0xffffffff80400000`
- **Headroom Remaining:**
  $$\text{Headroom} = 0xffffffff80400000 - 0xffffffff803f7878 = 0x8788 = 34,696\text{ bytes} \approx \mathbf{33.88\text{ KiB}}$$

The headroom requirement of $> 20\text{ KiB}$ is fulfilled with **33.88 KiB** of margin.

### 4.3 Zero Heap Overhead Verification

Kernel heap allocations were verified via `heap_print_stats()` during AstraFiles 3.0 execution:
- **Arena Range:** `0xffffffff80601000 .. 0xffffffff80611000` (64 KiB capacity)
- **Allocated Bytes by AstraFiles:** **0 bytes** (Live Allocs: 0)
- **Directory List Buffers:** Dynamically requested via Physical Page Manager (`pmm_alloc_contiguous()`) using Limine Higher-Half Direct Map (HHDM) virtual addresses, completely bypassing the heap.

---

## 5. Acceptance Testing & Empirical Verification

Acceptance testing was conducted in Oracle VirtualBox 7.x with UEFI boot and $1920 \times 1080 \times 32\text{ bpp}$ framebuffer console.

### 5.1 Test Results Summary

| Test Case | Description | Expected Result | Actual Result | Status |
| :---: | :--- | :--- | :--- | :---: |
| **TC-01** | Desktop Launch & Baseline Layout | Opens $780 \times 480$ window via F2, displays navigation bars, sidebar, and storage card | Verified via screenshot `vbox_af3_1_desktop_launch.png` | **PASS** |
| **TC-02** | Persistent Storage Staging & Navigation | Populates `/data`, navigates via sidebar bookmark, displays sorted entries and badges | Staged and enumerated without error | **PASS** |
| **TC-03** | Single File Deletion & Rose Modal | Clicks `test_del.txt`, clicks Rose Delete pill, displays modal, confirms deletion, unlinks from disk | Verified via screenshot `vbox_af3_2_toolbar_delete_modal.png` | **PASS** |
| **TC-04** | Multi-Selection Engine | Clicks `file_a.txt`, `Ctrl+Clicks` `file_b.txt`, highlights both with cobalt background and checkboxes | Verified via screenshot `vbox_af3_3_multiselect.png` | **PASS** |
| **TC-05** | Right-Click Context Menu | Right-clicks selected item, opens 8-option menu, dismisses cleanly without click-through | Verified via screenshot `vbox_af3_4_context_menu.png` | **PASS** |
| **TC-06** | Non-Empty Recursive Directory Deletion | Selects `nest_dir` (contains `sub_dir/inner.txt`), sends Delete, confirms modal, deletes recursively | Verified via screenshot `vbox_af3_5_recursive_delete.png` | **PASS** |
| **TC-07** | Cold Reboot Persistence Verification | Reboots VM via hardware reset, verifies `test_del.txt` and `nest_dir` do NOT reappear | Verified via screenshot `vbox_af3_6_reboot_verified.png` | **PASS** |

### Complete Acceptance Test Report Reference
The detailed step-by-step test logs and verification matrix are documented in:
[astrafiles_3_acceptance_tests.md](file:///c:/Users/Akash/Documents/MyKernel/astrafiles_3_acceptance_tests.md)

---

## 6. Screenshot Artifacts

Visual evidence captured directly from the live VirtualBox framebuffer at $1920 \times 1080$:

- **TC-01 Desktop Launch:** [vbox_af3_1_desktop_launch.png](file:///C:/Users/Akash/.gemini/antigravity/brain/b812260b-9611-47cb-a29a-d654bc77750c/vbox_af3_1_desktop_launch.png)
- **TC-03 Rose Confirmation Modal:** [vbox_af3_2_toolbar_delete_modal.png](file:///C:/Users/Akash/.gemini/antigravity/brain/b812260b-9611-47cb-a29a-d654bc77750c/vbox_af3_2_toolbar_delete_modal.png)
- **TC-04 Multi-Selection Highlight:** [vbox_af3_3_multiselect.png](file:///C:/Users/Akash/.gemini/antigravity/brain/b812260b-9611-47cb-a29a-d654bc77750c/vbox_af3_3_multiselect.png)
- **TC-05 8-Option Context Menu:** [vbox_af3_4_context_menu.png](file:///C:/Users/Akash/.gemini/antigravity/brain/b812260b-9611-47cb-a29a-d654bc77750c/vbox_af3_4_context_menu.png)
- **TC-06 Recursive Deletion Modal:** [vbox_af3_5_recursive_delete.png](file:///C:/Users/Akash/.gemini/antigravity/brain/b812260b-9611-47cb-a29a-d654bc77750c/vbox_af3_5_recursive_delete.png)
- **TC-07 Cold Reboot Verification:** [vbox_af3_6_reboot_verified.png](file:///C:/Users/Akash/.gemini/antigravity/brain/b812260b-9611-47cb-a29a-d654bc77750c/vbox_af3_6_reboot_verified.png)

---

## 7. Deliverables Summary

1. `kernel/gui/app_filemgr.c`: Native AstraFiles 3.0 desktop implementation.
2. `kernel/include/kernel/input.h`: Mouse event modifier bitmask.
3. `kernel/drivers/ps2.c`: PS/2 mouse modifier population.
4. `kernel/browser/browser_url.c`: `.bss` buffer consolidation (+16 KiB headroom).
5. `kernel/monitor/monitor.c`: Deterministic event injection test commands.
6. `astrafiles_delete_bug_report.md`: Root cause analysis and diagnostic breakdown.
7. `astrafiles_3_ui_architecture.md`: Visual styling, component layout, and event model.
8. `astrafiles_3_acceptance_tests.md`: Acceptance test procedure and empirical logs.
9. `astrafiles_3_final_report.md`: Comprehensive engineering wrap-up report.
10. `tests/test_vbox_astrafiles3_acceptance.py`: End-to-end automated acceptance test script.

---

## 8. Conclusion

The broken delete functionality in AstraOS File Manager has been completely eradicated, and the application has been elevated to production desktop standards as **AstraFiles 3.0**. The operating system now possesses a robust, intuitive, and visually cohesive file manager capable of managing files and complex directory structures reliably across cold reboot cycles.
