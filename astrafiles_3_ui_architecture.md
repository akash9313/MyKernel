# AstraFiles 3.0: UI Architecture, Design Specification & Event Model

**Project:** AstraOS Kernel v1.0.0 (x86-64 UEFI/Limine)  
**Component:** Native Graphical File Manager (`kernel/gui/app_filemgr.c`)  
**Design System:** Cosmic Vista Dark Aesthetic  
**Target Resolution:** $1920 \times 1080 \times 32\text{ bpp}$  
**Default Window Dimensions:** $780 \times 480\text{ px}$  
**Date:** November 17, 2026  

---

## 1. Architectural Overview

AstraFiles 3.0 is a complete re-architecture of the native AstraOS graphical file manager. It addresses every known defect in file and folder deletion, multi-selection, hit-testing, and coordinate translation, while introducing modern desktop ergonomics:

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

---

## 2. Visual Design & Theme Palette (Cosmic Vista Dark)

All components strictly adhere to the AstraOS Cosmic Vista graphite/slate dark-mode palette:

| UI Element | Color Hex | Visual Purpose |
| :--- | :--- | :--- |
| **Window Background** | `0x111827` | Deep slate canvas background |
| **Top Nav & Headers** | `0x0F172A` | Primary header & column surface |
| **Action Toolbar** | `0x162238` | Secondary interactive tool strip |
| **Sidebar Background** | `0x0B0F19` | Deepest dark navy navigation bar |
| **Status Bar** | `0x0B0F19` | Bottom telemetry & notification surface |
| **Border / Divider** | `0x1E293B` | Subtle panel separators and gridlines |
| **Accent Sky Blue** | `0x38BDF8` | Active folder, primary action focus, selection borders |
| **Selection Highlight**| `0x1E3A8A` | High-contrast deep cobalt row highlight |
| **Emerald Action** | `0x065F46` / `0x10B981` | `+ File` create button |
| **Amber Action** | `0x9A3412` / `0xF97316` | `+ Folder` create button |
| **Rose Action** | `0x881337` / `0xF43F5E` | `Delete` button and deletion confirmation dialog |
| **Text Primary** | `0xF8FAFC` | High-legibility off-white for labels and file names |
| **Text Secondary** | `0x94A3B8` | Muted slate for metadata, sizes, types |
| **Text Muted** | `0x64748B` | Subtle gray for section headers and empty placeholders |

---

## 3. Detailed Component Geometry & Layout Math

Window dimensions: `client_w = 780`, `client_h = 480`.

### 3.1 Top Navigation Bar (`y = 0 .. 34`, Height: $34\text{ px}$)
- **Background:** `0x0F172A`, bottom divider at `y = 34` (`0x1E293B`).
- **Interactive Controls (`y = 5 .. 29`, Height: $24\text{ px}$):**
  - `[<]` Back: `x = 8 .. 34` ($26\text{ px}$). Enabled if `history_pos > 0`.
  - `[>]` Forward: `x = 38 .. 64` ($26\text{ px}$). Enabled if `history_pos + 1 < history_count`.
  - `[^]` Up: `x = 68 .. 94` ($26\text{ px}$). Navigates to parent directory.
  - `[H]` Home: `x = 98 .. 124` ($26\text{ px}$). Navigates to `/data` persistent storage.
  - `[R]` Refresh: `x = 128 .. 154` ($26\text{ px}$). Reloads current directory from VFS.
  - **Breadcrumb Path Bar:** `x = 158 .. (client_w - 188)` ($434\text{ px}$). Pill container with clickable hierarchy segments (`Root`, `>`, `data`, `>`, `sub`).
  - **Search / Filter Input:** `x = (client_w - 178) .. (client_w - 6)` ($172\text{ px}$). Real-time substring filter on file names with clear button `[x]`.

### 3.2 Action Toolbar (`y = 35 .. 66`, Height: $32\text{ px}$)
- **Background:** `0x162238`, bottom divider at `y = 66` (`0x1E293B`).
- **Interactive Buttons (`y = 39 .. 63`, Height: $24\text{ px}$):**
  - `+ File`: `x = 8 .. 70` ($62\text{ px}$). Emerald rounded pill. Launches new file dialog.
  - `+ Folder`: `x = 76 .. 150` ($74\text{ px}$). Amber rounded pill. Launches new folder dialog.
  - `Cut`: `x = 156 .. 206` ($50\text{ px}$). Slate button. Copies path to clipboard in cut mode.
  - `Copy`: `x = 212 .. 264` ($52\text{ px}$). Slate button. Copies path to clipboard.
  - `Paste`: `x = 270 .. 326` ($56\text{ px}$). Slate button. Pastes clipboard item into current dir.
  - `Rename`: `x = 332 .. 396` ($64\text{ px}$). Slate button. Opens rename dialog on selected item.
  - **`Delete` Button:** `x = 402 .. 470` (**$68\text{ px}$**). Rose rounded pill (`0x881337`/`0xF43F5E`).
    - *Hit-Box:* Exact bounding box `rel_x >= 402 && rel_x <= 470 && rel_y >= 39 && rel_y <= 63`.
    - Text `"Delete"` ($48\text{ px}$) centered at `x = 412` with symmetric $10\text{ px}$ padding.
  - `Sort`: `x = 476 .. 540` ($64\text{ px}$). Toggles sort mode: Name (`Sort:N`) $\rightarrow$ Size (`Sort:S`) $\rightarrow$ Type (`Sort:T`).
  - `View`: `x = 546 .. 610` ($64\text{ px}`). Toggles view: Details (`View:D`) $\leftrightarrow$ Compact List (`View:L`).
  - `Select All`: `x = 616 .. 706` ($90\text{ px}$). Selects all non-`..` files in directory.

### 3.3 Left Navigation Sidebar (`x = 0 .. 140`, `y = 67 .. client_h - 24`)
- **Background:** `0x0B0F19`, right divider at `x = 140` (`0x1E293B`).
- **FAVORITES Section (`y = 75 .. 236`):**
  - Bookmark rows ($28\text{ px}$ pitch): Root (`/`), Storage (`/data`), Binaries (`/bin`), Web (`/www`), Config (`/etc`).
  - Active bookmark marked with rounded pill highlight (`0x1E293B`) and Sky Blue border (`0x38BDF8`).
- **STORAGE Section (`y = 246 .. 330`):**
  - Card container: `x = 8 .. 132`, `y = 262 .. 330`.
  - AstraFS persistent disk telemetry: Capacity bar showing used vs free sectors.
  - Capacity readout: e.g., `31MB free of 32MB`.

### 3.4 Main File Table Area (`x = 146 .. client_w - 14`, `y = 67 .. client_h - 24`)
- **Column Header (`y = 67 .. 91`, Height: $24\text{ px}$):**
  - Columns: `NAME` ($x = 182$), `TYPE` ($x = 426$), `SIZE` ($x = 566$).
- **File Rows (`y = 92 .. client_h - 24`, Height: $24\text{ px}$ per row):**
  - Visible capacity: $(456 - 92) / 24 = 15$ rows per page.
  - Row background: Alternating `0x111827` and `0x0D1424`.
  - Selection state: Filled `0x1E3A8A` with `0x38BDF8` border.
  - Multi-select checkbox: $12 \times 12\text{ px}$ at `x = main_x + 8`.
  - File Type Badges ($26 \times 16\text{ px}$ rounded pill):
    - `[DIR]` (Sky Blue `0x0284C7`)
    - `[C]` / `[C++]` (Emerald Green `0x059669`)
    - `[PY]` (Amber Orange `0xD97706`)
    - `[SH]` (Teal `0x0D9488`)
    - `[TXT]` / `[MD]` (Slate Gray `0x475569`)
    - `[ELF]` (Purple `0x7C3AED`)
    - `[FILE]` (Royal Blue `0x2563EB`)
  - Empty Directory placeholder: Centered string `"This folder is empty."` in Slate (`0x64748B`).

### 3.5 Scrollbar (`x = client_w - 12 .. client_w - 4`, `y = 92 .. client_h - 24`)
- Track width: $8\text{ px}$, Slate track (`0x0B0F19`), Slate thumb (`0x334155`).
- Integrated via `scrollview_t` with mouse-wheel support and drag tracking.

### 3.6 Bottom Status Bar (`y = client_h - 24 .. client_h`, Height: $24\text{ px}$)
- Left: Item count and selection summary (e.g., `6 items (2 selected) | Ready.`).
- Right: Real-time AstraFS storage capacity from `vfs_statfs()` (e.g., `AstraFS: 31MB free of 32MB`).

---

## 4. Multi-Selection Engine & Event State Machine

```
              ┌───────────────────────────┐
              │      Idle Canvas          │
              └─────────────┬─────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │ Click on Row      │ Ctrl+Click on Row │ Shift+Click on Row
        ▼                   ▼                   ▼
┌───────────────┐   ┌───────────────┐   ┌────────────────┐
│ Single Select │   │ Toggle Select │   │  Range Select  │
│ clear others  │   │ invert state  │   │ anchor .. row  │
│ count = 1     │   │ update count  │   │ update count   │
└───────┬───────┘   └───────┬───────┘   └────────┬───────┘
        │                   │                    │
        └───────────────────┼────────────────────┘
                            │
                            ▼
              ┌───────────────────────────┐
              │ Multi-Selection Active    │
              │  - Toolbar buttons active │
              │  - Checkboxes checked     │
              │  - Status bar updated     │
              └─────────────┬─────────────┘
                            │
           ┌────────────────┴────────────────┐
           │ Del Key / Toolbar / Context Menu│
           ▼                                 ▼
┌─────────────────────────────┐   ┌─────────────────────────────┐
│ Single Item Delete Modal    │   │ Multi-Item Delete Modal     │
│ "Delete '<filename>'?"      │   │ "Delete <N> selected items?"│
└──────────────┬──────────────┘   └──────────────┬──────────────┘
               │                                 │
               └────────────────┬────────────────┘
                                │ Click [Delete] / Press Enter
                                ▼
              ┌───────────────────────────────────┐
              │ Two-Pass Non-Skipping Deletion    │
              │ Pass 1: Gather child entries      │
              │ Pass 2: Unlink & rmdir            │
              │ Commit to AstraFS persistent disk │
              │ Reload directory & update status  │
              └───────────────────────────────────┘
```

### 4.1 Double-Click vs Single-Click Resolution
To eliminate Bug 3 (accidental opening when clicking selected item):
- Tick threshold: `40 ticks` ($400\text{ ms}$ at $100\text{ Hz}$ APIC timer).
- If `(fm->last_click_row == row && (apic_get_ticks() - fm->last_click_tick < 40))`:
  - Triggers `filemgr_open_item(fm, row)`.
- Otherwise:
  - Updates selection state (`select_single`, `toggle_select`, or `range_select`).
  - Records `last_click_tick = apic_get_ticks()` and `last_click_row = row`.

### 4.2 Context Menu Isolation (Zero Click-Through)
To eliminate Bug 4 (click-through upon dismissal):
- Inside `filemgr_on_event()` mouse down handler:
  ```c
  if (fm->menu_open) {
      if (within_menu_bounds) {
          execute_selected_menu_option();
          fm->menu_open = false;
          return;
      }
      /* Click outside context menu -> dismiss and CONSUME event */
      fm->menu_open = false;
      return; /* Explicit early return prevents click-through! */
  }
  ```

---

## 5. Memory Architecture & Zero-Heap Commitment

AstraFiles 3.0 operates with **zero bytes allocated from the kernel heap arena (`kmalloc`)**:
- All directory items (`filemgr_item_t items[1200]`) are backed by contiguous physical pages allocated through `pmm_alloc_contiguous()`.
- Temporary recursive deletion traversal buffers (`fm_child_entry_t children[240]`) are allocated dynamically from PMM and freed immediately upon completion.
- Kernel BSS footprint is strictly bounded to `2.5 KiB` for `filemgr_state_t`.
- Kernel ELF headroom remains at **$33.88\text{ KiB}$ free below $0xffffffff80400000$** (far exceeding the $20\text{ KiB}$ requirement).
