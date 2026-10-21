#include <kernel/elf.h>
#include <kernel/vmm.h>
#include <kernel/kprintf.h>

const char *elf_strerror(int err) {
    switch (err) {
        case ELF_OK:                         return "Success";
        case ELF_ERR_NULL_PTR:               return "NULL pointer provided";
        case ELF_ERR_TOO_SMALL:              return "File size too small for ELF header";
        case ELF_ERR_INVALID_MAGIC:          return "Invalid ELF magic signature";
        case ELF_ERR_UNSUPPORTED_CLASS:      return "Unsupported ELF class (only 64-bit supported)";
        case ELF_ERR_UNSUPPORTED_ENDIAN:     return "Unsupported endianness (only little-endian supported)";
        case ELF_ERR_UNSUPPORTED_MACHINE:    return "Unsupported machine architecture (only x86-64 supported)";
        case ELF_ERR_UNSUPPORTED_TYPE:       return "Unsupported ELF type (only ET_EXEC static executables supported)";
        case ELF_ERR_HEADER_OVERFLOW:        return "Header table offset or size arithmetic overflow";
        case ELF_ERR_INVALID_PHDR:           return "Program header table exceeds file bounds";
        case ELF_ERR_INVALID_SEGMENT:        return "Invalid segment bounds (p_filesz > p_memsz or out of bounds)";
        case ELF_ERR_INVALID_ADDRESS:        return "Segment virtual address outside valid user canonical space";
        case ELF_ERR_INVALID_ENTRY:          return "Entry point not within an executable PT_LOAD segment";
        case ELF_ERR_DYNAMIC_NOT_SUPPORTED:  return "Dynamic linking not supported (PT_INTERP / PT_DYNAMIC rejected)";
        case ELF_ERR_WX_CONFLICT:            return "W^X violation: Segment cannot be both writable and executable";
        case ELF_ERR_SEGMENT_OVERLAP:        return "Conflicting overlapping PT_LOAD segments detected";
        case ELF_ERR_ALIGNMENT:              return "Segment alignment constraint violated (p_vaddr % 4096 != p_offset % 4096)";
        case ELF_ERR_NO_LOAD_SEGMENTS:       return "No loadable PT_LOAD segments found";
        case ELF_ERR_NOMEM:                  return "Out of memory during ELF loading";
        default:                             return "Unknown ELF error";
    }
}

static ssize_t mem_read_at(elf_reader_t *reader, void *buf, size_t count, size_t offset) {
    elf_mem_context_t *ctx = (elf_mem_context_t *)reader->context;
    if (offset >= ctx->size) return 0;
    size_t avail = ctx->size - offset;
    size_t to_read = (count < avail) ? count : avail;
    const u8 *src = ctx->base + offset;
    u8 *dst = (u8 *)buf;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = src[i];
    }
    return (ssize_t)to_read;
}

static size_t mem_get_size(elf_reader_t *reader) {
    elf_mem_context_t *ctx = (elf_mem_context_t *)reader->context;
    return ctx->size;
}

void elf_reader_init_memory(elf_reader_t *reader, elf_mem_context_t *ctx, const void *buf, size_t size) {
    ctx->base = (const u8 *)buf;
    ctx->size = size;
    reader->context = ctx;
    reader->read_at = mem_read_at;
    reader->get_size = mem_get_size;
}

int elf_validate_reader(elf_reader_t *reader, elf_info_t *out_info) {
    if (!reader || !reader->read_at || !reader->get_size) {
        return ELF_ERR_NULL_PTR;
    }
    size_t size = reader->get_size(reader);
    if (size < sizeof(Elf64_Ehdr)) {
        return ELF_ERR_TOO_SMALL;
    }

    Elf64_Ehdr ehdr;
    if (reader->read_at(reader, &ehdr, sizeof(Elf64_Ehdr), 0) != sizeof(Elf64_Ehdr)) {
        return ELF_ERR_TOO_SMALL;
    }

    /* 1. Magic check */
    if (ehdr.e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr.e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr.e_ident[EI_MAG3] != ELFMAG3) {
        return ELF_ERR_INVALID_MAGIC;
    }

    /* 2. Class: 64-bit only */
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        return ELF_ERR_UNSUPPORTED_CLASS;
    }

    /* 3. Data: Little-Endian only */
    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        return ELF_ERR_UNSUPPORTED_ENDIAN;
    }

    /* 4. Version check */
    if (ehdr.e_ident[EI_VERSION] != EV_CURRENT || ehdr.e_version != EV_CURRENT) {
        return ELF_ERR_INVALID_MAGIC;
    }

    /* 5. Executable type: Static executables only */
    if (ehdr.e_type != ET_EXEC) {
        return ELF_ERR_UNSUPPORTED_TYPE;
    }

    /* 6. Machine: x86-64 only */
    if (ehdr.e_machine != EM_X86_64) {
        return ELF_ERR_UNSUPPORTED_MACHINE;
    }

    /* 7. Header sizes */
    if (ehdr.e_ehsize < sizeof(Elf64_Ehdr) || ehdr.e_phentsize < sizeof(Elf64_Phdr)) {
        return ELF_ERR_TOO_SMALL;
    }

    if (ehdr.e_phnum == 0 || ehdr.e_phnum > 64) {
        return ELF_ERR_INVALID_PHDR;
    }

    /* 8. Program header table bounds and overflow check */
    u64 ph_table_bytes = (u64)ehdr.e_phnum * (u64)ehdr.e_phentsize;
    if (ehdr.e_phoff + ph_table_bytes < ehdr.e_phoff || ehdr.e_phoff + ph_table_bytes > size) {
        return ELF_ERR_INVALID_PHDR;
    }

    size_t load_count = 0;
    virt_addr_t min_va = 0xFFFFFFFFFFFFFFFFULL;
    virt_addr_t max_va = 0;
    bool entry_in_exec_segment = false;

    /* Struct to record segments for overlap validation */
    struct {
        u64 start_va;
        u64 end_va;
    } loaded_segs[64];

    for (Elf64_Half i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr phdr;
        size_t ph_off = ehdr.e_phoff + (i * ehdr.e_phentsize);
        if (reader->read_at(reader, &phdr, sizeof(Elf64_Phdr), ph_off) != sizeof(Elf64_Phdr)) {
            return ELF_ERR_INVALID_PHDR;
        }

        /* Reject dynamic linking headers */
        if (phdr.p_type == PT_DYNAMIC || phdr.p_type == PT_INTERP) {
            return ELF_ERR_DYNAMIC_NOT_SUPPORTED;
        }

        if (phdr.p_type != PT_LOAD) {
            continue;
        }

        /* Enforce p_filesz <= p_memsz */
        if (phdr.p_filesz > phdr.p_memsz) {
            return ELF_ERR_INVALID_SEGMENT;
        }

        /* Check file offset and size within image bounds */
        if (phdr.p_offset + phdr.p_filesz < phdr.p_offset || phdr.p_offset + phdr.p_filesz > size) {
            return ELF_ERR_INVALID_SEGMENT;
        }

        /* Virtual address validation: Must reside in lower canonical user space */
        if (phdr.p_vaddr < 0x00200000ULL) {
            return ELF_ERR_INVALID_ADDRESS; /* Protect null page and lower 2 MiB */
        }
        if (phdr.p_vaddr + phdr.p_memsz < phdr.p_vaddr) {
            return ELF_ERR_INVALID_ADDRESS; /* Unsigned overflow */
        }
        if (phdr.p_vaddr + phdr.p_memsz > 0x0000700000000000ULL) {
            return ELF_ERR_INVALID_ADDRESS; /* Reserved for user stack and guard */
        }

        /* Alignment check */
        if ((phdr.p_vaddr & 0xFFFULL) != (phdr.p_offset & 0xFFFULL)) {
            return ELF_ERR_ALIGNMENT;
        }

        /* Strict W^X enforcement */
        if ((phdr.p_flags & PF_W) && (phdr.p_flags & PF_X)) {
            return ELF_ERR_WX_CONFLICT;
        }

        /* Overlap check with previous loadable segments */
        u64 page_start = phdr.p_vaddr & ~0xFFFULL;
        u64 page_end = (phdr.p_vaddr + phdr.p_memsz + 0xFFFULL) & ~0xFFFULL;

        for (size_t s = 0; s < load_count; s++) {
            if (page_start < loaded_segs[s].end_va && page_end > loaded_segs[s].start_va) {
                return ELF_ERR_SEGMENT_OVERLAP;
            }
        }

        loaded_segs[load_count].start_va = page_start;
        loaded_segs[load_count].end_va = page_end;
        load_count++;

        if (page_start < min_va) min_va = page_start;
        if (page_end > max_va) max_va = page_end;

        /* Check if entry point falls within this segment and segment is executable */
        if (ehdr.e_entry >= phdr.p_vaddr && ehdr.e_entry < phdr.p_vaddr + phdr.p_memsz) {
            if (phdr.p_flags & PF_X) {
                entry_in_exec_segment = true;
            }
        }
    }

    if (load_count == 0) {
        return ELF_ERR_NO_LOAD_SEGMENTS;
    }

    if (!entry_in_exec_segment) {
        return ELF_ERR_INVALID_ENTRY;
    }

    if (out_info) {
        out_info->entry_point = ehdr.e_entry;
        out_info->load_segment_count = load_count;
        out_info->min_vaddr = min_va;
        out_info->max_vaddr = max_va;
    }

    return ELF_OK;
}

int elf_validate(const void *image, size_t size, elf_info_t *out_info) {
    if (!image) {
        return ELF_ERR_NULL_PTR;
    }
    elf_reader_t reader;
    elf_mem_context_t ctx;
    elf_reader_init_memory(&reader, &ctx, image, size);
    return elf_validate_reader(&reader, out_info);
}
