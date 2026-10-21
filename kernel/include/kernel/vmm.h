#ifndef ASTRA_KERNEL_VMM_H
#define ASTRA_KERNEL_VMM_H

#include <kernel/types.h>

#include <kernel/boot.h>

/* Page Table Entry Hardware Bits (x86-64 4-Level Paging) */
#define PTE_PRESENT        (1ULL << 0)
#define PTE_WRITABLE       (1ULL << 1)
#define PTE_USER           (1ULL << 2)
#define PTE_PWT            (1ULL << 3)
#define PTE_PCD            (1ULL << 4)
#define PTE_ACCESSED       (1ULL << 5)
#define PTE_DIRTY          (1ULL << 6)
#define PTE_HUGE           (1ULL << 7)
#define PTE_GLOBAL         (1ULL << 8)
#define PTE_NX             (1ULL << 63)

#define PTE_ADDR_MASK      0x000FFFFFFFFFF000ULL
#define PTE_2MIB_ADDR_MASK 0x000FFFFFFFE00000ULL
#define PTE_1GIB_ADDR_MASK 0x000FFFFFFC000000ULL
#define CR3_ADDR_MASK      0x000FFFFFFFFFF000ULL

/* Explicit VMM Mapping Flags */
#define VMM_FLAG_READ      (1U << 0)
#define VMM_FLAG_WRITE     (1U << 1)
#define VMM_FLAG_USER      (1U << 2)
#define VMM_FLAG_NO_EXEC   (1U << 3)
#define VMM_FLAG_PWT       (1U << 4)
#define VMM_FLAG_PCD       (1U << 5)
#define VMM_FLAG_PAT       (1U << 6)
#define VMM_FLAG_GLOBAL    (1U << 7)

/* Dedicated Stack Layout */
#define KERNEL_GUARD_PAGE_VA   0xffffffff80400000ULL
#define KERNEL_STACK_BOTTOM_VA 0xffffffff80401000ULL
#define KERNEL_STACK_TOP_VA    0xffffffff80405000ULL
#define KERNEL_STACK_PAGE_COUNT 4

/* Dedicated Double-Fault IST1 Guarded Stack Layout */
#define DOUBLE_FAULT_GUARD_VA        0xffffffff80410000ULL
#define DOUBLE_FAULT_STACK_BOTTOM_VA 0xffffffff80411000ULL
#define DOUBLE_FAULT_STACK_TOP_VA    0xffffffff80413000ULL
#define DOUBLE_FAULT_STACK_PAGES     2

/* Local APIC MMIO Definitions */
#define APIC_DEFAULT_BASE_PA         0xfee00000ULL
#define APIC_VIRT_BASE               0xffffffff80700000ULL

/* Linker Section Symbols */
extern char __kernel_start[];
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __bss_start[];
extern char __bss_end[];
extern char __kernel_end[];

/* VMM Status & Error Codes */
#define VMM_SUCCESS             0
#define VMM_ERR_INVALID_ARG    -1
#define VMM_ERR_NONCANONICAL   -2
#define VMM_ERR_UNALIGNED      -3
#define VMM_ERR_NOMEM          -4
#define VMM_ERR_ALREADY_MAPPED -5
#define VMM_ERR_HUGE_CONFLICT  -6
#define VMM_ERR_NOT_MAPPED     -7
#define VMM_ERR_UNSUPPORTED    -8

struct vmm_mapping {
    bool present;
    phys_addr_t paddr;       /* Physical address translated to */
    size_t page_size;        /* 4096 (4 KiB), 2097152 (2 MiB), or 1073741824 (1 GiB) */
    u32 flags;               /* High-level flags: VMM_FLAG_READ, WRITE, USER, NO_EXEC, etc. */
    /* Effective permissions across all levels */
    bool writable;
    bool user;
    bool no_exec;
    bool pwt;
    bool pcd;
    bool pat;
    bool global;
    u64 raw_entry;
    int level;               /* 1 = PT (4 KiB), 2 = PD (2 MiB), 3 = PDPT (1 GiB) */
};

struct vmm_cpu_info {
    u32 phys_addr_bits;
    u32 virt_addr_bits;
    bool nx_supported;
    bool nx_enabled;
    bool la57_active;
};

/* VMM Public API */
int vmm_init(u64 hhdm_offset);
void vmm_get_cpu_info(struct vmm_cpu_info *out_info);
bool vmm_is_canonical(virt_addr_t va);

/* Read-only active address space inspection */
int vmm_walk_active(virt_addr_t va, struct vmm_mapping *out);

/* Inactive address space manipulation */
int vmm_create_space(phys_addr_t *out_pml4);
int vmm_map_page(phys_addr_t pml4_phys, virt_addr_t va, phys_addr_t pa, u32 flags);
int vmm_map_range(phys_addr_t pml4_phys, virt_addr_t va, phys_addr_t pa, size_t size, u32 flags);
int vmm_query(phys_addr_t pml4_phys, virt_addr_t va, struct vmm_mapping *out);
int vmm_unmap_page(phys_addr_t pml4_phys, virt_addr_t va);
int vmm_destroy_space(phys_addr_t pml4_phys);

/* Process Address Space Management */
phys_addr_t vmm_get_kernel_pml4(void);
int vmm_create_process_space(phys_addr_t *out_pml4);
int vmm_destroy_process_space(phys_addr_t pml4_phys);

/* Construction of verified kernel-owned address space */
int vmm_build_kernel_space(struct limine_memmap_response *mmap,
                           struct limine_kernel_address_response *kaddr,
                           struct limine_framebuffer_response *fb_resp,
                           phys_addr_t *out_pml4,
                           virt_addr_t *out_stack_top,
                           virt_addr_t *out_guard_page);

/* Assembly activation transition */
void vmm_activate_space_and_switch_stack(phys_addr_t new_cr3,
                                         virt_addr_t new_rsp,
                                         void (*continuation)(void));

/* Fault injection hook for rollback verification */
void vmm_set_fault_injection(int countdown);

/* In-kernel VMM test suite */
bool vmm_self_test(u64 hhdm_offset);

#endif /* ASTRA_KERNEL_VMM_H */
