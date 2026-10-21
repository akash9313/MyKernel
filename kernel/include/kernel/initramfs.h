#ifndef ASTRA_KERNEL_INITRAMFS_H
#define ASTRA_KERNEL_INITRAMFS_H

#include <kernel/types.h>
#include <kernel/vfs.h>

#define ASTRA_INITRAMFS_MAGIC "ASTRAFS1"
#define ASTRA_INITRAMFS_VERSION 1
#define ASTRA_INITRAMFS_MAX_ENTRIES 256
#define ASTRA_INITRAMFS_MAX_PATH 128

/* On-disk archive header */
typedef struct {
    char magic[8];        /* "ASTRAFS1" */
    u32  version;         /* 1 */
    u32  header_size;     /* sizeof(astra_initramfs_header_t) = 64 */
    u32  total_size;      /* Total archive byte size */
    u32  entry_count;     /* Number of entries */
    u32  entries_offset;  /* Byte offset to entries array */
    u32  data_offset;     /* Byte offset to payload data blob */
    u8   reserved[32];    /* Zeroed */
} __attribute__((packed)) astra_initramfs_header_t;

/* On-disk entry in archive directory table */
typedef struct {
    char name[ASTRA_INITRAMFS_MAX_PATH]; /* Normalized absolute path e.g. "/hello.txt", "/bin", "/bin/user_app.elf" */
    u32  type;                           /* 1=File (VFS_NODE_FILE), 2=Dir (VFS_NODE_DIR) */
    u32  data_offset;                    /* Offset from archive base to file payload (0 for dir) */
    u32  size;                           /* File size in bytes (0 for dir) */
    u32  flags;                          /* Reserved / flags (0) */
} __attribute__((packed)) astra_initramfs_entry_t;

/* Initramfs validation error codes */
typedef enum {
    INITRAMFS_OK                      =  0,
    INITRAMFS_ERR_NULL_PTR            = -1,
    INITRAMFS_ERR_TOO_SMALL           = -2,
    INITRAMFS_ERR_BAD_MAGIC           = -3,
    INITRAMFS_ERR_UNSUPPORTED_VERSION = -4,
    INITRAMFS_ERR_CORRUPT             = -5,
    INITRAMFS_ERR_OVERFLOW            = -6,
    INITRAMFS_ERR_TRAVERSAL           = -7,
    INITRAMFS_ERR_DUPLICATE           = -8,
    INITRAMFS_ERR_CONFLICT            = -9,
    INITRAMFS_ERR_BOUNDS              = -10,
    INITRAMFS_ERR_NOMEM               = -11,
} initramfs_err_t;

/* Public Initramfs API */
int initramfs_validate(const void *archive_data, size_t archive_size);
int initramfs_mount(const void *archive_data, size_t archive_size, vfs_node_t **out_root);
const char *initramfs_strerror(int err);

#endif /* ASTRA_KERNEL_INITRAMFS_H */
