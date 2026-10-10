#ifndef ASTRA_KERNEL_BOOT_H
#define ASTRA_KERNEL_BOOT_H

#include <limine.h>
#include <kernel/types.h>

struct limine_framebuffer_response *boot_get_framebuffer_response(void);
struct limine_memmap_response *boot_get_memmap_response(void);
struct limine_hhdm_response *boot_get_hhdm_response(void);
const char *boot_get_cmdline(void);
bool boot_is_base_revision_supported(void);

#endif /* ASTRA_KERNEL_BOOT_H */
