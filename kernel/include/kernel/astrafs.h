#ifndef ASTRA_KERNEL_ASTRAFS_H
#define ASTRA_KERNEL_ASTRAFS_H

#include <kernel/types.h>
#include <kernel/block.h>
#include <kernel/vfs.h>

#define ASTRAFS_MAGIC             0x41534653U /* "ASFS" */
#define ASTRAFS_VERSION           1
#define ASTRAFS_SECTOR_SIZE       512
#define ASTRAFS_MAX_FILES         64
#define ASTRAFS_FILENAME_MAX      48
#define ASTRAFS_FLAG_DIR          0x80000000U

/* On-Disk Metadata Layout */
#define ASTRAFS_SUPERBLOCK_A_LBA  1
#define ASTRAFS_SUPERBLOCK_B_LBA  2
#define ASTRAFS_INODE_A_LBA       16
#define ASTRAFS_INODE_B_LBA       32
#define ASTRAFS_INODE_SECTORS     16   /* 16 * 512 = 8192 B = 64 * 128 B */
#define ASTRAFS_BITMAP_A_LBA      48
#define ASTRAFS_BITMAP_B_LBA      64
#define ASTRAFS_BITMAP_SECTORS    16   /* 16 * 512 = 8192 B = 65536 bits */
#define ASTRAFS_MAX_SECTORS       (ASTRAFS_BITMAP_SECTORS * ASTRAFS_SECTOR_SIZE * 8) /* 65536 sectors = 32 MiB */
#define ASTRAFS_DATA_START_LBA    128

typedef struct astrafs_inode {
    char name[ASTRAFS_FILENAME_MAX]; /* Null-terminated filename */
    u32  in_use;                     /* 1 = active, 0 = unused slot */
    u32  file_size;                  /* Exact byte size */
    u32  start_lba;                  /* Starting sector in data region */
    u32  sector_count;               /* Number of contiguous sectors */
    u32  flags;                      /* File flags / permissions */
    u32  data_checksum;              /* Fletcher-32 checksum of file payload */
    u64  created_time;               /* Monotonic commit generation or timestamp */
    u64  modified_time;              /* Monotonic modification generation */
    u8   reserved[40];               /* Padding to exact 128 bytes */
} __attribute__((packed)) astrafs_inode_t;
_Static_assert(sizeof(astrafs_inode_t) == 128, "astrafs_inode_t must be exactly 128 bytes");

typedef struct astrafs_superblock {
    u32 magic;                       /* ASTRAFS_MAGIC */
    u32 version;                     /* ASTRAFS_VERSION */
    u64 generation;                  /* Monotonic generation counter */
    u32 sector_size;                 /* 512 */
    u32 total_sectors;               /* Total volume sectors (e.g. 65536) */
    u32 data_start_lba;              /* 128 */
    u32 inode_table_lba;             /* Active Inode Table LBA (16 or 32) */
    u32 inode_sectors;               /* 16 */
    u32 bitmap_lba;                  /* Active Allocation Bitmap LBA (48 or 64) */
    u32 bitmap_sectors;              /* 16 */
    u32 file_count;                  /* Number of active files */
    u32 used_sectors;                /* Total data sectors allocated */
    u8  reserved[456];               /* Padding to 508 bytes */
    u32 checksum;                    /* Fletcher-32 of first 508 bytes */
} __attribute__((packed)) astrafs_superblock_t;
_Static_assert(sizeof(astrafs_superblock_t) == 512, "astrafs_superblock_t must be exactly 512 bytes");

typedef struct astrafs_state {
    block_device_t       *dev;
    bool                 mounted;
    astrafs_superblock_t sb;
    u8                   active_set; /* 0 = Set A, 1 = Set B */
    astrafs_inode_t      inodes[ASTRAFS_MAX_FILES];
    u8                   bitmap[ASTRAFS_BITMAP_SECTORS * ASTRAFS_SECTOR_SIZE];
    vfs_node_t           vfs_nodes[ASTRAFS_MAX_FILES];
    vfs_node_t           root_node;
} astrafs_state_t;

/* Checksum Helper */
static inline u32 astrafs_checksum(const void *data, size_t bytes) {
    const u16 *words = (const u16 *)data;
    size_t nwords = bytes / 2;
    u32 sum1 = 0xffff, sum2 = 0xffff;
    while (nwords > 0) {
        size_t tlen = nwords > 359 ? 359 : nwords;
        nwords -= tlen;
        do {
            sum1 += *words++;
            sum2 += sum1;
        } while (--tlen);
        sum1 = (sum1 & 0xffff) + (sum1 >> 16);
        sum2 = (sum2 & 0xffff) + (sum2 >> 16);
    }
    if (bytes & 1) {
        u8 last_byte = ((const u8 *)data)[bytes - 1];
        sum1 += (u16)last_byte;
        sum2 += sum1;
        sum1 = (sum1 & 0xffff) + (sum1 >> 16);
        sum2 = (sum2 & 0xffff) + (sum2 >> 16);
    }
    sum1 = (sum1 & 0xffff) + (sum1 >> 16);
    sum2 = (sum2 & 0xffff) + (sum2 >> 16);
    return (sum2 << 16) | sum1;
}

/* Public AstraFS API */
bool astrafs_is_blank(block_device_t *dev);
int astrafs_format(block_device_t *dev);
int astrafs_mount(block_device_t *dev);
int astrafs_unmount(void);
bool astrafs_is_mounted(void);
astrafs_state_t *astrafs_get_state(void);
vfs_node_t *astrafs_get_root_node(void);
const vfs_ops_t *astrafs_get_ops(void);

int astrafs_lookup(const char *name, vfs_node_t **out_node);
int astrafs_create(const char *name, u32 flags, vfs_node_t **out_node);
int astrafs_mkdir(const char *name);
int astrafs_unlink(const char *name);
int astrafs_rmdir(const char *name);
ssize_t astrafs_read(vfs_node_t *node, void *buf, size_t count, size_t offset);
ssize_t astrafs_write(vfs_node_t *node, const void *buf, size_t count, size_t offset);
int astrafs_truncate(vfs_node_t *node, size_t new_size);
int astrafs_stat(vfs_node_t *node, vfs_stat_t *st);
int astrafs_readdir(size_t index, vfs_dirent_t *dirent);
int astrafs_statfs(vfs_statfs_t *st);
int astrafs_fsync(vfs_node_t *node);
int astrafs_sync(void);
int astrafs_rename(const char *old_name, const char *new_name);

/* Guest integration self-test */
bool astrafs_self_test(void);

#endif /* ASTRA_KERNEL_ASTRAFS_H */
