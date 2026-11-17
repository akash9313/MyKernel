#include <kernel/astrafs.h>
#include <kernel/mem.h>
#include <kernel/kprintf.h>
#include <kernel/panic.h>

static astrafs_state_t g_astrafs;

#ifndef HOST_TEST
#include <kernel/sync.h>
#include <kernel/sched.h>
static mutex_t g_astrafs_lock;
static bool g_astrafs_lock_initialized = false;

typedef struct {
    volatile int workers_done;
    volatile int errors;
} fs_conc_test_state_t;

static fs_conc_test_state_t g_fs_conc_state;

static void worker_fs_conc_a(void *arg) {
    (void)arg;
    vfs_node_t *na = NULL;
    int err = astrafs_create("conc_a.txt", VFS_O_CREAT | VFS_O_RDWR, &na);
    if (err != VFS_OK || !na) {
        __atomic_fetch_add(&g_fs_conc_state.errors, 1, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&g_fs_conc_state.workers_done, 1, __ATOMIC_SEQ_CST);
        return;
    }
    for (int i = 0; i < 5; i++) {
        const char *msg = "PAYLOAD_A_CONCURRENT";
        ssize_t w = astrafs_write(na, msg, 20, 0);
        if (w != 20) {
            __atomic_fetch_add(&g_fs_conc_state.errors, 1, __ATOMIC_SEQ_CST);
        }
        thread_yield();
    }
    astrafs_fsync(na);
    __atomic_fetch_add(&g_fs_conc_state.workers_done, 1, __ATOMIC_SEQ_CST);
}

static void worker_fs_conc_b(void *arg) {
    (void)arg;
    vfs_node_t *nb = NULL;
    int err = astrafs_create("conc_b.txt", VFS_O_CREAT | VFS_O_RDWR, &nb);
    if (err != VFS_OK || !nb) {
        __atomic_fetch_add(&g_fs_conc_state.errors, 1, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&g_fs_conc_state.workers_done, 1, __ATOMIC_SEQ_CST);
        return;
    }
    for (int i = 0; i < 5; i++) {
        const char *msg = "PAYLOAD_B_CONCURRENT";
        ssize_t w = astrafs_write(nb, msg, 20, 0);
        if (w != 20) {
            __atomic_fetch_add(&g_fs_conc_state.errors, 1, __ATOMIC_SEQ_CST);
        }
        thread_yield();
    }
    astrafs_fsync(nb);
    __atomic_fetch_add(&g_fs_conc_state.workers_done, 1, __ATOMIC_SEQ_CST);
}

static inline void astrafs_lock(void) {
    if (!g_astrafs_lock_initialized) {
        mutex_init(&g_astrafs_lock);
        g_astrafs_lock_initialized = true;
    }
    mutex_lock(&g_astrafs_lock);
}

static inline void astrafs_unlock(void) {
    if (g_astrafs_lock_initialized) {
        mutex_unlock(&g_astrafs_lock);
    }
}
#else
static inline void astrafs_lock(void) {}
static inline void astrafs_unlock(void) {}
#endif

/* String helpers */
static size_t kstrlen(const char *s) {
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static int kstrcmp(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static int kstrncmp(const char *a, const char *b, size_t n) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0') {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }
    return 0;
}

static void kstrncpy(char *dst, const char *src, size_t max) {
    if (!dst || max == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < max) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/* Bitmap helpers (1 bit per sector: 1 = used/reserved, 0 = free) */
static inline bool bitmap_get(const u8 *bm, u32 bit) {
    return (bm[bit / 8] & (1U << (bit % 8))) != 0;
}

static inline void bitmap_set(u8 *bm, u32 bit) {
    bm[bit / 8] |= (1U << (bit % 8));
}

static inline void bitmap_clear(u8 *bm, u32 bit) {
    bm[bit / 8] &= ~(1U << (bit % 8));
}

static u32 bitmap_alloc_extent(u8 *bm, u32 total_sectors, u32 count) {
    if (count == 0 || count > total_sectors) return 0;
    u32 run_start = 0;
    u32 run_length = 0;

    for (u32 s = ASTRAFS_DATA_START_LBA; s < total_sectors; s++) {
        if (!bitmap_get(bm, s)) {
            if (run_length == 0) {
                run_start = s;
            }
            run_length++;
            if (run_length == count) {
                /* Allocate contiguous extent */
                for (u32 i = 0; i < count; i++) {
                    bitmap_set(bm, run_start + i);
                }
                return run_start;
            }
        } else {
            run_length = 0;
        }
    }
    return 0; /* No contiguous extent found */
}

static void bitmap_free_extent(u8 *bm, u32 start_lba, u32 count) {
    if (start_lba < ASTRAFS_DATA_START_LBA || count == 0) return;
    for (u32 i = 0; i < count; i++) {
        bitmap_clear(bm, start_lba + i);
    }
}

static const char *strip_data_prefix(const char *name) {
    if (!name) return "";
    if (kstrncmp(name, "/data/", 6) == 0) {
        return name + 6;
    }
    if (kstrcmp(name, "/data") == 0) {
        return "";
    }
    if (name[0] == '/') {
        return name + 1;
    }
    return name;
}

/* VFS Operations Forward Declarations */
static int astrafs_op_open(vfs_node_t *node, u32 flags);
static int astrafs_op_close(vfs_node_t *node);
static ssize_t astrafs_op_read(vfs_node_t *node, void *buf, size_t count, size_t offset);
static ssize_t astrafs_op_write(vfs_node_t *node, const void *buf, size_t count, size_t offset);
static int astrafs_op_stat(vfs_node_t *node, vfs_stat_t *st);
static int astrafs_op_readdir(vfs_node_t *node, size_t index, vfs_dirent_t *dirent);
static int astrafs_op_lookup(vfs_node_t *parent, const char *name, vfs_node_t **out_node);
static int astrafs_op_fsync(vfs_node_t *node);
static int astrafs_op_rename(vfs_node_t *parent, const char *old_name, const char *new_name);

static const vfs_ops_t g_astrafs_ops = {
    .open    = astrafs_op_open,
    .close   = astrafs_op_close,
    .read    = astrafs_op_read,
    .write   = astrafs_op_write,
    .stat    = astrafs_op_stat,
    .readdir = astrafs_op_readdir,
    .lookup  = astrafs_op_lookup,
    .fsync   = astrafs_op_fsync,
    .rename  = astrafs_op_rename,
};

const vfs_ops_t *astrafs_get_ops(void) {
    return &g_astrafs_ops;
}

vfs_node_t *astrafs_get_root_node(void) {
    return &g_astrafs.root_node;
}

bool astrafs_is_mounted(void) {
    return g_astrafs.mounted;
}

astrafs_state_t *astrafs_get_state(void) {
    return &g_astrafs;
}

/* Atomic Dual-Set Metadata Commit */
static int astrafs_commit_metadata(void) {
    if (!g_astrafs.mounted || !g_astrafs.dev) {
        return VFS_ERR_INVAL;
    }

    u8 alt_set = 1 - g_astrafs.active_set;
    u32 alt_inode_lba = (alt_set == 0) ? ASTRAFS_INODE_A_LBA : ASTRAFS_INODE_B_LBA;
    u32 alt_bitmap_lba = (alt_set == 0) ? ASTRAFS_BITMAP_A_LBA : ASTRAFS_BITMAP_B_LBA;
    u32 alt_sb_lba = (alt_set == 0) ? ASTRAFS_SUPERBLOCK_A_LBA : ASTRAFS_SUPERBLOCK_B_LBA;

    /* 1. Write Inode Table to alternate metadata region */
    int ret = block_write(g_astrafs.dev, alt_inode_lba, ASTRAFS_INODE_SECTORS, g_astrafs.inodes);
    if (ret != BLOCK_OK) return VFS_ERR_BUSY;

    /* 2. Write Allocation Bitmap to alternate metadata region */
    ret = block_write(g_astrafs.dev, alt_bitmap_lba, ASTRAFS_BITMAP_SECTORS, g_astrafs.bitmap);
    if (ret != BLOCK_OK) return VFS_ERR_BUSY;

    /* 3. Flush alternate tables to disk before committing alternate superblock */
    ret = block_flush(g_astrafs.dev);
    if (ret != BLOCK_OK) return VFS_ERR_BUSY;

    /* 4. Prepare Alternate Superblock */
    astrafs_superblock_t alt_sb = g_astrafs.sb;
    alt_sb.generation = g_astrafs.sb.generation + 1;
    alt_sb.inode_table_lba = alt_inode_lba;
    alt_sb.bitmap_lba = alt_bitmap_lba;

    u32 fc = 0;
    u32 used = 0;
    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (g_astrafs.inodes[i].in_use) {
            fc++;
            used += g_astrafs.inodes[i].sector_count;
        }
    }
    alt_sb.file_count = fc;
    alt_sb.used_sectors = used;
    alt_sb.checksum = astrafs_checksum(&alt_sb, 508);

    /* 4. Write Alternate Superblock */
    ret = block_write(g_astrafs.dev, alt_sb_lba, 1, &alt_sb);
    if (ret != BLOCK_OK) return VFS_ERR_BUSY;

    /* 5. Flush disk cache */
    ret = block_flush(g_astrafs.dev);
    if (ret != BLOCK_OK) return VFS_ERR_BUSY;

    /* 6. Switch active set in memory */
    g_astrafs.active_set = alt_set;
    g_astrafs.sb = alt_sb;

    return VFS_OK;
}

int astrafs_format(block_device_t *dev) {
    if (!dev || !dev->ops || !dev->ops->write_sectors) {
        return VFS_ERR_INVAL;
    }
    if (dev->read_only || dev->is_boot_device) {
        return VFS_ERR_READ_ONLY;
    }
    if (dev->sector_size != ASTRAFS_SECTOR_SIZE) {
        return VFS_ERR_UNSUPPORTED;
    }

    /* Initialize zeroed inode table */
    static astrafs_inode_t clean_inodes[ASTRAFS_MAX_FILES];
    kmemset(clean_inodes, 0, sizeof(clean_inodes));

    /* Initialize bitmap: sectors 0..127 reserved (16 bytes = 128 bits set to 1) */
    static u8 clean_bitmap[ASTRAFS_BITMAP_SECTORS * ASTRAFS_SECTOR_SIZE];
    kmemset(clean_bitmap, 0, sizeof(clean_bitmap));
    for (u32 s = 0; s < ASTRAFS_DATA_START_LBA; s++) {
        bitmap_set(clean_bitmap, s);
    }

    /* Write Inode Tables A and B */
    if (block_write(dev, ASTRAFS_INODE_A_LBA, ASTRAFS_INODE_SECTORS, clean_inodes) != BLOCK_OK) {
        return VFS_ERR_BUSY;
    }
    if (block_write(dev, ASTRAFS_INODE_B_LBA, ASTRAFS_INODE_SECTORS, clean_inodes) != BLOCK_OK) {
        return VFS_ERR_BUSY;
    }

    /* Write Allocation Bitmaps A and B */
    if (block_write(dev, ASTRAFS_BITMAP_A_LBA, ASTRAFS_BITMAP_SECTORS, clean_bitmap) != BLOCK_OK) {
        return VFS_ERR_BUSY;
    }
    if (block_write(dev, ASTRAFS_BITMAP_B_LBA, ASTRAFS_BITMAP_SECTORS, clean_bitmap) != BLOCK_OK) {
        return VFS_ERR_BUSY;
    }

    /* Superblock A */
    astrafs_superblock_t sbA;
    kmemset(&sbA, 0, sizeof(sbA));
    sbA.magic = ASTRAFS_MAGIC;
    sbA.version = ASTRAFS_VERSION;
    sbA.generation = 1;
    sbA.sector_size = ASTRAFS_SECTOR_SIZE;
    u32 vol_sectors = (u32)dev->total_sectors;
    if (vol_sectors > ASTRAFS_MAX_SECTORS) vol_sectors = ASTRAFS_MAX_SECTORS;
    sbA.total_sectors = vol_sectors;
    sbA.data_start_lba = ASTRAFS_DATA_START_LBA;
    sbA.inode_table_lba = ASTRAFS_INODE_A_LBA;
    sbA.inode_sectors = ASTRAFS_INODE_SECTORS;
    sbA.bitmap_lba = ASTRAFS_BITMAP_A_LBA;
    sbA.bitmap_sectors = ASTRAFS_BITMAP_SECTORS;
    sbA.file_count = 0;
    sbA.used_sectors = 0;
    sbA.checksum = astrafs_checksum(&sbA, 508);

    /* Superblock B */
    astrafs_superblock_t sbB = sbA;
    sbB.inode_table_lba = ASTRAFS_INODE_B_LBA;
    sbB.bitmap_lba = ASTRAFS_BITMAP_B_LBA;
    sbB.checksum = astrafs_checksum(&sbB, 508);

    if (block_write(dev, ASTRAFS_SUPERBLOCK_A_LBA, 1, &sbA) != BLOCK_OK) {
        return VFS_ERR_BUSY;
    }
    if (block_write(dev, ASTRAFS_SUPERBLOCK_B_LBA, 1, &sbB) != BLOCK_OK) {
        return VFS_ERR_BUSY;
    }

    if (block_flush(dev) != BLOCK_OK) {
        return VFS_ERR_BUSY;
    }

    kprintf("[ASTRAFS] Formatted volume on '%s': %llu sectors, dual metadata sets A & B initialized.\n",
            dev->name, (unsigned long long)dev->total_sectors);
    return VFS_OK;
}

bool astrafs_is_blank(block_device_t *dev) {
    if (!dev || !dev->ops || !dev->ops->read_sectors) return false;
    static u8 s_sector[ASTRAFS_SECTOR_SIZE];

    /* Inspect LBA 0 (MBR), LBA 1 (Superblock A), LBA 2 (Superblock B) */
    for (u32 lba = 0; lba <= 2; lba++) {
        if (block_read(dev, lba, 1, s_sector) != BLOCK_OK) {
            return false;
        }
        for (size_t b = 0; b < ASTRAFS_SECTOR_SIZE; b++) {
            if (s_sector[b] != 0 && s_sector[b] != 0xFF) {
                return false; /* Non-blank byte detected */
            }
        }
    }
    return true;
}

static bool validate_metadata_integrity(block_device_t *dev, const astrafs_superblock_t *sb, const astrafs_inode_t *inodes, const u8 *bitmap) {
    if (!dev || !sb || !inodes || !bitmap) return false;

    if (sb->magic != ASTRAFS_MAGIC || sb->version != ASTRAFS_VERSION) return false;
    if (sb->sector_size != ASTRAFS_SECTOR_SIZE) return false;
    if (sb->total_sectors < 256 || sb->total_sectors > dev->total_sectors) return false;
    if (sb->data_start_lba != ASTRAFS_DATA_START_LBA) return false;
    if (sb->inode_sectors != ASTRAFS_INODE_SECTORS || sb->bitmap_sectors != ASTRAFS_BITMAP_SECTORS) return false;
    if (sb->file_count > ASTRAFS_MAX_FILES) return false;
    if (sb->checksum != astrafs_checksum(sb, 508)) return false;

    /* Verify reserved bits 0..127 in bitmap are marked used */
    for (u32 s = 0; s < ASTRAFS_DATA_START_LBA; s++) {
        if (!bitmap_get(bitmap, s)) return false;
    }

    u32 active_files = 0;
    u32 calculated_used_sectors = 0;

    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (!inodes[i].in_use) continue;
        active_files++;

        size_t nlen = kstrlen(inodes[i].name);
        if (nlen == 0 || nlen >= ASTRAFS_FILENAME_MAX) return false;

        /* Verify max file size */
        if (inodes[i].file_size > (4 * 1024 * 1024)) return false;

        u32 req_sec = (inodes[i].file_size == 0) ? 0 : (u32)((inodes[i].file_size + ASTRAFS_SECTOR_SIZE - 1) / ASTRAFS_SECTOR_SIZE);
        if (inodes[i].sector_count < req_sec) return false;

        if (inodes[i].sector_count > 0) {
            if (inodes[i].start_lba < ASTRAFS_DATA_START_LBA) return false;
            if (inodes[i].start_lba >= sb->total_sectors || inodes[i].sector_count > sb->total_sectors - inodes[i].start_lba) return false;

            /* Check non-overlapping extents against preceding in-use files */
            for (int j = 0; j < i; j++) {
                if (!inodes[j].in_use || inodes[j].sector_count == 0) continue;
                u32 startA = inodes[i].start_lba;
                u32 endA = startA + inodes[i].sector_count;
                u32 startB = inodes[j].start_lba;
                u32 endB = startB + inodes[j].sector_count;
                if (startA < endB && startB < endA) {
                    return false; /* Overlapping extents */
                }
            }

            /* Verify all sectors in extent are marked used in allocation bitmap */
            for (u32 s = 0; s < inodes[i].sector_count; s++) {
                if (!bitmap_get(bitmap, inodes[i].start_lba + s)) {
                    return false; /* Bitmap mismatch */
                }
            }
            calculated_used_sectors += inodes[i].sector_count;
        }
    }

    if (active_files != sb->file_count) return false;
    if (calculated_used_sectors != sb->used_sectors) return false;

    /* Verify allocation bitmap has no orphan or extraneous set bits in data region */
    u32 scan_sectors = (sb->total_sectors > ASTRAFS_MAX_SECTORS) ? ASTRAFS_MAX_SECTORS : sb->total_sectors;
    u32 bitmap_used_count = 0;
    for (u32 s = ASTRAFS_DATA_START_LBA; s < scan_sectors; s++) {
        if (bitmap_get(bitmap, s)) {
            bitmap_used_count++;
        }
    }
    if (bitmap_used_count != calculated_used_sectors) return false;

    return true;
}

int astrafs_mount(block_device_t *dev) {
    if (!dev || !dev->ops || !dev->ops->read_sectors) {
        return VFS_ERR_INVAL;
    }
    if (dev->sector_size != ASTRAFS_SECTOR_SIZE) {
        return VFS_ERR_UNSUPPORTED;
    }

    astrafs_superblock_t sbA, sbB;
    kmemset(&sbA, 0, sizeof(sbA));
    kmemset(&sbB, 0, sizeof(sbB));

    int errA = block_read(dev, ASTRAFS_SUPERBLOCK_A_LBA, 1, &sbA);
    int errB = block_read(dev, ASTRAFS_SUPERBLOCK_B_LBA, 1, &sbB);

    static astrafs_inode_t temp_inodes[ASTRAFS_MAX_FILES];
    static u8 temp_bitmap[ASTRAFS_BITMAP_SECTORS * ASTRAFS_SECTOR_SIZE];

    bool validA = false;
    if (errA == BLOCK_OK &&
        sbA.magic == ASTRAFS_MAGIC &&
        sbA.version == ASTRAFS_VERSION &&
        sbA.checksum == astrafs_checksum(&sbA, 508)) {
        if (block_read(dev, sbA.inode_table_lba, ASTRAFS_INODE_SECTORS, temp_inodes) == BLOCK_OK &&
            block_read(dev, sbA.bitmap_lba, ASTRAFS_BITMAP_SECTORS, temp_bitmap) == BLOCK_OK) {
            validA = validate_metadata_integrity(dev, &sbA, temp_inodes, temp_bitmap);
        }
    }

    bool validB = false;
    if (errB == BLOCK_OK &&
        sbB.magic == ASTRAFS_MAGIC &&
        sbB.version == ASTRAFS_VERSION &&
        sbB.checksum == astrafs_checksum(&sbB, 508)) {
        if (block_read(dev, sbB.inode_table_lba, ASTRAFS_INODE_SECTORS, temp_inodes) == BLOCK_OK &&
            block_read(dev, sbB.bitmap_lba, ASTRAFS_BITMAP_SECTORS, temp_bitmap) == BLOCK_OK) {
            validB = validate_metadata_integrity(dev, &sbB, temp_inodes, temp_bitmap);
        }
    }

    if (!validA && !validB) {
        kprintf("[ASTRAFS] Mount failed: Neither Superblock A nor B contains valid metadata.\n");
        return VFS_ERR_CORRUPT;
    }

    u8 chosen_set = 0;
    if (validA && validB) {
        chosen_set = ((s64)(sbB.generation - sbA.generation) > 0) ? 1 : 0;
    } else if (validA) {
        kprintf("[ASTRAFS] Notice: Superblock B is corrupted or invalid; falling back to valid Superblock A.\n");
        chosen_set = 0;
    } else {
        kprintf("[ASTRAFS] Notice: Superblock A is corrupted or invalid; falling back to valid Superblock B.\n");
        chosen_set = 1;
    }

    astrafs_superblock_t *chosen_sb = (chosen_set == 0) ? &sbA : &sbB;
    if (chosen_sb->total_sectors > ASTRAFS_MAX_SECTORS) {
        chosen_sb->total_sectors = ASTRAFS_MAX_SECTORS;
    }

    kmemset(&g_astrafs, 0, sizeof(g_astrafs));
    g_astrafs.dev = dev;
    g_astrafs.active_set = chosen_set;
    g_astrafs.sb = *chosen_sb;

    /* Read active inode table */
    int r = block_read(dev, g_astrafs.sb.inode_table_lba, ASTRAFS_INODE_SECTORS, g_astrafs.inodes);
    if (r != BLOCK_OK) return VFS_ERR_BUSY;

    /* Read active allocation bitmap */
    r = block_read(dev, g_astrafs.sb.bitmap_lba, ASTRAFS_BITMAP_SECTORS, g_astrafs.bitmap);
    if (r != BLOCK_OK) return VFS_ERR_BUSY;

    /* Setup root node for /data */
    kstrncpy(g_astrafs.root_node.name, "data", sizeof(g_astrafs.root_node.name));
    g_astrafs.root_node.type = VFS_NODE_DIR;
    g_astrafs.root_node.size = 0;
    g_astrafs.root_node.ops = &g_astrafs_ops;
    g_astrafs.root_node.fs_data = (void *)(uintptr_t)-1;
    g_astrafs.root_node.parent = vfs_get_root();
    g_astrafs.root_node.mode = 0777;
    g_astrafs.root_node.uid = 1000;
    g_astrafs.root_node.gid = 1000;

    /* Setup VFS nodes for active files */
    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (g_astrafs.inodes[i].in_use) {
            vfs_node_t *node = &g_astrafs.vfs_nodes[i];
            kstrncpy(node->name, g_astrafs.inodes[i].name, sizeof(node->name));
            node->type = (g_astrafs.inodes[i].flags & ASTRAFS_FLAG_DIR) ? VFS_NODE_DIR : VFS_NODE_FILE;
            node->size = g_astrafs.inodes[i].file_size;
            node->mode = (g_astrafs.inodes[i].flags & ASTRAFS_FLAG_DIR) ? 0777 : 0666;
            node->uid = 1000;
            node->gid = 1000;
            node->ops = &g_astrafs_ops;
            node->fs_data = (void *)(uintptr_t)i;
            node->parent = &g_astrafs.root_node;
        }
    }

    g_astrafs.mounted = true;

    kprintf("[ASTRAFS] Mounted filesystem from device '%s' (generation %llu, set %c, %u files, %u sectors used)\n",
            dev->name, (unsigned long long)g_astrafs.sb.generation,
            (chosen_set == 0) ? 'A' : 'B',
            g_astrafs.sb.file_count, g_astrafs.sb.used_sectors);

    return VFS_OK;
}

int astrafs_unmount(void) {
    astrafs_lock();
    if (!g_astrafs.mounted) {
        astrafs_unlock();
        return VFS_OK;
    }
    if (g_astrafs.dev) {
        block_flush(g_astrafs.dev);
    }
    g_astrafs.mounted = false;
    g_astrafs.dev = NULL;
    astrafs_unlock();
    return VFS_OK;
}

int astrafs_lookup(const char *name, vfs_node_t **out_node) {
    if (!out_node) return VFS_ERR_INVAL;
    *out_node = NULL;
    astrafs_lock();
    if (!g_astrafs.mounted) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }

    const char *stripped = strip_data_prefix(name);
    if (!stripped || stripped[0] == '\0') {
        *out_node = &g_astrafs.root_node;
        astrafs_unlock();
        return VFS_OK;
    }

    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (g_astrafs.inodes[i].in_use && kstrcmp(g_astrafs.inodes[i].name, stripped) == 0) {
            *out_node = &g_astrafs.vfs_nodes[i];
            astrafs_unlock();
            return VFS_OK;
        }
    }

    astrafs_unlock();
    return VFS_ERR_NOT_FOUND;
}

int astrafs_create(const char *name, u32 flags, vfs_node_t **out_node) {
    if (!out_node) return VFS_ERR_INVAL;
    *out_node = NULL;
    astrafs_lock();
    if (!g_astrafs.mounted || !g_astrafs.dev) {
        astrafs_unlock();
        return VFS_ERR_UNSUPPORTED;
    }
    if (g_astrafs.dev->read_only) {
        astrafs_unlock();
        return VFS_ERR_READ_ONLY;
    }

    const char *stripped = strip_data_prefix(name);
    if (!stripped || stripped[0] == '\0') {
        astrafs_unlock();
        return VFS_ERR_INVALID_PATH;
    }

    size_t nlen = kstrlen(stripped);
    if (nlen == 0 || nlen >= ASTRAFS_FILENAME_MAX) {
        astrafs_unlock();
        return VFS_ERR_INVALID_PATH;
    }

    /* Validate relative path syntax (reject leading/trailing slashes or empty components) */
    for (size_t c = 0; c < nlen; c++) {
        if (stripped[c] == '/' && (c == 0 || c == nlen - 1 || stripped[c + 1] == '/')) {
            astrafs_unlock();
            return VFS_ERR_INVALID_PATH;
        }
    }

    /* Check if already exists */
    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (g_astrafs.inodes[i].in_use && kstrcmp(g_astrafs.inodes[i].name, stripped) == 0) {
            *out_node = &g_astrafs.vfs_nodes[i];
            astrafs_unlock();
            return VFS_OK;
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (!g_astrafs.inodes[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        astrafs_unlock();
        return VFS_ERR_NOSPC;
    }

    astrafs_inode_t *ino = &g_astrafs.inodes[slot];
    kmemset(ino, 0, sizeof(*ino));
    kstrncpy(ino->name, stripped, sizeof(ino->name));
    ino->in_use = 1;
    ino->file_size = 0;
    ino->start_lba = 0;
    ino->sector_count = 0;
    ino->flags = flags;
    ino->data_checksum = 0;
    ino->created_time = g_astrafs.sb.generation + 1;
    ino->modified_time = g_astrafs.sb.generation + 1;

    vfs_node_t *node = &g_astrafs.vfs_nodes[slot];
    kstrncpy(node->name, stripped, sizeof(node->name));
    node->type = (flags & ASTRAFS_FLAG_DIR) ? VFS_NODE_DIR : VFS_NODE_FILE;
    node->size = 0;
    node->mode = (flags & ASTRAFS_FLAG_DIR) ? 0777 : 0666;
    node->uid = 1000;
    node->gid = 1000;
    node->ops = &g_astrafs_ops;
    node->fs_data = (void *)(uintptr_t)slot;
    node->parent = &g_astrafs.root_node;

    int ret = astrafs_commit_metadata();
    if (ret != VFS_OK) {
        kmemset(ino, 0, sizeof(*ino));
        kmemset(node, 0, sizeof(*node));
        astrafs_unlock();
        return ret;
    }

    *out_node = node;
    astrafs_unlock();
    return VFS_OK;
}

int astrafs_mkdir(const char *name) {
    astrafs_lock();
    if (!g_astrafs.mounted || !g_astrafs.dev) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }
    if (g_astrafs.dev->read_only) {
        astrafs_unlock();
        return VFS_ERR_READ_ONLY;
    }

    const char *stripped = strip_data_prefix(name);
    if (!stripped || stripped[0] == '\0') {
        astrafs_unlock();
        return VFS_ERR_INVALID_PATH;
    }

    size_t nlen = kstrlen(stripped);
    if (nlen == 0 || nlen >= ASTRAFS_FILENAME_MAX) {
        astrafs_unlock();
        return VFS_ERR_INVALID_PATH;
    }

    for (size_t c = 0; c < nlen; c++) {
        if (stripped[c] == '/' && (c == 0 || c == nlen - 1 || stripped[c + 1] == '/')) {
            astrafs_unlock();
            return VFS_ERR_INVALID_PATH;
        }
    }

    /* Check if already exists */
    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (g_astrafs.inodes[i].in_use && kstrcmp(g_astrafs.inodes[i].name, stripped) == 0) {
            astrafs_unlock();
            return VFS_ERR_EXISTS;
        }
    }

    int slot = -1;
    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (!g_astrafs.inodes[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        astrafs_unlock();
        return VFS_ERR_NOSPC;
    }

    astrafs_inode_t *ino = &g_astrafs.inodes[slot];
    kmemset(ino, 0, sizeof(*ino));
    kstrncpy(ino->name, stripped, sizeof(ino->name));
    ino->in_use = 1;
    ino->file_size = 0;
    ino->start_lba = 0;
    ino->sector_count = 0;
    ino->flags = ASTRAFS_FLAG_DIR;
    ino->data_checksum = 0;
    ino->created_time = g_astrafs.sb.generation + 1;
    ino->modified_time = g_astrafs.sb.generation + 1;

    vfs_node_t *node = &g_astrafs.vfs_nodes[slot];
    kstrncpy(node->name, stripped, sizeof(node->name));
    node->type = VFS_NODE_DIR;
    node->size = 0;
    node->mode = 0777;
    node->uid = 1000;
    node->gid = 1000;
    node->ops = &g_astrafs_ops;
    node->fs_data = (void *)(uintptr_t)slot;
    node->parent = &g_astrafs.root_node;

    int ret = astrafs_commit_metadata();
    if (ret != VFS_OK) {
        kmemset(ino, 0, sizeof(*ino));
        kmemset(node, 0, sizeof(*node));
        astrafs_unlock();
        return ret;
    }

    astrafs_unlock();
    return VFS_OK;
}

int astrafs_unlink(const char *name) {
    astrafs_lock();
    if (!g_astrafs.mounted || !g_astrafs.dev) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }
    if (g_astrafs.dev->read_only) {
        astrafs_unlock();
        return VFS_ERR_READ_ONLY;
    }

    const char *stripped = strip_data_prefix(name);
    if (!stripped || stripped[0] == '\0') {
        astrafs_unlock();
        return VFS_ERR_INVALID_PATH;
    }

    int slot = -1;
    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (g_astrafs.inodes[i].in_use && kstrcmp(g_astrafs.inodes[i].name, stripped) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }

    astrafs_inode_t *ino = &g_astrafs.inodes[slot];
    if (ino->flags & ASTRAFS_FLAG_DIR) {
        astrafs_unlock();
        return VFS_ERR_IS_DIR;
    }

    if (ino->sector_count > 0 && ino->start_lba != 0) {
        bitmap_free_extent(g_astrafs.bitmap, ino->start_lba, ino->sector_count);
    }

    astrafs_inode_t saved_ino = *ino;
    vfs_node_t saved_node = g_astrafs.vfs_nodes[slot];

    kmemset(ino, 0, sizeof(*ino));
    kmemset(&g_astrafs.vfs_nodes[slot], 0, sizeof(vfs_node_t));

    int ret = astrafs_commit_metadata();
    if (ret != VFS_OK) {
        *ino = saved_ino;
        g_astrafs.vfs_nodes[slot] = saved_node;
        if (saved_ino.sector_count > 0 && saved_ino.start_lba != 0) {
            for (u32 s = 0; s < saved_ino.sector_count; s++) {
                bitmap_set(g_astrafs.bitmap, saved_ino.start_lba + s);
            }
        }
    }

    astrafs_unlock();
    return ret;
}

int astrafs_rmdir(const char *name) {
    astrafs_lock();
    if (!g_astrafs.mounted || !g_astrafs.dev) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }
    if (g_astrafs.dev->read_only) {
        astrafs_unlock();
        return VFS_ERR_READ_ONLY;
    }

    const char *stripped = strip_data_prefix(name);
    if (!stripped || stripped[0] == '\0') {
        astrafs_unlock();
        return VFS_ERR_INVALID_PATH;
    }

    int slot = -1;
    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (g_astrafs.inodes[i].in_use && kstrcmp(g_astrafs.inodes[i].name, stripped) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }

    astrafs_inode_t *ino = &g_astrafs.inodes[slot];
    if (!(ino->flags & ASTRAFS_FLAG_DIR)) {
        astrafs_unlock();
        return VFS_ERR_NOT_DIR;
    }

    /* Verify directory is empty */
    size_t slen = kstrlen(stripped);
    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (i == slot || !g_astrafs.inodes[i].in_use) continue;
        if (kstrncmp(g_astrafs.inodes[i].name, stripped, slen) == 0 &&
            g_astrafs.inodes[i].name[slen] == '/') {
            astrafs_unlock();
            return VFS_ERR_BUSY;
        }
    }

    astrafs_inode_t saved_ino = *ino;
    vfs_node_t saved_node = g_astrafs.vfs_nodes[slot];

    kmemset(ino, 0, sizeof(*ino));
    kmemset(&g_astrafs.vfs_nodes[slot], 0, sizeof(vfs_node_t));

    int ret = astrafs_commit_metadata();
    if (ret != VFS_OK) {
        *ino = saved_ino;
        g_astrafs.vfs_nodes[slot] = saved_node;
    }

    astrafs_unlock();
    return ret;
}

ssize_t astrafs_read(vfs_node_t *node, void *buf, size_t count, size_t offset) {
    if (!node || !buf) return VFS_ERR_INVAL;
    if (count == 0) return 0;
    if (offset + count < offset) return VFS_ERR_INVAL;

    astrafs_lock();
    if (!g_astrafs.mounted || !g_astrafs.dev) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }

    int slot = (int)(uintptr_t)node->fs_data;
    if (slot < 0 || slot >= ASTRAFS_MAX_FILES || !g_astrafs.inodes[slot].in_use) {
        astrafs_unlock();
        return VFS_ERR_INVALID_HANDLE;
    }

    astrafs_inode_t *ino = &g_astrafs.inodes[slot];
    if (offset >= ino->file_size) {
        astrafs_unlock();
        return 0; /* EOF */
    }

    size_t available = ino->file_size - offset;
    size_t to_read = (count < available) ? count : available;

    static u8 s_sector_buf[ASTRAFS_SECTOR_SIZE];
    size_t bytes_read = 0;

    while (bytes_read < to_read) {
        size_t cur_offset = offset + bytes_read;
        u32 sec_index = (u32)(cur_offset / ASTRAFS_SECTOR_SIZE);
        u32 sec_offset = (u32)(cur_offset % ASTRAFS_SECTOR_SIZE);
        u32 target_lba = ino->start_lba + sec_index;

        if (sec_index >= ino->sector_count) break;

        int r = block_read(g_astrafs.dev, target_lba, 1, s_sector_buf);
        if (r != BLOCK_OK) {
            astrafs_unlock();
            return (bytes_read > 0) ? (ssize_t)bytes_read : (ssize_t)VFS_ERR_BUSY;
        }

        size_t chunk = ASTRAFS_SECTOR_SIZE - sec_offset;
        if (chunk > (to_read - bytes_read)) {
            chunk = to_read - bytes_read;
        }

        kmemcpy((u8 *)buf + bytes_read, s_sector_buf + sec_offset, chunk);
        bytes_read += chunk;
    }

    astrafs_unlock();
    return (ssize_t)bytes_read;
}

ssize_t astrafs_write(vfs_node_t *node, const void *buf, size_t count, size_t offset) {
    if (!node || !buf) return VFS_ERR_INVAL;
    if (count == 0) return 0;

    /* Reject 64-bit integer overflow */
    if (offset + count < offset) return VFS_ERR_INVAL;

    astrafs_lock();
    if (!g_astrafs.mounted || !g_astrafs.dev) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }
    if (g_astrafs.dev->read_only) {
        astrafs_unlock();
        return VFS_ERR_READ_ONLY;
    }

    int slot = (int)(uintptr_t)node->fs_data;
    if (slot < 0 || slot >= ASTRAFS_MAX_FILES || !g_astrafs.inodes[slot].in_use) {
        astrafs_unlock();
        return VFS_ERR_INVALID_HANDLE;
    }

    astrafs_inode_t *ino = &g_astrafs.inodes[slot];
    size_t new_size = (offset + count > ino->file_size) ? (offset + count) : ino->file_size;

    /* Max file size check (4 MiB limit) */
    if (new_size > (4 * 1024 * 1024)) {
        astrafs_unlock();
        return VFS_ERR_NOSPC;
    }

    u32 req_sectors = (new_size == 0) ? 0 : (u32)((new_size + ASTRAFS_SECTOR_SIZE - 1) / ASTRAFS_SECTOR_SIZE);

    /* Allocate fresh contiguous extent for Copy-On-Write data safety */
    u32 new_lba = bitmap_alloc_extent(g_astrafs.bitmap, g_astrafs.sb.total_sectors, req_sectors);
    if (new_lba == 0) {
        astrafs_unlock();
        return VFS_ERR_NOSPC;
    }

    static u8 s_sector_tmp[ASTRAFS_SECTOR_SIZE];

    /* 1. Copy unmodified sectors from old extent to new extent if old extent exists */
    if (ino->sector_count > 0 && ino->start_lba != 0) {
        for (u32 s = 0; s < ino->sector_count; s++) {
            int r = block_read(g_astrafs.dev, ino->start_lba + s, 1, s_sector_tmp);
            if (r != BLOCK_OK) {
                bitmap_free_extent(g_astrafs.bitmap, new_lba, req_sectors);
                astrafs_unlock();
                return VFS_ERR_BUSY;
            }
            /* If writing past EOF and this is the last sector of old extent, zero out old slack bytes */
            if (offset > ino->file_size && s == ino->sector_count - 1) {
                u32 old_slack = ino->file_size % ASTRAFS_SECTOR_SIZE;
                if (old_slack > 0) {
                    kmemset(s_sector_tmp + old_slack, 0, ASTRAFS_SECTOR_SIZE - old_slack);
                }
            }
            r = block_write(g_astrafs.dev, new_lba + s, 1, s_sector_tmp);
            if (r != BLOCK_OK) {
                bitmap_free_extent(g_astrafs.bitmap, new_lba, req_sectors);
                astrafs_unlock();
                return VFS_ERR_BUSY;
            }
        }
    }

    /* 2. Zero-fill any newly extended sectors beyond old capacity */
    kmemset(s_sector_tmp, 0, sizeof(s_sector_tmp));
    for (u32 s = ino->sector_count; s < req_sectors; s++) {
        int r = block_write(g_astrafs.dev, new_lba + s, 1, s_sector_tmp);
        if (r != BLOCK_OK) {
            bitmap_free_extent(g_astrafs.bitmap, new_lba, req_sectors);
            astrafs_unlock();
            return VFS_ERR_BUSY;
        }
    }

    /* 3. Write new data into the newly allocated extent */
    size_t bytes_written = 0;
    while (bytes_written < count) {
        size_t cur_offset = offset + bytes_written;
        u32 sec_index = (u32)(cur_offset / ASTRAFS_SECTOR_SIZE);
        u32 sec_offset = (u32)(cur_offset % ASTRAFS_SECTOR_SIZE);
        u32 target_lba = new_lba + sec_index;

        size_t chunk = ASTRAFS_SECTOR_SIZE - sec_offset;
        if (chunk > (count - bytes_written)) {
            chunk = count - bytes_written;
        }

        if (chunk < ASTRAFS_SECTOR_SIZE) {
            /* Read-modify-write partial sector in new extent */
            int r = block_read(g_astrafs.dev, target_lba, 1, s_sector_tmp);
            if (r != BLOCK_OK) kmemset(s_sector_tmp, 0, sizeof(s_sector_tmp));
            kmemcpy(s_sector_tmp + sec_offset, (const u8 *)buf + bytes_written, chunk);
            r = block_write(g_astrafs.dev, target_lba, 1, s_sector_tmp);
            if (r != BLOCK_OK) {
                bitmap_free_extent(g_astrafs.bitmap, new_lba, req_sectors);
                astrafs_unlock();
                return (bytes_written > 0) ? (ssize_t)bytes_written : (ssize_t)VFS_ERR_BUSY;
            }
        } else {
            /* Full sector direct write */
            int r = block_write(g_astrafs.dev, target_lba, 1, (const u8 *)buf + bytes_written);
            if (r != BLOCK_OK) {
                bitmap_free_extent(g_astrafs.bitmap, new_lba, req_sectors);
                astrafs_unlock();
                return (bytes_written > 0) ? (ssize_t)bytes_written : (ssize_t)VFS_ERR_BUSY;
            }
        }
        bytes_written += chunk;
    }

    /* 4. Issue PRE-COMMIT FLUSH to ensure payload sectors are persisted on disk */
    int flush_res = block_flush(g_astrafs.dev);
    if (flush_res != BLOCK_OK) {
        bitmap_free_extent(g_astrafs.bitmap, new_lba, req_sectors);
        astrafs_unlock();
        return VFS_ERR_BUSY;
    }

    /* 5. Snapshot old metadata for rollback before updating inode and bitmap */
    u32 old_lba = ino->start_lba;
    u32 old_count = ino->sector_count;
    u32 old_file_size = ino->file_size;
    u32 old_mtime = ino->modified_time;
    u32 old_checksum = ino->data_checksum;
    size_t old_node_size = node->size;

    if (old_count > 0 && old_lba != 0) {
        bitmap_free_extent(g_astrafs.bitmap, old_lba, old_count);
    }

    ino->start_lba = new_lba;
    ino->sector_count = req_sectors;
    ino->file_size = (u32)new_size;
    ino->modified_time = g_astrafs.sb.generation + 1;
    node->size = new_size;

    /* 6. Commit metadata atomically */
    int ret = astrafs_commit_metadata();
    if (ret != VFS_OK) {
        /* Roll back in-memory metadata: revert inode and allocation bitmap */
        bitmap_free_extent(g_astrafs.bitmap, new_lba, req_sectors);
        if (old_count > 0 && old_lba != 0) {
            for (u32 s = 0; s < old_count; s++) {
                bitmap_set(g_astrafs.bitmap, old_lba + s);
            }
        }
        ino->start_lba = old_lba;
        ino->sector_count = old_count;
        ino->file_size = old_file_size;
        ino->modified_time = old_mtime;
        ino->data_checksum = old_checksum;
        node->size = old_node_size;
        astrafs_unlock();
        return (ssize_t)ret;
    }
    astrafs_unlock();

    return (ssize_t)bytes_written;
}

int astrafs_truncate(vfs_node_t *node, size_t new_size) {
    if (!node) return VFS_ERR_INVAL;
    astrafs_lock();
    if (!g_astrafs.mounted || !g_astrafs.dev) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }
    if (g_astrafs.dev->read_only) {
        astrafs_unlock();
        return VFS_ERR_READ_ONLY;
    }

    int slot = (int)(uintptr_t)node->fs_data;
    if (slot < 0 || slot >= ASTRAFS_MAX_FILES || !g_astrafs.inodes[slot].in_use) {
        astrafs_unlock();
        return VFS_ERR_INVALID_HANDLE;
    }

    astrafs_inode_t *ino = &g_astrafs.inodes[slot];
    u32 old_lba = ino->start_lba;
    u32 old_count = ino->sector_count;
    u32 old_file_size = ino->file_size;
    u32 old_mtime = ino->modified_time;
    u32 old_checksum = ino->data_checksum;
    size_t old_node_size = node->size;

    if (new_size == 0) {
        if (ino->sector_count > 0 && ino->start_lba != 0) {
            bitmap_free_extent(g_astrafs.bitmap, ino->start_lba, ino->sector_count);
            ino->start_lba = 0;
            ino->sector_count = 0;
        }
        ino->file_size = 0;
        ino->data_checksum = 0;
        ino->modified_time = g_astrafs.sb.generation + 1;
        node->size = 0;
        int r = astrafs_commit_metadata();
        if (r != VFS_OK) {
            /* Roll back */
            if (old_count > 0 && old_lba != 0) {
                for (u32 s = 0; s < old_count; s++) {
                    bitmap_set(g_astrafs.bitmap, old_lba + s);
                }
            }
            ino->start_lba = old_lba;
            ino->sector_count = old_count;
            ino->file_size = old_file_size;
            ino->modified_time = old_mtime;
            ino->data_checksum = old_checksum;
            node->size = old_node_size;
        }
        astrafs_unlock();
        return r;
    }

    ino->file_size = (u32)new_size;
    ino->modified_time = g_astrafs.sb.generation + 1;
    node->size = new_size;
    int r = astrafs_commit_metadata();
    if (r != VFS_OK) {
        ino->file_size = old_file_size;
        ino->modified_time = old_mtime;
        node->size = old_node_size;
    }
    astrafs_unlock();
    return r;
}

int astrafs_stat(vfs_node_t *node, vfs_stat_t *st) {
    if (!node || !st) return VFS_ERR_INVAL;
    astrafs_lock();
    if (node == &g_astrafs.root_node) {
        kstrncpy(st->name, "data", sizeof(st->name));
        st->type = VFS_NODE_DIR;
        st->size = 0;
        astrafs_unlock();
        return VFS_OK;
    }

    int slot = (int)(uintptr_t)node->fs_data;
    if (slot < 0 || slot >= ASTRAFS_MAX_FILES || !g_astrafs.inodes[slot].in_use) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }

    kstrncpy(st->name, g_astrafs.inodes[slot].name, sizeof(st->name));
    st->type = (g_astrafs.inodes[slot].flags & ASTRAFS_FLAG_DIR) ? VFS_NODE_DIR : VFS_NODE_FILE;
    st->size = g_astrafs.inodes[slot].file_size;
    astrafs_unlock();
    return VFS_OK;
}

static int astrafs_node_readdir_locked(vfs_node_t *node, size_t index, vfs_dirent_t *dirent) {
    if (!dirent) return VFS_ERR_INVAL;
    if (!g_astrafs.mounted) return VFS_ERR_NOT_FOUND;

    const char *prefix = "";
    size_t plen = 0;
    if (node && node != &g_astrafs.root_node) {
        int slot = (int)(uintptr_t)node->fs_data;
        if (slot >= 0 && slot < ASTRAFS_MAX_FILES && g_astrafs.inodes[slot].in_use) {
            prefix = g_astrafs.inodes[slot].name;
            plen = kstrlen(prefix);
        }
    }

    char seen[ASTRAFS_MAX_FILES][ASTRAFS_FILENAME_MAX];
    int seen_count = 0;
    size_t match_count = 0;

    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (!g_astrafs.inodes[i].in_use) continue;
        const char *iname = g_astrafs.inodes[i].name;

        char child_name[ASTRAFS_FILENAME_MAX];
        vfs_node_type_t child_type = VFS_NODE_FILE;
        size_t child_size = g_astrafs.inodes[i].file_size;

        if (plen == 0) {
            /* Reading root (/data) */
            const char *slash = NULL;
            for (const char *s = iname; *s; s++) {
                if (*s == '/') { slash = s; break; }
            }
            if (!slash) {
                kstrncpy(child_name, iname, sizeof(child_name));
                child_type = (g_astrafs.inodes[i].flags & ASTRAFS_FLAG_DIR) ? VFS_NODE_DIR : VFS_NODE_FILE;
            } else {
                size_t comp_len = (size_t)(slash - iname);
                if (comp_len >= sizeof(child_name)) comp_len = sizeof(child_name) - 1;
                kstrncpy(child_name, iname, comp_len + 1);
                child_name[comp_len] = '\0';
                child_type = VFS_NODE_DIR;
                child_size = 0;
            }
        } else {
            /* Reading subdirectory */
            if (kstrncmp(iname, prefix, plen) != 0 || iname[plen] != '/') {
                continue;
            }
            const char *rel = iname + plen + 1;
            const char *slash = NULL;
            for (const char *s = rel; *s; s++) {
                if (*s == '/') { slash = s; break; }
            }
            if (!slash) {
                kstrncpy(child_name, rel, sizeof(child_name));
                child_type = (g_astrafs.inodes[i].flags & ASTRAFS_FLAG_DIR) ? VFS_NODE_DIR : VFS_NODE_FILE;
            } else {
                size_t comp_len = (size_t)(slash - rel);
                if (comp_len >= sizeof(child_name)) comp_len = sizeof(child_name) - 1;
                kstrncpy(child_name, rel, comp_len + 1);
                child_name[comp_len] = '\0';
                child_type = VFS_NODE_DIR;
                child_size = 0;
            }
        }

        bool already_seen = false;
        for (int s = 0; s < seen_count; s++) {
            if (kstrcmp(seen[s], child_name) == 0) {
                already_seen = true;
                break;
            }
        }
        if (already_seen) continue;

        if (seen_count < ASTRAFS_MAX_FILES) {
            kstrncpy(seen[seen_count++], child_name, sizeof(seen[0]));
        }

        if (match_count == index) {
            kstrncpy(dirent->name, child_name, sizeof(dirent->name));
            dirent->type = child_type;
            dirent->size = child_size;
            return VFS_OK;
        }
        match_count++;
    }

    return VFS_ERR_EOF;
}

int astrafs_readdir(size_t index, vfs_dirent_t *dirent) {
    if (!dirent) return VFS_ERR_INVAL;
    astrafs_lock();
    int ret = astrafs_node_readdir_locked(&g_astrafs.root_node, index, dirent);
    astrafs_unlock();
    return ret;
}

int astrafs_statfs(vfs_statfs_t *st) {
    if (!st) return VFS_ERR_INVAL;
    astrafs_lock();
    if (!g_astrafs.mounted || !g_astrafs.dev) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }

    u32 total_sec = g_astrafs.sb.total_sectors;
    u32 used_sec = g_astrafs.sb.used_sectors;
    u32 reserved_sec = ASTRAFS_DATA_START_LBA;
    u32 free_sec = 0;
    if (total_sec > used_sec + reserved_sec) {
        free_sec = total_sec - used_sec - reserved_sec;
    }

    st->block_size = ASTRAFS_SECTOR_SIZE;
    st->total_bytes = (size_t)total_sec * ASTRAFS_SECTOR_SIZE;
    st->free_bytes = (size_t)free_sec * ASTRAFS_SECTOR_SIZE;
    st->total_inodes = ASTRAFS_MAX_FILES;
    st->free_inodes = (ASTRAFS_MAX_FILES >= g_astrafs.sb.file_count) ?
                      (ASTRAFS_MAX_FILES - g_astrafs.sb.file_count) : 0;

    astrafs_unlock();
    return VFS_OK;
}

int astrafs_fsync(vfs_node_t *node) {
    if (!node) return VFS_ERR_INVAL;
    astrafs_lock();
    if (!g_astrafs.mounted || !g_astrafs.dev) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }
    int ret = block_flush(g_astrafs.dev);
    astrafs_unlock();
    return (ret == BLOCK_OK) ? VFS_OK : VFS_ERR_BUSY;
}

int astrafs_sync(void) {
    astrafs_lock();
    if (!g_astrafs.mounted || !g_astrafs.dev) {
        astrafs_unlock();
        return VFS_OK;
    }
    int ret = block_flush(g_astrafs.dev);
    astrafs_unlock();
    return (ret == BLOCK_OK) ? VFS_OK : VFS_ERR_BUSY;
}

int astrafs_rename(const char *old_name, const char *new_name) {
    astrafs_lock();
    if (!g_astrafs.mounted || !g_astrafs.dev) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }
    if (g_astrafs.dev->read_only) {
        astrafs_unlock();
        return VFS_ERR_READ_ONLY;
    }

    const char *old_s = strip_data_prefix(old_name);
    const char *new_s = strip_data_prefix(new_name);

    if (!old_s || old_s[0] == '\0' || !new_s || new_s[0] == '\0') {
        astrafs_unlock();
        return VFS_ERR_INVALID_PATH;
    }

    size_t nlen = kstrlen(new_s);
    if (nlen == 0 || nlen >= ASTRAFS_FILENAME_MAX) {
        astrafs_unlock();
        return VFS_ERR_INVALID_PATH;
    }
    for (size_t c = 0; c < nlen; c++) {
        if (new_s[c] == '/' && (c == 0 || c == nlen - 1 || new_s[c + 1] == '/')) {
            astrafs_unlock();
            return VFS_ERR_INVALID_PATH;
        }
    }

    int src_slot = -1;
    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (g_astrafs.inodes[i].in_use && kstrcmp(g_astrafs.inodes[i].name, old_s) == 0) {
            src_slot = i;
            break;
        }
    }
    if (src_slot < 0) {
        astrafs_unlock();
        return VFS_ERR_NOT_FOUND;
    }

    if (kstrcmp(old_s, new_s) == 0) {
        astrafs_unlock();
        return VFS_OK;
    }

    int dst_slot = -1;
    for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
        if (g_astrafs.inodes[i].in_use && kstrcmp(g_astrafs.inodes[i].name, new_s) == 0) {
            dst_slot = i;
            break;
        }
    }

    astrafs_inode_t saved_dst_ino;
    astrafs_inode_t saved_src_ino = g_astrafs.inodes[src_slot];
    vfs_node_t saved_vfs_dst;
    vfs_node_t saved_vfs_src = g_astrafs.vfs_nodes[src_slot];
    bool dst_existed = (dst_slot >= 0);

    if (dst_existed) {
        saved_dst_ino = g_astrafs.inodes[dst_slot];
        saved_vfs_dst = g_astrafs.vfs_nodes[dst_slot];

        astrafs_inode_t *dst_ino = &g_astrafs.inodes[dst_slot];
        if (dst_ino->sector_count > 0 && dst_ino->start_lba != 0) {
            bitmap_free_extent(g_astrafs.bitmap, dst_ino->start_lba, dst_ino->sector_count);
        }
        astrafs_inode_t *src_ino = &g_astrafs.inodes[src_slot];
        dst_ino->file_size = src_ino->file_size;
        dst_ino->start_lba = src_ino->start_lba;
        dst_ino->sector_count = src_ino->sector_count;
        dst_ino->flags = src_ino->flags;
        dst_ino->data_checksum = src_ino->data_checksum;
        dst_ino->modified_time = g_astrafs.sb.generation + 1;

        kmemset(src_ino, 0, sizeof(*src_ino));
        src_ino->in_use = 0;

        g_astrafs.vfs_nodes[dst_slot].size = dst_ino->file_size;
        kstrncpy(g_astrafs.vfs_nodes[dst_slot].name, new_s, sizeof(g_astrafs.vfs_nodes[dst_slot].name));
        kmemset(&g_astrafs.vfs_nodes[src_slot], 0, sizeof(vfs_node_t));
    } else {
        astrafs_inode_t *src_ino = &g_astrafs.inodes[src_slot];
        kstrncpy(src_ino->name, new_s, sizeof(src_ino->name));
        src_ino->modified_time = g_astrafs.sb.generation + 1;
        kstrncpy(g_astrafs.vfs_nodes[src_slot].name, new_s, sizeof(g_astrafs.vfs_nodes[src_slot].name));

        /* If renaming directory, update children */
        if (saved_src_ino.flags & ASTRAFS_FLAG_DIR) {
            size_t old_plen = kstrlen(old_s);
            size_t new_plen = kstrlen(new_s);
            for (int i = 0; i < ASTRAFS_MAX_FILES; i++) {
                if (i == src_slot || !g_astrafs.inodes[i].in_use) continue;
                if (kstrncmp(g_astrafs.inodes[i].name, old_s, old_plen) == 0 &&
                    g_astrafs.inodes[i].name[old_plen] == '/') {
                    char child_new[ASTRAFS_FILENAME_MAX];
                    kstrncpy(child_new, new_s, sizeof(child_new));
                    size_t rem = sizeof(child_new) - new_plen - 1;
                    kstrncpy(child_new + new_plen, g_astrafs.inodes[i].name + old_plen, rem);
                    kstrncpy(g_astrafs.inodes[i].name, child_new, sizeof(g_astrafs.inodes[i].name));
                    kstrncpy(g_astrafs.vfs_nodes[i].name, child_new, sizeof(g_astrafs.vfs_nodes[i].name));
                }
            }
        }
    }

    int ret = astrafs_commit_metadata();
    if (ret != VFS_OK) {
        /* Roll back rename in memory */
        if (dst_existed) {
            if (saved_dst_ino.sector_count > 0 && saved_dst_ino.start_lba != 0) {
                for (u32 s = 0; s < saved_dst_ino.sector_count; s++) {
                    bitmap_set(g_astrafs.bitmap, saved_dst_ino.start_lba + s);
                }
            }
            g_astrafs.inodes[dst_slot] = saved_dst_ino;
            g_astrafs.vfs_nodes[dst_slot] = saved_vfs_dst;
            g_astrafs.inodes[src_slot] = saved_src_ino;
            g_astrafs.vfs_nodes[src_slot] = saved_vfs_src;
        } else {
            g_astrafs.inodes[src_slot] = saved_src_ino;
            g_astrafs.vfs_nodes[src_slot] = saved_vfs_src;
        }
    }
    astrafs_unlock();
    return ret;
}

/* VFS Ops Adaptors */
static int astrafs_op_open(vfs_node_t *node, u32 flags) {
    (void)node;
    (void)flags;
    return VFS_OK;
}

static int astrafs_op_close(vfs_node_t *node) {
    (void)node;
    return VFS_OK;
}

static ssize_t astrafs_op_read(vfs_node_t *node, void *buf, size_t count, size_t offset) {
    return astrafs_read(node, buf, count, offset);
}

static ssize_t astrafs_op_write(vfs_node_t *node, const void *buf, size_t count, size_t offset) {
    return astrafs_write(node, buf, count, offset);
}

static int astrafs_op_stat(vfs_node_t *node, vfs_stat_t *st) {
    return astrafs_stat(node, st);
}

static int astrafs_op_readdir(vfs_node_t *node, size_t index, vfs_dirent_t *dirent) {
    astrafs_lock();
    int ret = astrafs_node_readdir_locked(node, index, dirent);
    astrafs_unlock();
    return ret;
}

static int astrafs_op_lookup(vfs_node_t *parent, const char *name, vfs_node_t **out_node) {
    (void)parent;
    return astrafs_lookup(name, out_node);
}

static int astrafs_op_fsync(vfs_node_t *node) {
    return astrafs_fsync(node);
}

static int astrafs_op_rename(vfs_node_t *parent, const char *old_name, const char *new_name) {
    (void)parent;
    return astrafs_rename(old_name, new_name);
}

/* Guest Subsystem Self-Test */
bool astrafs_self_test(void) {
    kprintf("[TEST] Running AstraFS persistent filesystem self-test...\n");

    if (!g_astrafs.mounted) {
        kprintf("[FAIL] AstraFS is not mounted.\n");
        return false;
    }

    /* 1. Test file creation */
    vfs_node_t *node = NULL;
    int err = astrafs_create("test_verify.txt", VFS_O_CREAT | VFS_O_RDWR, &node);
    if (err != VFS_OK || !node) {
        kprintf("[FAIL] AstraFS create failed (err=%d)\n", err);
        return false;
    }

    /* 2. Test file write */
    const char *test_msg = "ASTRAFS_DURABILITY_TEST_PAYLOAD_12345678";
    size_t test_len = kstrlen(test_msg);
    ssize_t w_bytes = astrafs_write(node, test_msg, test_len, 0);
    if (w_bytes != (ssize_t)test_len) {
        kprintf("[FAIL] AstraFS write failed (written=%ld, expected=%lu)\n", (long)w_bytes, (unsigned long)test_len);
        return false;
    }

    /* 3. Test file read back */
    char read_buf[64];
    kmemset(read_buf, 0, sizeof(read_buf));
    ssize_t r_bytes = astrafs_read(node, read_buf, sizeof(read_buf) - 1, 0);
    if (r_bytes != (ssize_t)test_len || kstrcmp(read_buf, test_msg) != 0) {
        kprintf("[FAIL] AstraFS read-back mismatch: '%s'\n", read_buf);
        return false;
    }

    /* 4. Test file readdir */
    vfs_dirent_t de;
    bool found = false;
    for (size_t idx = 0; ; idx++) {
        int r_err = astrafs_readdir(idx, &de);
        if (r_err == VFS_ERR_EOF) break;
        if (r_err == VFS_OK && kstrcmp(de.name, "test_verify.txt") == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        kprintf("[FAIL] AstraFS readdir did not find test_verify.txt\n");
        return false;
    }

    /* 5. Test truncation */
    if (astrafs_truncate(node, 0) != VFS_OK || node->size != 0) {
        kprintf("[FAIL] AstraFS truncate to 0 failed\n");
        return false;
    }

    /* 6. Test CoW overwrite & write ordering */
    const char *cow_msg1 = "COW_INITIAL_PAYLOAD_AAA";
    size_t cow_len1 = kstrlen(cow_msg1);
    if (astrafs_write(node, cow_msg1, cow_len1, 0) != (ssize_t)cow_len1) {
        kprintf("[FAIL] AstraFS CoW initial write failed\n");
        return false;
    }
    const char *cow_msg2 = "COW_OVERWRITE_PAYLOAD_BBB";
    size_t cow_len2 = kstrlen(cow_msg2);
    if (astrafs_write(node, cow_msg2, cow_len2, 0) != (ssize_t)cow_len2) {
        kprintf("[FAIL] AstraFS CoW overwrite failed\n");
        return false;
    }
    kmemset(read_buf, 0, sizeof(read_buf));
    if (astrafs_read(node, read_buf, sizeof(read_buf) - 1, 0) != (ssize_t)cow_len2 ||
        kstrcmp(read_buf, cow_msg2) != 0) {
        kprintf("[FAIL] AstraFS CoW read-back mismatch: '%s'\n", read_buf);
        return false;
    }

    /* 7. Test fsync */
    if (astrafs_fsync(node) != VFS_OK) {
        kprintf("[FAIL] AstraFS fsync failed\n");
        return false;
    }

    /* 8. Test atomic rename */
    if (astrafs_rename("test_verify.txt", "test_renamed.txt") != VFS_OK) {
        kprintf("[FAIL] AstraFS rename failed\n");
        return false;
    }
    vfs_node_t *r_node = NULL;
    if (astrafs_lookup("test_renamed.txt", &r_node) != VFS_OK || !r_node) {
        kprintf("[FAIL] AstraFS lookup renamed file failed\n");
        return false;
    }
    if (astrafs_lookup("test_verify.txt", &r_node) == VFS_OK) {
        kprintf("[FAIL] AstraFS old file still visible after rename\n");
        return false;
    }

    /* Clean up renamed test file */
    astrafs_truncate(r_node, 0);

#ifndef HOST_TEST
    /* 9. Kernel-Level Filesystem Concurrency Test (Phase AJ Task 4) */
    kprintf("[TEST] Running AstraFS kernel-level multi-worker concurrency stress test...\n");
    g_fs_conc_state.workers_done = 0;
    g_fs_conc_state.errors = 0;

    thread_t *t_a = thread_create("fs_worker_a", worker_fs_conc_a, NULL);
    thread_t *t_b = thread_create("fs_worker_b", worker_fs_conc_b, NULL);
    if (!t_a || !t_b) {
        kprintf("[FAIL] AstraFS: Failed to create concurrency test threads!\n");
        return false;
    }

    while (g_fs_conc_state.workers_done < 2 || sched_has_ready_threads()) {
        thread_yield();
    }
    sched_reap();

    if (g_fs_conc_state.errors != 0) {
        kprintf("[FAIL] AstraFS concurrency workers reported %d error(s)\n", g_fs_conc_state.errors);
        return false;
    }

    /* Verify both concurrent files exist and payloads match */
    vfs_node_t *na_chk = NULL;
    vfs_node_t *nb_chk = NULL;
    if (astrafs_lookup("conc_a.txt", &na_chk) != VFS_OK || !na_chk ||
        astrafs_lookup("conc_b.txt", &nb_chk) != VFS_OK || !nb_chk) {
        kprintf("[FAIL] AstraFS concurrent files not found after worker execution\n");
        return false;
    }

    char chk_a[32];
    char chk_b[32];
    kmemset(chk_a, 0, sizeof(chk_a));
    kmemset(chk_b, 0, sizeof(chk_b));
    if (astrafs_read(na_chk, chk_a, 20, 0) != 20 || kstrcmp(chk_a, "PAYLOAD_A_CONCURRENT") != 0 ||
        astrafs_read(nb_chk, chk_b, 20, 0) != 20 || kstrcmp(chk_b, "PAYLOAD_B_CONCURRENT") != 0) {
        kprintf("[FAIL] AstraFS concurrent read-back payload mismatch (a='%s', b='%s')\n", chk_a, chk_b);
        return false;
    }

    /* Verify metadata integrity */
    if (!validate_metadata_integrity(g_astrafs.dev, &g_astrafs.sb, g_astrafs.inodes, g_astrafs.bitmap)) {
        kprintf("[FAIL] AstraFS metadata integrity check failed after concurrency\n");
        return false;
    }

    /* Clean up concurrent test files */
    astrafs_truncate(na_chk, 0);
    astrafs_truncate(nb_chk, 0);

    kprintf("[TEST] Kernel concurrency verified: zero double-allocations, zero data corruption.\n");
    kprintf("ASTRA_FS_CONCURRENCY_OK\n");
#endif

    kprintf("ASTRA_FS_OK\n");
    return true;
}
