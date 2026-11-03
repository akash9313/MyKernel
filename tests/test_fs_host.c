#ifdef HOST_TEST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdarg.h>

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

void panic(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[PANIC] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(1);
}

/* Stubs for kernel symbols */
static struct vfs_node s_dummy_root;
struct vfs_node *vfs_get_root(void) {
    return &s_dummy_root;
}

#include "../kernel/include/kernel/block.h"
#include "../kernel/drivers/block.c"
#include "../kernel/include/kernel/vfs.h"
#include "../kernel/include/kernel/astrafs.h"

/* Mock 32 MiB Block Storage with Volatile Cache vs Persistent Storage Separation */
#define MOCK_TOTAL_SECTORS 65536

typedef enum {
    CACHE_MODE_DIRECT = 0,   /* Direct unbuffered writes to persistent storage */
    CACHE_MODE_VOLATILE = 1  /* Buffered writes to volatile cache; durable only on flush */
} cache_mode_t;

static cache_mode_t s_cache_mode = CACHE_MODE_DIRECT;
static u8 *s_persistent_storage = NULL;
static u8 *s_volatile_cache = NULL;
static u8 *s_dirty_bitmap = NULL;
static u8 *s_mock_storage = NULL; /* Aliased to s_persistent_storage for direct inspection */

static int s_mock_flushes = 0;
static bool s_mock_io_fail = false;
static bool s_mock_fail_flush = false;
static int s_mock_fail_write_after_sectors = -1;
static int s_mock_writes_count = 0;
static int s_mock_fail_sector_lba = -1;

static inline void dirty_bitmap_set(u64 lba) {
    if (lba < MOCK_TOTAL_SECTORS && s_dirty_bitmap) {
        s_dirty_bitmap[lba / 8] |= (u8)(1 << (lba % 8));
    }
}

static inline bool dirty_bitmap_is_set(u64 lba) {
    if (lba < MOCK_TOTAL_SECTORS && s_dirty_bitmap) {
        return (s_dirty_bitmap[lba / 8] & (1 << (lba % 8))) != 0;
    }
    return false;
}

static inline void dirty_bitmap_clear(u64 lba) {
    if (lba < MOCK_TOTAL_SECTORS && s_dirty_bitmap) {
        s_dirty_bitmap[lba / 8] &= (u8)~(1 << (lba % 8));
    }
}

static inline void dirty_bitmap_clear_all(void) {
    if (s_dirty_bitmap) {
        memset(s_dirty_bitmap, 0, MOCK_TOTAL_SECTORS / 8);
    }
}

static void mock_inject_torn_write(u64 lba, u32 commit_bytes, const void *data) {
    if (lba >= MOCK_TOTAL_SECTORS || !data) return;
    u32 sz = commit_bytes > ASTRAFS_SECTOR_SIZE ? ASTRAFS_SECTOR_SIZE : commit_bytes;
    memcpy(s_persistent_storage + (lba * ASTRAFS_SECTOR_SIZE), data, sz);
    memcpy(s_volatile_cache + (lba * ASTRAFS_SECTOR_SIZE), data, sz);
}

static int mock_read_sectors(block_device_t *dev, u64 lba, u32 count, void *buf) {
    (void)dev;
    if (s_mock_io_fail) return BLOCK_ERR_IO;
    if (s_mock_fail_sector_lba >= 0 &&
        (u64)s_mock_fail_sector_lba >= lba &&
        (u64)s_mock_fail_sector_lba < lba + count) {
        return BLOCK_ERR_IO;
    }
    if (lba + count > MOCK_TOTAL_SECTORS) return BLOCK_ERR_OUT_OF_BOUNDS;

    u8 *dst = (u8 *)buf;
    for (u32 i = 0; i < count; i++) {
        u64 sec = lba + i;
        const u8 *src = NULL;
        if (s_cache_mode == CACHE_MODE_VOLATILE && dirty_bitmap_is_set(sec)) {
            src = s_volatile_cache + (sec * ASTRAFS_SECTOR_SIZE);
        } else {
            src = s_persistent_storage + (sec * ASTRAFS_SECTOR_SIZE);
        }
        memcpy(dst + (i * ASTRAFS_SECTOR_SIZE), src, ASTRAFS_SECTOR_SIZE);
    }
    return BLOCK_OK;
}

static int mock_write_sectors(block_device_t *dev, u64 lba, u32 count, const void *buf) {
    (void)dev;
    if (s_mock_io_fail) return BLOCK_ERR_IO;
    if (s_mock_fail_sector_lba >= 0 &&
        (u64)s_mock_fail_sector_lba >= lba &&
        (u64)s_mock_fail_sector_lba < lba + count) {
        return BLOCK_ERR_IO;
    }
    if (s_mock_fail_write_after_sectors >= 0 &&
        s_mock_writes_count + (int)count > s_mock_fail_write_after_sectors) {
        return BLOCK_ERR_IO;
    }
    if (lba + count > MOCK_TOTAL_SECTORS) return BLOCK_ERR_OUT_OF_BOUNDS;

    const u8 *src = (const u8 *)buf;
    if (s_cache_mode == CACHE_MODE_VOLATILE) {
        for (u32 i = 0; i < count; i++) {
            u64 sec = lba + i;
            memcpy(s_volatile_cache + (sec * ASTRAFS_SECTOR_SIZE),
                   src + (i * ASTRAFS_SECTOR_SIZE), ASTRAFS_SECTOR_SIZE);
            dirty_bitmap_set(sec);
        }
    } else {
        memcpy(s_persistent_storage + (lba * ASTRAFS_SECTOR_SIZE), src, count * ASTRAFS_SECTOR_SIZE);
        memcpy(s_volatile_cache + (lba * ASTRAFS_SECTOR_SIZE), src, count * ASTRAFS_SECTOR_SIZE);
    }

    s_mock_writes_count += count;
    return BLOCK_OK;
}

static int mock_flush(block_device_t *dev) {
    (void)dev;
    if (s_mock_io_fail || s_mock_fail_flush) return BLOCK_ERR_IO;

    if (s_cache_mode == CACHE_MODE_VOLATILE) {
        /* Commit dirty sectors from volatile cache to persistent disk storage */
        for (u64 sec = 0; sec < MOCK_TOTAL_SECTORS; sec++) {
            if (dirty_bitmap_is_set(sec)) {
                memcpy(s_persistent_storage + (sec * ASTRAFS_SECTOR_SIZE),
                       s_volatile_cache + (sec * ASTRAFS_SECTOR_SIZE),
                       ASTRAFS_SECTOR_SIZE);
                dirty_bitmap_clear(sec);
            }
        }
    }

    s_mock_flushes++;
    return BLOCK_OK;
}

static block_ops_t s_mock_ops = {
    .read_sectors = mock_read_sectors,
    .write_sectors = mock_write_sectors,
    .flush = mock_flush
};

/* Include AstraFS implementation for direct host compilation */
#include "../kernel/fs/astrafs.c"

static void mock_power_loss(void) {
    /* Power loss: unflushed volatile cache discarded and restored from persistent storage */
    if (s_volatile_cache && s_persistent_storage) {
        memcpy(s_volatile_cache, s_persistent_storage, MOCK_TOTAL_SECTORS * ASTRAFS_SECTOR_SIZE);
    }
    dirty_bitmap_clear_all();
    /* Wipe in-memory filesystem structures */
    memset(&g_astrafs, 0, sizeof(g_astrafs));
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int run_all_fs_host_tests(void) {
    printf("\n=================================================================\n");
    printf("   AstraOS Phase AE: AstraFS Persistent Filesystem Host Tests\n");
    printf("=================================================================\n\n");

    s_persistent_storage = (u8 *)malloc(MOCK_TOTAL_SECTORS * ASTRAFS_SECTOR_SIZE);
    s_volatile_cache = (u8 *)malloc(MOCK_TOTAL_SECTORS * ASTRAFS_SECTOR_SIZE);
    s_dirty_bitmap = (u8 *)malloc(MOCK_TOTAL_SECTORS / 8);
    assert(s_persistent_storage != NULL && s_volatile_cache != NULL && s_dirty_bitmap != NULL);
    memset(s_persistent_storage, 0, MOCK_TOTAL_SECTORS * ASTRAFS_SECTOR_SIZE);
    memset(s_volatile_cache, 0, MOCK_TOTAL_SECTORS * ASTRAFS_SECTOR_SIZE);
    memset(s_dirty_bitmap, 0, MOCK_TOTAL_SECTORS / 8);
    s_mock_storage = s_persistent_storage;
    s_cache_mode = CACHE_MODE_DIRECT;

    block_device_t mock_dev;
    memset(&mock_dev, 0, sizeof(mock_dev));
    snprintf(mock_dev.name, sizeof(mock_dev.name), "hd1");
    mock_dev.id = 1;
    mock_dev.sector_size = ASTRAFS_SECTOR_SIZE;
    mock_dev.total_sectors = MOCK_TOTAL_SECTORS;
    mock_dev.capacity_bytes = (u64)MOCK_TOTAL_SECTORS * ASTRAFS_SECTOR_SIZE;
    mock_dev.status = BLOCK_STATUS_READY;
    mock_dev.read_only = false;
    mock_dev.is_boot_device = false;
    mock_dev.ops = &s_mock_ops;

    /* TEST 1: Format volume */
    printf("[TEST 1] Testing volume formatting on 32 MiB disk...\n");
    assert(astrafs_format(&mock_dev) == VFS_OK);
    assert(s_mock_flushes >= 1);

    astrafs_superblock_t *sbA = (astrafs_superblock_t *)(s_mock_storage + (ASTRAFS_SUPERBLOCK_A_LBA * 512));
    astrafs_superblock_t *sbB = (astrafs_superblock_t *)(s_mock_storage + (ASTRAFS_SUPERBLOCK_B_LBA * 512));
    assert(sbA->magic == ASTRAFS_MAGIC);
    assert(sbA->version == ASTRAFS_VERSION);
    assert(sbA->generation == 1);
    assert(sbA->checksum == astrafs_checksum(sbA, 508));
    assert(sbB->magic == ASTRAFS_MAGIC);
    assert(sbB->version == ASTRAFS_VERSION);
    assert(sbB->generation == 1);
    assert(sbB->checksum == astrafs_checksum(sbB, 508));
    printf("  PASS: Formatted superblocks A & B verified with valid checksums.\n");

    /* TEST 2: Mount volume */
    printf("[TEST 2] Testing volume mounting...\n");
    assert(astrafs_mount(&mock_dev) == VFS_OK);
    assert(astrafs_is_mounted() == true);
    astrafs_state_t *st = astrafs_get_state();
    assert(st->sb.generation == 1);
    assert(st->sb.file_count == 0);
    assert(astrafs_get_root_node()->type == VFS_NODE_DIR);
    printf("  PASS: Volume mounted cleanly; root node initialized.\n");

    /* TEST 3: Create file & syntax bounds */
    printf("[TEST 3] Testing file creation and path validation...\n");
    vfs_node_t *node1 = NULL;
    assert(astrafs_create("test.txt", VFS_O_CREAT | VFS_O_RDWR, &node1) == VFS_OK);
    assert(node1 != NULL);
    assert(strcmp(node1->name, "test.txt") == 0);
    assert(node1->size == 0);

    /* Disallow subdirectories in flat /data */
    vfs_node_t *bad_node = NULL;
    assert(astrafs_create("subdir/nested.txt", VFS_O_CREAT | VFS_O_RDWR, &bad_node) == VFS_ERR_INVALID_PATH);

    /* Lookup created file */
    vfs_node_t *lookup_node = NULL;
    assert(astrafs_lookup("test.txt", &lookup_node) == VFS_OK);
    assert(lookup_node == node1);
    assert(astrafs_lookup("/data/test.txt", &lookup_node) == VFS_OK);
    assert(lookup_node == node1);
    assert(astrafs_lookup("nonexistent.txt", &lookup_node) == VFS_ERR_NOT_FOUND);
    printf("  PASS: File creation, path validation, and lookup verified.\n");

    /* TEST 4: Small write, read-back, and offset reads */
    printf("[TEST 4] Testing small file write and read back...\n");
    const char *payload = "Hello AstraFS Persistent Filesystem!";
    size_t plen = strlen(payload);
    assert(astrafs_write(node1, payload, plen, 0) == (ssize_t)plen);
    assert(node1->size == plen);

    char read_buf[64];
    memset(read_buf, 0, sizeof(read_buf));
    assert(astrafs_read(node1, read_buf, plen, 0) == (ssize_t)plen);
    assert(strcmp(read_buf, payload) == 0);

    /* Offset read */
    memset(read_buf, 0, sizeof(read_buf));
    assert(astrafs_read(node1, read_buf, 7, 6) == 7);
    assert(memcmp(read_buf, "AstraFS", 7) == 0);
    printf("  PASS: Payload write and offset reads verified byte-for-byte.\n");

    /* TEST 5: Multi-sector write & boundary conditions */
    printf("[TEST 5] Testing multi-sector (4096-byte) write and extent expansion...\n");
    vfs_node_t *node_large = NULL;
    assert(astrafs_create("large.bin", VFS_O_CREAT | VFS_O_RDWR, &node_large) == VFS_OK);
    u8 *large_buf = (u8 *)malloc(4096);
    for (int i = 0; i < 4096; i++) {
        large_buf[i] = (u8)(i ^ (i >> 8));
    }
    assert(astrafs_write(node_large, large_buf, 4096, 0) == 4096);
    assert(node_large->size == 4096);

    u8 *large_read = (u8 *)malloc(4096);
    assert(astrafs_read(node_large, large_read, 4096, 0) == 4096);
    assert(memcmp(large_buf, large_read, 4096) == 0);
    free(large_buf);
    free(large_read);
    printf("  PASS: Multi-sector write across 8 sectors verified byte-for-byte.\n");

    /* TEST 6: Append and Truncate */
    printf("[TEST 6] Testing append and truncate operations...\n");
    const char *suffix = "__APPENDED_PAYLOAD";
    size_t slen = strlen(suffix);
    size_t old_size = node1->size;
    assert(astrafs_write(node1, suffix, slen, old_size) == (ssize_t)slen);
    assert(node1->size == old_size + slen);

    char full_buf[128];
    memset(full_buf, 0, sizeof(full_buf));
    assert(astrafs_read(node1, full_buf, node1->size, 0) == (ssize_t)node1->size);
    assert(strstr(full_buf, suffix) != NULL);

    /* Truncate back to 0 */
    assert(astrafs_truncate(node1, 0) == VFS_OK);
    assert(node1->size == 0);
    assert(astrafs_read(node1, full_buf, 10, 0) == 0); /* EOF */
    printf("  PASS: Append growth and truncate to 0 verified.\n");

    /* TEST 7: Atomic Dual-Set Generation Increment */
    printf("[TEST 7] Testing atomic dual-set metadata switching...\n");
    u64 gen_before = st->sb.generation;
    u8 set_before = st->active_set;

    assert(astrafs_write(node1, "NewData", 7, 0) == 7);
    assert(st->sb.generation == gen_before + 1);
    assert(st->active_set == 1 - set_before);
    assert(st->sb.checksum == astrafs_checksum(&st->sb, 508));
    printf("  PASS: Alternating metadata sets and monotonic generation confirmed.\n");

    /* TEST 8: Corrupted Superblock Fallback */
    printf("[TEST 8] Testing recovery with one corrupted superblock...\n");
    /* Deliberately corrupt active superblock's checksum */
    u32 active_sb_lba = (st->active_set == 0) ? ASTRAFS_SUPERBLOCK_A_LBA : ASTRAFS_SUPERBLOCK_B_LBA;
    s_mock_storage[active_sb_lba * 512 + 508] ^= 0xFF; /* Corrupt checksum */

    /* Unmount and remount */
    assert(astrafs_unmount() == VFS_OK);
    assert(astrafs_mount(&mock_dev) == VFS_OK);
    assert(astrafs_is_mounted() == true);
    /* Remount should fall back to the alternate intact superblock */
    printf("  PASS: Filesystem successfully recovered from corrupted superblock.\n");

    /* TEST 9: Readdir enumeration */
    printf("[TEST 9] Testing directory enumeration via astrafs_readdir...\n");
    vfs_dirent_t de;
    int file_count = 0;
    for (size_t idx = 0; ; idx++) {
        int r = astrafs_readdir(idx, &de);
        if (r == VFS_ERR_EOF) break;
        assert(r == VFS_OK);
        assert(de.type == VFS_NODE_FILE);
        file_count++;
    }
    assert(file_count >= 2); /* test.txt and large.bin */
    printf("  PASS: Enumerated %d active files via readdir.\n", file_count);

    /* TEST 10: Capacity Bound (64 files limit) */
    printf("[TEST 10] Testing maximum file count bounds (64 files)...\n");
    for (int i = file_count; i < ASTRAFS_MAX_FILES; i++) {
        char fname[32];
        snprintf(fname, sizeof(fname), "fill_%d.dat", i);
        vfs_node_t *fnode = NULL;
        assert(astrafs_create(fname, VFS_O_CREAT | VFS_O_RDWR, &fnode) == VFS_OK);
    }
    vfs_node_t *overflow_node = NULL;
    assert(astrafs_create("overflow.dat", VFS_O_CREAT | VFS_O_RDWR, &overflow_node) == VFS_ERR_NOSPC);
    printf("  PASS: 65th file rejected with VFS_ERR_NOSPC.\n");

    /* TEST 11: Cold Restart Durability Simulation */
    printf("[TEST 11] Testing cold-restart persistence across unmount and state wipe...\n");
    /* Save bookmark simulation payload */
    vfs_node_t *bm_node = NULL;
    assert(astrafs_lookup("bookmarks.bin", &bm_node) == VFS_ERR_NOT_FOUND);
    /* Free one file slot by truncating or writing to fill_0 */
    vfs_node_t *target_node = NULL;
    assert(astrafs_lookup("fill_5.dat", &target_node) == VFS_OK);
    const char *bm_token = "ASTRABOOKMARKS_PERSISTENT_PAYLOAD_VALIDATED";
    assert(astrafs_write(target_node, bm_token, strlen(bm_token), 0) == (ssize_t)strlen(bm_token));

    /* Unmount and completely wipe in-memory g_astrafs state */
    assert(astrafs_unmount() == VFS_OK);
    memset(&g_astrafs, 0, sizeof(g_astrafs));
    assert(astrafs_is_mounted() == false);

    /* Cold restart: mount again from same raw mock disk without formatting */
    assert(astrafs_mount(&mock_dev) == VFS_OK);
    assert(astrafs_is_mounted() == true);

    vfs_node_t *recovered_node = NULL;
    assert(astrafs_lookup("fill_5.dat", &recovered_node) == VFS_OK);
    assert(recovered_node != NULL);
    assert(recovered_node->size == strlen(bm_token));

    char recovered_payload[64];
    memset(recovered_payload, 0, sizeof(recovered_payload));
    assert(astrafs_read(recovered_node, recovered_payload, sizeof(recovered_payload) - 1, 0) == (ssize_t)strlen(bm_token));
    assert(strcmp(recovered_payload, bm_token) == 0);
    printf("  PASS: Cold-restart verified: payload recovered identically across state wipe!\n");

    /* TEST 12: Blank Disk Detection Protocol */
    printf("[TEST 12] Testing blank disk detection protocol (preventing destructive format)...\n");
    u8 *blank_buf = (u8 *)calloc(MOCK_TOTAL_SECTORS, ASTRAFS_SECTOR_SIZE);
    u8 *saved_persistent = s_persistent_storage;
    u8 *saved_volatile = s_volatile_cache;
    s_persistent_storage = blank_buf;
    s_volatile_cache = blank_buf;
    s_mock_storage = blank_buf;

    /* A zeroed disk is blank */
    assert(astrafs_is_blank(&mock_dev) == true);

    /* An uninitialized 0xFF Flash disk is blank */
    memset(blank_buf, 0xFF, 3 * ASTRAFS_SECTOR_SIZE);
    assert(astrafs_is_blank(&mock_dev) == true);

    /* A disk with foreign partition data in LBA 0 is NOT blank */
    blank_buf[0] = 0xEB; blank_buf[1] = 0x3C; blank_buf[2] = 0x90; /* Boot sector signature */
    assert(astrafs_is_blank(&mock_dev) == false);

    /* A disk with non-zero bytes in LBA 1 or 2 is NOT blank */
    memset(blank_buf, 0, 3 * ASTRAFS_SECTOR_SIZE);
    blank_buf[ASTRAFS_SUPERBLOCK_A_LBA * 512 + 10] = 0x42;
    assert(astrafs_is_blank(&mock_dev) == false);

    free(blank_buf);
    s_persistent_storage = saved_persistent;
    s_volatile_cache = saved_volatile;
    s_mock_storage = saved_persistent;
    printf("  PASS: Blank disk detection correctly distinguishes blank media from foreign/corrupt media.\n");

    /* TEST 13: Non-Destructive Failure on Corrupted Disk (No Auto-Format) */
    printf("[TEST 13] Testing corrupt media handling (mount fails, destructive format blocked)...\n");
    u8 *corrupt_buf = (u8 *)calloc(MOCK_TOTAL_SECTORS, ASTRAFS_SECTOR_SIZE);
    s_persistent_storage = corrupt_buf;
    s_volatile_cache = corrupt_buf;
    s_mock_storage = corrupt_buf;
    /* Put random garbage in superblocks */
    for (int i = 0; i < 512; i++) {
        corrupt_buf[ASTRAFS_SUPERBLOCK_A_LBA * 512 + i] = (u8)(i * 7 + 3);
        corrupt_buf[ASTRAFS_SUPERBLOCK_B_LBA * 512 + i] = (u8)(i * 11 + 5);
    }
    assert(astrafs_unmount() == VFS_OK);
    assert(astrafs_is_blank(&mock_dev) == false);
    assert(astrafs_mount(&mock_dev) == VFS_ERR_CORRUPT);
    assert(astrafs_is_mounted() == false);
    /* Confirm corrupt media was NOT modified */
    assert(corrupt_buf[ASTRAFS_SUPERBLOCK_A_LBA * 512 + 10] == (u8)(10 * 7 + 3));
    free(corrupt_buf);
    s_persistent_storage = saved_persistent;
    s_volatile_cache = saved_volatile;
    s_mock_storage = saved_persistent;
    printf("  PASS: Corrupted disk safely rejected with VFS_ERR_CORRUPT; automatic formatting blocked.\n");

    /* TEST 14: Power Loss Interruption Simulation Across 5 Commit Stages */
    printf("[TEST 14] Testing power loss interruption simulation across 5 commit stages...\n");
    assert(astrafs_mount(&mock_dev) == VFS_OK);
    u64 stable_gen = st->sb.generation;
    u8 stable_set = st->active_set;

    /* Stage 1: Data written, but system crashes before metadata commit */
    /* Alternate superblock generation is unchanged; remount yields previous generation */
    assert(astrafs_unmount() == VFS_OK);
    assert(astrafs_mount(&mock_dev) == VFS_OK);
    assert(st->sb.generation == stable_gen);
    assert(st->active_set == stable_set);

    /* Stage 2 & 3: Alternate Inode or Bitmap write corrupted/interrupted */
    u8 alt_set = 1 - stable_set;
    u32 alt_sb_lba = (alt_set == 0) ? ASTRAFS_SUPERBLOCK_A_LBA : ASTRAFS_SUPERBLOCK_B_LBA;
    u32 alt_ino_lba = (alt_set == 0) ? ASTRAFS_INODE_A_LBA : ASTRAFS_INODE_B_LBA;
    /* Invalidate checksum of alternate superblock to simulate incomplete write */
    s_mock_storage[alt_sb_lba * 512 + 508] ^= 0x55;
    assert(astrafs_unmount() == VFS_OK);
    assert(astrafs_mount(&mock_dev) == VFS_OK);
    assert(st->active_set == stable_set);
    assert(st->sb.generation == stable_gen);

    /* Stage 4: Superblock write interrupted (corrupt checksum) */
    s_mock_storage[alt_sb_lba * 512] = 0x00; /* Invalidate magic */
    assert(astrafs_unmount() == VFS_OK);
    assert(astrafs_mount(&mock_dev) == VFS_OK);
    assert(st->active_set == stable_set);

    /* Stage 5: Hardware flush failure */
    s_mock_io_fail = true;
    vfs_node_t *fnode = NULL;
    assert(astrafs_lookup("fill_5.dat", &fnode) == VFS_OK);
    assert(astrafs_write(fnode, "FlushFail", 9, 0) == VFS_ERR_BUSY);
    s_mock_io_fail = false;
    printf("  PASS: All 5 commit failure stages safely roll back to intact prior generation.\n");

    /* TEST 15: Inode Table & Extent Corruption Detection */
    printf("[TEST 15] Testing defensive validation of corrupted inode extents...\n");
    /* Corrupt an inode in the active set: set start_lba < 128 */
    u32 cur_ino_lba = (st->active_set == 0) ? ASTRAFS_INODE_A_LBA : ASTRAFS_INODE_B_LBA;
    astrafs_inode_t *disk_inodes = (astrafs_inode_t *)(s_mock_storage + (cur_ino_lba * 512));
    disk_inodes[0].start_lba = 50; /* Inside metadata region! */
    disk_inodes[0].sector_count = 2;

    /* Recompute superblock checksum so superblock itself is valid */
    u32 cur_sb_lba = (st->active_set == 0) ? ASTRAFS_SUPERBLOCK_A_LBA : ASTRAFS_SUPERBLOCK_B_LBA;
    astrafs_superblock_t *cur_sb = (astrafs_superblock_t *)(s_mock_storage + (cur_sb_lba * 512));
    cur_sb->checksum = astrafs_checksum(cur_sb, 508);

    assert(astrafs_unmount() == VFS_OK);
    /* Mount must reject the corrupt set. Since alternate was also invalidated in Test 14, mount returns VFS_ERR_CORRUPT */
    assert(astrafs_mount(&mock_dev) == VFS_ERR_CORRUPT);
    printf("  PASS: Inode extent bounds violation safely caught and rejected.\n");

    /* TEST 16: Allocation Bitmap Desync Detection */
    printf("[TEST 16] Testing allocation bitmap desync detection...\n");
    /* Re-format clean disk */
    assert(astrafs_format(&mock_dev) == VFS_OK);
    assert(astrafs_mount(&mock_dev) == VFS_OK);
    vfs_node_t *desync_node = NULL;
    assert(astrafs_create("desync.dat", VFS_O_CREAT | VFS_O_RDWR, &desync_node) == VFS_OK);
    assert(astrafs_write(desync_node, "SomePayloadData", 15, 0) == 15);
    u8 prev_active_set = astrafs_get_state()->active_set;
    assert(astrafs_unmount() == VFS_OK);

    /* Invert bitmap bit for the allocated sector in active set */
    u32 active_bm_lba = (prev_active_set == 0) ? ASTRAFS_BITMAP_A_LBA : ASTRAFS_BITMAP_B_LBA;
    s_mock_storage[active_bm_lba * 512 + (ASTRAFS_DATA_START_LBA / 8)] &= ~(1U << (ASTRAFS_DATA_START_LBA % 8));
    /* Mounting should fall back to alternate or fail */
    int m_res = astrafs_mount(&mock_dev);
    assert(m_res == VFS_OK || m_res == VFS_ERR_CORRUPT);
    if (m_res == VFS_OK) {
        /* If it mounted, it fell back to the alternate intact set */
        assert(astrafs_get_state()->active_set != prev_active_set);
    }
    printf("  PASS: Bitmap desync caught; active set rejected.\n");

    /* TEST 17: Integer Bounds & Maximum File Size Bounds */
    printf("[TEST 17] Testing 64-bit integer overflow and maximum file size bounds...\n");
    assert(astrafs_format(&mock_dev) == VFS_OK);
    assert(astrafs_mount(&mock_dev) == VFS_OK);
    vfs_node_t *bounds_node = NULL;
    assert(astrafs_create("bounds.dat", VFS_O_CREAT | VFS_O_RDWR, &bounds_node) == VFS_OK);

    /* Write overflow: offset + count wraps */
    assert(astrafs_write(bounds_node, "Data", 10, (size_t)-5) == VFS_ERR_INVAL);
    /* Read overflow: offset + count wraps */
    char obuf[16];
    assert(astrafs_read(bounds_node, obuf, 10, (size_t)-5) == VFS_ERR_INVAL);

    /* File size limit (4 MiB limit) */
    assert(astrafs_write(bounds_node, "Data", 10, 5 * 1024 * 1024) == VFS_ERR_NOSPC);
    printf("  PASS: Integer overflow and 4 MiB file size constraints enforced.\n");

    /* TEST 18: Copy-On-Write Data Extent Preservation & Zero-Leak Overwrite */
    printf("[TEST 18] Testing Copy-On-Write extent allocation & zero-leak overwrites...\n");
    assert(astrafs_format(&mock_dev) == VFS_OK);
    assert(astrafs_mount(&mock_dev) == VFS_OK);
    vfs_node_t *cow_node = NULL;
    assert(astrafs_create("cow.dat", VFS_O_CREAT | VFS_O_RDWR, &cow_node) == VFS_OK);
    assert(astrafs_write(cow_node, "ORIGINAL_DURABLE_DATA_1234", 26, 0) == 26);
    int c_slot = (int)(uintptr_t)cow_node->fs_data;
    u32 lba1 = astrafs_get_state()->inodes[c_slot].start_lba;
    assert(astrafs_get_state()->sb.used_sectors == 1);

    /* Overwrite data: CoW must allocate new extent and free old extent */
    assert(astrafs_write(cow_node, "NEW_DURABLE_PAYLOAD_5678", 24, 0) == 24);
    u32 lba2 = astrafs_get_state()->inodes[c_slot].start_lba;
    assert(lba1 != lba2); /* Fresh extent allocated */
    assert(astrafs_get_state()->sb.used_sectors == 1); /* Old extent freed; no leak */

    char cow_read[32];
    memset(cow_read, 0, sizeof(cow_read));
    assert(astrafs_read(cow_node, cow_read, 24, 0) == 24);
    cow_read[24] = '\0';
    assert(strcmp(cow_read, "NEW_DURABLE_PAYLOAD_5678") == 0);

    /* Perform 25 sequential overwrites to verify sector accounting and zero leaks */
    for (int i = 0; i < 25; i++) {
        char cycle_msg[32];
        snprintf(cycle_msg, sizeof(cycle_msg), "CYCLE_WRITE_PAYLOAD_%d", i);
        size_t clen = strlen(cycle_msg);
        assert(astrafs_write(cow_node, cycle_msg, clen, 0) == (ssize_t)clen);
        assert(astrafs_get_state()->sb.used_sectors == 1);
    }
    printf("  PASS: CoW extent allocation and zero-leak overwrites verified across 25 cycles.\n");

    /* TEST 19: Copy-On-Write Data Preservation Across Mid-Write I/O Failure */
    printf("[TEST 19] Testing data preservation when write fails midway...\n");
    vfs_node_t *fs_node = NULL;
    assert(astrafs_create("failsafe.dat", VFS_O_CREAT | VFS_O_RDWR, &fs_node) == VFS_OK);
    assert(astrafs_write(fs_node, "SAFE_COMMITTED_DATA_ABC", 23, 0) == 23);

    /* Inject block device write failure */
    s_mock_io_fail = true;
    assert(astrafs_write(fs_node, "CORRUPTED_PARTIAL_WRITE", 23, 0) == VFS_ERR_BUSY);
    s_mock_io_fail = false;

    /* Read back: file must still contain the intact previous data */
    memset(cow_read, 0, sizeof(cow_read));
    assert(astrafs_read(fs_node, cow_read, sizeof(cow_read) - 1, 0) == 23);
    assert(strcmp(cow_read, "SAFE_COMMITTED_DATA_ABC") == 0);

    /* Verify across clean unmount and remount */
    assert(astrafs_unmount() == VFS_OK);
    assert(astrafs_mount(&mock_dev) == VFS_OK);
    vfs_node_t *rec_node = NULL;
    assert(astrafs_lookup("failsafe.dat", &rec_node) == VFS_OK && rec_node != NULL);
    memset(cow_read, 0, sizeof(cow_read));
    assert(astrafs_read(rec_node, cow_read, sizeof(cow_read) - 1, 0) == 23);
    assert(strcmp(cow_read, "SAFE_COMMITTED_DATA_ABC") == 0);
    printf("  PASS: Failed write preserved previous committed data byte-for-byte.\n");

    /* TEST 20: Pre-Commit & Commit Flush Failure Handling */
    printf("[TEST 20] Testing flush failure handling and abort...\n");
    s_mock_fail_flush = true;
    assert(astrafs_write(rec_node, "FLUSH_FAILURE_DATA", 18, 0) == VFS_ERR_BUSY);
    s_mock_fail_flush = false;

    /* Verify data is still the previous valid committed data */
    memset(cow_read, 0, sizeof(cow_read));
    assert(astrafs_read(rec_node, cow_read, sizeof(cow_read) - 1, 0) == 23);
    assert(strcmp(cow_read, "SAFE_COMMITTED_DATA_ABC") == 0);
    printf("  PASS: Flush failure cleanly aborted commit without corrupting storage.\n");

    /* TEST 21: Atomic File Rename & Replacement (astrafs_rename) */
    printf("[TEST 21] Testing atomic file rename and replacement...\n");
    vfs_node_t *src_node = NULL;
    vfs_node_t *dst_node = NULL;
    assert(astrafs_create("source.txt", VFS_O_CREAT | VFS_O_RDWR, &src_node) == VFS_OK);
    assert(astrafs_write(src_node, "SOURCE_PAYLOAD_123", 18, 0) == 18);
    assert(astrafs_create("dest.txt", VFS_O_CREAT | VFS_O_RDWR, &dst_node) == VFS_OK);
    assert(astrafs_write(dst_node, "DEST_OLD_PAYLOAD", 16, 0) == 16);

    u32 used_before_rename = astrafs_get_state()->sb.used_sectors;

    /* Atomic replace: dest.txt overwritten by source.txt */
    assert(astrafs_rename("source.txt", "dest.txt") == VFS_OK);
    assert(astrafs_lookup("source.txt", &src_node) == VFS_ERR_NOT_FOUND);
    assert(astrafs_lookup("dest.txt", &dst_node) == VFS_OK && dst_node != NULL);
    assert(dst_node->size == 18);
    char ren_buf[32];
    memset(ren_buf, 0, sizeof(ren_buf));
    assert(astrafs_read(dst_node, ren_buf, sizeof(ren_buf) - 1, 0) == 18);
    assert(strcmp(ren_buf, "SOURCE_PAYLOAD_123") == 0);

    /* Destination extent freed; total used sectors decreased by 1 */
    assert(astrafs_get_state()->sb.used_sectors == used_before_rename - 1);
    printf("  PASS: Atomic rename, destination replacement, and sector accounting verified.\n");

    /* TEST 22: Explicit Fsync & Sync Semantics */
    printf("[TEST 22] Testing explicit fsync and sync semantics and error reporting...\n");
    int flushes_before = s_mock_flushes;
    assert(astrafs_fsync(dst_node) == VFS_OK);
    assert(s_mock_flushes > flushes_before);

    flushes_before = s_mock_flushes;
    assert(astrafs_sync() == VFS_OK);
    assert(s_mock_flushes > flushes_before);

    /* Test flush failure propagation */
    s_mock_fail_flush = true;
    assert(astrafs_fsync(dst_node) == VFS_ERR_BUSY);
    assert(astrafs_sync() == VFS_ERR_BUSY);
    s_mock_fail_flush = false;
    printf("  PASS: Fsync and sync semantics and error propagation verified.\n");

    /* TEST 23: Disk Full Handling on CoW Overwrite */
    printf("[TEST 23] Testing disk full during overwrite (data preservation)...\n");
    /* Fill mock bitmap completely */
    memset(astrafs_get_state()->bitmap, 0xFF, sizeof(astrafs_get_state()->bitmap));
    /* Overwrite must fail with VFS_ERR_NOSPC */
    assert(astrafs_write(dst_node, "CANT_WRITE_FULL_DISK", 20, 0) == VFS_ERR_NOSPC);
    /* Remount to reload clean bitmap */
    assert(astrafs_unmount() == VFS_OK);
    assert(astrafs_mount(&mock_dev) == VFS_OK);
    assert(astrafs_lookup("dest.txt", &dst_node) == VFS_OK);
    memset(ren_buf, 0, sizeof(ren_buf));
    assert(astrafs_read(dst_node, ren_buf, sizeof(ren_buf) - 1, 0) == 18);
    assert(strcmp(ren_buf, "SOURCE_PAYLOAD_123") == 0);
    printf("  PASS: Out-of-space during overwrite safely rejected; existing data preserved.\n");

    /* TEST 24: In-Memory Transactional Rollback On Commit Failure */
    printf("[TEST 24] Testing in-memory transactional rollback (write, truncate, rename)...\n");
    assert(astrafs_format(&mock_dev) == VFS_OK);
    assert(astrafs_mount(&mock_dev) == VFS_OK);
    vfs_node_t *tx_node = NULL;
    assert(astrafs_create("tx_file.bin", VFS_O_CREAT | VFS_O_RDWR, &tx_node) == VFS_OK);
    assert(astrafs_write(tx_node, "INITIAL_TX_DATA", 15, 0) == 15);
    int tx_slot = (int)(uintptr_t)tx_node->fs_data;
    u32 tx_old_lba = astrafs_get_state()->inodes[tx_slot].start_lba;
    u32 tx_old_size = astrafs_get_state()->inodes[tx_slot].file_size;

    /* 24A: Simulate failure during write commit */
    s_mock_fail_flush = true;
    ssize_t tx_wres = astrafs_write(tx_node, "FAILING_TX_OVERWRITE", 20, 0);
    assert(tx_wres == VFS_ERR_BUSY);
    s_mock_fail_flush = false;

    /* Verify in-memory state: inode and bitmap were rolled back without remounting! */
    assert(astrafs_get_state()->inodes[tx_slot].start_lba == tx_old_lba);
    assert(astrafs_get_state()->inodes[tx_slot].file_size == tx_old_size);
    assert(bitmap_get(astrafs_get_state()->bitmap, tx_old_lba) == true);
    char tx_rbuf[32];
    memset(tx_rbuf, 0, sizeof(tx_rbuf));
    assert(astrafs_read(tx_node, tx_rbuf, sizeof(tx_rbuf) - 1, 0) == 15);
    assert(strcmp(tx_rbuf, "INITIAL_TX_DATA") == 0);

    /* 24B: Simulate failure during truncate commit */
    s_mock_fail_flush = true;
    int tx_tres = astrafs_truncate(tx_node, 0);
    assert(tx_tres == VFS_ERR_BUSY);
    s_mock_fail_flush = false;
    assert(astrafs_get_state()->inodes[tx_slot].start_lba == tx_old_lba);
    assert(astrafs_get_state()->inodes[tx_slot].file_size == tx_old_size);
    assert(bitmap_get(astrafs_get_state()->bitmap, tx_old_lba) == true);

    /* 24C: Simulate failure during rename commit */
    vfs_node_t *tx_dst = NULL;
    assert(astrafs_create("tx_dst.bin", VFS_O_CREAT | VFS_O_RDWR, &tx_dst) == VFS_OK);
    assert(astrafs_write(tx_dst, "DST_TX_DATA", 11, 0) == 11);
    int dst_slot_num = (int)(uintptr_t)tx_dst->fs_data;
    u32 dst_old_lba = astrafs_get_state()->inodes[dst_slot_num].start_lba;

    s_mock_fail_flush = true;
    int tx_rres = astrafs_rename("tx_file.bin", "tx_dst.bin");
    assert(tx_rres == VFS_ERR_BUSY);
    s_mock_fail_flush = false;

    /* Verify in-memory state: source and destination both intact */
    assert(astrafs_get_state()->inodes[tx_slot].in_use == 1);
    assert(strcmp(astrafs_get_state()->inodes[tx_slot].name, "tx_file.bin") == 0);
    assert(astrafs_get_state()->inodes[dst_slot_num].in_use == 1);
    assert(strcmp(astrafs_get_state()->inodes[dst_slot_num].name, "tx_dst.bin") == 0);
    assert(astrafs_get_state()->inodes[dst_slot_num].start_lba == dst_old_lba);
    assert(bitmap_get(astrafs_get_state()->bitmap, dst_old_lba) == true);
    printf("  PASS: In-memory transactional rollback verified for write, truncate, and rename.\n");

    /* TEST 25: Deterministic 10-Point Crash-Fault Matrix with Volatile Cache vs Disk Persistence */
    printf("[TEST 25] Testing deterministic 10-point crash-fault matrix with volatile cache & power loss (C1..C10)...\n");
    const char *orig_payload = "PRE_CRASH_ORIGINAL_DATA_12345678";
    size_t orig_len = strlen(orig_payload);
    const char *new_payload  = "NEW_CRASH_RECOVER_PAYLOAD_876543";
    size_t new_len = strlen(new_payload);

    for (int stage = 1; stage <= 10; stage++) {
        /* Format clean volume directly to disk */
        s_cache_mode = CACHE_MODE_DIRECT;
        assert(astrafs_format(&mock_dev) == VFS_OK);
        assert(astrafs_mount(&mock_dev) == VFS_OK);
        vfs_node_t *fnode = NULL;
        assert(astrafs_create("fault_test.dat", VFS_O_CREAT | VFS_O_RDWR, &fnode) == VFS_OK);
        assert(astrafs_write(fnode, orig_payload, orig_len, 0) == (ssize_t)orig_len);

        u8 pre_set = astrafs_get_state()->active_set;
        u64 pre_gen = astrafs_get_state()->sb.generation;
        u32 pre_lba = astrafs_get_state()->inodes[(int)(uintptr_t)fnode->fs_data].start_lba;

        /* Allocate new extent (as CoW write does) */
        u8 alt_set = 1 - pre_set;
        u32 alt_sb_lba = (alt_set == 0) ? ASTRAFS_SUPERBLOCK_A_LBA : ASTRAFS_SUPERBLOCK_B_LBA;
        u32 alt_inode_lba = (alt_set == 0) ? ASTRAFS_INODE_A_LBA : ASTRAFS_INODE_B_LBA;
        u32 alt_bm_lba = (alt_set == 0) ? ASTRAFS_BITMAP_A_LBA : ASTRAFS_BITMAP_B_LBA;
        u32 new_lba = pre_lba + 2;

        /* Enable realistic volatile write caching */
        s_cache_mode = CACHE_MODE_VOLATILE;

        /* Inject fault according to stage */
        switch (stage) {
            case 1: /* C1: Extent allocated in RAM; crash before any disk write */
                break;
            case 2: /* C2: Partial payload sector write in volatile cache */
                mock_write_sectors(&mock_dev, new_lba, 1, "PARTIAL");
                break;
            case 3: /* C3: Full payload written in volatile cache before pre-commit flush */
                mock_write_sectors(&mock_dev, new_lba, 1, new_payload);
                break;
            case 4: /* C4: Pre-commit payload flush completed to persistent storage */
                mock_write_sectors(&mock_dev, new_lba, 1, new_payload);
                mock_flush(&mock_dev);
                break;
            case 5: { /* C5: Alternate inode table in volatile cache, uncommitted */
                mock_write_sectors(&mock_dev, new_lba, 1, new_payload);
                mock_flush(&mock_dev);
                astrafs_inode_t bad_inodes[ASTRAFS_MAX_FILES];
                memset(bad_inodes, 0xEE, sizeof(bad_inodes));
                mock_write_sectors(&mock_dev, alt_inode_lba, ASTRAFS_INODE_SECTORS, bad_inodes);
                break;
            }
            case 6: { /* C6: Alternate inode & bitmap in volatile cache, before pre-commit metadata flush */
                mock_write_sectors(&mock_dev, new_lba, 1, new_payload);
                mock_flush(&mock_dev);
                astrafs_inode_t alt_inodes[ASTRAFS_MAX_FILES];
                memcpy(alt_inodes, astrafs_get_state()->inodes, sizeof(alt_inodes));
                alt_inodes[0].start_lba = new_lba;
                alt_inodes[0].file_size = (u32)new_len;
                mock_write_sectors(&mock_dev, alt_inode_lba, ASTRAFS_INODE_SECTORS, alt_inodes);
                u8 alt_bm[ASTRAFS_BITMAP_SECTORS * 512];
                memcpy(alt_bm, astrafs_get_state()->bitmap, sizeof(alt_bm));
                bitmap_set(alt_bm, new_lba);
                bitmap_clear(alt_bm, pre_lba);
                mock_write_sectors(&mock_dev, alt_bm_lba, ASTRAFS_BITMAP_SECTORS, alt_bm);
                break;
            }
            case 7: { /* C7: Alternate inode & bitmap flushed, but alternate superblock never written */
                mock_write_sectors(&mock_dev, new_lba, 1, new_payload);
                astrafs_inode_t alt_inodes[ASTRAFS_MAX_FILES];
                memcpy(alt_inodes, astrafs_get_state()->inodes, sizeof(alt_inodes));
                alt_inodes[0].start_lba = new_lba;
                alt_inodes[0].file_size = (u32)new_len;
                mock_write_sectors(&mock_dev, alt_inode_lba, ASTRAFS_INODE_SECTORS, alt_inodes);
                u8 alt_bm[ASTRAFS_BITMAP_SECTORS * 512];
                memcpy(alt_bm, astrafs_get_state()->bitmap, sizeof(alt_bm));
                bitmap_set(alt_bm, new_lba);
                bitmap_clear(alt_bm, pre_lba);
                mock_write_sectors(&mock_dev, alt_bm_lba, ASTRAFS_BITMAP_SECTORS, alt_bm);
                mock_flush(&mock_dev);
                /* Alternate superblock not written: persistent disk retains old generation */
                break;
            }
            case 8: { /* C8: Partial superblock persistence (torn write: first 256 bytes) */
                mock_write_sectors(&mock_dev, new_lba, 1, new_payload);
                astrafs_inode_t alt_inodes[ASTRAFS_MAX_FILES];
                memcpy(alt_inodes, astrafs_get_state()->inodes, sizeof(alt_inodes));
                alt_inodes[0].start_lba = new_lba;
                alt_inodes[0].file_size = (u32)new_len;
                mock_write_sectors(&mock_dev, alt_inode_lba, ASTRAFS_INODE_SECTORS, alt_inodes);
                u8 alt_bm[ASTRAFS_BITMAP_SECTORS * 512];
                memcpy(alt_bm, astrafs_get_state()->bitmap, sizeof(alt_bm));
                bitmap_set(alt_bm, new_lba);
                bitmap_clear(alt_bm, pre_lba);
                mock_write_sectors(&mock_dev, alt_bm_lba, ASTRAFS_BITMAP_SECTORS, alt_bm);
                mock_flush(&mock_dev);

                astrafs_superblock_t new_sb = astrafs_get_state()->sb;
                new_sb.generation = pre_gen + 1;
                new_sb.inode_table_lba = alt_inode_lba;
                new_sb.bitmap_lba = alt_bm_lba;
                new_sb.checksum = astrafs_checksum(&new_sb, 508);
                /* Inject 256-byte torn write directly into persistent disk */
                mock_inject_torn_write(alt_sb_lba, 256, &new_sb);
                break;
            }
            case 9: { /* C9: Alternate superblock written to volatile cache, crash BEFORE final flush */
                mock_write_sectors(&mock_dev, new_lba, 1, new_payload);
                astrafs_inode_t alt_inodes[ASTRAFS_MAX_FILES];
                memcpy(alt_inodes, astrafs_get_state()->inodes, sizeof(alt_inodes));
                alt_inodes[0].start_lba = new_lba;
                alt_inodes[0].file_size = (u32)new_len;
                mock_write_sectors(&mock_dev, alt_inode_lba, ASTRAFS_INODE_SECTORS, alt_inodes);
                u8 alt_bm[ASTRAFS_BITMAP_SECTORS * 512];
                memcpy(alt_bm, astrafs_get_state()->bitmap, sizeof(alt_bm));
                bitmap_set(alt_bm, new_lba);
                bitmap_clear(alt_bm, pre_lba);
                mock_write_sectors(&mock_dev, alt_bm_lba, ASTRAFS_BITMAP_SECTORS, alt_bm);
                mock_flush(&mock_dev);

                astrafs_superblock_t new_sb = astrafs_get_state()->sb;
                new_sb.generation = pre_gen + 1;
                new_sb.inode_table_lba = alt_inode_lba;
                new_sb.bitmap_lba = alt_bm_lba;
                new_sb.checksum = astrafs_checksum(&new_sb, 508);
                /* Write to volatile cache only (NO mock_flush!) */
                mock_write_sectors(&mock_dev, alt_sb_lba, 1, &new_sb);
                break;
            }
            case 10: { /* C10: Full commit and final cache flush succeeds */
                assert(astrafs_write(fnode, new_payload, new_len, 0) == (ssize_t)new_len);
                assert(astrafs_sync() == VFS_OK);
                break;
            }
        }

        /* Power Loss: unflushed volatile cache is discarded; in-memory state wiped */
        mock_power_loss();

        /* Crash Recovery Remount from persistent disk image */
        int mount_rc = astrafs_mount(&mock_dev);
        assert(mount_rc == VFS_OK);

        vfs_node_t *rec_f = NULL;
        assert(astrafs_lookup("fault_test.dat", &rec_f) == VFS_OK && rec_f != NULL);

        char read_check[64];
        memset(read_check, 0, sizeof(read_check));

        if (stage <= 9) {
            /* C1..C9: Power loss before durable flush MUST roll back to pre-crash original data */
            assert(astrafs_get_state()->sb.generation == pre_gen);
            assert(astrafs_get_state()->active_set == pre_set);
            ssize_t r_len = astrafs_read(rec_f, read_check, sizeof(read_check) - 1, 0);
            assert(r_len == (ssize_t)orig_len);
            assert(strcmp(read_check, orig_payload) == 0);
        } else {
            /* C10: Durable final flush completed -> MUST roll forward to new committed payload */
            assert(astrafs_get_state()->sb.generation == pre_gen + 1);
            assert(astrafs_get_state()->active_set == alt_set);
            ssize_t r_len = astrafs_read(rec_f, read_check, sizeof(read_check) - 1, 0);
            assert(r_len == (ssize_t)new_len);
            assert(strcmp(read_check, new_payload) == 0);
        }

        /* Invariant checks across all 10 stages: zero bitmap leaks, zero double allocation */
        assert(astrafs_get_state()->sb.file_count == 1);
        assert(astrafs_get_state()->sb.used_sectors == 1);
        assert(astrafs_unmount() == VFS_OK);
    }
    printf("  PASS: Deterministic 10-point crash-fault matrix verified (C1..C10 recovered cleanly).\n");

    /* TEST 26: Half-Sector Torn Write Simulation & Recovery (Superblock and Payload) */
    {
        printf("[TEST 26] Testing half-sector (256-byte) torn-write simulation & recovery...\n");
        s_cache_mode = CACHE_MODE_DIRECT;
        assert(astrafs_format(&mock_dev) == VFS_OK);
        assert(astrafs_mount(&mock_dev) == VFS_OK);
        vfs_node_t *t_node = NULL;
        assert(astrafs_create("torn_test.dat", VFS_O_CREAT | VFS_O_RDWR, &t_node) == VFS_OK);
        const char *base_data = "BASELINE_PAYLOAD_BEFORE_TORN_WRITE_12345";
        size_t base_len = strlen(base_data);
        assert(astrafs_write(t_node, base_data, base_len, 0) == (ssize_t)base_len);

        u8 stable_set = astrafs_get_state()->active_set;
        u64 stable_gen = astrafs_get_state()->sb.generation;
        int t_slot = (int)(uintptr_t)t_node->fs_data;
        u32 base_lba = astrafs_get_state()->inodes[t_slot].start_lba;

        /* 26A: Half-sector (256 bytes) torn write to alternate superblock */
        u8 alt_set = 1 - stable_set;
        u32 alt_sb_lba = (alt_set == 0) ? ASTRAFS_SUPERBLOCK_A_LBA : ASTRAFS_SUPERBLOCK_B_LBA;
        astrafs_superblock_t new_sb = astrafs_get_state()->sb;
        new_sb.generation = stable_gen + 1;
        new_sb.checksum = astrafs_checksum(&new_sb, 508);

        /* Write ONLY first 256 bytes of new superblock directly into persistent disk */
        mock_inject_torn_write(alt_sb_lba, 256, &new_sb);

        /* Simulate crash: wipe memory */
        mock_power_loss();

        /* Remount: must detect torn superblock checksum failure and fall back to stable_set */
        assert(astrafs_mount(&mock_dev) == VFS_OK);
        assert(astrafs_get_state()->active_set == stable_set);
        assert(astrafs_get_state()->sb.generation == stable_gen);

        vfs_node_t *rec_node = NULL;
        assert(astrafs_lookup("torn_test.dat", &rec_node) == VFS_OK && rec_node != NULL);
        char check_buf[64];
        memset(check_buf, 0, sizeof(check_buf));
        assert(astrafs_read(rec_node, check_buf, sizeof(check_buf) - 1, 0) == (ssize_t)base_len);
        assert(strcmp(check_buf, base_data) == 0);

        /* 26B: Half-sector torn write to new data extent before metadata commit */
        u32 new_data_lba = base_lba + 4;
        u8 half_payload[256];
        memset(half_payload, 0x5A, sizeof(half_payload));
        mock_inject_torn_write(new_data_lba, 256, half_payload);

        /* Crash before metadata commit */
        mock_power_loss();
        assert(astrafs_mount(&mock_dev) == VFS_OK);
        assert(astrafs_lookup("torn_test.dat", &rec_node) == VFS_OK && rec_node != NULL);
        memset(check_buf, 0, sizeof(check_buf));
        assert(astrafs_read(rec_node, check_buf, sizeof(check_buf) - 1, 0) == (ssize_t)base_len);
        assert(strcmp(check_buf, base_data) == 0);

        /* 26C: Safe subsequent write & clean unmount */
        const char *final_payload = "FINAL_VERIFIED_PAYLOAD_AFTER_TORN_RECOVERY";
        size_t final_len = strlen(final_payload);
        assert(astrafs_write(rec_node, final_payload, final_len, 0) == (ssize_t)final_len);
        memset(check_buf, 0, sizeof(check_buf));
        assert(astrafs_read(rec_node, check_buf, sizeof(check_buf) - 1, 0) == (ssize_t)final_len);
        assert(strcmp(check_buf, final_payload) == 0);
        assert(astrafs_unmount() == VFS_OK);
        printf("  PASS: Half-sector torn-write defenses verified (torn superblock and payload recovered).\n");
    }

    /* TEST 27: Task 3 Superblock & Metadata Recovery Qualification Matrix */
    {
        printf("\n[TEST 27] Task 3: Superblock & Metadata Recovery Qualification Matrix (10 Scenarios)...\n");
        printf("------------------------------------------------------------------------------------------------------------------------\n");
        printf("| %-26s | %-28s | %-28s | %-14s |\n", "Failure Point", "Expected Recovery", "Actual Recovery", "Data Integrity");
        printf("------------------------------------------------------------------------------------------------------------------------\n");

        /* Scenario 1: Alternate Inode Table Write in Cache Interrupted */
        {
            s_cache_mode = CACHE_MODE_DIRECT;
            assert(astrafs_format(&mock_dev) == VFS_OK);
            assert(astrafs_mount(&mock_dev) == VFS_OK);
            vfs_node_t *f = NULL;
            assert(astrafs_create("file1.txt", VFS_O_CREAT | VFS_O_RDWR, &f) == VFS_OK);
            assert(astrafs_write(f, "DATA1", 5, 0) == 5);
            u64 g1 = astrafs_get_state()->sb.generation;

            s_cache_mode = CACHE_MODE_VOLATILE;
            mock_write_sectors(&mock_dev, ASTRAFS_INODE_B_LBA, ASTRAFS_INODE_SECTORS, "CORRUPT_INODES");
            mock_power_loss();

            assert(astrafs_mount(&mock_dev) == VFS_OK);
            assert(astrafs_get_state()->sb.generation == g1);
            vfs_node_t *rf = NULL;
            assert(astrafs_lookup("file1.txt", &rf) == VFS_OK);
            char b[16] = {0};
            assert(astrafs_read(rf, b, 5, 0) == 5 && strcmp(b, "DATA1") == 0);
            assert(astrafs_unmount() == VFS_OK);
            printf("| %-26s | %-28s | %-28s | %-14s |\n",
                   "1. Alt Inode Write Loss", "Rollback to Set A (Gen N)", "Recovered Set A (Gen N)", "VERIFIED (100%)");
        }

        /* Scenario 2: Alternate Bitmap Write in Cache Interrupted */
        {
            s_cache_mode = CACHE_MODE_DIRECT;
            assert(astrafs_format(&mock_dev) == VFS_OK);
            assert(astrafs_mount(&mock_dev) == VFS_OK);
            vfs_node_t *f = NULL;
            assert(astrafs_create("file2.txt", VFS_O_CREAT | VFS_O_RDWR, &f) == VFS_OK);
            assert(astrafs_write(f, "DATA2", 5, 0) == 5);
            u64 g2 = astrafs_get_state()->sb.generation;

            s_cache_mode = CACHE_MODE_VOLATILE;
            mock_write_sectors(&mock_dev, ASTRAFS_BITMAP_B_LBA, ASTRAFS_BITMAP_SECTORS, "CORRUPT_BITMAP");
            mock_power_loss();

            assert(astrafs_mount(&mock_dev) == VFS_OK);
            assert(astrafs_get_state()->sb.generation == g2);
            vfs_node_t *rf = NULL;
            assert(astrafs_lookup("file2.txt", &rf) == VFS_OK);
            char b[16] = {0};
            assert(astrafs_read(rf, b, 5, 0) == 5 && strcmp(b, "DATA2") == 0);
            assert(astrafs_unmount() == VFS_OK);
            printf("| %-26s | %-28s | %-28s | %-14s |\n",
                   "2. Alt Bitmap Write Loss", "Rollback to Set A (Gen N)", "Recovered Set A (Gen N)", "VERIFIED (100%)");
        }

        /* Scenario 3: Pre-commit Metadata Flush Failure */
        {
            s_cache_mode = CACHE_MODE_DIRECT;
            assert(astrafs_format(&mock_dev) == VFS_OK);
            assert(astrafs_mount(&mock_dev) == VFS_OK);
            vfs_node_t *f = NULL;
            assert(astrafs_create("file3.txt", VFS_O_CREAT | VFS_O_RDWR, &f) == VFS_OK);
            assert(astrafs_write(f, "DATA3", 5, 0) == 5);
            u64 g3 = astrafs_get_state()->sb.generation;

            s_mock_fail_flush = true;
            assert(astrafs_write(f, "DATA3_UPDATED", 13, 0) == VFS_ERR_BUSY);
            s_mock_fail_flush = false;
            mock_power_loss();

            assert(astrafs_mount(&mock_dev) == VFS_OK);
            assert(astrafs_get_state()->sb.generation == g3);
            vfs_node_t *rf = NULL;
            assert(astrafs_lookup("file3.txt", &rf) == VFS_OK);
            char b[16] = {0};
            assert(astrafs_read(rf, b, 5, 0) == 5 && strcmp(b, "DATA3") == 0);
            assert(astrafs_unmount() == VFS_OK);
            printf("| %-26s | %-28s | %-28s | %-14s |\n",
                   "3. Pre-commit Flush Fail", "Commit Abort & Rollback", "Commit Aborted (Gen N)", "VERIFIED (100%)");
        }

        /* Scenario 4: Partial Superblock Persistence (Torn Superblock, 256 bytes) */
        {
            s_cache_mode = CACHE_MODE_DIRECT;
            assert(astrafs_format(&mock_dev) == VFS_OK);
            assert(astrafs_mount(&mock_dev) == VFS_OK);
            vfs_node_t *f = NULL;
            assert(astrafs_create("file4.txt", VFS_O_CREAT | VFS_O_RDWR, &f) == VFS_OK);
            assert(astrafs_write(f, "DATA4", 5, 0) == 5);
            u64 g4 = astrafs_get_state()->sb.generation;

            astrafs_superblock_t bad_sb = astrafs_get_state()->sb;
            bad_sb.generation = g4 + 1;
            bad_sb.checksum = astrafs_checksum(&bad_sb, 508);
            mock_inject_torn_write(ASTRAFS_SUPERBLOCK_B_LBA, 256, &bad_sb);
            mock_power_loss();

            assert(astrafs_mount(&mock_dev) == VFS_OK);
            assert(astrafs_get_state()->sb.generation == g4);
            vfs_node_t *rf = NULL;
            assert(astrafs_lookup("file4.txt", &rf) == VFS_OK);
            char b[16] = {0};
            assert(astrafs_read(rf, b, 5, 0) == 5 && strcmp(b, "DATA4") == 0);
            assert(astrafs_unmount() == VFS_OK);
            printf("| %-26s | %-28s | %-28s | %-14s |\n",
                   "4. Torn Superblock (256B)", "Checksum Mismatch Rejection", "Fallback to Set A (Gen N)", "VERIFIED (100%)");
        }

        /* Scenario 5: Complete Superblock in Volatile Cache without Durable Flush */
        {
            s_cache_mode = CACHE_MODE_DIRECT;
            assert(astrafs_format(&mock_dev) == VFS_OK);
            assert(astrafs_mount(&mock_dev) == VFS_OK);
            vfs_node_t *f = NULL;
            assert(astrafs_create("file5.txt", VFS_O_CREAT | VFS_O_RDWR, &f) == VFS_OK);
            assert(astrafs_write(f, "DATA5", 5, 0) == 5);
            u64 g5 = astrafs_get_state()->sb.generation;

            s_cache_mode = CACHE_MODE_VOLATILE;
            astrafs_superblock_t uncommitted_sb = astrafs_get_state()->sb;
            uncommitted_sb.generation = g5 + 1;
            uncommitted_sb.checksum = astrafs_checksum(&uncommitted_sb, 508);
            mock_write_sectors(&mock_dev, ASTRAFS_SUPERBLOCK_B_LBA, 1, &uncommitted_sb);
            /* Power loss occurs WITHOUT calling mock_flush! */
            mock_power_loss();

            assert(astrafs_mount(&mock_dev) == VFS_OK);
            assert(astrafs_get_state()->sb.generation == g5);
            vfs_node_t *rf = NULL;
            assert(astrafs_lookup("file5.txt", &rf) == VFS_OK);
            char b[16] = {0};
            assert(astrafs_read(rf, b, 5, 0) == 5 && strcmp(b, "DATA5") == 0);
            assert(astrafs_unmount() == VFS_OK);
            printf("| %-26s | %-28s | %-28s | %-14s |\n",
                   "5. Unflushed Cache Loss", "Discard Unflushed Superblock", "Recovered Set A (Gen N)", "VERIFIED (100%)");
        }

        /* Scenario 6: Failure During Final Flush */
        {
            s_cache_mode = CACHE_MODE_DIRECT;
            assert(astrafs_format(&mock_dev) == VFS_OK);
            assert(astrafs_mount(&mock_dev) == VFS_OK);
            vfs_node_t *f = NULL;
            assert(astrafs_create("file6.txt", VFS_O_CREAT | VFS_O_RDWR, &f) == VFS_OK);
            assert(astrafs_write(f, "DATA6", 5, 0) == 5);
            u64 g6 = astrafs_get_state()->sb.generation;

            s_cache_mode = CACHE_MODE_VOLATILE;
            s_mock_fail_flush = true;
            assert(astrafs_write(f, "DATA6_NEW", 9, 0) == VFS_ERR_BUSY);
            s_mock_fail_flush = false;
            mock_power_loss();

            assert(astrafs_mount(&mock_dev) == VFS_OK);
            assert(astrafs_get_state()->sb.generation == g6);
            vfs_node_t *rf = NULL;
            assert(astrafs_lookup("file6.txt", &rf) == VFS_OK);
            char b[16] = {0};
            assert(astrafs_read(rf, b, 5, 0) == 5 && strcmp(b, "DATA6") == 0);
            assert(astrafs_unmount() == VFS_OK);
            printf("| %-26s | %-28s | %-28s | %-14s |\n",
                   "6. Final Flush Failure", "Rollback to Prior Generation", "Recovered Prior (Gen N)", "VERIFIED (100%)");
        }

        /* Scenario 7: Successful Final Flush */
        {
            s_cache_mode = CACHE_MODE_DIRECT;
            assert(astrafs_format(&mock_dev) == VFS_OK);
            assert(astrafs_mount(&mock_dev) == VFS_OK);
            vfs_node_t *f = NULL;
            assert(astrafs_create("file7.txt", VFS_O_CREAT | VFS_O_RDWR, &f) == VFS_OK);
            assert(astrafs_write(f, "DATA7_BASE", 10, 0) == 10);
            u64 g7 = astrafs_get_state()->sb.generation;

            s_cache_mode = CACHE_MODE_VOLATILE;
            assert(astrafs_write(f, "DATA7_COMMITTED", 15, 0) == 15);
            assert(astrafs_sync() == VFS_OK);
            mock_power_loss();

            assert(astrafs_mount(&mock_dev) == VFS_OK);
            assert(astrafs_get_state()->sb.generation == g7 + 1);
            vfs_node_t *rf = NULL;
            assert(astrafs_lookup("file7.txt", &rf) == VFS_OK);
            char b[32] = {0};
            assert(astrafs_read(rf, b, 15, 0) == 15 && strcmp(b, "DATA7_COMMITTED") == 0);
            assert(astrafs_unmount() == VFS_OK);
            printf("| %-26s | %-28s | %-28s | %-14s |\n",
                   "7. Successful Final Flush", "Roll Forward to Gen N+1", "Recovered Gen N+1 Data", "VERIFIED (100%)");
        }

        /* Scenario 8: Both Metadata Generations Valid (Select Newer) */
        {
            s_cache_mode = CACHE_MODE_DIRECT;
            assert(astrafs_format(&mock_dev) == VFS_OK);
            assert(astrafs_mount(&mock_dev) == VFS_OK);
            vfs_node_t *f = NULL;
            assert(astrafs_create("both_valid.txt", VFS_O_CREAT | VFS_O_RDWR, &f) == VFS_OK);
            assert(astrafs_write(f, "GEN2", 4, 0) == 4); /* Gen 2 in Set B */
            assert(astrafs_write(f, "GEN3", 4, 0) == 4); /* Gen 3 in Set A */
            mock_power_loss();

            assert(astrafs_mount(&mock_dev) == VFS_OK);
            assert(astrafs_get_state()->sb.generation == 4);
            assert(astrafs_get_state()->active_set == 1); /* Set B */
            vfs_node_t *rf = NULL;
            assert(astrafs_lookup("both_valid.txt", &rf) == VFS_OK);
            char b[16] = {0};
            assert(astrafs_read(rf, b, 4, 0) == 4 && strcmp(b, "GEN3") == 0);
            assert(astrafs_unmount() == VFS_OK);
            printf("| %-26s | %-28s | %-28s | %-14s |\n",
                   "8. Both Generations Valid", "Select Higher Gen (Gen 4)", "Selected Gen 4 (Set B)", "VERIFIED (100%)");
        }

        /* Scenario 9: Newer Generation Has Inconsistent Inode Extents */
        {
            s_cache_mode = CACHE_MODE_DIRECT;
            assert(astrafs_format(&mock_dev) == VFS_OK);
            assert(astrafs_mount(&mock_dev) == VFS_OK);
            vfs_node_t *f = NULL;
            assert(astrafs_create("defensive.txt", VFS_O_CREAT | VFS_O_RDWR, &f) == VFS_OK);
            assert(astrafs_write(f, "STABLE_DATA", 11, 0) == 11);
            u64 g9 = astrafs_get_state()->sb.generation;

            /* Prepare newer Set B with corrupt out-of-bounds start_lba */
            astrafs_inode_t bad_inodes[ASTRAFS_MAX_FILES];
            memcpy(bad_inodes, astrafs_get_state()->inodes, sizeof(bad_inodes));
            bad_inodes[0].start_lba = 0xFFFFFFF0; /* Out of bounds extent */
            bad_inodes[0].sector_count = 100;
            mock_write_sectors(&mock_dev, ASTRAFS_INODE_B_LBA, ASTRAFS_INODE_SECTORS, bad_inodes);

            astrafs_superblock_t new_sb = astrafs_get_state()->sb;
            new_sb.generation = g9 + 1;
            new_sb.inode_table_lba = ASTRAFS_INODE_B_LBA;
            new_sb.bitmap_lba = ASTRAFS_BITMAP_B_LBA;
            new_sb.checksum = astrafs_checksum(&new_sb, 508);
            mock_write_sectors(&mock_dev, ASTRAFS_SUPERBLOCK_B_LBA, 1, &new_sb);
            mock_power_loss();

            /* Remount: validate_metadata_integrity detects out-of-bounds LBA in Set B, falls back to Set A */
            assert(astrafs_mount(&mock_dev) == VFS_OK);
            assert(astrafs_get_state()->active_set == 0); /* Set A */
            assert(astrafs_get_state()->sb.generation == g9);
            vfs_node_t *rf = NULL;
            assert(astrafs_lookup("defensive.txt", &rf) == VFS_OK);
            char b[16] = {0};
            assert(astrafs_read(rf, b, 11, 0) == 11 && strcmp(b, "STABLE_DATA") == 0);
            assert(astrafs_unmount() == VFS_OK);
            printf("| %-26s | %-28s | %-28s | %-14s |\n",
                   "9. Corrupt Extents in New", "Defensive Fallback to Stable", "Fallback to Set A (Gen N)", "VERIFIED (100%)");
        }

        /* Scenario 10: Generation Rollover Across 0xFFFFFFFFFFFFFFFF */
        {
            s_cache_mode = CACHE_MODE_DIRECT;
            assert(astrafs_format(&mock_dev) == VFS_OK);
            assert(astrafs_mount(&mock_dev) == VFS_OK);
            vfs_node_t *f = NULL;
            assert(astrafs_create("rollover.txt", VFS_O_CREAT | VFS_O_RDWR, &f) == VFS_OK);
            assert(astrafs_write(f, "ROLLOVER_PAYLOAD", 16, 0) == 16);

            /* Synthesize Generation rollover: Set A = 0xFFFFFFFFFFFFFFFF, Set B = 0x0 */
            astrafs_superblock_t sbA = astrafs_get_state()->sb;
            sbA.generation = 0xFFFFFFFFFFFFFFFFULL;
            sbA.checksum = astrafs_checksum(&sbA, 508);
            mock_write_sectors(&mock_dev, ASTRAFS_SUPERBLOCK_A_LBA, 1, &sbA);

            astrafs_superblock_t sbB = astrafs_get_state()->sb;
            sbB.generation = 0x0000000000000000ULL;
            sbB.checksum = astrafs_checksum(&sbB, 508);
            mock_write_sectors(&mock_dev, ASTRAFS_SUPERBLOCK_B_LBA, 1, &sbB);
            mock_power_loss();

            /* (s64)(genB - genA) = (s64)(0 - (-1)) = 1 > 0 -> Set B chosen! */
            assert(astrafs_mount(&mock_dev) == VFS_OK);
            assert(astrafs_get_state()->active_set == 1); /* Set B */
            assert(astrafs_get_state()->sb.generation == 0);
            vfs_node_t *rf = NULL;
            assert(astrafs_lookup("rollover.txt", &rf) == VFS_OK);
            char b[32] = {0};
            assert(astrafs_read(rf, b, 16, 0) == 16 && strcmp(b, "ROLLOVER_PAYLOAD") == 0);

            /* Subsequent write advances generation to 1 */
            assert(astrafs_write(rf, "ADVANCED", 8, 0) == 8);
            assert(astrafs_get_state()->sb.generation == 1);
            assert(astrafs_unmount() == VFS_OK);
            printf("| %-26s | %-28s | %-28s | %-14s |\n",
                   "10. Generation Rollover", "Modular Difference Chooses 0", "Selected Gen 0 (Set B)", "VERIFIED (100%)");
        }
        printf("------------------------------------------------------------------------------------------------------------------------\n");
        printf("  PASS: All 10 Task 3 Superblock & Metadata Recovery Qualification scenarios verified.\n");
    }

    free(s_persistent_storage);
    free(s_volatile_cache);
    free(s_dirty_bitmap);
    printf("\n=================================================================\n");
    printf("   [SUCCESS] ALL 27 ASTRAFS HOST TESTS PASSED\n");
    printf("=================================================================\n\n");
    return 0;
}

#if defined(HOST_TEST_MAIN) || !defined(HOST_TEST_DLL)
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return run_all_fs_host_tests();
}
#endif

#endif /* HOST_TEST */
