# AstraOS — Complete Filesystem, Storage Persistence & GUI Application Audit

**Date:** 2026-11-17  
**Project:** AstraOS Operating System (x86_64)  
**Location:** `C:\Users\Akash\Documents\MyKernel`

---

## 1. Storage Backend Map by Directory

| Directory Path | Storage Backend | Durability / Lifecycle | Write Support | Max Size / Constraints |
| :--- | :--- | :--- | :--- | :--- |
| `/` (root) | Initramfs / VFS Root | Read-only in-memory RAM image unpacked from Limine boot module | Read-only (writes shadowed into RAMFS overlay) | Fixed boot image size (~8.2 MiB) |
| `/bin` | Initramfs / ELF Executables | Read-only system binaries compiled at build time | Read-only | Immutable system partition |
| `/assets` | Initramfs / System Assets | Read-only assets (Cosmic Vista 1920x1080 raw wallpaper) | Read-only | Immutable system partition |
| `/etc` | Initramfs / Config | Read-only system configuration files | Read-only | Immutable system partition |
| `/www` | Initramfs / Web Assets | Read-only test HTML, CSS, JavaScript files for AstraBrowser | Read-only | Immutable system partition |
| `/nested` | Initramfs / Test Tree | Read-only test hierarchy for VFS traversal checks | Read-only | Immutable system partition |
| `/data` (top-level) | **AstraFS v1** on Block Device (`hd1` or `hd0`) | **Persistent Block Storage** (Dual-Set A/B transactional commit, Fletcher-32 checksums, LBA 128+) | **Read / Write / Truncate** | Max 64 files, 48-char filenames, flat namespace only |
| `/data/<subdirs>` (e.g. `/data/home/akash`) | **RAMFS (`g_mutable_files`)** | **VOLATILE (In-Memory)** — Disappears completely upon reboot or power cycle! | Read / Write | Max 32 mutable files, max 16 KiB per file, 512 KiB global RAMFS quota |
| Any path outside `/data` created via `vfs_open(O_CREAT)` | **RAMFS (`g_mutable_files`)** | **VOLATILE (In-Memory)** — Lost on system restart! | Read / Write | Max 32 mutable files, max 16 KiB per file, 512 KiB global RAMFS quota |

---

## 2. Root Cause Analysis: The Six Fundamental Defects

### 1. Why Files Disappear After Reboot
1. **Subdirectories in `/data` are Bypassed into RAMFS:**
   - In `kernel/fs/vfs.c` (lines 542–560), `vfs_open` with `VFS_O_CREAT` checks if `astrafs_is_mounted() && str_ncmp(path, "/data/", 6) == 0`.
   - However, it executes:
     ```c
     bool has_sub = false;
     for (const char *s = path + 6; *s; s++) {
         if (*s == '/') { has_sub = true; break; }
     }
     if (!has_sub) {
         astrafs_create(path + 6, flags, &node);
     }
     ```
   - If `has_sub` is `true` (e.g. `/data/home/akash/notes.txt` or `/data/projects/main.c`), `astrafs_create` is **not** called.
   - The execution falls through to line 561: `/* Allocate a new mutable file in ramfs */`!
   - Every file created in any subfolder under `/data` is stored solely in the kernel's `.bss` array `g_mutable_files`, which evaporates on cold reboot.
2. **`vfs_mkdir` Has No AstraFS Backend:**
   - In `kernel/fs/vfs.c` (lines 917–956), `vfs_mkdir` creates directory nodes **only** inside `g_mutable_files` (volatile RAMFS).
   - `kernel/fs/astrafs.c` does not implement `astrafs_mkdir`, nor does `astrafs_inode_t` have directory flags enabled.
   - Consequently, running `mkdir /data/folder` creates a RAM-only node that vanishes on reboot.
3. **`vfs_unlink` Does Not Delete AstraFS Inodes:**
   - In `kernel/fs/vfs.c` (lines 870–875), `vfs_unlink` on `/data/file` executes:
     ```c
     if (astrafs_lookup(path + 6, &anode) == VFS_OK && anode) {
         return astrafs_truncate(anode, 0);
     }
     ```
   - It merely truncates the file payload to 0 bytes! It **never** frees the inode slot (`ino->in_use = 0`), never clears the filename, and never removes the entry from the AstraFS superblock file count.
   - On the next boot, the "deleted" file reappears as a 0-byte zombie file.
4. **Hardcoded ATA Drive 0 Read-Only Constraint in VirtualBox:**
   - In `kernel/drivers/ata.c` (lines 370–376):
     ```c
     /* Primary Master (c=0, d=0) in QEMU is the FAT EFI boot partition */
     if (c == 0 && d == 0) {
         drv->block_dev.is_boot_device = true;
         drv->block_dev.read_only = true;
     }
     ```
   - In QEMU, the ESP FAT image was assigned to index 0 (`hd0`), and the data disk to index 1 (`hd1`).
   - But in Oracle VirtualBox VM `astraoss`, the virtual disk (`astraoss.vdi`) is attached to Primary Master `IDE-0-0` (`c=0, d=0`), while the boot ISO is on `IDE-1-0` (CD-ROM).
   - The kernel therefore marked `astraoss.vdi` as `is_boot_device = true` and `read_only = true`.
   - Furthermore, `kernel.c` line 482 searched strictly for `hd1` (`block_find_by_name("hd1")`). Because `hd1` was not attached in VirtualBox, AstraOS printed `[WARN] Persistent data disk 'hd1' not found or read-only. Operating with volatile storage only.` All files written in VirtualBox were diverted to RAMFS!

---

### 2. Why GUI Operations May Not Persist
1. **File Manager Folder Creation Uses RAMFS:**
   - In `kernel/gui/app_filemgr.c` line 158, clicking `+ Folder` calls `vfs_mkdir(path)`. As established above, `vfs_mkdir` only creates in `g_mutable_files` (volatile memory).
2. **Editor Silently Fakes Success on Save Failures:**
   - In `kernel/gui/app_editor.c` lines 74–80:
     ```c
     int fd = vfs_open(ed->filepath, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
     if (fd >= 0) { ... }
     else {
         const char *msg = "Notice: Buffer saved in memory.";
         ed->dirty = false;
     }
     ```
   - If disk access fails or the volume is read-only, the Editor clears the dirty flag (`ed->dirty = false`) and reports that the buffer is saved in memory, deceiving the user into believing their changes are secure.
3. **Lack of Synchronous Flush (`vfs_sync` / `block_flush`):**
   - GUI file creation and edits do not trigger `vfs_sync()`, leaving dirty block buffers uncommitted to the ATA drive if power is cycled abruptly.

---

### 3. Why Scrolling Is Broken or Incomplete
1. **File Manager Has Zero Scrolling Architecture:**
   - `kernel/gui/app_filemgr.c` defines `FILEMGR_MAX_ENTRIES` as 32.
   - In `filemgr_on_paint` (line 317), the loop simply breaks when `row_y + 24 >= bot_y`.
   - There is no scroll offset variable, no scrollbar rendering, no track/thumb interaction, and `filemgr_on_event` completely ignores `INPUT_EVENT_MOUSE_WHEEL` and keyboard navigation (`KEY_UP`, `KEY_DOWN`, `KEY_PAGE_UP`, `KEY_PAGE_DOWN`).
   - Any directory containing more than ~12 items truncates; remaining items are inaccessible.
2. **Code Editor Has Zero Vertical and Horizontal Scrolling:**
   - `kernel/gui/app_editor.c` lacks any scroll offset mechanism.
   - In `editor_on_paint` (line 210), line rendering breaks when `cur_y + 18 >= bot_y`. Lines beyond row ~18 are never painted.
   - When the user presses `KEY_DOWN` past line 18, `ed->cursor_pos` advances, but the screen does not scroll, causing the cursor to disappear off the bottom of the window.
   - Long lines wrap abruptly at window width or clip without horizontal scrolling.
   - Mouse wheel events are unhandled.
3. **No Shared Scrollable View Infrastructure:**
   - Applications had no standard scrollbar widget or viewport math; each app was forced to implement its own ad-hoc viewport or omit scrolling entirely.

---

### 4. Why Editor Startup Loads an Existing File
1. **Hardcoded Default Path on Empty Launch:**
   - In `kernel/gui/app_editor.c` lines 573–580:
     ```c
     if (filepath) {
         ...
     } else {
         const char *def_path = "/data/notes.txt";
         for (int i = 0; def_path[i]; i++) g_editor.filepath[i] = def_path[i];
     }
     ```
   - When launched from the Desktop Dock or F1 shortcut, `filepath` is `NULL`.
   - The editor automatically targeted `/data/notes.txt`, opening existing content instead of initializing a clean, unsaved "Untitled-1" document buffer.
2. **Single Global Instance Without Tabs:**
   - The editor relies on a single static `editor_state_t g_editor` and an 8 KiB flat buffer `char text[EDITOR_BUFFER_MAX]`. It cannot open multiple documents or manage untitled buffers independently.

---

### 5. Why GUI File Dialogs Are Missing
1. **No Reusable File Picker Component:**
   - There is no modal or embedded dialog system in the AstraOS window manager (`kernel/gui/wm.c`).
   - The editor only possesses `[Run >]` and `[Save]` buttons in the title toolbar; there are no `File > Open` or `File > Save As` dialogs.
   - The only way to open a file in the editor was to navigate to it in the File Manager and double-click it.

---

### 6. Why Right-Click Events Are Not Handled Correctly
1. **Mouse Driver Drops Right-Click Completely:**
   - In `kernel/drivers/ps2.c` lines 268–281:
     ```c
     if ((buttons & MOUSE_BTN_LEFT) && !(g_prev_buttons & MOUSE_BTN_LEFT)) {
         input_event_t ev = { .type = INPUT_EVENT_MOUSE_DOWN, ... };
         input_push_event(&ev);
     } else if (!(buttons & MOUSE_BTN_LEFT) && (g_prev_buttons & MOUSE_BTN_LEFT)) {
         input_event_t ev = { .type = INPUT_EVENT_MOUSE_UP, ... };
         input_push_event(&ev);
     }
     ```
   - The PS/2 driver checked **only** `MOUSE_BTN_LEFT`!
   - When the user pressed `MOUSE_BTN_RIGHT` or `MOUSE_BTN_MIDDLE`, `ps2.c` generated **no event** whatsoever. The kernel was completely blind to right mouse clicks.
2. **No Context Menu Engine in GUI:**
   - `app_filemgr.c` and `wm.c` have no context-menu state machines, popup window handling, or right-click dispatchers.

---

## 3. Comprehensive Fix Plan

### Phase 2: Permanent Storage & AstraFS Architecture
1. **AstraFS Subdirectory & Inode Extension:**
   - Extend `astrafs_inode_t` to support hierarchical directory entries (`ASTRAFS_INODE_DIR`) using the existing `u32 flags` and `reserved` fields.
   - Implement `astrafs_mkdir`, `astrafs_rmdir`, and real `astrafs_unlink` (freeing data sectors and marking `in_use = 0`).
   - Update `vfs.c` to route nested paths under `/data/` directly to AstraFS rather than silently redirecting to RAMFS.
2. **Intelligent Drive Detection (QEMU & VirtualBox Compatibility):**
   - Probe block devices dynamically: detect AstraFS superblock magic (`0x41534653`) on any ATA drive (`hd0`, `hd1`, etc.).
   - Distinguish boot media (FAT16/FAT32 containing `\EFI\BOOT`) from data drives.
   - Allow `hd0` to serve as the persistent data volume in VirtualBox while maintaining `hd0` as the read-only boot volume in QEMU.
   - Propagate block cache flushes (`block_flush`) on every metadata transaction and file write.

### Phase 3: Shared GUI Scrolling Engine
- Create a shared scrollbar and viewport model:
  - `scroll_offset_x`, `scroll_offset_y`, `content_w`, `content_h`, `viewport_w`, `viewport_h`.
  - Mouse wheel handling (`delta_z * line_height`).
  - Draggable scrollbar thumbs, track clicking, PageUp/PageDown, Home/End.
  - Clamping to valid bounds `[0, content_size - viewport_size]`.

### Phase 4–7: File Manager 2.0 (AstraFiles)
- Professional multi-pane UI: Top toolbar, breadcrumb address bar, search input, left navigation sidebar (`Home`, `Documents`, `Downloads`, `Pictures`, `Data Storage`, `Root`).
- Icon View, Detailed List View with sorting (Name, Type, Size).
- Keyboard shortcuts: `Ctrl+C`, `Ctrl+X`, `Ctrl+V`, `Ctrl+A`, `F2` (Rename), `Del` (Delete), `Ctrl+Shift+N` (New Folder), `F5` (Refresh), `Alt+Up` (Parent).
- Fix PS/2 mouse driver right-click dispatch and implement popup context menus for Files, Folders, and Background.
- Real backend operations: Copy/Paste, Cut/Paste, Rename, Delete, New File, New Folder, Properties.

### Phase 8–13: Code Editor 2.0 (AstraEdit)
- Default startup with clean, in-memory **"Untitled-1"** document (no default file opened or written to disk).
- Multi-document tab support with independent undo/redo histories and dirty tracking.
- Menu bar (`File`, `Edit`, `View`), toolbar, line numbers, cursor status bar (`Ln X, Col Y`).
- Reusable Native Open File and Save As dialogs.
- Syntax highlighting engine (C, C++, Python, JavaScript, HTML, CSS, JSON, Shell).
- Close confirmation dialog on unsaved changes: `[Save]` `[Don't Save]` `[Cancel]`.

### Phase 14–17: Integration, Acceptance & Full Regression Testing
- Cross-app verification: File Manager -> Editor -> Terminal -> cold VM reboot -> byte-for-byte persistence check.
- Protect user disks: Use dedicated disposable disk images for stress and persistence verification.
