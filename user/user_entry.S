.code64
.section .text
.global _start
.type _start, @function

_start:
    /* Clear base pointer to mark outermost stack frame */
    xorq %rbp, %rbp

    /* Align stack to 16 bytes per System V ABI */
    andq $-16, %rsp

    /* Invoke main() */
    call main

    /* Call exit syscall with return value of main in %rdi */
    movq %rax, %rdi
    movq $4, %rax               /* SYS_EXIT = 4 */
    int $0x80

    /* Halt if exit fails */
1:  hlt
    jmp 1b

.global sys_write
.type sys_write, @function
/* long sys_write(int fd, const void *buf, size_t count); */
sys_write:
    movq $1, %rax               /* SYS_WRITE = 1 */
    int $0x80
    retq

.global sys_yield
.type sys_yield, @function
/* long sys_yield(void); */
sys_yield:
    movq $2, %rax               /* SYS_YIELD = 2 */
    int $0x80
    retq

.global sys_getpid
.type sys_getpid, @function
/* long sys_getpid(void); */
sys_getpid:
    movq $3, %rax               /* SYS_GETPID = 3 */
    int $0x80
    retq

.global sys_exit
.type sys_exit, @function
/* void sys_exit(int status) __attribute__((noreturn)); */
sys_exit:
    movq $4, %rax               /* SYS_EXIT = 4 */
    int $0x80
2:  hlt
    jmp 2b

.global sys_net_socket
.type sys_net_socket, @function
/* long sys_net_socket(void); */
sys_net_socket:
    movq $21, %rax
    int $0x80
    retq

.global sys_net_connect
.type sys_net_connect, @function
/* long sys_net_connect(int sock, unsigned int ip, unsigned short port, unsigned int timeout_ms); */
sys_net_connect:
    movq %rcx, %r10
    movq $22, %rax
    int $0x80
    retq

.global sys_net_send
.type sys_net_send, @function
/* long sys_net_send(int sock, const void *buf, size_t len); */
sys_net_send:
    movq $23, %rax
    int $0x80
    retq

.global sys_net_recv
.type sys_net_recv, @function
/* long sys_net_recv(int sock, void *buf, size_t max_len, unsigned int timeout_ms); */
sys_net_recv:
    movq %rcx, %r10
    movq $24, %rax
    int $0x80
    retq

.global sys_net_close
.type sys_net_close, @function
/* long sys_net_close(int sock); */
sys_net_close:
    movq $25, %rax
    int $0x80
    retq

.global sys_net_get_config
.type sys_net_get_config, @function
/* long sys_net_get_config(void *net_if); */
sys_net_get_config:
    movq $26, %rax
    int $0x80
    retq

.global sys_net_resolve
.type sys_net_resolve, @function
/* long sys_net_resolve(const char *hostname, unsigned int *out_ip, unsigned int timeout_ms); */
sys_net_resolve:
    movq $27, %rax
    int $0x80
    retq

.global sys_net_ping
.type sys_net_ping, @function
/* long sys_net_ping(unsigned int ip, unsigned int timeout_ms); */
sys_net_ping:
    movq $28, %rax
    int $0x80
    retq

.global sys_gui_get_screen_info
.type sys_gui_get_screen_info, @function
/* long sys_gui_get_screen_info(void *info); */
sys_gui_get_screen_info:
    movq $10, %rax
    int $0x80
    retq

.global sys_gui_create_window
.type sys_gui_create_window, @function
/* long sys_gui_create_window(const char *title, int x, int y, int w, int h); */
sys_gui_create_window:
    movq %rcx, %r10
    movq $11, %rax
    int $0x80
    retq

.global sys_gui_destroy_window
.type sys_gui_destroy_window, @function
/* long sys_gui_destroy_window(int id); */
sys_gui_destroy_window:
    movq $12, %rax
    int $0x80
    retq

.global sys_gui_get_event
.type sys_gui_get_event, @function
/* long sys_gui_get_event(int win_id, void *ev); */
sys_gui_get_event:
    movq $13, %rax
    int $0x80
    retq

.global sys_gui_present
.type sys_gui_present, @function
/* long sys_gui_present(int win_id); */
sys_gui_present:
    movq $14, %rax
    int $0x80
    retq

.global sys_gui_draw_rect
.type sys_gui_draw_rect, @function
/* long sys_gui_draw_rect(int win_id, int x, int y, int w, int h, unsigned int color); */
sys_gui_draw_rect:
    movq %rcx, %r10
    movq $15, %rax
    int $0x80
    retq

.global sys_gui_fill_rect
.type sys_gui_fill_rect, @function
/* long sys_gui_fill_rect(int win_id, int x, int y, int w, int h, unsigned int color); */
sys_gui_fill_rect:
    movq %rcx, %r10
    movq $16, %rax
    int $0x80
    retq

.global sys_gui_draw_text
.type sys_gui_draw_text, @function
/* long sys_gui_draw_text(int win_id, int x, int y, const char *text, unsigned int fg, unsigned int bg); */
sys_gui_draw_text:
    movq %rcx, %r10
    movq $17, %rax
    int $0x80
    retq

.global sys_gui_clear
.type sys_gui_clear, @function
/* long sys_gui_clear(int win_id, unsigned int color); */
sys_gui_clear:
    movq $18, %rax
    int $0x80
    retq

.global sys_gui_set_title
.type sys_gui_set_title, @function
/* long sys_gui_set_title(int win_id, const char *title); */
sys_gui_set_title:
    movq $19, %rax
    int $0x80
    retq

.global sys_sleep
.type sys_sleep, @function
/* long sys_sleep(unsigned int ms); */
sys_sleep:
    movq $20, %rax
    int $0x80
    retq

.global sys_open
.type sys_open, @function
/* long sys_open(const char *path, int flags); */
sys_open:
    movq $5, %rax
    int $0x80
    retq

.global sys_read
.type sys_read, @function
/* long sys_read(int fd, void *buf, size_t count); */
sys_read:
    movq $6, %rax
    int $0x80
    retq

.global sys_close
.type sys_close, @function
/* long sys_close(int fd); */
sys_close:
    movq $7, %rax
    int $0x80
    retq

.global sys_stat
.type sys_stat, @function
/* long sys_stat(const char *path, void *statbuf); */
sys_stat:
    movq $8, %rax
    int $0x80
    retq

.global sys_mmap
.type sys_mmap, @function
/* void *sys_mmap(void *addr, size_t length, int prot, int flags); */
sys_mmap:
    movq %rcx, %r10
    movq $29, %rax
    int $0x80
    retq

.global sys_munmap
.type sys_munmap, @function
/* long sys_munmap(void *addr, size_t length); */
sys_munmap:
    movq $30, %rax
    int $0x80
    retq

.global sys_net_fetch
.type sys_net_fetch, @function
/* long sys_net_fetch(const char *url, char *buf, size_t max_len, int *out_status); */
sys_net_fetch:
    movq %rcx, %r10
    movq $31, %rax
    int $0x80
    retq

.global sys_gui_draw_bitmap
.type sys_gui_draw_bitmap, @function
/* long sys_gui_draw_bitmap(int win_id, int x, int y, int w, int h, const unsigned int *pixels); */
sys_gui_draw_bitmap:
    movq %rcx, %r10
    movq $32, %rax
    int $0x80
    retq

.global memcpy
.type memcpy, @function
memcpy:
    movq %rdi, %rax
    movq %rdx, %rcx
    rep movsb
    retq

.global memset
.type memset, @function
memset:
    movq %rdi, %r8
    movb %sil, %al
    movq %rdx, %rcx
    rep stosb
    movq %r8, %rax
    retq



