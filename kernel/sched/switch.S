.code64
.section .text

.global thread_switch_asm
.type thread_switch_asm, @function
/*
 * void thread_switch_asm(void **prev_rsp_ptr, void *next_rsp, void *next_thread);
 *
 * System V AMD64 ABI Callee-Saved Registers:
 *   %rbx, %rbp, %r12, %r13, %r14, %r15
 *
 * Arguments:
 *   %rdi: Pointer to location where outgoing thread's RSP is stored
 *   %rsi: Incoming thread's saved RSP
 *   %rdx: Incoming thread_t pointer (to atomically synchronize g_current_thread)
 */
thread_switch_asm:
    /* Save callee-saved registers */
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushfq                          /* Preserve RFLAGS (including current IF state) */

    /* Context switch: swap stack pointers and synchronize g_current_thread */
    movq %rsp, (%rdi)               /* *prev_rsp_ptr = %rsp */
    movq %rdx, g_current_thread(%rip) /* g_current_thread = next */
    movq %rsi, %rsp                 /* %rsp = next_rsp */

    /* Restore incoming thread's context */
    popfq                           /* Restore RFLAGS (resumes incoming IF state) */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    retq

.global thread_entry_trampoline
.type thread_entry_trampoline, @function
/*
 * First-run trampoline for newly initialized threads.
 *
 * Setup before entry (popped by thread_switch_asm):
 *   %rbx: Function pointer to entry(arg)
 *   %r12: Argument pointer arg
 *   %rsp: 16-byte aligned at function boundary
 *   RFLAGS: IF=1 (Interrupts enabled as set in initial stack frame)
 */
thread_entry_trampoline:
    cld                             /* Strict System V ABI: DF=0 before entering C */

    /* Pass argument in %rdi and invoke entry(arg) */
    movq %r12, %rdi

    /* Enable interrupts only after all callee-saved registers are restored and stack is aligned */
    sti

    callq *%rbx

    /* If entry function returns, cleanly terminate thread */
    call thread_exit

    /* Catch-all halt loop in case thread_exit returns */
1:  cli
    hlt
    jmp 1b
