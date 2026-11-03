# AstraOS v1.0 Security Architecture & Hardening Guide

## 1. Privilege Boundaries & CPU Rings

AstraOS strictly enforces the x86-64 hardware privilege boundary between Ring 0 (Kernel Supervisor) and Ring 3 (User Space Applications):
- **GDT Segments**: Separate 64-bit descriptors configured for Kernel Code (`0x08`), Kernel Data (`0x10`), User Code (`0x18 | 3`), and User Data (`0x20 | 3`).
- **Syscall Gate**: Syscalls enter through software interrupt vector 128 (`int 0x80`, DPL=3).
- **RSP0 Isolation**: On every privilege transition to Ring 0, the CPU loads a clean kernel stack pointer from the TSS `RSP0` field.
- **Double Fault IST**: Vector 8 (`#DF`) is serviced on a dedicated Interrupt Stack Table (`IST1`), ensuring unrecoverable stack overflows cannot clobber memory.

---

## 2. Memory Safety & Paging Security

- **Paging Protection**: The user address space is strictly segregated below `0x0000800000000000`. Kernel virtual addresses ($\ge 0xffff800000000000$) have the Page Table Entry Supervisor bit set (`U/S = 0`). Any Ring 3 instruction attempting to read or write kernel space triggers an immediate Page Fault (`#PF`, Vector 14) and process termination (`process_exit_from_fault`).
- **W^X Enforcement**: Kernel code is mapped `Read-Only + Executable` (`RX`). Kernel data is mapped `Read-Write + No-Execute` (`RW+NX`). User ELF segments enforce strict W^X: text is `RX`, data/bss/heap is `RW+NX`.
- **Stack Guard Page**: User stacks are bounded by an unmapped guard page at the stack base. Stack underflows or overflows trigger `#PF` and terminate the offending process (`ASTRA_USER_GUARD_PAGE_OK`).
- **Page Zero Trap**: Physical frame 0 and virtual page 0 are permanently unmapped, causing any NULL pointer dereference to fault immediately.

---

## 3. System Call Pointer Validation & Resource Isolation

- **Buffer Boundary Validation**: Syscall arguments containing user pointers are checked by `vmm_validate_user_ptr()` and `vmm_validate_user_buffer()`. Any pointer falling outside the user address space or spanning into kernel memory is rejected with `-EFAULT`.
- **Arithmetic Overflow Guards**: Syscall parameters (e.g. `sys_mmap`, `sys_munmap`) check for integer overflow on `length + PAGE_SIZE - 1` and enforce a maximum allocation bound of 16 MiB (`ASTRA_AW_MUNMAP_OVERFLOW_DEFENSE_OK`).
- **File Descriptor Privilege Checks**: All open descriptors in `g_file_table` track `owner_pid`. Descriptors owned by Ring 0 (owner 0) are inaccessible from Ring 3. Cross-process descriptor manipulation is blocked with `-EBADF` (`ASTRA_AW_FD_CROSS_PRIV_DEFENSE_OK`).
- **GUI Window Ownership**: Desktop window 0 is protected from user-space modification. Window operations require `owner_pid == cur_pid` (`ASTRA_AW_GUI_CROSS_PRIV_DEFENSE_OK`).
- **Socket Ownership & Reclamation**: TCP sockets track `owner_pid`. When a process terminates, all associated sockets, file descriptors, and address space frames are automatically cleaned up (`ASTRA_AW_SOCKET_OWNER_CLEANUP_OK`).

---

## 4. Network & Cryptographic Hardening

- **ARP Header Validation**: Inbound ARP packets enforce hardware size 6 and protocol size 4 (`ASTRA_AW_ARP_SIZE_VALIDATION_OK`).
- **IPv4 Length Underflow Protection**: Inbound IPv4 packets validate `total_length >= ihl * 4` and `total_length <= frame_length`, preventing unsigned arithmetic underflow attacks (`ASTRA_AW_IPV4_UNDERFLOW_DEFENSE_OK`).
- **HTTP Smuggling Defense**: The HTTP client rejects conflicting `Content-Length` headers and premature chunked stream terminations (`ASTRA_HTTP_HARDENING_OK`).
- **TLS 1.3 Client**: Enforces TLS 1.3 only (protocol version 0x0304), ChaCha20-Poly1305 AEAD, X25519 Montgomery Diffie-Hellman, and RSA/X.509 CertificateVerify validation against an embedded trusted Root CA (`ASTRA_TLS_CERT_VALIDATION_OK`).

---

## 5. Storage Integrity & Crash Recovery

- **Boot Media Write Protection**: Boot drive `hd0` is flagged `read_only` at the block layer; writes fail-closed with `BLOCK_ERR_READ_ONLY` (`ASTRA_AY_BOOT_MEDIA_PROTECTION_OK`).
- **Block Boundary Enforcement**: Rejects `lba + count >= total_sectors` and `lba + count < lba` with `BLOCK_ERR_OUT_OF_BOUNDS`.
- **AstraFS Dual-Set Commit**: Metadata alternates between Set A and Set B with monotonic generation numbers. Power loss during any stage of commit rolls back cleanly to the prior intact generation (`ASTRA_AX_CRASH_RECOVERY_OK`).
- **Pre-Commit ATA Cache Flush**: Data extents are written and flushed to non-volatile disk cache via ATA `0xE7`/`0xEA` before metadata headers are committed (`ASTRA_AX_PRECOMMIT_FLUSH_OK`).

