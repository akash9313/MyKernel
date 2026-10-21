#include <kernel/vfs.h>
#include <kernel/heap.h>
#include <kernel/panic.h>
#include <kernel/kprintf.h>

typedef struct vfs_file {
    bool        in_use;
    vfs_node_t *node;
    size_t      offset;
    u32         flags;
} vfs_file_t;

static vfs_node_t *g_root_node = NULL;
static vfs_file_t  g_file_table[VFS_MAX_FDS];

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
        default:                     return "Unknown VFS error";
    }
}

int vfs_init(void) {
    g_root_node = NULL;
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

    /* Scan for disallowed components: '..', empty components '//', or './' */
    for (size_t i = 0; i < len; i++) {
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

    /*
     * Direct root ops lookup fast path if supported by root node.
     * Underlying initramfs implements parent lookup for full relative/nested paths.
     */
    if (g_root_node->ops && g_root_node->ops->lookup) {
        return g_root_node->ops->lookup(g_root_node, path, out_node);
    }

    return VFS_ERR_UNSUPPORTED;
}

vfs_fd_t vfs_open(const char *path, u32 flags) {
    if (!path) return VFS_ERR_INVAL;

    /* Write operations are explicitly rejected on read-only VFS */
    if (flags & (VFS_O_WRONLY | VFS_O_RDWR)) {
        return VFS_ERR_READ_ONLY;
    }

    vfs_node_t *node = NULL;
    int err = vfs_lookup(path, &node);
    if (err != VFS_OK) {
        return err;
    }
    if (!node) {
        return VFS_ERR_NOT_FOUND;
    }

    if (node->type == VFS_NODE_DIR) {
        return VFS_ERR_IS_DIR;
    }

    /* Find free file descriptor */
    int fd = -1;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
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
    g_file_table[fd].node = node;
    g_file_table[fd].offset = 0;
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

int vfs_close(vfs_fd_t fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return VFS_ERR_INVALID_HANDLE;
    if (!g_file_table[fd].in_use) return VFS_ERR_INVALID_HANDLE;

    vfs_node_t *node = g_file_table[fd].node;
    if (node && node->ops && node->ops->close) {
        node->ops->close(node);
    }

    g_file_table[fd].in_use = false;
    g_file_table[fd].node = NULL;
    g_file_table[fd].offset = 0;
    g_file_table[fd].flags = 0;

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

    if (node->ops && node->ops->readdir) {
        return node->ops->readdir(node, index, dirent);
    }

    return VFS_ERR_UNSUPPORTED;
}
