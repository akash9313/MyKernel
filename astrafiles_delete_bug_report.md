# AstraFiles 3.0: File and Folder Deletion Root Cause Analysis & Diagnostic Report

**Project:** AstraOS Kernel v1.0.0 (x86-64 UEFI/Limine)  
**Component:** GUI File Manager (`kernel/gui/app_filemgr.c`), VFS (`kernel/fs/vfs.c`), AstraFS (`kernel/fs/astrafs.c`)  
**Date:** November 17, 2026  

---

## 1. Executive Summary

An in-depth investigation into the AstraFiles 2.0 deletion subsystem was conducted to identify why the Delete button and context menu failed to reliably delete selected files and folders. 

Seven distinct failure mechanisms were identified spanning event hit-testing, coordinate clamping, selection state synchronization, recursive directory traversal, and error propagation.

---

## 2. Deletion Pathway Trace

The complete lifecycle of a user deletion request traverses the following layers:

```
[User Action: Toolbar / Right-Click / Key Delete]
          │
          ▼
[GUI Event Dispatch: wm.c -> filemgr_on_event()]
          │
          ▼
[Hit-Testing & Selected Item Identification]
          │ (Bug 2: Toolbar button width truncation)
          │ (Bug 3: Double-click collision resetting or opening item)
          │ (Bug 5: Parent directory '..' selection hazard)
          ▼
[Confirmation Modal: FM_DLG_DELETE_CONFIRM]
          │
          ▼
[User Confirms: Click [Delete] or Key Enter]
          │ (Bug 4: Dialog target index desynchronization)
          ▼
[filemgr_op_delete() -> filemgr_delete_recursive()]
          │ (Bug 1: readdir idx++ index-skipping in non-empty directories)
          ▼
[VFS Layer: vfs_unlink() / vfs_rmdir()]
          │
          ▼
[AstraFS Persistent Layer: astrafs_unlink() / astrafs_rmdir()]
          │ (Fails with VFS_ERR_BUSY (-16) when children remain)
          ▼
[Dual-Set Metadata Commit: astrafs_commit_metadata()]
          │
          ▼
[Directory Listing Refresh: filemgr_load_dir()]
          │ (Bug 7: Silent false-positive status reporting)
          ▼
[Visual Feedback & Status Bar]
```

---

## 3. Detailed Root Causes

### Root Cause 1: Recursive Deletion Index-Skipping Flaw (`vfs_readdir idx++`)
**Location:** `kernel/gui/app_filemgr.c:312-329`
```c
static void filemgr_delete_recursive(const char *path) {
    if (!path) return;
    vfs_stat_t st;
    if (vfs_stat(path, &st) != VFS_OK) return;
    if (st.type == VFS_NODE_DIR) {
        size_t idx = 0;
        vfs_dirent_t dent;
        while (vfs_readdir(path, idx++, &dent) == VFS_OK) {
            if (fm_strcmp(dent.name, ".") == 0 || fm_strcmp(dent.name, "..") == 0) continue;
            char child[128];
            fm_path_join(child, sizeof(child), path, dent.name);
            filemgr_delete_recursive(child);
        }
        vfs_rmdir(path);
    } else {
        vfs_unlink(path);
    }
}
```
**Mechanism:**
1. In AstraFS, directory entries are stored in a contiguous active inode table. When an entry is deleted, it is removed from the active inode set.
2. When `filemgr_delete_recursive(child)` is executed for entry at `idx = 0`, that child is deleted.
3. The remaining sibling entries shift forward into lower indices (what was previously index 1 now occupies index 0).
4. The while loop increments `idx++` to 1. On the subsequent iteration, `vfs_readdir(path, 1, &dent)` queries index 1, effectively **skipping the entry that moved into index 0**.
5. In any folder containing two or more files or subdirectories, every alternate child is skipped and remains on disk.
6. When `vfs_rmdir(path)` is called, `astrafs_rmdir` checks if the directory has active children. Because skipped children remain, `astrafs_rmdir` returns `VFS_ERR_BUSY` (-16).
7. The directory deletion fails completely, leaving the folder and remaining files intact.

### Root Cause 2: Toolbar Delete Button Hit-Box Width Truncation
**Location:** `kernel/gui/app_filemgr.c:528-531` and `1204-1213`
```c
/* Paint */
int del_x = fld_x + 80;
gfx_fill_rounded_rect(s, del_x, 5, 54, 24, 4, 0x881337);
gfx_draw_string(s, del_x + 8, 9, "Delete", 0xFFFFFF, 0, true);

/* Event Handler */
int del_x = fld_x + 80;
if (rel_x >= del_x && rel_x <= del_x + 54 && rel_y >= 5 && rel_y <= 29) { ... }
```
**Mechanism:**
1. The text string `"Delete"` has 6 characters. At 8 pixels per character (`FONT_WIDTH = 8`), the text width is $48\text{ px}$.
2. With an 8-pixel left margin (`del_x + 8`), the text extends to `del_x + 56`.
3. The painted button background and the mouse hit test were constrained to width 54 (`del_x + 54`).
4. Any mouse click on the right half of the word "Delete" (specifically characters 't' and 'e') falls outside `del_x + 54`. The event handler ignored the click, giving the impression that the Delete button was non-responsive.

### Root Cause 3: Repeated-Click / Double-Click Collision with Selection
**Location:** `kernel/gui/app_filemgr.c:1248-1252`
```c
if (row >= 0 && row < fm->item_count) {
    if (fm->selected_idx == row) {
        /* Double click / repeated click -> open item */
        filemgr_open_item(fm, row);
        return;
    }
    fm->selected_idx = row;
    ...
```
**Mechanism:**
1. There was no timer or interval threshold (e.g. 400ms) for distinguishing a single selection click from a double click.
2. Once an item was selected (`fm->selected_idx == row`), clicking on the item again (e.g., to confirm focus, prepare to drag, or right-click) immediately triggered `filemgr_open_item()`, launching AstraEdit or navigating into the folder instead of maintaining selection.

### Root Cause 4: Context Menu Dismissal Click-Through
**Location:** `kernel/gui/app_filemgr.c:1079-1130`
```c
if (fm->menu_open) {
    if (rel_x >= mx && rel_x <= mx + mw && rel_y >= my && rel_y <= my + mh) {
        ...
        return;
    }
    fm->menu_open = false;
}
/* No return! Execution falls through to table rows and buttons underneath! */
```
**Mechanism:**
When a context menu was open and the user clicked outside to close it, the handler cleared `fm->menu_open = false` but did not return. The click event proceeded immediately to hit-test table rows and toolbar buttons underneath, accidentally selecting or opening unintended files.

### Root Cause 5: Parent Directory (`..`) Deletion Hazard
**Location:** `kernel/gui/app_filemgr.c:179-184`
```c
if (fm->current_path[0] == '/' && fm->current_path[1] != '\0') {
    filemgr_item_t *up = &fm->items[fm->item_count++];
    fm_strcpy(up->name, "..", sizeof(up->name));
    up->is_dir = true;
    up->size = 0;
}
```
**Mechanism:**
In non-root directories, index 0 is populated with the `".." ` parent directory link. If index 0 was selected when clicking Delete or pressing the Delete key, `filemgr_op_delete` constructed `/data/folder/..` and passed it to deletion handlers, attempting to delete the parent directory.

### Root Cause 6: Complete Absence of Multi-Item Selection
**Location:** `kernel/gui/app_filemgr.c:34`
AstraFiles 2.0 only tracked a single scalar index `int selected_idx`. There was no support for `Ctrl+Click` (toggle selection), `Shift+Click` (range selection), or `Ctrl+A` (select all). Consequently, users could never delete multiple files in a batch.

### Root Cause 7: Silent Error Masking and False Success Feedback
**Location:** `kernel/gui/app_filemgr.c:336-340`
```c
if (item->is_dir) {
    filemgr_delete_recursive(path);
    filemgr_load_dir(fm, fm->current_path);
    fm_set_status(fm, "Folder deleted successfully.");
}
```
**Mechanism:**
`filemgr_delete_recursive()` was declared `void` and ignored the return code of `vfs_rmdir()` and `vfs_unlink()`. Even when deletion failed with `VFS_ERR_BUSY` or `VFS_ERR_DENIED`, the status bar unconditionally reported `"Folder deleted successfully."`, while the folder remained visible upon refresh.

---

## 4. Planned Architectural Repairs (AstraFiles 3.0)

1. **Two-Pass Non-Skipping Recursive Deletion:** Collect all child names into a fixed buffer before unlinking, ensuring zero index shifts during iteration.
2. **Proper Button Hit-Boxes:** Expand Delete button width to $68\text{ px}$ with symmetric $10\text{ px}$ padding.
3. **Tick-Based Double-Click Detection:** Check `apic_get_ticks() - last_click_tick < 40` ($400\text{ ms}$) before triggering `open_item`.
4. **Context Menu Event Consumption:** Add explicit `return` on outside clicks to prevent click-through.
5. **System Path & `..` Deletion Guard:** Explicitly reject deletion of `".."`, `"."`, `"/"`, `"/data"`, `"/bin"`, `"/assets"`, and `"/etc"`.
6. **Multi-Item Selection Engine:** Add per-item selection flags, `Ctrl+Click`, `Shift+Click`, and `Ctrl+A` select all.
7. **Unified Deletion Backend:** Shared backend `filemgr_action_delete()` supporting single and batch deletion with real error reporting.
