#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/kprintf.h>
#include <kernel/arch/x86_64/cpu.h>

static u64 g_hhdm_offset = 0;
static struct vmm_cpu_info g_cpu_info;
static bool g_vmm_initialized = false;
static int g_fault_countdown = -1;

static void *kmemset(void *dest, int val, size_t len) {
    u8 *d = (u8 *)dest;
    for (size_t i = 0; i < len; i++) {
        d[i] = (u8)val;
    }
    return dest;
}

void vmm_get_cpu_info(struct vmm_cpu_info *out_info) {
    if (out_info) {
        *out_info = g_cpu_info;
    }
}

bool vmm_is_canonical(virt_addr_t va) {
    /* For 48-bit canonical addressing, bits 47..63 must be identical */
    u64 upper = (u64)va >> 47;
    return (upper == 0) || (upper == 0x1FFFFULL);
}

void vmm_set_fault_injection(int countdown) {
    g_fault_countdown = countdown;
}

static int vmm_alloc_page_checked(phys_addr_t *out_paddr) {
    if (g_fault_countdown == 0) {
        return PMM_ERR_NOMEM;
    }
    if (g_fault_countdown > 0) {
        g_fault_countdown--;
    }
    return pmm_alloc_page(out_paddr);
}

int vmm_init(u64 hhdm_offset) {
    g_hhdm_offset = hhdm_offset;
    kmemset(&g_cpu_info, 0, sizeof(g_cpu_info));

    /* 1. Detect active paging mode using CR4.LA57 */
    u64 cr4 = read_cr4();
    if (cr4 & (1ULL << 12)) {
        g_cpu_info.la57_active = true;
        kprintf("[VMM] ERROR: 5-level paging (CR4.LA57) is active! AstraOS currently supports 4-level paging only.\n");
        return VMM_ERR_UNSUPPORTED;
    }
    g_cpu_info.la57_active = false;

    /* 2. Check physical & linear address width via CPUID */
    u32 max_ext = 0;
    cpuid(0x80000000, 0, &max_ext, NULL, NULL, NULL);

    if (max_ext >= 0x80000008) {
        u32 eax = 0;
        cpuid(0x80000008, 0, &eax, NULL, NULL, NULL);
        g_cpu_info.phys_addr_bits = eax & 0xFF;
        g_cpu_info.virt_addr_bits = (eax >> 8) & 0xFF;
    } else {
        g_cpu_info.phys_addr_bits = 36;
        g_cpu_info.virt_addr_bits = 48;
    }

    /* 3. Check and enable NX (No-Execute) support */
    if (max_ext >= 0x80000001) {
        u32 edx = 0;
        cpuid(0x80000001, 0, NULL, NULL, NULL, &edx);
        g_cpu_info.nx_supported = (edx & (1U << 20)) != 0;
    }

    if (g_cpu_info.nx_supported) {
        u64 efer = read_msr(0xC0000080);
        if (!(efer & (1ULL << 11))) {
            /* Enable IA32_EFER.NXE */
            write_msr(0xC0000080, efer | (1ULL << 11));
            efer = read_msr(0xC0000080);
        }
        g_cpu_info.nx_enabled = (efer & (1ULL << 11)) != 0;
    }

    kprintf("[VMM] Paging Architecture: 4-Level Paging (x86-64)\n");
    kprintf("[VMM] Address Widths:      %u-bit physical, %u-bit virtual\n",
            g_cpu_info.phys_addr_bits, g_cpu_info.virt_addr_bits);
    kprintf("[VMM] Execute-Disable:     %s (EFER.NXE %s)\n",
            g_cpu_info.nx_supported ? "Supported" : "Unsupported",
            g_cpu_info.nx_enabled ? "Enabled" : "Disabled");

    g_vmm_initialized = true;
    return VMM_SUCCESS;
}

int vmm_query(phys_addr_t pml4_phys, virt_addr_t va, struct vmm_mapping *out) {
    if (!out) {
        return VMM_ERR_INVALID_ARG;
    }

    kmemset(out, 0, sizeof(*out));

    if (!vmm_is_canonical(va)) {
        return VMM_ERR_NONCANONICAL;
    }

    bool eff_writable = true;
    bool eff_user = true;
    bool eff_nx = false;

    /* PML4 Level */
    u64 pml4_idx = (va >> 39) & 0x1FF;
    u64 *pml4 = (u64 *)(pml4_phys + g_hhdm_offset);
    u64 pml4e = pml4[pml4_idx];

    if (!(pml4e & PTE_PRESENT)) {
        out->present = false;
        return VMM_SUCCESS;
    }

    eff_writable = eff_writable && ((pml4e & PTE_WRITABLE) != 0);
    eff_user = eff_user && ((pml4e & PTE_USER) != 0);
    eff_nx = eff_nx || ((pml4e & PTE_NX) != 0);

    /* PDPT Level */
    phys_addr_t pdpt_phys = pml4e & PTE_ADDR_MASK;
    u64 pdpt_idx = (va >> 30) & 0x1FF;
    u64 *pdpt = (u64 *)(pdpt_phys + g_hhdm_offset);
    u64 pdpte = pdpt[pdpt_idx];

    if (!(pdpte & PTE_PRESENT)) {
        out->present = false;
        return VMM_SUCCESS;
    }

    eff_writable = eff_writable && ((pdpte & PTE_WRITABLE) != 0);
    eff_user = eff_user && ((pdpte & PTE_USER) != 0);
    eff_nx = eff_nx || ((pdpte & PTE_NX) != 0);

    /* Check 1 GiB Huge Page */
    if (pdpte & PTE_HUGE) {
        out->present = true;
        out->page_size = 1024ULL * 1024ULL * 1024ULL;
        out->paddr = (pdpte & PTE_1GIB_ADDR_MASK) | (va & 0x3FFFFFFFULL);
        out->level = 3;
        out->writable = eff_writable;
        out->user = eff_user;
        out->no_exec = eff_nx;
        out->raw_entry = pdpte;
        out->pwt = (pdpte & PTE_PWT) != 0;
        out->pcd = (pdpte & PTE_PCD) != 0;
        out->pat = (pdpte & (1ULL << 12)) != 0;
        out->global = (pdpte & PTE_GLOBAL) != 0;
        out->flags = VMM_FLAG_READ | (eff_writable ? VMM_FLAG_WRITE : 0) |
                     (eff_user ? VMM_FLAG_USER : 0) | (eff_nx ? VMM_FLAG_NO_EXEC : 0) |
                     (out->pwt ? VMM_FLAG_PWT : 0) | (out->pcd ? VMM_FLAG_PCD : 0) |
                     (out->pat ? VMM_FLAG_PAT : 0) | (out->global ? VMM_FLAG_GLOBAL : 0);
        return VMM_SUCCESS;
    }

    /* PD Level */
    phys_addr_t pd_phys = pdpte & PTE_ADDR_MASK;
    u64 pd_idx = (va >> 21) & 0x1FF;
    u64 *pd = (u64 *)(pd_phys + g_hhdm_offset);
    u64 pde = pd[pd_idx];

    if (!(pde & PTE_PRESENT)) {
        out->present = false;
        return VMM_SUCCESS;
    }

    eff_writable = eff_writable && ((pde & PTE_WRITABLE) != 0);
    eff_user = eff_user && ((pde & PTE_USER) != 0);
    eff_nx = eff_nx || ((pde & PTE_NX) != 0);

    /* Check 2 MiB Huge Page */
    if (pde & PTE_HUGE) {
        out->present = true;
        out->page_size = 2ULL * 1024ULL * 1024ULL;
        out->paddr = (pde & PTE_2MIB_ADDR_MASK) | (va & 0x1FFFFFULL);
        out->level = 2;
        out->writable = eff_writable;
        out->user = eff_user;
        out->no_exec = eff_nx;
        out->raw_entry = pde;
        out->pwt = (pde & PTE_PWT) != 0;
        out->pcd = (pde & PTE_PCD) != 0;
        out->pat = (pde & (1ULL << 12)) != 0;
        out->global = (pde & PTE_GLOBAL) != 0;
        out->flags = VMM_FLAG_READ | (eff_writable ? VMM_FLAG_WRITE : 0) |
                     (eff_user ? VMM_FLAG_USER : 0) | (eff_nx ? VMM_FLAG_NO_EXEC : 0) |
                     (out->pwt ? VMM_FLAG_PWT : 0) | (out->pcd ? VMM_FLAG_PCD : 0) |
                     (out->pat ? VMM_FLAG_PAT : 0) | (out->global ? VMM_FLAG_GLOBAL : 0);
        return VMM_SUCCESS;
    }

    /* PT Level (4 KiB Page) */
    phys_addr_t pt_phys = pde & PTE_ADDR_MASK;
    u64 pt_idx = (va >> 12) & 0x1FF;
    u64 *pt = (u64 *)(pt_phys + g_hhdm_offset);
    u64 pte = pt[pt_idx];

    if (!(pte & PTE_PRESENT)) {
        out->present = false;
        return VMM_SUCCESS;
    }

    eff_writable = eff_writable && ((pte & PTE_WRITABLE) != 0);
    eff_user = eff_user && ((pte & PTE_USER) != 0);
    eff_nx = eff_nx || ((pte & PTE_NX) != 0);

    out->present = true;
    out->page_size = 4096ULL;
    out->paddr = (pte & PTE_ADDR_MASK) | (va & 0xFFFULL);
    out->level = 1;
    out->writable = eff_writable;
    out->user = eff_user;
    out->no_exec = eff_nx;
    out->raw_entry = pte;
    out->pwt = (pte & PTE_PWT) != 0;
    out->pcd = (pte & PTE_PCD) != 0;
    out->pat = (pte & (1ULL << 7)) != 0;
    out->global = (pte & PTE_GLOBAL) != 0;
    out->flags = VMM_FLAG_READ | (eff_writable ? VMM_FLAG_WRITE : 0) |
                 (eff_user ? VMM_FLAG_USER : 0) | (eff_nx ? VMM_FLAG_NO_EXEC : 0) |
                 (out->pwt ? VMM_FLAG_PWT : 0) | (out->pcd ? VMM_FLAG_PCD : 0) |
                 (out->pat ? VMM_FLAG_PAT : 0) | (out->global ? VMM_FLAG_GLOBAL : 0);

    return VMM_SUCCESS;
}

int vmm_walk_active(virt_addr_t va, struct vmm_mapping *out) {
    if (!g_vmm_initialized) {
        return VMM_ERR_UNSUPPORTED;
    }
    if (!vmm_is_canonical(va)) {
        return VMM_ERR_NONCANONICAL;
    }
    u64 cr3 = read_cr3();
    phys_addr_t pml4_phys = cr3 & CR3_ADDR_MASK;
    return vmm_query(pml4_phys, va, out);
}

int vmm_create_space(phys_addr_t *out_pml4) {
    if (!g_vmm_initialized) {
        return VMM_ERR_UNSUPPORTED;
    }
    if (!out_pml4) {
        return VMM_ERR_INVALID_ARG;
    }
    phys_addr_t pml4_phys = 0;
    int res = vmm_alloc_page_checked(&pml4_phys);
    if (res != PMM_SUCCESS) {
        return VMM_ERR_NOMEM;
    }
    /* Zero new PML4 page table completely */
    kmemset((void *)(pml4_phys + g_hhdm_offset), 0, PAGE_SIZE);
    *out_pml4 = pml4_phys;
    return VMM_SUCCESS;
}

int vmm_map_page(phys_addr_t pml4_phys, virt_addr_t va, phys_addr_t pa, u32 flags) {
    if ((va & 0xFFF) != 0 || (pa & 0xFFF) != 0) {
        return VMM_ERR_UNALIGNED;
    }
    if (!vmm_is_canonical(va)) {
        return VMM_ERR_NONCANONICAL;
    }
    if (g_cpu_info.phys_addr_bits > 0 && pa >= (1ULL << g_cpu_info.phys_addr_bits)) {
        return VMM_ERR_INVALID_ARG;
    }

    /* Track newly allocated tables for rollback on error */
    struct {
        u64 *parent_entry;
        phys_addr_t table_phys;
    } rollbacks[3];
    int rollback_count = 0;

    u64 pml4_idx = (va >> 39) & 0x1FF;
    u64 *pml4 = (u64 *)(pml4_phys + g_hhdm_offset);
    phys_addr_t pdpt_phys = 0;

    u64 parent_flags = PTE_PRESENT | PTE_WRITABLE | ((flags & VMM_FLAG_USER) ? PTE_USER : 0);

    /* 1. Resolve or allocate PDPT */
    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        int alloc_res = vmm_alloc_page_checked(&pdpt_phys);
        if (alloc_res != PMM_SUCCESS) {
            return VMM_ERR_NOMEM;
        }
        kmemset((void *)(pdpt_phys + g_hhdm_offset), 0, PAGE_SIZE);
        pml4[pml4_idx] = pdpt_phys | parent_flags;
        rollbacks[rollback_count].parent_entry = &pml4[pml4_idx];
        rollbacks[rollback_count].table_phys = pdpt_phys;
        rollback_count++;
    } else {
        if (flags & VMM_FLAG_USER) {
            pml4[pml4_idx] |= PTE_USER;
        }
        pdpt_phys = pml4[pml4_idx] & PTE_ADDR_MASK;
    }

    /* 2. Resolve or allocate PD */
    u64 pdpt_idx = (va >> 30) & 0x1FF;
    u64 *pdpt = (u64 *)(pdpt_phys + g_hhdm_offset);
    phys_addr_t pd_phys = 0;

    if (pdpt[pdpt_idx] & PTE_PRESENT) {
        if (pdpt[pdpt_idx] & PTE_HUGE) {
            /* Conflict with 1 GiB huge page! Roll back and return explicit error */
            for (int i = 0; i < rollback_count; i++) {
                *(rollbacks[i].parent_entry) = 0;
                pmm_free_page(rollbacks[i].table_phys);
            }
            return VMM_ERR_HUGE_CONFLICT;
        }
        if (flags & VMM_FLAG_USER) {
            pdpt[pdpt_idx] |= PTE_USER;
        }
        pd_phys = pdpt[pdpt_idx] & PTE_ADDR_MASK;
    } else {
        int alloc_res = vmm_alloc_page_checked(&pd_phys);
        if (alloc_res != PMM_SUCCESS) {
            for (int i = 0; i < rollback_count; i++) {
                *(rollbacks[i].parent_entry) = 0;
                pmm_free_page(rollbacks[i].table_phys);
            }
            return VMM_ERR_NOMEM;
        }
        kmemset((void *)(pd_phys + g_hhdm_offset), 0, PAGE_SIZE);
        pdpt[pdpt_idx] = pd_phys | parent_flags;
        rollbacks[rollback_count].parent_entry = &pdpt[pdpt_idx];
        rollbacks[rollback_count].table_phys = pd_phys;
        rollback_count++;
    }

    /* 3. Resolve or allocate PT */
    u64 pd_idx = (va >> 21) & 0x1FF;
    u64 *pd = (u64 *)(pd_phys + g_hhdm_offset);
    phys_addr_t pt_phys = 0;

    if (pd[pd_idx] & PTE_PRESENT) {
        if (pd[pd_idx] & PTE_HUGE) {
            /* Conflict with 2 MiB huge page! Roll back and return explicit error */
            for (int i = 0; i < rollback_count; i++) {
                *(rollbacks[i].parent_entry) = 0;
                pmm_free_page(rollbacks[i].table_phys);
            }
            return VMM_ERR_HUGE_CONFLICT;
        }
        if (flags & VMM_FLAG_USER) {
            pd[pd_idx] |= PTE_USER;
        }
        pt_phys = pd[pd_idx] & PTE_ADDR_MASK;
    } else {
        int alloc_res = vmm_alloc_page_checked(&pt_phys);
        if (alloc_res != PMM_SUCCESS) {
            for (int i = 0; i < rollback_count; i++) {
                *(rollbacks[i].parent_entry) = 0;
                pmm_free_page(rollbacks[i].table_phys);
            }
            return VMM_ERR_NOMEM;
        }
        kmemset((void *)(pt_phys + g_hhdm_offset), 0, PAGE_SIZE);
        pd[pd_idx] = pt_phys | parent_flags;
        rollbacks[rollback_count].parent_entry = &pd[pd_idx];
        rollbacks[rollback_count].table_phys = pt_phys;
        rollback_count++;
    }

    /* 4. Write Leaf PT Entry */
    u64 pt_idx = (va >> 12) & 0x1FF;
    u64 *pt = (u64 *)(pt_phys + g_hhdm_offset);

    if (pt[pt_idx] & PTE_PRESENT) {
        /* Reject duplicate mappings */
        for (int i = 0; i < rollback_count; i++) {
            *(rollbacks[i].parent_entry) = 0;
            pmm_free_page(rollbacks[i].table_phys);
        }
        return VMM_ERR_ALREADY_MAPPED;
    }

    u64 pte = pa | PTE_PRESENT;
    if (flags & VMM_FLAG_WRITE) {
        pte |= PTE_WRITABLE;
    }
    if (flags & VMM_FLAG_USER) {
        pte |= PTE_USER;
    }
    if (flags & VMM_FLAG_PWT) {
        pte |= PTE_PWT;
    }
    if (flags & VMM_FLAG_PCD) {
        pte |= PTE_PCD;
    }
    if (flags & VMM_FLAG_PAT) {
        pte |= (1ULL << 7);
    }
    if (flags & VMM_FLAG_GLOBAL) {
        pte |= PTE_GLOBAL;
    }
    if ((flags & VMM_FLAG_NO_EXEC) && g_cpu_info.nx_supported && g_cpu_info.nx_enabled) {
        pte |= PTE_NX;
    }

    pt[pt_idx] = pte;
    return VMM_SUCCESS;
}

int vmm_unmap_page(phys_addr_t pml4_phys, virt_addr_t va) {
    if ((va & 0xFFF) != 0) {
        return VMM_ERR_UNALIGNED;
    }
    if (!vmm_is_canonical(va)) {
        return VMM_ERR_NONCANONICAL;
    }

    u64 pml4_idx = (va >> 39) & 0x1FF;
    u64 *pml4 = (u64 *)(pml4_phys + g_hhdm_offset);
    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        return VMM_ERR_NOT_MAPPED;
    }

    phys_addr_t pdpt_phys = pml4[pml4_idx] & PTE_ADDR_MASK;
    u64 pdpt_idx = (va >> 30) & 0x1FF;
    u64 *pdpt = (u64 *)(pdpt_phys + g_hhdm_offset);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        return VMM_ERR_NOT_MAPPED;
    }
    if (pdpt[pdpt_idx] & PTE_HUGE) {
        return VMM_ERR_HUGE_CONFLICT;
    }

    phys_addr_t pd_phys = pdpt[pdpt_idx] & PTE_ADDR_MASK;
    u64 pd_idx = (va >> 21) & 0x1FF;
    u64 *pd = (u64 *)(pd_phys + g_hhdm_offset);
    if (!(pd[pd_idx] & PTE_PRESENT)) {
        return VMM_ERR_NOT_MAPPED;
    }
    if (pd[pd_idx] & PTE_HUGE) {
        return VMM_ERR_HUGE_CONFLICT;
    }

    phys_addr_t pt_phys = pd[pd_idx] & PTE_ADDR_MASK;
    u64 pt_idx = (va >> 12) & 0x1FF;
    u64 *pt = (u64 *)(pt_phys + g_hhdm_offset);
    if (!(pt[pt_idx] & PTE_PRESENT)) {
        return VMM_ERR_NOT_MAPPED;
    }

    /* Clear leaf entry (does NOT free the mapped data page) */
    pt[pt_idx] = 0;

    /* Clean up empty owned tables */
    bool pt_empty = true;
    for (int i = 0; i < 512; i++) {
        if (pt[i] != 0) {
            pt_empty = false;
            break;
        }
    }
    if (pt_empty) {
        pmm_free_page(pt_phys);
        pd[pd_idx] = 0;

        bool pd_empty = true;
        for (int i = 0; i < 512; i++) {
            if (pd[i] != 0) {
                pd_empty = false;
                break;
            }
        }
        if (pd_empty) {
            pmm_free_page(pd_phys);
            pdpt[pdpt_idx] = 0;

            bool pdpt_empty = true;
            for (int i = 0; i < 512; i++) {
                if (pdpt[i] != 0) {
                    pdpt_empty = false;
                    break;
                }
            }
            if (pdpt_empty) {
                pmm_free_page(pdpt_phys);
                pml4[pml4_idx] = 0;
            }
        }
    }

    return VMM_SUCCESS;
}

int vmm_destroy_space(phys_addr_t pml4_phys) {
    if (pml4_phys == 0) {
        return VMM_ERR_INVALID_ARG;
    }

    /* Never destroy the active page table! */
    if (pml4_phys == (read_cr3() & CR3_ADDR_MASK)) {
        kprintf("[VMM] ERROR: Attempted to destroy active CR3 page table!\n");
        return VMM_ERR_INVALID_ARG;
    }

    u64 *pml4 = (u64 *)(pml4_phys + g_hhdm_offset);

    for (int pml4_i = 0; pml4_i < 512; pml4_i++) {
        if (pml4[pml4_i] & PTE_PRESENT) {
            phys_addr_t pdpt_phys = pml4[pml4_i] & PTE_ADDR_MASK;
            u64 *pdpt = (u64 *)(pdpt_phys + g_hhdm_offset);

            for (int pdpt_i = 0; pdpt_i < 512; pdpt_i++) {
                if ((pdpt[pdpt_i] & PTE_PRESENT) && !(pdpt[pdpt_i] & PTE_HUGE)) {
                    phys_addr_t pd_phys = pdpt[pdpt_i] & PTE_ADDR_MASK;
                    u64 *pd = (u64 *)(pd_phys + g_hhdm_offset);

                    for (int pd_i = 0; pd_i < 512; pd_i++) {
                        if ((pd[pd_i] & PTE_PRESENT) && !(pd[pd_i] & PTE_HUGE)) {
                            phys_addr_t pt_phys = pd[pd_i] & PTE_ADDR_MASK;
                            pmm_free_page(pt_phys); /* Frees owned PT page */
                        }
                    }
                    pmm_free_page(pd_phys); /* Frees owned PD page */
                }
            }
            pmm_free_page(pdpt_phys); /* Frees owned PDPT page */
        }
    }

    pmm_free_page(pml4_phys); /* Frees owned PML4 root page */
    return VMM_SUCCESS;
}

static phys_addr_t g_kernel_pml4 = 0;

phys_addr_t vmm_get_kernel_pml4(void) {
    if (g_kernel_pml4 != 0) {
        return g_kernel_pml4;
    }
    return read_cr3() & CR3_ADDR_MASK;
}

int vmm_create_process_space(phys_addr_t *out_pml4) {
    if (!out_pml4) {
        return VMM_ERR_INVALID_ARG;
    }

    phys_addr_t new_pml4_pa = 0;
    int res = vmm_alloc_page_checked(&new_pml4_pa);
    if (res != PMM_SUCCESS) {
        return VMM_ERR_NOMEM;
    }

    u64 *new_pml4 = (u64 *)(new_pml4_pa + g_hhdm_offset);
    phys_addr_t kernel_pml4_pa = vmm_get_kernel_pml4();
    u64 *kernel_pml4 = (u64 *)(kernel_pml4_pa + g_hhdm_offset);

    /* 1. Zero lower canonical half (user-space, PML4 entries 0..255) */
    kmemset(new_pml4, 0, 256 * sizeof(u64));

    /* 2. Clone higher canonical half (kernel-space, PML4 entries 256..511) */
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = kernel_pml4[i];
    }

    *out_pml4 = new_pml4_pa;
    return VMM_SUCCESS;
}

int vmm_destroy_process_space(phys_addr_t pml4_phys) {
    if (pml4_phys == 0 || pml4_phys == vmm_get_kernel_pml4()) {
        return VMM_ERR_INVALID_ARG;
    }

    /* Never destroy the currently active page table */
    if (pml4_phys == (read_cr3() & CR3_ADDR_MASK)) {
        write_cr3(vmm_get_kernel_pml4());
    }

    u64 *pml4 = (u64 *)(pml4_phys + g_hhdm_offset);

    /* Destroy only user-space PML4 entries (0..255) */
    for (int pml4_i = 0; pml4_i < 256; pml4_i++) {
        if (pml4[pml4_i] & PTE_PRESENT) {
            phys_addr_t pdpt_phys = pml4[pml4_i] & PTE_ADDR_MASK;
            u64 *pdpt = (u64 *)(pdpt_phys + g_hhdm_offset);

            for (int pdpt_i = 0; pdpt_i < 512; pdpt_i++) {
                if ((pdpt[pdpt_i] & PTE_PRESENT) && !(pdpt[pdpt_i] & PTE_HUGE)) {
                    phys_addr_t pd_phys = pdpt[pdpt_i] & PTE_ADDR_MASK;
                    u64 *pd = (u64 *)(pd_phys + g_hhdm_offset);

                    for (int pd_i = 0; pd_i < 512; pd_i++) {
                        if ((pd[pd_i] & PTE_PRESENT) && !(pd[pd_i] & PTE_HUGE)) {
                            phys_addr_t pt_phys = pd[pd_i] & PTE_ADDR_MASK;
                            pmm_free_page(pt_phys);
                        }
                    }
                    pmm_free_page(pd_phys);
                }
            }
            pmm_free_page(pdpt_phys);
            pml4[pml4_i] = 0;
        }
    }

    /* Free the process PML4 root page itself */
    pmm_free_page(pml4_phys);
    return VMM_SUCCESS;
}

/* --- In-Kernel VMM Test Suite --- */

bool vmm_self_test(u64 hhdm_offset) {
    kprintf("[VMM] Starting Virtual Memory Manager Self-Test...\n");

    /* 1. Query known active mappings */
    struct vmm_mapping active_map;

    /* 1a. Kernel code (&vmm_init) */
    virt_addr_t code_va = (virt_addr_t)&vmm_init;
    if (vmm_walk_active(code_va, &active_map) != VMM_SUCCESS || !active_map.present) {
        kprintf("[VMM] FAIL: Kernel code mapping not present for 0x%016llx\n", (unsigned long long)code_va);
        return false;
    }
    kprintf("[VMM] Active Kernel Code:  0x%016llx -> PA 0x%016llx (%llu KiB, NX=%d)\n",
            (unsigned long long)code_va, (unsigned long long)active_map.paddr,
            (unsigned long long)(active_map.page_size / 1024), active_map.no_exec);

    /* 1b. Kernel stack */
    virt_addr_t stack_va;
    __asm__ volatile("mov %%rsp, %0" : "=r"(stack_va));
    if (vmm_walk_active(stack_va, &active_map) != VMM_SUCCESS || !active_map.present || !active_map.writable) {
        kprintf("[VMM] FAIL: Stack mapping missing or not writable for 0x%016llx\n", (unsigned long long)stack_va);
        return false;
    }
    kprintf("[VMM] Active Kernel Stack: 0x%016llx -> PA 0x%016llx (%llu KiB, RW=%d)\n",
            (unsigned long long)stack_va, (unsigned long long)active_map.paddr,
            (unsigned long long)(active_map.page_size / 1024), active_map.writable);

    /* 1c. PMM page accessed through HHDM */
    phys_addr_t test_paddr;
    if (pmm_alloc_page(&test_paddr) != PMM_SUCCESS) {
        kprintf("[VMM] FAIL: Cannot allocate PMM test page\n");
        return false;
    }
    virt_addr_t hhdm_va = test_paddr + hhdm_offset;
    if (vmm_walk_active(hhdm_va, &active_map) != VMM_SUCCESS || !active_map.present || active_map.paddr != test_paddr) {
        kprintf("[VMM] FAIL: HHDM translation mismatch for 0x%016llx\n", (unsigned long long)hhdm_va);
        pmm_free_page(test_paddr);
        return false;
    }
    kprintf("[VMM] Active HHDM Page:    0x%016llx -> PA 0x%016llx (%llu KiB)\n",
            (unsigned long long)hhdm_va, (unsigned long long)active_map.paddr,
            (unsigned long long)(active_map.page_size / 1024));
    pmm_free_page(test_paddr);

    /* 1d. Unmapped virtual address check */
    virt_addr_t unmapped_va = 0x0000000000000000ULL;
    if (vmm_walk_active(unmapped_va, &active_map) != VMM_SUCCESS || active_map.present) {
        kprintf("[VMM] FAIL: Address zero should be unmapped in active table\n");
        return false;
    }

    /* 2. Create inactive address space and test mappings */
    struct pmm_stats pmm_start;
    pmm_get_stats(&pmm_start);

    phys_addr_t test_pml4 = 0;
    if (vmm_create_space(&test_pml4) != VMM_SUCCESS) {
        kprintf("[VMM] FAIL: Failed creating inactive address space\n");
        return false;
    }

    /* Allocate 3 data pages to map */
    phys_addr_t pa_a = 0, pa_b = 0, pa_c = 0;
    if (pmm_alloc_page(&pa_a) != PMM_SUCCESS ||
        pmm_alloc_page(&pa_b) != PMM_SUCCESS ||
        pmm_alloc_page(&pa_c) != PMM_SUCCESS) {
        kprintf("[VMM] FAIL: Failed allocating data pages\n");
        return false;
    }

    /* Map page A at canonical user VA: 0x00400000 (4 MiB) */
    virt_addr_t va_a = 0x0000000000400000ULL;
    u32 flags_a = VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXEC;
    if (vmm_map_page(test_pml4, va_a, pa_a, flags_a) != VMM_SUCCESS) {
        kprintf("[VMM] FAIL: Failed mapping page A\n");
        return false;
    }

    /* Map page B across PT boundary (PT covers 2 MiB): 0x00600000 (6 MiB) */
    virt_addr_t va_b = 0x0000000000600000ULL;
    u32 flags_b = VMM_FLAG_READ;
    if (vmm_map_page(test_pml4, va_b, pa_b, flags_b) != VMM_SUCCESS) {
        kprintf("[VMM] FAIL: Failed mapping page B across PT boundary\n");
        return false;
    }

    /* Map page C across PD boundary (PD covers 1 GiB): 0x0000000100000000ULL (4 GiB) */
    virt_addr_t va_c = 0x0000000100000000ULL;
    u32 flags_c = VMM_FLAG_READ | VMM_FLAG_WRITE;
    if (vmm_map_page(test_pml4, va_c, pa_c, flags_c) != VMM_SUCCESS) {
        kprintf("[VMM] FAIL: Failed mapping page C across PD boundary\n");
        return false;
    }

    /* Verify translations and permissions with vmm_query */
    struct vmm_mapping q_map;
    if (vmm_query(test_pml4, va_a, &q_map) != VMM_SUCCESS || !q_map.present ||
        q_map.paddr != pa_a || q_map.page_size != 4096 ||
        !q_map.writable || !q_map.user || !q_map.no_exec) {
        kprintf("[VMM] FAIL: Query verification failed for page A\n");
        return false;
    }

    /* Test offset within page */
    if (vmm_query(test_pml4, va_a + 0x123, &q_map) != VMM_SUCCESS || !q_map.present ||
        q_map.paddr != (pa_a + 0x123)) {
        kprintf("[VMM] FAIL: Page offset translation failed\n");
        return false;
    }

    if (vmm_query(test_pml4, va_b, &q_map) != VMM_SUCCESS || !q_map.present ||
        q_map.paddr != pa_b || q_map.writable || q_map.user) {
        kprintf("[VMM] FAIL: Query verification failed for page B\n");
        return false;
    }

    if (vmm_query(test_pml4, va_c, &q_map) != VMM_SUCCESS || !q_map.present ||
        q_map.paddr != pa_c || !q_map.writable || q_map.user) {
        kprintf("[VMM] FAIL: Query verification failed for page C\n");
        return false;
    }

    /* 3. Reject unaligned, noncanonical, duplicate, and unsupported operations */
    if (vmm_map_page(test_pml4, va_a + 1, pa_a, flags_a) != VMM_ERR_UNALIGNED) {
        kprintf("[VMM] FAIL: Unaligned VA not rejected\n");
        return false;
    }
    if (vmm_map_page(test_pml4, 0x00800000ULL, pa_a + 1, flags_a) != VMM_ERR_UNALIGNED) {
        kprintf("[VMM] FAIL: Unaligned PA not rejected\n");
        return false;
    }
    if (vmm_map_page(test_pml4, 0x0008000000000000ULL, pa_a, flags_a) != VMM_ERR_NONCANONICAL) {
        kprintf("[VMM] FAIL: Noncanonical VA not rejected\n");
        return false;
    }
    if (vmm_map_page(test_pml4, va_a, pa_a, flags_a) != VMM_ERR_ALREADY_MAPPED) {
        kprintf("[VMM] FAIL: Duplicate mapping not rejected\n");
        return false;
    }

    /* 4. Test controlled synthetic huge-page translation fixture */
    phys_addr_t fixture_pml4 = 0, fixture_pdpt = 0, fixture_pd = 0;
    if (pmm_alloc_page(&fixture_pml4) != PMM_SUCCESS ||
        pmm_alloc_page(&fixture_pdpt) != PMM_SUCCESS ||
        pmm_alloc_page(&fixture_pd) != PMM_SUCCESS) {
        kprintf("[VMM] FAIL: Allocating fixture tables failed\n");
        return false;
    }
    kmemset((void *)(fixture_pml4 + hhdm_offset), 0, PAGE_SIZE);
    kmemset((void *)(fixture_pdpt + hhdm_offset), 0, PAGE_SIZE);
    kmemset((void *)(fixture_pd + hhdm_offset), 0, PAGE_SIZE);

    u64 *f_pml4 = (u64 *)(fixture_pml4 + hhdm_offset);
    u64 *f_pdpt = (u64 *)(fixture_pdpt + hhdm_offset);
    u64 *f_pd = (u64 *)(fixture_pd + hhdm_offset);

    f_pml4[0] = fixture_pdpt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    f_pdpt[0] = fixture_pd | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    /* 2 MiB huge page at VA 0x200000 */
    f_pd[1] = 0x20000000ULL | PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_HUGE;
    /* 1 GiB huge page at VA 0x40000000 */
    f_pdpt[1] = 0x40000000ULL | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE;

    /* Query 2 MiB page in fixture */
    if (vmm_query(fixture_pml4, 0x00200000ULL, &q_map) != VMM_SUCCESS || !q_map.present ||
        q_map.page_size != (2ULL * 1024ULL * 1024ULL) || q_map.paddr != 0x20000000ULL || q_map.level != 2) {
        kprintf("[VMM] FAIL: Fixture 2 MiB huge page query failed\n");
        return false;
    }

    /* Query 1 GiB page in fixture */
    if (vmm_query(fixture_pml4, 0x40000000ULL, &q_map) != VMM_SUCCESS || !q_map.present ||
        q_map.page_size != (1024ULL * 1024ULL * 1024ULL) || q_map.paddr != 0x40000000ULL || q_map.level != 3) {
        kprintf("[VMM] FAIL: Fixture 1 GiB huge page query failed\n");
        return false;
    }

    /* Test mapping conflict on huge page without silently splitting */
    int conflict_res = vmm_map_page(fixture_pml4, 0x00201000ULL, pa_a, VMM_FLAG_READ);
    if (conflict_res != VMM_ERR_HUGE_CONFLICT) {
        kprintf("[VMM] FAIL: Mapping over 2 MiB page did not return VMM_ERR_HUGE_CONFLICT (got %d)\n", conflict_res);
        return false;
    }

    /* Clean up fixture pages */
    pmm_free_page(fixture_pd);
    pmm_free_page(fixture_pdpt);
    pmm_free_page(fixture_pml4);

    /* 5. Test allocation-failure rollback using bounded fault injection */
    phys_addr_t rollback_pml4 = 0;
    if (vmm_create_space(&rollback_pml4) != VMM_SUCCESS) {
        kprintf("[VMM] FAIL: Failed creating rollback space\n");
        return false;
    }

    /* Record PMM state before mapping attempt */
    struct pmm_stats pmm_before_fail;
    pmm_get_stats(&pmm_before_fail);

    /* Set fault injection: allow 1 allocation (PDPT succeeds), then fail on PD */
    vmm_set_fault_injection(1);
    int fail_res = vmm_map_page(rollback_pml4, 0x0000000200000000ULL, pa_a, VMM_FLAG_READ);
    vmm_set_fault_injection(-1); /* Restore fault injection */

    if (fail_res != VMM_ERR_NOMEM) {
        kprintf("[VMM] FAIL: Fault injection did not return VMM_ERR_NOMEM (got %d)\n", fail_res);
        return false;
    }

    /* Verify intermediate table was rolled back and PMM accounting returned */
    struct pmm_stats pmm_after_fail;
    pmm_get_stats(&pmm_after_fail);
    if (pmm_after_fail.free_pages != pmm_before_fail.free_pages) {
        kprintf("[VMM] FAIL: Rollback leak detected: free before=%llu, after=%llu\n",
                (unsigned long long)pmm_before_fail.free_pages,
                (unsigned long long)pmm_after_fail.free_pages);
        return false;
    }
    vmm_destroy_space(rollback_pml4);

    /* 6. Unmap and destroy test space */
    if (vmm_unmap_page(test_pml4, va_a) != VMM_SUCCESS) {
        kprintf("[VMM] FAIL: Unmapping page A failed\n");
        return false;
    }
    if (vmm_query(test_pml4, va_a, &q_map) != VMM_SUCCESS || q_map.present) {
        kprintf("[VMM] FAIL: Page A still present after unmap\n");
        return false;
    }

    /* Unmap remaining pages */
    vmm_unmap_page(test_pml4, va_b);
    vmm_unmap_page(test_pml4, va_c);

    /* Destroy space (frees owned table pages) */
    vmm_destroy_space(test_pml4);

    /* Explicitly free data pages */
    pmm_free_page(pa_a);
    pmm_free_page(pa_b);
    pmm_free_page(pa_c);

    /* 7. Confirm PMM accounting returns exactly to baseline */
    struct pmm_stats pmm_end;
    pmm_get_stats(&pmm_end);

    if (pmm_end.free_pages != pmm_start.free_pages ||
        pmm_end.allocated_pages != pmm_start.allocated_pages) {
        kprintf("[VMM] FAIL: Accounting mismatch after VMM test. Start free=%llu, end free=%llu\n",
                (unsigned long long)pmm_start.free_pages,
                (unsigned long long)pmm_end.free_pages);
        return false;
    }

    kprintf("[VMM] Self-test passed: Inactive table creation, boundary mapping, rollback, and cleanup verified.\n");
    kprintf("ASTRA_VMM_TABLES_OK\n\n");
    return true;
}

int vmm_map_range(phys_addr_t pml4_phys, virt_addr_t va, phys_addr_t pa, size_t size, u32 flags) {
    if ((va & 0xFFF) != 0 || (pa & 0xFFF) != 0 || (size & 0xFFF) != 0) {
        return VMM_ERR_UNALIGNED;
    }
    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
        int res = vmm_map_page(pml4_phys, va + offset, pa + offset, flags);
        if (res != VMM_SUCCESS) {
            return res;
        }
    }
    return VMM_SUCCESS;
}

int vmm_build_kernel_space(struct limine_memmap_response *mmap,
                           struct limine_kernel_address_response *kaddr,
                           struct limine_framebuffer_response *fb_resp,
                           phys_addr_t *out_pml4,
                           virt_addr_t *out_stack_top,
                           virt_addr_t *out_guard_page) {
    if (!mmap || !kaddr || !out_pml4 || !out_stack_top || !out_guard_page) {
        return VMM_ERR_INVALID_ARG;
    }

    /* 1. Allocate and zero a new root PML4 table */
    int res = vmm_create_space(out_pml4);
    if (res != VMM_SUCCESS) {
        return res;
    }
    phys_addr_t pml4 = *out_pml4;

    /* 2. Map Kernel ELF sections with strict W^X permissions */
    virt_addr_t k_virt_base = kaddr->virtual_base;
    phys_addr_t k_phys_base = kaddr->physical_base;

    virt_addr_t text_vstart = (virt_addr_t)__text_start;
    virt_addr_t text_vend = (virt_addr_t)__text_end;
    virt_addr_t rodata_vstart = (virt_addr_t)__rodata_start;
    virt_addr_t rodata_vend = (virt_addr_t)__rodata_end;
    virt_addr_t data_vstart = (virt_addr_t)__data_start;
    virt_addr_t bss_vend = (virt_addr_t)__bss_end;

    if ((text_vstart & 0xFFF) || (text_vend & 0xFFF) ||
        (rodata_vstart & 0xFFF) || (rodata_vend & 0xFFF) ||
        (data_vstart & 0xFFF) || (bss_vend & 0xFFF)) {
        return VMM_ERR_UNALIGNED;
    }

    kprintf("[VMM] Mapping Kernel ELF image:\n");
    kprintf("  .text:   0x%016llx .. 0x%016llx -> PA 0x%016llx (R-X)\n",
            (unsigned long long)text_vstart, (unsigned long long)text_vend,
            (unsigned long long)(k_phys_base + (text_vstart - k_virt_base)));
    kprintf("  .rodata: 0x%016llx .. 0x%016llx -> PA 0x%016llx (R--)\n",
            (unsigned long long)rodata_vstart, (unsigned long long)rodata_vend,
            (unsigned long long)(k_phys_base + (rodata_vstart - k_virt_base)));
    kprintf("  .data:   0x%016llx .. 0x%016llx -> PA 0x%016llx (RW-)\n",
            (unsigned long long)data_vstart, (unsigned long long)bss_vend,
            (unsigned long long)(k_phys_base + (data_vstart - k_virt_base)));

    /* .text: Read-Only, Executable */
    for (virt_addr_t va = text_vstart; va < text_vend; va += PAGE_SIZE) {
        phys_addr_t pa = k_phys_base + (va - k_virt_base);
        res = vmm_map_page(pml4, va, pa, VMM_FLAG_READ);
        if (res != VMM_SUCCESS) return res;
    }

    /* .rodata: Read-Only, Non-Executable */
    for (virt_addr_t va = rodata_vstart; va < rodata_vend; va += PAGE_SIZE) {
        phys_addr_t pa = k_phys_base + (va - k_virt_base);
        res = vmm_map_page(pml4, va, pa, VMM_FLAG_READ | VMM_FLAG_NO_EXEC);
        if (res != VMM_SUCCESS) return res;
    }

    /* .data & .bss: Read-Write, Non-Executable */
    for (virt_addr_t va = data_vstart; va < bss_vend; va += PAGE_SIZE) {
        phys_addr_t pa = k_phys_base + (va - k_virt_base);
        res = vmm_map_page(pml4, va, pa, VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC);
        if (res != VMM_SUCCESS) return res;
    }

    /* 3. Allocate and map dedicated kernel stack (16 KiB) with guard page */
    *out_guard_page = KERNEL_GUARD_PAGE_VA;
    *out_stack_top = KERNEL_STACK_TOP_VA;

    kprintf("[VMM] Mapping Dedicated Kernel Stack:\n");
    kprintf("  Guard Page: 0x%016llx (Unmapped)\n", (unsigned long long)KERNEL_GUARD_PAGE_VA);
    kprintf("  Stack Body: 0x%016llx .. 0x%016llx (16 KiB, RW-)\n",
            (unsigned long long)KERNEL_STACK_BOTTOM_VA, (unsigned long long)KERNEL_STACK_TOP_VA);

    for (virt_addr_t va = KERNEL_STACK_BOTTOM_VA; va < KERNEL_STACK_TOP_VA; va += PAGE_SIZE) {
        phys_addr_t stk_pa;
        res = pmm_alloc_page(&stk_pa);
        if (res != PMM_SUCCESS) return VMM_ERR_NOMEM;
        kmemset((void *)(stk_pa + g_hhdm_offset), 0, PAGE_SIZE);
        res = vmm_map_page(pml4, va, stk_pa, VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC);
        if (res != VMM_SUCCESS) return res;
    }

    /* 3b. Allocate and map dedicated Double-Fault IST1 Stack (8 KiB) with guard page */
    kprintf("[VMM] Mapping Double-Fault IST1 Stack:\n");
    kprintf("  Guard Page: 0x%016llx (Unmapped)\n", (unsigned long long)DOUBLE_FAULT_GUARD_VA);
    kprintf("  Stack Body: 0x%016llx .. 0x%016llx (8 KiB, RW-)\n",
            (unsigned long long)DOUBLE_FAULT_STACK_BOTTOM_VA, (unsigned long long)DOUBLE_FAULT_STACK_TOP_VA);

    for (virt_addr_t va = DOUBLE_FAULT_STACK_BOTTOM_VA; va < DOUBLE_FAULT_STACK_TOP_VA; va += PAGE_SIZE) {
        phys_addr_t df_pa;
        res = pmm_alloc_page(&df_pa);
        if (res != PMM_SUCCESS) return VMM_ERR_NOMEM;
        kmemset((void *)(df_pa + g_hhdm_offset), 0, PAGE_SIZE);
        res = vmm_map_page(pml4, va, df_pa, VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC);
        if (res != VMM_SUCCESS) return res;
    }

    /* 3c. Map Local APIC MMIO Page */
    u64 apic_msr = read_msr(0x1B);
    phys_addr_t apic_pa = apic_msr & 0xFFFFF000ULL;
    if (apic_pa == 0) apic_pa = APIC_DEFAULT_BASE_PA;
    res = vmm_map_page(pml4, APIC_VIRT_BASE, apic_pa,
                       VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC | VMM_FLAG_PCD | VMM_FLAG_PWT);
    if (res != VMM_SUCCESS) return res;

    /* 4. Map current (bootloader) stack page(s) to guarantee safe transition */
    u64 cur_rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(cur_rsp));
    virt_addr_t old_stk_base = cur_rsp & ~0xFFFULL;
    for (virt_addr_t va = old_stk_base - 2 * PAGE_SIZE; va <= old_stk_base + 2 * PAGE_SIZE; va += PAGE_SIZE) {
        struct vmm_mapping act;
        if (vmm_walk_active(va, &act) == VMM_SUCCESS && act.present) {
            struct vmm_mapping exist;
            if (vmm_query(pml4, va, &exist) != VMM_SUCCESS || !exist.present) {
                res = vmm_map_page(pml4, va, act.paddr, VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC);
                if (res != VMM_SUCCESS) return res;
            }
        }
    }

    /* 5. Map required HHDM ranges */
    phys_addr_t k_phys_text_start = k_phys_base + (text_vstart - k_virt_base);
    phys_addr_t k_phys_rodata_end = k_phys_base + (rodata_vend - k_virt_base);

    kprintf("[VMM] Building HHDM mappings for required physical regions...\n");
    for (u64 i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry *e = mmap->entries[i];
        if (!e) continue;

        if (e->type == LIMINE_MEMMAP_USABLE || e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            for (phys_addr_t pa = e->base; pa < e->base + e->length; pa += PAGE_SIZE) {
                virt_addr_t va = g_hhdm_offset + pa;
                struct vmm_mapping exist;
                if (vmm_query(pml4, va, &exist) == VMM_SUCCESS && exist.present) continue;
                res = vmm_map_page(pml4, va, pa, VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC);
                if (res != VMM_SUCCESS) return res;
            }
        } else if (e->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
            for (phys_addr_t pa = e->base; pa < e->base + e->length; pa += PAGE_SIZE) {
                virt_addr_t va = g_hhdm_offset + pa;
                struct vmm_mapping exist;
                if (vmm_query(pml4, va, &exist) == VMM_SUCCESS && exist.present) continue;

                /* Enforce W^X consistency: HHDM alias for text and rodata MUST be Read-Only! */
                u32 flags;
                if (pa >= k_phys_text_start && pa < k_phys_rodata_end) {
                    flags = VMM_FLAG_READ | VMM_FLAG_NO_EXEC;
                } else {
                    flags = VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC;
                }
                res = vmm_map_page(pml4, va, pa, flags);
                if (res != VMM_SUCCESS) return res;
            }
        } else if (e->type == LIMINE_MEMMAP_FRAMEBUFFER) {
            u32 fb_cache = 0;
            struct vmm_mapping act_fb;
            if (vmm_walk_active(g_hhdm_offset + e->base, &act_fb) == VMM_SUCCESS && act_fb.present) {
                if (act_fb.pwt) fb_cache |= VMM_FLAG_PWT;
                if (act_fb.pcd) fb_cache |= VMM_FLAG_PCD;
                if (act_fb.pat) fb_cache |= VMM_FLAG_PAT;
            }
            for (phys_addr_t pa = e->base; pa < e->base + e->length; pa += PAGE_SIZE) {
                virt_addr_t va = g_hhdm_offset + pa;
                struct vmm_mapping exist;
                if (vmm_query(pml4, va, &exist) == VMM_SUCCESS && exist.present) continue;
                res = vmm_map_page(pml4, va, pa, VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC | fb_cache);
                if (res != VMM_SUCCESS) return res;
            }
        }
    }

    /* 6. Ensure Framebuffer buffer is mapped if provided */
    if (fb_resp && fb_resp->framebuffer_count > 0 && fb_resp->framebuffers[0]) {
        struct limine_framebuffer *fb = fb_resp->framebuffers[0];
        virt_addr_t fb_va = (virt_addr_t)fb->address;
        size_t fb_size = (size_t)fb->pitch * fb->height;
        fb_size = (fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (size_t off = 0; off < fb_size; off += PAGE_SIZE) {
            virt_addr_t va = fb_va + off;
            struct vmm_mapping exist;
            if (vmm_query(pml4, va, &exist) == VMM_SUCCESS && exist.present) continue;

            struct vmm_mapping act;
            if (vmm_walk_active(va, &act) == VMM_SUCCESS && act.present) {
                u32 fb_cache = 0;
                if (act.pwt) fb_cache |= VMM_FLAG_PWT;
                if (act.pcd) fb_cache |= VMM_FLAG_PCD;
                if (act.pat) fb_cache |= VMM_FLAG_PAT;
                res = vmm_map_page(pml4, va, act.paddr, VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_NO_EXEC | fb_cache);
                if (res != VMM_SUCCESS) return res;
            }
        }
    }

    g_kernel_pml4 = pml4;
    kprintf("[VMM] Kernel-owned address space built successfully. PML4 root PA: 0x%016llx\n",
            (unsigned long long)pml4);
    return VMM_SUCCESS;
}
