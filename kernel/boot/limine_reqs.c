#include <limine.h>
#include <kernel/types.h>

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_kernel_file_request kernel_file_request = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

struct limine_framebuffer_response *boot_get_framebuffer_response(void) {
    return framebuffer_request.response;
}

struct limine_memmap_response *boot_get_memmap_response(void) {
    return memmap_request.response;
}

struct limine_hhdm_response *boot_get_hhdm_response(void) {
    return hhdm_request.response;
}

struct limine_kernel_address_response *boot_get_kernel_address_response(void) {
    return kernel_address_request.response;
}

struct limine_module_response *boot_get_module_response(void) {
    return module_request.response;
}

const char *boot_get_cmdline(void) {
    if (kernel_file_request.response && kernel_file_request.response->kernel_file) {
        return kernel_file_request.response->kernel_file->cmdline;
    }
    return NULL;
}

bool boot_is_base_revision_supported(void) {
    return LIMINE_BASE_REVISION_SUPPORTED;
}
