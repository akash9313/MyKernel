#ifndef ASTRA_KERNEL_VFS_H
#define ASTRA_KERNEL_VFS_H

#include <kernel/types.h>

/* File System Node Types */
typedef enum {
    VFS_NODE_NONE = 0,
    VFS_NODE_FILE = 1,
    VFS_NODE_DIR  = 2,
} vfs_node_type_t;

/* Explicit VFS Error Codes */
typedef enum {
    VFS_OK                 =  0,
    VFS_ERR_INVALID_PATH   = -1,
    VFS_ERR_NOT_FOUND      = -2,
    VFS_ERR_INVALID_HANDLE = -3,
    VFS_ERR_IS_DIR         = -4,
    VFS_ERR_NOT_DIR        = -5,
    VFS_ERR_READ_ONLY      = -6,
    VFS_ERR_UNSUPPORTED    = -7,
    VFS_ERR_EOF            = -8,
    VFS_ERR_NOMEM          = -9,
    VFS_ERR_INVAL          = -10,
    VFS_ERR_CORRUPT        = -11,
    VFS_ERR_BUSY           = -12,
} vfs_err_t;

/* Open Flags */
#define VFS_O_RDONLY 0x0001
#define VFS_O_WRONLY 0x0002
#define VFS_O_RDWR   0x0004

#define VFS_MAX_NAME_LEN 64
#define VFS_MAX_PATH_LEN 128
#define VFS_MAX_FDS      32

typedef int vfs_fd_t;

typedef struct {
    char            name[VFS_MAX_NAME_LEN];
    vfs_node_type_t type;
    size_t          size;
} vfs_stat_t;

typedef struct {
    char            name[VFS_MAX_NAME_LEN];
    vfs_node_type_t type;
    size_t          size;
} vfs_dirent_t;

struct vfs_node;
typedef struct vfs_node vfs_node_t;

typedef struct vfs_ops {
    int (*open)(vfs_node_t *node, u32 flags);
    int (*close)(vfs_node_t *node);
    ssize_t (*read)(vfs_node_t *node, void *buf, size_t count, size_t offset);
    int (*stat)(vfs_node_t *node, vfs_stat_t *st);
    int (*readdir)(vfs_node_t *node, size_t index, vfs_dirent_t *dirent);
    int (*lookup)(vfs_node_t *parent, const char *name, vfs_node_t **out_node);
} vfs_ops_t;

struct vfs_node {
    char            name[VFS_MAX_NAME_LEN];
    vfs_node_type_t type;
    size_t          size;
    const vfs_ops_t *ops;
    void           *fs_data;
    vfs_node_t     *parent;
};

/* VFS Public API */
int vfs_init(void);
int vfs_mount_root(vfs_node_t *root_node);
vfs_node_t *vfs_get_root(void);
int vfs_lookup(const char *path, vfs_node_t **out_node);
vfs_fd_t vfs_open(const char *path, u32 flags);
ssize_t vfs_read(vfs_fd_t fd, void *buf, size_t count);
ssize_t vfs_pread(vfs_fd_t fd, void *buf, size_t count, size_t offset);
int vfs_close(vfs_fd_t fd);
int vfs_stat(const char *path, vfs_stat_t *st);
int vfs_fstat(vfs_fd_t fd, vfs_stat_t *st);
int vfs_readdir(const char *path, size_t index, vfs_dirent_t *dirent);
const char *vfs_strerror(int err);
void vfs_initramfs_run_tests(void);

#endif /* ASTRA_KERNEL_VFS_H */
