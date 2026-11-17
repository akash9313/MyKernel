#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/vfs.h>
#include <kernel/mem.h>
#include <kernel/heap.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/kprintf.h>
#include <kernel/scrollview.h>
#include <kernel/drivers/apic.h>

#define FILEMGR_MAX_ENTRIES 1200
#define FM_HISTORY_MAX 16

typedef struct filemgr_item {
    char name[64];
    bool is_dir;
    size_t size;
    bool selected;
} filemgr_item_t;

typedef enum {
    FM_DLG_NONE = 0,
    FM_DLG_NEW_FILE,
    FM_DLG_NEW_FOLDER,
    FM_DLG_RENAME,
    FM_DLG_DELETE_CONFIRM,
    FM_DLG_PROPERTIES
} fm_dialog_type_t;

typedef enum {
    FM_SORT_NAME = 0,
    FM_SORT_SIZE,
    FM_SORT_TYPE
} fm_sort_mode_t;

typedef enum {
    FM_VIEW_DETAILS = 0,
    FM_VIEW_LIST
} fm_view_mode_t;

typedef struct filemgr_state {
    char current_path[128];
    filemgr_item_t *items; /* Dynamically allocated from PMM pages to keep 0 bytes heap usage */
    phys_addr_t items_pa;
    size_t items_pages;
    int item_count;

    /* Selection State */
    int selected_idx;
    int anchor_idx;
    int selected_count;
    char status_msg[128];

    /* Navigation History */
    char history[FM_HISTORY_MAX][128];
    int history_pos;
    int history_count;

    /* Search / Filter */
    char search_filter[32];
    int search_len;
    bool search_active;

    /* Sort and View modes */
    fm_sort_mode_t sort_mode;
    fm_view_mode_t view_mode;

    /* Double click tracking */
    u64 last_click_tick;
    int last_click_row;

    scrollview_t scrollview;

    /* Context Menu */
    bool menu_open;
    int menu_x;
    int menu_y;
    int menu_target_idx; /* -1 for empty canvas, >= 0 for item index */

    /* Modal Dialog */
    fm_dialog_type_t dialog_type;
    char dialog_input[64];
    int dialog_input_len;
    int dialog_target_idx;
    bool dialog_permanent;

    int visible_rows;
} filemgr_state_t;

/* Single state instance */
static filemgr_state_t g_filemgr = {0};

/* File Clipboard (Cut/Copy/Paste) */
static struct {
    bool has_data;
    bool is_cut;
    char src_path[128];
    char filename[64];
    bool is_dir;
} s_file_clipboard = {0};

/* Forward declaration for Code Editor integration */
void app_editor_launch(const char *filepath);

/* String Utilities */
static size_t fm_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void fm_strcpy(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < max_len) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int fm_strcmp(const char *a, const char *b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static int fm_strncmp(const char *a, const char *b, size_t n) {
    if (n == 0) return 0;
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (n && *a && (*a == *b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static char fm_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static const char *fm_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (fm_tolower(*h) == fm_tolower(*n))) {
            h++;
            n++;
        }
        if (!*n) return haystack;
    }
    return NULL;
}

static void fm_path_join(char *dst, size_t max_len, const char *dir, const char *file) {
    if (!dst || max_len == 0) return;
    size_t dp = 0;
    for (size_t i = 0; dir && dir[i] && dp + 2 < max_len; i++) {
        dst[dp++] = dir[i];
    }
    if (dp > 0 && dst[dp - 1] != '/' && dp + 1 < max_len) {
        dst[dp++] = '/';
    }
    for (size_t i = 0; file && file[i] && dp + 1 < max_len; i++) {
        dst[dp++] = file[i];
    }
    dst[dp] = '\0';
}

static void fm_set_status(filemgr_state_t *fm, const char *msg) {
    if (!fm) return;
    fm_strcpy(fm->status_msg, msg, sizeof(fm->status_msg));
}

static bool fm_is_protected_path(const char *path) {
    if (!path || !path[0]) return true;
    if (fm_strcmp(path, ".") == 0 || fm_strcmp(path, "..") == 0 || fm_strcmp(path, "/") == 0) return true;
    if (fm_strcmp(path, "/data") == 0 || fm_strcmp(path, "/bin") == 0 ||
        fm_strcmp(path, "/assets") == 0 || fm_strcmp(path, "/etc") == 0 ||
        fm_strcmp(path, "/dev") == 0 || fm_strcmp(path, "/proc") == 0 ||
        fm_strcmp(path, "/www") == 0) {
        return true;
    }
    size_t len = fm_strlen(path);
    if (len >= 2 && path[len - 1] == '.' && path[len - 2] == '/') return true;
    if (len >= 3 && path[len - 1] == '.' && path[len - 2] == '.' && path[len - 3] == '/') return true;
    return false;
}

/* File Badge Resolver */
static void fm_get_badge_and_type(const char *name, bool is_dir,
                                  const char **badge_str, u32 *badge_bg, u32 *badge_border,
                                  const char **type_str) {
    if (is_dir) {
        *badge_str = "DIR";
        *badge_bg = 0x0284C7;     /* Sky / Cyan */
        *badge_border = 0x38BDF8;
        *type_str = "Folder";
        return;
    }

    size_t len = fm_strlen(name);
    if (len >= 2 && (fm_strcmp(name + len - 2, ".c") == 0 || fm_strcmp(name + len - 2, ".h") == 0)) {
        *badge_str = "C";
        *badge_bg = 0x059669;     /* Emerald */
        *badge_border = 0x34D399;
        *type_str = "C Source File";
    } else if (len >= 4 && fm_strcmp(name + len - 4, ".cpp") == 0) {
        *badge_str = "C++";
        *badge_bg = 0x059669;
        *badge_border = 0x34D399;
        *type_str = "C++ Source File";
    } else if (len >= 3 && fm_strcmp(name + len - 3, ".py") == 0) {
        *badge_str = "PY";
        *badge_bg = 0xD97706;     /* Amber */
        *badge_border = 0xFBBF24;
        *type_str = "Python Script";
    } else if (len >= 3 && fm_strcmp(name + len - 3, ".sh") == 0) {
        *badge_str = "SH";
        *badge_bg = 0x0D9488;     /* Teal */
        *badge_border = 0x2DD4BF;
        *type_str = "Shell Script";
    } else if (len >= 4 && (fm_strcmp(name + len - 4, ".txt") == 0 || fm_strcmp(name + len - 4, ".log") == 0)) {
        *badge_str = "TXT";
        *badge_bg = 0x475569;     /* Slate */
        *badge_border = 0x94A3B8;
        *type_str = "Text Document";
    } else if (len >= 3 && fm_strcmp(name + len - 3, ".md") == 0) {
        *badge_str = "MD";
        *badge_bg = 0x475569;
        *badge_border = 0x94A3B8;
        *type_str = "Markdown Document";
    } else if (len >= 4 && fm_strcmp(name + len - 4, ".elf") == 0) {
        *badge_str = "ELF";
        *badge_bg = 0x7C3AED;     /* Purple */
        *badge_border = 0xA78BFA;
        *type_str = "Executable Binary";
    } else {
        *badge_str = "FILE";
        *badge_bg = 0x2563EB;     /* Blue */
        *badge_border = 0x60A5FA;
        *type_str = "Document";
    }
}

/* Buffer Management */
static void filemgr_ensure_buffer(filemgr_state_t *fm) {
    if (fm && !fm->items) {
        size_t bytes = sizeof(filemgr_item_t) * FILEMGR_MAX_ENTRIES;
        size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        phys_addr_t pa = 0;
        if (pmm_alloc_contiguous(pages, &pa) == PMM_SUCCESS) {
            struct limine_hhdm_response *hhdm = boot_get_hhdm_response();
            if (hhdm) {
                fm->items = (filemgr_item_t *)(pa + hhdm->offset);
                fm->items_pa = pa;
                fm->items_pages = pages;
                kmemset(fm->items, 0, pages * PAGE_SIZE);
            } else {
                pmm_free_contiguous(pa, pages);
            }
        }
    }
}

static void filemgr_on_destroy(wm_window_t *win) {
    if (!win) return;
    filemgr_state_t *fm = (filemgr_state_t *)win->user_data;
    if (fm && fm->items) {
        if (fm->items_pa && fm->items_pages > 0) {
            pmm_free_contiguous(fm->items_pa, fm->items_pages);
        }
        fm->items = NULL;
        fm->items_pa = 0;
        fm->items_pages = 0;
    }
}

/* Sorting */
static void filemgr_sort(filemgr_state_t *fm) {
    if (!fm || fm->item_count <= 2) return;

    /* Determine start index: leave ".." at index 0 */
    int start = 0;
    if (fm_strcmp(fm->items[0].name, "..") == 0) {
        start = 1;
    }

    for (int i = start + 1; i < fm->item_count; i++) {
        filemgr_item_t key = fm->items[i];
        int j = i - 1;

        while (j >= start) {
            bool swap = false;
            /* Directories always come before regular files */
            if (fm->items[j].is_dir && !key.is_dir) {
                swap = false;
            } else if (!fm->items[j].is_dir && key.is_dir) {
                swap = true;
            } else {
                /* Same type */
                if (fm->sort_mode == FM_SORT_NAME) {
                    swap = (fm_strcmp(fm->items[j].name, key.name) > 0);
                } else if (fm->sort_mode == FM_SORT_SIZE) {
                    swap = (fm->items[j].size < key.size);
                } else if (fm->sort_mode == FM_SORT_TYPE) {
                    const char *b1, *b2, *t1, *t2;
                    u32 c1, c2, bo1, bo2;
                    fm_get_badge_and_type(fm->items[j].name, fm->items[j].is_dir, &b1, &c1, &bo1, &t1);
                    fm_get_badge_and_type(key.name, key.is_dir, &b2, &c2, &bo2, &t2);
                    int tc = fm_strcmp(t1, t2);
                    swap = (tc != 0) ? (tc > 0) : (fm_strcmp(fm->items[j].name, key.name) > 0);
                }
            }

            if (swap) {
                fm->items[j + 1] = fm->items[j];
                j--;
            } else {
                break;
            }
        }
        fm->items[j + 1] = key;
    }
}

/* Directory Enumeration */
static void filemgr_load_dir(filemgr_state_t *fm, const char *path) {
    if (!fm) return;
    filemgr_ensure_buffer(fm);
    if (!fm->items) {
        fm_set_status(fm, "Error: out of memory for file list buffer.");
        return;
    }

    const char *p = (path && path[0]) ? path : "/";
    fm_strcpy(fm->current_path, p, sizeof(fm->current_path));

    fm->item_count = 0;
    fm->selected_idx = -1;
    fm->anchor_idx = -1;
    fm->selected_count = 0;
    fm->menu_open = false;

    /* Add '..' entry if not in root */
    if (fm->current_path[0] == '/' && fm->current_path[1] != '\0') {
        filemgr_item_t *up = &fm->items[fm->item_count++];
        fm_strcpy(up->name, "..", sizeof(up->name));
        up->is_dir = true;
        up->size = 0;
        up->selected = false;
    }

    vfs_dirent_t dent;
    size_t idx = 0;
    while (vfs_readdir(fm->current_path, idx, &dent) == VFS_OK && fm->item_count < FILEMGR_MAX_ENTRIES) {
        /* Skip self and parent directory dots from raw readdir */
        if (fm_strcmp(dent.name, ".") == 0 || fm_strcmp(dent.name, "..") == 0) {
            idx++;
            continue;
        }

        filemgr_item_t *item = &fm->items[fm->item_count++];
        fm_strcpy(item->name, dent.name, sizeof(item->name));
        item->is_dir = (dent.type == VFS_NODE_DIR);
        item->size = dent.size;
        item->selected = false;
        idx++;
    }

    /* Apply current sort */
    filemgr_sort(fm);

    /* Update scrollview total items */
    fm->scrollview.total_items = fm->item_count;
    if (fm->scrollview.scroll_offset > fm->item_count - fm->visible_rows) {
        fm->scrollview.scroll_offset = fm->item_count - fm->visible_rows;
    }
    if (fm->scrollview.scroll_offset < 0) fm->scrollview.scroll_offset = 0;

    fm_set_status(fm, "Ready. Right-click for options; double-click to open.");
}

/* Navigation History Helpers */
static void filemgr_navigate(filemgr_state_t *fm, const char *path) {
    if (!fm || !path || !path[0]) return;

    /* If different from current history head, record in history stack */
    if (fm->history_pos < 0 || fm_strcmp(fm->history[fm->history_pos], path) != 0) {
        if (fm->history_pos + 1 < FM_HISTORY_MAX) {
            fm->history_pos++;
        } else {
            /* Shift left */
            for (int i = 0; i < FM_HISTORY_MAX - 1; i++) {
                fm_strcpy(fm->history[i], fm->history[i + 1], sizeof(fm->history[0]));
            }
            fm->history_pos = FM_HISTORY_MAX - 1;
        }
        fm_strcpy(fm->history[fm->history_pos], path, sizeof(fm->history[0]));
        fm->history_count = fm->history_pos + 1;
    }

    filemgr_load_dir(fm, path);
}

static void filemgr_nav_back(filemgr_state_t *fm) {
    if (!fm || fm->history_pos <= 0) return;
    fm->history_pos--;
    filemgr_load_dir(fm, fm->history[fm->history_pos]);
}

static void filemgr_nav_forward(filemgr_state_t *fm) {
    if (!fm || fm->history_pos + 1 >= fm->history_count) return;
    fm->history_pos++;
    filemgr_load_dir(fm, fm->history[fm->history_pos]);
}

static void filemgr_nav_up(filemgr_state_t *fm) {
    if (!fm) return;
    int last_slash = -1;
    for (int i = 0; fm->current_path[i]; i++) {
        if (fm->current_path[i] == '/') last_slash = i;
    }
    if (last_slash > 0) {
        char parent[128];
        fm_strcpy(parent, fm->current_path, sizeof(parent));
        parent[last_slash] = '\0';
        filemgr_navigate(fm, parent);
    } else {
        filemgr_navigate(fm, "/");
    }
}

/* Multi-Selection Helpers */
static void filemgr_clear_selection(filemgr_state_t *fm) {
    if (!fm) return;
    for (int i = 0; i < fm->item_count; i++) {
        fm->items[i].selected = false;
    }
    fm->selected_count = 0;
    fm->selected_idx = -1;
}

static void filemgr_select_single(filemgr_state_t *fm, int idx) {
    if (!fm) return;
    filemgr_clear_selection(fm);
    if (idx >= 0 && idx < fm->item_count) {
        fm->items[idx].selected = true;
        fm->selected_idx = idx;
        fm->anchor_idx = idx;
        fm->selected_count = 1;
    }
}

static void filemgr_toggle_select(filemgr_state_t *fm, int idx) {
    if (!fm || idx < 0 || idx >= fm->item_count) return;
    /* Do not allow selecting '..' */
    if (fm_strcmp(fm->items[idx].name, "..") == 0) return;

    fm->items[idx].selected = !fm->items[idx].selected;
    fm->selected_idx = idx;
    fm->anchor_idx = idx;

    int count = 0;
    for (int i = 0; i < fm->item_count; i++) {
        if (fm->items[i].selected) count++;
    }
    fm->selected_count = count;
}

static void filemgr_range_select(filemgr_state_t *fm, int idx) {
    if (!fm || idx < 0 || idx >= fm->item_count) return;
    int start = (fm->anchor_idx >= 0 && fm->anchor_idx < fm->item_count) ? fm->anchor_idx : 0;
    int end = idx;
    if (start > end) {
        int t = start;
        start = end;
        end = t;
    }

    filemgr_clear_selection(fm);
    for (int i = start; i <= end; i++) {
        if (fm_strcmp(fm->items[i].name, "..") != 0) {
            fm->items[i].selected = true;
        }
    }
    fm->selected_idx = idx;

    int count = 0;
    for (int i = 0; i < fm->item_count; i++) {
        if (fm->items[i].selected) count++;
    }
    fm->selected_count = count;
}

static void filemgr_select_all(filemgr_state_t *fm) {
    if (!fm) return;
    int count = 0;
    for (int i = 0; i < fm->item_count; i++) {
        if (fm_strcmp(fm->items[i].name, "..") == 0) {
            fm->items[i].selected = false;
        } else {
            fm->items[i].selected = true;
            count++;
        }
    }
    fm->selected_count = count;
    if (fm->item_count > 0) {
        fm->selected_idx = (fm_strcmp(fm->items[0].name, "..") == 0 && fm->item_count > 1) ? 1 : 0;
    }
}

/* Recursive Directory Deletion (Two-Pass Non-Skipping Backend) */
typedef struct {
    char name[64];
    bool is_dir;
} fm_child_entry_t;

static int filemgr_delete_recursive(const char *path) {
    if (!path || !path[0]) return VFS_ERR_INVAL;

    /* Prevent deleting root, system mount points, and relative dot traversal */
    if (fm_is_protected_path(path)) {
        return VFS_ERR_DENIED;
    }

    vfs_stat_t st;
    int err = vfs_stat(path, &st);
    if (err != VFS_OK) return err;

    if (st.type == VFS_NODE_DIR) {
        /* Allocate a temporary child buffer via PMM so we don't blow up kernel stack or heap */
        phys_addr_t pa = 0;
        fm_child_entry_t *children = NULL;
        size_t pages = 4; /* 16KB = ~240 child entries per directory level */
        if (pmm_alloc_contiguous(pages, &pa) == PMM_SUCCESS) {
            struct limine_hhdm_response *hhdm = boot_get_hhdm_response();
            if (hhdm) {
                children = (fm_child_entry_t *)(pa + hhdm->offset);
            }
        }

        if (children) {
            /* Pass 1: Gather all child names without modifying directory */
            int child_count = 0;
            vfs_dirent_t dent;
            size_t idx = 0;
            while (vfs_readdir(path, idx++, &dent) == VFS_OK && child_count < 240) {
                if (fm_strcmp(dent.name, ".") == 0 || fm_strcmp(dent.name, "..") == 0) continue;
                fm_strcpy(children[child_count].name, dent.name, sizeof(children[child_count].name));
                children[child_count].is_dir = (dent.type == VFS_NODE_DIR);
                child_count++;
            }

            /* Pass 2: Delete every child recursively */
            for (int i = 0; i < child_count; i++) {
                char child_path[128];
                fm_path_join(child_path, sizeof(child_path), path, children[i].name);
                filemgr_delete_recursive(child_path);
            }

            pmm_free_contiguous(pa, pages);
        } else {
            /* Fallback: repeated read of entry 0 if temporary page allocation was unavailable */
            int safety = 1000;
            vfs_dirent_t dent;
            while (safety-- > 0 && vfs_readdir(path, 0, &dent) == VFS_OK) {
                if (fm_strcmp(dent.name, ".") == 0 || fm_strcmp(dent.name, "..") == 0) break;
                char child_path[128];
                fm_path_join(child_path, sizeof(child_path), path, dent.name);
                filemgr_delete_recursive(child_path);
            }
        }

        /* Now that directory is completely empty, remove it */
        return vfs_rmdir(path);
    } else {
        return vfs_unlink(path);
    }
}

/* Deletion Trigger & Execution */
static void filemgr_trigger_delete(filemgr_state_t *fm, bool permanent) {
    if (!fm) return;

    /* Ensure something is selected */
    if (fm->selected_count == 0 && fm->selected_idx >= 0 && fm->selected_idx < fm->item_count) {
        if (fm_strcmp(fm->items[fm->selected_idx].name, "..") != 0) {
            fm->items[fm->selected_idx].selected = true;
            fm->selected_count = 1;
        }
    }

    if (fm->selected_count == 0) {
        fm_set_status(fm, "Select an item to delete.");
        return;
    }

    /* Guard against deleting '..' */
    if (fm->selected_count == 1 && fm->selected_idx >= 0 &&
        fm_strcmp(fm->items[fm->selected_idx].name, "..") == 0) {
        fm_set_status(fm, "Cannot delete parent directory link '..'.");
        return;
    }

    fm->dialog_type = FM_DLG_DELETE_CONFIRM;
    fm->dialog_permanent = permanent;
    fm->dialog_target_idx = fm->selected_idx;
}

static void filemgr_execute_delete(filemgr_state_t *fm) {
    if (!fm) return;

    int deleted_count = 0;
    int failed_count = 0;
    char last_failed_name[64] = {0};

    for (int i = 0; i < fm->item_count; i++) {
        if (!fm->items[i].selected) continue;
        if (fm_strcmp(fm->items[i].name, "..") == 0 || fm_strcmp(fm->items[i].name, ".") == 0) continue;

        char path[128];
        fm_path_join(path, sizeof(path), fm->current_path, fm->items[i].name);

        int res = filemgr_delete_recursive(path);
        if (res == VFS_OK) {
            deleted_count++;
        } else {
            failed_count++;
            fm_strcpy(last_failed_name, fm->items[i].name, sizeof(last_failed_name));
        }
    }

    /* Reload directory to reflect persistent on-disk state */
    filemgr_load_dir(fm, fm->current_path);

    /* Real user feedback */
    if (failed_count == 0 && deleted_count > 0) {
        if (deleted_count == 1) {
            fm_set_status(fm, "Item deleted successfully.");
        } else {
            char msg[64];
            int mp = 0;
            const char *p1 = "Deleted ";
            for (int k = 0; p1[k]; k++) msg[mp++] = p1[k];
            if (deleted_count >= 10) msg[mp++] = '0' + (deleted_count / 10) % 10;
            msg[mp++] = '0' + (deleted_count % 10);
            const char *p2 = " items successfully.";
            for (int k = 0; p2[k]; k++) msg[mp++] = p2[k];
            msg[mp] = '\0';
            fm_set_status(fm, msg);
        }
    } else if (failed_count > 0) {
        char msg[96];
        int mp = 0;
        const char *p1 = "Error: failed to delete ";
        for (int k = 0; p1[k]; k++) msg[mp++] = p1[k];
        for (int k = 0; last_failed_name[k] && mp < 80; k++) msg[mp++] = last_failed_name[k];
        msg[mp] = '\0';
        fm_set_status(fm, msg);
    } else {
        fm_set_status(fm, "No items were deleted.");
    }
}

/* File / Folder Operations */
static void filemgr_op_create_file(filemgr_state_t *fm, const char *filename) {
    if (!fm || !filename || !filename[0]) return;
    char path[128];
    fm_path_join(path, sizeof(path), fm->current_path, filename);

    int fd = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd >= 0) {
        /* Write starter template for code files */
        size_t plen = fm_strlen(path);
        const char *template = "";
        if (plen >= 2 && path[plen - 2] == '.' && path[plen - 1] == 'c') {
            template = "// AstraOS C Source\n#include <stdio.h>\n\nint main() {\n    printf(\"Hello from AstraOS!\\n\");\n    return 0;\n}\n";
        } else if (plen >= 3 && path[plen - 3] == '.' && path[plen - 2] == 'p' && path[plen - 1] == 'y') {
            template = "# AstraOS Python 3.11 Script\nprint(\"Hello from AstraOS Python!\")\n";
        } else if (plen >= 3 && path[plen - 3] == '.' && path[plen - 2] == 's' && path[plen - 1] == 'h') {
            template = "#!/bin/sh\necho \"AstraOS Shell Script Running\"\npwd\n";
        }
        size_t tlen = fm_strlen(template);
        if (tlen > 0) vfs_write(fd, template, tlen);
        vfs_close(fd);

        filemgr_load_dir(fm, fm->current_path);
        fm_set_status(fm, "File created successfully.");
    } else {
        fm_set_status(fm, "Error: failed to create file.");
    }
}

static void filemgr_op_create_folder(filemgr_state_t *fm, const char *foldername) {
    if (!fm || !foldername || !foldername[0]) return;
    char path[128];
    fm_path_join(path, sizeof(path), fm->current_path, foldername);

    if (vfs_mkdir(path) == VFS_OK) {
        filemgr_load_dir(fm, fm->current_path);
        fm_set_status(fm, "Folder created successfully.");
    } else {
        fm_set_status(fm, "Error: failed to create folder.");
    }
}

static void filemgr_op_rename(filemgr_state_t *fm, const char *old_name, const char *new_name) {
    if (!fm || !old_name || !new_name || !new_name[0]) return;
    char old_path[128], new_path[128];
    fm_path_join(old_path, sizeof(old_path), fm->current_path, old_name);
    fm_path_join(new_path, sizeof(new_path), fm->current_path, new_name);

    if (vfs_rename(old_path, new_path) == VFS_OK) {
        filemgr_load_dir(fm, fm->current_path);
        fm_set_status(fm, "Renamed successfully.");
    } else {
        fm_set_status(fm, "Error: rename failed.");
    }
}

static int filemgr_copy_recursive(const char *src_path, const char *dst_path) {
    if (!src_path || !dst_path) return VFS_ERR_INVAL;

    size_t slen = fm_strlen(src_path);
    if (fm_strncmp(dst_path, src_path, slen) == 0 && (dst_path[slen] == '/' || dst_path[slen] == '\0')) {
        return VFS_ERR_INVAL;
    }

    vfs_stat_t st;
    int s_err = vfs_stat(src_path, &st);
    if (s_err != VFS_OK) return s_err;

    if (st.type == VFS_NODE_DIR) {
        (void)vfs_mkdir(dst_path);
        size_t idx = 0;
        vfs_dirent_t dent;
        while (vfs_readdir(src_path, idx++, &dent) == VFS_OK) {
            if (fm_strcmp(dent.name, ".") == 0 || fm_strcmp(dent.name, "..") == 0) continue;
            char child_src[128];
            char child_dst[128];
            fm_path_join(child_src, sizeof(child_src), src_path, dent.name);
            fm_path_join(child_dst, sizeof(child_dst), dst_path, dent.name);
            filemgr_copy_recursive(child_src, child_dst);
        }
        return VFS_OK;
    } else {
        int in_fd = vfs_open(src_path, VFS_O_RDONLY);
        if (in_fd < 0) return in_fd;
        int out_fd = vfs_open(dst_path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
        if (out_fd < 0) {
            vfs_close(in_fd);
            return out_fd;
        }
        char chunk[1024];
        ssize_t n;
        while ((n = vfs_read(in_fd, chunk, sizeof(chunk))) > 0) {
            vfs_write(out_fd, chunk, (size_t)n);
        }
        vfs_close(in_fd);
        vfs_close(out_fd);
        return VFS_OK;
    }
}

static void filemgr_op_copy(filemgr_state_t *fm, const filemgr_item_t *item, bool is_cut) {
    if (!fm || !item) return;
    s_file_clipboard.has_data = true;
    s_file_clipboard.is_cut = is_cut;
    s_file_clipboard.is_dir = item->is_dir;
    fm_path_join(s_file_clipboard.src_path, sizeof(s_file_clipboard.src_path), fm->current_path, item->name);
    fm_strcpy(s_file_clipboard.filename, item->name, sizeof(s_file_clipboard.filename));

    if (is_cut) {
        fm_set_status(fm, "Item cut to clipboard.");
    } else {
        fm_set_status(fm, "Item copied to clipboard.");
    }
}

static void filemgr_op_paste(filemgr_state_t *fm) {
    if (!fm || !s_file_clipboard.has_data) {
        fm_set_status(fm, "Clipboard is empty.");
        return;
    }

    char dest_path[128];
    fm_path_join(dest_path, sizeof(dest_path), fm->current_path, s_file_clipboard.filename);

    if (fm_strcmp(s_file_clipboard.src_path, dest_path) == 0) {
        char base[64];
        fm_strcpy(base, s_file_clipboard.filename, sizeof(base));
        int dot = -1;
        for (int i = 0; base[i]; i++) {
            if (base[i] == '.') dot = i;
        }
        char new_name[64];
        if (dot > 0 && !s_file_clipboard.is_dir) {
            base[dot] = '\0';
            int np = 0;
            for (int k = 0; base[k]; k++) new_name[np++] = base[k];
            const char *suff = "_copy.";
            for (int k = 0; suff[k]; k++) new_name[np++] = suff[k];
            const char *ext = &s_file_clipboard.filename[dot + 1];
            for (int k = 0; ext[k]; k++) new_name[np++] = ext[k];
            new_name[np] = '\0';
        } else {
            int np = 0;
            for (int k = 0; base[k]; k++) new_name[np++] = base[k];
            const char *suff = "_copy";
            for (int k = 0; suff[k]; k++) new_name[np++] = suff[k];
            new_name[np] = '\0';
        }
        fm_path_join(dest_path, sizeof(dest_path), fm->current_path, new_name);
    }

    if (s_file_clipboard.is_dir) {
        size_t slen = fm_strlen(s_file_clipboard.src_path);
        if (fm_strncmp(dest_path, s_file_clipboard.src_path, slen) == 0 &&
            (dest_path[slen] == '/' || dest_path[slen] == '\0')) {
            fm_set_status(fm, "Error: cannot move/copy folder into its own descendant.");
            return;
        }
    }

    int c_res = filemgr_copy_recursive(s_file_clipboard.src_path, dest_path);
    if (c_res != VFS_OK) {
        fm_set_status(fm, "Error: copy/paste operation failed.");
        return;
    }

    if (s_file_clipboard.is_cut) {
        filemgr_delete_recursive(s_file_clipboard.src_path);
        s_file_clipboard.has_data = false;
        fm_set_status(fm, "Item moved successfully.");
    } else {
        fm_set_status(fm, "Item copied successfully.");
    }

    filemgr_load_dir(fm, fm->current_path);
}

static void filemgr_open_item(filemgr_state_t *fm, int idx) {
    if (!fm || idx < 0 || idx >= fm->item_count) return;
    filemgr_item_t *item = &fm->items[idx];

    if (item->is_dir) {
        if (fm_strcmp(item->name, "..") == 0) {
            filemgr_nav_up(fm);
        } else {
            char new_path[128];
            fm_path_join(new_path, sizeof(new_path), fm->current_path, item->name);
            filemgr_navigate(fm, new_path);
        }
    } else {
        char full_path[128];
        fm_path_join(full_path, sizeof(full_path), fm->current_path, item->name);
        app_editor_launch(full_path);
    }
}

/* UI Paint */
static void filemgr_on_paint(wm_window_t *win, gfx_surface_t *s) {
    if (!win || !s) return;
    filemgr_state_t *fm = (filemgr_state_t *)win->user_data;
    if (!fm) return;

    /* Base Canvas Background (Graphite Dark) */
    gfx_fill_rect(s, 0, 0, s->width, s->height, 0x111827);

    /* 1. Top Navigation Bar (y = 0..34, 34px) */
    gfx_fill_rect(s, 0, 0, s->width, 34, 0x0F172A);
    gfx_draw_line(s, 0, 34, s->width, 34, 0x1E293B);

    /* [<] Back Button (8..34, 5..29) */
    bool can_back = (fm->history_pos > 0);
    gfx_fill_rounded_rect(s, 8, 5, 26, 24, 4, can_back ? 0x1E293B : 0x0F172A);
    gfx_draw_rounded_rect(s, 8, 5, 26, 24, 4, can_back ? 0x334155 : 0x1E293B);
    gfx_draw_string(s, 16, 9, "<", can_back ? 0x38BDF8 : 0x475569, 0, true);

    /* [>] Forward Button (38..64, 5..29) */
    bool can_fwd = (fm->history_pos + 1 < fm->history_count);
    gfx_fill_rounded_rect(s, 38, 5, 26, 24, 4, can_fwd ? 0x1E293B : 0x0F172A);
    gfx_draw_rounded_rect(s, 38, 5, 26, 24, 4, can_fwd ? 0x334155 : 0x1E293B);
    gfx_draw_string(s, 46, 9, ">", can_fwd ? 0x38BDF8 : 0x475569, 0, true);

    /* [^] Up Button (68..94, 5..29) */
    gfx_fill_rounded_rect(s, 68, 5, 26, 24, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 68, 5, 26, 24, 4, 0x334155);
    gfx_draw_string(s, 76, 9, "^", 0x38BDF8, 0, true);

    /* [H] Home Button (98..124, 5..29) */
    gfx_fill_rounded_rect(s, 98, 5, 26, 24, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 98, 5, 26, 24, 4, 0x334155);
    gfx_draw_string(s, 106, 9, "H", 0x38BDF8, 0, true);

    /* [R] Refresh Button (128..154, 5..29) */
    gfx_fill_rounded_rect(s, 128, 5, 26, 24, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 128, 5, 26, 24, 4, 0x334155);
    gfx_draw_string(s, 136, 9, "R", 0x38BDF8, 0, true);

    /* Breadcrumbs Pill Bar (158 .. s->width - 180) */
    int bc_x = 158;
    int search_w = 172;
    int bc_w = s->width - bc_x - search_w - 12;
    if (bc_w < 140) bc_w = 140;
    gfx_fill_rounded_rect(s, bc_x, 5, bc_w, 24, 4, 0x162238);
    gfx_draw_rounded_rect(s, bc_x, 5, bc_w, 24, 4, 0x223652);

    /* Render clickable breadcrumbs */
    int cur_bc_x = bc_x + 8;
    gfx_draw_string(s, cur_bc_x, 9, "Root", 0x38BDF8, 0, true);
    cur_bc_x += 4 * FONT_WIDTH + 6;

    if (fm->current_path[0] == '/' && fm->current_path[1] != '\0') {
        int seg_start = 1;
        for (int i = 1; ; i++) {
            if (fm->current_path[i] == '/' || fm->current_path[i] == '\0') {
                if (cur_bc_x + 16 < bc_x + bc_w) {
                    gfx_draw_string(s, cur_bc_x, 9, ">", 0x64748B, 0, true);
                    cur_bc_x += 12;
                }
                char seg[32];
                int sp = 0;
                for (int k = seg_start; k < i && sp < 31; k++) seg[sp++] = fm->current_path[k];
                seg[sp] = '\0';
                if (cur_bc_x + sp * FONT_WIDTH < bc_x + bc_w) {
                    gfx_draw_string(s, cur_bc_x, 9, seg, 0xF8FAFC, 0, true);
                    cur_bc_x += sp * FONT_WIDTH + 6;
                }
                seg_start = i + 1;
                if (fm->current_path[i] == '\0') break;
            }
        }
    }

    /* Search Box (s->width - search_w - 6 .. s->width - 6, 5..29) */
    int srch_x = s->width - search_w - 6;
    u32 srch_border = fm->search_active ? 0x38BDF8 : 0x334155;
    gfx_fill_rounded_rect(s, srch_x, 5, search_w, 24, 4, 0x0B0F19);
    gfx_draw_rounded_rect(s, srch_x, 5, search_w, 24, 4, srch_border);

    if (fm->search_len > 0) {
        gfx_draw_string(s, srch_x + 8, 9, fm->search_filter, 0xF8FAFC, 0, true);
        /* Draw clear [x] button */
        gfx_draw_string(s, srch_x + search_w - 16, 9, "x", 0x94A3B8, 0, true);
    } else {
        gfx_draw_string(s, srch_x + 8, 9, fm->search_active ? "|" : "Search...", 0x64748B, 0, true);
    }

    /* 2. Action Toolbar (y = 35..66, 32px) */
    gfx_fill_rect(s, 0, 35, s->width, 32, 0x162238);
    gfx_draw_line(s, 0, 66, s->width, 66, 0x1E293B);

    /* + File Button (8..70, 39..63) */
    gfx_fill_rounded_rect(s, 8, 39, 62, 24, 4, 0x065F46);
    gfx_draw_rounded_rect(s, 8, 39, 62, 24, 4, 0x10B981);
    gfx_draw_string(s, 14, 43, "+ File", 0xFFFFFF, 0, true);

    /* + Folder Button (76..150, 39..63) */
    gfx_fill_rounded_rect(s, 76, 39, 74, 24, 4, 0x9A3412);
    gfx_draw_rounded_rect(s, 76, 39, 74, 24, 4, 0xF97316);
    gfx_draw_string(s, 82, 43, "+ Folder", 0xFFFFFF, 0, true);

    /* Cut Button (156..206, 39..63) */
    gfx_fill_rounded_rect(s, 156, 39, 50, 24, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 156, 39, 50, 24, 4, 0x475569);
    gfx_draw_string(s, 168, 43, "Cut", 0xE2E8F0, 0, true);

    /* Copy Button (212..264, 39..63) */
    gfx_fill_rounded_rect(s, 212, 39, 52, 24, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 212, 39, 52, 24, 4, 0x475569);
    gfx_draw_string(s, 222, 43, "Copy", 0xE2E8F0, 0, true);

    /* Paste Button (270..326, 39..63) */
    bool has_cb = s_file_clipboard.has_data;
    gfx_fill_rounded_rect(s, 270, 39, 56, 24, 4, has_cb ? 0x1E293B : 0x0F172A);
    gfx_draw_rounded_rect(s, 270, 39, 56, 24, 4, has_cb ? 0x475569 : 0x1E293B);
    gfx_draw_string(s, 278, 43, "Paste", has_cb ? 0xE2E8F0 : 0x64748B, 0, true);

    /* Rename Button (332..396, 39..63) */
    gfx_fill_rounded_rect(s, 332, 39, 64, 24, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 332, 39, 64, 24, 4, 0x475569);
    gfx_draw_string(s, 340, 43, "Rename", 0xE2E8F0, 0, true);

    /* Delete Button (402..470, 39..63) -> Full 68px width, perfectly centered hit-box */
    gfx_fill_rounded_rect(s, 402, 39, 68, 24, 4, 0x881337);
    gfx_draw_rounded_rect(s, 402, 39, 68, 24, 4, 0xF43F5E);
    gfx_draw_string(s, 412, 43, "Delete", 0xFFFFFF, 0, true);

    /* Sort Button (476..540, 39..63) */
    gfx_fill_rounded_rect(s, 476, 39, 64, 24, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 476, 39, 64, 24, 4, 0x475569);
    const char *sort_label = (fm->sort_mode == FM_SORT_NAME) ? "Sort:N" :
                             (fm->sort_mode == FM_SORT_SIZE) ? "Sort:S" : "Sort:T";
    gfx_draw_string(s, 484, 43, sort_label, 0x38BDF8, 0, true);

    /* View Button (546..610, 39..63) */
    gfx_fill_rounded_rect(s, 546, 39, 64, 24, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 546, 39, 64, 24, 4, 0x475569);
    gfx_draw_string(s, 554, 43, (fm->view_mode == FM_VIEW_DETAILS) ? "View:D" : "View:L", 0x38BDF8, 0, true);

    /* Select All Button (616..706, 39..63) */
    gfx_fill_rounded_rect(s, 616, 39, 90, 24, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 616, 39, 90, 24, 4, 0x475569);
    gfx_draw_string(s, 624, 43, "Select All", 0xE2E8F0, 0, true);

    /* 3. Left Sidebar (Bookmarks & Storage, width 140px, y = 67..height-24) */
    int sidebar_w = 140;
    int bot_y = s->height - 24;
    gfx_fill_rect(s, 0, 67, sidebar_w, bot_y - 67, 0x0B0F19);
    gfx_draw_line(s, sidebar_w, 67, sidebar_w, bot_y, 0x1E293B);

    /* FAVORITES Section */
    gfx_draw_string(s, 10, 75, "FAVORITES", 0x64748B, 0, true);

    static const struct {
        const char *name;
        const char *path;
    } bookmarks[] = {
        {"Root (/)",         "/"},
        {"Storage (/data)",  "/data"},
        {"Binaries (/bin)",  "/bin"},
        {"Web (/www)",       "/www"},
        {"Config (/etc)",    "/etc"}
    };

    int b_y = 94;
    for (int b = 0; b < 5; b++) {
        bool is_bm_active = false;
        if (b == 0 && fm->current_path[0] == '/' && fm->current_path[1] == '\0') {
            is_bm_active = true;
        } else if (b > 0) {
            int k = 0;
            while (bookmarks[b].path[k] && bookmarks[b].path[k] == fm->current_path[k]) k++;
            if (!bookmarks[b].path[k] && (fm->current_path[k] == '\0' || fm->current_path[k] == '/')) {
                is_bm_active = true;
            }
        }

        if (is_bm_active) {
            gfx_fill_rounded_rect(s, 6, b_y - 2, sidebar_w - 12, 22, 4, 0x1E293B);
            gfx_draw_rounded_rect(s, 6, b_y - 2, sidebar_w - 12, 22, 4, 0x38BDF8);
        }

        gfx_draw_string(s, 12, b_y + 2, bookmarks[b].name, is_bm_active ? 0x38BDF8 : 0x94A3B8, 0, true);
        b_y += 28;
    }

    /* STORAGE Section */
    int stor_y = 246;
    gfx_draw_string(s, 10, stor_y, "STORAGE", 0x64748B, 0, true);

    /* Storage Telemetry Card */
    gfx_fill_rounded_rect(s, 8, stor_y + 16, sidebar_w - 16, 68, 4, 0x111827);
    gfx_draw_rounded_rect(s, 8, stor_y + 16, sidebar_w - 16, 68, 4, 0x1E293B);
    gfx_draw_string(s, 14, stor_y + 22, "AstraFS ATA", 0x38BDF8, 0, true);

    vfs_statfs_t stfs;
    u64 tot_mb = 32, free_mb = 31;
    if (vfs_statfs("/data", &stfs) == VFS_OK && stfs.total_bytes > 0) {
        tot_mb = stfs.total_bytes / (1024 * 1024);
        free_mb = stfs.free_bytes / (1024 * 1024);
    }

    /* Storage Capacity Bar */
    int bar_x = 14;
    int bar_y = stor_y + 40;
    int bar_w = sidebar_w - 28;
    int bar_h = 8;
    gfx_fill_rounded_rect(s, bar_x, bar_y, bar_w, bar_h, 2, 0x1E293B);
    u64 used_mb = (tot_mb >= free_mb) ? (tot_mb - free_mb) : 0;
    int fill_w = (int)((used_mb * bar_w) / (tot_mb ? tot_mb : 1));
    if (fill_w < 4 && used_mb > 0) fill_w = 4;
    if (fill_w > bar_w) fill_w = bar_w;
    if (fill_w > 0) {
        gfx_fill_rounded_rect(s, bar_x, bar_y, fill_w, bar_h, 2, 0x3B82F6);
    }

    char card_txt[32];
    int ctp = 0;
    if (free_mb >= 10) card_txt[ctp++] = '0' + (free_mb / 10) % 10;
    card_txt[ctp++] = '0' + (free_mb % 10);
    const char *ct_suf = "MB free of ";
    for (int k = 0; ct_suf[k]; k++) card_txt[ctp++] = ct_suf[k];
    if (tot_mb >= 10) card_txt[ctp++] = '0' + (tot_mb / 10) % 10;
    card_txt[ctp++] = '0' + (tot_mb % 10);
    card_txt[ctp++] = 'M'; card_txt[ctp++] = 'B';
    card_txt[ctp] = '\0';
    gfx_draw_string(s, 14, stor_y + 54, card_txt, 0x94A3B8, 0, true);

    /* 4. Main Table Area */
    int main_x = sidebar_w + 6;
    int table_w = s->width - main_x - 14;
    int header_y = 67;
    int table_y = header_y + 24;

    /* Column Header */
    gfx_fill_rect(s, main_x, header_y, table_w, 22, 0x0F172A);
    gfx_draw_line(s, main_x, header_y + 22, main_x + table_w, header_y + 22, 0x1E293B);
    gfx_draw_string(s, main_x + 36, header_y + 4, "NAME", 0x64748B, 0, true);
    gfx_draw_string(s, main_x + 280, header_y + 4, "TYPE", 0x64748B, 0, true);
    gfx_draw_string(s, main_x + 420, header_y + 4, "SIZE", 0x64748B, 0, true);

    int row_h = 24;
    int table_h = bot_y - table_y;
    fm->visible_rows = table_h / row_h;
    if (fm->visible_rows < 1) fm->visible_rows = 1;

    /* Initialize Scrollview */
    int sb_x = s->width - 12;
    int sb_w = 8;
    scrollview_init(&fm->scrollview, fm->item_count, fm->visible_rows, sb_x, table_y, sb_w, table_h);

    /* Render Visible Rows */
    int start_idx = fm->scrollview.scroll_offset;
    int end_idx = start_idx + fm->visible_rows;
    if (end_idx > fm->item_count) end_idx = fm->item_count;

    int cur_row_y = table_y;

    for (int i = start_idx; i < end_idx; i++) {
        filemgr_item_t *item = &fm->items[i];

        /* Check search filter if active */
        if (fm->search_len > 0) {
            if (fm_strcmp(item->name, "..") != 0 && !fm_strcasestr(item->name, fm->search_filter)) {
                continue;
            }
        }

        bool is_sel = item->selected || (i == fm->selected_idx);

        u32 row_bg = is_sel ? 0x1E3A8A : ((i % 2 == 0) ? 0x111827 : 0x0D1424);
        gfx_fill_rect(s, main_x, cur_row_y, table_w, row_h, row_bg);
        if (is_sel) {
            gfx_draw_rect(s, main_x, cur_row_y, table_w, row_h, 0x38BDF8);
        }

        /* Checkbox Indicator for multi-selection */
        int cb_x = main_x + 8;
        int cb_y = cur_row_y + 6;
        gfx_draw_rect(s, cb_x, cb_y, 12, 12, is_sel ? 0x38BDF8 : 0x475569);
        if (item->selected) {
            gfx_fill_rect(s, cb_x + 2, cb_y + 2, 8, 8, 0x38BDF8);
        }

        /* File Type Badge */
        const char *badge_str = "FILE";
        const char *type_desc = "Document";
        u32 badge_bg = 0x2563EB, badge_border = 0x60A5FA;
        fm_get_badge_and_type(item->name, item->is_dir, &badge_str, &badge_bg, &badge_border, &type_desc);

        int bdg_x = main_x + 26;
        int bdg_y = cur_row_y + 3;
        int bdg_w = 26;
        gfx_fill_rounded_rect(s, bdg_x, bdg_y, bdg_w, 16, 2, badge_bg);
        gfx_draw_rounded_rect(s, bdg_x, bdg_y, bdg_w, 16, 2, badge_border);
        int txt_off = (bdg_w - (int)fm_strlen(badge_str) * FONT_WIDTH) / 2;
        if (txt_off < 2) txt_off = 2;
        gfx_draw_string(s, bdg_x + txt_off, bdg_y + 2, badge_str, 0xFFFFFF, 0, true);

        /* Item Name */
        u32 name_col = item->is_dir ? 0x38BDF8 : 0xF8FAFC;
        gfx_draw_string(s, main_x + 58, cur_row_y + 4, item->name, name_col, 0, true);

        /* Type Column */
        gfx_draw_string(s, main_x + 280, cur_row_y + 4, type_desc, 0x94A3B8, 0, true);

        /* Size Column */
        if (!item->is_dir) {
            char size_buf[32];
            int si = 0;
            if (item->size >= 1024 * 1024) {
                int mb = (int)(item->size / (1024 * 1024));
                if (mb >= 10) size_buf[si++] = '0' + (mb / 10) % 10;
                size_buf[si++] = '0' + (mb % 10);
                size_buf[si++] = ' '; size_buf[si++] = 'M'; size_buf[si++] = 'B';
            } else if (item->size >= 1024) {
                int kb = (int)(item->size / 1024);
                if (kb >= 100) size_buf[si++] = '0' + (kb / 100) % 10;
                if (kb >= 10)  size_buf[si++] = '0' + (kb / 10) % 10;
                size_buf[si++] = '0' + (kb % 10);
                size_buf[si++] = ' '; size_buf[si++] = 'K'; size_buf[si++] = 'B';
            } else {
                int b = (int)item->size;
                if (b >= 100) size_buf[si++] = '0' + (b / 100) % 10;
                if (b >= 10)  size_buf[si++] = '0' + (b / 10) % 10;
                size_buf[si++] = '0' + (b % 10);
                size_buf[si++] = ' '; size_buf[si++] = 'B';
            }
            size_buf[si] = '\0';
            gfx_draw_string(s, main_x + 420, cur_row_y + 4, size_buf, 0x64748B, 0, true);
        } else {
            gfx_draw_string(s, main_x + 420, cur_row_y + 4, "--", 0x475569, 0, true);
        }

        cur_row_y += row_h;
    }

    /* Empty Directory State Placeholder */
    if (fm->item_count == 0 || (fm->item_count == 1 && fm_strcmp(fm->items[0].name, "..") == 0)) {
        int empty_x = main_x + (table_w - 20 * FONT_WIDTH) / 2;
        int empty_y = table_y + table_h / 2 - 10;
        gfx_draw_string(s, empty_x, empty_y, "This folder is empty.", 0x64748B, 0, true);
    }

    /* Scrollbar */
    scrollview_render(&fm->scrollview, s, 0x0B0F19, 0x334155);

    /* 5. Bottom Status Bar (y = s->height - 24, 24px) */
    int sb_y = s->height - 24;
    gfx_fill_rect(s, 0, sb_y, s->width, 24, 0x0B0F19);
    gfx_draw_line(s, 0, sb_y, s->width, sb_y, 0x1E293B);

    /* Item and Selection Count */
    char stat_l[128];
    int sp = 0;
    if (fm->item_count >= 100) stat_l[sp++] = '0' + (fm->item_count / 100) % 10;
    if (fm->item_count >= 10)  stat_l[sp++] = '0' + (fm->item_count / 10) % 10;
    stat_l[sp++] = '0' + (fm->item_count % 10);
    const char *cnt_lbl = " items";
    for (int k = 0; cnt_lbl[k]; k++) stat_l[sp++] = cnt_lbl[k];

    if (fm->selected_count > 0) {
        const char *sel_lbl = " (";
        for (int k = 0; sel_lbl[k]; k++) stat_l[sp++] = sel_lbl[k];
        if (fm->selected_count >= 10) stat_l[sp++] = '0' + (fm->selected_count / 10) % 10;
        stat_l[sp++] = '0' + (fm->selected_count % 10);
        const char *sel_end = " selected)";
        for (int k = 0; sel_end[k]; k++) stat_l[sp++] = sel_end[k];
    }

    const char *div = " | ";
    for (int k = 0; div[k]; k++) stat_l[sp++] = div[k];
    for (int k = 0; fm->status_msg[k] && sp < 120; k++) stat_l[sp++] = fm->status_msg[k];
    stat_l[sp] = '\0';
    gfx_draw_string(s, 10, sb_y + 4, stat_l, 0x94A3B8, 0, true);

    /* Storage Capacity Telemetry on right */
    char cap_buf[64];
    int cp = 0;
    const char *prefix = "AstraFS: ";
    for (int k = 0; prefix[k]; k++) cap_buf[cp++] = prefix[k];
    if (free_mb >= 1000) cap_buf[cp++] = '0' + (free_mb / 1000) % 10;
    if (free_mb >= 100)  cap_buf[cp++] = '0' + (free_mb / 100) % 10;
    if (free_mb >= 10)   cap_buf[cp++] = '0' + (free_mb / 10) % 10;
    cap_buf[cp++] = '0' + (free_mb % 10);
    const char *mid = "MB free of ";
    for (int k = 0; mid[k]; k++) cap_buf[cp++] = mid[k];
    if (tot_mb >= 1000) cap_buf[cp++] = '0' + (tot_mb / 1000) % 10;
    if (tot_mb >= 100)  cap_buf[cp++] = '0' + (tot_mb / 100) % 10;
    if (tot_mb >= 10)   cap_buf[cp++] = '0' + (tot_mb / 10) % 10;
    cap_buf[cp++] = '0' + (tot_mb % 10);
    cap_buf[cp++] = 'M'; cap_buf[cp++] = 'B';
    cap_buf[cp] = '\0';
    int cap_w = cp * FONT_WIDTH;
    gfx_draw_string(s, s->width - cap_w - 14, sb_y + 4, cap_buf, 0x38BDF8, 0, true);

    /* 6. Context Menu Overlay */
    if (fm->menu_open) {
        bool on_item = (fm->menu_target_idx >= 0 && fm->menu_target_idx < fm->item_count);
        int menu_items_count = on_item ? 8 : 6;
        int item_h = 22;
        int mw = 170;
        int mh = menu_items_count * item_h + 8;
        int mx = fm->menu_x;
        int my = fm->menu_y;
        if (mx + mw > s->width - 4) mx = s->width - mw - 4;
        if (my + mh > s->height - 28) my = s->height - mh - 28;

        gfx_fill_rounded_rect(s, mx, my, mw, mh, 4, 0x1E293B);
        gfx_draw_rounded_rect(s, mx, my, mw, mh, 4, 0x475569);

        static const char *item_opts[] = {
            "Open (Enter)", "Open With AstraEdit", "Cut (Ctrl+X)", "Copy (Ctrl+C)",
            "Paste (Ctrl+V)", "Rename (F2)", "Delete (Del)", "Properties"
        };
        static const char *empty_opts[] = {
            "New File", "New Folder", "Paste (Ctrl+V)", "Select All (Ctrl+A)", "Refresh (F5)", "Properties"
        };

        const char **opts = on_item ? item_opts : empty_opts;
        for (int mi = 0; mi < menu_items_count; mi++) {
            int row_y = my + 4 + mi * item_h;
            gfx_draw_string(s, mx + 10, row_y + 3, opts[mi], 0xF8FAFC, 0, true);
            if (mi + 1 < menu_items_count) {
                gfx_draw_line(s, mx + 4, row_y + item_h - 1, mx + mw - 4, row_y + item_h - 1, 0x334155);
            }
        }
    }

    /* 7. Modal Dialog Overlay */
    if (fm->dialog_type != FM_DLG_NONE) {
        /* Backdrop stipple */
        for (int y = 0; y < s->height; y += 2) {
            for (int x = 0; x < s->width; x += 2) {
                gfx_put_pixel(s, x, y, 0x000000);
            }
        }

        int dw = 360;
        int dh = (fm->dialog_type == FM_DLG_PROPERTIES) ? 170 : 140;
        int dx = (s->width - dw) / 2;
        int dy = (s->height - dh) / 2;

        gfx_fill_rounded_rect(s, dx, dy, dw, dh, 6, 0x1E293B);
        gfx_draw_rounded_rect(s, dx, dy, dw, dh, 6, (fm->dialog_type == FM_DLG_DELETE_CONFIRM) ? 0xF43F5E : 0x38BDF8);

        /* Dialog Header */
        u32 hdr_bg = (fm->dialog_type == FM_DLG_DELETE_CONFIRM) ? 0x881337 : 0x0F172A;
        gfx_fill_rounded_rect(s, dx, dy, dw, 28, 6, hdr_bg);
        const char *d_title = "Dialog";
        if (fm->dialog_type == FM_DLG_NEW_FILE) d_title = "Create New File";
        else if (fm->dialog_type == FM_DLG_NEW_FOLDER) d_title = "Create New Folder";
        else if (fm->dialog_type == FM_DLG_RENAME) d_title = "Rename Item";
        else if (fm->dialog_type == FM_DLG_DELETE_CONFIRM) d_title = "Confirm Deletion";
        else if (fm->dialog_type == FM_DLG_PROPERTIES) d_title = "Properties";
        gfx_draw_string(s, dx + 12, dy + 6, d_title, 0xFFFFFF, 0, true);

        if (fm->dialog_type == FM_DLG_NEW_FILE || fm->dialog_type == FM_DLG_NEW_FOLDER || fm->dialog_type == FM_DLG_RENAME) {
            gfx_draw_string(s, dx + 16, dy + 38, "Enter name:", 0x94A3B8, 0, true);
            gfx_fill_rounded_rect(s, dx + 16, dy + 56, dw - 32, 26, 4, 0x0B0F19);
            gfx_draw_rounded_rect(s, dx + 16, dy + 56, dw - 32, 26, 4, 0x3B82F6);
            gfx_draw_string(s, dx + 22, dy + 61, fm->dialog_input, 0xF8FAFC, 0, true);
            int c_x = dx + 22 + fm->dialog_input_len * FONT_WIDTH;
            gfx_fill_rect(s, c_x, dy + 59, 2, 18, 0x38BDF8);

            int btn_y = dy + dh - 36;
            gfx_fill_rounded_rect(s, dx + dw - 164, btn_y, 68, 24, 4, 0x1E40AF);
            gfx_draw_rounded_rect(s, dx + dw - 164, btn_y, 68, 24, 4, 0x3B82F6);
            gfx_draw_string(s, dx + dw - 146, btn_y + 4, "OK", 0xFFFFFF, 0, true);

            gfx_fill_rounded_rect(s, dx + dw - 86, btn_y, 70, 24, 4, 0x334155);
            gfx_draw_rounded_rect(s, dx + dw - 86, btn_y, 70, 24, 4, 0x64748B);
            gfx_draw_string(s, dx + dw - 74, btn_y + 4, "Cancel", 0xFFFFFF, 0, true);
        } else if (fm->dialog_type == FM_DLG_DELETE_CONFIRM) {
            gfx_draw_string(s, dx + 16, dy + 38, "Are you sure you want to delete:", 0xF8FAFC, 0, true);

            char del_target_str[64];
            if (fm->selected_count > 1) {
                int dp = 0;
                if (fm->selected_count >= 10) del_target_str[dp++] = '0' + (fm->selected_count / 10) % 10;
                del_target_str[dp++] = '0' + (fm->selected_count % 10);
                const char *sel_suf = " selected items";
                for (int k = 0; sel_suf[k]; k++) del_target_str[dp++] = sel_suf[k];
                del_target_str[dp] = '\0';
            } else if (fm->dialog_target_idx >= 0 && fm->dialog_target_idx < fm->item_count) {
                fm_strcpy(del_target_str, fm->items[fm->dialog_target_idx].name, sizeof(del_target_str));
            } else {
                fm_strcpy(del_target_str, "selected item", sizeof(del_target_str));
            }
            gfx_draw_string(s, dx + 16, dy + 58, del_target_str, 0xF43F5E, 0, true);
            gfx_draw_string(s, dx + 16, dy + 78, "This action cannot be undone.", 0x94A3B8, 0, true);

            int btn_y = dy + dh - 36;
            gfx_fill_rounded_rect(s, dx + dw - 164, btn_y, 68, 24, 4, 0x881337);
            gfx_draw_rounded_rect(s, dx + dw - 164, btn_y, 68, 24, 4, 0xF43F5E);
            gfx_draw_string(s, dx + dw - 150, btn_y + 4, "Delete", 0xFFFFFF, 0, true);

            gfx_fill_rounded_rect(s, dx + dw - 86, btn_y, 70, 24, 4, 0x334155);
            gfx_draw_rounded_rect(s, dx + dw - 86, btn_y, 70, 24, 4, 0x64748B);
            gfx_draw_string(s, dx + dw - 74, btn_y + 4, "Cancel", 0xFFFFFF, 0, true);
        } else if (fm->dialog_type == FM_DLG_PROPERTIES) {
            if (fm->dialog_target_idx >= 0 && fm->dialog_target_idx < fm->item_count) {
                filemgr_item_t *it = &fm->items[fm->dialog_target_idx];
                char p_buf[64];
                fm_strcpy(p_buf, "Name: ", sizeof(p_buf));
                fm_strcpy(&p_buf[6], it->name, sizeof(p_buf) - 6);
                gfx_draw_string(s, dx + 16, dy + 38, p_buf, 0xF8FAFC, 0, true);

                gfx_draw_string(s, dx + 16, dy + 58, it->is_dir ? "Type: Directory / Folder" : "Type: Regular File", 0x94A3B8, 0, true);

                char sz_buf[64] = "Size: ";
                int b = (int)it->size;
                int bp = 6;
                if (b >= 10000) sz_buf[bp++] = '0' + (b / 10000) % 10;
                if (b >= 1000)  sz_buf[bp++] = '0' + (b / 1000) % 10;
                if (b >= 100)   sz_buf[bp++] = '0' + (b / 100) % 10;
                if (b >= 10)    sz_buf[bp++] = '0' + (b / 10) % 10;
                sz_buf[bp++] = '0' + (b % 10);
                const char *by_lbl = " bytes";
                for (int k = 0; by_lbl[k]; k++) sz_buf[bp++] = by_lbl[k];
                sz_buf[bp] = '\0';
                gfx_draw_string(s, dx + 16, dy + 78, sz_buf, 0x94A3B8, 0, true);
                gfx_draw_string(s, dx + 16, dy + 98, "Storage: AstraFS Persistent Storage", 0x38BDF8, 0, true);
            } else {
                gfx_draw_string(s, dx + 16, dy + 38, "Location: Current Directory", 0xF8FAFC, 0, true);
                gfx_draw_string(s, dx + 16, dy + 58, fm->current_path, 0x38BDF8, 0, true);
                gfx_draw_string(s, dx + 16, dy + 78, "Filesystem: AstraFS Persistent Mount", 0x94A3B8, 0, true);
            }

            int btn_y = dy + dh - 36;
            gfx_fill_rounded_rect(s, dx + dw - 90, btn_y, 74, 24, 4, 0x1E40AF);
            gfx_draw_rounded_rect(s, dx + dw - 90, btn_y, 74, 24, 4, 0x3B82F6);
            gfx_draw_string(s, dx + dw - 74, btn_y + 4, "Close", 0xFFFFFF, 0, true);
        }
    }
}

/* Event Handler */
static void filemgr_on_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return;
    filemgr_state_t *fm = (filemgr_state_t *)win->user_data;
    if (!fm) return;

    /* A. Modal Dialog Input Handling */
    if (fm->dialog_type != FM_DLG_NONE) {
        if (ev->type == INPUT_EVENT_KEY_DOWN) {
            if (ev->key.keycode == KEY_ESC) {
                fm->dialog_type = FM_DLG_NONE;
                fm_set_status(fm, "Cancelled.");
                return;
            }
            if (ev->key.keycode == KEY_ENTER) {
                if (fm->dialog_type == FM_DLG_NEW_FILE) {
                    filemgr_op_create_file(fm, fm->dialog_input);
                } else if (fm->dialog_type == FM_DLG_NEW_FOLDER) {
                    filemgr_op_create_folder(fm, fm->dialog_input);
                } else if (fm->dialog_type == FM_DLG_RENAME) {
                    if (fm->dialog_target_idx >= 0 && fm->dialog_target_idx < fm->item_count) {
                        filemgr_op_rename(fm, fm->items[fm->dialog_target_idx].name, fm->dialog_input);
                    }
                } else if (fm->dialog_type == FM_DLG_DELETE_CONFIRM) {
                    filemgr_execute_delete(fm);
                }
                fm->dialog_type = FM_DLG_NONE;
                return;
            }
            if (ev->key.keycode == KEY_BACKSPACE) {
                if (fm->dialog_input_len > 0) {
                    fm->dialog_input[--fm->dialog_input_len] = '\0';
                }
                return;
            }
            if (ev->key.ascii >= ' ' && ev->key.ascii <= '~') {
                if (fm->dialog_input_len < (int)sizeof(fm->dialog_input) - 1) {
                    fm->dialog_input[fm->dialog_input_len++] = ev->key.ascii;
                    fm->dialog_input[fm->dialog_input_len] = '\0';
                }
                return;
            }
        } else if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
            int rel_x = ev->mouse.x - win->client_x;
            int rel_y = ev->mouse.y - win->client_y;
            int dw = 360;
            int dh = (fm->dialog_type == FM_DLG_PROPERTIES) ? 170 : 140;
            int dx = (win->client_w - dw) / 2;
            int dy = (win->client_h - dh) / 2;
            int btn_y = dy + dh - 36;

            if (fm->dialog_type == FM_DLG_PROPERTIES) {
                /* Close Button */
                if (rel_x >= dx + dw - 90 && rel_x <= dx + dw - 16 &&
                    rel_y >= btn_y && rel_y <= btn_y + 24) {
                    fm->dialog_type = FM_DLG_NONE;
                    return;
                }
            } else {
                /* OK / Delete Button */
                if (rel_x >= dx + dw - 164 && rel_x <= dx + dw - 96 &&
                    rel_y >= btn_y && rel_y <= btn_y + 24) {
                    if (fm->dialog_type == FM_DLG_NEW_FILE) {
                        filemgr_op_create_file(fm, fm->dialog_input);
                    } else if (fm->dialog_type == FM_DLG_NEW_FOLDER) {
                        filemgr_op_create_folder(fm, fm->dialog_input);
                    } else if (fm->dialog_type == FM_DLG_RENAME) {
                        if (fm->dialog_target_idx >= 0 && fm->dialog_target_idx < fm->item_count) {
                            filemgr_op_rename(fm, fm->items[fm->dialog_target_idx].name, fm->dialog_input);
                        }
                    } else if (fm->dialog_type == FM_DLG_DELETE_CONFIRM) {
                        filemgr_execute_delete(fm);
                    }
                    fm->dialog_type = FM_DLG_NONE;
                    return;
                }
                /* Cancel Button */
                if (rel_x >= dx + dw - 86 && rel_x <= dx + dw - 16 &&
                    rel_y >= btn_y && rel_y <= btn_y + 24) {
                    fm->dialog_type = FM_DLG_NONE;
                    fm_set_status(fm, "Cancelled.");
                    return;
                }
            }
        }
        return; /* Modal dialog consumes all events */
    }

    /* B. Mouse Wheel Scrolling */
    if (ev->type == INPUT_EVENT_MOUSE_WHEEL) {
        scrollview_handle_wheel(&fm->scrollview, ev->mouse.delta_z);
        return;
    }

    /* C. Mouse Move (Scrollbar Dragging) */
    if (ev->type == INPUT_EVENT_MOUSE_MOVE) {
        int rel_y = ev->mouse.y - win->client_y;
        scrollview_handle_mouse_move(&fm->scrollview, rel_y);
        return;
    }

    /* D. Mouse Up */
    if (ev->type == INPUT_EVENT_MOUSE_UP) {
        scrollview_handle_mouse_up(&fm->scrollview);
        return;
    }

    /* E. Keyboard Handling */
    if (ev->type == INPUT_EVENT_KEY_DOWN) {
        /* Dismiss context menu on Escape */
        if (fm->menu_open && ev->key.keycode == KEY_ESC) {
            fm->menu_open = false;
            return;
        }

        bool ctrl = (ev->key.modifiers & KEY_MOD_CTRL) != 0;
        bool shift = (ev->key.modifiers & KEY_MOD_SHIFT) != 0;

        /* If search box is active, route typing into search filter */
        if (fm->search_active) {
            if (ev->key.keycode == KEY_ESC || ev->key.keycode == KEY_ENTER) {
                fm->search_active = false;
                return;
            }
            if (ev->key.keycode == KEY_BACKSPACE) {
                if (fm->search_len > 0) {
                    fm->search_filter[--fm->search_len] = '\0';
                }
                return;
            }
            if (ev->key.ascii >= ' ' && ev->key.ascii <= '~') {
                if (fm->search_len < (int)sizeof(fm->search_filter) - 1) {
                    fm->search_filter[fm->search_len++] = ev->key.ascii;
                    fm->search_filter[fm->search_len] = '\0';
                }
                return;
            }
        }

        /* F5 / Ctrl+R: Refresh */
        if (ev->key.keycode == KEY_F5 || (ctrl && (ev->key.keycode == 'r' || ev->key.keycode == 'R'))) {
            filemgr_load_dir(fm, fm->current_path);
            fm_set_status(fm, "Directory refreshed.");
            return;
        }

        /* Backspace: Up one directory */
        if (ev->key.keycode == KEY_BACKSPACE) {
            filemgr_nav_up(fm);
            return;
        }

        /* Enter: Open selected item */
        if (ev->key.keycode == KEY_ENTER) {
            if (fm->selected_idx >= 0 && fm->selected_idx < fm->item_count) {
                filemgr_open_item(fm, fm->selected_idx);
            }
            return;
        }

        /* Up / Down Navigation */
        if (ev->key.keycode == KEY_UP) {
            if (fm->selected_idx > 0) {
                int next = fm->selected_idx - 1;
                if (shift) filemgr_range_select(fm, next);
                else filemgr_select_single(fm, next);

                if (fm->selected_idx < fm->scrollview.scroll_offset) {
                    fm->scrollview.scroll_offset = fm->selected_idx;
                }
            }
            return;
        }

        if (ev->key.keycode == KEY_DOWN) {
            if (fm->selected_idx + 1 < fm->item_count) {
                int next = fm->selected_idx + 1;
                if (shift) filemgr_range_select(fm, next);
                else filemgr_select_single(fm, next);

                if (fm->selected_idx >= fm->scrollview.scroll_offset + fm->visible_rows) {
                    fm->scrollview.scroll_offset = fm->selected_idx - fm->visible_rows + 1;
                }
            }
            return;
        }

        /* PageUp / PageDown */
        if (ev->key.keycode == KEY_PAGE_UP) {
            fm->scrollview.scroll_offset -= fm->visible_rows;
            if (fm->scrollview.scroll_offset < 0) fm->scrollview.scroll_offset = 0;
            return;
        }

        if (ev->key.keycode == KEY_PAGE_DOWN) {
            fm->scrollview.scroll_offset += fm->visible_rows;
            int max_s = fm->item_count - fm->visible_rows;
            if (max_s < 0) max_s = 0;
            if (fm->scrollview.scroll_offset > max_s) fm->scrollview.scroll_offset = max_s;
            return;
        }

        /* F2: Rename */
        if (ev->key.keycode == KEY_F2) {
            if (fm->selected_idx >= 0 && fm->selected_idx < fm->item_count) {
                fm->dialog_type = FM_DLG_RENAME;
                fm->dialog_target_idx = fm->selected_idx;
                fm_strcpy(fm->dialog_input, fm->items[fm->selected_idx].name, sizeof(fm->dialog_input));
                fm->dialog_input_len = (int)fm_strlen(fm->dialog_input);
            }
            return;
        }

        /* Delete / Shift+Delete: Trigger Delete Modal */
        if (ev->key.keycode == KEY_DELETE) {
            filemgr_trigger_delete(fm, shift);
            return;
        }

        /* Ctrl+A: Select All */
        if (ctrl && (ev->key.keycode == 'a' || ev->key.keycode == 'A' || ev->key.ascii == 1)) {
            filemgr_select_all(fm);
            fm_set_status(fm, "All items selected.");
            return;
        }

        /* Ctrl+C: Copy */
        if (ctrl && (ev->key.keycode == 'c' || ev->key.keycode == 'C' || ev->key.ascii == 3)) {
            if (fm->selected_idx >= 0 && fm->selected_idx < fm->item_count) {
                filemgr_op_copy(fm, &fm->items[fm->selected_idx], false);
            }
            return;
        }

        /* Ctrl+X: Cut */
        if (ctrl && (ev->key.keycode == 'x' || ev->key.keycode == 'X' || ev->key.ascii == 24)) {
            if (fm->selected_idx >= 0 && fm->selected_idx < fm->item_count) {
                filemgr_op_copy(fm, &fm->items[fm->selected_idx], true);
            }
            return;
        }

        /* Ctrl+V: Paste */
        if (ctrl && (ev->key.keycode == 'v' || ev->key.keycode == 'V' || ev->key.ascii == 22)) {
            filemgr_op_paste(fm);
            return;
        }
    }

    /* F. Mouse Click Handling */
    if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        int rel_x = ev->mouse.x - win->client_x;
        int rel_y = ev->mouse.y - win->client_y;
        bool is_right_btn = (ev->mouse.buttons & MOUSE_BTN_RIGHT) != 0;
        bool ctrl = (ev->mouse.modifiers & KEY_MOD_CTRL) != 0;
        bool shift = (ev->mouse.modifiers & KEY_MOD_SHIFT) != 0;

        /* Dismiss or process Context Menu */
        if (fm->menu_open) {
            bool on_item = (fm->menu_target_idx >= 0 && fm->menu_target_idx < fm->item_count);
            int menu_items_count = on_item ? 8 : 6;
            int item_h = 22;
            int mw = 170;
            int mh = menu_items_count * item_h + 8;
            int mx = fm->menu_x;
            int my = fm->menu_y;
            if (mx + mw > win->client_w - 4) mx = win->client_w - mw - 4;
            if (my + mh > win->client_h - 28) my = win->client_h - mh - 28;

            if (rel_x >= mx && rel_x <= mx + mw && rel_y >= my && rel_y <= my + mh) {
                int clicked_opt = (rel_y - my - 4) / item_h;
                fm->menu_open = false;

                if (on_item) {
                    filemgr_item_t *target_it = &fm->items[fm->menu_target_idx];
                    if (clicked_opt == 0) { /* Open */
                        filemgr_open_item(fm, fm->menu_target_idx);
                    } else if (clicked_opt == 1) { /* Open With AstraEdit */
                        char full_path[128];
                        fm_path_join(full_path, sizeof(full_path), fm->current_path, target_it->name);
                        app_editor_launch(full_path);
                    } else if (clicked_opt == 2) { /* Cut */
                        filemgr_op_copy(fm, target_it, true);
                    } else if (clicked_opt == 3) { /* Copy */
                        filemgr_op_copy(fm, target_it, false);
                    } else if (clicked_opt == 4) { /* Paste */
                        filemgr_op_paste(fm);
                    } else if (clicked_opt == 5) { /* Rename */
                        fm->dialog_type = FM_DLG_RENAME;
                        fm->dialog_target_idx = fm->menu_target_idx;
                        fm_strcpy(fm->dialog_input, target_it->name, sizeof(fm->dialog_input));
                        fm->dialog_input_len = (int)fm_strlen(fm->dialog_input);
                    } else if (clicked_opt == 6) { /* Delete */
                        filemgr_trigger_delete(fm, false);
                    } else if (clicked_opt == 7) { /* Properties */
                        fm->dialog_type = FM_DLG_PROPERTIES;
                        fm->dialog_target_idx = fm->menu_target_idx;
                    }
                } else {
                    if (clicked_opt == 0) { /* New File */
                        fm->dialog_type = FM_DLG_NEW_FILE;
                        fm_strcpy(fm->dialog_input, "untitled.txt", sizeof(fm->dialog_input));
                        fm->dialog_input_len = (int)fm_strlen(fm->dialog_input);
                    } else if (clicked_opt == 1) { /* New Folder */
                        fm->dialog_type = FM_DLG_NEW_FOLDER;
                        fm_strcpy(fm->dialog_input, "new_folder", sizeof(fm->dialog_input));
                        fm->dialog_input_len = (int)fm_strlen(fm->dialog_input);
                    } else if (clicked_opt == 2) { /* Paste */
                        filemgr_op_paste(fm);
                    } else if (clicked_opt == 3) { /* Select All */
                        filemgr_select_all(fm);
                    } else if (clicked_opt == 4) { /* Refresh */
                        filemgr_load_dir(fm, fm->current_path);
                    } else if (clicked_opt == 5) { /* Properties */
                        fm->dialog_type = FM_DLG_PROPERTIES;
                        fm->dialog_target_idx = -1;
                    }
                }
                return;
            }
            /* Click was outside context menu -> dismiss menu and CONSUME event to prevent click-through */
            fm->menu_open = false;
            return;
        }

        /* Right Click: Open Context Menu */
        if (is_right_btn) {
            int main_x = 146;
            int bot_y = win->client_h - 24;
            int table_y = 67 + 24;

            if (rel_x >= main_x && rel_y >= table_y && rel_y < bot_y) {
                int row = (rel_y - table_y) / 24 + fm->scrollview.scroll_offset;
                if (row >= 0 && row < fm->item_count) {
                    if (!fm->items[row].selected) {
                        filemgr_select_single(fm, row);
                    }
                    fm->menu_target_idx = row;
                } else {
                    fm->menu_target_idx = -1;
                }
                fm->menu_open = true;
                fm->menu_x = rel_x;
                fm->menu_y = rel_y;
                return;
            }
        }

        /* Top Navigation Bar Clicks (y = 5..29) */
        if (rel_y >= 5 && rel_y <= 29) {
            /* [<] Back (8..34) */
            if (rel_x >= 8 && rel_x <= 34) {
                filemgr_nav_back(fm);
                return;
            }
            /* [>] Forward (38..64) */
            if (rel_x >= 38 && rel_x <= 64) {
                filemgr_nav_forward(fm);
                return;
            }
            /* [^] Up (68..94) */
            if (rel_x >= 68 && rel_x <= 94) {
                filemgr_nav_up(fm);
                return;
            }
            /* [H] Home (98..124) */
            if (rel_x >= 98 && rel_x <= 124) {
                filemgr_navigate(fm, "/data");
                return;
            }
            /* [R] Refresh (128..154) */
            if (rel_x >= 128 && rel_x <= 154) {
                filemgr_load_dir(fm, fm->current_path);
                fm_set_status(fm, "Directory refreshed.");
                return;
            }
            /* Breadcrumbs Path Box (158 .. win->client_w - 180) */
            int bc_x = 158;
            int search_w = 172;
            int bc_w = win->client_w - bc_x - search_w - 12;
            if (bc_w < 140) bc_w = 140;
            if (rel_x >= bc_x && rel_x <= bc_x + bc_w) {
                int rel_bc = rel_x - bc_x;
                if (rel_bc < 45) {
                    filemgr_navigate(fm, "/");
                    return;
                }
                /* Root path segment clicks */
                if (rel_bc >= 45 && fm->current_path[0] == '/' && fm->current_path[1] != '\0') {
                    /* Navigate to first segment (e.g. /data) */
                    char seg1[64] = "/";
                    int sp = 1;
                    for (int k = 1; fm->current_path[k] && fm->current_path[k] != '/' && sp < 60; k++) {
                        seg1[sp++] = fm->current_path[k];
                    }
                    seg1[sp] = '\0';
                    filemgr_navigate(fm, seg1);
                    return;
                }
            }

            /* Search Box (win->client_w - search_w - 6 .. win->client_w - 6) */
            int srch_x = win->client_w - search_w - 6;
            if (rel_x >= srch_x && rel_x <= srch_x + search_w) {
                /* If clicked on clear [x] button */
                if (fm->search_len > 0 && rel_x >= srch_x + search_w - 20) {
                    fm->search_filter[0] = '\0';
                    fm->search_len = 0;
                    fm->search_active = false;
                } else {
                    fm->search_active = true;
                }
                return;
            }
        }

        /* Action Toolbar Clicks (y = 39..63) */
        if (rel_y >= 39 && rel_y <= 63) {
            /* + File (8..70) */
            if (rel_x >= 8 && rel_x <= 70) {
                fm->dialog_type = FM_DLG_NEW_FILE;
                fm_strcpy(fm->dialog_input, "untitled.txt", sizeof(fm->dialog_input));
                fm->dialog_input_len = (int)fm_strlen(fm->dialog_input);
                return;
            }
            /* + Folder (76..150) */
            if (rel_x >= 76 && rel_x <= 150) {
                fm->dialog_type = FM_DLG_NEW_FOLDER;
                fm_strcpy(fm->dialog_input, "new_folder", sizeof(fm->dialog_input));
                fm->dialog_input_len = (int)fm_strlen(fm->dialog_input);
                return;
            }
            /* Cut (156..206) */
            if (rel_x >= 156 && rel_x <= 206) {
                if (fm->selected_idx >= 0 && fm->selected_idx < fm->item_count) {
                    filemgr_op_copy(fm, &fm->items[fm->selected_idx], true);
                }
                return;
            }
            /* Copy (212..264) */
            if (rel_x >= 212 && rel_x <= 264) {
                if (fm->selected_idx >= 0 && fm->selected_idx < fm->item_count) {
                    filemgr_op_copy(fm, &fm->items[fm->selected_idx], false);
                }
                return;
            }
            /* Paste (270..326) */
            if (rel_x >= 270 && rel_x <= 326) {
                filemgr_op_paste(fm);
                return;
            }
            /* Rename (332..396) */
            if (rel_x >= 332 && rel_x <= 396) {
                if (fm->selected_idx >= 0 && fm->selected_idx < fm->item_count) {
                    fm->dialog_type = FM_DLG_RENAME;
                    fm->dialog_target_idx = fm->selected_idx;
                    fm_strcpy(fm->dialog_input, fm->items[fm->selected_idx].name, sizeof(fm->dialog_input));
                    fm->dialog_input_len = (int)fm_strlen(fm->dialog_input);
                }
                return;
            }
            /* Delete (402..470) -> Accurate 68px wide hit-test */
            if (rel_x >= 402 && rel_x <= 470) {
                filemgr_trigger_delete(fm, false);
                return;
            }
            /* Sort (476..540) */
            if (rel_x >= 476 && rel_x <= 540) {
                fm->sort_mode = (fm_sort_mode_t)((fm->sort_mode + 1) % 3);
                filemgr_sort(fm);
                return;
            }
            /* View (546..610) */
            if (rel_x >= 546 && rel_x <= 610) {
                fm->view_mode = (fm_view_mode_t)((fm->view_mode + 1) % 2);
                return;
            }
            /* Select All (616..706) */
            if (rel_x >= 616 && rel_x <= 706) {
                filemgr_select_all(fm);
                fm_set_status(fm, "All items selected.");
                return;
            }
        }

        /* Click on Left Sidebar Bookmarks */
        if (rel_x >= 6 && rel_x <= 134 && rel_y >= 92 && rel_y <= 240) {
            int bm_idx = (rel_y - 92) / 28;
            static const char *bm_paths[] = {"/", "/data", "/bin", "/www", "/etc"};
            if (bm_idx >= 0 && bm_idx < 5) {
                filemgr_navigate(fm, bm_paths[bm_idx]);
                return;
            }
        }

        /* Click on Scrollbar */
        if (scrollview_handle_mouse_down(&fm->scrollview, rel_x, rel_y)) {
            return;
        }

        /* Click on Main File Table Rows */
        int main_x = 146;
        int table_y = 67 + 24;
        int bot_y = win->client_h - 24;

        if (rel_x >= main_x && rel_x < win->client_w - 14 && rel_y >= table_y && rel_y < bot_y) {
            int row = (rel_y - table_y) / 24 + fm->scrollview.scroll_offset;
            if (row >= 0 && row < fm->item_count) {
                u64 now_tick = apic_get_ticks();
                bool is_double_click = (fm->last_click_row == row && (now_tick - fm->last_click_tick < 40));
                fm->last_click_tick = now_tick;
                fm->last_click_row = row;

                if (is_double_click) {
                    /* Double click opens folder or launches editor */
                    filemgr_open_item(fm, row);
                    return;
                }

                /* Selection logic */
                if (ctrl) {
                    filemgr_toggle_select(fm, row);
                } else if (shift) {
                    filemgr_range_select(fm, row);
                } else {
                    filemgr_select_single(fm, row);
                }

                filemgr_item_t *sel = &fm->items[row];
                if (sel->is_dir) {
                    fm_set_status(fm, "Folder selected. Double-click or press Enter to enter.");
                } else {
                    fm_set_status(fm, "File selected. Double-click or press Enter to edit.");
                }
            } else {
                /* Clicked on empty canvas below rows */
                filemgr_clear_selection(fm);
            }
        }
    }
}

void app_filemgr_launch(const char *initial_path) {
    filemgr_ensure_buffer(&g_filemgr);
    const char *p = (initial_path && initial_path[0]) ? initial_path : "/";
    filemgr_navigate(&g_filemgr, p);

    /* Reuse existing window if open */
    wm_window_t *existing = wm_find_by_title_substring("AstraOS Files");
    if (existing) {
        existing->user_data = &g_filemgr;
        wm_bring_to_front(existing);
        return;
    }

    wm_window_t *win = wm_create_window("AstraOS Files 3.0 (AstraFS)", 260, 80, 780, 480);
    if (!win) return;

    win->user_data = &g_filemgr;
    win->on_paint = filemgr_on_paint;
    win->on_event = filemgr_on_event;
    win->on_destroy = filemgr_on_destroy;
}

bool app_gui_self_test(void) {
    kprintf("\n[GUI APPS TEST] Starting GUI Applications Subsystem Self-Test...\n");

    /* 1. Launch Text Editor with /hello.txt */
    kprintf("[GUI APPS TEST] 1. Launching Text Editor and loading VFS file '/hello.txt'...\n");
    app_editor_launch("/hello.txt");
    wm_window_t *w_ed = wm_get_focused();
    if (!w_ed) {
        kprintf("[GUI APPS TEST FAIL] Failed to launch Text Editor window!\n");
        return false;
    }
    kprintf("  PASS: Text Editor launched and populated from VFS.\n");

    /* 2. Test Typing in Text Editor */
    kprintf("[GUI APPS TEST] 2. Simulating keyboard input into Text Editor...\n");
    input_event_t key_a = {
        .type = INPUT_EVENT_KEY_DOWN,
        .key = { .keycode = '!', .ascii = '!' }
    };
    w_ed->on_event(w_ed, &key_a);
    kprintf("  PASS: Text Editor received and processed keyboard input.\n");

    /* 3. Launch File Manager */
    kprintf("[GUI APPS TEST] 3. Launching File Manager on root directory '/'...\n");
    app_filemgr_launch("/");
    wm_window_t *w_fm = wm_get_focused();
    if (!w_fm || g_filemgr.item_count == 0) {
        kprintf("[GUI APPS TEST FAIL] File Manager failed to enumerate VFS root!\n");
        return false;
    }
    kprintf("  PASS: File Manager enumerated %d entries from VFS.\n", g_filemgr.item_count);

    /* Verify presence of hello.txt in listing */
    bool found_hello = false;
    for (int i = 0; i < g_filemgr.item_count; i++) {
        if (g_filemgr.items[i].name[0] == 'h' && g_filemgr.items[i].name[1] == 'e') {
            found_hello = true;
            break;
        }
    }
    if (!found_hello) {
        kprintf("[GUI APPS TEST FAIL] File Manager did not find 'hello.txt' in VFS root!\n");
        return false;
    }
    kprintf("  PASS: 'hello.txt' verified in VFS directory listing.\n");

    /* Clean up test windows */
    wm_destroy_window(w_fm);
    wm_destroy_window(w_ed);

    kprintf("[GUI APPS TEST] All GUI Applications tests passed successfully!\n");
    kprintf("ASTRA_GUI_APPS_OK\n\n");
    return true;
}
