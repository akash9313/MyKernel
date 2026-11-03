#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#define HOST_TEST 1
#include <kernel/types.h>
#include <kernel/elf.h>

/* Include implementation directly for host testing */
#include "../kernel/process/elf.c"

/* Synthetic minimal valid ELF64 executable generator */
static size_t create_valid_test_elf(u8 *buf, size_t buf_max, u64 entry_va, u64 code_va, u64 code_sz, u64 data_va, u64 data_sz) {
    size_t phoff = sizeof(Elf64_Ehdr);
    size_t phnum = (data_sz > 0) ? 2 : 1;
    size_t code_file_off = 0x1000;
    size_t data_file_off = 0x2000;
    size_t total_size = (data_sz > 0) ? (data_file_off + data_sz) : (code_file_off + code_sz);

    assert(total_size <= buf_max);
    memset(buf, 0, total_size);

    Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
    eh->e_ident[EI_MAG0] = ELFMAG0;
    eh->e_ident[EI_MAG1] = ELFMAG1;
    eh->e_ident[EI_MAG2] = ELFMAG2;
    eh->e_ident[EI_MAG3] = ELFMAG3;
    eh->e_ident[EI_CLASS] = ELFCLASS64;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_type = ET_EXEC;
    eh->e_machine = EM_X86_64;
    eh->e_version = EV_CURRENT;
    eh->e_entry = entry_va;
    eh->e_phoff = phoff;
    eh->e_ehsize = sizeof(Elf64_Ehdr);
    eh->e_phentsize = sizeof(Elf64_Phdr);
    eh->e_phnum = (Elf64_Half)phnum;

    /* Code segment (RX) */
    Elf64_Phdr *ph_code = (Elf64_Phdr *)(buf + phoff);
    ph_code->p_type = PT_LOAD;
    ph_code->p_flags = PF_R | PF_X;
    ph_code->p_offset = code_file_off;
    ph_code->p_vaddr = code_va;
    ph_code->p_paddr = code_va;
    ph_code->p_filesz = code_sz;
    ph_code->p_memsz = code_sz;
    ph_code->p_align = 0x1000;

    /* Fill code bytes with 'ret' instructions (0xC3) */
    memset(buf + code_file_off, 0xC3, code_sz);

    if (data_sz > 0) {
        /* Data segment (RW) */
        Elf64_Phdr *ph_data = (Elf64_Phdr *)(buf + phoff + sizeof(Elf64_Phdr));
        ph_data->p_type = PT_LOAD;
        ph_data->p_flags = PF_R | PF_W;
        ph_data->p_offset = data_file_off;
        ph_data->p_vaddr = data_va;
        ph_data->p_paddr = data_va;
        ph_data->p_filesz = data_sz;
        ph_data->p_memsz = data_sz + 0x100; /* Includes BSS */
        ph_data->p_align = 0x1000;
        memset(buf + data_file_off, 0x42, data_sz);
    }

    return total_size;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int run_all_elf_host_tests(void) {
    printf("\n=================================================================\n");
    printf("   AstraOS Phase AK: ELF64 Loader Security & Validation Tests    \n");
    printf("=================================================================\n\n");

    static u8 elf_buf[65536];
    elf_info_t info;

    /* TEST 1: Valid ELF64 Binary Validation */
    printf("[TEST 1] Testing valid ELF64 executable...\n");
    size_t sz = create_valid_test_elf(elf_buf, sizeof(elf_buf), 0x401000, 0x401000, 0x200, 0x402000, 0x100);
    assert(elf_validate(elf_buf, sz, &info) == ELF_OK);
    assert(info.entry_point == 0x401000);
    assert(info.load_segment_count == 2);
    assert(info.min_vaddr == 0x401000);
    assert(info.max_vaddr == 0x403000);
    printf("  PASS: Valid ELF64 binary validated with matching segments and entry.\n");

    /* TEST 2: NULL Pointer & Truncated Header */
    printf("[TEST 2] Testing NULL buffer & truncated header rejection...\n");
    assert(elf_validate(NULL, sz, &info) == ELF_ERR_NULL_PTR);
    assert(elf_validate(elf_buf, sizeof(Elf64_Ehdr) - 1, &info) == ELF_ERR_TOO_SMALL);
    printf("  PASS: NULL pointer and truncated sizes safely rejected.\n");

    /* TEST 3: Magic Number Corruption */
    printf("[TEST 3] Testing corrupted ELF magic signatures...\n");
    elf_buf[0] = 0x00;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_MAGIC);
    elf_buf[0] = ELFMAG0;
    elf_buf[1] = 'X';
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_MAGIC);
    elf_buf[1] = ELFMAG1;
    printf("  PASS: Invalid ELF magic rejected.\n");

    /* TEST 4: Class, Endianness, Machine & Type Validation */
    printf("[TEST 4] Testing class, endianness, machine and type constraints...\n");
    Elf64_Ehdr *eh = (Elf64_Ehdr *)elf_buf;

    eh->e_ident[EI_CLASS] = ELFCLASS32;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_UNSUPPORTED_CLASS);
    eh->e_ident[EI_CLASS] = ELFCLASS64;

    eh->e_ident[EI_DATA] = ELFDATA2MSB;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_UNSUPPORTED_ENDIAN);
    eh->e_ident[EI_DATA] = ELFDATA2LSB;

    eh->e_machine = 3; /* EM_386 */
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_UNSUPPORTED_MACHINE);
    eh->e_machine = EM_X86_64;

    eh->e_type = ET_DYN; /* Shared library / PIE */
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_UNSUPPORTED_TYPE);
    eh->e_type = ET_EXEC;
    printf("  PASS: 32-bit, big-endian, non-x86_64 and dynamic executables cleanly rejected.\n");

    /* TEST 5: Phdr Table Bounds & Count */
    printf("[TEST 5] Testing program header table bounds & count checks...\n");
    eh->e_phoff = sz + 100;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_PHDR);
    eh->e_phoff = sizeof(Elf64_Ehdr);

    eh->e_phnum = 0;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_PHDR);
    eh->e_phnum = 65;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_PHDR);
    eh->e_phnum = 2;
    printf("  PASS: Program header table offsets and count bounds strictly enforced.\n");

    /* TEST 6: Segment Bounds (p_filesz > p_memsz) */
    printf("[TEST 6] Testing p_filesz > p_memsz constraint...\n");
    Elf64_Phdr *ph = (Elf64_Phdr *)(elf_buf + eh->e_phoff);
    u64 orig_filesz = ph->p_filesz;
    ph->p_filesz = ph->p_memsz + 10;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_SEGMENT);
    ph->p_filesz = orig_filesz;
    printf("  PASS: Segment filesz > memsz rejected.\n");

    /* TEST 7: Degenerate Segment (p_memsz == 0) */
    printf("[TEST 7] Testing degenerate segment with zero memsz...\n");
    u64 orig_memsz = ph->p_memsz;
    ph->p_memsz = 0;
    ph->p_filesz = 0;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_SEGMENT);
    ph->p_memsz = orig_memsz;
    ph->p_filesz = orig_filesz;
    printf("  PASS: Degenerate segment with p_memsz == 0 safely rejected.\n");

    /* TEST 8: File Offset Overflow & Out of Bounds */
    printf("[TEST 8] Testing file offset bounds & arithmetic overflow...\n");
    ph->p_offset = sz + 500;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_SEGMENT);
    ph->p_offset = 0xFFFFFFFFFFFFF000ULL;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_SEGMENT);
    ph->p_offset = 0x1000;
    printf("  PASS: Out-of-bounds and overflowing file offsets rejected.\n");

    /* TEST 9: Virtual Address Canonical Range & Overflow */
    printf("[TEST 9] Testing canonical user address space bounds...\n");
    ph->p_vaddr = 0x1000; /* Below 2 MiB null protection */
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_ADDRESS);

    ph->p_vaddr = 0xFFFFFFFF80000000ULL; /* Supervisor / Higher Half */
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_ADDRESS);

    ph->p_vaddr = 0x00007FFFFFFFFFFFULL; /* Exceeds lower canonical user limit */
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_ADDRESS);

    ph->p_vaddr = 0xFFFFFFFFFFFFFF00ULL; /* Integer overflow */
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_ADDRESS);
    ph->p_vaddr = 0x401000;
    printf("  PASS: Low null-page, kernel addresses, user bounds, and overflows enforced.\n");

    /* TEST 10: Alignment Constraints */
    printf("[TEST 10] Testing alignment and offset congruence...\n");
    ph->p_align = 0x1000;
    ph->p_offset = 0x1001; /* Offset 1 mod 4096 vs VA 0 mod 4096 */
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_ALIGNMENT);
    ph->p_offset = 0x1000;

    ph->p_align = 1000; /* Non-power of two */
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_ALIGNMENT);
    ph->p_align = 0x1000;
    printf("  PASS: Power-of-two alignment and modulo congruence validated.\n");

    /* TEST 11: Strict W^X Protection */
    printf("[TEST 11] Testing W^X violation rejection...\n");
    ph->p_flags = PF_R | PF_W | PF_X; /* Both W and X */
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_WX_CONFLICT);
    ph->p_flags = PF_R | PF_X;
    printf("  PASS: Segments with simultaneous W and X flags rejected with ELF_ERR_WX_CONFLICT.\n");

    /* TEST 12: Dynamic Linking Segments */
    printf("[TEST 12] Testing dynamic headers rejection (PT_INTERP / PT_DYNAMIC)...\n");
    ph->p_type = PT_INTERP;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_DYNAMIC_NOT_SUPPORTED);
    ph->p_type = PT_DYNAMIC;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_DYNAMIC_NOT_SUPPORTED);
    ph->p_type = PT_LOAD;
    printf("  PASS: Dynamic linking headers cleanly rejected.\n");

    /* TEST 13: Overlapping PT_LOAD Segments */
    printf("[TEST 13] Testing overlapping PT_LOAD segment detection...\n");
    Elf64_Phdr *ph2 = (Elf64_Phdr *)(elf_buf + eh->e_phoff + sizeof(Elf64_Phdr));
    u64 orig_ph2_va = ph2->p_vaddr;
    u64 orig_ph2_off = ph2->p_offset;
    ph2->p_vaddr = 0x401000; /* Collides with first segment */
    ph2->p_offset = 0x1000;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_SEGMENT_OVERLAP);
    ph2->p_vaddr = orig_ph2_va;
    ph2->p_offset = orig_ph2_off;
    printf("  PASS: Overlapping virtual memory extents trapped and rejected.\n");

    /* TEST 14: Entry Point Validation */
    printf("[TEST 14] Testing entry point validation...\n");
    eh->e_entry = 0x402000; /* Falls in data segment (RW, not X) */
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_ENTRY);
    eh->e_entry = 0x500000; /* Outside any loaded segment */
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_ENTRY);
    eh->e_entry = 0x401000; /* Inside RX code segment */
    assert(elf_validate(elf_buf, sz, &info) == ELF_OK);
    printf("  PASS: Entry point must reside inside executable segment.\n");

    /* TEST 15: Cumulative Memory Allocation Limit (> 64 MiB) */
    printf("[TEST 15] Testing cumulative allocation limit defense...\n");
    ph->p_memsz = 65 * 1024 * 1024ULL;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_INVALID_SEGMENT);
    ph->p_memsz = orig_memsz;
    printf("  PASS: Excessive cumulative memory request (> 64 MiB) rejected.\n");

    /* TEST 16: Zero Loadable Segments */
    printf("[TEST 16] Testing binary with zero PT_LOAD segments...\n");
    ph->p_type = PT_NOTE;
    ph2->p_type = PT_NOTE;
    assert(elf_validate(elf_buf, sz, &info) == ELF_ERR_NO_LOAD_SEGMENTS);
    ph->p_type = PT_LOAD;
    ph2->p_type = PT_LOAD;
    printf("  PASS: Executable with zero loadable segments cleanly rejected.\n");

    printf("\n=================================================================\n");
    printf("   [SUCCESS] ALL 16 ELF64 LOADER SECURITY TESTS PASSED           \n");
    printf("=================================================================\n\n");

    return 0;
}

#ifndef HOST_DLL
int main(void) {
    return run_all_elf_host_tests();
}
#endif

