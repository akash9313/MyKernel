#include <kernel/initramfs.h>
#include <kernel/vfs.h>
#include <kernel/heap.h>
#include <kernel/panic.h>
#include <kernel/kprintf.h>

#define MAX_MOUNTED_ENTRIES (ASTRA_INITRAMFS_MAX_ENTRIES + 1)

typedef struct {
    bool                            mounted;
    const void                     *archive_data;
    size_t                          archive_size;
    const astra_initramfs_header_t *header;
    const astra_initramfs_entry_t  *entries;
    vfs_node_t                      root_node;
    vfs_node_t                      node_pool[ASTRA_INITRAMFS_MAX_ENTRIES];
} initramfs_fs_t;

static initramfs_fs_t g_initramfs;

static size_t kstrlen(const char *s) {
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static int kstrcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static int kstrncmp(const char *a, const char *b, size_t n) {
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

static void *kmemcpy(void *dest, const void *src, size_t len) {
    u8 *d = (u8 *)dest;
    const u8 *s = (const u8 *)src;
    for (size_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return dest;
}

const char *initramfs_strerror(int err) {
    switch (err) {
        case INITRAMFS_OK:                      return "Success";
        case INITRAMFS_ERR_NULL_PTR:            return "NULL archive pointer";
        case INITRAMFS_ERR_TOO_SMALL:           return "Archive size smaller than header";
        case INITRAMFS_ERR_BAD_MAGIC:           return "Invalid archive magic signature";
        case INITRAMFS_ERR_UNSUPPORTED_VERSION: return "Unsupported archive version";
        case INITRAMFS_ERR_CORRUPT:             return "Archive header or entry structure corrupt";
        case INITRAMFS_ERR_OVERFLOW:            return "Arithmetic overflow detected in archive headers";
        case INITRAMFS_ERR_TRAVERSAL:           return "Path traversal or malformed path component detected";
        case INITRAMFS_ERR_DUPLICATE:           return "Duplicate archive entry name";
        case INITRAMFS_ERR_CONFLICT:            return "Conflicting file and directory paths";
        case INITRAMFS_ERR_BOUNDS:              return "Entry offset or size extends beyond archive bounds";
        case INITRAMFS_ERR_NOMEM:               return "Insufficient memory to mount initramfs";
        default:                                return "Unknown initramfs error";
    }
}

int initramfs_validate(const void *archive_data, size_t archive_size) {
    if (!archive_data) {
        return INITRAMFS_ERR_NULL_PTR;
    }
    if (archive_size < sizeof(astra_initramfs_header_t)) {
        return INITRAMFS_ERR_TOO_SMALL;
    }

    const astra_initramfs_header_t *hdr = (const astra_initramfs_header_t *)archive_data;

    /* 1. Magic check */
    if (kstrncmp(hdr->magic, ASTRA_INITRAMFS_MAGIC, 8) != 0) {
        return INITRAMFS_ERR_BAD_MAGIC;
    }

    /* 2. Version check */
    if (hdr->version != ASTRA_INITRAMFS_VERSION) {
        return INITRAMFS_ERR_UNSUPPORTED_VERSION;
    }

    /* 3. Header size and total archive size checks */
    if (hdr->header_size < sizeof(astra_initramfs_header_t) || hdr->header_size > hdr->total_size) {
        return INITRAMFS_ERR_CORRUPT;
    }
    if (hdr->total_size > archive_size || hdr->total_size < hdr->header_size) {
        return INITRAMFS_ERR_BOUNDS;
    }

    /* 4. Entry count bounds */
    if (hdr->entry_count > ASTRA_INITRAMFS_MAX_ENTRIES) {
        return INITRAMFS_ERR_BOUNDS;
    }

    /* 5. Entry table bounds and overflow check */
    u64 table_bytes = (u64)hdr->entry_count * (u64)sizeof(astra_initramfs_entry_t);
    if ((u32)(hdr->entries_offset + (u32)table_bytes) < hdr->entries_offset ||
        (u64)hdr->entries_offset + table_bytes < (u64)hdr->entries_offset) {
        return INITRAMFS_ERR_OVERFLOW;
    }
    if ((u64)hdr->entries_offset + table_bytes > (u64)hdr->total_size) {
        return INITRAMFS_ERR_BOUNDS;
    }
    if (hdr->entries_offset < hdr->header_size) {
        return INITRAMFS_ERR_CORRUPT;
    }

    if (hdr->entry_count > 0 && (hdr->entries_offset + table_bytes > hdr->data_offset)) {
        return INITRAMFS_ERR_CORRUPT;
    }

    const astra_initramfs_entry_t *entries = (const astra_initramfs_entry_t *)((const u8 *)archive_data + hdr->entries_offset);

    /* 6. Validate each individual entry */
    for (u32 i = 0; i < hdr->entry_count; i++) {
        const astra_initramfs_entry_t *e = &entries[i];

        /* Ensure null-termination within name buffer */
        bool null_terminated = false;
        size_t name_len = 0;
        for (size_t c = 0; c < sizeof(e->name); c++) {
            if (e->name[c] == '\0') {
                null_terminated = true;
                name_len = c;
                break;
            }
        }
        if (!null_terminated || name_len == 0) {
            return INITRAMFS_ERR_CORRUPT;
        }

        /* Enforce absolute path */
        if (e->name[0] != '/') {
            return INITRAMFS_ERR_TRAVERSAL;
        }

        /* Check for path traversal components: .. or empty components */
        for (size_t c = 0; c < name_len; c++) {
            /* Disallow double slashes // */
            if (e->name[c] == '/' && c + 1 < name_len && e->name[c + 1] == '/') {
                return INITRAMFS_ERR_TRAVERSAL;
            }
            /* Disallow /./ */
            if (e->name[c] == '/' && c + 1 < name_len && e->name[c + 1] == '.') {
                if (c + 2 == name_len || e->name[c + 2] == '/') {
                    return INITRAMFS_ERR_TRAVERSAL;
                }
                /* Disallow /../ */
                if (e->name[c + 2] == '.' && (c + 3 == name_len || e->name[c + 3] == '/')) {
                    return INITRAMFS_ERR_TRAVERSAL;
                }
            }
        }

        /* Disallow trailing slash for files or subdirectories (e.g. "/nested/", only "/" may end with /) */
        if (name_len > 1 && e->name[name_len - 1] == '/') {
            return INITRAMFS_ERR_TRAVERSAL;
        }

        /* Node type validation */
        if (e->type != VFS_NODE_FILE && e->type != VFS_NODE_DIR) {
            return INITRAMFS_ERR_CORRUPT;
        }

        /* Payload bounds and overflow validation */
        if (e->type == VFS_NODE_FILE) {
            if ((u32)(e->data_offset + e->size) < e->data_offset ||
                (u64)e->data_offset + (u64)e->size < (u64)e->data_offset) {
                return INITRAMFS_ERR_OVERFLOW;
            }
            if (e->data_offset < hdr->data_offset || (u64)e->data_offset + (u64)e->size > (u64)hdr->total_size) {
                return INITRAMFS_ERR_BOUNDS;
            }
        } else {
            /* Directories have 0 size and 0 offset */
            if (e->size != 0) {
                return INITRAMFS_ERR_CORRUPT;
            }
        }

        /* 7. Check for duplicate names against previous entries */
        for (u32 j = 0; j < i; j++) {
            if (kstrcmp(e->name, entries[j].name) == 0) {
                return INITRAMFS_ERR_DUPLICATE;
            }

            /* 8. Check for conflicting file/directory hierarchy */
            size_t j_len = kstrlen(entries[j].name);
            if (entries[j].type == VFS_NODE_FILE) {
                /* If j is a file, i cannot be a child under j */
                if (name_len > j_len && kstrncmp(e->name, entries[j].name, j_len) == 0 && e->name[j_len] == '/') {
                    return INITRAMFS_ERR_CONFLICT;
                }
            }
            if (e->type == VFS_NODE_FILE) {
                /* If i is a file, previous j cannot be a child under i */
                if (j_len > name_len && kstrncmp(entries[j].name, e->name, name_len) == 0 && entries[j].name[name_len] == '/') {
                    return INITRAMFS_ERR_CONFLICT;
                }
            }
        }
    }

    return INITRAMFS_OK;
}

/* VFS Operations on Initramfs Nodes */

static int initramfs_op_open(vfs_node_t *node, u32 flags) {
    if (!node) return VFS_ERR_INVAL;
    if (flags & (VFS_O_WRONLY | VFS_O_RDWR)) {
        return VFS_ERR_READ_ONLY;
    }
    return VFS_OK;
}

static int initramfs_op_close(vfs_node_t *node) {
    (void)node;
    return VFS_OK;
}

static ssize_t initramfs_op_read(vfs_node_t *node, void *buf, size_t count, size_t offset) {
    if (!node || !buf) return VFS_ERR_INVAL;
    if (node->type == VFS_NODE_DIR) return VFS_ERR_IS_DIR;

    uintptr_t entry_idx = (uintptr_t)node->fs_data;
    if (entry_idx >= g_initramfs.header->entry_count) {
        return VFS_ERR_CORRUPT;
    }

    const astra_initramfs_entry_t *entry = &g_initramfs.entries[entry_idx];
    if (offset >= entry->size) {
        return 0; /* EOF */
    }

    size_t available = entry->size - offset;
    size_t to_read = (count < available) ? count : available;

    const u8 *payload = (const u8 *)g_initramfs.archive_data + entry->data_offset + offset;
    kmemcpy(buf, payload, to_read);

    return (ssize_t)to_read;
}

static int initramfs_op_stat(vfs_node_t *node, vfs_stat_t *st) {
    if (!node || !st) return VFS_ERR_INVAL;
    kstrncpy(st->name, node->name, sizeof(st->name));
    st->type = node->type;
    st->size = node->size;
    return VFS_OK;
}

static int initramfs_op_readdir(vfs_node_t *node, size_t index, vfs_dirent_t *dirent) {
    if (!node || !dirent) return VFS_ERR_INVAL;
    if (node->type != VFS_NODE_DIR) return VFS_ERR_NOT_DIR;

    const char *dir_path = node->name;
    size_t dir_len = kstrlen(dir_path);
    bool is_root = (kstrcmp(dir_path, "/") == 0);

    size_t match_count = 0;
    char seen_subdirs[16][VFS_MAX_NAME_LEN];
    size_t seen_count = 0;

    for (u32 i = 0; i < g_initramfs.header->entry_count; i++) {
        const char *ename = g_initramfs.entries[i].name;

        const char *rel = NULL;
        if (is_root) {
            if (ename[0] == '/') rel = ename + 1;
        } else {
            if (kstrncmp(ename, dir_path, dir_len) == 0 && ename[dir_len] == '/') {
                rel = ename + dir_len + 1;
            }
        }

        if (!rel || *rel == '\0') continue;

        /* Extract the first path component of the relative subpath */
        char comp[VFS_MAX_NAME_LEN];
        size_t clen = 0;
        bool is_sub_dir = false;
        while (rel[clen] && rel[clen] != '/' && clen + 1 < sizeof(comp)) {
            comp[clen] = rel[clen];
            clen++;
        }
        comp[clen] = '\0';
        if (rel[clen] == '/') {
            is_sub_dir = true;
        }

        /* Deduplicate directory components */
        if (is_sub_dir || g_initramfs.entries[i].type == VFS_NODE_DIR) {
            bool already_seen = false;
            for (size_t s = 0; s < seen_count; s++) {
                if (kstrcmp(seen_subdirs[s], comp) == 0) {
                    already_seen = true;
                    break;
                }
            }
            if (already_seen) continue;
            if (seen_count < 16) {
                kstrncpy(seen_subdirs[seen_count++], comp, sizeof(seen_subdirs[0]));
            }
        }

        if (match_count == index) {
            kstrncpy(dirent->name, comp, sizeof(dirent->name));
            if (is_sub_dir || g_initramfs.entries[i].type == VFS_NODE_DIR) {
                dirent->type = VFS_NODE_DIR;
                dirent->size = 0;
            } else {
                dirent->type = VFS_NODE_FILE;
                dirent->size = g_initramfs.entries[i].size;
            }
            return VFS_OK;
        }
        match_count++;
    }

    return VFS_ERR_EOF;
}

static int initramfs_op_lookup(vfs_node_t *parent, const char *path, vfs_node_t **out_node) {
    if (!parent || !path || !out_node) return VFS_ERR_INVAL;
    *out_node = NULL;

    /* If looking up "/", return root */
    if (kstrcmp(path, "/") == 0) {
        *out_node = &g_initramfs.root_node;
        return VFS_OK;
    }

    /* Scan entries for exact path match */
    for (u32 i = 0; i < g_initramfs.header->entry_count; i++) {
        if (kstrcmp(g_initramfs.entries[i].name, path) == 0) {
            *out_node = &g_initramfs.node_pool[i];
            return VFS_OK;
        }
    }

    return VFS_ERR_NOT_FOUND;
}

static const vfs_ops_t g_initramfs_ops = {
    .open    = initramfs_op_open,
    .close   = initramfs_op_close,
    .read    = initramfs_op_read,
    .stat    = initramfs_op_stat,
    .readdir = initramfs_op_readdir,
    .lookup  = initramfs_op_lookup,
};

int initramfs_mount(const void *archive_data, size_t archive_size, vfs_node_t **out_root) {
    if (!out_root) return VFS_ERR_INVAL;
    *out_root = NULL;

    int val_err = initramfs_validate(archive_data, archive_size);
    if (val_err != INITRAMFS_OK) {
        return val_err;
    }

    const astra_initramfs_header_t *hdr = (const astra_initramfs_header_t *)archive_data;
    const astra_initramfs_entry_t *entries = (const astra_initramfs_entry_t *)((const u8 *)archive_data + hdr->entries_offset);

    g_initramfs.mounted = true;
    g_initramfs.archive_data = archive_data;
    g_initramfs.archive_size = archive_size;
    g_initramfs.header = hdr;
    g_initramfs.entries = entries;

    /* Initialize root directory node */
    kstrncpy(g_initramfs.root_node.name, "/", sizeof(g_initramfs.root_node.name));
    g_initramfs.root_node.type = VFS_NODE_DIR;
    g_initramfs.root_node.size = 0;
    g_initramfs.root_node.ops = &g_initramfs_ops;
    g_initramfs.root_node.fs_data = (void *)(uintptr_t)0;
    g_initramfs.root_node.parent = NULL;

    /* Initialize vfs_node_t for each entry */
    for (u32 i = 0; i < hdr->entry_count; i++) {
        vfs_node_t *n = &g_initramfs.node_pool[i];
        kstrncpy(n->name, entries[i].name, sizeof(n->name));
        n->type = (entries[i].type == VFS_NODE_DIR) ? VFS_NODE_DIR : VFS_NODE_FILE;
        n->size = entries[i].size;
        n->ops = &g_initramfs_ops;
        n->fs_data = (void *)(uintptr_t)i;
        n->parent = &g_initramfs.root_node;
    }

    *out_root = &g_initramfs.root_node;
    return VFS_OK;
}
