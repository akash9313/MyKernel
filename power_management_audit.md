# AstraOS — Power Management & Session Lifecycle Audit

## Executive Summary

This audit investigates the current state of power management, system lifecycle, and session control in AstraOS. 
Prior to this phase, power actions (Shut Down, Restart, Log Out, Lock, Suspend) either executed incomplete stubs, blindly pulsed hardware ports without synchronization, or acted as cosmetic placeholders.

This document details the actual implementation of each subsystem, missing dependencies, and root causes of failure across all 11 evaluation areas.

---

## 1. Subsystem Audit Matrix

| Evaluation Area | Current Implementation | Missing Dependencies | Failure Cause / Defect |
| :--- | :--- | :--- | :--- |
| **Desktop Power Button & Menu** | Dropdown in `kernel/gui/desktop.c` showing PWR button, "Lock Screen", "Log Out", "Suspend", "Restart...", "Shut Down...". | Lifecycle state machine, disabled state rendering, authentication modal, multi-user backend. | "Lock Screen" and "Suspend" are non-functional text; "Log Out" merely destroys window structs without terminating processes; repeat clicks trigger concurrent actions. |
| **Window Manager Event Handling** | `wm_destroy_window()` in `kernel/gui/wm.c` destroys windows, but lacks application close protocol. | App close callback (`WM_EVENT_CLOSE`), dirty buffer verification, graceful process termination. | Closing windows does not flush app state or terminate background process threads. |
| **Shutdown & Reboot Commands** | Not registered in shell command table `g_builtins` in `kernel/shell/shell.c` (only `sync` exists). | System lifecycle system calls, centralized power dispatch, permission checks. | Running `shutdown` or `reboot` in Terminal returns `command not found: shutdown`. |
| **ACPI Initialization & Power Control** | Completely absent. `kernel/boot/limine_reqs.c` does not request RSDP pointer (`LIMINE_RSDP_REQUEST`). No table parser for RSDT/XSDT, FADT, or DSDT. | RSDP bootloader request, ACPI table headers, FADT parser, AML byte scanner for `_S5_` package, `PM1a_CNT` controller. | `sys_shutdown()` hardcodes blind writes to magic hypervisor ports (`0x604`, `0xB004`, `0x4004`, `0x0600`) and halts. On EFI / VirtualBox / real hardware, the machine stays frozen. |
| **UEFI Runtime Services** | Absent. `limine.h` defines `LIMINE_EFI_SYSTEM_TABLE_REQUEST`, but AstraOS never queries it. | Limine EFI system table request, runtime services table mapping, virtual address conversion. | No access to UEFI `ResetSystem()` runtime service. |
| **Interrupt & Timer Subsystems** | APIC timer and PIT in `kernel/drivers/apic.c`. Interrupts enabled via `sti()`. | Power quiesce hook, APIC timer stop/masking, LAPIC disable sequence. | Hardware interrupts continue firing during shutdown until raw `cli()` is reached, potentially preempting teardown. |
| **Scheduler & Process Lifecycle** | Preemptive scheduler with PCB states `PROCESS_RUNNING`, `PROCESS_BLOCKED`, `PROCESS_ZOMBIE`. | Process broadcast signal (`SIGTERM`/`SIGKILL`), process quiescing, worker thread termination. | Background threads continue executing during shutdown; no coordinated process tree teardown. |
| **AstraFS & VFS** | Dual-set transactional filesystem with `astrafs_sync()` and `vfs_sync()`. | Pre-shutdown atomic sync barrier, unmount synchronization hook. | While `vfs_sync()` exists, neither `sys_shutdown()` nor `sys_reboot()` calls it before resetting or halting. |
| **ATA/IDE Write Cache Flush** | `ata_flush_internal()` in `kernel/drivers/ata.c` supports standard `FLUSH CACHE (0xE7)` and `FLUSH CACHE EXT (0xEA)`. `block_flush()` exists. | Systematic pre-poweroff device flush loop across all block devices. | Cache flushes are never invoked during GUI restart or shutdown, risking write cache data loss on host poweroff. |
| **Session & User Management** | Minimal stub (`whoami` reads `$USER` or prints `"root"`). | User database (`/etc/passwd`), credentials, PAM, multi-user process ownership, session manager. | No user authentication; Lock Screen and Switch User cannot be securely implemented without fake security. |
| **Panic & Reboot Handlers** | `kernel_panic()` in `kernel/debug/panic.c` prints registers and calls `halt_forever()`. `sys_reboot()` does unverified 8042/PCI port writes. | Bounded loops for 8042 controller; ACPI 2.0+ `RESET_REG` execution; clean disk flush prior to reset. | 8042 loop in `sys_reboot()` has unbounded `while (temp & 2)` which hangs if keyboard controller is absent or stuck. |

---

## 2. Root Cause Analysis by Control Action

### 1. Shut Down Action
- **Observed Behavior**: In VirtualBox EFI, clicking Shut Down displays a confirmation box. Clicking Confirm prints `[SYSTEM] Shutting down AstraOS via ACPI...`, outputs words to ports `0x604`, `0xB004`, `0x4004`, and `0x0600`, and freezes on `halt_forever()`. VirtualBox does not power off.
- **Root Cause**:
  1. VirtualBox EFI ignores raw I/O port writes to `0x4004` unless ACPI S5 sleep registers are programmed according to the DSDT definition.
  2. The kernel does not parse ACPI tables (RSDP $\to$ XSDT $\to$ FADT $\to$ DSDT).
  3. No VFS sync or ATA cache flush is executed before `halt_forever()`.

### 2. Restart Action
- **Observed Behavior**: In some environments, CPU resets via PCI `0xCF9` or triple fault; however, in others, it hangs in the 8042 polling loop. Disk data written immediately before restart may be lost.
- **Root Cause**:
  1. The 8042 keyboard controller polling loop `do { temp = inb(0x64); ... } while (temp & 2);` is unbounded. If 8042 is unavailable or input buffer remains set, the system hangs permanently.
  2. The ACPI 2.0+ standard Reset Register (`FADT->RESET_REG`) is never checked or utilized.
  3. Dirty filesystem pages and device write caches are not flushed before triggering the reset.

### 3. Sleep / Suspend Action
- **Observed Behavior**: Clicking "Suspend" does nothing; text is greyed out.
- **Root Cause**: ACPI S3 (Suspend-to-RAM) requires waking vector configuration (FACS table), CPU state preservation, paging state restoration, APIC reinitialization, and video framebuffer mode restoration. The current kernel does not have S3 sleep infrastructure.

### 4. Lock Screen Action
- **Observed Behavior**: Clicking "Lock Screen" does nothing.
- **Root Cause**: AstraOS has no authentication engine, no password hashing, and no secure lock overlay. Implementing a visual lock screen without real authentication would violate the zero-mockup rule.

### 5. Log Out & Switch User Actions
- **Observed Behavior**: "Log Out" destroys open window handles, leaving the desktop running with no active windows. "Switch User" is absent from the menu.
- **Root Cause**: AstraOS does not support multiple isolated user sessions or a login display manager. Destroying windows without user session separation gives the false appearance of logging out.

### 6. Terminal Power Commands
- **Observed Behavior**: `shutdown`, `poweroff`, `reboot`, `halt` return command not found errors.
- **Root Cause**: The shell command dispatcher in `kernel/shell/shell.c` does not register power lifecycle commands.

---

## 3. Required Architectural Changes

1. **ACPI Driver (`kernel/drivers/acpi.c`, `kernel/include/kernel/acpi.h`)**:
   - Issue `LIMINE_RSDP_REQUEST` in `kernel/boot/limine_reqs.c`.
   - Implement EBDA / BIOS ROM fallback scan (`0x9FC00..0x9FFFF`, `0xE0000..0xFFFFF`).
   - Validate checksums of RSDP, RSDT/XSDT, FADT, and DSDT.
   - Extract `PM1a_CNT_BLK`, `PM1b_CNT_BLK`, `SMI_CMD`, `ACPI_ENABLE`, `RESET_REG`, `RESET_VALUE`.
   - Parse AML byte stream of DSDT to locate `_S5_` (`SLP_TYPa`, `SLP_TYPb`) and `_S3_` objects.
   - Provide `acpi_poweroff()` and `acpi_reboot()`.

2. **Centralized Power Lifecycle Manager (`kernel/drivers/power.c`, `kernel/include/kernel/power.h`)**:
   - Introduce Power State Machine:
     `RUNNING` $\to` `SHUTDOWN_REQUESTED` $\to` `SYNCING` $\to` `QUIESCING` $\to` `POWERING_OFF` / `RESTARTING` / `FAILED`.
   - Prevent concurrent lifecycle operations via atomic locks.
   - Coordinated shutdown sequence:
     1. Stop accepting new user/process operations.
     2. Notify open GUI applications and close windows gracefully.
     3. Call `vfs_sync()` (flushes AstraFS and dirty RAM filesystem blocks).
     4. Iterate all writable block devices and call `block_flush()` to commit drive hardware caches.
     5. Quiesce background workers and network devices.
     6. Disable interrupts (`cli()`).
     7. Trigger ACPI S5 power-off with bounded fallbacks.

3. **Coordinated Restart Sequence**:
   - Flushes VFS and ATA write caches.
   - Tries ACPI 2.0+ `RESET_REG`.
   - Bounded 8042 keyboard controller reset (100,000 iterations max).
   - Fast PCI reset via `0xCF9` (`0x02` $\to` `0x06`).
   - Architectural CPU Triple Fault (`lidt null_idt; int3`).

4. **Terminal Power Commands**:
   - Register `shutdown`, `poweroff`, `reboot`, `halt`, `suspend`, `lock`, `logout` in `kernel/shell/shell.c`.
   - Bind them to the centralized power manager.
   - Clearly report status or reason for unavailable actions (e.g., `lock: authentication subsystem not configured`).

5. **Power Menu UI Updates**:
   - Make Shut Down and Restart fully functional with modal confirmation.
   - For unsupported actions (Lock, Switch User, Log Out, Sleep), render them explicitly disabled with explanatory tooltips/subtext ("Unavailable: Single-user session", "Unavailable: ACPI S3 unconfigured").
