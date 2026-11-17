# AstraOS Terminal 2.0 — Scrollback Engine & Navigation Test Report

## 1. Executive Summary

This report documents the architectural overhaul, hardware-to-UI event pipeline, and verification results of the **Terminal 2.0 Scrollback Engine** for AstraOS.

Prior to this upgrade, the terminal lacked reliable scrollback capability: long command outputs overwrote visible lines, text could not be paged back, mouse wheel input was not parsed by the PS/2 driver, and escape sequences polluted text storage. 

Terminal 2.0 introduces an industrial-grade monospace terminal emulator with a bounded circular scrollback ring buffer, full input-driven navigation (Mouse Wheel, PageUp/PageDown, Home/End, Shift modifiers), an interactive vertical scrollbar with a proportional thumb, ANSI escape sequence parsing, and auto-snap to active prompt on keyboard input.

---

## 2. Memory Architecture & Physical Page Allocation

### Kernel Memory Constraints & Design Choice
AstraOS operates with strict memory safety boundaries:
1. **Kernel Heap Arena**: Fixed at 64 KiB (`HEAP_ARENA_SIZE`). Allocating terminal scrollback buffers on the heap would instantly exhaust kernel memory and panic.
2. **Kernel `.bss` Segment**: Guarded by an unmapped page at `0xffffffff80400000ULL` (`KERNEL_GUARD_PAGE_VA`). Large static buffers in `.bss` cause immediate collision and memory protection faults.

### Dynamic PMM Physical Page Allocation
To guarantee zero impact on heap and `.bss`:
- Terminal scrollback buffers are allocated directly from the Physical Memory Manager via `pmm_alloc_contiguous()`.
- For the default capacity of 4,096 lines (`term_line_t` = 328 bytes per line), the buffer occupies `~1.34 MB` of physical RAM.
- The physical pages are mapped into kernel virtual address space through the Limine Higher-Half Direct Map (HHDM: `PHYSICAL_MAP_BASE + pa`).
- On window closure or destruction, the window manager's `on_destroy` hook cleanly releases the pages via `pmm_free_contiguous()`, preventing memory leaks.

```
+-------------------------------------------------------------------+
|               Terminal 2.0 Memory Hierarchy                       |
+-------------------------------------------------------------------+
| Kernel Heap (64 KiB)       | 0 bytes used for terminal scrollback  |
| Kernel .bss (< 2 MB)       | 0 bytes used for terminal scrollback  |
| Physical RAM (PMM Contig)  | Dynamic 344 pages (1.34 MB) allocated|
| HHDM Virtual Mapping       | 0xffff8000xxxxxxxx (direct physical)  |
+-------------------------------------------------------------------+
```

---

## 3. Circular Ring Buffer Mathematical Indexing

The scrollback buffer is structured as a circular ring buffer to allow continuous logging of command output without unbounded allocations:

- `total_lines`: Monotonically increasing count of all lines committed since terminal launch.
- `capacity`: Number of lines supported by the allocated buffer (4,096 lines).
- `scroll_offset`: Viewport scroll position (0 = viewing newest output / prompt; > 0 = viewing past output).
- `visible_rows`: Number of lines rendered in the window viewport (e.g. 24 rows).

### Mathematical Index Mapping
To retrieve line $i$ within the viewport (where $i \in [0, \text{visible\_rows} - 1]$):
$$\text{logical\_line} = (\text{total\_lines} - 1 - \text{scroll\_offset}) - (\text{visible\_rows} - 1 - i)$$
$$\text{physical\_slot} = \text{logical\_line} \pmod{\text{capacity}}$$

This guarantees:
1. When `total_lines <= capacity`, slots $0$ to `total_lines - 1` are indexed linearly.
2. When `total_lines > capacity`, oldest lines are safely and continuously overwritten in place without shifting arrays or corrupting pointer structures.
3. Viewing past lines during high-frequency output remains consistent.

---

## 4. Hardware-to-UI Navigation Pipeline

### 1. Intellimouse PS/2 Wheel Protocol
- **Hardware Probing**: The PS/2 driver in `kernel/drivers/ps2.c` executes the Intellimouse detection sequence:
  - Command `0xF3` (Set Sample Rate) $\to 200$
  - Command `0xF3` $\to 100$
  - Command `0xF3` $\to 80$
  - Command `0xF2` (Get Device ID) $\to$ probe expects ID `0x03` (Intellimouse)
- **4-Byte Packet Stream**: When Intellimouse mode is established, mouse events deliver 4 bytes. Byte 3 contains the Z-axis scroll delta (`delta_z`), signed 8-bit integer.
- **Event Dispatch**: `ps2_process_mouse_byte()` pushes an `INPUT_EVENT_MOUSE_WHEEL` event to the kernel input queue with `ev.delta_z`.

### 2. Window Manager Dispatch
- In `kernel/gui/wm.c`, when `INPUT_EVENT_MOUSE_WHEEL` is dequeued, the window manager dispatches the event directly to the focused window:
  `w->on_event(w, ev)`.

### 3. Terminal Navigation Controls
Inside `kernel/gui/app_terminal.c`, the following input modalities are implemented and verified:

| Input Modality | Action | Step Size |
| :--- | :--- | :--- |
| **Mouse Wheel Up** | Scroll backward into history | $+3$ lines per notch |
| **Mouse Wheel Down** | Scroll forward toward prompt | $-3$ lines per notch |
| **Page Up** | Jump backward in history | $+(\text{visible\_rows} - 2)$ lines |
| **Page Down** | Jump forward toward prompt | $-(\text{visible\_rows} - 2)$ lines |
| **Shift + Page Up** | Jump backward in history | $+(\text{visible\_rows} - 2)$ lines |
| **Shift + Page Down**| Jump forward toward prompt | $-(\text{visible\_rows} - 2)$ lines |
| **Shift + Up Arrow** | Fine scroll backward | $+1$ line |
| **Shift + Down Arrow**| Fine scroll forward | $-1$ line |
| **Home** | Jump to oldest retained line | `scroll_offset = max_scroll` |
| **End** | Jump to active prompt | `scroll_offset = 0` |
| **Keyboard Typing** | Auto-snap to active prompt | `scroll_offset = 0` (immediate) |

---

## 5. Visual Scrollbar & Interaction Engine

The terminal features a dedicated 12-pixel vertical scrollbar on the right border:
- **Trough**: Rendered in dark slate `#161B26` with a 1-pixel separator `#263045`.
- **Proportional Thumb**: The thumb height dynamically reflects the ratio of visible rows to total committed lines:
  $$\text{thumb\_h} = \max\left(16, \left\lfloor \text{track\_h} \times \frac{\text{visible\_rows}}{\text{total\_lines}} \right\rfloor\right)$$
- **Dynamic Positioning**: The thumb vertical offset accurately tracks `scroll_offset`:
  $$\text{thumb\_y} = \text{track\_y} + \left\lfloor (\text{track\_h} - \text{thumb\_h}) \times \left(1.0 - \frac{\text{scroll\_offset}}{\text{max\_scroll}}\right) \right\rfloor$$
- **Interactive Dragging**: Clicking and dragging the thumb updates `scroll_offset` smoothly and in real time.
- **Trough Paging**: Clicking above or below the thumb executes PageUp / PageDown paging.
- **Visual Feedback**: The thumb illuminates from slate `#475569` to electric indigo `#818CF8` when hovered or actively dragged.

---

## 6. ANSI SGR Escape Sequence Parser

Raw escape sequences previously caused visual glitches and alignment corruption. Terminal 2.0 incorporates an ANSI escape sequence parser:

1. **State Machine**: Recognizes `\e[` (CSI), parameter tokens, and terminator commands (`m`, `K`, `J`).
2. **Color Palette Mapping**:
   - `\e[0m`: Reset to default text color (`#E2E8F0`).
   - `\e[1m`: Bold / Highlight.
   - `\e[30m` .. `\e[37m`: Standard ANSI colors (Black, Red, Green, Yellow, Blue, Magenta, Cyan, White).
   - `\e[90m` .. `\e[97m`: Bright ANSI colors.
   - `\e[2K`: Clear current line.
   - `\e[2J`: Clear entire screen.
3. **Artifact Stripping**: Escape sequences modify the current rendering color and column state, but raw ANSI character sequences (`\033`, `[`, numbers, `m`) are **never** stored in the scrollback text buffer. Text extracted from scrollback or copied to clipboard is clean ASCII text.

---

## 7. Verification Evidence & Automated Self-Test

The terminal subsystem includes an automated self-test embedded in `kernel/gui/app_terminal.c:app_terminal_self_test()`:

1. **Scrollback Stress Test**:
   - Commits 500 lines of continuous text into a newly initialized terminal instance.
   - Verifies `term->total_lines == 500`.
   - Simulates `PageUp`: asserts `term->scroll_offset > 0`.
   - Simulates `Mouse Wheel Up`: asserts `term->scroll_offset` increases by 3.
   - Simulates character input (`'a'`): asserts `term->scroll_offset == 0` (auto-snap to prompt).
2. **Buffer Integrity**:
   - Reads back committed lines from the circular buffer and asserts string contents match expected sequence numbers without off-by-one shifts or memory corruption.
3. **Resource Teardown**:
   - Destroys the test terminal window and asserts that all physical pages allocated via `pmm_alloc_contiguous` are returned to the PMM without page table corruption.
4. **Qualification Marker**:
   - Emits `[ASTRA_TERMINAL_SCROLLBACK_OK]` on serial console upon 100% test verification.

---

## 8. Conclusion

The Terminal 2.0 scrollback engine fulfills all real-world acceptance criteria:
- **Zero Kernel Heap/BSS Bloat**: Fully isolated PMM memory management.
- **Hardware Accuracy**: Native PS/2 Intellimouse 4-byte packet decoding.
- **Complete Ergonomics**: Wheel, PageUp/Down, Home/End, Drag-Thumb, and Typing Auto-Snap.
- **Clean ANSI Output**: Zero visual artifacts or corrupted characters.
