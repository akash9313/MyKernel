#ifndef ASTRA_KERNEL_ELF_H
#define ASTRA_KERNEL_ELF_H

#include <kernel/types.h>

/* Standard ELF Types */
typedef u64 Elf64_Addr;
typedef u64 Elf64_Off;
typedef u16 Elf64_Half;
typedef u32 Elf64_Word;
typedef s32 Elf64_Sword;
typedef u64 Elf64_Xword;
typedef s64 Elf64_Sxword;

#define EI_NIDENT 16

/* e_ident[] indexes */
#define EI_MAG0        0
#define EI_MAG1        1
#define EI_MAG2        2
#define EI_MAG3        3
#define EI_CLASS       4
#define EI_DATA        5
#define EI_VERSION     6
#define EI_OSABI       7
#define EI_ABIVERSION  8
#define EI_PAD         9

/* ELF Magic */
#define ELFMAG0        0x7f
#define ELFMAG1        'E'
#define ELFMAG2        'L'
#define ELFMAG3        'F'

/* EI_CLASS values */
#define ELFCLASSNONE   0
#define ELFCLASS32     1
#define ELFCLASS64     2

/* EI_DATA values */
#define ELFDATANONE    0
#define ELFDATA2LSB    1
#define ELFDATA2MSB    2

/* e_type values */
#define ET_NONE        0
#define ET_REL         1
#define ET_EXEC        2
#define ET_DYN         3
#define ET_CORE        4

/* e_machine values */
#define EM_NONE        0
#define EM_386         3
#define EM_X86_64      62

/* Version */
#define EV_NONE        0
#define EV_CURRENT     1

/* Program Header Types (p_type) */
#define PT_NULL        0
#define PT_LOAD        1
#define PT_DYNAMIC     2
#define PT_INTERP      3
#define PT_NOTE        4
#define PT_SHLIB       5
#define PT_PHDR        6
#define PT_TLS         7
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK   0x6474e551
#define PT_GNU_RELRO   0x6474e552

/* Program Header Flags (p_flags) */
#define PF_X           (1U << 0)
#define PF_W           (1U << 1)
#define PF_R           (1U << 2)

/* ELF64 File Header */
typedef struct {
    u8         e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff;
    Elf64_Off  e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

/* ELF64 Program Header */
typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} __attribute__((packed)) Elf64_Phdr;

/* Status and Error Codes for ELF Loader */
#define ELF_OK                          0
#define ELF_ERR_NULL_PTR               -1
#define ELF_ERR_TOO_SMALL              -2
#define ELF_ERR_INVALID_MAGIC          -3
#define ELF_ERR_UNSUPPORTED_CLASS      -4
#define ELF_ERR_UNSUPPORTED_ENDIAN     -5
#define ELF_ERR_UNSUPPORTED_MACHINE    -6
#define ELF_ERR_UNSUPPORTED_TYPE       -7
#define ELF_ERR_HEADER_OVERFLOW        -8
#define ELF_ERR_INVALID_PHDR           -9
#define ELF_ERR_INVALID_SEGMENT        -10
#define ELF_ERR_INVALID_ADDRESS        -11
#define ELF_ERR_INVALID_ENTRY          -12
#define ELF_ERR_DYNAMIC_NOT_SUPPORTED  -13
#define ELF_ERR_WX_CONFLICT            -14
#define ELF_ERR_SEGMENT_OVERLAP        -15
#define ELF_ERR_ALIGNMENT              -16
#define ELF_ERR_NO_LOAD_SEGMENTS       -17
#define ELF_ERR_NOMEM                  -18

/* Summary Information from Validated ELF */
typedef struct {
    virt_addr_t entry_point;
    size_t      load_segment_count;
    virt_addr_t min_vaddr;
    virt_addr_t max_vaddr;
} elf_info_t;

/* Generic Bounded Reader Interface for ELF Loading */
struct elf_reader;
typedef struct elf_reader {
    void *context;
    ssize_t (*read_at)(struct elf_reader *reader, void *buf, size_t count, size_t offset);
    size_t (*get_size)(struct elf_reader *reader);
} elf_reader_t;

typedef struct {
    const u8 *base;
    size_t    size;
} elf_mem_context_t;

void elf_reader_init_memory(elf_reader_t *reader, elf_mem_context_t *ctx, const void *buf, size_t size);

/* Public API */
int elf_validate_reader(elf_reader_t *reader, elf_info_t *out_info);
int elf_validate(const void *image, size_t size, elf_info_t *out_info);
const char *elf_strerror(int err);

#endif /* ASTRA_KERNEL_ELF_H */
