# AstraOS — Code & Text Editor 2.0 (AstraEdit) Architecture Specification

**Date:** 2026-11-17  
**Module:** `kernel/gui/app_editor.c`  
**Header Dependencies:** `kernel/wm.h`, `kernel/gfx.h`, `kernel/vfs.h`, `kernel/pmm.h`, `kernel/boot.h`, `kernel/scrollview.h`, `kernel/terminal.h`

---

## 1. Architectural Overview & Component Structure

AstraEdit 2.0 replaces the legacy single-file viewer with a native multi-tab code and text editing environment. It delivers tabbed document management, dynamic memory allocation via PMM, responsive scrolling with line gutter calculation, full clipboard and shortcut integration, and modal dialogs for file operations.

```
+-----------------------------------------------------------------------------------------------+
| AstraOS Code Editor                                                             [-] [+] [X]   |
+-----------------------------------------------------------------------------------------------+
| [+ New]  [Open]  [Save]  [Save As]  [Run >]                              [Python 3.11] Badge  |
+-----------------------------------------------------------------------------------------------+
| [o Untitled-1 x]  [o server.py x]  [o /data/notes.txt x]  [+]                                 |
+----+------------------------------------------------------------------------------------+-----+
| Ln | Text Canvas                                                                        |     |
|----+------------------------------------------------------------------------------------+ | | |
|  1 | #!/usr/bin/env python3                                                             | | | |
|  2 | import sys                                                                         | |#| |
|  3 |                                                                                    | | | |
|  4 | def main():                                                                        | | | |
|  5 |     print("AstraOS Code Editor 2.0")|                                              | | | |
|  6 |                                                                                    | |v| |
+----+------------------------------------------------------------------------------------+-----+
| Ln 5, Col 37 | UTF-8 | 4 Spaces                            Saved to persistent VFS successfully|
+-----------------------------------------------------------------------------------------------+
```

---

## 2. Dynamic Memory Architecture & Heap Protection

A critical architectural requirement in AstraOS is preserving the kernel heap arena (`HEAP_ARENA_SIZE = 64 KiB`). Allocating 32 KiB document buffers on the kernel heap caused memory exhaustion panics during multi-threaded stress tests.

### 2.1 Physical Memory Manager (PMM) Dynamic Page Mapping
AstraEdit allocates all document buffers and undo snapshots directly from physical page frames using `pmm_alloc_contiguous()`. These physical pages are mapped to virtual addresses via Limine's Higher Half Direct Map (`HHDM`):

```c
size_t text_pages = (EDITOR_TAB_BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE; // 8 pages (32 KiB)
phys_addr_t text_pa = 0;
pmm_alloc_contiguous(text_pages, &text_pa);
tab->text = (char *)(text_pa + hhdm->offset);
tab->text_pa = text_pa;
tab->text_pages = text_pages;

size_t undo_pages = (EDITOR_UNDO_BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE; // 2 pages (8 KiB)
phys_addr_t undo_pa = 0;
pmm_alloc_contiguous(undo_pages, &undo_pa);
tab->undo_text = (char *)(undo_pa + hhdm->offset);
tab->undo_pa = undo_pa;
tab->undo_pages = undo_pages;
```

### 2.2 Memory Isolation Benefits
1. **Zero Kernel Heap Overhead:** Uses 0 bytes of the 64 KiB `kmalloc` heap arena.
2. **Zero BSS Overhead:** Document storage is never statically placed in `.bss`, preserving kernel headroom below `0xffffffff80400000` ($> 55\text{ KiB}$ remaining).
3. **Leak-Free Destruction:** When a tab is closed or the editor window is destroyed (`editor_on_destroy`), physical pages are returned to the PMM pool via `pmm_free_contiguous()`.

---

## 3. Data Structures

### 3.1 Document Tab (`editor_tab_t`)
Each open document tab encapsulates complete buffer state, cursor positions, scrolling metrics, and undo snapshot:

```c
typedef struct editor_tab {
    char filepath[128];
    char title[32];
    bool is_untitled;
    char *text;          /* Mapped from PMM physical pages (32 KiB) */
    phys_addr_t text_pa;
    size_t text_pages;
    int text_len;
    int cursor_pos;
    bool dirty;
    int vscroll;         /* First visible line in canvas */
    int hscroll;
    int total_lines;

    /* Single undo snapshot */
    char *undo_text;     /* Mapped from PMM physical pages (8 KiB) */
    phys_addr_t undo_pa;
    size_t undo_pages;
    int undo_len;
    int undo_pos;
    bool has_undo;

    scrollview_t scrollview;
} editor_tab_t;
```

### 3.2 Global Editor State (`editor_state_t`)
Supports up to 6 concurrent tabs (`EDITOR_MAX_TABS`), active modal dialogs, and status messages:

```c
typedef struct editor_state {
    editor_tab_t tabs[EDITOR_MAX_TABS];
    int tab_count;
    int active_tab;
    char status_msg[128];

    /* Modal Dialog */
    ed_dialog_type_t dialog_type;
    char dialog_input[128];
    int dialog_input_len;
    int close_tab_target;

    int untitled_counter;
} editor_state_t;
```

---

## 4. Document Lifecycle & Launch Workflows

### 4.1 Blank Launch Mode (`app_editor_launch(NULL)`)
- When invoked from the dock, desktop shortcut, or terminal command `edit` with no arguments, AstraEdit opens a pristine document titled `Untitled-1` (incrementing on subsequent untitled tabs).
- **No Default File:** Unlike legacy iterations that forced opening `/hello.txt`, `app_editor_launch(NULL)` initializes an empty buffer (`text_len = 0`, `dirty = false`, `is_untitled = true`).
- The Save workflow on an untitled tab intercepts the operation and routes directly to the `Save As` modal dialog.

### 4.2 File Launch Mode (`app_editor_launch(filepath)`)
1. Checks if the requested path is already loaded in an active tab. If found, switches to that tab and brings the editor window to the foreground.
2. If the single active tab is a clean, untouched `Untitled` document, AstraEdit reuses that tab for the opened file rather than opening a redundant tab.
3. Loads file contents through `vfs_open(filepath, VFS_O_RDONLY)` and reads up to 32,767 bytes into `tab->text`.
4. Calculates line count, extracts base filename for tab header, and initializes the cursor at the beginning or end of file.

---

## 5. UI Layout & Viewport Geometry

The editor client canvas is structured into five distinct functional zones:

| Zone | Coordinates | Purpose |
| :--- | :--- | :--- |
| **Top Action Bar** | $y \in [0, 32]$ | Action buttons (`+ New`, `Open`, `Save`, `Save As`, `Run >`) and syntax badge |
| **Tab Strip** | $y \in [33, 61]$ | Multi-tab buttons with dirty status dots, close buttons `[x]`, and `[+]` button |
| **Line Gutter** | $x \in [0, 42], y \in [62, h-24]$ | Displays 1-based line numbers aligned with text rows; active line highlighted |
| **Text Canvas** | $x \in [43, w-14], y \in [62, h-24]$ | Text editing surface, glowing caret (2px vertical), tab expansion (4 spaces) |
| **Vertical Scrollbar** | $x \in [w-14, w-4], y \in [62, h-24]$ | Draggable thumb, wheel scroll target, paging track |
| **Status Bar** | $y \in [h-24, h]$ | Left status message; right telemetry: `Ln X, Col Y \| UTF-8 \| 4 Spaces` |

### 5.1 Syntax Badge Detection
The file extension is inspected to display an informative language tag in the top right corner:
- `.c` $\rightarrow$ `[C Source]` (Sky Blue `#38BDF8`)
- `.cpp` $\rightarrow$ `[C++ Source]` (Indigo `#818CF8`)
- `.py` $\rightarrow$ `[Python 3.11]` (Amber `#FBBF24`)
- `.sh` $\rightarrow$ `[Shell Script]` (Emerald `#34D399`)
- `.h` $\rightarrow$ `[C Header]` (Cyan `#22D3EE`)
- Default $\rightarrow$ `[Text Document]` (Slate `#94A3B8`)

---

## 6. Input Handling & Shortcut Engine

### 6.1 Keyboard Shortcuts Matrix

| Shortcut | Keycode / ASCII | Action Description |
| :--- | :--- | :--- |
| **Ctrl + N** | `'n'`, `'N'`, `0x0E` | Create new untitled document tab |
| **Ctrl + O** | `'o'`, `'O'`, `0x0F` | Open "Open Document" modal dialog |
| **Ctrl + S** | `'s'`, `'S'`, `0x13` | Save active tab directly (or triggers Save As if untitled) |
| **Ctrl + W** | `'w'`, `'W'`, `0x17` | Close active tab (prompts if unsaved changes exist) |
| **Ctrl + P** | `'p'`, `'P'`, `0x10` | Run file in AstraOS Terminal via `run <filepath>` |
| **Ctrl + Z** | `'z'`, `'Z'`, `0x1A` | Undo last text modification |
| **Ctrl + C** | `'c'`, `'C'`, `0x03` | Copy active line to system clipboard |
| **Ctrl + V** | `'v'`, `'V'`, `0x16` | Paste text from system clipboard at cursor position |
| **Ctrl + X** | `'x'`, `'X'`, `0x18` | Cut active line to system clipboard |
| **Ctrl + A** | `'a'`, `'A'`, `0x01` | Jump cursor to end of document |
| **Ctrl + D** | `'d'`, `'D'`, `0x04` | Duplicate active line below current line |
| **Ctrl + K** | `'k'`, `'K'`, `0x0B` | Delete active line |
| **Page Up** | `KEY_PAGE_UP` | Scroll viewport up 15 lines |
| **Page Down** | `KEY_PAGE_DOWN` | Scroll viewport down 15 lines |
| **Home** | `KEY_HOME` | Jump cursor to beginning of current line |
| **End** | `KEY_END` | Jump cursor to end of current line |
| **Up / Down** | `KEY_UP`, `KEY_DOWN` | Move cursor up/down with column tracking |
| **Left / Right** | `KEY_LEFT`, `KEY_RIGHT` | Step cursor backward/forward by character |
| **Tab** | `KEY_TAB` | Insert 4 soft spaces into buffer |
| **Enter** | `KEY_ENTER` | Insert newline `\n` and split line |
| **Backspace** | `KEY_BACKSPACE` | Delete character preceding cursor with undo snapshot |
| **Delete** | `KEY_DELETE` | Delete character following cursor with undo snapshot |

### 6.2 Mouse Interaction & Scrollview
- **Wheel Scrolling:** Dispatches `delta_z` events, stepping `tab->vscroll` by $\pm 3$ lines per notch.
- **Scrollbar Dragging:** Implemented via `scrollview_handle_mouse_down()`, `scrollview_handle_mouse_move()`, and `scrollview_handle_mouse_up()`.
- **Tab Selection & Closing:** Clicking a tab activates it; clicking the `[x]` on a tab invokes `editor_close_tab()`. If the tab has unsaved changes, the `ED_DLG_CONFIRM_CLOSE` modal dialog is triggered.

---

## 7. Modal Dialog Subsystem

AstraEdit features an in-window modal dialog state machine (`ed_dialog_type_t`). While active, modal dialogs draw a stippled dark backdrop over the text canvas, intercept all keyboard and mouse events, and prevent background editing until dismissed.

```
       +---------------------------------------------+
       |             Save Document As                |
       +---------------------------------------------+
       | Enter path (e.g. /data/filename.txt):       |
       | [ /data/my_script.py|                     ] |
       |                                             |
       |                   [  Save  ]   [ Cancel ]   |
       +---------------------------------------------+
```

### 7.1 Dialog Types
1. **`ED_DLG_SAVE_AS`:**
   - Pre-fills input with `/data/` to guide the user towards persistent block storage.
   - On `Enter` or clicking `[Save]`, calls `editor_save_as_commit()`, opening `vfs_open(..., O_WRONLY | O_CREAT | O_TRUNC)`, writing buffer, updating `tab->filepath`, clearing `tab->is_untitled`, and clearing `tab->dirty`.
2. **`ED_DLG_OPEN_FILE`:**
   - Pre-fills input with `/data/`.
   - On `Enter` or clicking `[Open]`, invokes `editor_open_file_in_tab()`.
3. **`ED_DLG_CONFIRM_CLOSE`:**
   - Triggered when closing a dirty tab (`tab->dirty == true`).
   - Presents three options:
     - `[Save]`: Saves active tab before closing.
     - `[Don't Save]`: Discards unsaved modifications and closes tab.
     - `[Cancel]`: Returns to editing without losing state.

---

## 8. Integration with Terminal & System Shell

When the user clicks `[Run >]` or presses `Ctrl+P`:
1. If the document is untitled or has unsaved edits, it is automatically committed to persistent storage.
2. The file path is prefixed with `run ` (e.g., `run /data/my_script.py`).
3. Calls `app_terminal_launch_with_command(cmd)`.
4. If an AstraOS Terminal window is already open, the command executes in the active terminal; otherwise, a new terminal instance is launched, bringing the output to the foreground.

---

## 9. Architectural Verification Matrix

| Architectural Invariant | Design Mechanism | Verification Result |
| :--- | :--- | :--- |
| **Heap Arena Safety** | PMM pages allocated via `pmm_alloc_contiguous()` | Zero `kmalloc` heap usage; passes multi-core stress tests |
| **Headroom Buffer** | Static buffers eliminated; `.bss` usage $< 400$ bytes | Free headroom below `0xffffffff80400000` is $55,415\text{ bytes}$ |
| **Blank Launch Policy** | `app_editor_launch(NULL)` spawns clean `Untitled-1` | Verified in VirtualBox EFI (`vbox_fm_ed_4_editor_blank.png`) |
| **Save Workflow** | Intercepts untitled tabs and prompts for path in `/data/` | Verified in VirtualBox EFI (`vbox_fm_ed_5_editor_save_as_dialog.png`) |
| **Zero Data Loss** | Writes committed directly to AstraFS LBA sectors | Verified byte-for-byte across cold hardware reboots |
