# AstraOS — File Manager 2.0 (AstraFiles) Architecture Specification

**Date:** 2026-11-17  
**Module:** `kernel/gui/app_filemgr.c`  
**Integration:** `kernel/include/kernel/scrollview.h`, `kernel/fs/vfs.h`, `kernel/fs/astrafs.h`

---

## 1. Architectural Overview & Component Structure

AstraFiles 2.0 replaces the legacy single-directory view with a native graphical shell file manager featuring persistent hierarchical folder navigation, mouse-wheel scrolling, right-click context menus, modal dialogs, and clipboard operations.

```
+-----------------------------------------------------------------------------------------------+
| AstraOS Files (VFS)                                                             [-] [+] [X]   |
+-----------------------------------------------------------------------------------------------+
| [^] [R]  Path: Root > data > docs              [+ File]  [+ Folder]  [Delete]  [Open File]   |
+---------------+-------------------------------------------------------------------------------+
| LOCATIONS     | NAME                       TYPE           SIZE                      |^|       |
|               +---------------------------------------------------------------------| |       |
| Root (/)      | [D] ..                     Folder         --                        | |       |
| Storage(/data)| [D] docs                   Folder         --                        |#| Thumb |
| Binaries(/bin)| [F] notes.txt              File           1.4 KB                    | |       |
| Web (/www)    | [F] main.c                 File           420 B                     | |       |
| Config (/etc) | [F] script.py              File           280 B                     |v|       |
+---------------+-------------------------------------------------------------------------------+
| 5 items | Directory refreshed.                   AstraFS: 63 MB free of 64 MB (98% free)      |
+-----------------------------------------------------------------------------------------------+
```

---

## 2. Key Data Structures

### 2.1 File Item Buffer (`filemgr_item_t`)
Each directory entry is tracked with:
```c
typedef struct filemgr_item {
    char name[64];
    bool is_dir;
    size_t size;
} filemgr_item_t;
```
Up to 128 items (`FILEMGR_MAX_ENTRIES`) are allocated dynamically using physical page frames via `pmm_alloc_contiguous()` and mapped through the Higher Half Direct Map (`HHDM`), avoiding kernel heap fragmentation and keeping static `.bss` usage below 400 bytes.

### 2.2 Global State & Viewport Geometry (`filemgr_state_t`)
```c
typedef struct filemgr_state {
    char current_path[128];
    filemgr_item_t *items;
    phys_addr_t items_pa;
    size_t items_pages;
    int item_count;
    int selected_idx;
    char status_msg[128];

    scrollview_t scrollview;

    /* Context Menu */
    bool menu_open;
    int menu_x;
    int menu_y;
    int menu_target_idx;

    /* Modal Dialog */
    fm_dialog_type_t dialog_type;
    char dialog_input[64];
    int dialog_input_len;
    int dialog_target_idx;

    int visible_rows;
} filemgr_state_t;
```

---

## 3. Subsystem Breakdown

### 3.1 Navigation & Breadcrumbs Engine
- **Up Button `[^]`:** Truncates `current_path` to the previous slash and reloads directory.
- **Refresh Button `[R]`:** Triggers `filemgr_load_dir()` and queries current directory state.
- **Clickable Breadcrumb Pills:** The path is segmented by `/`. Each segment is rendered with a clickable pill. Clicking any segment navigates directly to that directory level.

### 3.2 Vertical Scrolling Engine (`scrollview_t`)
- **Shared Implementation (`kernel/include/kernel/scrollview.h`):** Encapsulates mouse-wheel events, draggable scrollbar thumb, track paging clicks, and bounds clamping.
- **Mouse Wheel:** Dispatches `scrollview_handle_wheel()`, scrolling 3 items per notch.
- **Scrollbar Drag:** Detects thumb clicks, enters dragging state, computes delta items relative to travel height, and updates viewport.
- **Keyboard Navigation:** `Up`/`Down` arrow keys step through selection and auto-scroll viewport; `PageUp`/`PageDown` jump by full visible page.

### 3.3 Right-Click Context Menu
- Right-click (`MOUSE_BTN_RIGHT`) on the main table calculates the clicked row index and opens the floating context menu.
- **Context Actions (Target Item):** `Open (Enter)`, `Cut (Ctrl+X)`, `Copy (Ctrl+C)`, `Paste (Ctrl+V)`, `Rename (F2)`, `Delete (Del)`, `Properties`.
- **Context Actions (Empty Canvas):** `New File`, `New Folder`, `Paste (Ctrl+V)`, `Refresh (F5)`, `Properties`.

### 3.4 Modal Dialog Subsystem
- Modal overlays display centered on screen with backdrop dimming.
- Handled dialog types:
  - `FM_DLG_NEW_FILE`: Prompts for filename, creates new file with optional template.
  - `FM_DLG_NEW_FOLDER`: Prompts for folder name, calls `vfs_mkdir()`.
  - `FM_DLG_RENAME`: Prompts for new name, calls `vfs_rename()`.
  - `FM_DLG_DELETE_CONFIRM`: Prompts for deletion confirmation, calls `vfs_unlink()` or `vfs_rmdir()`.
  - `FM_DLG_PROPERTIES`: Displays file telemetry (path, size, sector count, storage backend).

### 3.5 Clipboard Architecture (`s_file_clipboard`)
- Supports global copy/cut/paste across directory levels:
```c
static struct {
    bool has_data;
    bool is_cut;
    char src_path[128];
    char filename[64];
    bool is_dir;
} s_file_clipboard;
```
- During paste: opens source with `VFS_O_RDONLY`, creates destination with `VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC`, streams chunks, and calls `vfs_unlink(src_path)` if marked for cut.
