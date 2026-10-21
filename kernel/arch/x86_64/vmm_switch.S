/* AstraOS Address-Space & Stack Activation Transition */

.section .text
.global vmm_activate_space_and_switch_stack
.type vmm_activate_space_and_switch_stack, @function

/*
 * C signature:
 * void vmm_activate_space_and_switch_stack(phys_addr_t new_cr3,
 *                                          virt_addr_t new_rsp,
 *                                          void (*continuation)(void));
 *
 * Calling convention (System V AMD64 ABI):
 *   %rdi = new_cr3 (physical address of owned PML4 root)
 *   %rsi = new_rsp (virtual address of dedicated stack top)
 *   %rdx = continuation function pointer
 */
vmm_activate_space_and_switch_stack:
    cli

    /* 1. If CR4.PGE is active, clearing PGE invalidates global translations. */
    mov %cr4, %rax
    test $0x80, %rax             /* Check bit 7 (PGE) */
    jz .Lno_pge

    /* PGE active: clear PGE, switch CR3, re-enable PGE */
    mov %rax, %rcx
    btr $7, %rax                 /* Clear PGE */
    mov %rax, %cr4
    mov %rdi, %cr3               /* Switch to owned root */
    mov %rcx, %cr4               /* Restore PGE */
    jmp .Lstack_switch

.Lno_pge:
    mov %rdi, %cr3               /* Switch to owned root */

.Lstack_switch:
    /* 2. Switch stack to dedicated stack */
    mov %rsi, %rsp
    and $-16, %rsp               /* Align to 16-byte boundary */
    xor %rbp, %rbp               /* Clear frame pointer for stack unwinding */
    push $0                      /* ABI alignment: simulate 8-byte return address (%rsp % 16 == 8) */

    /* 3. Transfer control directly to continuation */
    jmp *%rdx

.size vmm_activate_space_and_switch_stack, . - vmm_activate_space_and_switch_stack
