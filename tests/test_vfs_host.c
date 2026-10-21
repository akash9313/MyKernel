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
    printf("------------------------------------------------------\n");
    printf("[SUCCESS] All host-side VFS & Initramfs tests passed!\n");
    printf("======================================================\n");

    free(arch_data);
    return 0;
}

#endif /* HOST_TEST */
