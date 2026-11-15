# AstraOS — Terminal, Shell & Package Manager Architectural Audit

**Document:** `terminal_command_audit.md`  
**Date:** November 2026  
**Auditor:** AstraOS Core Engineering  
**Scope:** `kernel/gui/app_terminal.c`, `kernel/shell/shell.c`, `kernel/pkg/pkg.c`, VFS/AstraFS, Process/ELF Loader, Networking, Input/PS2 subsystems.

---

## 1. Executive Summary

A comprehensive architectural audit was performed on the AstraOS command environment. The inspection covered:
1. **Terminal Application & Window Manager Integration (`kernel/gui/app_terminal.c`)**:
   - Discovered critical GUI bug in cursor positioning: The cursor and input buffer text are hardcoded to the bottom-most row of the terminal window (`visible_rows - 1`) rather than the active prompt row.
   - Identified missing keyboard navigation keycodes: `KEY_HOME`, `KEY_END`, and `KEY_DELETE` are not defined in `input.h` or processed in `ps2.c` / `app_terminal.c`.
   - Multi-line input wrapping and dynamic cursor coordinate translation are absent.
2. **Interactive Shell Engine (`kernel/shell/shell.c`)**:
   - 28 built-in commands are registered, but many essential utilities requested for OS operation are completely missing (e.g. `mkdir`, `head`, `tail`, `grep`, `wc`, `top`, `hexdump`, `which`, `alias`, `sleep`, `find`, `tree`).
   - Existing commands lack standard flags (e.g., `ls` lacks `-l` and `-a`; `rm` lacks `-r` and `-f`; `grep` does not exist).
   - Shell control structures (`&&`, `||`, `;`, `|`, `<`) are not implemented.
   - Command-not-found package suggestion is binary `[y/N]` rather than structured menu with details and cancel options.
3. **Package Manager Subsystem (`kernel/pkg/pkg.c`)**:
   - `pkg_update` printed a fabricated HTTPS synchronization message to an unhosted URL (`https://repo.astraos.org/v1/packages.idx`).
   - Online package installation lacks network download verification; offline package extraction works via `.apkg` format with RSA-2048/SHA-256.
   - `pkg repo list` command is missing.
   - Missing-command installation does not support interactive selection or multi-option prompts.

---

## 2. Command Inventory & Status Classification

Every command has been evaluated according to five strict statuses:
- **WORKING**: Command executes correctly with real kernel/VFS operations.
- **PARTIAL**: Command exists but lacks standard flags, arguments, or error handling.
- **BROKEN**: Command exists but fails or produces inaccurate/faked results.
- **MISSING**: Command is not registered or implemented.
- **BLOCKED**: Command requires a missing subsystem or unhosted external resource.

### A. Basic Shell Commands

| Command | Status | Current Implementation Analysis & Gap | Required Action |
| :--- | :---: | :--- | :--- |
| `help` | **WORKING** | Lists built-in commands; needs update with newly implemented utilities. | Expand with categorized command index. |
| `clear` | **WORKING** | Calls `terminal_clear()` to reset scrollback buffer. | Retain. |
| `echo` | **PARTIAL** | Prints arguments; lacks `-n` (omit trailing newline) and `-e` (escape sequences). | Add `-n` and `-e` parsing. |
| `pwd` | **WORKING** | Correctly outputs `$PWD` and normalized working directory. | Retain. |
| `whoami` | **WORKING** | Outputs `$USER` or `root`. | Retain. |
| `hostname` | **WORKING** | Outputs `$HOST` (`astraos`); supports setting new hostname with argument. | Retain. |
| `date` | **WORKING** | Reads hardware RTC (`rtc_get_datetime`) and outputs UTC timestamp. | Retain. |
| `uname` | **PARTIAL** | Outputs system banner; lacks standard flags (`-a`, `-s`, `-r`, `-m`, `-v`). | Implement POSIX flags. |
| `env` | **WORKING** | Prints all active environment variables; supports key listing. | Add inline assignment support. |
| `history` | **MISSING** | History is recorded in terminal buffer but no `history` command exists in shell. | Implement `cmd_history` reading history table. |
| `exit` | **MISSING** | No built-in `exit` command to close terminal window or terminate shell session. | Implement `cmd_exit` to cleanly close window. |
| `which` | **MISSING** | Not implemented. Cannot locate commands in `$PATH` or built-in table. | Implement `cmd_which` scanning built-ins and `$PATH`. |
| `type` | **MISSING** | Not implemented. Cannot distinguish built-in, alias, or external binary. | Implement `cmd_type` evaluating resolution hierarchy. |
| `alias` | **MISSING** | No alias table or alias substitution in lexical tokenizer. | Implement alias table and `cmd_alias`. |
| `unalias` | **MISSING** | Not implemented. | Implement `cmd_unalias`. |
| `true` | **MISSING** | Not implemented. | Implement `cmd_true` returning status 0. |
| `false` | **MISSING** | Not implemented. | Implement `cmd_false` returning status 1. |
| `sleep` | **MISSING** | Not implemented. | Implement `cmd_sleep` using APIC timer ticks / `thread_sleep`. |

---

### B. Filesystem Commands

| Command | Status | Current Implementation Analysis & Gap | Required Action |
| :--- | :---: | :--- | :--- |
| `ls` | **PARTIAL** | Enumerates directory entries; lacks `-l` (long listing with sizes/types) and `-a` (hidden files). | Add `-l`, `-a`, `-h` flag parsing. |
| `cd` | **WORKING** | Normalizes paths, checks `VFS_NODE_DIR`, updates `$PWD`. | Add `cd -` (previous dir) and `cd ~`. |
| `cat` | **PARTIAL** | Reads file via `vfs_read`; lacks multi-file concatenation and stdin piping. | Support multi-file args and piped stdin. |
| `touch` | **WORKING** | Creates file or updates node timestamp via `vfs_open(O_CREAT)`. | Retain. |
| `mkdir` | **MISSING** | VFS currently lacks `vfs_mkdir`. Cannot create directories in RAMFS or AstraFS. | Implement `vfs_mkdir()` and `cmd_mkdir` with `-p`. |
| `rmdir` | **MISSING** | VFS currently lacks `vfs_rmdir`. | Implement `vfs_rmdir()` for empty directories. |
| `rm` | **PARTIAL** | Unlinks single file; lacks `-r`/`-R` recursive directory removal and `-f`. | Add `-r` and `-f` flags. |
| `cp` | **WORKING** | Copies data between VFS nodes. | Retain. |
| `mv` | **WORKING** | Renames/moves node via `vfs_rename`. | Retain. |
| `stat` | **WORKING** | Displays file type, size, and inode/block details. | Retain. |
| `df` | **WORKING** | Displays filesystem capacity, used, and free sectors for mounted volumes. | Add `-h` human-readable sizes. |
| `du` | **MISSING** | Not implemented. Cannot inspect directory disk usage. | Implement recursive size computation. |
| `find` | **MISSING** | Not implemented. Cannot search directory tree by name or pattern. | Implement recursive traversal and matching. |
| `tree` | **MISSING** | Not implemented. | Implement tree hierarchy printer. |
| `head` | **MISSING** | Not implemented. | Implement `cmd_head` with `-n <lines>`. |
| `tail` | **MISSING** | Not implemented. | Implement `cmd_tail` with `-n <lines>`. |
| `wc` | **MISSING** | Not implemented. | Implement `cmd_wc` with `-l`, `-w`, `-c`. |
| `grep` | **MISSING** | Not implemented. | Implement pattern search with `-n`, `-i`, `-v`. |
| `sort` | **MISSING** | Not implemented. | Implement line sorting with `-r`. |
| `uniq` | **MISSING** | Not implemented. | Implement adjacent line deduplication (`-c`, `-d`). |
| `diff` | **MISSING** | Not implemented. | Implement line-by-line file comparison. |
| `file` | **MISSING** | Not implemented. | Implement file magic inspection (ELF, HTML, text, APKG). |
| `basename` | **MISSING** | Not implemented. | Implement path suffix stripping. |
| `dirname` | **MISSING** | Not implemented. | Implement path directory stripping. |
| `realpath` | **MISSING** | Not implemented. | Implement canonical path resolver. |
| `readlink` | **MISSING** | Not implemented. | Implement symlink inspection (report `EINVAL` / realpath). |

---

### C. Text and File Utilities

| Command | Status | Current Implementation Analysis & Gap | Required Action |
| :--- | :---: | :--- | :--- |
| `printf` | **MISSING** | Not implemented. | Implement formatting with `\n`, `\t`, `%s`, `%d`. |
| `tee` | **MISSING** | Not implemented. | Implement dual stdout and file output with `-a`. |
| `cut` | **MISSING** | Not implemented. | Implement field delimiter cutting (`-d`, `-f`, `-c`). |
| `tr` | **MISSING** | Not implemented. | Implement character translation and deletion (`-d`). |
| `paste` | **MISSING** | Not implemented. | Implement parallel file line merging. |
| `nl` | **MISSING** | Not implemented. | Implement line numbering utility. |
| `fold` | **MISSING** | Not implemented. | Implement line wrapping utility (`-w`). |
| `rev` | **MISSING** | Not implemented. | Implement character reversal utility. |
| `strings` | **MISSING** | Not implemented. | Implement ASCII sequence extractor from binaries. |
| `hexdump` | **MISSING** | Not implemented. | Implement canonical hex + ASCII viewer (`-C`). |

---

### D. System and Diagnostic Commands

| Command | Status | Current Implementation Analysis & Gap | Required Action |
| :--- | :---: | :--- | :--- |
| `ps` | **WORKING** | Queries `sched_get_thread_stats()` to display PID, state, CPU, name. | Retain. |
| `top` | **MISSING** | Not implemented. No periodic process monitor view. | Implement snapshot process monitor. |
| `free` | **WORKING** | Queries PMM allocator and kernel heap for physical memory stats. | Retain. |
| `uptime` | **WORKING** | Reads APIC ticks and computes hours:minutes:seconds. | Retain. |
| `cpuinfo` | **WORKING** | Reads CPUID and SMP topology structures. | Retain. |
| `meminfo` | **MISSING** | Not implemented. Detailed memory breakdown missing. | Implement PMM + heap + paging info view. |
| `dmesg` | **MISSING** | Not implemented. Kernel ring buffer not exposed to shell. | Implement boot log reader from kernel buffer. |
| `lsblk` | **MISSING** | Not implemented. | Implement block device enumerator (`hd0`, `hd1`). |
| `mount` | **MISSING** | Not implemented. Cannot inspect mount table. | Implement mount table viewer (`/`, `/data`). |
| `umount` | **MISSING** | Not implemented. | Implement unmount for `/data` (AstraFS). |

---

### E. Networking Commands

| Command | Status | Current Implementation Analysis & Gap | Required Action |
| :--- | :---: | :--- | :--- |
| `ip` | **WORKING** | Displays IP addresses, subnet, gateway, MAC. | Retain. |
| `ifconfig` | **WORKING** | Formats network device statistics and addresses. | Retain. |
| `ping` | **WORKING** | Sends ICMP Echo requests and times responses. | Retain. |
| `nslookup` | **WORKING** | Sends DNS A queries to configured nameserver. | Retain. |
| `curl` | **WORKING** | Issues HTTP/HTTPS GET requests with TLS 1.3. | Retain. |
| `wget` | **MISSING** | Not implemented. | Implement file download utility over HTTP/HTTPS. |
| `netstat` | **MISSING** | Not implemented. Sockets and interfaces not listed. | Implement socket and interface listener listing. |

---

### F. Package Manager & Command-Not-Found Subsystem

| Feature / Command | Status | Current Implementation Analysis & Gap | Required Action |
| :--- | :---: | :--- | :--- |
| `pkg search` | **WORKING** | Searches in-memory catalog entries matching query. | Retain. |
| `pkg list` | **WORKING** | Lists registered installed packages and file counts. | Retain. |
| `pkg info` | **WORKING** | Displays package version, architecture, and files. | Retain. |
| `pkg install <name>` | **PARTIAL** | Verified RSA/SHA256 extraction for local `.apkg` files, but for repo packages without an archive it fell back to writing a dummy ELF stub. | Support local `.apkg` files properly; reject missing archives cleanly without dummy stubs. |
| `pkg install <path.apkg>` | **WORKING** | Verifies RSA-2048 and SHA-256 signatures, extracts transactionally. | Retain and thoroughly test. |
| `pkg remove` | **WORKING** | Enforces dependency protection, unlinks installed files. | Retain. |
| `pkg update` | **BROKEN** | Printed fake HTTPS download `https://repo.astraos.org/v1/packages.idx`. | Mark online repo as BLOCKED if unreachable; remove fabricated output; check real network status. |
| `pkg upgrade` | **PARTIAL** | Fixed message without real version comparison. | Compare installed versions against repo metadata. |
| `pkg clean` | **WORKING** | Purges staging files in `/tmp`. | Retain. |
| `pkg repo list` | **MISSING** | Not implemented in `cmd_pkg`. | Implement repository listing with honest online status. |
| **Command-Not-Found Flow** | **PARTIAL** | Only prompts `Install package '<pkg>' now? [y/N]`. Lacks multi-option menu `[1] Install [2] Info [3] Cancel`, package size, dependencies. | Implement required 3-option interactive flow. |

---

## 3. Terminal GUI & Cursor Positioning Bug Analysis

### Root-Cause Analysis
In `kernel/gui/app_terminal.c`:
- **Line 165**: `int cur_y = 4 + (visible_rows - 1) * TERM_FONT_HEIGHT;`
  The vertical coordinate for both typed input characters and the blinking cursor is hardcoded to `visible_rows - 1` (row 23 of a 24-row window).
- **Line 146**: Lines 1 through `total_lines` are rendered sequentially starting at `y = 4`. When the terminal is first opened, `total_lines == 1`. The prompt string (`root@astraos:[/]$ `) is printed into scrollback line 1 and rendered at `y = 4` (top row).
- **Resulting Defect**: The prompt is rendered at row 0 (top line), but the user's keystrokes and cursor are rendered 23 rows below at the very bottom of the window, leaving an unsightly 22-line blank gap between the prompt and the cursor!
- **Editing Inaccuracies**:
  - `KEY_HOME`, `KEY_END`, and `KEY_DELETE` are not defined in `input.h` or handled in `terminal_on_event()`.
  - Multi-line input wrapping is not handled: when input exceeds column 80, the cursor overflows horizontally rather than wrapping to the next line.
  - When the user presses `Left` arrow, the cursor moves, but if it wraps, it is miscalculated.

### Mathematical Coordinate Correction Plan
Let `prompt_len = cur_line->len` (e.g. 17 chars).
Let `cursor_idx = term->cursor_pos`.
The prompt is rendered on the active input line:
- Active line number in scrollback: `active_line = term->total_lines`.
- Active line visual row offset: `row_offset = active_line - start_line`.
- Starting visual Y: `base_y = 4 + row_offset * TERM_FONT_HEIGHT`.
- Total character position: `char_pos = prompt_len + cursor_idx`.
- Resolved column: `col = char_pos % TERM_DEFAULT_COLS`.
- Wrapped row offset: `extra_rows = char_pos / TERM_DEFAULT_COLS`.
- Final Cursor Coordinates:
  $$\text{cursor\_x} = 8 + \text{col} \times \text{TERM\_FONT\_WIDTH}$$
  $$\text{cursor\_y} = \text{base\_y} + \text{extra\_rows} \times \text{TERM\_FONT\_HEIGHT}$$
Input text rendering will use the exact same calculation to ensure 100% pixel-perfect congruence.

---

## 4. Implementation Strategy & Next Steps

1. **Step 1: Fix Terminal Cursor Positioning & Input Keys**:
   - Add `KEY_HOME`, `KEY_END`, `KEY_DELETE` to `kernel/include/kernel/input.h`.
   - Map extended PS/2 scancodes `0x47`, `0x4F`, `0x53` in `kernel/drivers/ps2.c`.
   - Re-architect cursor and text rendering in `kernel/gui/app_terminal.c` to use active line coordinates and proper line wrapping.
   - Add automated cursor coordinate tests.
2. **Step 2: Implement Missing Basic & Intermediate Commands**:
   - Implement `vfs_mkdir()` and `vfs_rmdir()` in `kernel/fs/vfs.c`.
   - Implement all missing Section 2 utilities in `kernel/shell/shell.c`.
   - Add practical flags (`ls -l`, `ls -a`, `grep -n`, `head -n`, `tail -n`, `wc -l`, etc.).
3. **Step 3: Shell Control Features**:
   - Implement command chaining (`&&`, `||`, `;`).
   - Implement in-memory pipe simulation (`cmd1 | cmd2`).
   - Implement input redirection (`<`).
4. **Step 4: Interactive Command-Not-Found & Package Manager**:
   - Implement the interactive 3-option menu for missing commands.
   - Implement `pkg repo list`.
   - Remove fake download output; honest online status reporting (mark BLOCKED if offline).
   - Ensure local `.apkg` installation is 100% verified.
5. **Step 5: Verification & Regression**:
   - Run unit tests, host suites, QEMU regression, and VirtualBox verification.

