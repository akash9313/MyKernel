# AstraOS Advanced Native Terminal, Universal Command Framework & Package Manager — Architecture Audit

**Author:** AstraOS Core Architecture & Systems Team  
**Date:** 2026-11-03  
**Status:** COMPLETE (Baseline Audit)  
**Deliverable:** Phase 1 Architecture Audit (`advanced_terminal_audit.md`)

---

## 1. Executive Summary

This document presents a comprehensive, evidence-based audit of AstraOS's current kernel, process execution, memory management, filesystem, networking, cryptography, GUI, and user-space capabilities. The objective is to identify the precise technical foundation, capabilities, and existing architectural gaps before implementing the **AstraOS Advanced Native Terminal**, the **Native Interactive Shell Engine**, the **Universal Command Library**, and the **Native Package Manager (`pkg`)**.

---

## 2. Architectural Subsystem Inventory & Audit Findings

### 2.1. Kernel Architecture & Execution Privileges
* **Hardware Architecture:** x86-64 freestanding OS booted via UEFI / Limine protocol into long mode (64-bit).
* **Privilege Levels:**
  * **Kernel (Ring 0 / CPL=0):** Runs with GDT selectors `0x08` (Kernel Code 64) and `0x10` (Kernel Data 64). Full privileged instruction execution and direct physical memory access via HHDM (Higher Half Direct Map).
  * **User Space (Ring 3 / CPL=3):** Runs with GDT selectors `0x23` (User Code 64, RPL=3) and `0x1B` (User Data 64, RPL=3). Ring 3 code cannot execute privileged instructions (`cli`, `sti`, `lidt`, `mov crX`, `in`/`out`, `wrmsr`).
* **Privilege Transition:**
  * Entry to Ring 3 is performed strictly via `iretq` in `user_jump_to_ring3(entry, rsp)`. The stack frame pushes `SS (0x1B)`, `RSP`, `RFLAGS (IF=1, IOPL=0)`, `CS (0x23)`, and `RIP (entry)`. All general-purpose registers (`%rax` through `%r15`) are zeroed before `iretq` to prevent kernel data leakage.
  * System call transition from Ring 3 to Ring 0 is performed via `int $0x80` targeting IDT vector 128 (Interrupt Gate, DPL=3).
  * Stack switching on interrupt/syscall is hardware-enforced via the TSS (Task State Segment) `RSP0` pointer, set per-thread to the kernel stack top.

### 2.2. User-Space Process Execution & Application Isolation
* **Which applications genuinely run in user space:**
  * In the existing test harness and userspace suite (`user/`), genuine Ring 3 processes exist:
    * `hello_gui.elf` — User-mode GUI test creating a native window and handling mouse events.
    * `counter_gui.elf` — User-mode counter GUI rendering in an isolated window.
    * `normal_app.elf` — User-mode worker executing compute loops and `sys_exit`.
    * `cpu_worker.elf`, `blocking_worker.elf` — User-mode SMP and scheduler stress tests.
  * Each user process has its own isolated page table PML4 (`pml4_phys`). Kernel memory is mapped in the upper canonical half (HHDM and kernel text), marked `PTE_GLOBAL` but supervisor-only (`US=0`). Ring 3 access triggers page fault (`#PF` vector 14).
* **Virtual Memory Layout per Process:**
  * User Code Base: `0x0000000000400000` (`USER_CODE_BASE_VA`)
  * User MMAP Base: `0x0000000040000000` (`USER_MMAP_BASE_VA`) to `0x80000000` (1 GiB ceiling)
  * User Stack Guard Page: `0x00006ffffff00000` (`USER_STACK_GUARD_VA`, unmapped, non-present)
  * User Stack Range: `0x0000700000000000` to `0x0000700000004000` (16 KiB default, mapped `R/W/U/NX`)
  * Canonical Boundary: Addresses $\ge \text{0x0000800000000000}$ are non-canonical or kernel space. Pointers crossing this boundary are rejected by `user_ptr_valid()`.

### 2.3. System Call Interface & Audit
* **Syscall Vector:** IDT Vector 128 (0x80) with DPL=3.
* **Registered System Calls (37 total):**
  1. `SYS_WRITE` (1): Writes to console (stdout=1, stderr=2) or owned VFS file descriptor ($\ge 3$).
  2. `SYS_YIELD` (2): Cooperative reschedule (`thread_yield()`).
  3. `SYS_GETPID` (3): Retrieves caller's Process ID.
  4. `SYS_EXIT` (4): Terminates caller process, cleans up address space, unlinks threads.
  5. `SYS_OPEN` (5): Opens VFS path with `VFS_O_*` flags; assigns FD ownership to caller PID.
  6. `SYS_READ` (6): Reads up to count bytes from owned VFS FD into validated user buffer.
  7. `SYS_CLOSE` (7): Closes owned VFS FD.
  8. `SYS_STAT` (8): Retrieves file size and node type into `vfs_stat_t`.
  9. `SYS_READDIR` (9): Reads directory entry at index into `vfs_dirent_t`.
  10. `SYS_GUI_GET_SCREEN_INFO` (10): Queries display resolution and pitch.
  11. `SYS_GUI_CREATE_WINDOW` (11): Allocates a window owned by caller PID.
  12. `SYS_GUI_DESTROY_WINDOW` (12): Destroys window owned by caller PID.
  13. `SYS_GUI_GET_EVENT` (13): Pops mouse/keyboard/window events for window or system.
  14. `SYS_GUI_PRESENT` (14): Blits and composites window onto desktop backbuffer.
  15. `SYS_GUI_DRAW_RECT` (15): Draws rectangle outline into window client surface.
  16. `SYS_GUI_FILL_RECT` (16): Fills solid rectangle into window client surface.
  17. `SYS_GUI_DRAW_TEXT` (17): Renders text into window client surface with 8x16 font.
  18. `SYS_GUI_CLEAR` (18): Clears window client surface to background color.
  19. `SYS_GUI_SET_TITLE` (19): Updates window title bar string.
  20. `SYS_SLEEP` (20): Sleeps caller thread for specified milliseconds.
  21. `SYS_NET_SOCKET` (21): Allocates TCP socket owned by caller PID.
  22. `SYS_NET_CONNECT` (22): Establishes TCP connection to remote IP:port.
  23. `SYS_NET_SEND` (23): Transmits data over connected TCP socket.
  24. `SYS_NET_RECV` (24): Receives incoming payload from TCP socket with timeout.
  25. `SYS_NET_CLOSE` (25): Gracefully closes TCP connection.
  26. `SYS_NET_GET_CONFIG` (26): Queries local IP, subnet mask, gateway, and MAC address.
  27. `SYS_NET_RESOLVE` (27): Resolves DNS hostname to IPv4 address via UDP port 53.
  28. `SYS_NET_PING` (28): Sends ICMP Echo Request and returns round-trip time (RTT ms).
  29. `SYS_MMAP` (29): Allocates anonymous virtual memory pages with strict $W \oplus X$ enforcement.
  30. `SYS_MUNMAP` (30): Unmaps virtual memory pages and returns physical frames to PMM.
  31. `SYS_NET_FETCH` (31): High-level HTTP/HTTPS URL fetch via kernel TLS/HTTP engine.
  32. `SYS_GUI_DRAW_BITMAP` (32): Blits RGBA pixel array into window client surface.
  33. `SYS_FSYNC` (33): Flushes file data and metadata to durable block storage.
  34. `SYS_SYNC` (34): Synchronizes all filesystem superblocks and dirty buffers.
  35. `SYS_RENAME` (35): Atomically renames or moves a VFS path.
  36. `SYS_WAITPID` (36): Waits for child process termination with status code.
  37. `SYS_GETCPUID` (37): Returns current APIC CPU core ID.

* **Syscall Security & Validation:**
  * All pointers passed from Ring 3 are validated via `user_ptr_valid(ptr, len, check_writable)`.
  * Pointer arithmetic is checked against 64-bit integer overflow.
  * Addresses must be strictly within user canonical range (`< 0x0000800000000000`).
  * Every 4 KiB page spanning the requested buffer is verified in the active PML4 for present bit and `PTE_USER` privilege bit, plus `PTE_WRITE` if destination is writable.

### 2.4. Executable Loading (ELF64)
* **Status:** Fully functional static ELF64 executable loader (`kernel/process/elf.c`).
* **Supported ELF Properties:**
  * Format: ELF64 (`EI_CLASS == 2`), 2's complement little endian (`EI_DATA == 1`).
  * Architecture: x86-64 (`EM_X86_64 == 62`).
  * Executable Type: `ET_EXEC` (Static executable with fixed virtual load addresses).
  * Program Headers: Iterates `PT_LOAD` segments, maps pages with appropriate VMM permissions (`VMM_FLAG_READ`, `VMM_FLAG_WRITE`, `VMM_FLAG_NO_EXEC` for $W \oplus X$ protection).
* **Missing Capabilities (Identified Gaps):**
  * Dynamic linking (`PT_DYNAMIC`, `PT_INTERP`, `ld.so`, `.so` shared libraries) is **NOT** present. All user programs must be statically linked.
  * Relocatable / Position-Independent Executables (`ET_DYN` / PIE) are currently rejected by `elf.c:elf_validate_reader()`.
  * Process argument passing (`argc`, `argv`, `envp`) is currently **NOT** populated onto the initial user stack.

### 2.5. Environment Variables and Process Arguments
* **Current State:**
  * `process_t` does not store environment variables or command-line argument buffers.
  * In `process_entry_trampoline()`, `user_jump_to_ring3(proc->code_va, proc->user_stack_top)` enters the entry point with an uninitialized user stack.
* **Requirements for Universal Command Framework:**
  * Shell session environment store (`PATH`, `USER`, `HOME`, `TERM`, `PWD`, `SHELL`).
  * Shell tokenizer and parameter expander (`$VAR`).
  * Stack initialization helper that formats `argc`, `argv[]`, `NULL`, `envp[]`, `NULL`, and string buffers in standard System V AMD64 ABI format before launching external user executables.

### 2.6. Filesystem Architecture & Atomic Operations
* **Filesystem Structure:**
  * VFS layer (`kernel/fs/vfs.c`): Virtual file system providing path resolution, mount table, file descriptors, permission ownership, and operation dispatch (`open`, `read`, `write`, `stat`, `readdir`, `fsync`, `rename`).
  * Initramfs (`kernel/fs/initramfs.c`): Read-only boot archive mounted at `/` (containing `/bin/`, `/www/`, etc.).
  * AstraFS (`kernel/fs/astrafs.c`): Durable block-backed filesystem mounted at `/data` (or persistent storage).
* **Atomic Package Installation Support:**
  * AstraFS features dual-set superblocks (Superblock A at LBA 1, Superblock B at LBA 2) and dual inode tables with monotonic generation counters and Fletcher-32 checksums.
  * VFS supports atomic rename (`vfs_rename` / `SYS_RENAME`).
  * Package manager can stage files in `/pkg/staging/` or temporary filenames, verify package integrity and digital signatures, and atomically commit them to final installation targets. If an installation fails mid-way, staging files are deleted, leaving system state pristine.

### 2.7. Networking, Cryptography & Secure Package Transport
* **Networking Capabilities:**
  * Intel 82540EM (e1000) Gigabit Ethernet driver (`kernel/drivers/e1000.c`).
  * ARP, IPv4, ICMP ping, DHCP client, DNS resolver (UDP port 53), TCP client with sliding window, retransmission, and socket abstraction (`kernel/net/`).
  * HTTP client with chunked transfer and redirect handling (`kernel/net/http.c`).
  * High-level URL fetch (`browser_fetch_url()` in `kernel/browser/browser_url.c`).
* **Cryptographic Foundation:**
  * Hardware entropy / TRNG (`RDRAND` / `RDSEED`) in `kernel/crypto/crypto_rand.c`.
  * SHA-256 and HMAC-SHA256 (`kernel/crypto/sha256.c`).
  * HKDF key derivation (`kernel/crypto/hkdf.c`).
  * ChaCha20-Poly1305 AEAD cipher (`kernel/crypto/chacha20_poly1305.c`).
  * X25519 Elliptic Curve Diffie-Hellman (`kernel/crypto/x25519.c`).
  * RSA-2048 PKCS#1 v1.5 and PSS signature verification (`kernel/crypto/rsa.c`).
  * X.509 certificate validation and expiration checks (`kernel/crypto/x509.c`).
  * TLS 1.3 Client Handshake & Secure Record Layer (`kernel/net/tls.c`).
* **Audit Determination for Secure Downloads:**
  * Current networking and cryptography **fully support secure HTTPS package downloads**. Packages and repository metadata can be fetched over TLS 1.3, verified against trusted AstraOS Root Public Keys, and integrity-checked via SHA-256 hashes.

### 2.8. GUI Window Manager & Desktop Integration
* **Graphics Subsystem:** Hardware linear framebuffer (`kernel/drivers/gfx.c`) with double-buffering (`gfx_flip()`), dirty-rect clipping, and fast monospaced 8x16 bitmap typography.
* **Window Manager (`kernel/gui/wm.c`):** Multi-window support, z-ordering, focus management, title bars, close buttons, dragging, and clipping.
* **Desktop Shell (`kernel/gui/desktop.c`):**
  * Renders background grid, desktop shortcuts, taskbar, start menu, system status tray.
  * Controlled lifecycle: `BOOTING` $\rightarrow$ `KERNEL_READY` $\rightarrow$ `GUI_INITIALIZING` $\rightarrow$ `DESKTOP_READY`.
  * Desktop self-test and user-launch-only policy: applications **never autostart** on boot; they open strictly upon explicit user interaction (shortcuts or start menu).
* **Keyboard Input Subsystem:** PS/2 keyboard driver (`kernel/drivers/ps2.c` / `kernel/input.h`) delivering ASCII keys, scancodes, and modifier keys (`Shift`, `Ctrl`, `Alt`) into an event queue.

---

## 3. Gap Analysis & Architecture Roadmap

| Subsystem | Existing State | Requirement for Native Terminal & Package Framework | Action Required |
|---|---|---|---|
| **Terminal GUI App** | None (no `app_terminal.c`) | Native windowed terminal app with monospaced font, dark theme, scrolling buffer, cursor, keyboard event handling | Implement `kernel/gui/app_terminal.c` and `kernel/include/kernel/terminal.h` |
| **Desktop Launcher** | Shortcuts for Editor, Files, Browser | Visible Terminal shortcut on desktop and entry in Start Menu; zero autostart on boot | Update `kernel/gui/desktop.c` and quick launch bar |
| **Shell Engine** | None (serial monitor only) | Interactive shell with tokenization, quotes, escapes, env vars, CWD, history, PATH search, redirection, exit status | Implement `kernel/shell/shell.c` and `kernel/include/kernel/shell.h` |
| **Native Command Library** | None | Real commands for basic, filesystem, process, system info, networking, pkg, app launching | Implement command execution handlers using real kernel APIs |
| **Process Argument Passing** | Not passed to user stack | System V AMD64 ABI `argc`/`argv`/`envp` layout on user stack | Add argument setup helper for process creation |
| **Package Manager (`pkg`)** | None | Manifest parsing, SHA-256, RSA verification, staging, atomic install, removal, dependency resolution | Implement `kernel/pkg/pkg.c` and `kernel/include/kernel/pkg.h` |
| **Repository Infrastructure** | None | Signed repository index (`packages.idx`), HTTPS/local repo fetch, package metadata | Implement repository metadata parser and package fetcher |
| **Command Not Found Flow** | None | Check builtins $\rightarrow$ PATH $\rightarrow$ package repo; prompt user `Install? [y/N]` | Implement in shell execution pipeline |
| **Linux/Windows Compatibility** | None | Honest platform explanation, no fake commands | Implement compatibility message dispatch |

---

## 4. Responses to the Eight Audit Questions

1. **Which applications genuinely run in user space?**  
   User-mode binaries including `hello_gui.elf`, `counter_gui.elf`, `normal_app.elf`, `cpu_worker.elf`, `blocking_worker.elf`, `faulting_app.elf`, and `heartbeat.elf` run in CPL=3 with isolated PML4 page tables. Windowed built-in apps (`app_editor`, `app_filemgr`, `app_browser`) currently run inside the kernel window manager framework, while user ELF applications interact via GUI syscalls (10–19, 32).
2. **Which system calls are available?**  
   37 system calls are implemented and validated via `int $0x80`: process lifecycle (`SYS_EXIT`, `SYS_YIELD`, `SYS_GETPID`, `SYS_SLEEP`, `SYS_WAITPID`), VFS I/O (`SYS_WRITE`, `SYS_OPEN`, `SYS_READ`, `SYS_CLOSE`, `SYS_STAT`, `SYS_READDIR`, `SYS_FSYNC`, `SYS_SYNC`, `SYS_RENAME`), memory (`SYS_MMAP`, `SYS_MUNMAP`), networking (`SYS_NET_SOCKET` through `SYS_NET_FETCH`), and GUI windowing (`SYS_GUI_*`).
3. **Whether executable loading is fully functional?**  
   Yes, static ELF64 executable loading is fully functional for `ET_EXEC` binaries on x86-64. Program headers are parsed and mapped with strict $W \oplus X$ permissions.
4. **Whether dynamic linking exists?**  
   No. Dynamic linking (`PT_DYNAMIC`, `PT_INTERP`, `ld.so`) does not exist in AstraOS. All executables must be static ELF64.
5. **Whether environment variables exist?**  
   Not currently in process execution. The shell engine will introduce an environment variable table (`PATH`, `USER`, `HOME`, `TERM`, `PWD`, `SHELL`) with `$VAR` substitution.
6. **Whether process arguments are supported?**  
   Currently, process creation loads ELF code and points RSP to top-of-stack without placing `argc`/`argv` on the stack. Argument setup will be added to support command-line arguments for executables.
7. **Whether current networking supports secure package downloads?**  
   Yes. AstraOS has a native TLS 1.3 stack, X.509 certificate validation, RSA-2048 PKCS#1 v1.5 / PSS verification, SHA-256 hashing, and HTTP/HTTPS client support.
8. **Whether the filesystem supports atomic package installation?**  
   Yes. AstraFS provides dual-set superblocks and generation-tracked inode structures, and VFS supports atomic rename (`vfs_rename`). Staged installation with rollback on failure is supported.

---

## 5. Audit Conclusion & Next Steps

The audit confirms that AstraOS possesses all necessary fundamental building blocks (multi-window GUI, keyboard input, VFS file operations, process isolation, static ELF64 loading, TCP/TLS networking, SHA-256 and RSA cryptography). No architecture refactoring is required. We can proceed directly to Phase 2: Professional Terminal Application and the subsequent phases.

