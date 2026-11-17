# AstraOS Terminal 2.0 — Architectural Investigation & System Design

## 1. Executive Summary

This architecture document presents the comprehensive audit of the AstraOS Terminal, Shell, and Execution Subsystems, answering the 8 core investigation questions and outlining the engineering design for **Terminal 2.0**.

---

## 2. Architectural Investigation (Phase 1 Audit)

### Question 1: Why Scrolling Currently Fails
1. **Tiny Ring Buffer**: `TERM_SCROLLBACK_LINES` was statically hardcoded to `256` lines.
2. **Broken Ring Indexing Math**: In `terminal_on_paint()`, `start_line` was calculated from unbounded `term->total_lines` without bounding `scroll_offset` to the actual retained capacity (`capacity = 256`). When output exceeded 256 lines (e.g. running `help`, long `cat`, or `dmesg`), scrolling up indexed into slots that were already overwritten by newer output, causing corrupted, repeated, or blank lines.
3. **Keystroke Viewport Clobbering**: Every navigation key (`KEY_HOME`, `KEY_END`, `KEY_LEFT`, `KEY_RIGHT`, `KEY_UP`, `KEY_DOWN`, `KEY_BACKSPACE`, `KEY_DELETE`) executed unconditional `term->scroll_offset = 0`, instantly snapping the viewport to the bottom.
4. **No Mouse Event Handling**: `terminal_on_event()` only intercepted `INPUT_EVENT_KEY_DOWN`. It had zero handling for `INPUT_EVENT_MOUSE_DOWN`, `INPUT_EVENT_MOUSE_MOVE`, `INPUT_EVENT_MOUSE_UP`, or mouse wheel events.
5. **No Scrollbar**: The terminal renderer drew no scrollbar, no trough, and no thumb.
6. **No Shift+PageUp**: Modifier bits (`KEY_MOD_SHIFT`) were unhandled for paging keys.

### Question 2: Fixed-Size Text Rows & Reflow
* The legacy terminal defined `term_line_t` with static `chars[80]` and `colors[80]`.
* Output wrapping simply advanced to the next row at column 80 without tracking logical line continuations.
* Terminal window resizing was not handled by the client renderer: visible rows were capped at `TERM_DEFAULT_ROWS` (24).

### Question 3: History & Scrollback Representation
* **Command History**: Stored as a fixed 32-entry array of 256-byte strings (`history[32][256]`) inside `terminal_state_t`.
* **Scrollback**: Stored in a static inline array `scrollback[256]` in `terminal_state_t`.
* **Critical Memory Constraint**: The kernel heap arena is fixed at **64 KiB** (`HEAP_ARENA_SIZE = 64ULL * 1024ULL` in `kernel/include/kernel/heap.h`). Furthermore, the kernel `.bss` section has approximately **20 KiB** headroom before the unmapped guard page at `0xffffffff80400000`. Statically allocating or heap-allocating 10,000 lines ($10,000 \times 164 \text{ B} \approx 1.64\text{ MB}$) would cause immediate kernel panic. Scrollback buffers must be allocated using physical page frames via `pmm_alloc_contiguous()` and mapped through the Limine Higher Half Direct Map (HHDM), consuming zero bytes of the heap arena and zero bytes of `.bss`.

### Question 4: Input Event Path
```
[Hardware PS/2 Keyboard & Mouse]
                │
                ▼ (Port 0x60 IRQ1 / IRQ12 & monitor poll)
[kernel/drivers/ps2.c]
                │
                ▼ input_push_event()
[kernel/drivers/input.c (Circular Event Queue)]
                │
                ▼ input_pop_event()
[kernel/monitor/monitor.c / desktop.c]
                │
                ▼ wm_handle_input_event()
[kernel/gui/wm.c]
                │
                ▼ win->on_event()
[kernel/gui/app_terminal.c (terminal_on_event)]
```

### Question 5: Output Generation and Display Pipeline
```
[Shell Command / User Process]
                │
                ▼ sys_write() / shell_out_fn (terminal_shell_output)
[kernel/gui/app_terminal.c (terminal_print)]
                │ (ANSI sequence parsing, line break & wrap)
                ▼
[Scrollback Ring Buffer (PMM/HHDM Allocated)]
                │
                ▼ wm_render() -> win->on_paint() (terminal_on_paint)
[Graphics Surface / Backbuffer (gfx_draw_char)]
                │
                ▼ gfx_swap_buffers()
[Hardware Framebuffer]
```

### Question 6: Command Execution Model (Kernel vs User-Space)
* Built-in commands (`ls`, `cat`, `grep`, `pwd`, etc.) reside in `g_builtins[]` inside `kernel/shell/shell.c` and execute directly in Ring 0 kernel context.
* External binaries (e.g. `/bin/*.elf`) execute in Ring 3 isolated user address spaces via `process_create_elf_file()`.
* Terminal 2.0 introduces user-space command execution, standard stream routing, and standalone CLI binaries.

### Question 7: Working vs Partial vs Blocked Commands
* **WORKING (Solid in Kernel Shell)**: `help`, `clear`, `echo`, `pwd`, `whoami`, `hostname`, `date`, `uname`, `history`, `which`, `type`, `alias`, `unalias`, `true`, `false`, `sleep`, `ls`, `cd`, `cat`, `touch`, `mkdir`, `rmdir`, `rm`, `cp`, `mv`, `stat`, `df`, `du`, `find`, `tree`, `head`, `tail`, `wc`, `grep`, `sort`, `uniq`, `diff`, `file`, `basename`, `dirname`, `realpath`, `readlink`, `printf`, `tee`, `cut`, `tr`, `paste`, `nl`, `fold`, `rev`, `strings`, `hexdump`, `ps`, `top`, `free`, `uptime`, `cpuinfo`, `meminfo`, `dmesg`, `lsblk`, `mount`, `umount`, `ip`, `ifconfig`, `ping`, `nslookup`, `curl`, `wget`, `netstat`, `pkg`, `python`, `run`, `cc`, `sh`.
* **PARTIAL**: Redirection & pipe buffer (in-memory ring buffer, needs formal process pipe syscalls); scrollback navigation in terminal.
* **MISSING / TO IMPLEMENT**: `whereis`, `printenv`, `export`, `unset`, `yes`, `less`, `more`, `cmp`, `comm`, `xargs`, `sync`, `chmod`, `chown`, `ln`, `mktemp`, `truncate`, `kill`, `jobs`, `fg`, `bg`, `time`, `watch`, `pgrep`, `pkill`, `tar`, `gzip`, `gunzip`, `sha256sum`, `md5sum`, `base64`.

### Question 8: Missing Kernel Primitives for Full User-Space CLI
1. Stdin file descriptor (fd 0) support in `sys_read()`.
2. Inheritable standard file descriptor table and redirection (`sys_dup`, `sys_dup2`, `sys_pipe`).
3. Command argument passing (`argc`, `argv`, `envp`) into user-space stack during ELF setup.
4. Process termination signaling (`sys_kill`, SIGINT on Ctrl+C).
5. Working directory inheritance (`sys_getcwd`, `sys_chdir`).
6. Unlink and directory syscalls (`sys_unlink`, `sys_mkdir`, `sys_rmdir`).

---

## 3. Terminal 2.0 Architectural Specification

```
┌─────────────────────────────────────────────────────────────┐
│                    AstraOS Terminal 2.0                     │
├─────────────────────────────────────────────────────────────┤
│  ┌───────────────────────────────────────────────────────┐  │
│  │ Viewport (Client Grid: COLS x ROWS)                   │▲ │
│  │ Rendered with 8x16 Monospace Font                     │█ │
│  │ ANSI 16-Color Palette + SGR Styles                    │█ │ Scrollbar
│  │ Text Selection with Drag Highlight                    │█ │ (Interactive
│  │ Integrated Input Cursor (Blink, Line Editing)         │█ │  Thumb &
│  │                                                       │▼ │  Trough)
│  └───────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────┐  │
│  │ Scrollback Ring Buffer (PMM-backed, 10,000 Lines)     │  │
│  │ - Monotonic total_lines counter                       │  │
│  │ - Bounded capacity & rollover without corruption      │  │
│  │ - Preserved line length & attribute map               │  │
│  └───────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────┐  │
│  │ Terminal Controller & Event Dispatcher                │  │
│  │ - Mouse Wheel (Intellimouse PS/2 packet decoding)     │  │
│  │ - PageUp, PageDown, Shift+PageUp, Shift+PageDown      │  │
│  │ - Home / End navigation                               │  │
│  │ - Scrollbar mouse drag & trough paging                │  │
│  │ - Clipboard Copy (Ctrl+C) / Paste (Ctrl+V)            │  │
│  │ - Auto-snap to prompt on keyboard input               │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```
