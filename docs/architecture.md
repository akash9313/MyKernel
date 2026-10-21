# AstraOS Kernel Architecture Notes

## Overview
AstraOS is an original, freestanding 64-bit operating system kernel written in C17 and x86-64 assembly. The design prioritizes correctness, debuggability, memory protection, and reproducible verification over premature complexity.

## Milestone 2 Slice: Physical Memory Management & Verified Diagnostics

### 1. Boot Protocol & Toolchain
- **Bootloader**: Limine (v8.x binary, protocol revision 3). Pinned commit assets tracked in `third_party/limine/`.
- **Authoritative Protocol Header**: Single consolidated copy at `third_party/limine/limine.h` (SHA-256: `f8b35287579dbe50e9d84cca7b558e39b2b64e499f9b37ef5ebe969c05104c33`).
- **Firmware**: UEFI x86-64 (tested on OVMF / EDK2).
- **Binary Format**: ELF64 executable statically linked at virtual base `0xffffffff80200000`.
- **Protocol Markers**:
  - `LIMINE_REQUESTS_START_MARKER`
  - `LIMINE_BASE_REVISION(3)`
  - `limine_framebuffer_request`
  - `limine_memmap_request`
  - `limine_hhdm_request`
  - `limine_kernel_file_request`
  - `LIMINE_REQUESTS_END_MARKER`

### 2. CPU Initialization & Segment Protection
- **GDT (Global Descriptor Table)**:
  - Selector `0x00`: Null descriptor.
  - Selector `0x08`: 64-bit Kernel Code segment (DPL=0, L=1, Readable, Present).
  - Selector `0x10`: 64-bit Kernel Data segment (DPL=0, Writable, Present).
  - Selector `0x18`: 64-bit User Data segment descriptor (DPL=3, Writable, Present).
  - Selector `0x20`: 64-bit User Code segment descriptor (DPL=3, L=1, Readable, Present).
  - *Architectural Status*: The CPU currently runs entirely in Ring 0. User segment descriptors are staged in the GDT for future user-space transitions, but Ring 3 execution, process isolation, TSS stack switching (`RSP0`), and syscall ABIs are deliberately uninitialized at this stage.
- **IDT (Interrupt Descriptor Table)**:
  - 256 gates total.
  - Vectors 0..31 populated with CPU exception handler stubs.
  - Strict System V AMD64 ABI compliance:
    - 16-byte stack alignment guaranteed prior to C function calls.
    - Direction Flag (`DF`) explicitly cleared via `cld` in `isr_common_stub`.
    - Hardware error codes accurately preserved (vectors 8, 10..14, 17, 21, 29, 30 push error code; others push dummy 0).
    - Full register frame (`RAX`..`R15`, `RIP`, `CS`, `RFLAGS`, `RSP`, `SS`, and `CR2` for Page Faults) logged on exception.
  - Verified via isolated deliberate `#UD` test mode (`test=exception`).

### 3. Diagnostics & Memory Accounting
- **COM1 UART Driver**: Base port `0x3F8`, 115200 baud, 8N1, FIFO enabled, loopback verified.
- **Audited Memory Map Accounting**:
  - Strict separation between actual physical RAM (Usable, Bootloader, Kernel, ACPI) and high MMIO/PCIe device apertures (e.g. `0xfd00000000`–`0xffffffffff`).
  - High MMIO space is never mislabeled as installed RAM.
  - Arithmetic overflow and zero-length bounds checks enforced.
- **kprintf**: Freestanding zero-heap formatter (`%s`, `%c`, `%d`, `%u`, `%x`, `%X`, `%p`, `%%`) teeing to serial and framebuffer.
- **Headless Fallback**: Safe, verified execution in serial-only mode when no graphical framebuffer exists (verified with `-vga none` and `nofb`).

### 4. Physical Page Manager (PMM)
- **Allocation Granularity**: 4096-byte physical pages (`PAGE_SIZE = 4096`).
- **Allocator Design**: Region-based bitmap allocator.
  - Instead of a monolithic bitmap covering sparse high MMIO apertures, each usable physical memory descriptor from the Limine map becomes an independent `struct pmm_region`.
  - Discards non-usable memory (reserved, bootloader-reclaimable, ACPI, framebuffer).
  - Upward alignment for region starts, downward alignment for region ends.
  - Physical Page Zero (`0x00000000`) is unconditionally reserved.
  - Metadata (region descriptors + bitmaps) is carved out of the first suitable usable memory region, mapped via the Higher-Half Direct Map (HHDM), and marked allocated before serving requests.
  - Metadata overhead is ~0.003% of managed RAM (~20 KiB for 512 MiB RAM).
- **Invariants & Error Trapping**:
  - Traps unaligned physical addresses (`PMM_ERR_UNALIGNED`).
  - Traps attempts to free page zero (`PMM_ERR_PAGE_ZERO`).
  - Traps addresses outside managed usable regions (`PMM_ERR_UNMANAGED`).
  - Traps double frees (`PMM_ERR_DOUBLE_FREE`).
  - Traps memory exhaustion (`PMM_ERR_NOMEM`).
- **Synchronization**: Single-core design; SMP locking will be introduced in Milestone 3.

### 5. Kernel Serial Monitor
- Interactive development shell over COM1 UART.
- Commands:
  - `help`: lists all available commands.
  - `version`: reports kernel version, architecture, and active subsystems.
  - `clear`: clears terminal and framebuffer.
  - `memmap`: displays physical memory map table with RAM vs MMIO breakdown.
  - `mem`: displays detailed PMM statistics and region-by-region allocation state.
  - `fault`: executes deliberate `ud2` instruction for interactive exception verification.
  - `halt`: cleanly halts CPU.

### 6. Virtual Memory Manager (VMM) & Kernel-Owned Address Space
- **Paging Architecture**: Standard 4-level paging (`PML4` -> `PDPT` -> `PD` -> `PT`).
  - 5-level paging (`CR4.LA57`) is checked and actively rejected.
  - Linear address canonicalness (48-bit sign extension) is strictly validated.
  - Leaf translations use 4 KiB granular mappings; huge pages (2 MiB, 1 GiB) are supported by the active inspector (`vmm_walk_active`) and collision-checked during mapping.
- **W^X Enforcement**:
  - Kernel `.text`: Mapped Read-Only, Executable (`VMM_FLAG_READ`).
  - Kernel `.rodata`: Mapped Read-Only, Non-Executable (`VMM_FLAG_READ | VMM_FLAG_NO_EXEC`).
  - Kernel `.data` & `.bss`: Mapped Read-Write, Non-Executable (`VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC`).
  - Dedicated Kernel Stack: 16 KiB (4 pages), mapped Read-Write, Non-Executable.
  - Guard Page: `0xffffffff80400000` is deliberately unmapped (`PTE_PRESENT` is 0).
  - HHDM Consistency: Physical pages hosting kernel `.text` and `.rodata` are mapped into HHDM as Read-Only, eliminating writable code aliases.
- **Hardware Protections**:
  - `EFER.NXE`: Enabled via MSR `0xC0000080` (bit 11).
  - `CR0.WP`: Enabled (bit 16), enforcing supervisor write protection.
  - `CR4.PGE`: Toggled during assembly activation to invalidate all global TLB translations.
- **Activation Switch (`vmm_activate_space_and_switch_stack`)**:
  - Atomic assembly transition: disables interrupts, clears and restores `CR4.PGE`, updates `CR3`, switches `RSP` to dedicated stack with System V ABI 16-byte alignment (`%rsp % 16 == 8` simulated frame), and jumps directly to `kernel_continuation`.
  - Old page tables and bootloader stack are preserved and kept mapped; never freed.

### 7. Transactional Kernel Heap (`kmalloc` / `kfree`)
- **Arena Design**: 64 KiB fixed arena at `0xffffffff80601000` .. `0xffffffff80611000`.
- **Boundary Guards**: Unmapped guard pages at lower (`0xffffffff80600000`) and upper (`0xffffffff80611000`) boundaries prevent overruns.
- **Block Layout**: Explicit headers with magic cookies (`0xA5712A01`), block size, and allocation state; minimum 16-byte alignment.
- **Bidirectional Coalescing**: Boundary tags enable O(1) merging with both left and right neighbors upon free.
- **Transactional Rollback**: Initialization is transactional; failures mid-initialization cleanly roll back all allocated physical frames and virtual mappings with zero leakage.

### 8. Task State Segment (TSS) & Interrupt Stacking (IST1)
- **TSS Configuration**: 104-byte 64-bit TSS loaded via `ltr` (selector `0x28`).
- **IST1 Double Fault Stack**: Dedicated 8 KiB stack at `0xffffffff80411000` .. `0xffffffff80413000` with unmapped guard page at `0xffffffff80410000`.
- **Double Fault Isolation**: Vector 8 configured as an IST gate pointing to IST1, ensuring safe diagnostic reporting and halt even under total kernel stack exhaustion.

### 9. Local APIC & Periodic Timer Delivery
- **APIC Configuration**: Base MSR `0x1B`, Spurious Vector 255 (SVR bit 8 set).
- **Periodic Timer**: Vector 32, divide configuration by 16, initial counter for ~100 Hz ticks.
- **Interrupt Stacking**: Safe EOI acknowledgment (`apic_send_eoi`) on every tick.

### 10. Multitasking, Blocking, Wait Queues & Timer Preemption
- **Kernel Threading**: Up to 32 kernel threads with dedicated 16 KiB stacks bounded by 4 KiB unmapped guard pages.
- **Context Switch (`thread_switch_asm`)**:
  - Callee-saved register preservation (`RBX`, `RBP`, `R12`..`R15`).
  - Saved RFLAGS preserving interrupted IF state.
  - Strict System V AMD64 ABI stack alignment at entry trampoline (`RSP % 16 == 8` entering trampoline, `(RSP + 8) % 16 == 0` entering C function).
  - Deferred thread reclamation preventing active stack freeing.
- **Scheduler State Machine**:
  - `THREAD_READY`: Runnable thread in scheduler FIFO ready queue.
  - `THREAD_RUNNING`: Currently running on CPU (`g_current_thread`).
  - `THREAD_BLOCKED`: Suspended in wait queue or timer queue (`sched_block_current`).
  - `THREAD_EXITED`: Finished execution; stack and descriptor awaiting `sched_reap()`.
- **Blocking & Synchronization**:
  - FIFO wait queues (`wait_queue_t`) with O(1) enqueue/dequeue and arbitrary element removal.
  - Persistent auto-reset events (`event_t`) preventing lost wakeups via latching signals and FIFO delivery.
  - Tick-based sleep (`thread_sleep_ticks`) using sorted deadline delta queue and modular arithmetic (`deadline.h`) for wraparound safety.
  - Race-safe idle loop using `sti; hlt` to wait for timer or event wakeups.
- **Preemption Safety & Timer-Driven Preemption**:
  - Per-thread `preempt_count` counter for nested critical section tracking.
  - Reschedule flag `need_resched` set on timer tick or thread unblocking.
  - Safe IRQ exit hook (`sched_try_preempt_irq_exit`) in `irq_common_stub`: preemption occurs only when returning from hardware IRQ to an unnested thread (`preempt_count == 0`, `interrupted_rflags.IF == 1`, `interrupt_depth == 0`).

### 11. Kernel Synchronization Primitives (Phase D: Mutexes & Counting Semaphores)
- **Design Philosophy**: Safe, single-CPU kernel synchronization primitives built upon existing scheduler FIFO wait queues (`wait_queue_t`), `THREAD_BLOCKED` state, preemption guards, and interrupt discipline.
- **Kernel Mutex (`mutex_t`)**:
  - **Core Operations**: `mutex_init`, `mutex_lock`, `mutex_try_lock`, `mutex_unlock`, `mutex_is_locked`, `mutex_get_owner`.
  - **Ownership Tracking**: `m->owner` references the current owning `thread_t *`. `curr->mutex_count` tracks number of held mutexes per thread.
  - **Strictly Non-Recursive**: Calling `mutex_lock` on an already-owned mutex triggers an immediate diagnosed kernel panic (`mutex_lock: recursive acquisition attempted...`).
  - **Non-Owner Unlock Detection**: Calling `mutex_unlock` from a thread that does not own the mutex triggers an immediate diagnosed panic (`mutex_unlock: TID %u attempted to unlock mutex owned by TID %u...`).
  - **Direct Ownership Handoff & Anti-Barging**: On unlock with pending waiters, ownership is transferred directly to the FIFO head waiter (`m->owner = next_owner; next_owner->mutex_count++`) before transitioning the waiter to `THREAD_READY`. Newly arriving contenders see `m->owner != NULL` and cannot steal the mutex ahead of queued waiters.
  - **Zero Dynamic Allocation**: Mutex structs are caller-allocated; acquisition, try-lock, release, and waiter wakeups perform zero dynamic memory allocation (`kmalloc`/`kfree`).
  - **Calling Context Discipline**: `mutex_lock` can block; it strictly rejects invocation from interrupt context (`in_interrupt() == true`), preemption-disabled critical sections (`preempt_count > 0`), or with interrupts disabled (`IF == 0`).
  - **Thread Exit Protection**: Threads attempting to exit while owning mutexes (`curr->mutex_count > 0`) are diagnosed and trapped immediately in `thread_exit`.
- **Counting Semaphore (`semaphore_t`)**:
  - **Core Operations**: `semaphore_init`, `semaphore_wait`, `semaphore_try_wait`, `semaphore_signal`, `semaphore_get_count`.
  - **Permit Accounting**: Initialized with `initial_count` and bounded by `maximum_count` (`maximum_count > 0`, `initial_count <= maximum_count`).
  - **Direct Permit Handoff & Anti-Barging**: When signaling with waiters, the permit is granted directly to the FIFO head waiter without incrementing `s->count`. Arriving threads calling `semaphore_try_wait` see `count == 0` and cannot steal the permit.
  - **Overflow Protection**: Signaling beyond `maximum_count` when no waiters exist returns `SEMAPHORE_ERR_OVERFLOW` with state preserved.
  - **Hardware IRQ Signaling**: `semaphore_signal` is safe from maskable hardware interrupt handlers. It is bounded, allocation-free, and avoids scheduling directly; awakened threads are enqueued as `THREAD_READY`, and preemption is deferred to the IRQ exit boundary via `sched_try_preempt_irq_exit`. Signals from CPU exception or NMI contexts are strictly prohibited.
- **Subsystem Protection & Lock Ordering**:
  - Allocators (PMM bitmap, transactional Heap arena) and scheduler runqueues use low-level preemption counters (`preempt_disable`/`preempt_enable`) and brief interrupt masking (`cli`/`sti`) rather than blocking mutexes, eliminating lock inversion and deadlock risks.
  - Emergency panic output (`panic.c`) remains completely lock-free using direct serial/framebuffer formatting under `cli`.

### 12. User Mode, System Calls & Process Foundation (Phase E)
- **User-Mode Execution at CPL3**:
  - User code executes in 64-bit Long Mode at CPL 3 with segment selectors `CS = 0x23` (`GDT_USER_CODE_SEG | 3`) and `SS/DS/ES = 0x1B` (`GDT_USER_DATA_SEG | 3`).
  - Transition to Ring 3 is performed via `iretq` by constructing an interrupt frame on the thread's kernel stack (`SS`, `user_rsp`, `RFLAGS=0x202`, `CS=0x23`, `user_rip`).
  - General purpose registers are cleared prior to `iretq` to prevent kernel data leakage.
- **Kernel/User Privilege Transition & TSS `RSP0`**:
  - The Task State Segment (`struct tss`) holds `rsp0`, which points to the top of the currently active thread's kernel stack.
  - When a transition from Ring 3 to Ring 0 occurs via interrupt, exception, or software interrupt `int $0x80`, the CPU hardware automatically switches `RSP` to `TSS.RSP0` and pushes the user `SS`, `RSP`, `RFLAGS`, `CS`, and `RIP`.
  - On every thread context switch (`thread_switch_asm`), `TSS.RSP0` is updated to the incoming thread's `stack_top`.
- **System Call Entry Mechanism (`int $0x80`)**:
  - IDT entry 128 (`0x80`) is registered with DPL=3 (`0xEE`), permitting unprivileged Ring 3 invocation.
  - Syscall ABI:
    - `RAX`: Syscall number (`SYS_WRITE=1`, `SYS_YIELD=2`, `SYS_GETPID=3`, `SYS_EXIT=4`).
    - `RDI`: First argument.
    - `RSI`: Second argument.
    - `RDX`: Third argument.
    - `RAX`: Returns 0/positive on success, negative error code (`-EFAULT`, `-EBADF`, `-ENOSYS`) on failure.
    - Callee-saved and caller-saved registers (except `RAX`) are preserved across the call.
  - Handled via dedicated `syscall_stub_128` and dispatched in `syscall_dispatcher`.
- **Per-Process Address-Space Ownership & Virtual Memory Layout**:
  - Virtual address space is partitioned at the canonical boundary:
    - Lower canonical half (`0x0000000000000000` .. `0x00007fffffffffff`, PML4 entries 0..255): Private user-space memory (`PTE_USER`).
    - Upper canonical half (`0xffff800000000000` .. `0xffffffffffffffff`, PML4 entries 256..511): Shared kernel space (`PTE_USER == 0`, supervisor only).
  - Process creation (`vmm_create_process_space`):
    - Allocates a new PML4 page table root.
    - Clones kernel PML4 entries 256..511 (HHDM, kernel image, stacks, heap, APIC).
    - Zeroes user PML4 entries 0..255.
  - Process destruction (`process_destroy_space`):
    - Unmaps and frees user physical pages and lower-half page tables (PML4 entries 0..255).
    - Preserves shared upper-half kernel page tables.
    - Frees the process PML4 root page without physical memory or table leaks.
- **User Stack Allocation & Guard Pages**:
  - User stack is mapped in high user virtual memory (e.g. `0x0000700000000000` .. `0x0000700000004000`, 16 KiB, `VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXEC`).
  - Below the user stack bottom lies an unmapped guard page (`0x00006ffffff00000`). Stack overflow generates an immediate unmapped page fault.
- **Process Abstraction (`process_t`)**:
  - Distinct `pid_t` identifier, separate from scheduler `tid_t`.
  - Process lifecycle: `PROCESS_READY`, `PROCESS_RUNNING`, `PROCESS_BLOCKED`, `PROCESS_ZOMBIE`, `PROCESS_DEAD`.
  - Maintains ownership of process PML4 page table, allocated user physical pages, user stack, and associated main thread (`thread_t *`).
  - Integrated with scheduler: thread holds back-pointer `thread->process`.
- **Safe User Pointer Validation**:
  - Validates that user pointers are non-null, within the lower canonical half (`ptr + len <= 0x00007fffffffffff`), non-overflowing, and mapped with `PTE_USER` (and `PTE_WRITABLE` if destination buffer).
  - Kernel space pointers (`>= 0x0000800000000000`) passed as user buffers are strictly rejected with `-EFAULT`.
- **User Fault Isolation**:
  - CPU exceptions occurring while `(frame->cs & 3) == 3` (Ring 3) do not panic the kernel.
  - The faulting user process is safely terminated, its resources reaped, and the scheduler dispatches the next runnable task.

## 13. ELF Loader & Native User Program Execution (Phase F)

- **Supported ELF Subset**:
  - **Format**: Standard Executable and Linkable Format (ELF64).
  - **Class & Endianness**: 64-bit (`ELFCLASS64`), Little-Endian (`ELFDATA2LSB`), Version 1 (`EV_CURRENT`).
  - **Type & Architecture**: Static executables (`ET_EXEC`) targeting x86-64 (`EM_X86_64 = 62`).
  - **Dynamic Linking Exclusion**: Executables requesting dynamic linking (`PT_INTERP` or `PT_DYNAMIC`) are strictly rejected with an explicit error code (`ELF_ERR_DYNAMIC_NOT_SUPPORTED`).
  - **Supported Program Headers**: `PT_LOAD`, `PT_NULL`, `PT_NOTE`, `PT_GNU_STACK`.
- **Validation and Safety Contract (`elf_validate`)**:
  - **Magic & Identifiers**: Validates `0x7f 'E' 'L' 'F'`, 64-bit architecture, little-endian byte ordering.
  - **Header Structure**: Asserts `e_ehsize >= sizeof(Elf64_Ehdr)` and `e_phentsize >= sizeof(Elf64_Phdr)`.
  - **Table Bounds**: Checks for arithmetic overflow on `e_phoff + (e_phnum * e_phentsize)` and validates against the total image buffer size.
  - **Segment Limits**: Enforces `p_filesz <= p_memsz`. Checks that `p_offset + p_filesz` does not exceed image bounds.
  - **Address Canonicality**: Enforces all `PT_LOAD` virtual addresses reside within user canonical space (`0x0000000000200000` .. `0x0000700000000000ULL`), preserving low-memory null guards and upper-memory stack and kernel regions.
  - **W^X Enforcement**: Rejects any loadable segment flagged with both Write and Execute (`PF_W | PF_X`).
  - **Entry Point Verification**: Confirms that `e_entry` points within a valid, executable (`PF_X`) `PT_LOAD` segment.
  - **Alignment**: Requires page-offset alignment consistency (`p_vaddr % PAGE_SIZE == p_offset % PAGE_SIZE`).
- **Segment Loading & .BSS Zero Initialization**:
  - For each `PT_LOAD` segment:
    - Calculates the enclosing 4 KiB virtual page span `[seg_start, seg_end)`.
    - Allocates physical frames via PMM.
    - Zeroes the entire physical page frame.
    - Copies `p_filesz` bytes from the ELF file image into the corresponding page offsets.
    - Leaves remaining `p_memsz - p_filesz` bytes zero-filled, guaranteeing compliant `.bss` initialization.
    - Maps pages with explicit user permissions (`VMM_FLAG_USER`, plus `READ`, `WRITE`, and `NO_EXEC` derived from `p_flags`).
- **Transactional Process Creation (`process_create_elf`)**:
  - Sequentially executes:
    1. Validation of ELF image.
    2. Creation of fresh, isolated process page tables (`vmm_create_process_space`).
    3. Segment allocation, clearing, copying, and mapping.
    4. User stack allocation (16 KiB) with unmapped lower guard page.
    5. Setting entry point RIP to `e_entry`.
    6. Kernel execution thread creation (`thread_create`) and binding.
  - On any failure (allocation failure, mapping conflict, or invalid header), all allocated physical frames, stack pages, and page table hierarchies are cleanly rolled back and freed.
- **Storage Independence**:
  - The ELF loader operates exclusively on memory buffers (`const void *image, size_t size`).
  - Decouples parsing from the storage backend, enabling transparent loading from embedded memory, initrd/initramfs, or block-backed filesystems.

## 14. Initramfs & Virtual File System (VFS) (Phase G)
- **Initramfs Archive**: Custom multi-file archive with 64-byte headers, magic validation, path canonicalization, and bounds checking.
- **VFS Interface**: Standard operations (`vfs_open`, `vfs_read`, `vfs_pread`, `vfs_close`, `vfs_stat`, `vfs_readdir`).
- **Path Traversal Protection**: Rejects non-canonical relative escapes, directory traversals outside root, and malformed paths.
- **Native User ELF Loading**: Seamlessly executes `/bin/user_app.elf` loaded directly from VFS.

## 15. Graphics Subsystem & Double Buffering (Phase I)
- **Framebuffer Discovery**: Dynamic discovery via Limine `limine_framebuffer_request` response. Resolves physical address, width, height, pitch, and 32-bit pixel format.
- **Double Buffering**: Backbuffer allocated via contiguous PMM physical pages (`pmm_alloc_contiguous`) and mapped via Higher-Half Direct Map (HHDM), avoiding kernel heap exhaustion.
- **Drawing Primitives & Clipping**: Safe clipped pixel writing (`gfx_put_pixel`), line drawing, solid rectangle fills (`gfx_fill_rect`), rectangle borders (`gfx_draw_rect`), block transfers (`gfx_blit`), and 8x16 bitmap font glyph rendering (`gfx_draw_char`, `gfx_draw_string`).
- **Presentation Flip**: `gfx_present()` synchronizes the backbuffer to hardware display memory.
- **Headless & Serial Fallback**: Automatically preserves serial console output and boots cleanly even if graphics is absent.

## 16. Input Subsystem & PS/2 Drivers (Phase I)
- **PS/2 Controller Initialization**: 8259 PIC remapped to vectors 32..47; IRQ1 (vector 33, keyboard) and IRQ12 (vector 44, mouse) unmasked. Local APIC LINT0 configured as ExtINT.
- **Keyboard Driver**: Scancode Set 1 decoding, shift/ctrl/caps modifier tracking, ASCII and special key (`KEY_ENTER`, `KEY_BACKSPACE`, `KEY_UP`, `KEY_DOWN`, etc.) event generation.
- **Mouse Driver**: 3-byte streaming mouse packet processing, relative movement decoding, button state tracking (left, right, middle), and screen-boundary coordinate clamping.
- **Interrupt-Safe Event FIFO**: 256-entry lock-free ring buffer protected by `irq_save()` / `irq_restore()`. Non-blocking push and pop.

## 17. Window Manager & Desktop Compositor (Phase I)
- **Window Management (`wm.h`, `wm.c`)**:
  - Multiple concurrent windows (up to `WM_MAX_WINDOWS = 16`).
  - Per-window dedicated client backing buffer allocated via PMM.
  - Window z-ordering, focus elevation (`wm_bring_to_front`), hit-testing (`wm_window_at`).
  - Interactive titlebar dragging with screen clamping.
  - Close button (`[X]`) and minimize button (`[-]`) controls.
  - Event dispatch (`on_paint`, `on_event`).
- **Desktop Shell (`desktop.c`)**:
  - Dark futuristic aesthetic: deep space navy background (`0x080D1A`), subtle grid lines (`0x0E172E`), electric cyan-blue (`0x38BDF8`) and indigo-purple (`0x818CF8`) accents.
  - Desktop branding and application shortcut icons (`ED` Text Editor, `FM` Files).
  - Bottom taskbar dock with "AstraOS" launcher menu, active window taskbar tabs, and live status bar (RAM usage and uptime clock).
  - Software mouse arrow cursor overlay.

## 18. GUI Applications (Phase I)
- **Text Editor (`app_editor.c`)**:
  - Text buffer, typing cursor, editable text area.
  - Real-time keyboard typing, Enter (new line), Backspace (character deletion).
  - VFS file loading (opens and displays `/hello.txt`).
  - Save button with clear feedback regarding read-only initramfs root.
- **File Manager (`app_filemgr.c`)**:
  - VFS directory enumeration (`/`, `/nested`, `/bin`).
  - Directory navigation (`..` parent traversal).
  - File selection, file size reporting, and "Open in Editor" integration.

## 19. GUI System Calls & Memory Protection
- Syscall extensions for user-mode GUI integration:
  - `SYS_GUI_GET_SCREEN_INFO`: Returns display resolution and color format.
  - `SYS_GUI_CREATE_WINDOW`: Validates window bounds, allocates window handle.
  - `SYS_GUI_DESTROY_WINDOW`: Destroys window and releases client surface.
  - `SYS_GUI_GET_EVENT`: Delivers queued input events to user applications.
  - `SYS_GUI_PRESENT`: Blits user client surface to screen with strict user pointer validation.

## 20. Networking Subsystem, E1000 Driver & TCP/IP Stack (Phase J)
- **PCI Subsystem (`pci.h`, `pci.c`)**:
  - Configuration space I/O access via ports `0xCF8` (address) and `0xCFC` (data).
  - Recursive bus/device/function enumeration identifying vendors, devices, classes, BARs, and IRQs.
  - Bus mastering and memory space enable (`pci_enable_bus_master`).
- **Intel E1000 NIC Driver (`e1000.h`, `e1000.c`)**:
  - Targets Intel 82540EM Gigabit Ethernet controller (`8086:100e`).
  - MMIO BAR0 mapped directly into the kernel PML4 page tables with Cache-Disable (`VMM_FLAG_PCD`) and Write-Through (`VMM_FLAG_PWT`) flags.
  - MAC address discovery from EEPROM/RAL/RAH registers.
  - 32-descriptor circular TX and RX descriptor rings (16-byte aligned, contiguous physical DMA buffers).
  - Software tail pointer tracking, ownership bit verification, and packet transmission/reception.
- **Ethernet & ARP (`ethernet.c`, `arp.c`)**:
  - Ethernet II frame assembly and header validation. Minimum frame padding (60 bytes).
  - ARP request generation, broadcast transmission, reply parsing, and in-memory ARP cache table with timeout expiry.
- **IPv4 & ICMP (`ipv4.c`, `icmp.c`)**:
  - IPv4 packet assembly, 16-bit Internet checksum calculation, and header parsing.
  - Gateway routing: local subnet traffic routed directly, external traffic routed via default gateway MAC.
  - ICMP Echo Request and Reply handling (`icmp_ping`).
- **UDP, DHCP & DNS (`udp.c`, `dhcp.c`, `dns.c`)**:
  - UDP port binding and checksumming with IPv4 pseudo-header.
  - DHCP client: transmits DHCPDISCOVER, parses DHCPOFFER, requests lease, and parses DHCPACK to bind IP (`10.0.2.15`), netmask, default gateway (`10.0.2.2`), and DNS server.
  - DNS client: formats QNAME labels, issues UDP queries to port 53, decodes compression pointers (`0xC0`), and extracts A-record IPv4 addresses.
- **TCP Protocol Engine (`tcp.c`)**:
  - Up to 16 concurrent TCP sockets.
  - Connection state machine: `CLOSED`, `SYN_SENT`, `ESTABLISHED`, `FIN_WAIT`, `CLOSE_WAIT`.
  - 3-way handshake (`SYN` -> `SYN-ACK` -> `ACK`), sequence number tracking, and teardown (`FIN` -> `ACK`).
  - 16 KiB per-socket circular receive buffer.
- **HTTP Client (`http.c`)**:
  - Native HTTP 1.0 GET client over TCP.
  - Hostname resolution, request formatting, stream reception, and status code parsing.
- **Networking Syscalls & User Interface (`syscall.c`, `user_entry.S`, `user/net_client.c`)**:
  - `SYS_NET_SOCKET`, `SYS_NET_CONNECT`, `SYS_NET_SEND`, `SYS_NET_RECV`, `SYS_NET_CLOSE`, `SYS_NET_GET_CONFIG`, `SYS_NET_RESOLVE`, `SYS_NET_PING`.
  - Strict user-pointer validation and memory protection.
  - Interactive monitor commands: `ifconfig`, `ping`, `nslookup`, `http`, `dhcp`.

## 21. Native Graphical Web Browser — AstraBrowser (Phase K)
- **Modular Subsystem Architecture (`browser.h`, `browser_url.c`, `html_parser.c`, `browser_render.c`, `app_browser.c`)**:
  - **URL Parser & Navigation (`browser_url.c`)**:
    - Supports `http://` (web networking) and `file://` (local initramfs/VFS) schemes.
    - Explicitly and safely rejects `https://` URLs with an informative user notice explaining that TLS/cryptographic support is scheduled for a future milestone (strictly avoids faking TLS or disabling security).
    - Extracts scheme, hostname, explicit or default ports (80), and path components into a bounded `browser_url_t` structure.
  - **HTTP Client Integration (`browser_url.c`)**:
    - Direct integration with AstraOS TCP/IP stack (`tcp_connect`, `tcp_send`, `tcp_recv`, `tcp_close`).
    - Emits compliant HTTP/1.1 `GET` requests with `Host` and `Connection: close` headers.
    - Robustly parses HTTP response status lines and headers, locates the `\r\n\r\n` header-body boundary, and extracts the payload.
    - Implements bounded buffer safety (caps reads at buffer capacity to prevent overflows).
  - **Bounded Minimal HTML Parser (`html_parser.c`)**:
    - Stack-safe, non-recursive tokenizer parsing up to `BROWSER_MAX_NODES` (128 nodes) without risking kernel stack overflow.
    - Decodes standard HTML entities (`&amp;`, `&lt;`, `&gt;`, `&quot;`, `&nbsp;`).
    - Recognizes structure tags (`<html>`, `<head>`, `<body>`, `<title>`), headings (`<h1>` through `<h3>`), text blocks (`<p>`, `<br>`, `<hr>`), text formatting (`<b>`, `<strong>`, `<i>`, `<em>`), clickable hyperlinks (`<a href="...">`), and unordered/ordered lists (`<ul>`, `<ol>`, `<li>`).
    - Resiliently recovers from malformed and unclosed HTML without infinite loops.
    - Ignores unsupported elements safely; strictly executes zero JavaScript or scripts.
  - **Document Layout Engine (`browser_render.c`)**:
    - Converts parsed HTML DOM nodes into positioned rendering boxes (`render_box_t`).
    - Computes word-wrapping with whitespace collapsing and paragraph flow.
    - Applies typographical hierarchy: distinct font scales, vertical margins, heading spacing, and list indentation.
    - Creates clickable hitboxes for anchor elements (`<a>`), recording target `href` URLs.
    - Computes total document height for scroll metrics.
  - **Viewport Rendering & Scrollbar (`app_browser.c`)**:
    - Renders layout boxes strictly clipped to the browser viewport area.
    - Implements vertical scrolling with a functional scrollbar track and thumb.
    - Supports mouse wheel, scrollbar track clicking, and keyboard Up/Down arrow scrolling.
  - **Browser UI & Navigation Chrome (`app_browser.c`)**:
    - Window title displays the parsed `<title>` tag of the active page.
    - Toolbar containing interactive buttons: Back (`<-`), Forward (`->`), Reload (`R`), and Go.
    - Editable address/URL bar with real-time text editing, typing cursor, and Enter key navigation.
    - Navigation history stack (`browser_history_entry_t`, up to 16 entries) for Back/Forward traversal.
    - Status bar indicating network/fetch states: `Loading...`, `Ready (HTTP 200 OK)`, `404 Not Found`, or error descriptions.
  - **Local Test Pages (`/www/*`)**:
    - Packaged into initramfs: `/www/index.html` (portal), `/www/about.html` (system info & links), `/www/scroll.html` (long multi-paragraph page for scroll verification), and `/www/malformed.html` (parser resilience test).
  - **Architectural & Security Boundary Note**:
    - Currently, GUI applications (Text Editor, File Manager, AstraBrowser) execute within the kernel address space with direct VFS and network access. They do not yet execute in isolated user-space Ring 3 sandboxes. This limitation is explicitly acknowledged; AstraBrowser is native kernel-integrated graphical software, not an isolated sandbox.

## 22. Secure Networking, TLS 1.3 & HTTPS Subsystem (Phase L)
- **Cryptographic Primitives & Foundations (`kernel/crypto/`)**:
  - **Hardware Entropy (`crypto_rand.c`)**:
    - Hardware `rdrand` instruction detection via CPUID (ECX bit 30).
    - True hardware entropy generation with fail-closed retry loop (up to 10 retries per 64-bit word).
    - Unconditional refusal to proceed with insecure fallback if hardware entropy is unavailable (`crypto_has_secure_entropy`).
  - **FIPS 180-4 SHA-256 (`sha256.c`)**:
    - Complete 32-bit big-endian SHA-256 implementation with standard round constants and schedule.
    - Verified against NIST CAVP known-answer vectors ("abc").
  - **RFC 2104 / RFC 4231 HMAC-SHA256 & RFC 5869 HKDF (`hkdf.c`)**:
    - Key derivation via `hkdf_extract` and `hkdf_expand`.
    - RFC 8446 Section 7.1 compliant `hkdf_expand_label(secret, label, context, context_len, length, out)` formatting `tls13 <label>` byte strings.
    - Verified against RFC 4231 Test Case 1 and RFC 5869 test vectors.
  - **RFC 8439 ChaCha20-Poly1305 AEAD (`chacha20_poly1305.c`)**:
    - 256-bit key, 96-bit nonce ChaCha20 stream cipher with quarter-round permutations.
    - Poly1305 one-time authenticator using 130-bit modulo arithmetic ($2^{130}-5$).
    - AEAD construction with additional authenticated data (AAD) padding and length block.
    - Fail-closed constant-time MAC verification rejecting corrupted ciphertext and tampered headers.
  - **RFC 7748 X25519 Diffie-Hellman Key Exchange (`x25519.c`)**:
    - Montgomery curve $y^2 = x^3 + 486662x^2 + x$ over $\mathbb{F}_{2^{255}-19}$.
    - Constant-time Montgomery ladder with conditional swap (`cswap`) to resist side-channel timing analysis.
    - Key clamping (clearing bottom 3 bits, setting bit 254, clearing bit 255).
    - Ephemeral client keypair generation and shared secret derivation.
  - **2048-bit RSA PKCS#1 v1.5 Signature Verification (`rsa.c`)**:
    - Big-integer arithmetic supporting modular exponentiation with public exponent $e = 65537$.
    - PKCS#1 v1.5 signature padding verification against SHA-256 DigestInfo ASN.1 structure.
  - **ASN.1 DER X.509 Certificate Parser & Validator (`x509.c`)**:
    - Parses X.509 v3 certificates, Serial Number, Issuer, Subject, Validity dates (UTCOrGeneralizedTime), Subject Public Key Info (RSA 2048), Subject Alternative Names (DNS names and IP addresses), and Basic Constraints.
    - Validates signature chain against trusted Root CA certificates stored in `/etc/ssl/certs/ca.crt`.
    - Enforces validity time bounds against system RTC clock.
    - Verifies hostname and IP address match against SAN entries.
- **Real-Time Clock Driver (`kernel/drivers/rtc.c`)**:
  - Direct CMOS RTC register reading (ports 0x70 / 0x71).
  - BCD-to-binary decoding, century handling, and POSIX epoch timestamp calculation.
- **TLS 1.3 Client Engine (`kernel/net/tls.c`)**:
  - RFC 8446 compliant TLS 1.3 state machine:
    - `ClientHello` formatting with `supported_versions` (0x0304), `supported_groups` (X25519), `key_share` (client ephemeral share), and `server_name` (SNI).
    - `ServerHello` processing and ephemeral key agreement.
    - Middlebox compatibility mode handling (RFC 8446 Appendix D.4: transparently ignores `ChangeCipherSpec` records).
    - Key derivation for Handshake Traffic Secrets (`client_hs_key`, `server_hs_key`).
    - Encrypted Extensions, Server Certificate, CertificateVerify, and Server Finished processing.
    - X.509 certificate validation against trusted root CA.
    - RSA signature verification over RFC 8446 Section 4.4.3 formatted transcript hash.
    - HMAC-SHA256 verification of Server Finished and derivation of Application Traffic Secrets (`client_app_key`, `server_app_key`).
    - Sending Client Finished and transitioning to `TLS_STATE_CONNECTED`.
    - Robust handling of post-handshake messages (`NewSessionTicket` / inner record type 22) and closure alerts (inner record type 21).
    - Secure memory wiping of all cryptographic session keys upon session close.
- **AstraBrowser HTTPS Integration (`kernel/browser/browser_url.c`, `app_browser.c`)**:
  - Full support for `https://` URLs on default port 443 or explicit custom ports.
  - Hostname resolution via DNS / ARP / IPv4 stack.
  - Live TLS 1.3 encrypted handshake and session establishment.
  - Encrypted HTTP request formatting and secure transmission over `tls_send`.
  - Encrypted response reception over `tls_recv`, stream reassembly, and HTTP status/body extraction.
  - Browser status bar reports verified state: `Ready (HTTPS 200 OK)`.
  - Interactive launch via monitor: `browser https://<host>:<port>/`.

## 23. CSS Engine & Rich Web Layout Subsystem (Phase M)
- **Bounded CSS Engine (`kernel/browser/css_engine.c`, `kernel/include/kernel/css.h`)**:
  - **Selector Matching**:
    - Universal selector (`*`, specificity 0).
    - Tag/element selector (`body`, `h1`, `p`, `div`, `span`, specificity 1).
    - Class selector (`.classname`, specificity 10).
    - ID selector (`#idname`, specificity 100).
    - Inline style attribute (`style="..."`, specificity 1000).
  - **Cascade & Specificity Engine**:
    - Deterministic specificity weighting. When multiple rules match an element, the rule with the highest specificity takes precedence.
    - Tie-breaking: when specificity matches, later declared rules override earlier rules.
    - Text property inheritance: inherited properties (`color`, `font-size`/scale, `font-weight`/bold, `text-align`) automatically cascade from parent DOM nodes to children unless overridden.
  - **Color Parsing (`css_parse_color`)**:
    - 3-digit hexadecimal (`#rgb` expanded to `#rrggbb`).
    - 6-digit hexadecimal (`#rrggbb`).
    - Standard named colors (`black`, `white`, `red`, `green`, `blue`, `yellow`, `cyan`, `magenta`, `gray`, `navy`, `darkblue`, `darkgray`, `lightgray`).
  - **Box Model Properties**:
    - Margins: `margin`, `margin-top`, `margin-bottom`, `margin-left`, `margin-right`.
    - Padding: `padding`, `padding-top`, `padding-bottom`, `padding-left`, `padding-right`.
    - Borders: `border-width`, `border-color`, `border-style`, shorthand `border: 1px solid #hex`.
    - Dimensions: `width`, `background-color`.
    - Flow control: `display: block`, `display: inline`, and `display: none` (skips rendering node and children).
  - **Typography & Font Scaling (`kernel/drivers/gfx.c`)**:
    - `font-size`: Large fonts (`large`, `24px`, `2em`) dynamically mapped to 2x glyph scaling (`scale=2`, 16x32 bitmap cells) via `gfx_draw_char_scale`.
    - `font-weight`: Bold text rendered with dual-pass 1-pixel horizontal offset for heavy visual weight.
    - `text-align`: Left, Center, and Right alignment with line-by-line block re-centering.
  - **Bounded Execution & Memory Safety**:
    - Hard limit of 64 stylesheet rules (`CSS_MAX_RULES = 64`).
    - Fixed 32-character selector limit (`CSS_MAX_SELECTOR_LEN = 32`).
    - Safe non-recursive tokenization without unbounded memory allocation.
    - Malformed CSS (unclosed braces, trailing semicolons, unrecognized properties) safely ignored without memory corruption.
- **Embedded & Inline Stylesheets (`kernel/browser/html_parser.c`)**:
    - Automatically extracts and compiles `<style> ... </style>` stylesheets within `<head>` or `<body>`.
    - Extracts `class="..."` and `id="..."` attributes from HTML tags.
    - Extracts and compiles inline `style="..."` attributes with highest specificity.
- **Deterministic Test Suite & Initramfs Test Pages**:
  - In-kernel self-test (`css_self_test`) verifying hex/named color parsing, universal/tag/class/id specificity ranking, and malformed CSS tolerance, emitting `ASTRA_CSS_PARSER_OK` and `ASTRA_CSS_CASCADE_OK`.
  - Layout verification emitting `ASTRA_CSS_LAYOUT_OK`.
  - Packaged pages: `/www/css_test.html` (selectors, cascade, borders, padding), `/www/rich_page.html` (multi-column card layout, centered text, styled links), `/www/css_malformed.html` (torture test for malformed CSS).
- **Strict Architectural Security Boundary**:
  - **No JavaScript**: Scripting engines, `eval`, and JavaScript execution are strictly forbidden and not implemented.
  - **Kernel Space Browser**: AstraBrowser executes within kernel space; all parsed web content (HTML, CSS) is treated as untrusted input with bounded buffers and bounds checking. It is explicitly documented that AstraBrowser is not yet an isolated user-space sandbox.

## 24. User-Space GUI Applications & Process Isolation (Phase N)

### Process Memory Layout & Isolation
- **Per-Process Page Tables**: Each user process allocates an independent Level-4 page table (`PML4`) mapped via physical memory frames allocated from PMM.
- **Virtual Address Space Partitioning**:
  - **User Space (`0x0000000000000000 .. 0x00007fffffffffff`)**:
    - Executable code segment (.text): Mapped at `0x0000000000400000` (Read-Only, Executable, User-accessible `PTE_USER`).
    - Read-only data (.rodata): Mapped at `0x0000000000401000` (Read-Only, Non-Executable, User-accessible `PTE_USER | PTE_NO_EXEC`).
    - Read-write data / BSS (.bss): Mapped at `0x0000000000402000` (Read/Write, Non-Executable, User-accessible `PTE_USER | PTE_WRITE | PTE_NO_EXEC`).
    - Dedicated User Stack: 16 KiB mapped at `0x0000700000000000 .. 0x0000700000004000` with top at `0x0000700000004000` (Read/Write, Non-Executable, `PTE_USER | PTE_WRITE | PTE_NO_EXEC`).
    - User Guard Page: Virtual address below the user stack (`0x00006fffffffc000`) is left unmapped to trap stack overflows.
  - **Kernel Space (`0xffff800000000000 .. 0xffffffffffffffff`)**:
    - Higher-Half Direct Map (HHDM) and kernel image (`.text`, `.rodata`, `.data`, heap, APIC MMIO) are mirrored into each process's PML4 root to service interrupts and system calls.
    - **Supervisor Protection**: All kernel mappings strictly omit the `PTE_USER` bit (bit 2 is 0). Any read, write, or instruction fetch from user mode (CPL=3) to any kernel address immediately triggers a hardware Page Fault (`#PF`, Vector 14) with `PF Flags: User=1`.

### Privilege Boundaries & Transitions
- **CPL 3 Execution**: Ring 3 entry is achieved via `iretq` after populating an interrupt frame on the kernel stack:
  - User SS: `0x001b` (GDT selector `0x18` with RPL=3).
  - User RSP: `0x0000700000004000`.
  - User RFLAGS: `0x0000000000000202` (Interrupts enabled, reserved bit 1 set).
  - User CS: `0x0023` (GDT selector `0x20` with RPL=3).
  - User RIP: ELF entry point (`0x0000000000400000`).
- **TSS and Privilege Stack Transition**: The Task State Segment (`TSS`) specifies `RSP0` pointing to the kernel thread stack. Upon any interrupt or `int $0x80` from Ring 3, the CPU automatically switches to the secure supervisor stack, preventing stack tampering.
- **Syscall Gate**: IDT vector 128 (`0x80`) is registered as a 64-bit Interrupt Gate with `DPL=3` (`0xEE`), allowing user programs to invoke system calls.

### Secure System Call Interface
- **Validation Rules**:
  - `user_ptr_valid(ptr, len, check_writable)`: Strictly validates that the pointer is non-null, within canonical user address space (`< 0x0000800000000000`), does not cause 64-bit integer overflow, and that every 4 KiB page across `[ptr, ptr + len - 1]` is mapped in the active PML4 with `PTE_USER` (and `PTE_WRITE` if writable).
  - User strings are bounded by `copy_user_string(dst, src_user, max_len)` verifying byte-by-byte user permissions.
  - Raw kernel pointers passed from user mode are rejected with `-EFAULT`.
  - Invalid dimensions, negative coordinates, or oversized requests are rejected with `-EINVAL`.
  - Resource access across process boundaries: Window destruction, event polling, and drawing requests verify that `win->owner_pid == proc->pid`, rejecting unauthorized non-owner processes with `-EBADF`.
- **System Call ABI**:
  - `SYS_GUI_GET_SCREEN_INFO` (10): returns screen width, height, pitch, bpp.
  - `SYS_GUI_CREATE_WINDOW` (11): creates a window owned by caller PID.
  - `SYS_GUI_DESTROY_WINDOW` (12): destroys caller-owned window.
  - `SYS_GUI_GET_EVENT` (13): pops an input event from window's private queue.
  - `SYS_GUI_PRESENT` (14): blits window backbuffer to compositor backbuffer.
  - `SYS_GUI_DRAW_RECT` (15): draws outline rectangle in window client coordinates.
  - `SYS_GUI_FILL_RECT` (16): fills rectangle with color in window client coordinates.
  - `SYS_GUI_DRAW_TEXT` (17): renders text string within window client area.
  - `SYS_GUI_CLEAR` (18): clears window client area to specified color.
  - `SYS_GUI_SET_TITLE` (19): sets window title string.
  - `SYS_SLEEP` (20): puts calling thread to sleep for requested milliseconds.

### Window Service & Event Routing
- **Kernel Framebuffer Ownership**: The kernel graphics subsystem retains exclusive access to physical framebuffer memory. User applications cannot directly map or modify the framebuffer.
- **Per-Window Event Queues**: Each window maintains a private 32-entry circular event buffer (`event_queue`). The window manager delivers client-relative mouse events, key events, and titlebar close button events directly into the window's queue.
- **Lifecycle & Resource Reclamation**: When a process exits (`process_exit`) or faults, `wm_destroy_all_for_pid(proc->pid)` is invoked automatically, destroying all windows created by the process and freeing their surface memory.

### Fault Containment & Security Verification
- If a user process attempts an illegal memory access (e.g. reading supervisor memory at `0xffffffff80400000`), the CPU triggers `#PF` (Vector 14) with `User=1`.
- The kernel intercepts the user fault, records fault information, terminates the offending process with status `-11`, reclaims its address space and windows, and continues running the operating system without panic or crash (`ASTRA_GUI_ISOLATION_OK`).

### Native User GUI Applications
- `/bin/hello_gui.elf`: Native ELF executable loaded from initramfs. Runs at CPL 3, creates a window, renders cards, buttons, text ("Hello from AstraOS User Space", "Ring 3 CPL=3 Verified"), handles events, and exits cleanly with code 0 (`ASTRA_USER_GUI_OK`).
- Concurrent execution: `/bin/counter_gui.elf` runs concurrently with `hello_gui_multi`, verifying independent virtual memory spaces (CR3 roots), independent window management, and concurrent scheduling (`ASTRA_MULTI_PROCESS_GUI_OK`).

### AstraBrowser Migration Assessment & Roadmap
- **Dependency Audit**:
  - Kernel memory: Replaced with bounded user-space dynamic allocator (`umalloc`) backed by `sys_mmap`.
  - Networking & TLS: Uses kernel sockets and in-kernel TLS 1.3 engine exposed via `sys_net_fetch` (31), preserving full X.509 certificate and hostname validation.
  - HTML & CSS parsers: Fully migrated into user space inside `/bin/browser.elf`.
  - Window & GFX: Window creation, drawing, text, and event handling performed via `sys_gui_*` syscalls.
- **Migration Roadmap**:
  1. *Phase N (Completed)*: Ring 3 process isolation, GUI syscall interface, and native ELF GUI applications.
  2. *Phase O (Completed)*: Real Ring 3 user-space AstraBrowser (`/bin/browser.elf`), `sys_mmap`/`sys_munmap`, user VFS access, and TLS-backed HTTP/HTTPS network fetching.

---

## 26. Phase O: User-Space AstraBrowser & Memory Management Architecture

### Overview
Phase O migrates AstraBrowser from kernel space to an isolated, unprivileged Ring 3 ELF application (`/bin/browser.elf`), executing with `CPL=3`. The browser manages its own URL navigation, browser history, HTML DOM construction, CSS parsing and specificity cascade, box model layout, and user-space dynamic memory allocation. Privileged operations—including physical page table manipulation, network hardware drivers, TCP/IP state machines, TLS cryptographic handshakes, and framebuffer compositing—remain strictly inside the kernel behind validated system call gates.

### Browser Process Memory Layout
The user-space browser process executes in a dedicated 4-level paging address space (`CR3`):
- `0x0000000000400000 .. 0x0000000000409000`: ELF LOAD segments:
  - `.text`: Read-Only, Executable (`R-X`, `PTE_USER`)
  - `.rodata`: Read-Only, No-Execute (`R-- NX`, `PTE_USER`)
  - `.data` / `.bss`: Read-Write, No-Execute (`RW- NX`, `PTE_USER`, zero-initialized)
- `0x0000000040000000 .. 0x0000000080000000`: Dynamic anonymous mmap arena (`USER_MMAP_BASE_VA` to `USER_MMAP_LIMIT_VA`):
  - Allocated dynamically on demand via `sys_mmap`.
  - Backed by zero-initialized 4 KiB physical pages allocated through the kernel PMM.
  - Protected with strict `W^X` enforcement (`PROT_READ | PROT_WRITE`, No-Execute).
  - Tracked in `proc->mapped_pas` (up to 1024 pages = 4 MiB capacity).
- `0x0000700000000000 .. 0x0000700000004000`: User Stack:
  - Guard Page: `0x0000700000000000` (Unmapped).
  - Stack Body: `0x0000700000001000 .. 0x0000700000004000` (12 KiB, `RW- NX`, `PTE_USER`).
- `0xffff800000000000 .. 0xffffffffffffffff`: Supervisor Space (Ring 0 only):
  - Accessible only when `CPL=0`. Any user-space read/write attempt triggers an immediate hardware Page Fault (#PF Vector 14, `User=1`).

### User-Space Dynamic Memory Allocator
- AstraBrowser includes an internal dynamic memory allocator `umalloc(size)` backed by `sys_mmap` (Syscall 29) and `sys_munmap` (Syscall 30).
- `umalloc` requests 128 KiB chunks of anonymous zeroed virtual memory from the kernel, aligning requests to 16-byte boundaries.
- Parsing structures, DOM nodes (`dom_node_t`), layout boxes (`layout_box_t`), stylesheets (`stylesheet_t`), and HTML buffers are dynamically allocated in user space.
- Upon application exit or page transition, `ufree_arena()` unmaps allocated virtual pages back to the kernel.

### System Call Boundaries & Security Invariants
All interactions with the operating system occur via software interrupt `int $0x80`:
1. **Memory Syscalls**:
   - `sys_mmap(addr, length, prot, flags)` (29): Allocates zero-filled physical frames, maps them into canonical user virtual space with `PTE_USER` and caller-specified protection. Strict W^X enforcement rejects simultaneous write and execute permissions (`PROT_WRITE | PROT_EXEC`). Requires `MAP_ANONYMOUS`.
   - `sys_munmap(addr, length)` (30): Validates page alignment and canonical bounds, verifies the caller process owns the mapping, unmaps page entries, flushes CPU TLB, frees physical frames, and updates process accounting.
2. **VFS File Syscalls**:
   - `sys_open(path, flags)` (25): Validates user-space path string; opens local initramfs file; returns file descriptor (`fd`).
   - `sys_read(fd, buf, count)` (26): Validates user destination buffer (`user_ptr_valid(buf, count, true)`); reads up to `count` bytes; returns bytes read or 0 on EOF.
   - `sys_close(fd)` (27): Closes file descriptor and releases handle.
   - `sys_stat(path, statbuf)` (28): Populates file size and attributes.
3. **Networking & TLS Syscall**:
   - `sys_net_fetch(url, buf, max_len)` (31): Validates user URL string and destination buffer. Inspects URL scheme:
     - `http://`: Performs TCP connection and HTTP GET request using kernel TCP stack.
     - `https://`: Performs full TLS 1.3 handshake, Diffie-Hellman key exchange (X25519), and authenticates the remote server certificate against local trusted CAs using the kernel's X.509 DER parser and RSA signature verifier.
     - Copies response payload to user buffer and returns payload length.
4. **GUI Syscalls**:
   - `sys_gui_create_window` (11), `sys_gui_get_event` (13), `sys_gui_draw_rect` (15), `sys_gui_fill_rect` (16), `sys_gui_draw_text` (17), `sys_gui_clear` (18), `sys_gui_present` (14).
   - Window ownership is checked on every operation (`win->owner_pid == proc->pid`). Non-owners are rejected with `-EBADF`.
   - Framebuffer memory remains supervisor-only; rendering requests write to the window's kernel-allocated client backbuffer.

### Fault Isolation & Resource Reclamation
- **Containment**: If `/bin/browser.elf` attempts an unauthorized supervisor access or triggers a memory fault, the CPU page fault handler catches the exception (`Vector 14`), logs the faulting address and process name, and terminates the process with exit code `-11`.
- **Automatic Reclamation**: In `process_exit()`, the kernel iterates over `proc->mapped_pas` and frees every physical page allocated via `sys_mmap`, cleans up all user page tables, destroys all windows owned by the process (`wm_destroy_all_for_pid`), and reaps thread stacks.
- **Desktop Stability**: The kernel desktop, compositor, and concurrent applications continue running completely unaffected (`ASTRA_BROWSER_ISOLATION_OK`).

### Migration Status & Unsupported Features
- **Migrated to User Space**:
  - HTML tokenization, tag parsing, entity decoding, and DOM tree creation.
  - CSS parsing, selector matching, specificity cascade, and property inheritance.
  - Box model layout, text measurement, line wrapping, and hyperlink hitboxes.
  - Browser navigation state, back/forward history stacks, and URL bar interaction.
  - User-space memory management via `umalloc` and `sys_mmap`.
- **Kernel Fallback**: The existing in-kernel AstraBrowser implementation (`app_browser.c`) remains available as a fallback via the serial monitor command `browser <url>` and desktop launcher.
- **Unsupported Features**:
  - JavaScript execution and dynamic DOM scripting (`<script>`, `eval`) are not implemented.
  - External remote images (`<img>`) and external resources are upgraded in Phase P.
  - Complex CSS grid/flexbox and CSS transitions/animations remain out of scope.

---

## 27. Phase P: Images, External Resources & Interactive HTML Forms Architecture

### Overview
Phase P expands the Ring 3 AstraBrowser (`/bin/browser.elf`) with three core web capabilities while strictly adhering to user/kernel isolation:
1. **Uncompressed BMP Image Decoding & Rendering**: User-space decoding of standard 24-bit and 32-bit Windows BMP images, complete with header bounds checking, arithmetic overflow validation, bottom-up/top-down stride calculation, and safe GUI bitmap blitting via `sys_gui_draw_bitmap`.
2. **External CSS Resource Fetching & Resolution**: Support for `<link rel="stylesheet" href="...">`, resolving relative URLs against document base URLs (`file:///`, `http://`, `https://`), fetching stylesheets via VFS or kernel TLS/HTTP sockets (`sys_net_fetch`), cascading rules into the document style context, and enforcing resource size and recursion limits.
3. **Interactive HTML Forms & Control State**: Form controls (`<input type="text">`, `<input type="checkbox">`, `<textarea>`, `<input type="submit">`, `<button>`), active keyboard focus tracking, interactive text editing, checkbox toggling, and URL-encoded `GET` query serialization (`application/x-www-form-urlencoded`).

All untrusted image decoding, CSS parsing, and form state tracking execute strictly in Ring 3 CPL 3. The kernel retains privilege over the physical framebuffer, memory allocation, and networking hardware.

### BMP Image Decoding Engine
- **Decoder Location**: Executed entirely in user space in `/bin/browser.elf`.
- **Supported Formats**: Uncompressed 24-bit (`BI_RGB`, 3 bytes/pixel) and 32-bit (`BI_RGB`, 4 bytes/pixel) Windows Bitmap format (`0x4D42` "BM").
- **Security Validation Invariants**:
  - Magic signature verified (`0x4D42`); rejected if corrupted.
  - Total buffer size checked against file header `bfSize`.
  - Header sizes validated (`biSize >= 40`, `biPlanes == 1`, `biCompression == 0`).
  - Bit depth restricted strictly to 24 or 32 bpp.
  - Dimensions validated: `1 <= width <= 512`, `1 <= abs(height) <= 512`.
  - Arithmetic overflow check: `width * height <= 262144` pixels.
  - Decoded memory bounded: maximum 256 KiB allocation via dynamic arena.
  - Row stride padded to 4-byte boundaries: `((width * (bpp / 8) + 3) / 4) * 4`.
  - Handles both standard bottom-up (`height > 0`) and top-down (`height < 0`) row orders.
  - Converts BGR/BGRA row bytes to ARGB32 format (`0xAARRGGBB`) suitable for GUI presentation.
- **Rendering & Fault Tolerance**:
  - Rendered using the privileged `sys_gui_draw_bitmap(win_id, x, y, w, h, pixels)` system call (Syscall 32), which performs kernel surface bounds clipping and pixel format conversion.
  - Missing, invalid, or oversized images trigger a clean fallback: a styled outline box with fallback alt-text (e.g. `[IMG]`) without crashing the browser or kernel.

### External Resource & CSS Resolution
- **Link Tag Parsing**: Detects `<link rel="stylesheet" href="...">` in document headers.
- **URL Resolution Algorithm**:
  - Relative URLs (e.g. `style.css`, `../theme.css`, `/root.css`) resolved against document base URL (`file:///`, `http://`, `https://`).
  - Distinguishes root-relative paths (`/...`) from document-relative paths (`./...` or name).
- **Resource Fetching**:
  - Local `file:///` URLs fetched via `sys_open`, `sys_read`, and `sys_close`.
  - Remote `http://` and `https://` URLs fetched via `sys_net_fetch`, leveraging the kernel's in-kernel TCP stack and TLS 1.3 cryptographic engine with full X.509 certificate validation.
- **Limits & Defenses**:
  - Stylesheet buffer bounded to 8 KiB per external resource.
  - Total rules capped to `MAX_RULES` (64).
  - Bounded reflow: external CSS rules are parsed and appended to the stylesheet before initial document layout, eliminating unbounded reflow loops.
  - Malformed CSS rules are ignored without crashing parser state.

### Interactive HTML Form Controls & Submission
- **Supported Form Controls**:
  - `<input type="text">`: Single-line text input with keyboard focus, cursor rendering, character insertion, and backspace deletion.
  - `<input type="checkbox">`: Toggleable boolean state with visible checkmark rendering.
  - `<textarea>`: Multi-line text editing supporting newline entry.
  - `<input type="submit">` and `<button>`: Clickable submit buttons triggering form submission.
  - `<form action="..." method="GET">`: Form container defining submission target and method.
- **Event Handling & Focus Model**:
  - Uses existing GUI window events: `INPUT_EVENT_MOUSE_DOWN` determines hit detection against control bounding boxes (`form_control_t.x, y, w, h`).
  - Active control ID tracked in `g_browser.focused_control_id`.
  - `INPUT_EVENT_KEY_DOWN` routes typed characters and backspaces to the focused input or textarea.
- **Form Submission**:
  - `GET` submission serializes all form inputs into a standard URL query string: `field1=value1&field2=value2`.
  - Special characters (spaces to `+`, reserved characters to `%XX`) are percent-encoded via `url_encode_append`.
  - Target URL resolved against current document URL, query string appended with `?`, and navigated via `navigate_to`.
  - Remote submissions require explicit user action (clicking submit button or pressing Enter in a text field). No automated transmission occurs.

### Verification & Test Coverage
- `ASTRA_BROWSER_IMAGE_OK`: Valid 4x4 BMP decoded, corrupted magic rejected, oversized dimensions rejected.
- `ASTRA_BROWSER_EXT_CSS_OK`: Relative and absolute URL resolution, external stylesheet download, rule cascade.
- `ASTRA_BROWSER_FORMS_OK`: HTML form tokenization, control population, interactive checkbox toggling, GET query encoding.
- `ASTRA_BROWSER_ISOLATION_OK`: User fault containment and total frame reclamation upon browser exit or fault.

---

## 28. Phase Q: Browser Navigation, Layout & Reliability Hardening Architecture

### Overview
Phase Q hardens and stabilizes AstraBrowser (`/bin/browser.elf`) running in Ring 3 CPL 3, transforming it into a reliable and usable web client without compromising OS isolation:
1. **Full URL Normalization & Link Resolution**: Robust segment-based path normalization (`normalize_path`) resolving `.` and `..` segments while preventing path traversal above root `/`. Seamless resolution of relative (`page.html`, `../img.bmp`), root-relative (`/root.css`), absolute (`file:///`, `http://`, `https://`), and fragment links (`#anchor`).
2. **In-Page Fragment Anchor Navigation**: URL fragments (`#id`) parsed into `parsed_url_t.fragment`. When an in-page fragment link is clicked or navigated, AstraBrowser locates the target layout box by `id_name` and scrolls the viewport smoothly without issuing redundant network/VFS re-fetches or re-parsing the DOM, preserving existing form state.
3. **Bounded Back/Forward History Stack**: A bounded 16-entry history stack (`history[16][256]`) supporting true Back `[<-]` and Forward `[->]` navigation. Oldest entries are safely shifted left on overflow. Navigating to a new page from an intermediate history state correctly truncates forward history before appending the new URL. The address bar stays synchronized with the current page URL.
4. **Vertical Document Scrolling & Viewport Hit-Testing**: Viewport height (`view_h = 382px`) and scrollbar geometry (`WIN_WIDTH - 14px`) with proportional thumb calculation. Mouse clicks on the scrollbar track/thumb page up/down by chunks (`(view_h * 3) / 4`). Unfocused keyboard controls (`KEY_UP`/`KEY_DOWN`, `k`/`j`) scroll by 24px line increments. Hit testing rigorously translates screen mouse coordinates to document virtual coordinates (`vy = my - TOOLBAR_HEIGHT + scroll_y`) and excludes the scrollbar strip from document element hit detection.
5. **Form State Preservation & Safe GET Encoding**: Form values and checkbox toggle states are preserved across ordinary redraws and document scrolling. GET submissions correctly serialize parameters with percent-encoding (`+` for spaces, `%XX` for reserved characters), omitting unchecked checkboxes per RFC standard. Form submission is triggered strictly by explicit user action (clicking submit button or pressing Enter in a focused text field).
6. **Resource Bounds & Error Containment**: URL string lengths capped to 250 characters; invalid URLs and missing VFS/network resources display user-friendly error pages (`404 Not Found` / `Invalid URL`) while keeping the browser in a consistent state without crashing or leaking resources.

### Architectural Invariants
- **Ring 3 Isolation**: All HTML tokenization, CSS cascade, layout box generation, navigation history, and form state tracking reside strictly in user space (`CPL=3`).
- **Syscall Memory Safety**: Privileged kernel syscalls (`sys_gui_draw_bitmap`, `sys_net_fetch`, `sys_mmap`, `sys_gui_*`) validate all user pointers with `user_ptr_valid`, enforce arithmetic overflow checks (`w * h <= 2048 * 2048`), verify window ownership (`win->owner_pid == proc->pid`), and clip drawing to window client rectangles.
- **Fixed User Stack Boundary**: AstraBrowser avoids large automatic stack allocations by utilizing static/BSS storage for self-test apps and dynamic arena allocations (`umalloc` via `sys_mmap`), safely respecting the 16 KiB `USER_STACK_PAGES` limit and unmapped guard page.

### Test Coverage & Verification Markers
- `ASTRA_BROWSER_NAV_OK`: Automated self-test verifying relative link resolution (`../`, `./`), root-relative resolution, in-page fragment resolution, root-clamped dot-dot traversal, and bounded 16-entry history push/shift/truncate/back/forward operations.
- `ASTRA_BROWSER_SCROLL_OK`: Automated self-test verifying vertical layout bounds (`doc_height > view_h`), fragment target box coordinate calculation, `scroll_y` bounds clamping (`[0, max_scroll]`), screen-to-document coordinate translation, and form state preservation across document scrolling.

---

## 29. Phase R Architecture — JavaScript Engine Feasibility, Isolated Sandboxed Runtime & DOM Bridge

### Feasibility Analysis & Engine Evaluation
A comprehensive architectural audit of the AstraOS user-space runtime environment identified the following constraints:
1. **Target & Compiler Flags**:
   - Compiling for `-target x86_64-unknown-none-elf` with `-std=c17`, `-ffreestanding`, `-fno-pic`, `-fno-pie`, `-mno-red-zone`, `-mcmodel=small`, and `-O2`.
   - **Critical Flag**: `-mgeneral-regs-only`. Floating-point/SSE/AVX/x87 instructions are strictly prohibited in user space to prevent unhandled FPU context switching in the kernel scheduler. Any code generating floating-point instructions triggers Clang compilation failure or hardware #UD/#NM exceptions.
2. **Freestanding Runtime Environment**:
   - `-nostdlib`: No standard C library. No `malloc`, `free`, `realloc`, `setjmp`, `longjmp`, `math.h` (`sin`, `cos`, `pow`, `floor`, `ceil`), `stdio.h`, `errno.h`, or pthreads.
3. **User Memory & Stack Budget**:
   - `USER_STACK_PAGES = 4` (16 KiB total stack), preceded by an unmapped guard page (`USER_STACK_GUARD_VA = 0x00006ffffff00000`). Deep recursive call stacks immediately trigger #PF (Vector 14).
   - `PROCESS_MAX_MAPPED_PAGES = 1024` (4 MiB maximum dynamic mmap physical frames per process).

### Engine Feasibility Comparison Matrix

| Engine | License | Standards Scope | Libc / OS Dependencies | Floating Point / FPU | Stack / Memory Footprint | Feasibility Verdict in AstraOS |
|---|---|---|---|---|---|---|
| **QuickJS** | MIT | Complete ES2020 | POSIX libc (`malloc`, `setjmp`, `FILE*`, `fenv`) | Strict requirement for IEEE 754 doubles & `math.h` | >200 KiB RAM; recursive evaluator | **Infeasible**: Violates `-mgeneral-regs-only`, requires complete libc & FPU context switching. |
| **Duktape** | MIT | ES5.1 + partial ES6 | Libc (`setjmp`/`longjmp`, `math.h`, `stdio.h`) | Strict requirement for IEEE 754 doubles | Heavy stack usage; easily overflows 16 KiB stack | **Infeasible**: Cannot compile under `-mgeneral-regs-only`; overflows 16 KiB user stack. |
| **JerryScript** | Apache 2.0 | Microcontroller ES5.1 | CMake build; libc shims | FPU / soft-float required for IEEE 754 | Configurable memory pool, but recursive C call stack | **Infeasible**: Requires complex build toolchain shims, soft-float library, and libc headers. |
| **Cesanta mjs**| Apache 2.0 | Restricted ES6 subset | stdarg, libc types | Double NaN-tagging or 32-bit int | ~50 KiB code; pointer tagging assumptions | **Infeasible without heavy fork**: Depends on standard C headers and variable-length argument lists. |
| **AstraJS Runtime**| MIT / Permissive | Sandboxed Integer ES Subset | Zero external dependencies; pure freestanding C17 | Pure integer arithmetic (s64), zero FPU/SSE instructions | 16-byte `js_value_t`, bounded recursion, <64B stack per frame | **Selected & Implemented**: 100% compliant with `-mgeneral-regs-only`, `-nostdlib`, and 16 KiB stack. |

### Engine Architecture & Language Specification
The **AstraJS Isolated Runtime** is an unprivileged, deterministic JavaScript subset interpreter:
- **Types**: `JS_VAL_UNDEFINED`, `JS_VAL_NULL`, `JS_VAL_BOOL`, `JS_VAL_NUMBER` (signed 64-bit integer), `JS_VAL_STRING` (dynamic string buffer in sandboxed arena), `JS_VAL_FUNCTION`, `JS_VAL_OBJECT` (host object), `JS_VAL_DOM_ELEMENT`.
- **Value Representation**: Compact 16-byte `js_value_t` storing strings as `const char *` references inside the sandboxed arena, avoiding stack-allocated string copies.
- **Statements Supported**: `var`/`let` declarations, user function declarations (`function name(args...) { ... }`), `return [expr];`, `if (cond) { ... } else { ... }`, `while (cond) { ... }`, blocks `{ ... }`, and expression statements.
- **Operators**: Binary arithmetic (`+`, `-`, `*`, `/`, `%`), comparisons (`==`, `!=`, `<`, `<=`, `>`, `>=`), logical operators (`&&`, `||`, `!`), unary minus/plus, parenthesized expressions, and string concatenation (`+`).
- **Zero FPU Instructions**: Integer math only. Division and modulo by zero are trapped with `JS_ERR_DIV_BY_ZERO`.

### Strict Execution Bounds & Security Sandbox
1. **Instruction Budget Limit**: `JS_DEFAULT_INSTRUCTIONS = 10,000`. Incremented on every token and loop iteration. An infinite loop (`while (1) {}`) cleanly halts with `JS_ERR_TIMEOUT`.
2. **Call Stack Recursion Limit**: `JS_DEFAULT_CALL_DEPTH = 16`. Prevents unbounded recursion from exhausting the 16 KiB user process stack, halting cleanly with `JS_ERR_STACK_OVERFLOW`.
3. **Sandboxed Memory Budget**: `JS_DEFAULT_MEM_BUDGET = 32,768` bytes. Strings and dynamic objects are allocated within `vm->mem_pool`. Allocations exceeding the budget halt with `JS_ERR_OUT_OF_MEMORY`.
4. **Source Length Limit**: `JS_MAX_SCRIPT_LEN = 4,096` bytes. Oversized script blocks halt with `JS_ERR_OVERSIZED`.
5. **Privilege Boundary**:
   - Executes 100% inside unprivileged Ring 3 (`/bin/browser.elf`).
   - Never exposes raw kernel pointers, hardware addresses, or framebuffer buffers.
   - Forbidden APIs (`eval`, `Function`, `window.open`, `fetch`, `XMLHttpRequest`, `document.cookie`) immediately trigger `JS_ERR_UNSUPPORTED_API`.
   - Scripts cannot transmit network packets or trigger automated form submissions.

### Minimal DOM Bridge
- `document.getElementById(id)`: Safe lookup in user-space DOM nodes (`dom_node_t.id_name`). Returns `JS_VAL_DOM_ELEMENT` on match, `JS_VAL_NULL` otherwise.
- `elem.textContent = "..."`: Safely updates `dom_node_t.text` in user space. Modified text flows into subsequent layout and rendering passes without reloading the document.
- `console.log(...)`: Formats values and writes non-blocking serial debug output via unprivileged `sys_write(1, ...)`.

### Error Containment & Browser Recovery
Any script error (`JS_ERR_SYNTAX`, `JS_ERR_TIMEOUT`, `JS_ERR_STACK_OVERFLOW`, `JS_ERR_OUT_OF_MEMORY`, `JS_ERR_UNSUPPORTED_API`) is caught safely inside `parse_html()`. The error description is logged non-destructively to the browser status bar (`[JS Error] ...`), and the browser continues parsing and rendering the remaining document without terminating or destabilizing the process or kernel.

### Verification Markers
- `ASTRA_JS_FEASIBILITY_OK`: Unit test suite verifying minimal arithmetic expressions, variable assignments, user function declaration/invocation, syntax error recovery, infinite-loop timeout enforcement, recursion depth limit enforcement, memory budget enforcement, malformed string rejection, and security exception on forbidden host APIs.
- `ASTRA_JS_RUNTIME_OK`: End-to-end DOM bridge test verifying `<script>` parsing, `document.getElementById()`, and dynamic `elem.textContent` DOM text mutation.
- `ASTRA_JS_ISOLATION_OK`: Fault containment test verifying that a malicious/forbidden script invocation (`window.open`) raises a security exception, records the error in the status bar, and leaves subsequent DOM elements intact and responsive.

---

## 30. Phase S Architecture — Safe DOM Integration & JavaScript Runtime Hardening

### Restricted Custom Interpreter Classification
AstraJS is explicitly defined and maintained as a **restricted custom integer interpreter**, not an ECMAScript-compliant JavaScript engine. It is strictly scoped to provide deterministic, bounded user-space scripting for AstraBrowser without floating-point requirements (`-mgeneral-regs-only`), standard C library dependencies (`-nostdlib`), or deep call stacks.

### Confirmed Source Audit Findings & Vulnerability Mitigations
Prior to Phase S enhancements, a comprehensive source audit revealed the following concrete vulnerabilities and architectural defects:
1. **Logical Operator Non-Short-Circuiting**: `&&` and `||` evaluated both sides unconditionally, leading to unexpected side-effects and crashes when accessing properties on guarded null checks (e.g., `elem != null && elem.textContent == "x"`).
2. **Hardware Exception on Integer Division**: Evaluating `LLONG_MIN / -1` triggers a CPU `#DE` (Divide Error, Vector 0) trap in x86-64 hardware. Without explicit software guards, malicious or accidental arithmetic crashed the user browser process.
3. **Unchecked Signed 64-Bit Arithmetic**: Additions, subtractions, and multiplications could silently overflow signed integers, inducing undefined behavior.
4. **Stale DOM Handles Across Navigation**: `document.getElementById()` handles returned naked array indices. If a script retained a handle across page navigation or DOM regeneration, mutations corrupted or modified arbitrary nodes in subsequent documents.
5. **Null Handle Property Access**: Dereferencing `null` or unassigned DOM references lacked explicit type checks, leading to invalid handle traps.
6. **Layout Desynchronization on DOM Mutation**: Modifying `elem.textContent` updated the DOM node's string buffer but did not trigger an explicit layout reflow or repaint cycle.
7. **Cross-Script Variable Scope Loss**: Re-instantiating the interpreter on every `<script>` tag wiped variable definitions and user functions declared in preceding script blocks within the same document.

### JavaScript Runtime Hardening & Correctness
1. **Operator Precedence & Recursive Descent**:
   The expression parser enforces standard operator precedence through layered precedence functions:
   - Primary expressions (literals, identifiers, parenthesized expressions).
   - Postfix/Member expressions (`ident(args...)`, `ident.property`).
   - Unary expressions (prefix `-`, `+`, `!`, supporting arbitrary unary chaining e.g. `!(!x)`).
   - Multiplicative expressions (`*`, `/`, `%`).
   - Additive expressions (`+`, `-`).
   - Relational expressions (`<`, `<=`, `>`, `>=`).
   - Equality expressions (`==`, `!=`).
   - Logical AND (`&&`).
   - Logical OR (`||`).
   - Assignment expressions (`=`).
2. **Short-Circuit Evaluation**:
   The recursive-descent parser and evaluator accept a `do_eval` parameter:
   - `false && expr`: When the LHS is falsy, the RHS is parsed for syntax correctness with `do_eval = false`, suppressing variable lookups, arithmetic, and side effects.
   - `true || expr`: When the LHS is truthy, the RHS is similarly skipped with `do_eval = false`.
   - `if (cond) stmt else stmt`: The unchosen branch is parsed and skipped safely without naive character or brace counting.
3. **Arithmetic Bounds & Overflow Protection**:
   - Division by zero and modulo by zero return `JS_ERR_DIV_BY_ZERO`.
   - Signed 64-bit integer division guards against `lhs == LLONG_MIN && rhs == -1`, returning `JS_ERR_OVERFLOW` and preventing CPU `#DE` traps.
   - Addition, subtraction, and multiplication employ GCC/Clang builtins (`__builtin_add_overflow`, `__builtin_sub_overflow`, `__builtin_mul_overflow`), returning `JS_ERR_OVERFLOW` on limit violations.
4. **Execution & Memory Budgets**:
   - `JS_MAX_SCRIPT_LEN = 4096`: Scripts exceeding source length are rejected immediately before tokenization (`JS_ERR_OVERSIZED`).
   - `JS_DEFAULT_INSTRUCTIONS = 10000`: Every token and loop iteration decrements the budget, halting runaway scripts with `JS_ERR_TIMEOUT`.
   - `JS_DEFAULT_CALL_DEPTH = 16`: Stack recursion depth is tracked and capped, preventing stack overflows on the 16 KiB user stack with `JS_ERR_STACK_OVERFLOW`.
   - `JS_DEFAULT_MEM_BUDGET = 32768`: Arena allocations enforce strict capacity bounds (`JS_ERR_OUT_OF_MEMORY`).

### Safe DOM Bridge Architecture
1. **Document Generation Guards (`doc_generation`)**:
   DOM handles are represented as a compound structure `js_dom_handle_t`:
   ```c
   typedef struct {
       uint32_t doc_generation;
       uint32_t node_index;
   } js_dom_handle_t;
   ```
   Every navigation advances `browser_app_t.doc_generation`. Any attempt to access or mutate an element with a stale generation is rejected with `JS_ERR_INVALID_HANDLE`.
2. **Safe Element Resolution**:
   - `document.getElementById(id)` returns `JS_VAL_DOM_ELEMENT` with the current generation upon match, or `JS_VAL_NULL` if not found.
   - Accessing properties on `JS_VAL_NULL` returns a controlled `JS_ERR_TYPE` error.
3. **Text Sanitization & Form Sync**:
   - Script-provided text for `elem.textContent` is treated strictly as plain text, never interpreted as HTML markup.
   - Text is clamped to the node's maximum capacity (256 bytes) with null termination.
   - If the element corresponds to an interactive form control (`<input>` or `<textarea>`), the internal input buffer is synchronized (`set_input_value`).
4. **Bounded Layout Reflow**:
   - Successful `textContent` mutation invokes `layout_page(b)`, recomputing layout boxes, text dimensions, and scroll bounds.
   - Reflow occurs along a bounded, non-recursive code path, preventing reflow recursion loops.

### Script Lifecycle & Fault Containment
- **Persistent Document Scope**: Scripts within the same document share a persistent VM instance (`b->doc_vm`), allowing functions and variables defined in one `<script>` block to be referenced by subsequent script blocks.
- **Navigation Reset**: `navigate_to()` reinitializes `b->doc_vm` and advances `b->doc_generation`, reclaiming memory pools and wiping previous script state.
- **Repaint Idempotency**: Script execution occurs exclusively during HTML parsing. Window repainting (`render_browser_page`) only draws pre-computed layout boxes and never re-executes `<script>` blocks.
- **Fail-Safe HTML Rendering**: Syntax, timeout, or runtime errors in a script block log a controlled warning in the status bar and do not prevent the rest of the HTML document from parsing and rendering.

### Verification Markers
- `ASTRA_JS_HARDENING_OK`: Automated unit tests verifying operator precedence, unary minus/not chaining, signed integer overflow boundaries, division by zero, `LLONG_MIN / -1` CPU trap prevention, logical `&&`/`||` short-circuiting, recursion depth bounds, and instruction budget exhaustion.
- `ASTRA_DOM_BRIDGE_OK`: Automated integration tests verifying `document.getElementById()` lookup, missing element `null` handling, `textContent` text mutation, stale DOM handle invalidation across document generations, and bounded layout reflow.

---

## 31. Phase T: Safe DOM Events & Interactive Browser Controls

### Objective & Architectural Constraints
Phase T extends AstraOS's hardened AstraJS runtime to support safe, user-triggered DOM interactions while strictly preserving Ring 3 unprivileged execution, bounded resource consumption, and process isolation.
- **Ring 3 Isolation**: All event handling, callback evaluation, hit-testing, and layout mutation execute strictly within the user-space `/bin/browser.elf` process (CPL 3). Zero script code or DOM event structures exist in Ring 0.
- **Bounded Resource Limits**: Registered event callbacks, script execution lengths, instruction budgets, call stack depths, and memory allocations are explicitly capped.
- **Non-Standard Scope**: AstraOS does not implement a full W3C event model (no bubbling, capturing, `EventTarget`, or `addEventListener`). It implements a focused, deterministic click event model designed for lightweight embedded environments.

### Input Path & Chrome Discrimination
1. **Window Manager Event Polling**:
   AstraBrowser polls input events via `sys_gui_get_event` (vector 128 syscall 71).
2. **Chrome vs. Document Viewport Discrimination**:
   - The window chrome consists of a 34px top toolbar (`TOOLBAR_HEIGHT = 34`) and a 14px right scrollbar (`SCROLLBAR_WIDTH = 14`).
   - If a mouse click occurs at $y < \text{TOOLBAR_HEIGHT}$, it is routed exclusively to toolbar controls (Back, Forward, Reload, URL bar, Go button). Under no circumstances can a chrome click activate page elements.
   - If a mouse click occurs at $x \ge \text{win\_width} - \text{SCROLLBAR_WIDTH}$, it is routed exclusively to the vertical scrollbar track/thumb.
3. **Viewport Coordinate Translation**:
   For events in the document content region ($y \ge \text{TOOLBAR_HEIGHT}$ and $x < \text{win\_width} - \text{SCROLLBAR_WIDTH}$):
   $$\text{doc\_x} = \text{mouse\_x}$$
   $$\text{doc\_y} = \text{mouse\_y} - \text{TOOLBAR_HEIGHT} + \text{scroll\_y}$$
   This translation ensures accurate hit-testing regardless of current vertical document scroll offsets.

### Minimal Event Model & Registration Semantics
AstraBrowser supports user-triggered click actions on HTML `<button>` and `<a>` elements via two safe mechanisms:
1. **Declarative HTML Attributes**:
   - Elements can specify `onclick="expression;"` (e.g., `<button onclick="toggle();">Click</button>`).
   - The HTML parser extracts the `onclick` attribute into a bounded 128-byte string field (`fc->onclick` in form controls, `bx->onclick` in layout boxes).
2. **Programmatic DOM Property Assignment**:
   - Scripts can bind callbacks via property assignment on DOM handles:
     - String statement assignment: `btn.onclick = "count = count + 1;";`
     - Function identifier assignment: `btn.onclick = myHandler;`
   - In the runtime, the assignment parser checks the target DOM handle against `vm->doc_generation`. If valid, it invokes the bridge callback `vm->dom_set_onclick(node_idx, action)`.
   - Function references are automatically formatted into execution calls (`myHandler()`), preserving existing function declarations.

### Event Dispatch & Execution Pipeline
When a user click is detected within a clickable element's bounding box:
1. **Reentrancy & Recursion Protection**:
   To prevent recursive event dispatch from exhausting the 16 KiB Ring 3 user stack, the browser enforces a reentrancy lock:
   ```c
   if (b->dispatching_event) return;
   b->dispatching_event = true;
   ```
2. **Instruction Budget Reset**:
   Prior to dispatching an event callback, the document VM's instruction counter is reset (`b->doc_vm.instruction_count = 0`), granting the event a clean 10,000-instruction budget independent of past parsing passes.
3. **Evaluation & Fault Containment**:
   - The callback code is executed via `js_eval(&b->doc_vm, action)`.
   - Script errors (such as accessing properties on `null` for missing elements) log a controlled warning in the status bar and return cleanly without crashing the browser or kernel.
4. **Bounded Reflow & Repaint**:
   - If a script mutates `elem.textContent`, the DOM text is updated, `layout_page(b)` recomputes boxes and scroll limits, and the window is redrawn.
   - Reflow runs synchronously along a bounded, non-recursive path.
5. **Reentrancy Lock Release**:
   `b->dispatching_event = false;` is released upon completion of callback processing.

### Document Lifecycle & Callback Invalidation
- **Document Generation Enforcement**: All DOM handles store `doc_generation`. Navigating to a new URL or reloading increments `b->doc_generation` and frees all per-page memory arenas (`b->doc_arena`).
- **Complete Stale Invalidation**: Previous event handlers and element references are immediately invalidated upon navigation, preventing callbacks from one page from affecting subsequent pages.
- **Link Navigation Preservation**: Clicking an anchor `<a href="page.html">` navigates to the target. If the anchor has an `onclick` handler, the handler executes first; if `href` is `#` or empty, page navigation is suppressed, matching standard web conventions.

### Resource Limits
| Resource | Bound | Enforcement Mechanism |
|---|---|---|
| Max Script Buffer per Block | 4,096 bytes | Pre-allocated in per-document arena |
| Max Onclick Action Length | 128 bytes | Stored directly in `form_control_t` & `layout_box_t` |
| Execution Instruction Budget | 10,000 instructions | Decremented per AST evaluation; resets per click |
| Call Stack Depth | 16 frames | Tracked via `vm->call_depth`; fails with `JS_ERR_STACK_OVERFLOW` |
| Event Dispatch Depth | 1 (Non-reentrant) | Guarded by `b->dispatching_event` boolean flag |
| Maximum DOM Nodes | 128 nodes | Fixed array allocation in `browser_app_t` |
| Document Memory Arena | 64 KiB | Managed via user-space `umalloc` |

### Automated Verification Markers
- `ASTRA_DOM_EVENTS_OK`: Verified via in-engine unit test suite (`js_run_phase_t_self_tests`), asserting programmatic string assignment, function reference assignment, missing element `TypeError` handling, stale generation handle rejection, and state toggling.
- `ASTRA_BROWSER_INTERACTIVE_OK`: Verified via end-to-end browser self-tests (`run_phase_t_self_tests`), asserting toolbar height coordinate discrimination, scrolled document hit-testing, button click dispatch, DOM text mutation, state counter increment, error recovery, link click handling, and reentrancy blocking.







