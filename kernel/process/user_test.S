.code64
.section .rodata

.global user_prog1_start
.global user_prog1_end
user_prog1_start:
    /*
     * User Program 1: Minimal Process Demo
     *
     * Runs in Ring 3 (CPL=3).
     * Tests:
     *   1. sys_getpid (SYS_GETPID=3)
     *   2. invalid syscall number (999 -> -ENOSYS)
     *   3. sys_write with NULL buffer -> -EFAULT
     *   4. sys_write with kernel buffer -> -EFAULT
     *   5. sys_write with valid message -> 27
     *   6. sys_yield (SYS_YIELD=2)
     *   7. sys_exit(0) (SYS_EXIT=4)
     */

    /* Test 1: getpid() */
    movq $3, %rax               /* SYS_GETPID = 3 */
    int $0x80
    testq %rax, %rax            /* PID must be > 0 */
    jle .Lfail

    /* Test 2: invalid syscall number 999 */
    movq $999, %rax
    int $0x80
    cmpq $-38, %rax             /* -ENOSYS = -38 */
    jne .Lfail

    /* Test 3: sys_write with NULL buffer */
    movq $1, %rax               /* SYS_WRITE = 1 */
    movq $1, %rdi               /* fd = 1 (stdout) */
    xorq %rsi, %rsi             /* buf = NULL */
    movq $10, %rdx              /* count = 10 */
    int $0x80
    cmpq $-14, %rax             /* -EFAULT = -14 */
    jne .Lfail

    /* Test 4: sys_write with kernel pointer (0xffffffff80000000) */
    movq $1, %rax
    movq $1, %rdi
    movabsq $0xffffffff80000000, %rsi
    movq $10, %rdx
    int $0x80
    cmpq $-14, %rax             /* -EFAULT = -14 */
    jne .Lfail

    /* Test 5: sys_write with valid message */
    movq $1, %rax
    movq $1, %rdi
    leaq msg_demo(%rip), %rsi
    movq $27, %rdx              /* length = 27 */
    int $0x80
    cmpq $27, %rax
    jne .Lfail

    /* Test 6: sys_yield() */
    movq $2, %rax               /* SYS_YIELD = 2 */
    int $0x80
    testq %rax, %rax
    jnz .Lfail

    /* Test 7: sys_exit(0) - Normal clean exit */
    movq $4, %rax               /* SYS_EXIT = 4 */
    xorq %rdi, %rdi             /* exit_code = 0 */
    int $0x80

    /* Should not be reached */
1:  jmp 1b

.Lfail:
    /* Exit with error code 99 if any boundary test fails */
    movq $4, %rax               /* SYS_EXIT = 4 */
    movq $99, %rdi              /* exit_code = 99 */
    int $0x80
2:  jmp 2b

msg_demo:
    .ascii "Hello from AstraOS Ring 3!\n"
    .balign 8

user_prog1_end:


.global user_prog2_fault_start
.global user_prog2_fault_end
user_prog2_fault_start:
    /*
     * User Program 2: Negative Memory Protection Test
     *
     * Runs in Ring 3 (CPL=3).
     * Attempts to read from kernel-space memory (0xffffffff80400000).
     * Must trigger CPU Page Fault (#PF, Vector 14) with User violation.
     */
    movabsq $0xffffffff80400000, %rax
    movq (%rax), %rbx           /* Illegal access -> Hardware #PF */

    /* If it somehow doesn't fault, exit with failure code 77 */
    movq $4, %rax
    movq $77, %rdi
    int $0x80
user_prog2_fault_end:

.global user_prog3_gui_fault_start
.global user_prog3_gui_fault_end
user_prog3_gui_fault_start:
    /*
     * User Program 3: GUI Window Creation followed by Deliberate Fault
     *
     * 1. Calls SYS_GUI_CREATE_WINDOW (11) from Ring 3 to allocate a window.
     * 2. Attempts illegal read from kernel address 0xffffffff80400000.
     * 3. Must trigger #PF Vector 14; kernel must reclaim window and memory.
     */
    movq $11, %rax              /* SYS_GUI_CREATE_WINDOW = 11 */
    xorq %rdi, %rdi             /* NULL title */
    movq $100, %rsi             /* x */
    movq $100, %rdx             /* y */
    movq $200, %r10             /* w */
    movq $150, %r8              /* h */
    int $0x80

    /* Deliberately read from supervisor memory */
    movabsq $0xffffffff80400000, %rax
    movq (%rax), %rbx

    /* Failure if reached */
    movq $4, %rax
    movq $77, %rdi
    int $0x80
user_prog3_gui_fault_end:

.global user_prog4_mmap_fault_start
.global user_prog4_mmap_fault_end
user_prog4_mmap_fault_start:
    /*
     * User Program 4: MMAP Allocation & Hardware Isolation Fault
     *
     * 1. Calls SYS_MMAP (29) to allocate 8192 bytes (2 pages).
     * 2. Writes to the newly allocated pages.
     * 3. Attempts illegal access to unmapped supervisor memory (0xffffffff80400000).
     * 4. Must trigger #PF Vector 14; kernel must reclaim all allocated pages and PML4.
     */
    movq $29, %rax              /* SYS_MMAP = 29 */
    xorq %rdi, %rdi             /* addr = NULL */
    movq $8192, %rsi            /* length = 8192 */
    movq $3, %rdx               /* prot = PROT_READ | PROT_WRITE (3) */
    movq $0x22, %r10            /* flags = MAP_ANONYMOUS | MAP_PRIVATE (0x22) */
    int $0x80

    testq %rax, %rax
    jle .Lmmap_fail

    /* Write to the mapped memory */
    movq $0x12345678, (%rax)

    /* Deliberately access unmapped supervisor memory to trigger #PF */
    movabsq $0xffffffff80400000, %rcx
    movq (%rcx), %rdx

.Lmmap_fail:
    movq $4, %rax
    movq $88, %rdi
    int $0x80
5:  jmp 5b

user_prog4_mmap_fault_end:


