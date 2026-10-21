.code64
.section .text

.extern exception_handler

.macro ISR_NOERR index
.global isr_stub_\index
isr_stub_\index:
    pushq $0
    pushq $\index
    jmp isr_common_stub
.endm

.macro ISR_ERR index
.global isr_stub_\index
isr_stub_\index:
    pushq $\index
    jmp isr_common_stub
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

isr_common_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld
    movq %rsp, %rdi
    call exception_handler

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp
    iretq

.extern interrupt_handler

.macro IRQ_STUB index
.global irq_stub_\index
irq_stub_\index:
    pushq $0
    pushq $\index
    jmp irq_common_stub
.endm

IRQ_STUB 32
IRQ_STUB 33
IRQ_STUB 34
IRQ_STUB 35
IRQ_STUB 36
IRQ_STUB 37
IRQ_STUB 38
IRQ_STUB 39
IRQ_STUB 40
IRQ_STUB 41
IRQ_STUB 42
IRQ_STUB 43
IRQ_STUB 44
IRQ_STUB 45
IRQ_STUB 46
IRQ_STUB 47
IRQ_STUB 255

irq_common_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld
    movq %rsp, %rdi
    call interrupt_handler

    /*
     * Phase C: IRQ-exit preemption hook.
     *
     * Stack layout at this point (RSP points to saved r15):
     *   RSP+0  .. RSP+112  : 15 saved GP regs (r15..rax)
     *   RSP+120             : vector
     *   RSP+128             : error_code (0)
     *   RSP+136             : RIP   (CPU-pushed)
     *   RSP+144             : CS    (CPU-pushed)
     *   RSP+152             : RFLAGS (CPU-pushed)  <- interrupted context's IF
     *   RSP+160             : RSP   (CPU-pushed)
     *   RSP+168             : SS    (CPU-pushed)
     *
     * Pass the interrupted thread's RFLAGS as the argument.  If the scheduler
     * decides to preempt, it switches to another thread via thread_switch_asm.
     * When this thread is later rescheduled it will resume immediately after
     * thread_switch_asm inside sched_try_preempt_irq_exit, return here, and
     * fall through to the register restore + iretq below.
     */
    movq 152(%rsp), %rdi          /* interrupted_rflags = frame->rflags */
    call sched_try_preempt_irq_exit

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp
    iretq

.extern syscall_dispatcher
.global syscall_stub_128
syscall_stub_128:
    pushq $0
    pushq $128
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld
    movq %rsp, %rdi
    call syscall_dispatcher

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp
    iretq

.global user_jump_to_ring3
.type user_jump_to_ring3, @function
/*
 * void user_jump_to_ring3(virt_addr_t entry, virt_addr_t user_rsp);
 *
 * Enters x86-64 CPL3 (user mode) via iretq.
 * Arguments:
 *   %rdi: Entry point RIP
 *   %rsi: User stack pointer RSP
 */
user_jump_to_ring3:
    /* Set data segment registers to user data selector (0x18 | 3 = 0x1B) */
    movw $0x1B, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    /* Push iretq stack frame */
    pushq $0x1B                 /* SS: User Data Segment with RPL=3 */
    pushq %rsi                  /* RSP: User Stack Pointer */
    pushfq                      /* Current RFLAGS */
    popq %rax
    orq $0x200, %rax            /* Set IF (Interrupts Enabled) */
    andq $~0x3000, %rax         /* Clear IOPL (IOPL = 0) */
    pushq %rax                  /* RFLAGS */
    pushq $0x23                 /* CS: User Code Segment with RPL=3 */
    pushq %rdi                  /* RIP: User Entry Point */

    /* Zero all GP registers to prevent kernel information leakage */
    xorq %rax, %rax
    xorq %rbx, %rbx
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rsi, %rsi
    xorq %rdi, %rdi
    xorq %rbp, %rbp
    xorq %r8, %r8
    xorq %r9, %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15

    iretq

