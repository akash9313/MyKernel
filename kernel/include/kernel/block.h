#ifndef ASTRA_BLOCK_H
#define ASTRA_BLOCK_H

#include <kernel/types.h>

#define BLOCK_SECTOR_SIZE 512
#define BLOCK_MAX_DEVICES 4
#define BLOCK_NAME_MAX 32

typedef enum {
    BLOCK_STATUS_UNINITIALIZED = 0,
    BLOCK_STATUS_READY,
    BLOCK_STATUS_BUSY,
    BLOCK_STATUS_ERROR,
    BLOCK_STATUS_OFFLINE
} block_status_t;

typedef enum {
    BLOCK_OK = 0,
    BLOCK_ERR_INVALID_PARAM = -1,
    BLOCK_ERR_OUT_OF_BOUNDS = -2,
    BLOCK_ERR_IO = -3,
    BLOCK_ERR_TIMEOUT = -4,
    BLOCK_ERR_NO_DEVICE = -5,
    BLOCK_ERR_READ_ONLY = -6,
    BLOCK_ERR_OVERFLOW = -7,
    BLOCK_ERR_UNSUPPORTED = -8
} block_err_t;

typedef struct block_device block_device_t;

typedef struct block_ops {
    int (*read_sectors)(block_device_t *dev, u64 lba, u32 count, void *buf);
    int (*write_sectors)(block_device_t *dev, u64 lba, u32 count, const void *buf);
    int (*flush)(block_device_t *dev);
} block_ops_t;

struct block_device {
    char name[BLOCK_NAME_MAX];
    u32 id;
    u32 sector_size;     /* Logical sector size in bytes (typically 512) */
    u64 total_sectors;   /* Total LBA sector capacity */
    u64 capacity_bytes;  /* total_sectors * sector_size */
    bool read_only;      /* Write protection flag */
    bool is_boot_device; /* Boot media guard flag */
    block_status_t status;
    const block_ops_t *ops;
    void *priv;          /* Driver private data */
};

/* Core subsystem lifecycle & registration */
void block_init(void);
int block_register_device(block_device_t *dev);
int block_get_device_count(void);
block_device_t *block_get_device(int idx);
block_device_t *block_find_by_name(const char *name);

/* High-level safe block operations with boundary & permission validation */
int block_read(block_device_t *dev, u64 lba, u32 count, void *buf);
int block_write(block_device_t *dev, u64 lba, u32 count, const void *buf);
int block_flush(block_device_t *dev);

/* Utility and diagnostic helpers */
const char *block_strerror(int err);
bool block_self_test(void);

#endif /* ASTRA_BLOCK_H */

