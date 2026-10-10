#ifndef ASTRA_KERNEL_MEM_H
#define ASTRA_KERNEL_MEM_H

#include <kernel/types.h>
#include <limine.h>

void memmap_init(struct limine_memmap_response *mmap);
void memmap_print(void);
u64 memmap_get_total_usable(void);
u64 memmap_get_total_memory(void);

#endif /* ASTRA_KERNEL_MEM_H */
