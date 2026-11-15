#include <kernel/vfs.h>
#include <kernel/astrafs.h>
#include <kernel/heap.h>
#include <kernel/panic.h>
#include <kernel/kprintf.h>

typedef struct vfs_file {
    bool        in_use;
    pid_t       owner_pid;
    vfs_node_t *node;
    size_t      offset;
    u32         flags;
} vfs_file_t;

static size_t str_len(const char *s) {
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static int str_cmp(const char *a, const char *b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static int str_ncmp(const char *a, const char *b, size_t n) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0') {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }
    return 0;
}

static void str_copy(char *dst, const char *src, size_t max) {
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

/* Phase AC: Mutable RAM-backed file structure */
typedef struct {
    bool        in_use;
    char        name[VFS_MAX_PATH_LEN];
    size_t      size;
    vfs_node_t  node;
    u8          data[VFS_MAX_FILE_SIZE];
} vfs_mutable_file_t;

static vfs_node_t         *g_root_node = NULL;
static vfs_file_t          g_file_table[VFS_MAX_FDS];
static vfs_mutable_file_t  g_mutable_files[VFS_MAX_MUTABLE_FILES];
static size_t              g_total_mutable_allocated = 0;

/* Phase AC: Mutable RAM Filesystem Operations (ramfs) */
static int ramfs_op_open(vfs_node_t *node, u32 flags) {
    (void)node;
    (void)flags;
    return VFS_OK;
}

static int ramfs_op_close(vfs_node_t *node) {
    (void)node;
    return VFS_OK;
}

static ssize_t ramfs_op_read(vfs_node_t *node, void *buf, size_t count, size_t offset) {
    if (!node || !buf) return VFS_ERR_INVAL;
    vfs_mutable_file_t *mf = (vfs_mutable_file_t *)node->fs_data;
    if (!mf || !mf->in_use) return VFS_ERR_INVALID_HANDLE;

    if (offset >= mf->size) return 0; /* EOF */

    size_t available = mf->size - offset;
    size_t to_read = (count < available) ? count : available;

    for (size_t i = 0; i < to_read; i++) {
        ((u8 *)buf)[i] = mf->data[offset + i];
    }
    return (ssize_t)to_read;
}

static ssize_t ramfs_op_write(vfs_node_t *node, const void *buf, size_t count, size_t offset) {
    if (!node || !buf) return VFS_ERR_INVAL;
    if (count == 0) return 0;
    vfs_mutable_file_t *mf = (vfs_mutable_file_t *)node->fs_data;
    if (!mf || !mf->in_use) return VFS_ERR_INVALID_HANDLE;

    /* Reject 64-bit integer overflow */
    if (offset + count < offset) {
        return VFS_ERR_INVAL;
    }

    /* Reject writes exceeding maximum per-file capacity */
    if (offset + count > VFS_MAX_FILE_SIZE) {
        return VFS_ERR_NOSPC;
    }

    size_t new_size = (offset + count > mf->size) ? (offset + count) : mf->size;
    size_t growth = (new_size > mf->size) ? (new_size - mf->size) : 0;

    /* Reject writes exceeding total storage quota */
    if (g_total_mutable_allocated + growth > VFS_TOTAL_MUTABLE_CAPACITY) {
        return VFS_ERR_NOSPC;
    }

    /* Zero-fill any unwritten gap when writing past EOF */
    if (offset > mf->size) {
        for (size_t i = mf->size; i < offset; i++) {
            mf->data[i] = 0;
        }
    }

    /* Write data into buffer */
    for (size_t i = 0; i < count; i++) {
        mf->data[offset + i] = ((const u8 *)buf)[i];
    }

    g_total_mutable_allocated += growth;
    mf->size = new_size;
    node->size = new_size;

    return (ssize_t)count;
}

static int ramfs_op_stat(vfs_node_t *node, vfs_stat_t *st) {
    if (!node || !st) return VFS_ERR_INVAL;
    vfs_mutable_file_t *mf = (vfs_mutable_file_t *)node->fs_data;
    if (!mf || !mf->in_use) return VFS_ERR_NOT_FOUND;

    str_copy(st->name, node->name, sizeof(st->name));
    st->type = node->type;
    st->size = mf->size;
    return VFS_OK;
}

static int ramfs_op_readdir(vfs_node_t *node, size_t index, vfs_dirent_t *dirent) {
    if (!node || !dirent) return VFS_ERR_INVAL;
    if (node->type != VFS_NODE_DIR) return VFS_ERR_NOT_DIR;

    const char *parent_path = node->name;
    size_t plen = str_len(parent_path);
    size_t match_idx = 0;

    for (int i = 0; i < VFS_MAX_MUTABLE_FILES; i++) {
        if (!g_mutable_files[i].in_use) continue;
        const char *p = g_mutable_files[i].name;
        if (str_ncmp(p, parent_path, plen) == 0 && p[plen] == '/') {
            const char *sub = p + plen + 1;
            bool direct = true;
            for (size_t s = 0; sub[s]; s++) {
                if (sub[s] == '/') { direct = false; break; }
            }
            if (direct) {
                if (match_idx == index) {
                    str_copy(dirent->name, sub, sizeof(dirent->name));
                    dirent->type = g_mutable_files[i].node.type;
                    dirent->size = g_mutable_files[i].size;
                    return VFS_OK;
                }
                match_idx++;
            }
        }
    }
    return VFS_ERR_EOF;
}

static const vfs_ops_t g_ramfs_ops = {
    .open    = ramfs_op_open,
    .close   = ramfs_op_close,
    .read    = ramfs_op_read,
    .write   = ramfs_op_write,
    .stat    = ramfs_op_stat,
    .readdir = ramfs_op_readdir,
    .lookup  = NULL,
};

const char *vfs_strerror(int err) {
    switch (err) {
        case VFS_OK:                 return "Success";
        case VFS_ERR_INVALID_PATH:   return "Invalid path syntax or traversal attempt";
        case VFS_ERR_NOT_FOUND:      return "No such file or directory";
        case VFS_ERR_INVALID_HANDLE: return "Invalid file descriptor";
        case VFS_ERR_IS_DIR:         return "Path is a directory, not a regular file";
        case VFS_ERR_NOT_DIR:        return "Path component is not a directory";
        case VFS_ERR_READ_ONLY:      return "Read-only filesystem: write or modify rejected";
        case VFS_ERR_UNSUPPORTED:    return "Operation not supported on this node";
        case VFS_ERR_EOF:            return "End of file";
        case VFS_ERR_NOMEM:          return "Out of memory";
        case VFS_ERR_INVAL:          return "Invalid argument";
        case VFS_ERR_CORRUPT:        return "Filesystem or archive corrupted";
        case VFS_ERR_BUSY:           return "Resource or file table busy";
        case VFS_ERR_EXISTS:         return "File already exists";
        case VFS_ERR_NOSPC:          return "No space left on device or storage quota exceeded";
        default:                     return "Unknown VFS error";
    }
}

int vfs_init(void) {
    g_root_node = NULL;
    g_total_mutable_allocated = 0;
    for (int i = 0; i < VFS_MAX_MUTABLE_FILES; i++) {
        g_mutable_files[i].in_use = false;
        g_mutable_files[i].name[0] = '\0';
        g_mutable_files[i].size = 0;
        g_mutable_files[i].node.name[0] = '\0';
        g_mutable_files[i].node.type = VFS_NODE_NONE;
        g_mutable_files[i].node.size = 0;
        g_mutable_files[i].node.ops = NULL;
        g_mutable_files[i].node.fs_data = NULL;
        g_mutable_files[i].node.parent = NULL;
    }
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        g_file_table[i].in_use = false;
        g_file_table[i].node = NULL;
        g_file_table[i].offset = 0;
        g_file_table[i].flags = 0;
    }
    return VFS_OK;
}

int vfs_mount_root(vfs_node_t *root_node) {
    if (!root_node) return VFS_ERR_INVAL;
    if (root_node->type != VFS_NODE_DIR) return VFS_ERR_NOT_DIR;
    g_root_node = root_node;
    return VFS_OK;
}

vfs_node_t *vfs_get_root(void) {
    return g_root_node;
}

static int validate_path_syntax(const char *path) {
    if (!path) return VFS_ERR_INVALID_PATH;
    size_t len = str_len(path);
    if (len == 0 || len >= VFS_MAX_PATH_LEN) return VFS_ERR_INVALID_PATH;
    if (path[0] != '/') return VFS_ERR_INVALID_PATH; /* Absolute path required */

    /* Scan for disallowed components: '..', empty components '//', './', or control characters */
    for (size_t i = 0; i < len; i++) {
        /* Disallow ASCII control characters (< 32) and DEL (127) */
        if ((unsigned char)path[i] < 32 || (unsigned char)path[i] == 127) {
            return VFS_ERR_INVALID_PATH;
        }
        /* Disallow double slash // */
        if (path[i] == '/' && i + 1 < len && path[i + 1] == '/') {
            return VFS_ERR_INVALID_PATH;
        }
        /* Disallow /./ or trailing /. */
        if (path[i] == '/' && i + 1 < len && path[i + 1] == '.') {
            if (i + 2 == len || path[i + 2] == '/') {
                return VFS_ERR_INVALID_PATH;
            }
            /* Disallow /../ or trailing /.. */
            if (path[i + 2] == '.' && (i + 3 == len || path[i + 3] == '/')) {
                return VFS_ERR_INVALID_PATH;
            }
        }
    }
    return VFS_OK;
}

int vfs_lookup(const char *path, vfs_node_t **out_node) {
    if (!out_node || !path) return VFS_ERR_INVAL;
    *out_node = NULL;

    if (!g_root_node) return VFS_ERR_NOT_FOUND;

    int syn_err = validate_path_syntax(path);
    if (syn_err != VFS_OK) return syn_err;

    /* Root path "/" */
    if (str_cmp(path, "/") == 0) {
        *out_node = g_root_node;
        return VFS_OK;
    }

    /* AstraFS mount check for /data */
    if (astrafs_is_mounted()) {
        if (str_cmp(path, "/data") == 0 || str_cmp(path, "/data/") == 0) {
            *out_node = astrafs_get_root_node();
            return VFS_OK;
        }
        if (str_ncmp(path, "/data/", 6) == 0) {
            int a_err = astrafs_lookup(path + 6, out_node);
            if (a_err == VFS_OK) {
                return VFS_OK;
            }
        }
    }

    /* 1. Fast check in mutable files table */
    for (int i = 0; i < VFS_MAX_MUTABLE_FILES; i++) {
        if (g_mutable_files[i].in_use && str_cmp(g_mutable_files[i].name, path) == 0) {
            *out_node = &g_mutable_files[i].node;
            return VFS_OK;
        }
    }

    /*
     * 2. Direct root ops lookup fast path if supported by root node.
     * Underlying initramfs implements parent lookup for full relative/nested paths.
     */
    if (g_root_node->ops && g_root_node->ops->lookup) {
        return g_root_node->ops->lookup(g_root_node, path, out_node);
    }

    return VFS_ERR_UNSUPPORTED;
}

vfs_fd_t vfs_open(const char *path, u32 flags) {
    if (!path) return VFS_ERR_INVAL;

    int syn_err = validate_path_syntax(path);
    if (syn_err != VFS_OK) return syn_err;

    vfs_node_t *node = NULL;
    int err = vfs_lookup(path, &node);

    /* Existing file */
    if (err == VFS_OK && node != NULL) {
        if (node->type == VFS_NODE_DIR) {
            return VFS_ERR_IS_DIR;
        }

        /* Write operations are explicitly rejected if the node does not support write,
         * unless VFS_O_CREAT is requested, which promotes read-only initramfs files to mutable ramfs (Copy-On-Write) */
        if (flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_TRUNC)) {
            if (!node->ops || !node->ops->write) {
                if (flags & VFS_O_CREAT) {
                    /* Allocate a new mutable ramfs file to shadow/overlay the read-only node */
                    int slot = -1;
                    for (int i = 0; i < VFS_MAX_MUTABLE_FILES; i++) {
                        if (!g_mutable_files[i].in_use) {
                            slot = i;
                            break;
                        }
                    }
                    if (slot < 0) {
                        return VFS_ERR_NOSPC;
                    }

                    vfs_mutable_file_t *mf = &g_mutable_files[slot];
                    mf->in_use = true;
                    str_copy(mf->name, path, sizeof(mf->name));
                    mf->size = 0;
                    for (size_t b = 0; b < VFS_MAX_FILE_SIZE; b++) {
                        mf->data[b] = 0;
                    }

                    /* If not truncating, copy existing contents */
                    if (!(flags & VFS_O_TRUNC) && node->ops && node->ops->read) {
                        size_t to_copy = (node->size < VFS_MAX_FILE_SIZE) ? node->size : VFS_MAX_FILE_SIZE;
                        ssize_t rd = node->ops->read(node, mf->data, to_copy, 0);
                        if (rd > 0) {
                            mf->size = (size_t)rd;
                            g_total_mutable_allocated += mf->size;
                        }
                    }

                    str_copy(mf->node.name, path, sizeof(mf->node.name));
                    mf->node.type = VFS_NODE_FILE;
                    mf->node.size = mf->size;
                    mf->node.ops = &g_ramfs_ops;
                    mf->node.fs_data = mf;
                    mf->node.parent = g_root_node;

                    node = &mf->node;
                } else {
                    return VFS_ERR_READ_ONLY;
                }
            }
        }

        /* Handle O_TRUNC on writable node */
        if ((flags & VFS_O_TRUNC) && (flags & (VFS_O_WRONLY | VFS_O_RDWR))) {
            if (node->ops == astrafs_get_ops()) {
                astrafs_truncate(node, 0);
            } else {
                vfs_mutable_file_t *mf = (vfs_mutable_file_t *)node->fs_data;
                if (mf && mf->in_use) {
                    if (g_total_mutable_allocated >= mf->size) {
                        g_total_mutable_allocated -= mf->size;
                    }
                    mf->size = 0;
                    node->size = 0;
                }
            }
        }
    } else if (err == VFS_ERR_NOT_FOUND && (flags & VFS_O_CREAT)) {
        bool created_astrafs = false;
        if (astrafs_is_mounted() && str_ncmp(path, "/data/", 6) == 0) {
            bool has_sub = false;
            for (const char *s = path + 6; *s; s++) {
                if (*s == '/') { has_sub = true; break; }
            }
            if (!has_sub) {
                int cr_err = astrafs_create(path + 6, flags, &node);
                if (cr_err == VFS_OK) {
                    created_astrafs = true;
                } else {
                    return cr_err;
                }
            }
        }
        if (!created_astrafs) {
            /* Allocate a new mutable file in ramfs */
            int slot = -1;
            for (int i = 0; i < VFS_MAX_MUTABLE_FILES; i++) {
                if (!g_mutable_files[i].in_use) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                return VFS_ERR_NOSPC; /* Maximum file count reached */
            }

            vfs_mutable_file_t *mf = &g_mutable_files[slot];
            mf->in_use = true;
            str_copy(mf->name, path, sizeof(mf->name));
            mf->size = 0;
            for (size_t b = 0; b < VFS_MAX_FILE_SIZE; b++) {
                mf->data[b] = 0;
            }

            str_copy(mf->node.name, path, sizeof(mf->node.name));
            mf->node.type = VFS_NODE_FILE;
            mf->node.size = 0;
            mf->node.ops = &g_ramfs_ops;
            mf->node.fs_data = mf;
            mf->node.parent = g_root_node;

            node = &mf->node;
        }
    } else {
        return err;
    }

    /* Find free file descriptor (0, 1, 2 reserved for stdin, stdout, stderr) */
    int fd = -1;
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!g_file_table[i].in_use) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        return VFS_ERR_BUSY;
    }

    if (node->ops && node->ops->open) {
        int o_err = node->ops->open(node, flags);
        if (o_err != VFS_OK) {
            return o_err;
        }
    }

    g_file_table[fd].in_use = true;
    g_file_table[fd].owner_pid = 0;
    g_file_table[fd].node = node;
    g_file_table[fd].offset = (flags & VFS_O_APPEND) ? node->size : 0;
    g_file_table[fd].flags = flags;

    return fd;
}

ssize_t vfs_pread(vfs_fd_t fd, void *buf, size_t count, size_t offset) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return VFS_ERR_INVALID_HANDLE;
    if (!g_file_table[fd].in_use) return VFS_ERR_INVALID_HANDLE;
    if (!buf) return VFS_ERR_INVAL;
    if (count == 0) return 0;

    vfs_node_t *node = g_file_table[fd].node;
    if (!node) return VFS_ERR_INVALID_HANDLE;
    if (node->type == VFS_NODE_DIR) return VFS_ERR_IS_DIR;

    /* Bounds and EOF check */
    if (offset >= node->size) {
        return 0; /* EOF */
    }

    /* Overflow check */
    if (offset + count < offset) {
        return VFS_ERR_INVAL;
    }

    size_t available = node->size - offset;
    size_t to_read = (count < available) ? count : available;

    if (node->ops && node->ops->read) {
        return node->ops->read(node, buf, to_read, offset);
    }

    return VFS_ERR_UNSUPPORTED;
}

ssize_t vfs_read(vfs_fd_t fd, void *buf, size_t count) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return VFS_ERR_INVALID_HANDLE;
    if (!g_file_table[fd].in_use) return VFS_ERR_INVALID_HANDLE;

    size_t cur_offset = g_file_table[fd].offset;
    ssize_t bytes_read = vfs_pread(fd, buf, count, cur_offset);
    if (bytes_read > 0) {
        g_file_table[fd].offset += (size_t)bytes_read;
    }
    return bytes_read;
}

ssize_t vfs_pwrite(vfs_fd_t fd, const void *buf, size_t count, size_t offset) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return VFS_ERR_INVALID_HANDLE;
    if (!g_file_table[fd].in_use) return VFS_ERR_INVALID_HANDLE;
    if (!buf) return VFS_ERR_INVAL;
    if (count == 0) return 0;

    /* Check if opened with write permissions */
    if (!(g_file_table[fd].flags & (VFS_O_WRONLY | VFS_O_RDWR))) {
        return VFS_ERR_READ_ONLY;
    }

    vfs_node_t *node = g_file_table[fd].node;
    if (!node) return VFS_ERR_INVALID_HANDLE;
    if (node->type == VFS_NODE_DIR) return VFS_ERR_IS_DIR;

    /* Overflow check */
    if (offset + count < offset) {
        return VFS_ERR_INVAL;
    }

    if (node->ops && node->ops->write) {
        return node->ops->write(node, buf, count, offset);
    }

    return VFS_ERR_READ_ONLY;
}

ssize_t vfs_write(vfs_fd_t fd, const void *buf, size_t count) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return VFS_ERR_INVALID_HANDLE;
    if (!g_file_table[fd].in_use) return VFS_ERR_INVALID_HANDLE;

    size_t cur_offset = g_file_table[fd].offset;
    ssize_t bytes_written = vfs_pwrite(fd, buf, count, cur_offset);
    if (bytes_written > 0) {
        g_file_table[fd].offset += (size_t)bytes_written;
    }
    return bytes_written;
}

int vfs_close(vfs_fd_t fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return VFS_ERR_INVALID_HANDLE;
    if (!g_file_table[fd].in_use) return VFS_ERR_INVALID_HANDLE;

    vfs_node_t *node = g_file_table[fd].node;
    if (node && node->ops && node->ops->close) {
        node->ops->close(node);
    }

    g_file_table[fd].in_use = false;
    g_file_table[fd].owner_pid = 0;
    g_file_table[fd].node = NULL;
    g_file_table[fd].offset = 0;
    g_file_table[fd].flags = 0;

    return VFS_OK;
}

pid_t vfs_get_owner(vfs_fd_t fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !g_file_table[fd].in_use) return 0;
    return g_file_table[fd].owner_pid;
}

int vfs_set_owner(vfs_fd_t fd, pid_t pid) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !g_file_table[fd].in_use) return VFS_ERR_INVALID_HANDLE;
    g_file_table[fd].owner_pid = pid;
    return VFS_OK;
}

int vfs_close_all_for_pid(pid_t pid) {
    if (pid == 0) return 0;
    int closed = 0;
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (g_file_table[i].in_use && g_file_table[i].owner_pid == pid) {
            vfs_close((vfs_fd_t)i);
            closed++;
        }
    }
    return closed;
}

int vfs_fsync(vfs_fd_t fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return VFS_ERR_INVALID_HANDLE;
    if (!g_file_table[fd].in_use) return VFS_ERR_INVALID_HANDLE;

    vfs_node_t *node = g_file_table[fd].node;
    if (!node) return VFS_ERR_INVALID_HANDLE;

    if (node->ops && node->ops->fsync) {
        return node->ops->fsync(node);
    }
    return VFS_OK;
}

int vfs_sync(void) {
    int ret = VFS_OK;
    if (astrafs_is_mounted()) {
        int r = astrafs_sync();
        if (r != VFS_OK) ret = r;
    }

    int dev_count = block_get_device_count();
    for (int i = 0; i < dev_count; i++) {
        block_device_t *dev = block_get_device(i);
        if (dev && !dev->read_only && !dev->is_boot_device) {
            int fr = block_flush(dev);
            if (fr != BLOCK_OK) {
                ret = VFS_ERR_BUSY;
            }
        }
    }
    return ret;
}

int vfs_rename(const char *old_path, const char *new_path) {
    if (!old_path || !new_path) return VFS_ERR_INVAL;

    int syn1 = validate_path_syntax(old_path);
    if (syn1 != VFS_OK) return syn1;
    int syn2 = validate_path_syntax(new_path);
    if (syn2 != VFS_OK) return syn2;

    /* If AstraFS is mounted, /data/ is a separate filesystem */
    if (astrafs_is_mounted()) {
        bool old_is_data = (str_ncmp(old_path, "/data/", 6) == 0);
        bool new_is_data = (str_ncmp(new_path, "/data/", 6) == 0);

        if (old_is_data != new_is_data) {
            return VFS_ERR_UNSUPPORTED; /* Cannot rename cross-filesystem */
        }
        if (old_is_data && new_is_data) {
            bool old_has_sub = false;
            for (const char *s = old_path + 6; *s; s++) {
                if (*s == '/') { old_has_sub = true; break; }
            }
            bool new_has_sub = false;
            for (const char *s = new_path + 6; *s; s++) {
                if (*s == '/') { new_has_sub = true; break; }
            }
            if (!old_has_sub && !new_has_sub) {
                vfs_node_t *old_node = NULL;
                (void)astrafs_lookup(old_path + 6, &old_node);
                int res = astrafs_rename(old_path + 6, new_path + 6);
                if (res == VFS_OK) {
                    /* Update any open file descriptor referencing the renamed file */
                    vfs_node_t *renamed_node = NULL;
                    if (astrafs_lookup(new_path + 6, &renamed_node) == VFS_OK && renamed_node) {
                        for (int i = 0; i < VFS_MAX_FDS; i++) {
                            if (g_file_table[i].in_use) {
                                if (g_file_table[i].node == renamed_node || (old_node && g_file_table[i].node == old_node)) {
                                    g_file_table[i].node = renamed_node;
                                    str_copy(renamed_node->name, new_path + 6, sizeof(renamed_node->name));
                                }
                            }
                        }
                    }
                }
                return res;
            }
        }
    }

    /* Handle mutable ramfs rename */
    int old_slot = -1;
    int new_slot = -1;
    for (int i = 0; i < VFS_MAX_MUTABLE_FILES; i++) {
        if (g_mutable_files[i].in_use) {
            if (str_cmp(g_mutable_files[i].name, old_path) == 0) old_slot = i;
            if (str_cmp(g_mutable_files[i].name, new_path) == 0) new_slot = i;
        }
    }
    if (old_slot < 0) {
        return VFS_ERR_NOT_FOUND;
    }

    if (new_slot >= 0 && new_slot != old_slot) {
        /* Destination exists in ramfs: overwrite destination */
        if (g_total_mutable_allocated >= g_mutable_files[new_slot].size) {
            g_total_mutable_allocated -= g_mutable_files[new_slot].size;
        }
        g_mutable_files[new_slot].in_use = false;
        /* Update any open descriptor referencing new_slot to point to old_slot */
        for (int i = 0; i < VFS_MAX_FDS; i++) {
            if (g_file_table[i].in_use && g_file_table[i].node == &g_mutable_files[new_slot].node) {
                g_file_table[i].node = &g_mutable_files[old_slot].node;
            }
        }
    }

    str_copy(g_mutable_files[old_slot].name, new_path, sizeof(g_mutable_files[old_slot].name));
    str_copy(g_mutable_files[old_slot].node.name, new_path, sizeof(g_mutable_files[old_slot].node.name));
    return VFS_OK;
}

int vfs_unlink(const char *path) {
    if (!path) return VFS_ERR_INVAL;

    if (astrafs_is_mounted() && str_ncmp(path, "/data/", 6) == 0) {
        vfs_node_t *anode = NULL;
        if (astrafs_lookup(path + 6, &anode) == VFS_OK && anode) {
            return astrafs_truncate(anode, 0);
        }
    }

    /* Check mutable ramfs files */
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MUTABLE_FILES; i++) {
        if (g_mutable_files[i].in_use && str_cmp(g_mutable_files[i].name, path) == 0) {
            slot = i;
            break;
        }
    }

    if (slot >= 0) {
        if (g_total_mutable_allocated >= g_mutable_files[slot].size) {
            g_total_mutable_allocated -= g_mutable_files[slot].size;
        }
        g_mutable_files[slot].in_use = false;
        g_mutable_files[slot].size = 0;
        g_mutable_files[slot].name[0] = '\0';
        for (int i = 0; i < VFS_MAX_FDS; i++) {
            if (g_file_table[i].in_use && g_file_table[i].node == &g_mutable_files[slot].node) {
                g_file_table[i].in_use = false;
                g_file_table[i].node = NULL;
            }
        }
        return VFS_OK;
    }

    /* Check if path is in read-only initramfs */
    vfs_node_t *node = NULL;
    int err = vfs_lookup(path, &node);
    if (err == VFS_OK && node) {
        return VFS_ERR_READ_ONLY;
    }

    return VFS_ERR_NOT_FOUND;
}

int vfs_mkdir(const char *path) {
    if (!path) return VFS_ERR_INVAL;
    int syn_err = validate_path_syntax(path);
    if (syn_err != VFS_OK) return syn_err;

    /* Check if node already exists */
    vfs_node_t *existing = NULL;
    if (vfs_lookup(path, &existing) == VFS_OK && existing) {
        return VFS_ERR_EXISTS;
    }

    /* Find empty slot in g_mutable_files */
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MUTABLE_FILES; i++) {
        if (!g_mutable_files[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return VFS_ERR_NOSPC;

    vfs_mutable_file_t *mf = &g_mutable_files[slot];
    mf->in_use = true;
    str_copy(mf->name, path, sizeof(mf->name));
    mf->size = 0;
    for (size_t b = 0; b < VFS_MAX_FILE_SIZE; b++) {
        mf->data[b] = 0;
    }

    str_copy(mf->node.name, path, sizeof(mf->node.name));
    mf->node.type = VFS_NODE_DIR;
    mf->node.size = 0;
    mf->node.ops = &g_ramfs_ops;
    mf->node.fs_data = mf;
    mf->node.parent = g_root_node;

    return VFS_OK;
}

int vfs_rmdir(const char *path) {
    if (!path) return VFS_ERR_INVAL;
    int syn_err = validate_path_syntax(path);
    if (syn_err != VFS_OK) return syn_err;

    /* Find mutable directory node */
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MUTABLE_FILES; i++) {
        if (g_mutable_files[i].in_use && str_cmp(g_mutable_files[i].name, path) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* Check if exists in initramfs */
        vfs_node_t *node = NULL;
        if (vfs_lookup(path, &node) == VFS_OK && node) {
            return VFS_ERR_READ_ONLY;
        }
        return VFS_ERR_NOT_FOUND;
    }

    if (g_mutable_files[slot].node.type != VFS_NODE_DIR) {
        return VFS_ERR_NOT_DIR;
    }

    /* Check if directory is empty */
    size_t plen = str_len(path);
    for (int i = 0; i < VFS_MAX_MUTABLE_FILES; i++) {
        if (!g_mutable_files[i].in_use || i == slot) continue;
        if (str_ncmp(g_mutable_files[i].name, path, plen) == 0 && g_mutable_files[i].name[plen] == '/') {
            return VFS_ERR_BUSY; /* Directory not empty */
        }
    }

    g_mutable_files[slot].in_use = false;
    g_mutable_files[slot].name[0] = '\0';
    g_mutable_files[slot].size = 0;
    return VFS_OK;
}

int vfs_stat(const char *path, vfs_stat_t *st) {
    if (!st || !path) return VFS_ERR_INVAL;

    vfs_node_t *node = NULL;
    int err = vfs_lookup(path, &node);
    if (err != VFS_OK) return err;
    if (!node) return VFS_ERR_NOT_FOUND;

    if (node->ops && node->ops->stat) {
        return node->ops->stat(node, st);
    }

    str_copy(st->name, node->name, sizeof(st->name));
    st->type = node->type;
    st->size = node->size;
    return VFS_OK;
}

int vfs_fstat(vfs_fd_t fd, vfs_stat_t *st) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return VFS_ERR_INVALID_HANDLE;
    if (!g_file_table[fd].in_use) return VFS_ERR_INVALID_HANDLE;
    if (!st) return VFS_ERR_INVAL;

    vfs_node_t *node = g_file_table[fd].node;
    if (!node) return VFS_ERR_INVALID_HANDLE;

    if (node->ops && node->ops->stat) {
        return node->ops->stat(node, st);
    }

    str_copy(st->name, node->name, sizeof(st->name));
    st->type = node->type;
    st->size = node->size;
    return VFS_OK;
}

int vfs_readdir(const char *path, size_t index, vfs_dirent_t *dirent) {
    if (!dirent || !path) return VFS_ERR_INVAL;

    vfs_node_t *node = NULL;
    int err = vfs_lookup(path, &node);
    if (err != VFS_OK) return err;
    if (!node) return VFS_ERR_NOT_FOUND;
    if (node->type != VFS_NODE_DIR) return VFS_ERR_NOT_DIR;

    size_t base_count = 0;
    if (node->ops && node->ops->readdir) {
        vfs_dirent_t dummy;
        while (node->ops->readdir(node, base_count, &dummy) == VFS_OK) {
            base_count++;
        }
        if (index < base_count) {
            return node->ops->readdir(node, index, dirent);
        }
    }

    /* Enumerate mutable ramfs files for this directory */
    size_t target_mutable_idx = index - base_count;
    size_t mutable_match = 0;
    bool is_root = (str_cmp(path, "/") == 0);
    size_t plen = str_len(path);

    for (int i = 0; i < VFS_MAX_MUTABLE_FILES; i++) {
        if (!g_mutable_files[i].in_use) continue;
        const char *p = g_mutable_files[i].name;
        const char *rel = NULL;
        if (is_root) {
            if (p[0] == '/' && p[1] != '\0') {
                const char *cand = p + 1;
                bool direct = true;
                for (size_t s = 0; cand[s]; s++) {
                    if (cand[s] == '/') { direct = false; break; }
                }
                if (direct) rel = cand;
            }
        } else {
            if (str_ncmp(p, path, plen) == 0 && p[plen] == '/') {
                const char *cand = p + plen + 1;
                bool direct = true;
                for (size_t s = 0; cand[s]; s++) {
                    if (cand[s] == '/') { direct = false; break; }
                }
                if (direct) rel = cand;
            }
        }
        if (rel) {
            if (mutable_match == target_mutable_idx) {
                str_copy(dirent->name, rel, sizeof(dirent->name));
                dirent->type = g_mutable_files[i].node.type;
                dirent->size = g_mutable_files[i].size;
                return VFS_OK;
            }
            mutable_match++;
        }
    }

    return VFS_ERR_EOF;
}
