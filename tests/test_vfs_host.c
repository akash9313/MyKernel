#ifdef HOST_TEST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <setjmp.h>

#include <stdarg.h>

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

void kernel_panic(const char *file, int line, const char *fmt, ...) {
    (void)file;
    (void)line;
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    abort();
}

#include "../kernel/include/kernel/vfs.h"
#include "../kernel/include/kernel/initramfs.h"
#include "../kernel/include/kernel/astrafs.h"

/* AstraFS stubs for VFS host tests (unmounted state) */
bool astrafs_is_mounted(void) { return false; }
vfs_node_t *astrafs_get_root_node(void) { return NULL; }
const vfs_ops_t *astrafs_get_ops(void) { return NULL; }
int astrafs_lookup(const char *name, vfs_node_t **out_node) { (void)name; (void)out_node; return VFS_ERR_NOT_FOUND; }
int astrafs_create(const char *name, u32 flags, vfs_node_t **out_node) { (void)name; (void)flags; (void)out_node; return VFS_ERR_UNSUPPORTED; }
int astrafs_truncate(vfs_node_t *node, size_t new_size) { (void)node; (void)new_size; return VFS_ERR_UNSUPPORTED; }
int astrafs_sync(void) { return VFS_OK; }
int astrafs_rename(const char *old_name, const char *new_name) { (void)old_name; (void)new_name; return VFS_ERR_UNSUPPORTED; }
int block_get_device_count(void) { return 0; }
block_device_t *block_get_device(int idx) { (void)idx; return NULL; }

/* Include implementations directly for host test translation unit */
#include "../kernel/fs/vfs.c"
#include "../kernel/fs/initramfs.c"

static u8 *create_synthetic_archive(size_t *out_size) {
    /* Create a valid archive in memory */
    size_t header_size = 64;
    size_t entry_size = 144;
    size_t entry_count = 3;
    size_t entries_offset = header_size;
    size_t data_offset = entries_offset + entry_count * entry_size;
    if (data_offset % 16 != 0) data_offset += 16 - (data_offset % 16);

    const char *f1_name = "/test.txt";
    const char *f1_data = "Hello Host VFS!";
    size_t f1_size = strlen(f1_data);
    size_t f1_off = data_offset;

    const char *d1_name = "/sub";

    const char *f2_name = "/sub/nested.txt";
    const char *f2_data = "Nested Content Data";
    size_t f2_size = strlen(f2_data);
    size_t f2_off = f1_off + f1_size;
    if (f2_off % 16 != 0) f2_off += 16 - (f2_off % 16);

    size_t total_size = f2_off + f2_size;
    u8 *buf = (u8 *)calloc(1, total_size);
    assert(buf != NULL);

    /* Header */
    astra_initramfs_header_t *hdr = (astra_initramfs_header_t *)buf;
    memcpy(hdr->magic, "ASTRAFS1", 8);
    hdr->version = 1;
    hdr->header_size = (u32)header_size;
    hdr->total_size = (u32)total_size;
    hdr->entry_count = (u32)entry_count;
    hdr->entries_offset = (u32)entries_offset;
    hdr->data_offset = (u32)data_offset;

    /* Entry 0: /test.txt */
    astra_initramfs_entry_t *e = (astra_initramfs_entry_t *)(buf + entries_offset);
    strncpy(e[0].name, f1_name, sizeof(e[0].name));
    e[0].type = VFS_NODE_FILE;
    e[0].data_offset = (u32)f1_off;
    e[0].size = (u32)f1_size;
    memcpy(buf + f1_off, f1_data, f1_size);

    /* Entry 1: /sub */
    strncpy(e[1].name, d1_name, sizeof(e[1].name));
    e[1].type = VFS_NODE_DIR;
    e[1].data_offset = 0;
    e[1].size = 0;

    /* Entry 2: /sub/nested.txt */
    strncpy(e[2].name, f2_name, sizeof(e[2].name));
    e[2].type = VFS_NODE_FILE;
    e[2].data_offset = (u32)f2_off;
    e[2].size = (u32)f2_size;
    memcpy(buf + f2_off, f2_data, f2_size);

    *out_size = total_size;
    return buf;
}

__declspec(dllexport) int run_all_vfs_host_tests(void) {
    printf("======================================================\n");
    printf("       AstraOS VFS & Initramfs Host Unit Tests        \n");
    printf("======================================================\n");

    /* 1. Valid Archive Validation & Mount */
    printf("[HOST TEST] 1. Valid archive validation & mount...\n");
    size_t arch_size = 0;
    u8 *arch_data = create_synthetic_archive(&arch_size);
    assert(arch_data != NULL);

    int val = initramfs_validate(arch_data, arch_size);
    assert(val == INITRAMFS_OK);

    vfs_init();
    vfs_node_t *root = NULL;
    int mnt = initramfs_mount(arch_data, arch_size, &root);
    assert(mnt == VFS_OK);
    assert(root != NULL);
    assert(vfs_mount_root(root) == VFS_OK);
    printf("  PASS: Synthetic initramfs validated and mounted successfully.\n");

    /* 2. Path Lookup and Traversals */
    printf("[HOST TEST] 2. Path lookup and invalid syntax rejection...\n");
    vfs_node_t *node = NULL;
    assert(vfs_lookup("/", &node) == VFS_OK && node->type == VFS_NODE_DIR);
    assert(vfs_lookup("/test.txt", &node) == VFS_OK && node->type == VFS_NODE_FILE);
    assert(vfs_lookup("/sub", &node) == VFS_OK && node->type == VFS_NODE_DIR);
    assert(vfs_lookup("/sub/nested.txt", &node) == VFS_OK && node->type == VFS_NODE_FILE);
    assert(vfs_lookup("/nonexistent", &node) == VFS_ERR_NOT_FOUND);

    /* Malformed / Traversal attempts */
    assert(vfs_lookup(NULL, &node) == VFS_ERR_INVAL);
    assert(vfs_lookup("", &node) == VFS_ERR_INVALID_PATH);
    assert(vfs_lookup("test.txt", &node) == VFS_ERR_INVALID_PATH);
    assert(vfs_lookup("/../etc/passwd", &node) == VFS_ERR_INVALID_PATH);
    assert(vfs_lookup("/sub/../test.txt", &node) == VFS_ERR_INVALID_PATH);
    assert(vfs_lookup("//double//slash", &node) == VFS_ERR_INVALID_PATH);
    assert(vfs_lookup("/./dot/slash", &node) == VFS_ERR_INVALID_PATH);
    printf("  PASS: Path lookup resolution and traversal rejections verified.\n");

    /* 3. File Operations: Open, Read, Pread, Close, EOF */
    printf("[HOST TEST] 3. File operations, offset reads, EOF, and handle validation...\n");
    /* Reject opening directory as file */
    assert(vfs_open("/sub", VFS_O_RDONLY) == VFS_ERR_IS_DIR);
    /* Reject write flags */
    assert(vfs_open("/test.txt", VFS_O_WRONLY) == VFS_ERR_READ_ONLY);
    assert(vfs_open("/test.txt", VFS_O_RDWR) == VFS_ERR_READ_ONLY);

    vfs_fd_t fd = vfs_open("/test.txt", VFS_O_RDONLY);
    assert(fd >= 0);

    /* Sequential read */
    char buf[64];
    memset(buf, 0, sizeof(buf));
    ssize_t r1 = vfs_read(fd, buf, 5);
    assert(r1 == 5);
    assert(memcmp(buf, "Hello", 5) == 0);

    ssize_t r2 = vfs_read(fd, buf + 5, 20);
    assert(r2 == 10); /* " Host VFS!" is 10 chars */
    assert(memcmp(buf, "Hello Host VFS!", 15) == 0);

    /* EOF check: further read returns 0 */
    ssize_t r3 = vfs_read(fd, buf, 10);
    assert(r3 == 0);

    /* Pread at specific offset (middle) */
    memset(buf, 0, sizeof(buf));
    ssize_t pr = vfs_pread(fd, buf, 4, 6); /* "Host" at offset 6 */
    assert(pr == 4);
    assert(memcmp(buf, "Host", 4) == 0);

    /* Invalid FD operations */
    assert(vfs_read(99, buf, 5) == VFS_ERR_INVALID_HANDLE);
    assert(vfs_read(-1, buf, 5) == VFS_ERR_INVALID_HANDLE);
    assert(vfs_close(99) == VFS_ERR_INVALID_HANDLE);

    /* Close FD */
    assert(vfs_close(fd) == VFS_OK);
    /* Double close returns error */
    assert(vfs_close(fd) == VFS_ERR_INVALID_HANDLE);
    printf("  PASS: File open, sequential read, pread, EOF, and handles verified.\n");

    /* 4. Stat and Readdir */
    printf("[HOST TEST] 4. File metadata stat and directory enumeration readdir...\n");
    vfs_stat_t st;
    assert(vfs_stat("/test.txt", &st) == VFS_OK);
    assert(st.type == VFS_NODE_FILE);
    assert(st.size == 15);

    vfs_dirent_t dent;
    /* Readdir on "/" */
    assert(vfs_readdir("/", 0, &dent) == VFS_OK);
    assert(dent.type == VFS_NODE_FILE && strcmp(dent.name, "test.txt") == 0);
    assert(vfs_readdir("/", 1, &dent) == VFS_OK);
    assert(dent.type == VFS_NODE_DIR && strcmp(dent.name, "sub") == 0);
    assert(vfs_readdir("/", 2, &dent) == VFS_ERR_EOF);

    /* Readdir on "/sub" */
    assert(vfs_readdir("/sub", 0, &dent) == VFS_OK);
    assert(dent.type == VFS_NODE_FILE && strcmp(dent.name, "nested.txt") == 0);
    assert(vfs_readdir("/sub", 1, &dent) == VFS_ERR_EOF);

    /* Readdir on regular file returns NOT_DIR */
    assert(vfs_readdir("/test.txt", 0, &dent) == VFS_ERR_NOT_DIR);
    printf("  PASS: Stat metadata and directory enumeration verified.\n");

    /* 5. Malformed Archive Negative Verification */
    printf("[HOST TEST] 5. Negative testing of malformed, corrupt, and malicious archives...\n");
    /* NULL pointer */
    assert(initramfs_validate(NULL, 100) == INITRAMFS_ERR_NULL_PTR);
    /* Truncated header */
    assert(initramfs_validate(arch_data, 30) == INITRAMFS_ERR_TOO_SMALL);

    /* Bad Magic */
    u8 bad_buf[512];
    memcpy(bad_buf, arch_data, sizeof(bad_buf));
    bad_buf[0] = 'X';
    assert(initramfs_validate(bad_buf, arch_size) == INITRAMFS_ERR_BAD_MAGIC);

    /* Bad Version */
    memcpy(bad_buf, arch_data, sizeof(bad_buf));
    ((astra_initramfs_header_t *)bad_buf)->version = 42;
    assert(initramfs_validate(bad_buf, arch_size) == INITRAMFS_ERR_UNSUPPORTED_VERSION);

    /* Bounds: total_size > archive_size */
    memcpy(bad_buf, arch_data, sizeof(bad_buf));
    ((astra_initramfs_header_t *)bad_buf)->total_size = (u32)(arch_size + 1000);
    assert(initramfs_validate(bad_buf, arch_size) == INITRAMFS_ERR_BOUNDS);

    /* Overflow: entries_offset + bytes wrapping 32-bit */
    memcpy(bad_buf, arch_data, sizeof(bad_buf));
    ((astra_initramfs_header_t *)bad_buf)->entries_offset = 0xFFFFFFF0;
    assert(initramfs_validate(bad_buf, 0xFFFFFFFF) == INITRAMFS_ERR_OVERFLOW);

    /* Path traversal in entry */
    memcpy(bad_buf, arch_data, sizeof(bad_buf));
    astra_initramfs_entry_t *be = (astra_initramfs_entry_t *)(bad_buf + ((astra_initramfs_header_t *)bad_buf)->entries_offset);
    strncpy(be[0].name, "/../passwd", sizeof(be[0].name));
    assert(initramfs_validate(bad_buf, arch_size) == INITRAMFS_ERR_TRAVERSAL);

    /* Relative path in entry */
    memcpy(bad_buf, arch_data, sizeof(bad_buf));
    be = (astra_initramfs_entry_t *)(bad_buf + ((astra_initramfs_header_t *)bad_buf)->entries_offset);
    strncpy(be[0].name, "relative_path", sizeof(be[0].name));
    assert(initramfs_validate(bad_buf, arch_size) == INITRAMFS_ERR_TRAVERSAL);

    /* Duplicate entries */
    memcpy(bad_buf, arch_data, sizeof(bad_buf));
    be = (astra_initramfs_entry_t *)(bad_buf + ((astra_initramfs_header_t *)bad_buf)->entries_offset);
    strncpy(be[1].name, be[0].name, sizeof(be[1].name));
    assert(initramfs_validate(bad_buf, arch_size) == INITRAMFS_ERR_DUPLICATE);

    /* File/Directory Conflict (file /sub vs existing /sub) */
    memcpy(bad_buf, arch_data, sizeof(bad_buf));
    be = (astra_initramfs_entry_t *)(bad_buf + ((astra_initramfs_header_t *)bad_buf)->entries_offset);
    strncpy(be[0].name, "/sub/child.txt", sizeof(be[0].name));
    be[0].type = VFS_NODE_FILE;
    strncpy(be[1].name, "/sub", sizeof(be[1].name));
    be[1].type = VFS_NODE_FILE; /* /sub is a file but /sub/child.txt exists! */
    be[1].data_offset = be[0].data_offset;
    be[1].size = 1;
    assert(initramfs_validate(bad_buf, arch_size) == INITRAMFS_ERR_CONFLICT);

    printf("  PASS: All malformed archive variants safely caught and rejected.\n");

    /* 6. Phase AC: Mutable RAM Filesystem (Creation, Write, Read, Append, Truncation, Limits) */
    printf("[HOST TEST] 6. Phase AC: Mutable RAM Filesystem (Creation, Write, Read, Append, Truncation, Limits)...\n");

    /* 6A: Opening nonexistent mutable file without VFS_O_CREAT returns NOT_FOUND */
    assert(vfs_open("/data/test.txt", VFS_O_WRONLY) == VFS_ERR_NOT_FOUND);

    /* 6B: File creation with VFS_O_CREAT | VFS_O_WRONLY */
    vfs_fd_t mfd = vfs_open("/data/test.txt", VFS_O_WRONLY | VFS_O_CREAT);
    assert(mfd >= 3);

    /* Zero-length write returns 0 */
    assert(vfs_write(mfd, "test", 0) == 0);

    /* Write 13 bytes */
    const char *msg1 = "Hello Mutable";
    ssize_t w1 = vfs_write(mfd, msg1, 13);
    assert(w1 == 13);
    assert(vfs_close(mfd) == VFS_OK);

    /* 6C: Stat verification */
    vfs_stat_t mst;
    assert(vfs_stat("/data/test.txt", &mst) == VFS_OK);
    assert(mst.type == VFS_NODE_FILE);
    assert(mst.size == 13);

    /* 6D: Read back written data */
    vfs_fd_t rfd = vfs_open("/data/test.txt", VFS_O_RDONLY);
    assert(rfd >= 3);
    char m_buf[64];
    memset(m_buf, 0, sizeof(m_buf));
    assert(vfs_read(rfd, m_buf, 0) == 0); /* Zero length read */
    assert(vfs_read(rfd, m_buf, sizeof(m_buf)) == 13);
    assert(memcmp(m_buf, "Hello Mutable", 13) == 0);
    assert(vfs_read(rfd, m_buf, sizeof(m_buf)) == 0); /* EOF */
    assert(vfs_close(rfd) == VFS_OK);

    /* 6E: Append mode (VFS_O_APPEND) */
    vfs_fd_t afd = vfs_open("/data/test.txt", VFS_O_WRONLY | VFS_O_APPEND);
    assert(afd >= 3);
    const char *msg2 = " Appended!";
    assert(vfs_write(afd, msg2, 10) == 10);
    assert(vfs_close(afd) == VFS_OK);

    assert(vfs_stat("/data/test.txt", &mst) == VFS_OK);
    assert(mst.size == 23);

    rfd = vfs_open("/data/test.txt", VFS_O_RDONLY);
    assert(rfd >= 3);
    memset(m_buf, 0, sizeof(m_buf));
    assert(vfs_read(rfd, m_buf, sizeof(m_buf)) == 23);
    assert(memcmp(m_buf, "Hello Mutable Appended!", 23) == 0);
    assert(vfs_close(rfd) == VFS_OK);

    /* 6F: Truncate mode (VFS_O_TRUNC) */
    vfs_fd_t tfd = vfs_open("/data/test.txt", VFS_O_WRONLY | VFS_O_TRUNC);
    assert(tfd >= 3);
    assert(vfs_stat("/data/test.txt", &mst) == VFS_OK);
    assert(mst.size == 0);
    assert(vfs_write(tfd, "Truncated", 9) == 9);
    assert(vfs_close(tfd) == VFS_OK);

    assert(vfs_stat("/data/test.txt", &mst) == VFS_OK);
    assert(mst.size == 9);

    /* 6G: Protection: Initramfs files reject VFS_O_TRUNC and VFS_O_WRONLY */
    assert(vfs_open("/test.txt", VFS_O_WRONLY | VFS_O_TRUNC) == VFS_ERR_READ_ONLY);

    /* 6H: Max file size enforcement (16384 bytes) */
    vfs_fd_t cap_fd = vfs_open("/data/cap.bin", VFS_O_WRONLY | VFS_O_CREAT);
    assert(cap_fd >= 3);
    /* Writing beyond VFS_MAX_FILE_SIZE (16384) */
    u8 dummy[1024];
    memset(dummy, 0xAA, sizeof(dummy));
    /* Write 16 chunks of 1024 = 16384 bytes */
    for (int c = 0; c < 16; c++) {
        assert(vfs_write(cap_fd, dummy, 1024) == 1024);
    }
    /* 17th chunk exceeds 16384 limit -> VFS_ERR_NOSPC */
    assert(vfs_write(cap_fd, dummy, 1) == VFS_ERR_NOSPC);
    assert(vfs_close(cap_fd) == VFS_OK);

    /* 6I: Max file count enforcement (VFS_MAX_MUTABLE_FILES = 8) */
    /* We currently have 2 files (/data/test.txt and /data/cap.bin). Create 6 more */
    for (int f = 2; f < VFS_MAX_MUTABLE_FILES; f++) {
        char fname[32];
        snprintf(fname, sizeof(fname), "/data/file%d.bin", f);
        vfs_fd_t dfd = vfs_open(fname, VFS_O_WRONLY | VFS_O_CREAT);
        assert(dfd >= 3);
        assert(vfs_close(dfd) == VFS_OK);
    }
    /* 9th file creation exceeds file count -> VFS_ERR_NOSPC */
    assert(vfs_open("/data/overflow.bin", VFS_O_WRONLY | VFS_O_CREAT) == VFS_ERR_NOSPC);

    /* 6J: Durability & rename operations in ramfs */
    vfs_fd_t sync_fd = vfs_open("/data/test.txt", VFS_O_RDWR);
    assert(sync_fd >= 3);
    assert(vfs_fsync(sync_fd) == VFS_OK);
    assert(vfs_close(sync_fd) == VFS_OK);
    assert(vfs_sync() == VFS_OK);

    assert(vfs_rename("/data/test.txt", "/data/test_renamed.txt") == VFS_OK);
    vfs_node_t *ren_node = NULL;
    assert(vfs_lookup("/data/test_renamed.txt", &ren_node) == VFS_OK && ren_node != NULL);
    assert(ren_node->size == 9);
    assert(vfs_lookup("/data/test.txt", &ren_node) == VFS_ERR_NOT_FOUND);

    printf("  PASS: Mutable RAM filesystem creation, write, read, append, truncation, bounds, fsync, and rename verified.\n");
    printf("------------------------------------------------------\n");
    printf("[SUCCESS] All host-side VFS & Initramfs tests passed!\n");
    printf("======================================================\n");

    free(arch_data);
    return 0;
}

#ifndef HOST_DLL
int main(void) {
    return run_all_vfs_host_tests();
}
#endif

#endif /* HOST_TEST */
