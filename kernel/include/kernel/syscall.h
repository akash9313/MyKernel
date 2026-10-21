#ifndef ASTRA_KERNEL_SYSCALL_H
#define ASTRA_KERNEL_SYSCALL_H

#include <kernel/types.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/input.h>

/* System Call Numbers */
#define SYS_WRITE               1
#define SYS_YIELD               2
#define SYS_GETPID              3
#define SYS_EXIT                4
#define SYS_OPEN                5
#define SYS_READ                6
#define SYS_CLOSE               7
#define SYS_STAT                8
#define SYS_READDIR             9
#define SYS_GUI_GET_SCREEN_INFO 10
#define SYS_GUI_CREATE_WINDOW   11
#define SYS_GUI_DESTROY_WINDOW  12
#define SYS_GUI_GET_EVENT       13
#define SYS_GUI_PRESENT         14
#define SYS_GUI_DRAW_RECT       15
#define SYS_GUI_FILL_RECT       16
#define SYS_GUI_DRAW_TEXT       17
#define SYS_GUI_CLEAR           18
#define SYS_GUI_SET_TITLE       19
#define SYS_SLEEP               20
#define SYS_NET_SOCKET          21
#define SYS_NET_CONNECT         22
#define SYS_NET_SEND            23
#define SYS_NET_RECV            24
#define SYS_NET_CLOSE           25
#define SYS_NET_GET_CONFIG      26
#define SYS_NET_RESOLVE         27
#define SYS_NET_PING            28
#define SYS_MMAP                29
#define SYS_MUNMAP              30
#define SYS_NET_FETCH           31
#define SYS_GUI_DRAW_BITMAP     32

/* mmap protection flags */
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

/* mmap flags */
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
#define MAP_FIXED       0x10
#define MAP_FAILED      ((void *)-1)

typedef struct gui_screen_info {
    u32 width;
    u32 height;
    u32 pitch;
    u32 bpp;
} gui_screen_info_t;

/* Standard System Call Error Codes */
#define EPERM       1
#define ENOENT      2
#define ESRCH       3
#define EIO         5
#define EBADF       9
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define ENODEV      19
#define EISDIR      21
#define EINVAL      22
#define EROFS       30
#define ENOSYS      38
#define ETIMEDOUT   110

/* Maximum length for a single sys_write call */
#define SYSCALL_MAX_WRITE_LEN 4096

/* Initialize Syscall subsystem (registers IDT vector 128 / 0x80) */
void syscall_init(void);

/* Syscall dispatcher called from assembly stub (vector 128) */
void syscall_dispatcher(struct interrupt_frame *frame);

/* Safe User Pointer Validation */
bool user_ptr_valid(const void *ptr, size_t len, bool check_writable);

/* Syscall Implementations */
s64 sys_write(int fd, const char *buf, size_t count);
s64 sys_yield(void);
s64 sys_getpid(void);
void sys_exit(int exit_code) __attribute__((noreturn));
s64 sys_open(const char *path, int flags);
s64 sys_read(int fd, char *buf, size_t count);
s64 sys_close(int fd);
s64 sys_stat(const char *path, void *statbuf);
s64 sys_readdir(const char *path, size_t index, void *direntbuf);
s64 sys_gui_get_screen_info(gui_screen_info_t *info);
s64 sys_gui_create_window(const char *title, int x, int y, int w, int h);
s64 sys_gui_destroy_window(int id);
s64 sys_gui_get_event(int win_id, input_event_t *ev);
s64 sys_gui_present(int win_id);
s64 sys_gui_draw_rect(int win_id, int x, int y, int w, int h, u32 color);
s64 sys_gui_fill_rect(int win_id, int x, int y, int w, int h, u32 color);
s64 sys_gui_draw_text(int win_id, int x, int y, const char *text, u32 fg, u32 bg);
s64 sys_gui_clear(int win_id, u32 color);
s64 sys_gui_set_title(int win_id, const char *title);
s64 sys_sleep(u32 ms);
s64 sys_mmap(void *addr, size_t length, int prot, int flags);
s64 sys_munmap(void *addr, size_t length);
s64 sys_net_fetch(const char *url, char *buf, size_t max_len, int *out_status);
s64 sys_gui_draw_bitmap(int win_id, int x, int y, int w, int h, const u32 *pixels);

#endif /* ASTRA_KERNEL_SYSCALL_H */
