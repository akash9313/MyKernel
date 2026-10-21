#include <kernel/types.h>
#include <kernel/boot.h>
#include <kernel/kprintf.h>
#include <kernel/panic.h>
#include <kernel/vfs.h>
#include <kernel/initramfs.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/heap.h>
#include <kernel/pmm.h>

static void *kmemcpy(void *dest, const void *src, size_t len) {
    u8 *d = (u8 *)dest;
    const u8 *s = (const u8 *)src;
    for (size_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return dest;
}

static void *kmemset(void *dest, int val, size_t len) {
    u8 *d = (u8 *)dest;
    for (size_t i = 0; i < len; i++) {
        d[i] = (u8)val;
    }
    return dest;
}

static int kmemcmp(const void *s1, const void *s2, size_t len) {
    const u8 *p1 = (const u8 *)s1;
    const u8 *p2 = (const u8 *)s2;
    for (size_t i = 0; i < len; i++) {
        if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
    }
    return 0;
}

static int kstrcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static void kstrncpy(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < max_len) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static bool kstrstr_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    for (size_t i = 0; haystack[i] != '\0'; i++) {
        size_t j = 0;
        while (needle[j] != '\0' && haystack[i + j] == needle[j]) {
            j++;
        }
        if (needle[j] == '\0') {
            return true;
        }
    }
    return false;
}

void vfs_initramfs_run_tests(void) {
    kprintf("\n=======================================================\n");
    kprintf("       AstraOS Phase G: Initramfs & VFS Tests          \n");
    kprintf("=======================================================\n");

    /* -------------------------------------------------------------
     * TEST 1: Phase G - Archive Validation and VFS Root Mount
     * ------------------------------------------------------------- */
    kprintf("[TEST 1] Initializing VFS subsystem...\n");
    vfs_init();

    struct limine_module_response *mod_resp = boot_get_module_response();
    if (!mod_resp || mod_resp->module_count == 0 || !mod_resp->modules || !mod_resp->modules[0]) {
        panic("TEST 1 FAILED: No initramfs module provided by bootloader!");
    }

    struct limine_file *initramfs_file = mod_resp->modules[0];
    const void *archive_data = initramfs_file->address;
    size_t archive_size = (size_t)initramfs_file->size;
    kprintf("[TEST 1] Initramfs module loaded at 0x%016llx, size: %llu bytes, path: '%s'\n",
            (unsigned long long)(uintptr_t)archive_data, (unsigned long long)archive_size,
            initramfs_file->path ? initramfs_file->path : "<unknown>");

    int val_res = initramfs_validate(archive_data, archive_size);
    if (val_res != INITRAMFS_OK) {
        panic("TEST 1 FAILED: Boot initramfs validation failed! Error %d", val_res);
    }

    vfs_node_t *root_node = NULL;
    int mount_res = initramfs_mount(archive_data, archive_size, &root_node);
    if (mount_res != VFS_OK || !root_node) {
        panic("TEST 1 FAILED: Failed to mount initramfs archive! Error %d", mount_res);
    }

    int vfs_mnt_res = vfs_mount_root(root_node);
    if (vfs_mnt_res != VFS_OK) {
        panic("TEST 1 FAILED: Failed to mount root node to VFS! Error %d", vfs_mnt_res);
    }

    kprintf("[TEST 1] PASS: Initramfs verified and mounted at VFS root successfully.\n");
    kprintf("ASTRA_INITRAMFS_OK\n\n");

    /* -------------------------------------------------------------
     * TEST 2: Phase G - Path Resolution & Traversal Defense
     * ------------------------------------------------------------- */
    kprintf("[TEST 2] Testing VFS path lookup and traversal defense...\n");

    vfs_node_t *node = NULL;

    /* Lookup root "/" */
    if (vfs_lookup("/", &node) != VFS_OK || !node || node->type != VFS_NODE_DIR) {
        panic("TEST 2 FAILED: Root '/' lookup failed!");
    }

    /* Lookup regular file "/hello.txt" */
    if (vfs_lookup("/hello.txt", &node) != VFS_OK || !node || node->type != VFS_NODE_FILE) {
        panic("TEST 2 FAILED: Lookup of '/hello.txt' failed!");
    }

    /* Lookup directories */
    if (vfs_lookup("/nested", &node) != VFS_OK || !node || node->type != VFS_NODE_DIR) {
        panic("TEST 2 FAILED: Lookup of '/nested' failed!");
    }
    if (vfs_lookup("/nested/deep", &node) != VFS_OK || !node || node->type != VFS_NODE_DIR) {
        panic("TEST 2 FAILED: Lookup of '/nested/deep' failed!");
    }

    /* Lookup nested file */
    if (vfs_lookup("/nested/deep/secret.txt", &node) != VFS_OK || !node || node->type != VFS_NODE_FILE) {
        panic("TEST 2 FAILED: Lookup of '/nested/deep/secret.txt' failed!");
    }

    /* Lookup ELF binaries */
    if (vfs_lookup("/bin/user_app.elf", &node) != VFS_OK || !node || node->type != VFS_NODE_FILE) {
        panic("TEST 2 FAILED: Lookup of '/bin/user_app.elf' failed!");
    }
    if (vfs_lookup("/bin/bad_elf.elf", &node) != VFS_OK || !node || node->type != VFS_NODE_FILE) {
        panic("TEST 2 FAILED: Lookup of '/bin/bad_elf.elf' failed!");
    }

    /* Negative tests: Non-existent file */
    if (vfs_lookup("/missing_file.txt", &node) != VFS_ERR_NOT_FOUND) {
        panic("TEST 2 FAILED: Nonexistent file did not return VFS_ERR_NOT_FOUND!");
    }

    /* Traversal attempts */
    if (vfs_lookup("../hello.txt", &node) != VFS_ERR_INVALID_PATH) {
        panic("TEST 2 FAILED: Relative path '../hello.txt' not rejected!");
    }
    if (vfs_lookup("hello.txt", &node) != VFS_ERR_INVALID_PATH) {
        panic("TEST 2 FAILED: Relative path without slash not rejected!");
    }
    if (vfs_lookup("/nested/../hello.txt", &node) != VFS_ERR_INVALID_PATH) {
        panic("TEST 2 FAILED: Embedded '..' path traversal not rejected!");
    }
    if (vfs_lookup("/nested/deep/../../etc/shadow", &node) != VFS_ERR_INVALID_PATH) {
        panic("TEST 2 FAILED: Deep traversal attempt not rejected!");
    }
    if (vfs_lookup(NULL, &node) != VFS_ERR_INVAL) {
        panic("TEST 2 FAILED: NULL path not rejected with VFS_ERR_INVAL!");
    }

    kprintf("[TEST 2] PASS: Path resolution and traversal defense verified.\n");
    kprintf("ASTRA_VFS_LOOKUP_OK\n\n");

    /* -------------------------------------------------------------
     * TEST 3: Phase G - File Operations, Pread, Stat, Readdir
     * ------------------------------------------------------------- */
    kprintf("[TEST 3] Testing VFS file operations (open, read, pread, stat, readdir)...\n");

    /* Write/Modify flag rejection */
    int w_fd = vfs_open("/hello.txt", VFS_O_WRONLY);
    if (w_fd != VFS_ERR_READ_ONLY) {
        panic("TEST 3 FAILED: Opening read-only file with VFS_O_WRONLY did not return VFS_ERR_READ_ONLY! (got %d)", w_fd);
    }
    int rw_fd = vfs_open("/hello.txt", VFS_O_RDWR);
    if (rw_fd != VFS_ERR_READ_ONLY) {
        panic("TEST 3 FAILED: Opening read-only file with VFS_O_RDWR did not return VFS_ERR_READ_ONLY! (got %d)", rw_fd);
    }

    /* Open regular file */
    int fd = vfs_open("/hello.txt", VFS_O_RDONLY);
    if (fd < 0) {
        panic("TEST 3 FAILED: Failed to open '/hello.txt'! Error %d", fd);
    }

    /* Stat via fd */
    vfs_stat_t st;
    if (vfs_fstat(fd, &st) != VFS_OK) {
        panic("TEST 3 FAILED: vfs_fstat failed!");
    }
    if (st.type != VFS_NODE_FILE || st.size != 30) {
        panic("TEST 3 FAILED: Unexpected stat info for /hello.txt: type=%d, size=%llu",
              st.type, (unsigned long long)st.size);
    }

    /* Incremental reading */
    char buf[64];
    kmemset(buf, 0, sizeof(buf));
    ssize_t n1 = vfs_read(fd, buf, 5);
    if (n1 != 5 || kmemcmp(buf, "Hello", 5) != 0) {
        panic("TEST 3 FAILED: First chunk read failed! (n1=%lld, buf='%s')", (long long)n1, buf);
    }

    ssize_t n2 = vfs_read(fd, buf + 5, sizeof(buf) - 5);
    if (n2 != 25) {
        panic("TEST 3 FAILED: Second chunk read failed! (n2=%lld)", (long long)n2);
    }
    buf[30] = '\0';
    if (kstrcmp(buf, "Hello from AstraOS Initramfs!\n") != 0) {
        panic("TEST 3 FAILED: Content mismatch: '%s'", buf);
    }

    /* EOF read returns 0 */
    ssize_t neof = vfs_read(fd, buf, sizeof(buf));
    if (neof != 0) {
        panic("TEST 3 FAILED: Read at EOF did not return 0! (got %lld)", (long long)neof);
    }

    /* Positional read (pread) does not alter fd offset */
    char pread_buf[16];
    kmemset(pread_buf, 0, sizeof(pread_buf));
    ssize_t np = vfs_pread(fd, pread_buf, 5, 0);
    if (np != 5 || kmemcmp(pread_buf, "Hello", 5) != 0) {
        panic("TEST 3 FAILED: vfs_pread failed! (np=%lld, buf='%s')", (long long)np, pread_buf);
    }
    if (vfs_read(fd, buf, sizeof(buf)) != 0) {
        panic("TEST 3 FAILED: vfs_pread altered file offset!");
    }

    vfs_close(fd);

    /* Read deep nested file */
    int deep_fd = vfs_open("/nested/deep/secret.txt", VFS_O_RDONLY);
    if (deep_fd < 0) {
        panic("TEST 3 FAILED: Failed to open deep secret file!");
    }
    char deep_buf[64];
    kmemset(deep_buf, 0, sizeof(deep_buf));
    ssize_t nd = vfs_read(deep_fd, deep_buf, sizeof(deep_buf) - 1);
    if (nd <= 0 || !kstrstr_contains(deep_buf, "AstraOS nested filesystem secret")) {
        panic("TEST 3 FAILED: Deep secret content mismatch! (nd=%lld, buf='%s')", (long long)nd, deep_buf);
    }
    vfs_close(deep_fd);

    /* Directory enumeration with vfs_readdir */
    vfs_dirent_t dent;
    bool found_hello = false, found_nested = false, found_bin = false;
    size_t d_idx = 0;
    while (vfs_readdir("/", d_idx, &dent) == VFS_OK) {
        if (kstrcmp(dent.name, "hello.txt") == 0 && dent.type == VFS_NODE_FILE) found_hello = true;
        if (kstrcmp(dent.name, "nested") == 0 && dent.type == VFS_NODE_DIR) found_nested = true;
        if (kstrcmp(dent.name, "bin") == 0 && dent.type == VFS_NODE_DIR) found_bin = true;
        d_idx++;
    }
    if (!found_hello || !found_nested || !found_bin) {
        panic("TEST 3 FAILED: readdir on '/' missing entries! hello=%d, nested=%d, bin=%d",
              found_hello, found_nested, found_bin);
    }

    /* Subdirectory enumeration */
    bool found_deep = false;
    d_idx = 0;
    while (vfs_readdir("/nested", d_idx, &dent) == VFS_OK) {
        if (kstrcmp(dent.name, "deep") == 0 && dent.type == VFS_NODE_DIR) found_deep = true;
        d_idx++;
    }
    if (!found_deep) {
        panic("TEST 3 FAILED: readdir on '/nested' missing 'deep' subdirectory!");
    }

    /* Negative readdir on regular file */
    if (vfs_readdir("/hello.txt", 0, &dent) != VFS_ERR_NOT_DIR) {
        panic("TEST 3 FAILED: readdir on regular file did not return VFS_ERR_NOT_DIR!");
    }

    kprintf("[TEST 3] PASS: File operations, chunked read, pread, EOF, fstat, and readdir verified.\n");
    kprintf("ASTRA_VFS_READ_OK\n\n");

    /* -------------------------------------------------------------
     * TEST 4: Phase G - Parser Defense on Corrupted & Malformed Archives
     * ------------------------------------------------------------- */
    kprintf("[TEST 4] Testing parser resilience against corrupted & malformed initramfs archives...\n");

    size_t scratch_size = archive_size;
    size_t scratch_pages = (scratch_size + 4095) / 4096;
    phys_addr_t scratch_pa = 0;
    if (pmm_alloc_contiguous(scratch_pages, &scratch_pa) != PMM_SUCCESS) {
        panic("TEST 4 FAILED: Out of memory for negative testing buffer!");
    }
    u64 hhdm = boot_get_hhdm_response()->offset;
    u8 *scratch = (u8 *)(scratch_pa + hhdm);

    /* 4A: NULL buffer */
    if (initramfs_validate(NULL, scratch_size) != INITRAMFS_ERR_NULL_PTR) {
        panic("TEST 4A FAILED: NULL archive buffer not rejected with INITRAMFS_ERR_NULL_PTR!");
    }

    /* 4B: Truncated buffer */
    if (initramfs_validate(archive_data, 16) != INITRAMFS_ERR_TOO_SMALL) {
        panic("TEST 4B FAILED: Truncated buffer not rejected with INITRAMFS_ERR_TOO_SMALL!");
    }

    /* 4C: Corrupted magic */
    kmemcpy(scratch, archive_data, scratch_size);
    scratch[0] = 'X'; scratch[1] = 'Y'; scratch[2] = 'Z';
    if (initramfs_validate(scratch, scratch_size) != INITRAMFS_ERR_BAD_MAGIC) {
        panic("TEST 4C FAILED: Bad magic not rejected with INITRAMFS_ERR_BAD_MAGIC!");
    }

    /* 4D: Unsupported version */
    kmemcpy(scratch, archive_data, scratch_size);
    astra_initramfs_header_t *shdr = (astra_initramfs_header_t *)scratch;
    shdr->version = 999;
    if (initramfs_validate(scratch, scratch_size) != INITRAMFS_ERR_UNSUPPORTED_VERSION) {
        panic("TEST 4D FAILED: Unsupported version not rejected with INITRAMFS_ERR_UNSUPPORTED_VERSION!");
    }

    /* 4E: Total size bounds violation */
    kmemcpy(scratch, archive_data, scratch_size);
    shdr = (astra_initramfs_header_t *)scratch;
    shdr->total_size = (u32)(scratch_size + 1024);
    if (initramfs_validate(scratch, scratch_size) != INITRAMFS_ERR_BOUNDS) {
        panic("TEST 4E FAILED: Out-of-bounds total_size not rejected with INITRAMFS_ERR_BOUNDS!");
    }

    /* 4F: Out-of-bounds entry data offset */
    kmemcpy(scratch, archive_data, scratch_size);
    shdr = (astra_initramfs_header_t *)scratch;
    astra_initramfs_entry_t *sentries = (astra_initramfs_entry_t *)(scratch + shdr->entries_offset);
    sentries[0].data_offset = (u32)(scratch_size + 4096);
    if (initramfs_validate(scratch, scratch_size) != INITRAMFS_ERR_BOUNDS) {
        panic("TEST 4F FAILED: Out-of-bounds entry data_offset not rejected with INITRAMFS_ERR_BOUNDS!");
    }

    /* 4G: Path traversal entry */
    kmemcpy(scratch, archive_data, scratch_size);
    shdr = (astra_initramfs_header_t *)scratch;
    sentries = (astra_initramfs_entry_t *)(scratch + shdr->entries_offset);
    kstrncpy(sentries[0].name, "/../evil.txt", sizeof(sentries[0].name));
    if (initramfs_validate(scratch, scratch_size) != INITRAMFS_ERR_TRAVERSAL) {
        panic("TEST 4G FAILED: Path traversal in entry not rejected with INITRAMFS_ERR_TRAVERSAL!");
    }

    /* 4H: Duplicate entry */
    kmemcpy(scratch, archive_data, scratch_size);
    shdr = (astra_initramfs_header_t *)scratch;
    sentries = (astra_initramfs_entry_t *)(scratch + shdr->entries_offset);
    if (shdr->entry_count >= 2) {
        kstrncpy(sentries[1].name, sentries[0].name, sizeof(sentries[1].name));
        if (initramfs_validate(scratch, scratch_size) != INITRAMFS_ERR_DUPLICATE) {
            panic("TEST 4H FAILED: Duplicate entry name not rejected with INITRAMFS_ERR_DUPLICATE!");
        }
    }

    pmm_free_contiguous(scratch_pa, scratch_pages);

    kprintf("[TEST 4] PASS: All malformed, corrupted, and adversarial archives safely rejected.\n");
    kprintf("ASTRA_VFS_INVALID_ARCHIVE_OK\n\n");

    /* -------------------------------------------------------------
     * TEST 5: Phase G - Native User ELF Execution from VFS
     * ------------------------------------------------------------- */
    kprintf("[TEST 5] Testing VFS-based native ELF execution...\n");

    /* Negative test: /bin/bad_elf.elf */
    process_t *bad_proc = process_create_elf_file("bad_elf", "/bin/bad_elf.elf");
    if (bad_proc != NULL) {
        panic("TEST 5 FAILED: Corrupted ELF /bin/bad_elf.elf unexpectedly succeeded!");
    }
    kprintf("[TEST 5] Negative test passed: /bin/bad_elf.elf rejected cleanly.\n");

    /* Positive execution of native user ELF loaded via VFS */
    process_t *vfs_user_proc = process_create_elf_file("vfs_user_app", "/bin/user_app.elf");
    if (!vfs_user_proc) {
        panic("TEST 5 FAILED: Failed to create user process from '/bin/user_app.elf'!");
    }

    kprintf("[TEST 5] Process '%s' created via VFS (PID %u). Yielding to run user code...\n",
            vfs_user_proc->name, vfs_user_proc->pid);

    while (vfs_user_proc->state != PROCESS_DEAD) {
        thread_yield();
    }

    kprintf("[TEST 5] VFS user process finished with exit code %d (fault=%d)\n",
            vfs_user_proc->exit_code, vfs_user_proc->exited_from_fault);

    if (vfs_user_proc->exit_code != 0 || vfs_user_proc->exited_from_fault) {
        panic("TEST 5 FAILED: VFS user process failed! (exit_code=%d, fault=%d)",
              vfs_user_proc->exit_code, vfs_user_proc->exited_from_fault);
    }

    process_reap_zombies();

    kprintf("[TEST 5] PASS: Native user-space ELF loaded through VFS and executed successfully in Ring 3!\n");
    kprintf("ASTRA_VFS_ELF_EXEC_OK\n\n");
}
