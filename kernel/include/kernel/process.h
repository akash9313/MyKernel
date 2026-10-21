#ifndef ASTRA_KERNEL_PROCESS_H
#define ASTRA_KERNEL_PROCESS_H

#include <kernel/types.h>
#include <kernel/sched.h>
#include <kernel/vmm.h>
#include <kernel/arch/x86_64/idt.h>

#define PROCESS_MAX_PROCESSES 16

/* Standard User Virtual Memory Layout */
#define USER_CODE_BASE_VA      0x0000000000400000ULL
#define USER_STACK_TOP_VA      0x0000700000004000ULL
#define USER_STACK_BOTTOM_VA   0x0000700000000000ULL
#define USER_STACK_PAGES       4
#define USER_STACK_GUARD_VA    0x00006ffffff00000ULL
#define USER_MMAP_BASE_VA      0x0000000040000000ULL
#define USER_MMAP_MAX_VA       0x0000000080000000ULL

typedef u32 pid_t;

typedef enum process_state {
    PROCESS_UNUSED = 0,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_ZOMBIE,
    PROCESS_DEAD
} process_state_t;

typedef struct process {
    pid_t pid;
    process_state_t state;
    char name[32];

    /* Address Space (CR3 Root) */
    phys_addr_t pml4_phys;

    /* User Memory Regions */
    virt_addr_t code_va;
    size_t code_page_count;
    phys_addr_t code_pas[4];

    virt_addr_t user_stack_top;
    virt_addr_t user_stack_bottom;
    size_t stack_page_count;
    phys_addr_t stack_pas[USER_STACK_PAGES];
    virt_addr_t user_stack_guard;

#define PROCESS_MAX_MAPPED_PAGES 1024

    /* Tracked Allocated Physical Frames for Reclamation */
    phys_addr_t mapped_pas[PROCESS_MAX_MAPPED_PAGES];
    size_t mapped_page_count;

    /* Dynamic User MMAP Bump VA */
    virt_addr_t mmap_curr_va;

    /* Main Kernel Thread for Execution Context */
    thread_t *thread;

    /* Termination & Exit Accounting */
    int exit_code;
    bool exited_from_fault;
    u64 fault_vector;
    u64 fault_cr2;
    u64 fault_rip;

    /* Deferred Reclamation Linkage */
    struct process *reap_next;
} process_t;

/* Process Subsystem API */
void process_subsystem_init(void);
struct elf_reader;
process_t *process_create(const char *name, const void *code_bytes, size_t code_len);
process_t *process_create_elf_reader(const char *name, struct elf_reader *reader);
process_t *process_create_elf(const char *name, const void *elf_bytes, size_t elf_size);
process_t *process_create_elf_file(const char *name, const char *vfs_path);
process_t *process_current(void);
void process_exit(int exit_code) __attribute__((noreturn));
void process_exit_from_fault(struct interrupt_frame *frame);
void process_reap_zombies(void);

/* Low-level jump to Ring 3 */
void user_jump_to_ring3(virt_addr_t entry, virt_addr_t user_rsp) __attribute__((noreturn));

/* In-kernel Process and User-Mode Test Suites */
void process_run_tests(void);
void process_gui_run_tests(void);
void process_browser_run_tests(void);

#endif /* ASTRA_KERNEL_PROCESS_H */
