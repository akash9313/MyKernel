#include <kernel/syscall.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/vmm.h>
#include <kernel/serial.h>
#include <kernel/framebuffer.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/kprintf.h>
#include <kernel/gfx.h>
#include <kernel/wm.h>
#include <kernel/input.h>
#include <kernel/net.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/mem.h>
#include <kernel/browser.h>

extern void syscall_stub_128(void);

void syscall_init(void) {
    /*
     * Register Vector 128 (0x80) as a 64-bit Interrupt Gate with DPL=3 (attributes 0xEE)
     * so that user mode code (CPL=3) can trigger it via 'int $0x80'.
     */
    idt_set_gate(128, syscall_stub_128, 0xEE);
    kprintf("[SYSCALL] Syscall entry point initialized at IDT vector 128 (0x80, DPL=3).\n");
}

bool user_ptr_valid(const void *ptr, size_t len, bool check_writable) {
    if (!ptr) {
        return false;
    }
    if (len == 0) {
        return true;
    }

    u64 start = (u64)ptr;
    u64 end = start + len;

    /* Reject 64-bit integer overflow */
    if (end < start) {
        return false;
    }

    /* Reject addresses outside user canonical space (>= 0x0000800000000000ULL) */
    if (start >= 0x0000800000000000ULL || end > 0x0000800000000000ULL) {
        return false;
    }

    /*
     * Inspect each 4 KiB page spanning [start, end - 1].
     * Every page must be present and marked with user permission (PTE_USER).
     */
    phys_addr_t pml4 = read_cr3() & CR3_ADDR_MASK;
    u64 first_page = start & ~4095ULL;
    u64 last_page = (end - 1ULL) & ~4095ULL;

    for (u64 va = first_page; va <= last_page; va += 4096ULL) {
        struct vmm_mapping info;
        if (vmm_query(pml4, va, &info) != VMM_SUCCESS) {
            return false;
        }
        if (!info.present || !info.user) {
            return false;
        }
        if (check_writable && !info.writable) {
            return false;
        }
    }

    return true;
}

s64 sys_write(int fd, const char *buf, size_t count) {
    if (fd != 1 && fd != 2) {
        return -EBADF;
    }
    if (count == 0) {
        return 0;
    }
    if (count > SYSCALL_MAX_WRITE_LEN) {
        return -EINVAL;
    }
    if (!user_ptr_valid(buf, count, false)) {
        return -EFAULT;
    }

    for (size_t i = 0; i < count; i++) {
        char c = buf[i];
        serial_putchar(c);
        fb_putchar(c);
    }

    return (s64)count;
}

s64 sys_yield(void) {
    thread_yield();
    return 0;
}

s64 sys_getpid(void) {
    process_t *proc = process_current();
    if (!proc) {
        return -ESRCH;
    }
    return (s64)proc->pid;
}

#include <kernel/vfs.h>

void sys_exit(int exit_code) {
    process_exit(exit_code);
}

static int map_vfs_error(int err) {
    switch (err) {
        case VFS_OK:                 return 0;
        case VFS_ERR_INVALID_PATH:   return -EINVAL;
        case VFS_ERR_NOT_FOUND:      return -ENOENT;
        case VFS_ERR_INVALID_HANDLE: return -EBADF;
        case VFS_ERR_IS_DIR:         return -EISDIR;
        case VFS_ERR_NOT_DIR:        return -ENOENT;
        case VFS_ERR_READ_ONLY:      return -EROFS;
        case VFS_ERR_UNSUPPORTED:    return -ENOSYS;
        case VFS_ERR_EOF:            return 0;
        case VFS_ERR_NOMEM:          return -ENOMEM;
        case VFS_ERR_INVAL:          return -EINVAL;
        case VFS_ERR_CORRUPT:        return -EFAULT;
        case VFS_ERR_BUSY:           return -EBADF;
        default:                     return -EINVAL;
    }
}

static bool copy_user_string(char *dst, const char *src_user, size_t max_len) {
    if (!src_user || !dst || max_len == 0) return false;
    for (size_t i = 0; i < max_len; i++) {
        if (!user_ptr_valid(&src_user[i], 1, false)) return false;
        char c = src_user[i];
        dst[i] = c;
        if (c == '\0') return true;
    }
    return false; /* Not null-terminated within max_len */
}

s64 sys_open(const char *path, int flags) {
    char kpath[VFS_MAX_PATH_LEN];
    if (!copy_user_string(kpath, path, sizeof(kpath))) {
        return -EFAULT;
    }

    vfs_fd_t fd = vfs_open(kpath, (u32)flags);
    if (fd < 0) {
        return (s64)map_vfs_error(fd);
    }
    return (s64)fd;
}

s64 sys_read(int fd, char *buf, size_t count) {
    if (count == 0) return 0;
    if (!user_ptr_valid(buf, count, true)) {
        return -EFAULT;
    }

    ssize_t bytes = vfs_read((vfs_fd_t)fd, buf, count);
    if (bytes < 0) {
        return (s64)map_vfs_error((int)bytes);
    }
    return (s64)bytes;
}

s64 sys_close(int fd) {
    int res = vfs_close((vfs_fd_t)fd);
    if (res != VFS_OK) {
        return (s64)map_vfs_error(res);
    }
    return 0;
}

s64 sys_stat(const char *path, void *statbuf) {
    if (!user_ptr_valid(statbuf, sizeof(vfs_stat_t), true)) {
        return -EFAULT;
    }
    char kpath[VFS_MAX_PATH_LEN];
    if (!copy_user_string(kpath, path, sizeof(kpath))) {
        return -EFAULT;
    }

    vfs_stat_t st;
    int res = vfs_stat(kpath, &st);
    if (res != VFS_OK) {
        return (s64)map_vfs_error(res);
    }

    *(vfs_stat_t *)statbuf = st;
    return 0;
}

s64 sys_readdir(const char *path, size_t index, void *direntbuf) {
    if (!user_ptr_valid(direntbuf, sizeof(vfs_dirent_t), true)) {
        return -EFAULT;
    }
    char kpath[VFS_MAX_PATH_LEN];
    if (!copy_user_string(kpath, path, sizeof(kpath))) {
        return -EFAULT;
    }

    vfs_dirent_t dent;
    int res = vfs_readdir(kpath, index, &dent);
    if (res != VFS_OK) {
        return (s64)map_vfs_error(res);
    }

    *(vfs_dirent_t *)direntbuf = dent;
    return 0;
}

s64 sys_gui_get_screen_info(gui_screen_info_t *info) {
    if (!user_ptr_valid(info, sizeof(gui_screen_info_t), true)) {
        return -EFAULT;
    }
    gfx_surface_t *bb = gfx_get_backbuffer();
    if (!bb) return -ENOSYS;

    info->width = (u32)bb->width;
    info->height = (u32)bb->height;
    info->pitch = (u32)bb->pitch_pixels;
    info->bpp = 32;
    return 0;
}

s64 sys_gui_create_window(const char *title, int x, int y, int w, int h) {
    char ktitle[64];
    if (title) {
        if (!copy_user_string(ktitle, title, sizeof(ktitle))) {
            return -EFAULT;
        }
    } else {
        ktitle[0] = '\0';
    }

    if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
        return -EINVAL;
    }

    process_t *proc = process_current();
    u32 owner_pid = proc ? proc->pid : 0;

    wm_window_t *win = wm_create_window_owner(ktitle, x, y, w, h, owner_pid);
    if (!win) return -ENOMEM;
    return (s64)win->id;
}

s64 sys_gui_destroy_window(int id) {
    wm_window_t *win = wm_find_by_id(id);
    if (!win) return -ENOENT;

    process_t *proc = process_current();
    if (proc && win->owner_pid != 0 && win->owner_pid != proc->pid) {
        return -EBADF; /* Non-owner process rejected */
    }

    wm_destroy_window(win);
    return 0;
}

s64 sys_gui_get_event(int win_id, input_event_t *ev) {
    if (!user_ptr_valid(ev, sizeof(input_event_t), true)) {
        return -EFAULT;
    }

    process_t *proc = process_current();
    if (win_id > 0) {
        wm_window_t *win = wm_find_by_id(win_id);
        if (!win) return -ENOENT;
        if (proc && win->owner_pid != 0 && win->owner_pid != proc->pid) {
            return -EBADF;
        }
        input_event_t win_ev;
        if (wm_window_pop_event(win, &win_ev)) {
            *ev = win_ev;
            return 1;
        }
        return 0;
    }

    /* Fallback for system-level event polling (win_id == 0) */
    input_event_t kernel_ev;
    if (input_pop_event(&kernel_ev)) {
        *ev = kernel_ev;
        return 1;
    }
    return 0;
}

s64 sys_gui_present(int win_id) {
    if (win_id > 0) {
        wm_window_t *win = wm_find_by_id(win_id);
        if (!win) return -ENOENT;
        process_t *proc = process_current();
        if (proc && win->owner_pid != 0 && win->owner_pid != proc->pid) {
            return -EBADF;
        }
    }
    desktop_render();
    return 0;
}

s64 sys_gui_draw_rect(int win_id, int x, int y, int w, int h, u32 color) {
    wm_window_t *win = wm_find_by_id(win_id);
    if (!win) return -ENOENT;

    process_t *proc = process_current();
    if (proc && win->owner_pid != 0 && win->owner_pid != proc->pid) {
        return -EBADF;
    }

    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        return -EINVAL;
    }

    gfx_draw_rect(&win->client_surf, x, y, w, h, color);
    return 0;
}

s64 sys_gui_fill_rect(int win_id, int x, int y, int w, int h, u32 color) {
    wm_window_t *win = wm_find_by_id(win_id);
    if (!win) return -ENOENT;

    process_t *proc = process_current();
    if (proc && win->owner_pid != 0 && win->owner_pid != proc->pid) {
        return -EBADF;
    }

    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        return -EINVAL;
    }

    gfx_fill_rect(&win->client_surf, x, y, w, h, color);
    return 0;
}

s64 sys_gui_draw_text(int win_id, int x, int y, const char *text, u32 fg, u32 bg) {
    wm_window_t *win = wm_find_by_id(win_id);
    if (!win) return -ENOENT;

    process_t *proc = process_current();
    if (proc && win->owner_pid != 0 && win->owner_pid != proc->pid) {
        return -EBADF;
    }

    char ktext[256];
    if (!copy_user_string(ktext, text, sizeof(ktext))) {
        return -EFAULT;
    }

    gfx_draw_string(&win->client_surf, x, y, ktext, fg, bg, (bg != 0));
    return 0;
}

s64 sys_gui_clear(int win_id, u32 color) {
    wm_window_t *win = wm_find_by_id(win_id);
    if (!win) return -ENOENT;

    process_t *proc = process_current();
    if (proc && win->owner_pid != 0 && win->owner_pid != proc->pid) {
        return -EBADF;
    }

    gfx_fill_rect(&win->client_surf, 0, 0, win->client_w, win->client_h, color);
    return 0;
}

s64 sys_gui_set_title(int win_id, const char *title) {
    wm_window_t *win = wm_find_by_id(win_id);
    if (!win) return -ENOENT;

    process_t *proc = process_current();
    if (proc && win->owner_pid != 0 && win->owner_pid != proc->pid) {
        return -EBADF;
    }

    char ktitle[64];
    if (!copy_user_string(ktitle, title, sizeof(ktitle))) {
        return -EFAULT;
    }

    size_t ti = 0;
    while (ktitle[ti] && ti < sizeof(win->title) - 1) {
        win->title[ti] = ktitle[ti];
        ti++;
    }
    win->title[ti] = '\0';
    return 0;
}

s64 sys_gui_draw_bitmap(int win_id, int x, int y, int w, int h, const u32 *pixels) {
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        return -EINVAL;
    }
    size_t count = (size_t)w * (size_t)h;
    if (count > 2048 * 2048) return -EINVAL;
    size_t bytes = count * sizeof(u32);
    if (!user_ptr_valid(pixels, bytes, false)) {
        return -EFAULT;
    }

    wm_window_t *win = wm_find_by_id(win_id);
    if (!win) return -ENOENT;

    process_t *proc = process_current();
    if (proc && win->owner_pid != 0 && win->owner_pid != proc->pid) {
        return -EBADF;
    }

    gfx_draw_bitmap(&win->client_surf, x, y, w, h, pixels);
    return 0;
}

s64 sys_sleep(u32 ms) {
    if (ms == 0) {
        thread_yield();
        return 0;
    }
    u64 ticks = (ms + 19) / 20;
    if (ticks == 0) ticks = 1;
    thread_sleep_ticks(ticks);
    return 0;
}

s64 sys_net_socket(void) {
    int sock = tcp_socket_create();
    if (sock < 0) return -ENOMEM;
    return (s64)sock;
}

s64 sys_net_connect(int sock_id, u32 remote_ip, u16 remote_port, u32 timeout_ms) {
    int res = tcp_connect(sock_id, (ip_addr_t)remote_ip, remote_port, timeout_ms);
    if (res < 0) return -EINVAL;
    return 0;
}

s64 sys_net_send(int sock_id, const void *buf, size_t len) {
    if (len == 0) return 0;
    if (!user_ptr_valid(buf, len, false)) return -EFAULT;
    int sent = tcp_send(sock_id, buf, len);
    if (sent < 0) return -EINVAL;
    return (s64)sent;
}

s64 sys_net_recv(int sock_id, void *buf, size_t max_len, u32 timeout_ms) {
    if (max_len == 0) return 0;
    if (!user_ptr_valid(buf, max_len, true)) return -EFAULT;
    int received = tcp_recv(sock_id, buf, max_len, timeout_ms);
    if (received < 0) return -EINVAL;
    return (s64)received;
}

s64 sys_net_close(int sock_id) {
    int res = tcp_close(sock_id);
    if (res < 0) return -EINVAL;
    return 0;
}

s64 sys_net_get_config(net_if_t *user_if) {
    if (!user_ptr_valid(user_if, sizeof(net_if_t), true)) return -EFAULT;
    net_if_t *nif = net_get_interface();
    if (!nif) return -ENODEV;
    *user_if = *nif;
    return 0;
}

s64 sys_net_resolve(const char *hostname, ip_addr_t *out_ip, u32 timeout_ms) {
    if (!user_ptr_valid(out_ip, sizeof(ip_addr_t), true)) return -EFAULT;
    char khost[128];
    if (!copy_user_string(khost, hostname, sizeof(khost))) return -EFAULT;
    ip_addr_t ip = 0;
    if (!dns_resolve(khost, &ip, timeout_ms)) {
        return -ENOENT;
    }
    *out_ip = ip;
    return 0;
}

s64 sys_net_ping(ip_addr_t dst_ip, u32 timeout_ms) {
    int rtt = icmp_ping(dst_ip, timeout_ms);
    if (rtt < 0) return -ETIMEDOUT;
    return (s64)rtt;
}

s64 sys_mmap(void *addr, size_t length, int prot, int flags) {
    if (length == 0 || length > (16 * 1024 * 1024)) {
        return -EINVAL;
    }

    /* Validate flags: must be MAP_ANONYMOUS */
    if ((flags & MAP_ANONYMOUS) == 0) {
        return -EINVAL;
    }

    /* Enforce W^X: never allow writable and executable simultaneously */
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
        return -EPERM;
    }

    process_t *proc = process_current();
    if (!proc || proc->pml4_phys == 0) {
        return -EPERM;
    }

    size_t page_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    if (proc->mapped_page_count + page_count > PROCESS_MAX_MAPPED_PAGES) {
        return -ENOMEM;
    }

    virt_addr_t va = 0;
    if (addr == NULL) {
        /* Bump allocator from proc->mmap_curr_va */
        va = proc->mmap_curr_va;
        if (va < USER_MMAP_BASE_VA || va + (page_count * PAGE_SIZE) > USER_MMAP_MAX_VA) {
            return -ENOMEM;
        }
        proc->mmap_curr_va += page_count * PAGE_SIZE;
    } else {
        va = (virt_addr_t)addr;
        if ((va & 0xFFF) != 0) {
            return -EINVAL;
        }
        if (va < USER_MMAP_BASE_VA || va + (page_count * PAGE_SIZE) > USER_MMAP_MAX_VA) {
            return -EINVAL;
        }
        /* Check if any page in the requested range is already mapped */
        for (size_t i = 0; i < page_count; i++) {
            struct vmm_mapping info;
            if (vmm_query(proc->pml4_phys, va + (i * PAGE_SIZE), &info) == VMM_SUCCESS && info.present) {
                return -EINVAL;
            }
        }
    }

    u32 vmm_flags = VMM_FLAG_READ | VMM_FLAG_USER;
    if (prot & PROT_WRITE) {
        vmm_flags |= VMM_FLAG_WRITE;
    }
    if (!(prot & PROT_EXEC)) {
        vmm_flags |= VMM_FLAG_NO_EXEC;
    }

    struct limine_hhdm_response *hhdm = boot_get_hhdm_response();
    u64 hhdm_off = hhdm ? hhdm->offset : 0;

    /* Allocate, zero, and map each 4 KiB physical page */
    size_t allocated = 0;
    for (size_t i = 0; i < page_count; i++) {
        phys_addr_t pa = 0;
        if (pmm_alloc_page(&pa) != PMM_SUCCESS) {
            /* Rollback previously mapped pages in this mmap call */
            for (size_t j = 0; j < allocated; j++) {
                virt_addr_t r_va = va + (j * PAGE_SIZE);
                vmm_unmap_page(proc->pml4_phys, r_va);
                proc->mapped_page_count--;
                pmm_free_page(proc->mapped_pas[proc->mapped_page_count]);
                proc->mapped_pas[proc->mapped_page_count] = 0;
            }
            return -ENOMEM;
        }

        if (hhdm_off) {
            kmemset((void *)(pa + hhdm_off), 0, PAGE_SIZE);
        }

        if (vmm_map_page(proc->pml4_phys, va + (i * PAGE_SIZE), pa, vmm_flags) != VMM_SUCCESS) {
            pmm_free_page(pa);
            for (size_t j = 0; j < allocated; j++) {
                virt_addr_t r_va = va + (j * PAGE_SIZE);
                vmm_unmap_page(proc->pml4_phys, r_va);
                proc->mapped_page_count--;
                pmm_free_page(proc->mapped_pas[proc->mapped_page_count]);
                proc->mapped_pas[proc->mapped_page_count] = 0;
            }
            return -ENOMEM;
        }

        proc->mapped_pas[proc->mapped_page_count++] = pa;
        allocated++;
    }

    return (s64)va;
}

s64 sys_munmap(void *addr, size_t length) {
    if (!addr || length == 0) {
        return -EINVAL;
    }
    virt_addr_t va = (virt_addr_t)addr;
    if ((va & 0xFFF) != 0) {
        return -EINVAL;
    }

    size_t page_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    if (va < USER_MMAP_BASE_VA || va + (page_count * PAGE_SIZE) > USER_MMAP_MAX_VA) {
        return -EINVAL;
    }

    process_t *proc = process_current();
    if (!proc || proc->pml4_phys == 0) {
        return -EPERM;
    }

    for (size_t i = 0; i < page_count; i++) {
        virt_addr_t pva = va + (i * PAGE_SIZE);
        struct vmm_mapping info;
        if (vmm_query(proc->pml4_phys, pva, &info) == VMM_SUCCESS && info.present) {
            vmm_unmap_page(proc->pml4_phys, pva);
            invlpg(pva);
            if (info.paddr != 0) {
                pmm_free_page(info.paddr);
                /* Remove from tracked pages list */
                for (size_t m = 0; m < proc->mapped_page_count; m++) {
                    if (proc->mapped_pas[m] == info.paddr) {
                        proc->mapped_pas[m] = proc->mapped_pas[proc->mapped_page_count - 1];
                        proc->mapped_pas[proc->mapped_page_count - 1] = 0;
                        proc->mapped_page_count--;
                        break;
                    }
                }
            }
        }
    }

    return 0;
}

s64 sys_net_fetch(const char *url_user, char *buf_user, size_t max_len, int *status_user) {
    if (!url_user || !buf_user || max_len < 32) {
        return -EINVAL;
    }
    if (!user_ptr_valid(buf_user, max_len, true)) {
        return -EFAULT;
    }
    if (status_user && !user_ptr_valid(status_user, sizeof(int), true)) {
        return -EFAULT;
    }

    char kurl[256];
    if (!copy_user_string(kurl, url_user, sizeof(kurl))) {
        return -EFAULT;
    }

    browser_url_t url;
    if (!browser_url_parse(kurl, &url)) {
        if (status_user) *status_user = 400;
        return -EINVAL;
    }

    /* Bounded kernel temporary buffer for fetch */
    static char fetch_buf[BROWSER_HTML_BUFFER_MAX];
    int kstatus = 0;
    int nbytes = browser_fetch_url(&url, fetch_buf, sizeof(fetch_buf), &kstatus);
    if (nbytes < 0) {
        if (status_user) *status_user = (kstatus ? kstatus : 500);
        return -EIO;
    }

    size_t copy_len = (size_t)nbytes;
    if (copy_len >= max_len) {
        copy_len = max_len - 1;
    }
    kmemcpy(buf_user, fetch_buf, copy_len);
    buf_user[copy_len] = '\0';

    if (status_user) {
        *status_user = kstatus;
    }
    return (s64)copy_len;
}

void syscall_dispatcher(struct interrupt_frame *frame) {
    u64 syscall_nr = frame->rax;

    switch (syscall_nr) {
        case SYS_WRITE:
            frame->rax = (u64)sys_write((int)frame->rdi, (const char *)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_YIELD:
            frame->rax = (u64)sys_yield();
            break;
        case SYS_GETPID:
            frame->rax = (u64)sys_getpid();
            break;
        case SYS_EXIT:
            sys_exit((int)frame->rdi);
            break;
        case SYS_OPEN:
            frame->rax = (u64)sys_open((const char *)frame->rdi, (int)frame->rsi);
            break;
        case SYS_READ:
            frame->rax = (u64)sys_read((int)frame->rdi, (char *)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_CLOSE:
            frame->rax = (u64)sys_close((int)frame->rdi);
            break;
        case SYS_STAT:
            frame->rax = (u64)sys_stat((const char *)frame->rdi, (void *)frame->rsi);
            break;
        case SYS_READDIR:
            frame->rax = (u64)sys_readdir((const char *)frame->rdi, (size_t)frame->rsi, (void *)frame->rdx);
            break;
        case SYS_GUI_GET_SCREEN_INFO:
            frame->rax = (u64)sys_gui_get_screen_info((gui_screen_info_t *)frame->rdi);
            break;
        case SYS_GUI_CREATE_WINDOW:
            frame->rax = (u64)sys_gui_create_window((const char *)frame->rdi, (int)frame->rsi, (int)frame->rdx, (int)frame->r10, (int)frame->r8);
            break;
        case SYS_GUI_DESTROY_WINDOW:
            frame->rax = (u64)sys_gui_destroy_window((int)frame->rdi);
            break;
        case SYS_GUI_GET_EVENT:
            frame->rax = (u64)sys_gui_get_event((int)frame->rdi, (input_event_t *)frame->rsi);
            break;
        case SYS_GUI_PRESENT:
            frame->rax = (u64)sys_gui_present((int)frame->rdi);
            break;
        case SYS_GUI_DRAW_RECT:
            frame->rax = (u64)sys_gui_draw_rect((int)frame->rdi, (int)frame->rsi, (int)frame->rdx, (int)frame->r10, (int)frame->r8, (u32)frame->r9);
            break;
        case SYS_GUI_FILL_RECT:
            frame->rax = (u64)sys_gui_fill_rect((int)frame->rdi, (int)frame->rsi, (int)frame->rdx, (int)frame->r10, (int)frame->r8, (u32)frame->r9);
            break;
        case SYS_GUI_DRAW_TEXT:
            frame->rax = (u64)sys_gui_draw_text((int)frame->rdi, (int)frame->rsi, (int)frame->rdx, (const char *)frame->r10, (u32)frame->r8, (u32)frame->r9);
            break;
        case SYS_GUI_CLEAR:
            frame->rax = (u64)sys_gui_clear((int)frame->rdi, (u32)frame->rsi);
            break;
        case SYS_GUI_SET_TITLE:
            frame->rax = (u64)sys_gui_set_title((int)frame->rdi, (const char *)frame->rsi);
            break;
        case SYS_SLEEP:
            frame->rax = (u64)sys_sleep((u32)frame->rdi);
            break;
        case SYS_NET_SOCKET:
            frame->rax = (u64)sys_net_socket();
            break;
        case SYS_NET_CONNECT:
            frame->rax = (u64)sys_net_connect((int)frame->rdi, (u32)frame->rsi, (u16)frame->rdx, (u32)frame->r10);
            break;
        case SYS_NET_SEND:
            frame->rax = (u64)sys_net_send((int)frame->rdi, (const void *)frame->rsi, (size_t)frame->rdx);
            break;
        case SYS_NET_RECV:
            frame->rax = (u64)sys_net_recv((int)frame->rdi, (void *)frame->rsi, (size_t)frame->rdx, (u32)frame->r10);
            break;
        case SYS_NET_CLOSE:
            frame->rax = (u64)sys_net_close((int)frame->rdi);
            break;
        case SYS_NET_GET_CONFIG:
            frame->rax = (u64)sys_net_get_config((net_if_t *)frame->rdi);
            break;
        case SYS_NET_RESOLVE:
            frame->rax = (u64)sys_net_resolve((const char *)frame->rdi, (ip_addr_t *)frame->rsi, (u32)frame->rdx);
            break;
        case SYS_NET_PING:
            frame->rax = (u64)sys_net_ping((ip_addr_t)frame->rdi, (u32)frame->rsi);
            break;
        case SYS_MMAP:
            frame->rax = (u64)sys_mmap((void *)frame->rdi, (size_t)frame->rsi, (int)frame->rdx, (int)frame->r10);
            break;
        case SYS_MUNMAP:
            frame->rax = (u64)sys_munmap((void *)frame->rdi, (size_t)frame->rsi);
            break;
        case SYS_NET_FETCH:
            frame->rax = (u64)sys_net_fetch((const char *)frame->rdi, (char *)frame->rsi, (size_t)frame->rdx, (int *)frame->r10);
            break;
        case SYS_GUI_DRAW_BITMAP:
            frame->rax = (u64)sys_gui_draw_bitmap((int)frame->rdi, (int)frame->rsi, (int)frame->rdx, (int)frame->r10, (int)frame->r8, (const u32 *)frame->r9);
            break;
        default:
            frame->rax = (u64)-ENOSYS;
            break;
    }
}
