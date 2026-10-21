# AstraOS Feature Status

## Status Legend
- **Implemented & Tested**: Functionality is implemented, compiled, and verified via automated host/QEMU integration tests.
- **Implemented**: Code exists and compiles, awaiting dedicated unit/stress test suite.
- **Incomplete / In Progress**: Scaffolded or partially implemented.
- **Planned**: Scheduled for upcoming development milestones.

| Subsystem | Feature | Status | Notes / Evidence |
|-----------|---------|--------|------------------|
| **Boot** | Limine Protocol v3 Handshake | **Implemented & Tested** | Verified base revision 3, responses validated |
| **Boot** | UEFI Booting with OVMF | **Implemented & Tested** | Tested on QEMU 9.2.0 + EDK2 across 128M, 512M, 1024M |
| **Boot** | Consolidated Limine Header | **Implemented & Tested** | Single authoritative `third_party/limine/limine.h` (SHA-256 pinned) |
| **CPU** | GDT 64-bit Descriptors | **Implemented & Tested** | Kernel (`0x08`/`0x10`) and user (`0x18`/`0x20`) descriptors staged; CPU in Ring 0 |
| **CPU** | IDT Exception Handling (0..31) | **Implemented & Tested** | Strict System V ABI alignment, `cld` flag clearing, 32 exception gates |
| **CPU** | Deliberate Exception Test (#UD) | **Implemented & Tested** | Isolated test via `test=exception` and `fault` monitor command; asserts Vector 6 |
| **Diagnostics** | COM1 Serial Port (UART 16550) | **Implemented & Tested** | 115200 8N1, FIFO enabled, loopback verified |
| **Diagnostics** | Zero-Heap kprintf Formatter | **Implemented & Tested** | Supports `%s`, `%c`, `%d`, `%u`, `%x`, `%p`, `%%` |
| **Diagnostics** | Kernel Panic & Safe Halt | **Implemented & Tested** | Logs location, fault registers (CR2, CR3), executes `cli; hlt` |
| **Display** | Framebuffer Driver & Font | **Implemented & Tested** | 1280x800 @ 32bpp, 8x16 font, color masks, vertical scroll |
| **Display** | Serial-Only Safe Fallback | **Implemented & Tested** | Tested via `-vga none` and `nofb` cmdline flag |
| **Memory** | Audited Memory Map Accounting | **Implemented & Tested** | Distinguishes RAM from MMIO apertures; overflow checks enforced |
| **Memory** | Physical Page Allocator (PMM) | **Implemented & Tested** | Region-based bitmap allocator; 4 KiB pages; page 0 reserved; metadata reserved |
| **Memory** | In-Kernel PMM Self-Test | **Implemented & Tested** | Synthetic and live test suites; emits `ASTRA_PMM_OK` |
| **Memory** | 4-Level Paging Architecture (VMM) | **Implemented & Tested** | PML4, PDPT, PD, PT allocation & mapping; inactive space tests pass (`ASTRA_VMM_TABLES_OK`) |
| **Memory** | Kernel-Owned Address Space Activation | **Implemented & Tested** | Owned root CR3 active; dedicated 16 KiB stack; emits `ASTRA_OWN_PAGING_OK` |
| **Memory** | CPU Write Protection (CR0.WP) | **Implemented & Tested** | Verified via isolated `test=ro_fault`: #PF Vector 14, Present=1, Write=1 (`ASTRA_PERM_TEST_OK`) |
| **Memory** | Execute-Disable Protection (EFER.NXE) | **Implemented & Tested** | Verified via isolated `test=nx_fault`: #PF Vector 14, Present=1, Fetch=1 (`ASTRA_PERM_TEST_OK`) |
| **Memory** | Unmapped Guard Page Protection | **Implemented & Tested** | Verified via isolated `test=guard_fault`: #PF Vector 14, Present=0 (`ASTRA_PERM_TEST_OK`) |
| **Memory** | Strict W^X Consistency Across HHDM | **Implemented & Tested** | HHDM aliases of kernel `.text` and `.rodata` are Read-Only; zero writable executable aliases |
| **Monitor** | Kernel Serial Shell | **Implemented & Tested** | Supports `help`, `version`, `clear`, `memmap`, `mem`, `vmap <va>`, `fault`, `halt` |
| **Memory** | Kernel Heap (`kmalloc`/`kfree`) | **Implemented & Tested** | Boundary guards, bidirectional coalescing, rollback (`ASTRA_HEAP_OK`, `ASTRA_HEAP_ROLLBACK_OK`) |
| **CPU** | Task State Segment (TSS / IST1) | **Implemented & Tested** | Double fault IST1 stack with unmapped guard page (`ASTRA_TSS_OK`, `ASTRA_DF_TEST_OK`) |
| **CPU** | Local APIC Timer (Vector 32) | **Implemented & Tested** | Periodic timer delivery, EOI acknowledgment, tick accounting (`ASTRA_APIC_TIMER_OK`) |
| **Multitasking** | Cooperative Kernel Threading | **Implemented & Tested** | Guarded thread stacks, ABI-compliant switch, deferred reclamation (`ASTRA_CONTEXT_SWITCH_OK`, `ASTRA_COOP_SCHED_OK`, `ASTRA_THREAD_LIFECYCLE_OK`, `ASTRA_THREAD_GUARD_OK`) |
| **Multitasking** | Blocking, Wait Queues & Events | **Implemented & Tested** | FIFO wait queues, auto-reset events, lost-wakeup prevention (`ASTRA_WAIT_QUEUE_OK`, `ASTRA_EVENT_OK`) |
| **Multitasking** | Tick-Based Sleep & Idle Halt | **Implemented & Tested** | Monotonic deadline queue, wraparound safety, race-safe `sti; hlt` idle loop (`ASTRA_SLEEP_OK`, `ASTRA_IDLE_WAKE_OK`) |
| **Multitasking** | Preemption Guard & Rescheduling Contract | **Implemented & Tested** | Per-thread nesting `preempt_count`, `need_resched`, critical section balance (`ASTRA_PREEMPT_GUARD_OK`, `ASTRA_CRITICAL_SECTION_OK`) |
| **Multitasking** | Timer-Driven Preemption (Phase C) | **Implemented & Tested** | Safe IRQ-exit hook in `irq_common_stub`, non-cooperative spinners verified (`ASTRA_PREEMPT_OK`) |
| **Synchronization** | Kernel Mutexes (`mutex_t`) | **Implemented & Tested** | Non-recursive, ownership tracking, FIFO direct handoff, anti-barging (`ASTRA_MUTEX_OK`, `ASTRA_MUTEX_HANDOFF_OK`) |
| **Synchronization** | Counting Semaphores (`semaphore_t`) | **Implemented & Tested** | Bounded permits, overflow rejection, direct handoff, maskable IRQ signaling (`ASTRA_SEMAPHORE_OK`, `ASTRA_SEMAPHORE_IRQ_OK`) |
| **Synchronization** | Stress & Bounded Buffer (Phase D) | **Implemented & Tested** | 2000-item producer-consumer under timer preemption, 0 deadlocks, 0 leaks (`ASTRA_SYNC_STRESS_OK`) |
| **Testing** | Automated Full Regression Suite | **Implemented & Tested** | 16 automated QEMU suites (128M/512M/1024M, nofb, 12 fault/exception modes) + 3 host suites |
| **Process Foundation** | Minimal Process Abstraction (`process_t`) | **Implemented & Tested** | Distinct PID, isolated address space, user stack & guard page, thread binding |
| **User Mode** | x86-64 CPL3 Execution (`iretq`) | **Implemented & Tested** | User code segment (`0x23`), data (`0x1B`), zeroed registers, verified (`ASTRA_USERMODE_OK`) |
| **System Calls** | Syscall Mechanism (`int $0x80`) & ABI | **Implemented & Tested** | IDT vector 128 (DPL=3), TSS `RSP0` transition, 4 syscalls: `write`, `yield`, `getpid`, `exit` (`ASTRA_SYSCALL_OK`) |
| **Security** | Safe User Pointer Validation | **Implemented & Tested** | Rejects NULL, arithmetic overflow, addresses >= `0x800000000000`, unmapped/supervisor pages |
| **Lifecycle** | Process Exit & Resource Cleanup | **Implemented & Tested** | Clean termination via `sys_exit`, page unmapping, frame freeing, zero leaks (`ASTRA_PROCESS_EXIT_OK`) |
| **Security** | Hardware User Fault Trapping | **Implemented & Tested** | User #PF (Vector 14) trapped, process killed with status -11, kernel isolated intact (`ASTRA_USER_MEMORY_PROTECTION_OK`) |
| **ELF Loader** | ELF64 Executable Validation | **Implemented & Tested** | Validates headers, segments, entry points; rejects corrupt variants (`ASTRA_ELF_HEADER_OK`, `ASTRA_ELF_SEGMENTS_OK`, `ASTRA_ELF_INVALID_INPUT_OK`, `ASTRA_ELF_ROLLBACK_OK`) |
| **User Space** | Native ELF Execution in Ring 3 | **Implemented & Tested** | Loads native ELF into Ring 3, verifies `.bss` zeroing and syscalls (`ASTRA_ELF_USER_EXEC_OK`) |
| **Storage / VFS** | Initramfs Mount & File Lookup | **Implemented & Tested** | In-memory archive mounted at root; path resolution and traversal defenses verified (`ASTRA_INITRAMFS_OK`, `ASTRA_VFS_LOOKUP_OK`) |
| **Storage / VFS** | File Operations (`read`, `pread`, `stat`) | **Implemented & Tested** | Supports opening, sequential and positional reads, EOF, and directory listing (`ASTRA_VFS_READ_OK`, `ASTRA_VFS_INVALID_ARCHIVE_OK`) |
| **Storage / VFS** | VFS-based ELF Execution | **Implemented & Tested** | Native user binary loaded directly from `/bin/user_app.elf` in VFS (`ASTRA_VFS_ELF_EXEC_OK`) |
| **Graphics** | Framebuffer Discovery & Validation | **Implemented & Tested** | Dynamic discovery via Limine; width, height, pitch, 32bpp format verified (`ASTRA_FRAMEBUFFER_OK`) |
| **Graphics** | Double Buffering & Clipping Primitives | **Implemented & Tested** | Contiguous PMM backbuffer; safe clipped pixels, rects, lines, 8x16 bitmap font (`ASTRA_GRAPHICS_OK`) |
| **Input Drivers** | PS/2 Keyboard & Mouse Drivers | **Implemented & Tested** | 8259 PIC remap, IRQ1/IRQ12 unmask, 256-entry interrupt-safe FIFO, 3-byte mouse packets (`ASTRA_INPUT_OK`) |
| **Window Manager**| Compositor & Window Operations | **Implemented & Tested** | Multi-window management, z-ordering, focus, titlebar dragging, close/minimize buttons (`ASTRA_WINDOW_MANAGER_OK`) |
| **Desktop Shell** | Futuristic Dark Desktop | **Implemented & Tested** | Deep navy background, grid, taskbar dock, app launcher, live status/clock, arrow cursor (`ASTRA_DESKTOP_OK`) |
| **GUI Applications**| Text Editor & File Manager | **Implemented & Tested** | Interactive Text Editor (typing, VFS load) & File Manager (directory browsing, launch in editor) (`ASTRA_GUI_APPS_OK`) |
| **VirtualBox Boot**| Oracle VirtualBox 7 EFI ISO Boot | **Implemented & Tested** | El Torito FAT16 EFI ISO boots in VirtualBox; all 43 markers verified (`ASTRA_BOOT_OK` .. `ASTRA_GUI_APPS_OK`) |
| **Networking** | PCI Bus Enumeration & BAR Mapping | **Implemented & Tested** | Type 0 header decoding, MMIO BAR0 dynamically mapped into kernel PML4 (`ASTRA_PCI_NET_DEVICE_OK`) |
| **Networking** | Intel 82540EM (E1000) Gigabit NIC | **Implemented & Tested** | Circular DMA descriptor rings, physical memory buffers, EEPROM MAC discovery, polling & IRQ delivery (`ASTRA_NET_DRIVER_OK`) |
| **Networking** | Ethernet Framing & ARP Subsystem | **Implemented & Tested** | 14-byte IEEE 802.3 headers, dynamic ARP resolution table, reply caching (`ASTRA_ARP_OK`) |
| **Networking** | IPv4 & ICMP Protocol Handlers | **Implemented & Tested** | IPv4 header checksum verification, fragmentation validation, ICMP Echo Request/Reply (`ASTRA_IPV4_OK`) |
| **Networking** | UDP & DHCP Client | **Implemented & Tested** | Dynamic IPv4 configuration via DISCOVER/OFFER/REQUEST/ACK, gateway and DNS extraction (`ASTRA_DHCP_OK`) |
| **Networking** | DNS Resolver Subsystem | **Implemented & Tested** | UDP-based RFC 1035 A-record resolution, domain query encoding, response parsing (`ASTRA_DNS_OK`) |
| **Networking** | TCP State Engine | **Implemented & Tested** | 3-way handshake (SYN, SYN-ACK, ACK), sequence/ack tracking, FIN teardown, retransmit timeouts (`ASTRA_TCP_OK`) |
| **Networking** | HTTP/1.1 Client | **Implemented & Tested** | GET request formatting, HTTP status code extraction, chunk/body reception (`ASTRA_HTTP_CLIENT_OK`) |
| **Networking** | Socket Syscalls & User Client | **Implemented & Tested** | User-space socket API (`sys_net_*`), strict user-pointer checks, native `/bin/net_client.elf` executable |
| **Diagnostics** | Monitor Network Commands | **Implemented & Tested** | Interactive serial monitor commands: `ifconfig`, `dhcp`, `ping`, `nslookup`, `http` |
| **Web Browser** | URL Parsing & Scheme Dispatch | **Implemented & Tested** | Supports `http://`, `file:///`, and explicit rejection of `https://` with TLS roadmap notice |
| **Web Browser** | Minimal HTML Tokenizer & Parser | **Implemented & Tested** | Bounded non-recursive parsing up to 128 nodes, entity decoding, malformed tag recovery |
| **Web Browser** | Layout Engine & Typography | **Implemented & Tested** | Word-wrapping, headings (H1-H3), lists, horizontal rules, clickable hyperlink hitboxes |
| **Web Browser** | Local VFS Page Navigation | **Implemented & Tested** | Packaged initramfs test suite (`/www/index.html`, `/www/about.html`, etc.) (`ASTRA_BROWSER_RENDER_OK`) |
| **Web Browser** | HTTP Protocol Integration | **Implemented & Tested** | HTTP/1.1 GET formatting, header parsing, body extraction over TCP (`ASTRA_BROWSER_HTTP_OK`) |
| **Web Browser** | GUI Browser Window & Chrome | **Implemented & Tested** | Back/Forward/Reload/Go buttons, editable URL bar, vertical scrollbar, status bar, desktop launcher integration |
| **Cryptography**| Hardware Entropy (`rdrand`) | **Implemented & Tested** | CPUID bit 30 detection, fail-closed entropy generation; non-insecure fallback strictly enforced |
| **Cryptography**| FIPS 180-4 SHA-256 Hash | **Implemented & Tested** | 32-bit big-endian SHA-256; verified against NIST CAVP known-answer vectors |
| **Cryptography**| RFC 2104 / RFC 5869 HMAC & HKDF | **Implemented & Tested** | HKDF-Extract/Expand and RFC 8446 Section 7.1 `hkdf_expand_label`; verified against RFC 4231 & 5869 |
| **Cryptography**| RFC 8439 ChaCha20-Poly1305 AEAD | **Implemented & Tested** | 256-bit key, 96-bit nonce AEAD cipher; verified against RFC 8439 test vectors; fail-closed MAC |
| **Cryptography**| RFC 7748 X25519 Key Agreement | **Implemented & Tested** | Montgomery curve scalar multiplication with constant-time cswap ladder; verified against RFC 7748 |
| **Cryptography**| 2048-bit RSA PKCS#1 v1.5 Verification | **Implemented & Tested** | Big-integer modular exponentiation, DigestInfo parsing, signature verification |
| **Certificates**| ASN.1 DER X.509 Certificate Parser | **Implemented & Tested** | X.509 v3 parser, SAN (DNS/IP) matching, CA trust chain validation, RTC clock validity checks (`ASTRA_TLS_CERT_VALIDATION_OK`) |
| **Hardware** | CMOS Real-Time Clock (RTC) Driver | **Implemented & Tested** | Direct CMOS I/O port reading (0x70/0x71), BCD conversion, POSIX timestamp calculation |
| **Secure Net** | RFC 8446 TLS 1.3 Client Engine | **Implemented & Tested** | Full TLS 1.3 handshake, Middlebox compatibility mode, session key derivation, record encryption/decryption (`ASTRA_TLS_HANDSHAKE_OK`) |
| **Secure Net** | AstraBrowser HTTPS Navigation | **Implemented & Tested** | Encrypted `https://` browsing to port 443 / custom ports, secure request/response (`ASTRA_BROWSER_HTTPS_OK`) |
| **VirtualBox Boot**| VirtualBox 7 EFI HTTPS Boot | **Implemented & Tested** | Boots ISO in VirtualBox 7 with EFI and E1000 NAT; all 54 markers verified (`ASTRA_BOOT_OK` .. `ASTRA_TLS_CERT_VALIDATION_OK`) |
| **CSS Engine** | Bounded CSS Tokenizer & Parser | **Implemented & Tested** | Universal, tag, class, ID selectors, inline `style="..."`, bounded buffers (`ASTRA_CSS_PARSER_OK`) |
| **CSS Engine** | Cascade, Specificity & Inheritance | **Implemented & Tested** | Specificity weighting (1000/100/10/1/0), tie-breaking, text property inheritance (`ASTRA_CSS_CASCADE_OK`) |
| **CSS Layout** | Box Model & Display Properties | **Implemented & Tested** | Margins, paddings, borders, width, `display: block/inline/none`, text alignment (`ASTRA_CSS_LAYOUT_OK`) |
| **CSS Typography**| Scaled Bitmap Typography | **Implemented & Tested** | 2x scaled glyph rendering (`gfx_draw_char_scale`) for headings, bold font weight offsets |
| **VirtualBox Boot**| VirtualBox 7 EFI Rich Web Boot | **Implemented & Tested** | Verified in VirtualBox 7 EFI; all 57 boot markers verified (`ASTRA_BOOT_OK` .. `ASTRA_CSS_LAYOUT_OK`) |
| **Ring 3 GUI** | Native ELF User-Space GUI Application | **Implemented & Tested** | `/bin/hello_gui.elf` runs at CPL 3, creates window, draws cards/buttons/text, handles input, exits cleanly (`ASTRA_USER_GUI_OK`) |
| **Multitasking GUI**| Concurrent Independent User GUI Processes | **Implemented & Tested** | Two simultaneous GUI apps (`hello_gui_multi`, `counter_gui`) running with distinct PIDs, PML4 roots, and windows (`ASTRA_MULTI_PROCESS_GUI_OK`) |
| **GUI Security** | Process Isolation & Fault Containment | **Implemented & Tested** | User pointer validation, non-owner window rejection (`-EBADF`), user #PF trapped, owned windows auto-reclaimed (`ASTRA_GUI_ISOLATION_OK`) |
| **VirtualBox Boot**| VirtualBox 7 EFI User-Space GUI Boot | **Implemented & Tested** | Verified in VirtualBox 7 EFI; all 60 boot markers verified (`ASTRA_BOOT_OK` .. `ASTRA_GUI_ISOLATION_OK`) |
| **User Memory** | User-Space Page Allocation (`sys_mmap`/`sys_munmap`) | **Implemented & Tested** | Syscalls 29/30; anonymous 4 KiB pages, W^X enforcement, zero-initialization, automatic reclamation on termination (`ASTRA_MMAP_VALIDATION_OK`, `ASTRA_USER_MMAP_OK`) |
| **User Browser**| Real Ring 3 User-Space AstraBrowser ELF | **Implemented & Tested** | `/bin/browser.elf` (51.8 KiB); CPL 3 execution; HTML/CSS parser, cascade, box layout, `umalloc` arena, window GUI, event loop (`ASTRA_USER_BROWSER_OK`, `ASTRA_USER_BROWSER_EXEC_OK`) |
| **Browser Isolation**| Browser Hardware Fault Containment & Cleanup | **Implemented & Tested** | Unauthorized supervisor access triggers #PF (Vector 14); process killed (-11); mmap pages & GUI windows reaped cleanly (`ASTRA_BROWSER_ISOLATION_OK`) |
| **Web Images**  | Uncompressed BMP Decoding & GUI Rendering | **Implemented & Tested** | 24-bit/32-bit BMP decoder in Ring 3; header/stride/dimensions validation; safe bitmap blitting syscall (`sys_gui_draw_bitmap`); fallback boxes for missing/corrupt images (`ASTRA_BROWSER_IMAGE_OK`) |
| **External CSS**| `<link rel="stylesheet">` & URL Resolution | **Implemented & Tested** | Relative/absolute URL resolution against base URL (`file:///`, `http://`, `https://`); bounded VFS/net fetch; rule cascade into stylesheet (`ASTRA_BROWSER_EXT_CSS_OK`) |
| **HTML Forms**  | Interactive Form Controls & GET Submission | **Implemented & Tested** | `<input type="text|checkbox|submit">`, `<textarea>`, `<button>`; keyboard focus & text editing; checkbox toggling; URL-encoded GET query serialization (`ASTRA_BROWSER_FORMS_OK`) |
| **Browser Navigation** | URL Normalization, Links & Bounded History | **Implemented & Tested** | Segment-based path normalization (`normalize_path`), relative/root-relative links, bounded 16-entry history stack, address bar sync, Back/Forward navigation (`ASTRA_BROWSER_NAV_OK`) |
| **Layout & Scroll** | Fragment Anchors, Vertical Scroll & Hit-Testing | **Implemented & Tested** | In-page `#fragment` jumps without re-fetch, scrollbar track/thumb paging, keyboard scroll (`KEY_UP`/`KEY_DOWN`), document coordinate hit-testing, form state preservation (`ASTRA_BROWSER_SCROLL_OK`) |
| **JS Feasibility**  | Toolchain Audit & Feasibility Gate | **Implemented & Tested** | Comprehensive evaluation of 5 engines; integer-only runtime selected due to `-mgeneral-regs-only`, `-nostdlib`, and 16 KiB stack (`ASTRA_JS_FEASIBILITY_OK`) |
| **JS Engine Runtime**| Isolated Ring 3 AstraJS & DOM Bridge | **Implemented & Tested** | Sandboxed integer JS subset (`var`, `function`, `if/else`, `while`, expressions); `document.getElementById()`, `elem.textContent` mutation; instruction/stack/memory budget guards (`ASTRA_JS_RUNTIME_OK`) |
| **JS Security & Isolation**| Fault Containment & Privilege Boundary | **Implemented & Tested** | Pure Ring 3 unprivileged execution; security exceptions for forbidden APIs (`window.open`, `fetch`, `eval`); error containment without browser/kernel crash (`ASTRA_JS_ISOLATION_OK`) |
| **JS Runtime Hardening** | Expression Precedence, Overflow & Short-Circuit | **Implemented & Tested** | Correct operator precedence, unary minus/not chaining, short-circuit `&&`/`||`, 64-bit overflow trapping (`__builtin_*_overflow`, `LLONG_MIN/-1` guard), bounded call stack (16) & instruction counter (10k) (`ASTRA_JS_HARDENING_OK`) |
| **Safe DOM Bridge** | Document Generation Handles & Safe Reflow | **Implemented & Tested** | `doc_generation` handle validation preventing stale element access across navigation; `null` handle property access protection; text-only sanitization; form input sync; bounded layout reflow (`ASTRA_DOM_BRIDGE_OK`) |
| **DOM Events Model**| Bounded Callback Registration & Function Binding | **Implemented & Tested** | Declarative `onclick="..."` HTML attributes and programmatic `btn.onclick = ...` property binding; generation-checked; reentrancy guard (`ASTRA_DOM_EVENTS_OK`) |
| **Interactive Controls**| Browser Hit-Testing, Event Dispatch & Reflow | **Implemented & Tested** | Viewport translation accounting for 34px toolbar and scroll offset; button click dispatch; DOM text mutation; state toggling; reflow and repaint (`ASTRA_BROWSER_INTERACTIVE_OK`) |
| **JS Adversarial Testing**| Lexer, Scoping, Budget & Re-declaration Hardening | **Implemented & Tested** | Empty string preservation (`len == 0`), raw escape length preservation, unclosed function body syntax check, parameter scope isolation without global mutation, slot reuse on re-declaration, 500-case host fuzzing (`ASTRA_JS_ADVERSARIAL_OK`) |
| **Browser Security Regression**| Empty Onclick, Mutation Throttling & Stale Rejection | **Implemented & Tested** | Empty/malformed `onclick` recovery, timeout on infinite loops, mutation coalescing (throttle to 32 per event), boundary coordinate checking, stale handle rejection across navigation (`ASTRA_BROWSER_SECURITY_OK`) |
| **VirtualBox Boot**| VirtualBox 7 EFI Phase U Security Regression Boot | **Implemented & Tested** | Verified in VirtualBox 7 EFI; all 79 boot markers verified (`ASTRA_BOOT_OK` .. `ASTRA_BROWSER_SECURITY_OK`) |

