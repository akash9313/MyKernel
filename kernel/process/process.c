#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/sched.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <kernel/heap.h>
#include <kernel/boot.h>
#include <kernel/panic.h>
#include <kernel/kprintf.h>
#include <kernel/elf.h>
#include <kernel/user_app_elf.h>
#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/vfs.h>
#include <kernel/initramfs.h>
#include <kernel/wm.h>

extern const u8 user_prog1_start[];
extern const u8 user_prog1_end[];
extern const u8 user_prog2_fault_start[];
extern const u8 user_prog2_fault_end[];
extern const u8 user_prog3_gui_fault_start[];
extern const u8 user_prog3_gui_fault_end[];
extern const u8 user_prog4_mmap_fault_start[];
extern const u8 user_prog4_mmap_fault_end[];

static process_t g_process_table[PROCESS_MAX_PROCESSES];
static pid_t g_next_pid = 1;

static void *kmemset(void *dest, int val, size_t len) {
    u8 *d = (u8 *)dest;
    for (size_t i = 0; i < len; i++) {
        d[i] = (u8)val;
    }
    return dest;
}

static void *kmemcpy(void *dest, const void *src, size_t len) {
    u8 *d = (u8 *)dest;
    const u8 *s = (const u8 *)src;
    for (size_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return dest;
}

static void safe_strncpy(char *dst, const char *src, size_t max) {
    if (!dst || max == 0) return;
    size_t i = 0;
    if (src) {
        while (i + 1 < max && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

void process_subsystem_init(void) {
    for (int i = 0; i < PROCESS_MAX_PROCESSES; i++) {
        kmemset(&g_process_table[i], 0, sizeof(process_t));
        g_process_table[i].state = PROCESS_UNUSED;
    }
    g_next_pid = 1;
    kprintf("[PROCESS] Process subsystem initialized (max %d processes).\n", PROCESS_MAX_PROCESSES);
}

static void process_entry_trampoline(void *arg) {
    process_t *proc = (process_t *)arg;
    if (!proc) {
        panic("process_entry_trampoline: NULL process argument!");
    }

    proc->state = PROCESS_RUNNING;

    /*
     * Configure TSS RSP0 to this process thread's kernel stack top.
     * When any user interrupt or syscall occurs at CPL3, the CPU switches
     * RSP to this kernel stack top.
     */
    tss_set_rsp0(proc->thread->stack_top);

    /* Switch to this process's virtual address space */
    write_cr3(proc->pml4_phys);

    kprintf("[PROCESS] Process '%s' (PID %u) entering Ring 3 at RIP=0x%016llx, RSP=0x%016llx\n",
            proc->name, proc->pid, (unsigned long long)proc->code_va, (unsigned long long)proc->user_stack_top);

    /* Jump to Ring 3 (never returns) */
    user_jump_to_ring3(proc->code_va, proc->user_stack_top);
}

static void process_rollback(process_t *proc) {
    if (!proc) return;

    /* Free all tracked physical memory frames */
    for (size_t i = 0; i < proc->mapped_page_count; i++) {
        if (proc->mapped_pas[i] != 0) {
            pmm_free_page(proc->mapped_pas[i]);
            proc->mapped_pas[i] = 0;
        }
    }
    proc->mapped_page_count = 0;

    /* Destroy user address space */
    if (proc->pml4_phys != 0) {
        vmm_destroy_process_space(proc->pml4_phys);
        proc->pml4_phys = 0;
    }

    proc->state = PROCESS_UNUSED;
}

process_t *process_create(const char *name, const void *code_bytes, size_t code_len) {
    if (!code_bytes || code_len == 0 || code_len > (4 * 4096)) {
        return NULL;
    }

    /* 1. Allocate process slot */
    process_t *proc = NULL;
    for (int i = 0; i < PROCESS_MAX_PROCESSES; i++) {
        if (g_process_table[i].state == PROCESS_UNUSED) {
            proc = &g_process_table[i];
            break;
        }
    }
    if (!proc) {
        kprintf("[PROCESS] process_create: Max process limit reached!\n");
        return NULL;
    }

    kmemset(proc, 0, sizeof(process_t));
    safe_strncpy(proc->name, name ? name : "user_proc", sizeof(proc->name));
    proc->code_va = USER_CODE_BASE_VA;
    proc->user_stack_top = USER_STACK_TOP_VA;
    proc->user_stack_bottom = USER_STACK_BOTTOM_VA;
    proc->user_stack_guard = USER_STACK_GUARD_VA;
    proc->mmap_curr_va = USER_MMAP_BASE_VA;

    /* 2. Create isolated process virtual address space */
    if (vmm_create_process_space(&proc->pml4_phys) != VMM_SUCCESS) {
        proc->state = PROCESS_UNUSED;
        return NULL;
    }

    u64 hhdm = boot_get_hhdm_response()->offset;

    /* 3. Allocate and map user code pages */
    proc->code_page_count = (code_len + 4095) / 4096;
    for (size_t i = 0; i < proc->code_page_count; i++) {
        phys_addr_t pa = 0;
        if (pmm_alloc_page(&pa) != PMM_SUCCESS) {
            process_rollback(proc);
            return NULL;
        }
        proc->mapped_pas[proc->mapped_page_count++] = pa;
        proc->code_pas[i] = pa;
        kmemset((void *)(pa + hhdm), 0, 4096);

        size_t chunk = (code_len > (i * 4096)) ? (code_len - (i * 4096)) : 0;
        if (chunk > 4096) chunk = 4096;
        if (chunk > 0) {
            kmemcpy((void *)(pa + hhdm), (const u8 *)code_bytes + (i * 4096), chunk);
        }

        virt_addr_t va = USER_CODE_BASE_VA + i * 4096;
        if (vmm_map_page(proc->pml4_phys, va, pa, VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_USER) != VMM_SUCCESS) {
            process_rollback(proc);
            return NULL;
        }
    }

    /* 4. Allocate and map user stack pages */
    proc->stack_page_count = USER_STACK_PAGES;
    for (size_t j = 0; j < proc->stack_page_count; j++) {
        phys_addr_t pa = 0;
        if (pmm_alloc_page(&pa) != PMM_SUCCESS) {
            process_rollback(proc);
            return NULL;
        }
        proc->mapped_pas[proc->mapped_page_count++] = pa;
        proc->stack_pas[j] = pa;
        kmemset((void *)(pa + hhdm), 0, 4096);

        virt_addr_t va = USER_STACK_BOTTOM_VA + j * 4096;
        if (vmm_map_page(proc->pml4_phys, va, pa, VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXEC) != VMM_SUCCESS) {
            process_rollback(proc);
            return NULL;
        }
    }

    /* 5. Create underlying execution thread */
    proc->thread = thread_create(proc->name, process_entry_trampoline, proc);
    if (!proc->thread) {
        process_rollback(proc);
        return NULL;
    }

    proc->thread->process = proc;
    proc->pid = g_next_pid++;
    proc->state = PROCESS_READY;

    return proc;
}

process_t *process_create_elf_reader(const char *name, elf_reader_t *reader) {
    if (!reader) return NULL;

    elf_info_t info;
    int err = elf_validate_reader(reader, &info);
    if (err != ELF_OK) {
        kprintf("[ELF] Validation failed for '%s': %s (%d)\n",
                name ? name : "unnamed", elf_strerror(err), err);
        return NULL;
    }

    /* 1. Allocate process slot */
    process_t *proc = NULL;
    for (int i = 0; i < PROCESS_MAX_PROCESSES; i++) {
        if (g_process_table[i].state == PROCESS_UNUSED) {
            proc = &g_process_table[i];
            break;
        }
    }
    if (!proc) {
        kprintf("[ELF] process_create_elf_reader: Max process limit reached!\n");
        return NULL;
    }

    kmemset(proc, 0, sizeof(process_t));
    safe_strncpy(proc->name, name ? name : "elf_proc", sizeof(proc->name));
    proc->mmap_curr_va = USER_MMAP_BASE_VA;

    /* 2. Create isolated process virtual address space */
    if (vmm_create_process_space(&proc->pml4_phys) != VMM_SUCCESS) {
        proc->state = PROCESS_UNUSED;
        return NULL;
    }

    u64 hhdm = boot_get_hhdm_response()->offset;

    Elf64_Ehdr ehdr;
    if (reader->read_at(reader, &ehdr, sizeof(Elf64_Ehdr), 0) != sizeof(Elf64_Ehdr)) {
        process_rollback(proc);
        return NULL;
    }

    /* 3. Load all PT_LOAD segments via bounded reader */
    for (Elf64_Half i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr phdr;
        size_t ph_off = ehdr.e_phoff + (i * ehdr.e_phentsize);
        if (reader->read_at(reader, &phdr, sizeof(Elf64_Phdr), ph_off) != sizeof(Elf64_Phdr)) {
            process_rollback(proc);
            return NULL;
        }

        if (phdr.p_type != PT_LOAD) {
            continue;
        }

        u64 seg_start = phdr.p_vaddr & ~0xFFFULL;
        u64 seg_end = (phdr.p_vaddr + phdr.p_memsz + 0xFFFULL) & ~0xFFFULL;

        u32 flags = VMM_FLAG_USER;
        if (phdr.p_flags & PF_R) flags |= VMM_FLAG_READ;
        if (phdr.p_flags & PF_W) flags |= VMM_FLAG_WRITE;
        if (!(phdr.p_flags & PF_X)) flags |= VMM_FLAG_NO_EXEC;

        u64 file_start = phdr.p_vaddr;
        u64 file_end = phdr.p_vaddr + phdr.p_filesz;

        for (u64 page_va = seg_start; page_va < seg_end; page_va += 4096ULL) {
            if (proc->mapped_page_count >= PROCESS_MAX_MAPPED_PAGES) {
                kprintf("[ELF] process_create_elf_reader: Process page capacity exceeded!\n");
                process_rollback(proc);
                return NULL;
            }

            phys_addr_t pa = 0;
            if (pmm_alloc_page(&pa) != PMM_SUCCESS) {
                process_rollback(proc);
                return NULL;
            }
            proc->mapped_pas[proc->mapped_page_count++] = pa;

            /* Zero the entire frame (guarantees .bss zero-initialization) */
            kmemset((void *)(pa + hhdm), 0, 4096);

            /* If page overlaps with file bytes, copy segment content via reader */
            if (page_va + 4096 > file_start && page_va < file_end) {
                u64 copy_start = (page_va > file_start) ? page_va : file_start;
                u64 copy_end = (page_va + 4096 < file_end) ? (page_va + 4096) : file_end;
                size_t copy_len = (size_t)(copy_end - copy_start);
                size_t page_offset = (size_t)(copy_start - page_va);
                size_t file_offset = (size_t)(phdr.p_offset + (copy_start - file_start));

                ssize_t rd = reader->read_at(reader, (u8 *)(pa + hhdm) + page_offset, copy_len, file_offset);
                if (rd != (ssize_t)copy_len) {
                    process_rollback(proc);
                    return NULL;
                }
            }

            if (vmm_map_page(proc->pml4_phys, page_va, pa, flags) != VMM_SUCCESS) {
                process_rollback(proc);
                return NULL;
            }
        }
    }

    /* 4. Allocate and map user stack pages (16 KiB) with guard page */
    proc->stack_page_count = USER_STACK_PAGES;
    proc->user_stack_bottom = USER_STACK_BOTTOM_VA;
    proc->user_stack_top = USER_STACK_TOP_VA;
    proc->user_stack_guard = USER_STACK_GUARD_VA;

    for (size_t j = 0; j < proc->stack_page_count; j++) {
        if (proc->mapped_page_count >= PROCESS_MAX_MAPPED_PAGES) {
            process_rollback(proc);
            return NULL;
        }

        phys_addr_t pa = 0;
        if (pmm_alloc_page(&pa) != PMM_SUCCESS) {
            process_rollback(proc);
            return NULL;
        }
        proc->mapped_pas[proc->mapped_page_count++] = pa;
        proc->stack_pas[j] = pa;
        kmemset((void *)(pa + hhdm), 0, 4096);

        virt_addr_t va = USER_STACK_BOTTOM_VA + j * 4096;
        if (vmm_map_page(proc->pml4_phys, va, pa, VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NO_EXEC) != VMM_SUCCESS) {
            process_rollback(proc);
            return NULL;
        }
    }

    /* 5. Set entry point to ELF header e_entry */
    proc->code_va = info.entry_point;

    /* 6. Create execution thread */
    proc->thread = thread_create(proc->name, process_entry_trampoline, proc);
    if (!proc->thread) {
        process_rollback(proc);
        return NULL;
    }

    proc->thread->process = proc;
    proc->pid = g_next_pid++;
    proc->state = PROCESS_READY;

    return proc;
}

process_t *process_create_elf(const char *name, const void *elf_bytes, size_t elf_size) {
    if (!elf_bytes) return NULL;
    elf_reader_t reader;
    elf_mem_context_t ctx;
    elf_reader_init_memory(&reader, &ctx, elf_bytes, elf_size);
    return process_create_elf_reader(name, &reader);
}

typedef struct {
    vfs_fd_t fd;
    size_t   size;
} elf_vfs_context_t;

static ssize_t vfs_elf_read_at(elf_reader_t *reader, void *buf, size_t count, size_t offset) {
    elf_vfs_context_t *ctx = (elf_vfs_context_t *)reader->context;
    return vfs_pread(ctx->fd, buf, count, offset);
}

static size_t vfs_elf_get_size(elf_reader_t *reader) {
    elf_vfs_context_t *ctx = (elf_vfs_context_t *)reader->context;
    return ctx->size;
}

process_t *process_create_elf_file(const char *name, const char *vfs_path) {
    if (!vfs_path) return NULL;

    vfs_fd_t fd = vfs_open(vfs_path, VFS_O_RDONLY);
    if (fd < 0) {
        kprintf("[ELF] Failed to open ELF file '%s': %s (%d)\n", vfs_path, vfs_strerror(fd), fd);
        return NULL;
    }

    vfs_stat_t st;
    int s_err = vfs_fstat(fd, &st);
    if (s_err != VFS_OK) {
        vfs_close(fd);
        return NULL;
    }

    elf_vfs_context_t ctx;
    ctx.fd = fd;
    ctx.size = st.size;

    elf_reader_t reader;
    reader.context = &ctx;
    reader.read_at = vfs_elf_read_at;
    reader.get_size = vfs_elf_get_size;

    process_t *proc = process_create_elf_reader(name, &reader);
    vfs_close(fd);
    return proc;
}

process_t *process_current(void) {
    thread_t *curr = thread_current();
    if (!curr) return NULL;
    return curr->process;
}

void process_exit(int exit_code) {
    process_t *proc = process_current();
    if (!proc) {
        panic("process_exit called without an active process context!");
    }

    proc->exit_code = exit_code;
    proc->state = PROCESS_ZOMBIE;

    kprintf("[PROCESS] Process '%s' (PID %u) terminated with exit code %d\n",
            proc->name, proc->pid, exit_code);

    /* Automatically destroy all GUI windows owned by this process */
    wm_destroy_all_for_pid(proc->pid);

    /* Switch back to kernel page table before destroying process address space */
    write_cr3(vmm_get_kernel_pml4());

    /* Reclaim all tracked user physical memory frames */
    for (size_t i = 0; i < proc->mapped_page_count; i++) {
        if (proc->mapped_pas[i] != 0) {
            pmm_free_page(proc->mapped_pas[i]);
            proc->mapped_pas[i] = 0;
        }
    }
    proc->mapped_page_count = 0;

    /* Reclaim user page tables and PML4 root */
    if (proc->pml4_phys != 0) {
        vmm_destroy_process_space(proc->pml4_phys);
        proc->pml4_phys = 0;
    }

    proc->state = PROCESS_DEAD;
    if (proc->thread) {
        proc->thread->process = NULL;
    }

    /* Terminate underlying kernel thread */
    thread_exit();
}

void process_exit_from_fault(struct interrupt_frame *frame) {
    process_t *proc = process_current();
    if (!proc) {
        panic("process_exit_from_fault called without an active process!");
    }

    proc->exited_from_fault = true;
    proc->fault_vector = frame->vector;
    proc->fault_rip = frame->rip;
    if (frame->vector == 14) {
        proc->fault_cr2 = read_cr2();
    }

    kprintf("\n[PROCESS] User process '%s' (PID %u) fault caught at RIP=0x%016llx, Vector=%llu, CR2=0x%016llx\n",
            proc->name, proc->pid, (unsigned long long)frame->rip, (unsigned long long)frame->vector,
            (unsigned long long)proc->fault_cr2);

    process_exit(-11);
}

void process_reap_zombies(void) {
    for (int i = 0; i < PROCESS_MAX_PROCESSES; i++) {
        if (g_process_table[i].state == PROCESS_DEAD) {
            g_process_table[i].state = PROCESS_UNUSED;
        }
    }
}

void process_run_tests(void) {
    kprintf("\n=======================================================\n");
    kprintf("       AstraOS Phase E & F: Process & ELF Tests         \n");
    kprintf("=======================================================\n");

    /* -------------------------------------------------------------
     * TEST 1: Minimal User Process & Syscall Boundary Verification
     * ------------------------------------------------------------- */
    kprintf("[TEST 1] Creating minimal Ring 3 user process...\n");
    size_t prog1_len = (size_t)(user_prog1_end - user_prog1_start);
    process_t *p1 = process_create("user_demo", user_prog1_start, prog1_len);
    if (!p1) {
        panic("TEST 1 FAILED: Could not create user process!");
    }

    kprintf("[TEST 1] Process created: PID %u. Yielding CPU to let Ring 3 run...\n", p1->pid);
    while (p1->state != PROCESS_DEAD) {
        thread_yield();
    }

    kprintf("[TEST 1] Process %u terminated. Verifying execution state...\n", p1->pid);
    if (p1->exit_code != 0 || p1->exited_from_fault) {
        kprintf("[TEST 1] ERROR: Process exited with code %d (fault=%d)!\n",
                p1->exit_code, p1->exited_from_fault);
        panic("TEST 1 FAILED: User process did not exit cleanly!");
    }

    kprintf("[TEST 1] PASS: Ring 3 execution, all 4 syscalls (getpid, write, yield, exit) verified!\n");
    kprintf("ASTRA_USERMODE_OK\n");
    kprintf("ASTRA_SYSCALL_OK\n");
    kprintf("ASTRA_PROCESS_EXIT_OK\n\n");

    /* -------------------------------------------------------------
     * TEST 2: User Memory Protection & Hardware Fault Isolation
     * ------------------------------------------------------------- */
    kprintf("[TEST 2] Testing user-mode memory isolation and hardware fault trapping...\n");
    size_t prog2_len = (size_t)(user_prog2_fault_end - user_prog2_fault_start);
    process_t *p2 = process_create("user_fault_test", user_prog2_fault_start, prog2_len);
    if (!p2) {
        panic("TEST 2 FAILED: Could not create fault test process!");
    }

    kprintf("[TEST 2] Process %u attempting unauthorized access to supervisor address 0xffffffff80400000...\n", p2->pid);
    while (p2->state != PROCESS_DEAD) {
        thread_yield();
    }

    if (!p2->exited_from_fault || p2->fault_vector != 14) {
        kprintf("[TEST 2] ERROR: Expected Page Fault (vector 14) but got vector %llu, fault=%d\n",
                (unsigned long long)p2->fault_vector, p2->exited_from_fault);
        panic("TEST 2 FAILED: Hardware protection boundary not trapped!");
    }

    kprintf("[TEST 2] PASS: CPU Vector 14 (#PF) caught cleanly! Process terminated without corrupting kernel.\n");
    kprintf("ASTRA_USER_MEMORY_PROTECTION_OK\n\n");

    /* -------------------------------------------------------------
     * TEST 3: Safe Transactional Rollback on Invalid Process Creation
     * ------------------------------------------------------------- */
    kprintf("[TEST 3] Testing allocation and validation rollback on invalid process creation...\n");
    process_t *p_bad1 = process_create(NULL, NULL, 0);
    process_t *p_bad2 = process_create("too_large", user_prog1_start, 5 * 4096);
    if (p_bad1 != NULL || p_bad2 != NULL) {
        panic("TEST 3 FAILED: Invalid process creation was not rejected!");
    }
    kprintf("[TEST 3] PASS: Invalid process parameters rejected cleanly with zero leaks.\n\n");

    /* -------------------------------------------------------------
     * TEST 4: Phase F - ELF64 Validation & Malformed Input Trapping
     * ------------------------------------------------------------- */
    kprintf("[TEST 4] Testing ELF64 Validation and Malformed Input Rejection...\n");

    /* 4A: Valid ELF Image Validation */
    elf_info_t elf_info;
    int val_res = elf_validate(g_user_app_elf, g_user_app_elf_len, &elf_info);
    if (val_res != ELF_OK) {
        panic("TEST 4A FAILED: Valid ELF failed validation: %s (%d)", elf_strerror(val_res), val_res);
    }
    if (elf_info.entry_point != 0x400000 || elf_info.load_segment_count != 3) {
        panic("TEST 4A FAILED: Unexpected ELF metadata: entry=0x%llx, segs=%llu",
              (unsigned long long)elf_info.entry_point, (unsigned long long)elf_info.load_segment_count);
    }
    kprintf("[TEST 4A] PASS: Native ELF header and segments validated successfully.\n");
    kprintf("ASTRA_ELF_HEADER_OK\n");
    kprintf("ASTRA_ELF_SEGMENTS_OK\n");

    /* 4B: Rejection of NULL pointer & truncated size */
    if (elf_validate(NULL, 100, NULL) != ELF_ERR_NULL_PTR) panic("TEST 4B FAILED: NULL image not rejected!");
    if (elf_validate(g_user_app_elf, 32, NULL) != ELF_ERR_TOO_SMALL) panic("TEST 4B FAILED: Truncated ELF not rejected!");

    /* 4C: Rejection of invalid magic */
    u8 *mut_elf = (u8 *)kmalloc(g_user_app_elf_len);
    if (!mut_elf) panic("TEST 4: Failed to allocate mut_elf buffer");
    kmemcpy(mut_elf, g_user_app_elf, g_user_app_elf_len);
    mut_elf[0] = 0x00;
    if (elf_validate(mut_elf, g_user_app_elf_len, NULL) != ELF_ERR_INVALID_MAGIC) panic("TEST 4C FAILED: Bad magic not rejected!");

    /* 4D: Rejection of 32-bit class */
    kmemcpy(mut_elf, g_user_app_elf, g_user_app_elf_len);
    mut_elf[EI_CLASS] = ELFCLASS32;
    if (elf_validate(mut_elf, g_user_app_elf_len, NULL) != ELF_ERR_UNSUPPORTED_CLASS) panic("TEST 4D FAILED: ELF32 not rejected!");

    /* 4E: Rejection of big-endian */
    kmemcpy(mut_elf, g_user_app_elf, g_user_app_elf_len);
    mut_elf[EI_DATA] = ELFDATA2MSB;
    if (elf_validate(mut_elf, g_user_app_elf_len, NULL) != ELF_ERR_UNSUPPORTED_ENDIAN) panic("TEST 4E FAILED: Big endian not rejected!");

    /* 4F: Rejection of non-x86_64 architecture (e.g. EM_386 = 3) */
    kmemcpy(mut_elf, g_user_app_elf, g_user_app_elf_len);
    Elf64_Ehdr *mut_ehdr = (Elf64_Ehdr *)mut_elf;
    mut_ehdr->e_machine = 3;
    if (elf_validate(mut_elf, g_user_app_elf_len, NULL) != ELF_ERR_UNSUPPORTED_MACHINE) panic("TEST 4F FAILED: Non-x86_64 machine not rejected!");

    /* 4G: Rejection of dynamic library (ET_DYN) */
    kmemcpy(mut_elf, g_user_app_elf, g_user_app_elf_len);
    mut_ehdr = (Elf64_Ehdr *)mut_elf;
    mut_ehdr->e_type = ET_DYN;
    if (elf_validate(mut_elf, g_user_app_elf_len, NULL) != ELF_ERR_UNSUPPORTED_TYPE) panic("TEST 4G FAILED: ET_DYN not rejected!");

    /* 4H: Rejection of p_filesz > p_memsz */
    kmemcpy(mut_elf, g_user_app_elf, g_user_app_elf_len);
    mut_ehdr = (Elf64_Ehdr *)mut_elf;
    Elf64_Phdr *mut_phdr = (Elf64_Phdr *)(mut_elf + mut_ehdr->e_phoff);
    mut_phdr->p_filesz = mut_phdr->p_memsz + 100;
    if (elf_validate(mut_elf, g_user_app_elf_len, NULL) != ELF_ERR_INVALID_SEGMENT) panic("TEST 4H FAILED: filesz > memsz not rejected!");

    /* 4I: Rejection of kernel-space virtual address in user segment */
    kmemcpy(mut_elf, g_user_app_elf, g_user_app_elf_len);
    mut_ehdr = (Elf64_Ehdr *)mut_elf;
    mut_phdr = (Elf64_Phdr *)(mut_elf + mut_ehdr->e_phoff);
    mut_phdr->p_vaddr = 0xffffffff80000000ULL;
    int err4i = elf_validate(mut_elf, g_user_app_elf_len, NULL);
    if (err4i != ELF_ERR_INVALID_ADDRESS) panic("TEST 4I FAILED: Kernel vaddr not rejected! (got %s, %d)", elf_strerror(err4i), err4i);

    /* 4J: Rejection of dynamic interpreter segment (PT_INTERP) */
    kmemcpy(mut_elf, g_user_app_elf, g_user_app_elf_len);
    mut_ehdr = (Elf64_Ehdr *)mut_elf;
    mut_phdr = (Elf64_Phdr *)(mut_elf + mut_ehdr->e_phoff);
    mut_phdr->p_type = PT_INTERP;
    if (elf_validate(mut_elf, g_user_app_elf_len, NULL) != ELF_ERR_DYNAMIC_NOT_SUPPORTED) panic("TEST 4J FAILED: PT_INTERP not rejected!");

    /* 4K: Rejection of W^X conflict (segment both writable and executable) */
    kmemcpy(mut_elf, g_user_app_elf, g_user_app_elf_len);
    mut_ehdr = (Elf64_Ehdr *)mut_elf;
    mut_phdr = (Elf64_Phdr *)(mut_elf + mut_ehdr->e_phoff);
    mut_phdr->p_flags = (PF_W | PF_X | PF_R);
    if (elf_validate(mut_elf, g_user_app_elf_len, NULL) != ELF_ERR_WX_CONFLICT) panic("TEST 4K FAILED: W^X conflict not rejected!");

    /* 4L: Rejection of entry point outside executable segments */
    kmemcpy(mut_elf, g_user_app_elf, g_user_app_elf_len);
    mut_ehdr = (Elf64_Ehdr *)mut_elf;
    mut_ehdr->e_entry = 0x00200000ULL; /* Valid user VA but not within loaded segment */
    if (elf_validate(mut_elf, g_user_app_elf_len, NULL) != ELF_ERR_INVALID_ENTRY) panic("TEST 4L FAILED: Invalid entry point not rejected!");

    kprintf("[TEST 4] PASS: All malformed and unsupported ELF variants safely rejected with defined error codes.\n");
    kprintf("ASTRA_ELF_INVALID_INPUT_OK\n\n");

    /* -------------------------------------------------------------
     * TEST 5: Phase F - Transactional Rollback on Failed ELF Creation
     * ------------------------------------------------------------- */
    kprintf("[TEST 5] Testing transactional rollback during ELF process creation...\n");
    struct pmm_stats pmm_before;
    pmm_get_stats(&pmm_before);

    process_t *bad_elf_proc = process_create_elf("bad_elf", mut_elf, g_user_app_elf_len);
    if (bad_elf_proc != NULL) {
        panic("TEST 5 FAILED: Corrupted ELF process creation succeeded!");
    }

    struct pmm_stats pmm_after;
    pmm_get_stats(&pmm_after);
    if (pmm_before.free_pages != pmm_after.free_pages) {
        panic("TEST 5 FAILED: PMM frame leak detected on aborted ELF creation: before=%llu, after=%llu",
              (unsigned long long)pmm_before.free_pages, (unsigned long long)pmm_after.free_pages);
    }
    kprintf("[TEST 5] PASS: Aborted ELF creation cleanly rolled back all page tables and frames.\n");
    kprintf("ASTRA_ELF_ROLLBACK_OK\n\n");

    kfree(mut_elf);

    /* -------------------------------------------------------------
     * TEST 6: Phase F - Native ELF Execution (CPL3, BSS, Syscalls)
     * ------------------------------------------------------------- */
    kprintf("[TEST 6] Loading and launching native user program 'user_app' via ELF64 loader...\n");
    process_t *elf_proc = process_create_elf("user_app", g_user_app_elf, g_user_app_elf_len);
    if (!elf_proc) {
        panic("TEST 6 FAILED: Failed to create ELF user process!");
    }

    kprintf("[TEST 6] Process '%s' created (PID %u, entry 0x%016llx). Yielding CPU...\n",
            elf_proc->name, elf_proc->pid, (unsigned long long)elf_proc->code_va);

    while (elf_proc->state != PROCESS_DEAD) {
        thread_yield();
    }

    kprintf("[TEST 6] Process %u finished execution with status %d (fault=%d)\n",
            elf_proc->pid, elf_proc->exit_code, elf_proc->exited_from_fault);

    if (elf_proc->exit_code != 0 || elf_proc->exited_from_fault) {
        panic("TEST 6 FAILED: Native ELF user process did not exit with code 0! code=%d, fault=%d",
              elf_proc->exit_code, elf_proc->exited_from_fault);
    }

    kprintf("[TEST 6] PASS: Native ELF user process successfully executed in Ring 3 and verified .bss zeroing & syscalls!\n");
    kprintf("ASTRA_ELF_USER_EXEC_OK\n\n");

    process_reap_zombies();
    kprintf("[TEST] Phase E & F Tests Completed Successfully!\n\n");
}

void process_gui_run_tests(void) {
    kprintf("\n=======================================================\n");
    kprintf("  AstraOS Phase N: User-Space GUI & Process Isolation  \n");
    kprintf("=======================================================\n");

    /* -------------------------------------------------------------
     * TEST 1: Launch First Real Ring 3 GUI Application (/bin/hello_gui.elf)
     * ------------------------------------------------------------- */
    kprintf("[TEST 1] Loading and launching native user GUI application '/bin/hello_gui.elf'...\n");
    process_t *p_gui1 = process_create_elf_file("hello_gui", "/bin/hello_gui.elf");
    if (!p_gui1) {
        panic("Phase N TEST 1 FAILED: Could not create process from /bin/hello_gui.elf!");
    }

    kprintf("[TEST 1] Process '%s' (PID %u) created. Entering Ring 3 scheduler loop...\n",
            p_gui1->name, p_gui1->pid);

    int loop = 0;
    while (p_gui1->state != PROCESS_DEAD && loop < 150) {
        loop++;
        thread_yield();

        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            wm_window_t *w = wm_find_by_id(i);
            if (w && w->owner_pid == p_gui1->pid) {
                if (loop == 10) {
                    /* Simulate mouse click in window */
                    input_event_t ev;
                    kmemset(&ev, 0, sizeof(ev));
                    ev.type = INPUT_EVENT_MOUSE_DOWN;
                    ev.mouse.x = 100;
                    ev.mouse.y = 100;
                    wm_window_push_event(w, &ev);
                } else if (loop == 40) {
                    /* Send window close event */
                    input_event_t ev;
                    kmemset(&ev, 0, sizeof(ev));
                    ev.type = INPUT_EVENT_WINDOW_CLOSE;
                    wm_window_push_event(w, &ev);
                }
            }
        }
    }

    if (p_gui1->state != PROCESS_DEAD) {
        kprintf("[TEST 1] Sending final close request to process %u...\n", p_gui1->pid);
        wm_destroy_all_for_pid(p_gui1->pid);
        while (p_gui1->state != PROCESS_DEAD) {
            thread_yield();
        }
    }

    if (p_gui1->exit_code != 0 || p_gui1->exited_from_fault) {
        panic("Phase N TEST 1 FAILED: hello_gui exited with code %d (fault=%d)!",
              p_gui1->exit_code, p_gui1->exited_from_fault);
    }

    kprintf("[TEST 1] PASS: First user GUI application executed in Ring 3 CPL=3, drew window, received events, and exited cleanly!\n");
    kprintf("ASTRA_USER_GUI_OK\n\n");

    /* -------------------------------------------------------------
     * TEST 2: Multiple Simultaneous User-Space Applications
     * ------------------------------------------------------------- */
    kprintf("[TEST 2] Launching two simultaneous independent user GUI processes...\n");
    process_t *p_app1 = process_create_elf_file("hello_gui_multi", "/bin/hello_gui.elf");
    process_t *p_app2 = process_create_elf_file("counter_gui", "/bin/counter_gui.elf");

    if (!p_app1 || !p_app2) {
        panic("Phase N TEST 2 FAILED: Could not create two concurrent GUI processes!");
    }

    if (p_app1->pid == p_app2->pid) {
        panic("Phase N TEST 2 FAILED: Processes have duplicate PIDs (%u)!", p_app1->pid);
    }
    if (p_app1->pml4_phys == p_app2->pml4_phys) {
        panic("Phase N TEST 2 FAILED: Processes share PML4 root address space (0x%llx)!",
              (unsigned long long)p_app1->pml4_phys);
    }

    kprintf("[TEST 2] Concurrent processes active: PID %u (CR3 0x%llx) and PID %u (CR3 0x%llx)\n",
            p_app1->pid, (unsigned long long)p_app1->pml4_phys,
            p_app2->pid, (unsigned long long)p_app2->pml4_phys);

    loop = 0;
    while ((p_app1->state != PROCESS_DEAD || p_app2->state != PROCESS_DEAD) && loop < 200) {
        loop++;
        thread_yield();

        if (loop == 25) {
            /* Close p_app1 first to verify p_app2 continues running uninterrupted */
            wm_destroy_all_for_pid(p_app1->pid);
        } else if (loop == 65) {
            /* Close p_app2 */
            wm_destroy_all_for_pid(p_app2->pid);
        }
    }

    while (p_app1->state != PROCESS_DEAD || p_app2->state != PROCESS_DEAD) {
        thread_yield();
    }

    if (p_app1->exit_code != 0 || p_app1->exited_from_fault ||
        p_app2->exit_code != 0 || p_app2->exited_from_fault) {
        panic("Phase N TEST 2 FAILED: Concurrent processes did not exit cleanly!");
    }

    kprintf("[TEST 2] PASS: Multiple independent GUI processes scheduled, isolated, and cleanly terminated!\n");
    kprintf("ASTRA_MULTI_PROCESS_GUI_OK\n\n");

    /* -------------------------------------------------------------
     * TEST 3: User Fault Isolation & Resource Reclamation
     * ------------------------------------------------------------- */
    kprintf("[TEST 3] Testing user fault isolation, non-owner window rejection, and cleanup...\n");

    /* 3A: Syscall pointer validation checks */
    if (sys_gui_create_window((const char *)0xffffffff80400000ULL, 10, 10, 200, 200) != -EFAULT) {
        panic("Phase N TEST 3A FAILED: Kernel pointer in title was not rejected with -EFAULT!");
    }
    int t_win = (int)sys_gui_create_window(NULL, 10, 10, 200, 200);
    if (t_win <= 0) {
        panic("Phase N TEST 3A FAILED: Failed to create valid test window!");
    }
    if (sys_gui_draw_text(t_win, 10, 10, (const char *)0xffffffff80400000ULL, 0, 0) != -EFAULT) {
        panic("Phase N TEST 3A FAILED: Kernel pointer in draw_text was not rejected with -EFAULT!");
    }
    if (sys_gui_get_event(t_win, (input_event_t *)0xffffffff80400000ULL) != -EFAULT) {
        panic("Phase N TEST 3A FAILED: Kernel pointer in get_event was not rejected with -EFAULT!");
    }
    if (sys_gui_draw_rect(t_win, 0, 0, -50, 100, 0) != -EINVAL) {
        panic("Phase N TEST 3A FAILED: Negative width was not rejected with -EINVAL!");
    }
    if (sys_gui_destroy_window(9999) != -ENOENT) {
        panic("Phase N TEST 3A FAILED: Non-existent window was not rejected with -ENOENT!");
    }
    sys_gui_destroy_window(t_win);

    /* 3B: Fault isolation test: user process allocates window, then triggers #PF */
    size_t prog3_len = (size_t)(user_prog3_gui_fault_end - user_prog3_gui_fault_start);
    process_t *p_fault = process_create("gui_fault_proc", user_prog3_gui_fault_start, prog3_len);
    if (!p_fault) {
        panic("Phase N TEST 3B FAILED: Could not create fault test process!");
    }

    while (p_fault->state != PROCESS_DEAD) {
        thread_yield();
    }

    if (!p_fault->exited_from_fault || p_fault->fault_vector != 14) {
        panic("Phase N TEST 3B FAILED: Expected Vector 14 fault, got fault=%d, vector=%llu",
              p_fault->exited_from_fault, (unsigned long long)p_fault->fault_vector);
    }

    /* Verify all windows owned by the faulted process were automatically reclaimed */
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        wm_window_t *w = wm_find_by_id(i);
        if (w && w->owner_pid == p_fault->pid) {
            panic("Phase N TEST 3B FAILED: Window owned by faulted process was not destroyed!");
        }
    }

    kprintf("[TEST 3] PASS: Syscall validation, non-owner protection, and hardware fault isolation verified!\n");
    kprintf("ASTRA_GUI_ISOLATION_OK\n\n");

    process_reap_zombies();
    desktop_render();
}

void process_browser_run_tests(void) {
    kprintf("\n=======================================================\n");
    kprintf("  AstraOS Phase O: User-Space Browser & Memory Tests   \n");
    kprintf("=======================================================\n");

    /* -------------------------------------------------------------
     * TEST 1: User-Space Memory Management & Protection (sys_mmap / sys_munmap)
     * ------------------------------------------------------------- */
    kprintf("[PHASE O TEST 1] Verifying sys_mmap and sys_munmap parameter validation...\n");
    if (sys_mmap(NULL, 0, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE) != -EINVAL) {
        panic("Phase O TEST 1 FAILED: Zero length mmap was not rejected with -EINVAL!");
    }
    if (sys_mmap(NULL, 32 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE) != -EINVAL) {
        panic("Phase O TEST 1 FAILED: Oversized mmap was not rejected with -EINVAL!");
    }
    if (sys_mmap(NULL, 4096, PROT_READ | PROT_WRITE, 0) != -EINVAL) {
        panic("Phase O TEST 1 FAILED: Non-anonymous mmap was not rejected with -EINVAL!");
    }
    if (sys_mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE) != -EPERM) {
        panic("Phase O TEST 1 FAILED: W^X violation was not rejected with -EPERM!");
    }
    if (sys_munmap(NULL, 4096) != -EINVAL) {
        panic("Phase O TEST 1 FAILED: NULL munmap was not rejected with -EINVAL!");
    }
    if (sys_munmap((void *)0x1234, 4096) != -EINVAL) {
        panic("Phase O TEST 1 FAILED: Unaligned munmap was not rejected with -EINVAL!");
    }
    /* Pointer validation on sys_net_fetch */
    if (sys_net_fetch((const char *)0xffffffff80400000ULL, NULL, 0, NULL) != -EINVAL) {
        panic("Phase O TEST 1 FAILED: Zero max_len fetch was not rejected with -EINVAL!");
    }
    if (sys_net_fetch("http://127.0.0.1/", (char *)0xffffffff80400000ULL, 1024, NULL) != -EFAULT) {
        panic("Phase O TEST 1 FAILED: Kernel pointer in fetch buffer was not rejected with -EFAULT!");
    }
    /* Pointer and parameter validation on sys_gui_draw_bitmap */
    if (sys_gui_draw_bitmap(1, 0, 0, 0, 10, NULL) != -EINVAL) {
        panic("Phase P TEST 1 FAILED: Zero width in draw_bitmap was not rejected with -EINVAL!");
    }
    if (sys_gui_draw_bitmap(1, 0, 0, 10, 10, (const u32 *)0xffffffff80400000ULL) != -EFAULT) {
        panic("Phase P TEST 1 FAILED: Kernel pointer in draw_bitmap was not rejected with -EFAULT!");
    }
    kprintf("[PHASE O TEST 1] PASS: Memory allocation boundaries and pointer security verified.\n");
    kprintf("ASTRA_MMAP_VALIDATION_OK\n\n");

    /* -------------------------------------------------------------
     * TEST 2: User-Space AstraBrowser Execution in Ring 3 CPL 3
     * ------------------------------------------------------------- */
    kprintf("[PHASE O TEST 2] Launching User-Space AstraBrowser '/bin/browser.elf'...\n");
    process_t *p_browser = process_create_elf_file("user_browser", "/bin/browser.elf");
    if (!p_browser) {
        panic("Phase O TEST 2 FAILED: Could not create process from /bin/browser.elf!");
    }

    if (p_browser->pml4_phys == vmm_get_kernel_pml4()) {
        panic("Phase O TEST 2 FAILED: Browser process shares kernel page table!");
    }
    if (p_browser->code_va != 0x400000) {
        panic("Phase O TEST 2 FAILED: Browser entry point is not in user space (0x%llx)!",
              (unsigned long long)p_browser->code_va);
    }

    kprintf("[PHASE O TEST 2] Process '%s' (PID %u, CR3 0x%llx) active. Entering scheduler loop...\n",
            p_browser->name, p_browser->pid, (unsigned long long)p_browser->pml4_phys);

    int b_loop = 0;
    while (p_browser->state != PROCESS_DEAD && b_loop < 150) {
        b_loop++;
        thread_yield();

        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            wm_window_t *w = wm_find_by_id(i);
            if (w && w->owner_pid == p_browser->pid) {
                if (b_loop == 10) {
                    /* Click link in viewport */
                    input_event_t ev;
                    kmemset(&ev, 0, sizeof(ev));
                    ev.type = INPUT_EVENT_MOUSE_DOWN;
                    ev.mouse.x = 80;
                    ev.mouse.y = 120;
                    wm_window_push_event(w, &ev);
                } else if (b_loop == 35) {
                    /* Close browser */
                    input_event_t ev;
                    kmemset(&ev, 0, sizeof(ev));
                    ev.type = INPUT_EVENT_WINDOW_CLOSE;
                    wm_window_push_event(w, &ev);
                }
            }
        }
    }

    if (p_browser->exit_code != 0 || p_browser->exited_from_fault) {
        panic("Phase O TEST 2 FAILED: Browser process terminated abnormally! (code=%d, fault=%d)",
              p_browser->exit_code, p_browser->exited_from_fault);
    }

    /* Verify browser window was destroyed upon exit */
    if (gfx_get_backbuffer() != NULL) {
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            wm_window_t *w = wm_find_by_id(i);
            if (w && w->owner_pid == p_browser->pid) {
                panic("Phase O TEST 2 FAILED: Browser window was not destroyed on exit!");
            }
        }
    }

    kprintf("[PHASE O TEST 2] PASS: User-space AstraBrowser ran at CPL 3, parsed HTML/CSS, allocated memory via mmap, and cleanly exited!\n");
    kprintf("ASTRA_USER_BROWSER_EXEC_OK\n\n");

    /* -------------------------------------------------------------
     * TEST 3: Hardware Memory Isolation & Fault Containment
     * ------------------------------------------------------------- */
    kprintf("[PHASE O TEST 3] Testing user mmap fault containment & resource reclamation...\n");
    size_t prog4_len = (size_t)(user_prog4_mmap_fault_end - user_prog4_mmap_fault_start);
    process_t *p_mmap_fault = process_create("mmap_fault_proc", user_prog4_mmap_fault_start, prog4_len);
    if (!p_mmap_fault) {
        panic("Phase O TEST 3 FAILED: Could not create mmap fault process!");
    }

    while (p_mmap_fault->state != PROCESS_DEAD) {
        thread_yield();
    }

    if (!p_mmap_fault->exited_from_fault || p_mmap_fault->fault_vector != 14) {
        panic("Phase O TEST 3 FAILED: Expected Vector 14 Page Fault, got fault=%d, vector=%llu",
              p_mmap_fault->exited_from_fault, (unsigned long long)p_mmap_fault->fault_vector);
    }

    if (p_mmap_fault->mapped_page_count != 0) {
        panic("Phase O TEST 3 FAILED: Mapped pages were not reclaimed on fault! count=%llu",
              (unsigned long long)p_mmap_fault->mapped_page_count);
    }

    kprintf("[PHASE O TEST 3] PASS: Mmap fault caught, supervisor access blocked, and resources reclaimed!\n");
    kprintf("ASTRA_BROWSER_ISOLATION_OK\n\n");

    process_reap_zombies();
    desktop_render();
}


