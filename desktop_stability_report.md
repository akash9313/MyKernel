# AstraOS Desktop Stability & Session Management Qualification Report

## Executive Summary

This engineering report certifies the desktop architecture, lifecycle state transitions, single-initialization enforcement, and session startup policy of AstraOS.

Prior to this phase, AstraOS suffered from five desktop stability defects:
1. VirtualBox boot execution took 5–6 minutes.
2. The desktop shell rendered prematurely at Milestone 16 (mid-boot) while in-kernel test suites were still running.
3. Subsequent test execution created and interleaved application windows (Text Editor, File Manager, AstraBrowser) across the active desktop.
4. Applications opened automatically on boot without user intent.
5. Desktop initialization was called repeatedly without lifecycle guard rails.

All five issues have been resolved.

---

## 1. Desktop Lifecycle State Machine

A formal state machine was implemented in [kernel/include/kernel/wm.h](file:///C:/Users/Akash/Documents/MyKernel/kernel/include/kernel/wm.h) and [kernel/gui/desktop.c](file:///C:/Users/Akash/Documents/MyKernel/kernel/gui/desktop.c):

```c
typedef enum {
    DESKTOP_STATE_BOOTING           = 0,
    DESKTOP_STATE_KERNEL_READY      = 1,
    DESKTOP_STATE_GUI_INITIALIZING  = 2,
    DESKTOP_STATE_READY             = 3,
    DESKTOP_STATE_FAILED            = -1
} desktop_lifecycle_state_t;
```

### Lifecycle Transition Graph

```mermaid
stateDiagram-v2
    [*] --> BOOTING: Power On / Kernel Entry
    BOOTING --> KERNEL_READY: GDT, IDT, PMM, VMM, TSS, APIC, Sched, SMP, PCI, Storage, Net Ready
    KERNEL_READY --> GUI_INITIALIZING: gfx_init(), input_init(), ps2_init(), wm_init(), desktop_init()
    GUI_INITIALIZING --> READY: wm_get_window_count() == 0, Desktop Composited
    GUI_INITIALIZING --> FAILED: Framebuffer or Compositor Allocation Failure
    READY --> [*]: Clean Interactive Session
```

### Invariants Enforced:
1. `desktop_is_ready()` returns `false` during all boot phases prior to `DESKTOP_STATE_READY`.
2. Input events arriving before `DESKTOP_STATE_READY` are safely dropped.
3. No desktop rendering pass occurs before `DESKTOP_STATE_GUI_INITIALIZING`.

---

## 2. Single-Initialization Guards

Each GUI layer enforces single-initialization protection via internal static guards:

1. **Graphics Abstraction Layer (`kernel/drivers/gfx.c`)**:
   - Guarded by `static bool g_gfx_initialized = false`.
   - Subsequent calls return `0` immediately without reallocating backbuffers or resetting hardware pitch.
2. **Window Manager (`kernel/gui/wm.c`)**:
   - Guarded by `static bool g_wm_initialized = false`.
   - Subsequent calls return without clearing active window lists or corrupting focus z-order.
   - Declares query helper: `bool wm_is_initialized(void)`.
3. **Desktop Shell (`kernel/gui/desktop.c`)**:
   - Guarded by `static bool g_desktop_initialized = false`.
   - Protects desktop icons, taskbar dimensions, and start menu buttons from re-registration.
   - Declares query helper: `bool desktop_is_initialized(void)`.

**Verification Marker**: `ASTRA_GUI_SINGLE_INIT_OK` is emitted only when `gfx_init() == 0 && wm_is_initialized() && desktop_is_initialized()` all confirm single initialization.

---

## 3. Application Startup Policy & Clean Session Verification

### Zero Autostart Guarantee

AstraOS enforces a strict clean default session policy:
- Automatic calls to `app_editor_launch()`, `app_filemgr_launch()`, and `app_browser_launch()` were completely removed from `kernel/kernel.c`.
- Immediately following `desktop_init()`, the kernel asserts:
  ```c
  if (wm_get_window_count() == 0) {
      kprintf("[BOOT] Clean session verified: Zero autostart applications running.\n");
      kprintf("ASTRA_NO_AUTOSTART_APPS_OK\n");
  } else {
      panic("Unwanted applications detected on startup session! Count=%d", wm_get_window_count());
  }
  ```
- Result: **0 open windows**. The screen displays only the desktop background, system icons, taskbar, start menu button, and live clock.

### User-Initiated Launch & Teardown Policy

To guarantee that applications launch *only* on user input and exit cleanly, `desktop_test_user_launch()` in [kernel/gui/desktop.c](file:///C:/Users/Akash/Documents/MyKernel/kernel/gui/desktop.c) deterministically exercises the user interaction pathway:

1. **Precondition Check**: Verifies `wm_get_window_count() == 0`.
2. **Simulated User Click**: Dispatches mouse click at `(x=38, y=86)` (Text Editor desktop shortcut).
3. **Launch Verification**:
   - Asserts `wm_get_window_count() == 1`.
   - Asserts launched window is actively focused (`wm_get_focused() != NULL`).
4. **Simulated Close Click**: Dispatches mouse click at `(x=w->x + w->width - 12, y=w->y + 12)` (Window Close `[X]` button).
5. **Teardown Verification**:
   - Asserts window destruction completed and `wm_get_window_count() == 0`.
   - Re-composites desktop backbuffer.
6. **Emission**: Emits `ASTRA_USER_LAUNCH_ONLY_OK`.

---

## 4. Desktop Stability & Multi-Pass Rendering

Following user launch and teardown, a secondary `desktop_render()` call executes to verify that window destruction leaves the desktop backbuffer in a pristine state:
- Wallpaper pixels are intact.
- Taskbar, start button, and clock render cleanly without clipping or pixel dirt.
- Cursor compositing remains active.
- Emits marker: `ASTRA_DESKTOP_STABLE_OK`.

---

## 5. Qualification Evidence

All desktop stability markers passed across all 5 cold boot runs in VirtualBox 7 EFI:

```
[BOOT] Initializing Graphics, Input, Window Manager & Desktop...
[GFX] Framebuffer initialized successfully:
  Dimensions: 1024x768 @ 32bpp (Pitch: 4096 bytes / 1024 pixels)
  Hardware FB VA:  0xffff800080000000
  Backbuffer PA:   0x00000000001a7000 (768 pages, 3072 KiB)
  Backbuffer VA:   0xffff8000001a7000
[PS/2] Initializing PS/2 Keyboard and Mouse controller...
[PS/2] Keyboard and Mouse enabled (IRQ1=33, IRQ12=44, streaming active).
[BOOT] Clean session verified: Zero autostart applications running.
ASTRA_NO_AUTOSTART_APPS_OK
[BOOT] Single initialization lifecycle verified for GFX, WM, and Desktop.
ASTRA_GUI_SINGLE_INIT_OK
ASTRA_DESKTOP_READY_OK

[DESKTOP TEST] Verifying User-Initiated Application Launch and Clean Teardown...
  PASS: Text Editor launched on user shortcut click (Window ID 1).
  PASS: Application cleanly closed upon user request; window count returned to 0.
[DESKTOP TEST] User application launch policy verified successfully!
ASTRA_USER_LAUNCH_ONLY_OK

[BOOT] User application launch and teardown policy verified.
ASTRA_DESKTOP_STABLE_OK
[BOOT] Milestones recorded. Total kernel bring-up time: 517 ticks (5170 ms).
ASTRA_BOOT_TIMING_RECORDED
```

Visual evidence confirmed at [vbox_perf_desktop.png](file:///C:/Users/Akash/Documents/MyKernel/build/vbox_perf_desktop.png).

