#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/vfs.h>
#include <kernel/mem.h>
#include <kernel/heap.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/kprintf.h>
#include <kernel/terminal.h>
#include <kernel/scrollview.h>

extern u64 apic_get_ticks(void);

#define EDITOR_MAX_TABS 6
#define EDITOR_TAB_BUF_SIZE 131072  /* 128 KiB per tab - supports > 3,000 lines */
#define EDITOR_UNDO_BUF_SIZE 32768  /* 32 KiB snapshot for undo & redo */
#define EDITOR_MAX_BROWSER_ENTRIES 256

typedef enum {
    ED_DLG_NONE = 0,
    ED_DLG_FILE_BROWSER,
    ED_DLG_CONFIRM_CLOSE,
    ED_DLG_CONFIRM_RUN,
    ED_DLG_FIND,
    ED_DLG_REPLACE
} ed_dialog_type_t;

typedef struct editor_tab {
    char filepath[128];
    char title[32];
    bool is_untitled;
    char *text;          /* Mapped from PMM physical pages (128 KiB) */
    phys_addr_t text_pa;
    size_t text_pages;
    int text_len;
    int cursor_pos;
    int sel_start;       /* -1 if no selection */
    int sel_end;         /* -1 if no selection */
    bool dirty;
    int vscroll;         /* First visible line in canvas */
    int hscroll;         /* First visible column in canvas */
    int total_lines;

    /* Single undo snapshot */
    char *undo_text;     /* Mapped from PMM physical pages (32 KiB) */
    phys_addr_t undo_pa;
    size_t undo_pages;
    int undo_len;
    int undo_pos;
    bool has_undo;

    /* Single redo snapshot */
    char *redo_text;     /* Mapped from PMM physical pages (32 KiB) */
    phys_addr_t redo_pa;
    size_t redo_pages;
    int redo_len;
    int redo_pos;
    bool has_redo;

    scrollview_t scrollview;
} editor_tab_t;

typedef struct ed_file_entry {
    char name[64];
    bool is_dir;
    size_t size;
} ed_file_entry_t;

typedef struct ed_file_browser {
    bool is_save_mode;       /* false = Open dialog, true = Save As dialog */
    bool run_after_save;     /* Flag to launch Run in terminal once saved */
    char current_dir[128];
    char history[8][128];
    int history_cnt;
    int history_idx;

    ed_file_entry_t *entries;/* Mapped from PMM pages */
    phys_addr_t entries_pa;
    size_t entries_pages;
    int entry_count;
    int selected_entry;
    int vscroll;
    u64 last_click_tick;
    int last_click_entry;

    char filename[64];
    int filename_len;

    int filter_idx;          /* 0 = All Files, 1 = C/C++, 2 = Python, 3 = Text */
    bool confirm_overwrite;
    char error_msg[64];
} ed_file_browser_t;

typedef struct ed_find_replace {
    char find_str[64];
    int find_len;
    char replace_str[64];
    int replace_len;
    int active_field;        /* 0 = find, 1 = replace */
    char status[64];
} ed_find_replace_t;

typedef struct editor_state {
    editor_tab_t tabs[EDITOR_MAX_TABS];
    int tab_count;
    int active_tab;
    char status_msg[128];

    ed_dialog_type_t dialog_type;
    int close_tab_target;
    int untitled_counter;

    ed_file_browser_t browser;
    ed_find_replace_t fr;
} editor_state_t;

static editor_state_t g_editor = {0};

/* String utilities */
static size_t ed_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void ed_strcpy(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < max_len) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int ed_strcmp(const char *a, const char *b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static int ed_strncmp(const char *a, const char *b, size_t n) {
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

static void ed_path_join(char *dst, size_t max_len, const char *dir, const char *name) {
    if (!dst || max_len == 0) return;
    if (!dir || !dir[0] || (dir[0] == '/' && dir[1] == '\0')) {
        dst[0] = '/';
        dst[1] = '\0';
        if (name && name[0]) {
            size_t p = 1;
            for (size_t i = 0; name[i] && p + 1 < max_len; i++) dst[p++] = name[i];
            dst[p] = '\0';
        }
        return;
    }
    size_t p = 0;
    for (size_t i = 0; dir[i] && p + 1 < max_len; i++) dst[p++] = dir[i];
    if (p > 0 && dst[p - 1] != '/' && p + 1 < max_len) dst[p++] = '/';
    if (name) {
        for (size_t i = 0; name[i] && p + 1 < max_len; i++) dst[p++] = name[i];
    }
    dst[p] = '\0';
}

static void ed_set_status(editor_state_t *ed, const char *msg) {
    if (!ed) return;
    ed_strcpy(ed->status_msg, msg, sizeof(ed->status_msg));
}

static void ed_extract_basename(char *dst, size_t max_len, const char *path) {
    if (!dst || max_len == 0) return;
    if (!path || !path[0]) {
        ed_strcpy(dst, "untitled.txt", max_len);
        return;
    }
    const char *last = path;
    for (int i = 0; path[i]; i++) {
        if (path[i] == '/') last = &path[i + 1];
    }
    if (!*last) last = path;
    ed_strcpy(dst, last, max_len);
}

static int ed_count_lines(const char *text, int len) {
    if (!text || len <= 0) return 1;
    int lines = 1;
    for (int i = 0; i < len; i++) {
        if (text[i] == '\n') lines++;
    }
    return lines;
}

/* Selection Helpers */
static bool ed_has_selection(const editor_tab_t *tab) {
    if (!tab) return false;
    return (tab->sel_start >= 0 && tab->sel_end >= 0 && tab->sel_start != tab->sel_end);
}

static void ed_get_sel_bounds(const editor_tab_t *tab, int *s_min, int *s_max) {
    if (!tab || !s_min || !s_max) return;
    if (tab->sel_start <= tab->sel_end) {
        *s_min = tab->sel_start;
        *s_max = tab->sel_end;
    } else {
        *s_min = tab->sel_end;
        *s_max = tab->sel_start;
    }
}

static void ed_clear_selection(editor_tab_t *tab) {
    if (!tab) return;
    tab->sel_start = -1;
    tab->sel_end = -1;
}

/* Tab Memory Management */
static void editor_free_tab_buffers(editor_tab_t *tab) {
    if (!tab) return;
    if (tab->text && tab->text_pa && tab->text_pages > 0) {
        pmm_free_contiguous(tab->text_pa, tab->text_pages);
        tab->text = NULL;
        tab->text_pa = 0;
        tab->text_pages = 0;
    }
    if (tab->undo_text && tab->undo_pa && tab->undo_pages > 0) {
        pmm_free_contiguous(tab->undo_pa, tab->undo_pages);
        tab->undo_text = NULL;
        tab->undo_pa = 0;
        tab->undo_pages = 0;
    }
    if (tab->redo_text && tab->redo_pa && tab->redo_pages > 0) {
        pmm_free_contiguous(tab->redo_pa, tab->redo_pages);
        tab->redo_text = NULL;
        tab->redo_pa = 0;
        tab->redo_pages = 0;
    }
}

static bool editor_init_tab(editor_tab_t *tab, const char *filepath, const char *title, bool is_untitled) {
    if (!tab) return false;
    kmemset(tab, 0, sizeof(editor_tab_t));

    struct limine_hhdm_response *hhdm = boot_get_hhdm_response();
    if (!hhdm) return false;

    size_t text_pages = (EDITOR_TAB_BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    phys_addr_t text_pa = 0;
    if (pmm_alloc_contiguous(text_pages, &text_pa) != PMM_SUCCESS) return false;

    size_t undo_pages = (EDITOR_UNDO_BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    phys_addr_t undo_pa = 0;
    if (pmm_alloc_contiguous(undo_pages, &undo_pa) != PMM_SUCCESS) {
        pmm_free_contiguous(text_pa, text_pages);
        return false;
    }

    size_t redo_pages = (EDITOR_UNDO_BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    phys_addr_t redo_pa = 0;
    if (pmm_alloc_contiguous(redo_pages, &redo_pa) != PMM_SUCCESS) {
        pmm_free_contiguous(text_pa, text_pages);
        pmm_free_contiguous(undo_pa, undo_pages);
        return false;
    }

    tab->text = (char *)(text_pa + hhdm->offset);
    tab->text_pa = text_pa;
    tab->text_pages = text_pages;
    kmemset(tab->text, 0, text_pages * PAGE_SIZE);

    tab->undo_text = (char *)(undo_pa + hhdm->offset);
    tab->undo_pa = undo_pa;
    tab->undo_pages = undo_pages;
    kmemset(tab->undo_text, 0, undo_pages * PAGE_SIZE);

    tab->redo_text = (char *)(redo_pa + hhdm->offset);
    tab->redo_pa = redo_pa;
    tab->redo_pages = redo_pages;
    kmemset(tab->redo_text, 0, redo_pages * PAGE_SIZE);

    tab->text_len = 0;
    tab->cursor_pos = 0;
    tab->sel_start = -1;
    tab->sel_end = -1;
    tab->dirty = false;
    tab->vscroll = 0;
    tab->hscroll = 0;
    tab->total_lines = 1;
    tab->is_untitled = is_untitled;

    if (filepath) ed_strcpy(tab->filepath, filepath, sizeof(tab->filepath));
    else tab->filepath[0] = '\0';

    if (title) ed_strcpy(tab->title, title, sizeof(tab->title));
    else ed_strcpy(tab->title, "Untitled", sizeof(tab->title));

    return true;
}

static void editor_save_undo(editor_tab_t *tab) {
    if (!tab || !tab->undo_text || !tab->text) return;
    int len = (tab->text_len < EDITOR_UNDO_BUF_SIZE - 1) ? tab->text_len : EDITOR_UNDO_BUF_SIZE - 1;
    for (int i = 0; i < len; i++) {
        tab->undo_text[i] = tab->text[i];
    }
    tab->undo_text[len] = '\0';
    tab->undo_len = len;
    tab->undo_pos = tab->cursor_pos;
    tab->has_undo = true;
    tab->has_redo = false; /* New edit breaks redo chain */
}

static void editor_apply_undo(editor_state_t *ed, editor_tab_t *tab) {
    if (!tab || !tab->has_undo || !tab->undo_text || !tab->text || !tab->redo_text) return;

    /* Save current state to redo */
    int rlen = (tab->text_len < EDITOR_UNDO_BUF_SIZE - 1) ? tab->text_len : EDITOR_UNDO_BUF_SIZE - 1;
    for (int i = 0; i < rlen; i++) tab->redo_text[i] = tab->text[i];
    tab->redo_text[rlen] = '\0';
    tab->redo_len = rlen;
    tab->redo_pos = tab->cursor_pos;
    tab->has_redo = true;

    /* Restore from undo */
    for (int i = 0; i < tab->undo_len; i++) tab->text[i] = tab->undo_text[i];
    tab->text[tab->undo_len] = '\0';
    tab->text_len = tab->undo_len;
    tab->cursor_pos = tab->undo_pos;
    tab->dirty = true;
    tab->total_lines = ed_count_lines(tab->text, tab->text_len);
    tab->has_undo = false;
    ed_clear_selection(tab);

    ed_set_status(ed, "Undo applied.");
}

static void editor_apply_redo(editor_state_t *ed, editor_tab_t *tab) {
    if (!tab || !tab->has_redo || !tab->redo_text || !tab->text || !tab->undo_text) return;

    /* Save current state to undo */
    int ulen = (tab->text_len < EDITOR_UNDO_BUF_SIZE - 1) ? tab->text_len : EDITOR_UNDO_BUF_SIZE - 1;
    for (int i = 0; i < ulen; i++) tab->undo_text[i] = tab->text[i];
    tab->undo_text[ulen] = '\0';
    tab->undo_len = ulen;
    tab->undo_pos = tab->cursor_pos;
    tab->has_undo = true;

    /* Restore from redo */
    for (int i = 0; i < tab->redo_len; i++) tab->text[i] = tab->redo_text[i];
    tab->text[tab->redo_len] = '\0';
    tab->text_len = tab->redo_len;
    tab->cursor_pos = tab->redo_pos;
    tab->dirty = true;
    tab->total_lines = ed_count_lines(tab->text, tab->text_len);
    tab->has_redo = false;
    ed_clear_selection(tab);

    ed_set_status(ed, "Redo applied.");
}

static void ed_delete_selection(editor_state_t *ed, editor_tab_t *tab) {
    if (!tab || !ed_has_selection(tab) || !tab->text) return;
    int s_min, s_max;
    ed_get_sel_bounds(tab, &s_min, &s_max);
    int del_count = s_max - s_min;
    if (del_count <= 0) return;

    editor_save_undo(tab);

    for (int i = s_min; i <= tab->text_len - del_count; i++) {
        tab->text[i] = tab->text[i + del_count];
    }
    tab->text_len -= del_count;
    tab->text[tab->text_len] = '\0';
    tab->cursor_pos = s_min;
    tab->dirty = true;
    tab->total_lines = ed_count_lines(tab->text, tab->text_len);
    ed_clear_selection(tab);
    if (ed) ed_set_status(ed, "Selection deleted.");
}

/* File Browser Engine */
static void ed_browser_free_entries(ed_file_browser_t *fb) {
    if (!fb) return;
    if (fb->entries && fb->entries_pa && fb->entries_pages > 0) {
        pmm_free_contiguous(fb->entries_pa, fb->entries_pages);
        fb->entries = NULL;
        fb->entries_pa = 0;
        fb->entries_pages = 0;
    }
    fb->entry_count = 0;
}

static void ed_browser_ensure_entries(ed_file_browser_t *fb) {
    if (!fb || fb->entries) return;
    struct limine_hhdm_response *hhdm = boot_get_hhdm_response();
    if (!hhdm) return;

    size_t bytes = sizeof(ed_file_entry_t) * EDITOR_MAX_BROWSER_ENTRIES;
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    phys_addr_t pa = 0;
    if (pmm_alloc_contiguous(pages, &pa) == PMM_SUCCESS) {
        fb->entries = (ed_file_entry_t *)(pa + hhdm->offset);
        fb->entries_pa = pa;
        fb->entries_pages = pages;
        kmemset(fb->entries, 0, pages * PAGE_SIZE);
    }
}

static bool ed_browser_filter_match(const char *name, bool is_dir, int filter_idx) {
    if (is_dir) return true;
    if (filter_idx == 0) return true; /* All Files (*.*) */
    if (!name) return false;
    size_t len = ed_strlen(name);
    if (filter_idx == 1) { /* C/C++ (*.c, *.cpp, *.h) */
        if (len >= 2 && name[len - 2] == '.' && (name[len - 1] == 'c' || name[len - 1] == 'h')) return true;
        if (len >= 4 && name[len - 4] == '.' && name[len - 3] == 'c' && name[len - 2] == 'p' && name[len - 1] == 'p') return true;
        return false;
    }
    if (filter_idx == 2) { /* Python (*.py) */
        if (len >= 3 && name[len - 3] == '.' && name[len - 2] == 'p' && name[len - 1] == 'y') return true;
        return false;
    }
    if (filter_idx == 3) { /* Text (*.txt) */
        if (len >= 4 && name[len - 4] == '.' && name[len - 3] == 't' && name[len - 2] == 'x' && name[len - 1] == 't') return true;
        return false;
    }
    return true;
}

static void ed_browser_load_dir(ed_file_browser_t *fb, const char *dir_path) {
    if (!fb) return;
    ed_browser_ensure_entries(fb);
    if (!fb->entries) {
        ed_strcpy(fb->error_msg, "Out of memory", sizeof(fb->error_msg));
        return;
    }

    const char *p = (dir_path && dir_path[0]) ? dir_path : "/data";
    ed_strcpy(fb->current_dir, p, sizeof(fb->current_dir));
    fb->entry_count = 0;
    fb->selected_entry = -1;
    fb->vscroll = 0;
    fb->confirm_overwrite = false;
    fb->error_msg[0] = '\0';

    /* Add ".." if not in root */
    if (fb->current_dir[0] == '/' && fb->current_dir[1] != '\0') {
        ed_file_entry_t *up = &fb->entries[fb->entry_count++];
        ed_strcpy(up->name, "..", sizeof(up->name));
        up->is_dir = true;
        up->size = 0;
    }

    size_t idx = 0;
    vfs_dirent_t dent;
    while (vfs_readdir(fb->current_dir, idx++, &dent) == VFS_OK && fb->entry_count < EDITOR_MAX_BROWSER_ENTRIES) {
        if (ed_strcmp(dent.name, ".") == 0 || ed_strcmp(dent.name, "..") == 0) continue;
        bool is_dir = (dent.type == VFS_NODE_DIR);
        if (!ed_browser_filter_match(dent.name, is_dir, fb->filter_idx)) continue;

        ed_file_entry_t *e = &fb->entries[fb->entry_count++];
        ed_strcpy(e->name, dent.name, sizeof(e->name));
        e->is_dir = is_dir;
        e->size = dent.size;
    }
}

static void ed_browser_open_dialog(editor_state_t *ed, bool is_save, bool run_after) {
    if (!ed) return;
    ed_file_browser_t *fb = &ed->browser;
    fb->is_save_mode = is_save;
    fb->run_after_save = run_after;
    fb->history_cnt = 0;
    fb->history_idx = 0;

    /* Initial directory: /data if exists, else / */
    ed_strcpy(fb->current_dir, "/data", sizeof(fb->current_dir));
    if (is_save && ed->active_tab >= 0 && ed->active_tab < ed->tab_count) {
        editor_tab_t *cur_tab = &ed->tabs[ed->active_tab];
        if (!cur_tab->is_untitled && cur_tab->filepath[0]) {
            ed_extract_basename(fb->filename, sizeof(fb->filename), cur_tab->filepath);
        } else {
            ed_strcpy(fb->filename, "untitled.txt", sizeof(fb->filename));
        }
    } else {
        fb->filename[0] = '\0';
    }
    fb->filename_len = (int)ed_strlen(fb->filename);

    ed_browser_load_dir(fb, fb->current_dir);
    ed->dialog_type = ED_DLG_FILE_BROWSER;
}

static void editor_create_untitled_tab(editor_state_t *ed) {
    if (!ed || ed->tab_count >= EDITOR_MAX_TABS) {
        ed_set_status(ed, "Maximum open tab limit reached (6 tabs).");
        return;
    }
    ed->untitled_counter++;
    char title[32];
    int p = 0;
    const char *pre = "Untitled-";
    for (int i = 0; pre[i]; i++) title[p++] = pre[i];
    if (ed->untitled_counter >= 10) title[p++] = '0' + (ed->untitled_counter / 10);
    title[p++] = '0' + (ed->untitled_counter % 10);
    title[p] = '\0';

    int new_idx = ed->tab_count;
    if (editor_init_tab(&ed->tabs[new_idx], "", title, true)) {
        ed->tab_count++;
        ed->active_tab = new_idx;
        ed_set_status(ed, "New untitled document ready.");
    } else {
        ed_set_status(ed, "Error: out of memory for new tab.");
    }
}

static void editor_open_file_in_tab(editor_state_t *ed, const char *filepath) {
    if (!ed || !filepath || !filepath[0]) return;

    /* Check if already open */
    for (int i = 0; i < ed->tab_count; i++) {
        if (ed_strcmp(ed->tabs[i].filepath, filepath) == 0) {
            ed->active_tab = i;
            ed_set_status(ed, "Switched to already open document.");
            return;
        }
    }

    /* Reuse single empty untitled clean tab if present */
    int target_idx = ed->tab_count;
    if (ed->tab_count == 1 && ed->tabs[0].is_untitled && !ed->tabs[0].dirty && ed->tabs[0].text_len == 0) {
        target_idx = 0;
    } else if (ed->tab_count >= EDITOR_MAX_TABS) {
        ed_set_status(ed, "Cannot open file: maximum tab limit reached.");
        return;
    }

    char title[32];
    ed_extract_basename(title, sizeof(title), filepath);

    editor_tab_t *tab = &ed->tabs[target_idx];
    if (target_idx == ed->tab_count) {
        if (!editor_init_tab(tab, filepath, title, false)) {
            ed_set_status(ed, "Error: out of memory for new tab.");
            return;
        }
        ed->tab_count++;
    } else {
        ed_strcpy(tab->filepath, filepath, sizeof(tab->filepath));
        ed_strcpy(tab->title, title, sizeof(tab->title));
        tab->is_untitled = false;
        tab->cursor_pos = 0;
        tab->vscroll = 0;
        tab->hscroll = 0;
    }

    /* Read file contents from VFS */
    int fd = vfs_open(filepath, VFS_O_RDONLY);
    if (fd >= 0) {
        ssize_t bytes_read = vfs_read(fd, tab->text, EDITOR_TAB_BUF_SIZE - 1);
        if (bytes_read >= 0) {
            tab->text[bytes_read] = '\0';
            tab->text_len = (int)bytes_read;
            tab->cursor_pos = 0;
        }
        vfs_close(fd);
        tab->dirty = false;
        tab->total_lines = ed_count_lines(tab->text, tab->text_len);
        ed->active_tab = target_idx;
        ed_set_status(ed, "Loaded from persistent VFS successfully.");
    } else {
        tab->text[0] = '\0';
        tab->text_len = 0;
        tab->dirty = false;
        tab->total_lines = 1;
        ed->active_tab = target_idx;
        ed_set_status(ed, "New file created at specified path.");
    }
}

static void editor_close_tab(editor_state_t *ed, int tab_idx) {
    if (!ed || tab_idx < 0 || tab_idx >= ed->tab_count) return;

    editor_free_tab_buffers(&ed->tabs[tab_idx]);

    /* Shift remaining tabs */
    for (int i = tab_idx; i + 1 < ed->tab_count; i++) {
        ed->tabs[i] = ed->tabs[i + 1];
    }
    ed->tab_count--;

    if (ed->tab_count == 0) {
        editor_create_untitled_tab(ed);
        ed->active_tab = 0;
    } else {
        if (ed->active_tab >= ed->tab_count) {
            ed->active_tab = ed->tab_count - 1;
        }
    }
    ed_set_status(ed, "Tab closed.");
}

static void editor_save_active_tab(editor_state_t *ed) {
    if (!ed || ed->active_tab < 0 || ed->active_tab >= ed->tab_count) return;
    editor_tab_t *tab = &ed->tabs[ed->active_tab];

    if (tab->is_untitled || !tab->filepath[0]) {
        ed_browser_open_dialog(ed, true, false);
        return;
    }

    int fd = vfs_open(tab->filepath, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd >= 0) {
        ssize_t written = vfs_write(fd, tab->text, tab->text_len);
        vfs_close(fd);
        if (written == (ssize_t)tab->text_len) {
            tab->dirty = false;
            ed_set_status(ed, "Saved to persistent VFS successfully.");
        } else {
            ed_set_status(ed, "Warning: partial write to file.");
        }
    } else {
        ed_set_status(ed, "Error: failed to open file for writing on VFS.");
    }
}

static void editor_save_as_commit(editor_state_t *ed, const char *new_path, bool run_after) {
    if (!ed || !new_path || !new_path[0] || ed->active_tab < 0 || ed->active_tab >= ed->tab_count) return;
    editor_tab_t *tab = &ed->tabs[ed->active_tab];

    int fd = vfs_open(new_path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd >= 0) {
        ssize_t written = vfs_write(fd, tab->text, tab->text_len);
        vfs_close(fd);
        if (written == (ssize_t)tab->text_len) {
            ed_strcpy(tab->filepath, new_path, sizeof(tab->filepath));
            ed_extract_basename(tab->title, sizeof(tab->title), new_path);
            tab->is_untitled = false;
            tab->dirty = false;
            ed_set_status(ed, "Saved file to persistent VFS successfully.");

            if (run_after) {
                char cmd[160];
                int cp = 0;
                const char *prefix = "run ";
                for (int k = 0; prefix[k]; k++) cmd[cp++] = prefix[k];
                for (int k = 0; tab->filepath[k] && cp < 150; k++) cmd[cp++] = tab->filepath[k];
                cmd[cp] = '\0';
                app_terminal_launch_with_command(cmd);
                ed_set_status(ed, "Saved and executed in AstraOS Terminal.");
            }
        } else {
            ed_set_status(ed, "Warning: partial write occurred during Save As.");
        }
    } else {
        ed_set_status(ed, "Error: failed to create destination file on VFS.");
    }
}

static void editor_trigger_run(editor_state_t *ed) {
    if (!ed || ed->active_tab < 0 || ed->active_tab >= ed->tab_count) return;
    editor_tab_t *tab = &ed->tabs[ed->active_tab];

    if (tab->is_untitled || tab->dirty) {
        /* Priority 2: Never automatically save without user prompt */
        ed->dialog_type = ED_DLG_CONFIRM_RUN;
        return;
    }

    char cmd[160];
    int cp = 0;
    const char *prefix = "run ";
    for (int k = 0; prefix[k]; k++) cmd[cp++] = prefix[k];
    for (int k = 0; tab->filepath[k] && cp < 150; k++) cmd[cp++] = tab->filepath[k];
    cmd[cp] = '\0';

    app_terminal_launch_with_command(cmd);
    ed_set_status(ed, "Executing program in AstraOS Terminal...");
}

/* UI Paint */
static void editor_on_paint(wm_window_t *win, gfx_surface_t *s) {
    if (!win || !s) return;
    editor_state_t *ed = (editor_state_t *)win->user_data;
    if (!ed) return;

    editor_tab_t *tab = (ed->active_tab >= 0 && ed->active_tab < ed->tab_count) ? &ed->tabs[ed->active_tab] : NULL;

    /* Base Background */
    gfx_fill_rect(s, 0, 0, s->width, s->height, 0x111827);

    /* 1. Top Header Bar (Action Buttons & Syntax Badge) (32px) */
    gfx_fill_rect(s, 0, 0, s->width, 32, 0x0F172A);
    gfx_draw_line(s, 0, 32, s->width, 32, 0x1E293B);

    /* [+ New] Button */
    gfx_fill_rounded_rect(s, 8, 5, 56, 22, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 8, 5, 56, 22, 4, 0x334155);
    gfx_draw_string(s, 16, 8, "+ New", 0xF8FAFC, 0, true);

    /* [Open] Button */
    gfx_fill_rounded_rect(s, 68, 5, 52, 22, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 68, 5, 52, 22, 4, 0x334155);
    gfx_draw_string(s, 76, 8, "Open", 0xF8FAFC, 0, true);

    /* [Save] Button */
    u32 save_bg = (tab && tab->dirty) ? 0x065F46 : 0x1E40AF;
    u32 save_bd = (tab && tab->dirty) ? 0x10B981 : 0x3B82F6;
    gfx_fill_rounded_rect(s, 124, 5, 52, 22, 4, save_bg);
    gfx_draw_rounded_rect(s, 124, 5, 52, 22, 4, save_bd);
    gfx_draw_string(s, 132, 8, "Save", 0xFFFFFF, 0, true);

    /* [Save As] Button */
    gfx_fill_rounded_rect(s, 180, 5, 68, 22, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 180, 5, 68, 22, 4, 0x334155);
    gfx_draw_string(s, 188, 8, "Save As", 0xF8FAFC, 0, true);

    /* [Run >] Button */
    gfx_fill_rounded_rect(s, 252, 5, 58, 22, 4, 0x166534);
    gfx_draw_rounded_rect(s, 252, 5, 58, 22, 4, 0x22C55E);
    gfx_draw_string(s, 260, 8, "Run >", 0xFFFFFF, 0, true);

    /* [Find] Button */
    gfx_fill_rounded_rect(s, 314, 5, 52, 22, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 314, 5, 52, 22, 4, 0x334155);
    gfx_draw_string(s, 322, 8, "Find", 0x94A3B8, 0, true);

    /* Language / Syntax Badge */
    const char *badge_text = "[Text Document]";
    u32 badge_color = 0x94A3B8;
    if (tab && tab->filepath[0]) {
        size_t f_len = ed_strlen(tab->filepath);
        if (f_len >= 2 && tab->filepath[f_len - 2] == '.' && tab->filepath[f_len - 1] == 'c') {
            badge_text = "[C Source]";
            badge_color = 0x38BDF8;
        } else if (f_len >= 4 && tab->filepath[f_len - 4] == '.' && tab->filepath[f_len - 3] == 'c' &&
                   tab->filepath[f_len - 2] == 'p' && tab->filepath[f_len - 1] == 'p') {
            badge_text = "[C++ Source]";
            badge_color = 0x818CF8;
        } else if (f_len >= 3 && tab->filepath[f_len - 3] == '.' && tab->filepath[f_len - 2] == 'p' &&
                   tab->filepath[f_len - 1] == 'y') {
            badge_text = "[Python 3.11]";
            badge_color = 0xFBBF24;
        } else if (f_len >= 3 && tab->filepath[f_len - 3] == '.' && tab->filepath[f_len - 2] == 's' &&
                   tab->filepath[f_len - 1] == 'h') {
            badge_text = "[Shell Script]";
            badge_color = 0x34D399;
        } else if (f_len >= 2 && tab->filepath[f_len - 2] == '.' && tab->filepath[f_len - 1] == 'h') {
            badge_text = "[C Header]";
            badge_color = 0x22D3EE;
        }
    }
    int badge_w = (int)ed_strlen(badge_text) * FONT_WIDTH;
    gfx_draw_string(s, s->width - badge_w - 14, 8, badge_text, badge_color, 0, true);

    /* 2. Tab Strip (y = 33 .. 60, height 28px) */
    gfx_fill_rect(s, 0, 33, s->width, 28, 0x0B0F19);
    gfx_draw_line(s, 0, 61, s->width, 61, 0x1E293B);

    int tab_w = 120;
    for (int i = 0; i < ed->tab_count; i++) {
        editor_tab_t *t = &ed->tabs[i];
        int tx = 8 + i * (tab_w + 4);
        if (tx + tab_w > s->width - 40) break;

        bool is_act = (i == ed->active_tab);
        u32 t_bg = is_act ? 0x1E293B : 0x0F172A;
        u32 t_bd = is_act ? 0x38BDF8 : 0x1E293B;

        gfx_fill_rounded_rect(s, tx, 36, tab_w, 24, 4, t_bg);
        gfx_draw_rounded_rect(s, tx, 36, tab_w, 24, 4, t_bd);
        if (is_act) {
            gfx_draw_line(s, tx + 4, 36, tx + tab_w - 4, 36, 0x38BDF8);
        }

        /* Status Dot */
        u32 dot_col = t->dirty ? 0xF59E0B : 0x10B981;
        gfx_fill_rounded_rect(s, tx + 6, 45, 6, 6, 3, dot_col);

        /* Tab Title */
        char short_title[16];
        int stp = 0;
        for (int k = 0; t->title[k] && stp < 9; k++) short_title[stp++] = t->title[k];
        short_title[stp] = '\0';
        gfx_draw_string(s, tx + 16, 40, short_title, is_act ? 0xF8FAFC : 0x94A3B8, 0, true);

        /* Close Button [x] */
        gfx_draw_string(s, tx + tab_w - 16, 40, "x", 0x64748B, 0, true);
    }

    /* [+] Tab Button */
    if (ed->tab_count < EDITOR_MAX_TABS) {
        int add_x = 8 + ed->tab_count * (tab_w + 4);
        if (add_x + 28 < s->width - 10) {
            gfx_fill_rounded_rect(s, add_x, 36, 24, 24, 4, 0x1E293B);
            gfx_draw_rounded_rect(s, add_x, 36, 24, 24, 4, 0x334155);
            gfx_draw_string(s, add_x + 8, 40, "+", 0x38BDF8, 0, true);
        }
    }

    /* 3. Editor Canvas: Line Number Gutter & Text Area */
    int gutter_w = 42;
    int canvas_top = 62;
    int sb_y = s->height - 24;
    int canvas_h = sb_y - canvas_top;

    gfx_fill_rect(s, 0, canvas_top, gutter_w, canvas_h, 0x0B0F19);
    gfx_draw_line(s, gutter_w, canvas_top, gutter_w, sb_y, 0x1E293B);

    if (!tab) return;

    /* Calculate cursor line and column */
    int cur_line = 1;
    int cur_col = 1;
    for (int i = 0; i < tab->cursor_pos && i < tab->text_len; i++) {
        if (tab->text[i] == '\n') {
            cur_line++;
            cur_col = 1;
        } else {
            cur_col++;
        }
    }

    int visible_lines = canvas_h / 18;
    if (visible_lines < 1) visible_lines = 1;
    int text_max_x = s->width - 16;
    int text_x = gutter_w + 10;
    int visible_cols = (text_max_x - text_x) / FONT_WIDTH;
    if (visible_cols < 10) visible_cols = 10;

    /* Auto-scroll vertical viewport to keep cursor visible */
    if (cur_line - 1 < tab->vscroll) {
        tab->vscroll = cur_line - 1;
    }
    if (cur_line - 1 >= tab->vscroll + visible_lines) {
        tab->vscroll = cur_line - visible_lines;
    }
    if (tab->vscroll < 0) tab->vscroll = 0;

    /* Auto-scroll horizontal viewport to keep cursor visible */
    if (cur_col - 1 < tab->hscroll) {
        tab->hscroll = cur_col - 1;
    }
    if (cur_col - 1 >= tab->hscroll + visible_cols) {
        tab->hscroll = cur_col - visible_cols;
    }
    if (tab->hscroll < 0) tab->hscroll = 0;

    /* Find start index in tab->text for line tab->vscroll */
    int start_pos = 0;
    int cur_scan_line = 0;
    while (start_pos < tab->text_len && cur_scan_line < tab->vscroll) {
        if (tab->text[start_pos] == '\n') cur_scan_line++;
        start_pos++;
    }

    int cur_y = canvas_top + 6;
    int line_col = 1;
    int line_num = tab->vscroll + 1;

    /* Draw first visible line number in gutter */
    char lbuf[8];
    int lp = 0;
    if (line_num < 10) { lbuf[lp++] = ' '; lbuf[lp++] = ' '; lbuf[lp++] = '0' + line_num; }
    else if (line_num < 100) { lbuf[lp++] = ' '; lbuf[lp++] = '0' + (line_num / 10); lbuf[lp++] = '0' + (line_num % 10); }
    else { lbuf[lp++] = '0' + (line_num / 100); lbuf[lp++] = '0' + ((line_num / 10) % 10); lbuf[lp++] = '0' + (line_num % 10); }
    lbuf[lp] = '\0';
    u32 g_col = (line_num == cur_line) ? 0x38BDF8 : 0x475569;
    gfx_draw_string(s, 8, cur_y, lbuf, g_col, 0, true);

    int s_min = -1, s_max = -1;
    bool has_sel = ed_has_selection(tab);
    if (has_sel) ed_get_sel_bounds(tab, &s_min, &s_max);

    /* Render text characters and selection */
    for (int i = start_pos; i <= tab->text_len; i++) {
        int screen_x = text_x + (line_col - 1 - tab->hscroll) * FONT_WIDTH;

        if (i == tab->cursor_pos) {
            /* Active glowing caret */
            if (screen_x >= text_x && screen_x <= text_max_x) {
                gfx_fill_rect(s, screen_x, cur_y - 1, 2, 16, 0x38BDF8);
            }
        }
        if (i == tab->text_len) break;

        char c = tab->text[i];
        if (c == '\n') {
            line_num++;
            line_col = 1;
            cur_y += 18;
            if (cur_y + 18 >= sb_y) break;

            /* Render next line number in gutter */
            lp = 0;
            if (line_num < 10) { lbuf[lp++] = ' '; lbuf[lp++] = ' '; lbuf[lp++] = '0' + line_num; }
            else if (line_num < 100) { lbuf[lp++] = ' '; lbuf[lp++] = '0' + (line_num / 10); lbuf[lp++] = '0' + (line_num % 10); }
            else { lbuf[lp++] = '0' + (line_num / 100); lbuf[lp++] = '0' + ((line_num / 10) % 10); lbuf[lp++] = '0' + (line_num % 10); }
            lbuf[lp] = '\0';
            g_col = (line_num == cur_line) ? 0x38BDF8 : 0x475569;
            gfx_draw_string(s, 8, cur_y, lbuf, g_col, 0, true);
        } else if (c == '\r') {
            line_col = 1;
        } else if (c == '\t') {
            line_col += 4;
        } else {
            bool in_sel = (has_sel && i >= s_min && i < s_max);
            if (screen_x >= text_x && screen_x + FONT_WIDTH <= text_max_x) {
                if (in_sel) {
                    gfx_fill_rect(s, screen_x, cur_y - 1, FONT_WIDTH, 16, 0x1D4ED8);
                    gfx_draw_char(s, screen_x, cur_y, c, 0xFFFFFF, 0, true);
                } else {
                    gfx_draw_char(s, screen_x, cur_y, c, 0xF1F5F9, 0, true);
                }
            }
            line_col++;
        }
    }

    /* 4. Canvas Vertical Scrollbar */
    int sb_track_x = s->width - 14;
    int sb_track_w = 10;
    scrollview_init(&tab->scrollview, tab->total_lines, visible_lines, sb_track_x, canvas_top, sb_track_w, canvas_h);
    tab->scrollview.scroll_offset = tab->vscroll;
    scrollview_render(&tab->scrollview, s, 0x0B0F19, 0x334155);

    /* 5. Bottom Status Bar (24px) */
    gfx_fill_rect(s, 0, sb_y, s->width, 24, 0x0B0F19);
    gfx_draw_line(s, 0, sb_y, s->width, sb_y, 0x1E293B);

    /* Left status message */
    gfx_draw_string(s, 10, sb_y + 4, ed->status_msg, 0x94A3B8, 0, true);

    /* Right telemetry: Ln X, Col Y | UTF-8 | 4 Spaces */
    char r_stat[64];
    int rp = 0;
    const char *lp_str = "Ln ";
    for (int k = 0; lp_str[k]; k++) r_stat[rp++] = lp_str[k];
    if (cur_line >= 100) r_stat[rp++] = '0' + (cur_line / 100);
    if (cur_line >= 10)  r_stat[rp++] = '0' + ((cur_line / 10) % 10);
    r_stat[rp++] = '0' + (cur_line % 10);

    const char *cp_str = ", Col ";
    for (int k = 0; cp_str[k]; k++) r_stat[rp++] = cp_str[k];
    if (cur_col >= 100) r_stat[rp++] = '0' + (cur_col / 100);
    if (cur_col >= 10)  r_stat[rp++] = '0' + ((cur_col / 10) % 10);
    r_stat[rp++] = '0' + (cur_col % 10);

    const char *tail = " | UTF-8 | 4 Spaces";
    for (int k = 0; tail[k]; k++) r_stat[rp++] = tail[k];
    r_stat[rp] = '\0';
    int rw = rp * FONT_WIDTH;
    gfx_draw_string(s, s->width - rw - 14, sb_y + 4, r_stat, 0x64748B, 0, true);

    /* 6. Modal Dialogs */
    if (ed->dialog_type != ED_DLG_NONE) {
        /* Backdrop */
        for (int y = 0; y < s->height; y += 2) {
            for (int x = 0; x < s->width; x += 2) {
                gfx_put_pixel(s, x, y, 0x000000);
            }
        }

        /* Priority 3: Graphical File Browser Dialog */
        if (ed->dialog_type == ED_DLG_FILE_BROWSER) {
            ed_file_browser_t *fb = &ed->browser;
            int dw = 520;
            int dh = 320;
            int dx = (s->width - dw) / 2;
            int dy = (s->height - dh) / 2;

            gfx_fill_rounded_rect(s, dx, dy, dw, dh, 6, 0x1E293B);
            gfx_draw_rounded_rect(s, dx, dy, dw, dh, 6, 0x38BDF8);

            /* Dialog Header (28px) */
            gfx_fill_rounded_rect(s, dx, dy, dw, 28, 6, 0x0F172A);
            const char *title = fb->is_save_mode ? "Save Document As (Persistent Storage)" : "Open Document";
            gfx_draw_string(s, dx + 12, dy + 6, title, 0x38BDF8, 0, true);
            gfx_draw_string(s, dx + dw - 20, dy + 6, "x", 0x64748B, 0, true);

            /* Navigation Bar (dy + 32) */
            /* [< Back] */
            gfx_fill_rounded_rect(s, dx + 10, dy + 32, 26, 22, 4, 0x0F172A);
            gfx_draw_rounded_rect(s, dx + 10, dy + 32, 26, 22, 4, 0x334155);
            gfx_draw_string(s, dx + 18, dy + 35, "<", 0x38BDF8, 0, true);

            /* [Forward >] */
            gfx_fill_rounded_rect(s, dx + 40, dy + 32, 26, 22, 4, 0x0F172A);
            gfx_draw_rounded_rect(s, dx + 40, dy + 32, 26, 22, 4, 0x334155);
            gfx_draw_string(s, dx + 48, dy + 35, ">", 0x38BDF8, 0, true);

            /* [^ Up] */
            gfx_fill_rounded_rect(s, dx + 70, dy + 32, 26, 22, 4, 0x0F172A);
            gfx_draw_rounded_rect(s, dx + 70, dy + 32, 26, 22, 4, 0x334155);
            gfx_draw_string(s, dx + 78, dy + 35, "^", 0x38BDF8, 0, true);

            /* [/data] Quick persistent pill */
            gfx_fill_rounded_rect(s, dx + 102, dy + 32, 54, 22, 4, 0x065F46);
            gfx_draw_rounded_rect(s, dx + 102, dy + 32, 54, 22, 4, 0x10B981);
            gfx_draw_string(s, dx + 108, dy + 35, "/data", 0xFFFFFF, 0, true);

            /* [/] Quick root pill */
            gfx_fill_rounded_rect(s, dx + 160, dy + 32, 28, 22, 4, 0x0F172A);
            gfx_draw_rounded_rect(s, dx + 160, dy + 32, 28, 22, 4, 0x334155);
            gfx_draw_string(s, dx + 170, dy + 35, "/", 0x38BDF8, 0, true);

            /* Path Display */
            gfx_fill_rounded_rect(s, dx + 192, dy + 32, dw - 202, 22, 4, 0x0B0F19);
            gfx_draw_rounded_rect(s, dx + 192, dy + 32, dw - 202, 22, 4, 0x1E293B);
            char p_show[64];
            int psp = 0;
            const char *loc = "Dir: ";
            for (int k = 0; loc[k]; k++) p_show[psp++] = loc[k];
            for (int k = 0; fb->current_dir[k] && psp < 60; k++) p_show[psp++] = fb->current_dir[k];
            p_show[psp] = '\0';
            gfx_draw_string(s, dx + 198, dy + 35, p_show, 0xF8FAFC, 0, true);

            /* Directory Listing Table (dy + 58, height 144) */
            int tbl_x = dx + 10;
            int tbl_y = dy + 58;
            int tbl_w = dw - 20;
            int tbl_h = 144;
            gfx_fill_rect(s, tbl_x, tbl_y, tbl_w, tbl_h, 0x0B0F19);
            gfx_draw_rect(s, tbl_x, tbl_y, tbl_w, tbl_h, 0x334155);

            int vis_rows = 6;
            for (int r = 0; r < vis_rows; r++) {
                int e_idx = fb->vscroll + r;
                if (e_idx >= fb->entry_count) break;
                ed_file_entry_t *ent = &fb->entries[e_idx];
                int ry = tbl_y + r * 24;

                bool is_sel = (e_idx == fb->selected_entry);
                if (is_sel) {
                    gfx_fill_rect(s, tbl_x + 1, ry + 1, tbl_w - 2, 22, 0x1E40AF);
                } else if (r % 2 == 1) {
                    gfx_fill_rect(s, tbl_x + 1, ry + 1, tbl_w - 2, 22, 0x0F172A);
                }

                /* Type Badge: [D] for Dir, [F] for File */
                if (ent->is_dir) {
                    gfx_draw_string(s, tbl_x + 6, ry + 4, "[D]", 0xF59E0B, 0, true);
                } else {
                    gfx_draw_string(s, tbl_x + 6, ry + 4, "[F]", 0x38BDF8, 0, true);
                }

                /* Name */
                gfx_draw_string(s, tbl_x + 36, ry + 4, ent->name, is_sel ? 0xFFFFFF : 0xF8FAFC, 0, true);

                /* Size */
                if (!ent->is_dir) {
                    char sz_buf[16];
                    int szp = 0;
                    size_t b = ent->size;
                    if (b < 1024) {
                        if (b >= 100) sz_buf[szp++] = '0' + (b / 100) % 10;
                        if (b >= 10)  sz_buf[szp++] = '0' + (b / 10) % 10;
                        sz_buf[szp++] = '0' + (b % 10);
                        sz_buf[szp++] = ' '; sz_buf[szp++] = 'B';
                    } else {
                        size_t kb = b / 1024;
                        if (kb >= 100) sz_buf[szp++] = '0' + (kb / 100) % 10;
                        if (kb >= 10)  sz_buf[szp++] = '0' + (kb / 10) % 10;
                        sz_buf[szp++] = '0' + (kb % 10);
                        sz_buf[szp++] = ' '; sz_buf[szp++] = 'K'; sz_buf[szp++] = 'B';
                    }
                    sz_buf[szp] = '\0';
                    gfx_draw_string(s, tbl_x + tbl_w - 64, ry + 4, sz_buf, 0x94A3B8, 0, true);
                } else {
                    gfx_draw_string(s, tbl_x + tbl_w - 64, ry + 4, "--", 0x64748B, 0, true);
                }
            }

            /* Scrollbar track */
            if (fb->entry_count > vis_rows) {
                int st_x = tbl_x + tbl_w - 8;
                gfx_fill_rect(s, st_x, tbl_y, 8, tbl_h, 0x1E293B);
                int th_h = (vis_rows * tbl_h) / fb->entry_count;
                if (th_h < 12) th_h = 12;
                int max_scroll = fb->entry_count - vis_rows;
                int th_y = tbl_y + (fb->vscroll * (tbl_h - th_h)) / (max_scroll > 0 ? max_scroll : 1);
                gfx_fill_rect(s, st_x + 1, th_y, 6, th_h, 0x38BDF8);
            }

            /* Bottom Bar Controls (File Name input & Type Filter) */
            int ctrl_y = dy + 210;
            gfx_draw_string(s, dx + 12, ctrl_y + 4, "File Name:", 0x94A3B8, 0, true);
            gfx_fill_rounded_rect(s, dx + 84, ctrl_y, 230, 24, 4, 0x0B0F19);
            gfx_draw_rounded_rect(s, dx + 84, ctrl_y, 230, 24, 4, 0x3B82F6);
            gfx_draw_string(s, dx + 90, ctrl_y + 4, fb->filename, 0xF8FAFC, 0, true);
            /* Caret */
            int fn_cx = dx + 90 + fb->filename_len * FONT_WIDTH;
            if (fn_cx < dx + 300) gfx_fill_rect(s, fn_cx, ctrl_y + 3, 2, 18, 0x38BDF8);

            /* Filter Selector */
            gfx_draw_string(s, dx + 322, ctrl_y + 4, "Type:", 0x94A3B8, 0, true);
            const char *filt_names[] = { "All (*.*)", "C/C++", "Python", "Text" };
            gfx_fill_rounded_rect(s, dx + 360, ctrl_y, 148, 24, 4, 0x0F172A);
            gfx_draw_rounded_rect(s, dx + 360, ctrl_y, 148, 24, 4, 0x334155);
            gfx_draw_string(s, dx + 368, ctrl_y + 4, filt_names[fb->filter_idx % 4], 0x38BDF8, 0, true);

            /* Action Buttons & Overwrite Prompt */
            int act_y = dy + dh - 36;
            if (fb->confirm_overwrite) {
                gfx_draw_string(s, dx + 14, act_y + 4, "File exists! Overwrite?", 0xF59E0B, 0, true);
                /* [Overwrite] */
                gfx_fill_rounded_rect(s, dx + dw - 176, act_y, 82, 26, 4, 0x881337);
                gfx_draw_rounded_rect(s, dx + dw - 176, act_y, 82, 26, 4, 0xF43F5E);
                gfx_draw_string(s, dx + dw - 168, act_y + 5, "Overwrite", 0xFFFFFF, 0, true);
            } else {
                if (fb->error_msg[0]) {
                    gfx_draw_string(s, dx + 14, act_y + 4, fb->error_msg, 0xF43F5E, 0, true);
                }
                /* [Open] / [Save] */
                gfx_fill_rounded_rect(s, dx + dw - 176, act_y, 82, 26, 4, 0x1E40AF);
                gfx_draw_rounded_rect(s, dx + dw - 176, act_y, 82, 26, 4, 0x3B82F6);
                gfx_draw_string(s, dx + dw - 156, act_y + 5, fb->is_save_mode ? "Save" : "Open", 0xFFFFFF, 0, true);
            }

            /* [Cancel] */
            gfx_fill_rounded_rect(s, dx + dw - 86, act_y, 76, 26, 4, 0x334155);
            gfx_draw_rounded_rect(s, dx + dw - 86, act_y, 76, 26, 4, 0x64748B);
            gfx_draw_string(s, dx + dw - 72, act_y + 5, "Cancel", 0xFFFFFF, 0, true);
        } else if (ed->dialog_type == ED_DLG_CONFIRM_CLOSE) {
            int dw = 380;
            int dh = 150;
            int dx = (s->width - dw) / 2;
            int dy = (s->height - dh) / 2;

            gfx_fill_rounded_rect(s, dx, dy, dw, dh, 6, 0x1E293B);
            gfx_draw_rounded_rect(s, dx, dy, dw, dh, 6, 0x38BDF8);

            gfx_fill_rounded_rect(s, dx, dy, dw, 28, 6, 0x0F172A);
            gfx_draw_string(s, dx + 12, dy + 6, "Unsaved Changes", 0x38BDF8, 0, true);

            gfx_draw_string(s, dx + 16, dy + 42, "Document has unsaved changes.", 0xF8FAFC, 0, true);
            gfx_draw_string(s, dx + 16, dy + 62, "Do you want to save before closing?", 0x94A3B8, 0, true);

            int btn_y = dy + dh - 36;
            /* Save */
            gfx_fill_rounded_rect(s, dx + 20, btn_y, 80, 24, 4, 0x065F46);
            gfx_draw_rounded_rect(s, dx + 20, btn_y, 80, 24, 4, 0x10B981);
            gfx_draw_string(s, dx + 42, btn_y + 4, "Save", 0xFFFFFF, 0, true);

            /* Don't Save */
            gfx_fill_rounded_rect(s, dx + 110, btn_y, 110, 24, 4, 0x881337);
            gfx_draw_rounded_rect(s, dx + 110, btn_y, 110, 24, 4, 0xF43F5E);
            gfx_draw_string(s, dx + 120, btn_y + 4, "Don't Save", 0xFFFFFF, 0, true);

            /* Cancel */
            gfx_fill_rounded_rect(s, dx + 230, btn_y, 80, 24, 4, 0x334155);
            gfx_draw_rounded_rect(s, dx + 230, btn_y, 80, 24, 4, 0x64748B);
            gfx_draw_string(s, dx + 248, btn_y + 4, "Cancel", 0xFFFFFF, 0, true);
        } else if (ed->dialog_type == ED_DLG_CONFIRM_RUN) {
            int dw = 380;
            int dh = 150;
            int dx = (s->width - dw) / 2;
            int dy = (s->height - dh) / 2;

            gfx_fill_rounded_rect(s, dx, dy, dw, dh, 6, 0x1E293B);
            gfx_draw_rounded_rect(s, dx, dy, dw, dh, 6, 0x22C55E);

            gfx_fill_rounded_rect(s, dx, dy, dw, 28, 6, 0x0F172A);
            gfx_draw_string(s, dx + 12, dy + 6, "Run Program Confirmation", 0x22C55E, 0, true);

            gfx_draw_string(s, dx + 16, dy + 42, "Document has unsaved modifications.", 0xF8FAFC, 0, true);
            gfx_draw_string(s, dx + 16, dy + 62, "Save document before executing in Terminal?", 0x94A3B8, 0, true);

            int btn_y = dy + dh - 36;
            /* Save & Run */
            gfx_fill_rounded_rect(s, dx + 16, btn_y, 100, 24, 4, 0x065F46);
            gfx_draw_rounded_rect(s, dx + 16, btn_y, 100, 24, 4, 0x10B981);
            gfx_draw_string(s, dx + 24, btn_y + 4, "Save & Run", 0xFFFFFF, 0, true);

            /* Run Unsaved Copy */
            gfx_fill_rounded_rect(s, dx + 124, btn_y, 140, 24, 4, 0x1E40AF);
            gfx_draw_rounded_rect(s, dx + 124, btn_y, 140, 24, 4, 0x3B82F6);
            gfx_draw_string(s, dx + 132, btn_y + 4, "Run Temp Copy", 0xFFFFFF, 0, true);

            /* Cancel */
            gfx_fill_rounded_rect(s, dx + 272, btn_y, 76, 24, 4, 0x334155);
            gfx_draw_rounded_rect(s, dx + 272, btn_y, 76, 24, 4, 0x64748B);
            gfx_draw_string(s, dx + 288, btn_y + 4, "Cancel", 0xFFFFFF, 0, true);
        } else if (ed->dialog_type == ED_DLG_FIND || ed->dialog_type == ED_DLG_REPLACE) {
            int dw = 380;
            int dh = (ed->dialog_type == ED_DLG_REPLACE) ? 170 : 130;
            int dx = (s->width - dw) / 2;
            int dy = (s->height - dh) / 2;

            gfx_fill_rounded_rect(s, dx, dy, dw, dh, 6, 0x1E293B);
            gfx_draw_rounded_rect(s, dx, dy, dw, dh, 6, 0x38BDF8);

            gfx_fill_rounded_rect(s, dx, dy, dw, 28, 6, 0x0F172A);
            const char *title = (ed->dialog_type == ED_DLG_REPLACE) ? "Find & Replace" : "Find in Document";
            gfx_draw_string(s, dx + 12, dy + 6, title, 0x38BDF8, 0, true);

            /* Find input */
            gfx_draw_string(s, dx + 16, dy + 38, "Find:", 0x94A3B8, 0, true);
            gfx_fill_rounded_rect(s, dx + 80, dy + 34, dw - 96, 24, 4, 0x0B0F19);
            gfx_draw_rounded_rect(s, dx + 80, dy + 34, dw - 96, 24, 4, (ed->fr.active_field == 0) ? 0x38BDF8 : 0x334155);
            gfx_draw_string(s, dx + 86, dy + 38, ed->fr.find_str, 0xF8FAFC, 0, true);
            if (ed->fr.active_field == 0) {
                int cx = dx + 86 + ed->fr.find_len * FONT_WIDTH;
                gfx_fill_rect(s, cx, dy + 37, 2, 18, 0x38BDF8);
            }

            if (ed->dialog_type == ED_DLG_REPLACE) {
                gfx_draw_string(s, dx + 16, dy + 68, "Replace:", 0x94A3B8, 0, true);
                gfx_fill_rounded_rect(s, dx + 80, dy + 64, dw - 96, 24, 4, 0x0B0F19);
                gfx_draw_rounded_rect(s, dx + 80, dy + 64, dw - 96, 24, 4, (ed->fr.active_field == 1) ? 0x38BDF8 : 0x334155);
                gfx_draw_string(s, dx + 86, dy + 68, ed->fr.replace_str, 0xF8FAFC, 0, true);
                if (ed->fr.active_field == 1) {
                    int cx = dx + 86 + ed->fr.replace_len * FONT_WIDTH;
                    gfx_fill_rect(s, cx, dy + 67, 2, 18, 0x38BDF8);
                }
            }

            /* Buttons */
            int btn_y = dy + dh - 36;
            gfx_fill_rounded_rect(s, dx + dw - 180, btn_y, 82, 24, 4, 0x1E40AF);
            gfx_draw_rounded_rect(s, dx + dw - 180, btn_y, 82, 24, 4, 0x3B82F6);
            gfx_draw_string(s, dx + dw - 164, btn_y + 4, "Next", 0xFFFFFF, 0, true);

            if (ed->dialog_type == ED_DLG_REPLACE) {
                gfx_fill_rounded_rect(s, dx + dw - 270, btn_y, 82, 24, 4, 0x065F46);
                gfx_draw_rounded_rect(s, dx + dw - 270, btn_y, 82, 24, 4, 0x10B981);
                gfx_draw_string(s, dx + dw - 258, btn_y + 4, "Replace", 0xFFFFFF, 0, true);
            }

            gfx_fill_rounded_rect(s, dx + dw - 90, btn_y, 76, 24, 4, 0x334155);
            gfx_draw_rounded_rect(s, dx + dw - 90, btn_y, 76, 24, 4, 0x64748B);
            gfx_draw_string(s, dx + dw - 76, btn_y + 4, "Close", 0xFFFFFF, 0, true);
        }
    }
}

/* Event Handler */
static void editor_on_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return;
    editor_state_t *ed = (editor_state_t *)win->user_data;
    if (!ed) return;

    editor_tab_t *tab = (ed->active_tab >= 0 && ed->active_tab < ed->tab_count) ? &ed->tabs[ed->active_tab] : NULL;

    /* A. Modal Dialog Input Handling */
    if (ed->dialog_type != ED_DLG_NONE) {
        if (ed->dialog_type == ED_DLG_FILE_BROWSER) {
            ed_file_browser_t *fb = &ed->browser;
            int dw = 520;
            int dh = 320;
            int dx = (win->client_w - dw) / 2;
            int dy = (win->client_h - dh) / 2;
            int tbl_x = dx + 10;
            int tbl_y = dy + 58;
            int tbl_w = dw - 20;
            int tbl_h = 144;
            int vis_rows = 6;
            int act_y = dy + dh - 36;

            if (ev->type == INPUT_EVENT_KEY_DOWN) {
                if (ev->key.keycode == KEY_ESC) {
                    ed_browser_free_entries(fb);
                    ed->dialog_type = ED_DLG_NONE;
                    return;
                }
                if (ev->key.keycode == KEY_ENTER) {
                    if (fb->confirm_overwrite) {
                        /* Confirmed overwrite */
                        char full_p[128];
                        ed_path_join(full_p, sizeof(full_p), fb->current_dir, fb->filename);
                        editor_save_as_commit(ed, full_p, fb->run_after_save);
                        ed_browser_free_entries(fb);
                        ed->dialog_type = ED_DLG_NONE;
                        return;
                    }
                    if (fb->is_save_mode) {
                        if (fb->filename_len > 0) {
                            char full_p[128];
                            ed_path_join(full_p, sizeof(full_p), fb->current_dir, fb->filename);
                            vfs_node_t *ex = NULL;
                            if (vfs_lookup(full_p, &ex) == VFS_OK && ex) {
                                fb->confirm_overwrite = true;
                                return;
                            }
                            editor_save_as_commit(ed, full_p, fb->run_after_save);
                            ed_browser_free_entries(fb);
                            ed->dialog_type = ED_DLG_NONE;
                        }
                    } else {
                        /* Open mode */
                        if (fb->selected_entry >= 0 && fb->selected_entry < fb->entry_count) {
                            ed_file_entry_t *ent = &fb->entries[fb->selected_entry];
                            if (ent->is_dir) {
                                char new_d[128];
                                ed_path_join(new_d, sizeof(new_d), fb->current_dir, ent->name);
                                ed_browser_load_dir(fb, new_d);
                            } else {
                                char full_p[128];
                                ed_path_join(full_p, sizeof(full_p), fb->current_dir, ent->name);
                                editor_open_file_in_tab(ed, full_p);
                                ed_browser_free_entries(fb);
                                ed->dialog_type = ED_DLG_NONE;
                            }
                        } else if (fb->filename_len > 0) {
                            char full_p[128];
                            ed_path_join(full_p, sizeof(full_p), fb->current_dir, fb->filename);
                            editor_open_file_in_tab(ed, full_p);
                            ed_browser_free_entries(fb);
                            ed->dialog_type = ED_DLG_NONE;
                        }
                    }
                    return;
                }
                if (ev->key.keycode == KEY_UP) {
                    if (fb->selected_entry > 0) {
                        fb->selected_entry--;
                        if (fb->selected_entry < fb->vscroll) fb->vscroll = fb->selected_entry;
                        ed_strcpy(fb->filename, fb->entries[fb->selected_entry].name, sizeof(fb->filename));
                        fb->filename_len = (int)ed_strlen(fb->filename);
                    }
                    return;
                }
                if (ev->key.keycode == KEY_DOWN) {
                    if (fb->selected_entry + 1 < fb->entry_count) {
                        fb->selected_entry++;
                        if (fb->selected_entry >= fb->vscroll + vis_rows) fb->vscroll = fb->selected_entry - vis_rows + 1;
                        ed_strcpy(fb->filename, fb->entries[fb->selected_entry].name, sizeof(fb->filename));
                        fb->filename_len = (int)ed_strlen(fb->filename);
                    }
                    return;
                }
                if (ev->key.keycode == KEY_BACKSPACE) {
                    if (fb->filename_len > 0) {
                        fb->filename[--fb->filename_len] = '\0';
                        fb->confirm_overwrite = false;
                    }
                    return;
                }
                if (ev->key.ascii >= ' ' && ev->key.ascii <= '~') {
                    if (fb->filename_len < (int)sizeof(fb->filename) - 1) {
                        fb->filename[fb->filename_len++] = ev->key.ascii;
                        fb->filename[fb->filename_len] = '\0';
                        fb->confirm_overwrite = false;
                    }
                    return;
                }
            } else if (ev->type == INPUT_EVENT_MOUSE_WHEEL) {
                if (ev->mouse.delta_z > 0 && fb->vscroll > 0) fb->vscroll--;
                else if (ev->mouse.delta_z < 0 && fb->vscroll + vis_rows < fb->entry_count) fb->vscroll++;
                return;
            } else if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
                int rel_x = ev->mouse.x - win->client_x;
                int rel_y = ev->mouse.y - win->client_y;

                /* Close button [x] on dialog title */
                if (rel_x >= dx + dw - 24 && rel_x <= dx + dw - 6 && rel_y >= dy + 4 && rel_y <= dy + 24) {
                    ed_browser_free_entries(fb);
                    ed->dialog_type = ED_DLG_NONE;
                    return;
                }

                /* Nav Buttons */
                /* [^ Up] */
                if (rel_x >= dx + 70 && rel_x <= dx + 96 && rel_y >= dy + 32 && rel_y <= dy + 54) {
                    int last_slash = -1;
                    for (int i = 0; fb->current_dir[i]; i++) {
                        if (fb->current_dir[i] == '/') last_slash = i;
                    }
                    if (last_slash > 0) {
                        fb->current_dir[last_slash] = '\0';
                        ed_browser_load_dir(fb, fb->current_dir);
                    } else {
                        ed_browser_load_dir(fb, "/");
                    }
                    return;
                }
                /* [/data] pill */
                if (rel_x >= dx + 102 && rel_x <= dx + 156 && rel_y >= dy + 32 && rel_y <= dy + 54) {
                    ed_browser_load_dir(fb, "/data");
                    return;
                }
                /* [/] pill */
                if (rel_x >= dx + 160 && rel_x <= dx + 188 && rel_y >= dy + 32 && rel_y <= dy + 54) {
                    ed_browser_load_dir(fb, "/");
                    return;
                }

                /* Table Click */
                if (rel_x >= tbl_x && rel_x <= tbl_x + tbl_w && rel_y >= tbl_y && rel_y <= tbl_y + tbl_h) {
                    int row = (rel_y - tbl_y) / 24;
                    int clicked_idx = fb->vscroll + row;
                    if (clicked_idx >= 0 && clicked_idx < fb->entry_count) {
                        u64 now_tick = apic_get_ticks();
                        bool is_double = (fb->last_click_entry == clicked_idx && (now_tick - fb->last_click_tick < 40));
                        fb->last_click_tick = now_tick;
                        fb->last_click_entry = clicked_idx;
                        fb->selected_entry = clicked_idx;

                        ed_file_entry_t *ent = &fb->entries[clicked_idx];
                        if (!ent->is_dir) {
                            ed_strcpy(fb->filename, ent->name, sizeof(fb->filename));
                            fb->filename_len = (int)ed_strlen(fb->filename);
                            fb->confirm_overwrite = false;
                        }

                        if (is_double) {
                            if (ent->is_dir) {
                                if (ed_strcmp(ent->name, "..") == 0) {
                                    int last_slash = -1;
                                    for (int i = 0; fb->current_dir[i]; i++) {
                                        if (fb->current_dir[i] == '/') last_slash = i;
                                    }
                                    if (last_slash > 0) fb->current_dir[last_slash] = '\0';
                                    else ed_strcpy(fb->current_dir, "/", sizeof(fb->current_dir));
                                    ed_browser_load_dir(fb, fb->current_dir);
                                } else {
                                    char new_d[128];
                                    ed_path_join(new_d, sizeof(new_d), fb->current_dir, ent->name);
                                    ed_browser_load_dir(fb, new_d);
                                }
                            } else {
                                /* Double click on file: commit Open or Save */
                                char full_p[128];
                                ed_path_join(full_p, sizeof(full_p), fb->current_dir, ent->name);
                                if (fb->is_save_mode) {
                                    editor_save_as_commit(ed, full_p, fb->run_after_save);
                                } else {
                                    editor_open_file_in_tab(ed, full_p);
                                }
                                ed_browser_free_entries(fb);
                                ed->dialog_type = ED_DLG_NONE;
                            }
                        }
                    }
                    return;
                }

                /* Filter Selector Click */
                if (rel_x >= dx + 360 && rel_x <= dx + 508 && rel_y >= dy + 210 && rel_y <= dy + 234) {
                    fb->filter_idx = (fb->filter_idx + 1) % 4;
                    ed_browser_load_dir(fb, fb->current_dir);
                    return;
                }

                /* Action Button: [Open] or [Save] or [Overwrite] */
                if (rel_x >= dx + dw - 176 && rel_x <= dx + dw - 94 && rel_y >= act_y && rel_y <= act_y + 26) {
                    if (fb->confirm_overwrite) {
                        char full_p[128];
                        ed_path_join(full_p, sizeof(full_p), fb->current_dir, fb->filename);
                        editor_save_as_commit(ed, full_p, fb->run_after_save);
                        ed_browser_free_entries(fb);
                        ed->dialog_type = ED_DLG_NONE;
                        return;
                    }
                    if (fb->is_save_mode) {
                        if (fb->filename_len > 0) {
                            char full_p[128];
                            ed_path_join(full_p, sizeof(full_p), fb->current_dir, fb->filename);
                            vfs_node_t *ex = NULL;
                            if (vfs_lookup(full_p, &ex) == VFS_OK && ex) {
                                fb->confirm_overwrite = true;
                                return;
                            }
                            editor_save_as_commit(ed, full_p, fb->run_after_save);
                            ed_browser_free_entries(fb);
                            ed->dialog_type = ED_DLG_NONE;
                        }
                    } else {
                        if (fb->selected_entry >= 0 && fb->selected_entry < fb->entry_count && !fb->entries[fb->selected_entry].is_dir) {
                            char full_p[128];
                            ed_path_join(full_p, sizeof(full_p), fb->current_dir, fb->entries[fb->selected_entry].name);
                            editor_open_file_in_tab(ed, full_p);
                            ed_browser_free_entries(fb);
                            ed->dialog_type = ED_DLG_NONE;
                        } else if (fb->filename_len > 0) {
                            char full_p[128];
                            ed_path_join(full_p, sizeof(full_p), fb->current_dir, fb->filename);
                            editor_open_file_in_tab(ed, full_p);
                            ed_browser_free_entries(fb);
                            ed->dialog_type = ED_DLG_NONE;
                        }
                    }
                    return;
                }

                /* [Cancel] Button */
                if (rel_x >= dx + dw - 86 && rel_x <= dx + dw - 10 && rel_y >= act_y && rel_y <= act_y + 26) {
                    ed_browser_free_entries(fb);
                    ed->dialog_type = ED_DLG_NONE;
                    return;
                }
            }
            return;
        } else if (ed->dialog_type == ED_DLG_CONFIRM_CLOSE) {
            if (ev->type == INPUT_EVENT_KEY_DOWN && ev->key.keycode == KEY_ESC) {
                ed->dialog_type = ED_DLG_NONE;
                return;
            }
            if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
                int rel_x = ev->mouse.x - win->client_x;
                int rel_y = ev->mouse.y - win->client_y;
                int dw = 380;
                int dh = 150;
                int dx = (win->client_w - dw) / 2;
                int dy = (win->client_h - dh) / 2;
                int btn_y = dy + dh - 36;

                /* Save */
                if (rel_x >= dx + 20 && rel_x <= dx + 100 && rel_y >= btn_y && rel_y <= btn_y + 24) {
                    editor_save_active_tab(ed);
                    editor_close_tab(ed, ed->close_tab_target);
                    ed->dialog_type = ED_DLG_NONE;
                    return;
                }
                /* Don't Save */
                if (rel_x >= dx + 110 && rel_x <= dx + 220 && rel_y >= btn_y && rel_y <= btn_y + 24) {
                    editor_close_tab(ed, ed->close_tab_target);
                    ed->dialog_type = ED_DLG_NONE;
                    return;
                }
                /* Cancel */
                if (rel_x >= dx + 230 && rel_x <= dx + 310 && rel_y >= btn_y && rel_y <= btn_y + 24) {
                    ed->dialog_type = ED_DLG_NONE;
                    return;
                }
            }
            return;
        } else if (ed->dialog_type == ED_DLG_CONFIRM_RUN) {
            if (ev->type == INPUT_EVENT_KEY_DOWN && ev->key.keycode == KEY_ESC) {
                ed->dialog_type = ED_DLG_NONE;
                return;
            }
            if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
                int rel_x = ev->mouse.x - win->client_x;
                int rel_y = ev->mouse.y - win->client_y;
                int dw = 380;
                int dh = 150;
                int dx = (win->client_w - dw) / 2;
                int dy = (win->client_h - dh) / 2;
                int btn_y = dy + dh - 36;

                /* Save & Run */
                if (rel_x >= dx + 16 && rel_x <= dx + 116 && rel_y >= btn_y && rel_y <= btn_y + 24) {
                    ed->dialog_type = ED_DLG_NONE;
                    if (tab->is_untitled) {
                        ed_browser_open_dialog(ed, true, true);
                    } else {
                        editor_save_active_tab(ed);
                        char cmd[160];
                        int cp = 0;
                        const char *prefix = "run ";
                        for (int k = 0; prefix[k]; k++) cmd[cp++] = prefix[k];
                        for (int k = 0; tab->filepath[k] && cp < 150; k++) cmd[cp++] = tab->filepath[k];
                        cmd[cp] = '\0';
                        app_terminal_launch_with_command(cmd);
                    }
                    return;
                }

                /* Run Temp Copy */
                if (rel_x >= dx + 124 && rel_x <= dx + 264 && rel_y >= btn_y && rel_y <= btn_y + 24) {
                    ed->dialog_type = ED_DLG_NONE;
                    const char *tmp_p = "/tmp/run_temp.py";
                    int tfd = vfs_open(tmp_p, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
                    if (tfd >= 0) {
                        vfs_write(tfd, tab->text, tab->text_len);
                        vfs_close(tfd);
                        char cmd[160];
                        ed_strcpy(cmd, "run /tmp/run_temp.py", sizeof(cmd));
                        app_terminal_launch_with_command(cmd);
                        ed_set_status(ed, "Running temporary execution copy in Terminal.");
                    }
                    return;
                }

                /* Cancel */
                if (rel_x >= dx + 272 && rel_x <= dx + 348 && rel_y >= btn_y && rel_y <= btn_y + 24) {
                    ed->dialog_type = ED_DLG_NONE;
                    return;
                }
            }
            return;
        } else if (ed->dialog_type == ED_DLG_FIND || ed->dialog_type == ED_DLG_REPLACE) {
            ed_find_replace_t *fr = &ed->fr;
            int dw = 380;
            int dh = (ed->dialog_type == ED_DLG_REPLACE) ? 170 : 130;
            int dx = (win->client_w - dw) / 2;
            int dy = (win->client_h - dh) / 2;
            int btn_y = dy + dh - 36;

            if (ev->type == INPUT_EVENT_KEY_DOWN) {
                if (ev->key.keycode == KEY_ESC) {
                    ed->dialog_type = ED_DLG_NONE;
                    return;
                }
                if (ev->key.keycode == KEY_TAB && ed->dialog_type == ED_DLG_REPLACE) {
                    fr->active_field = 1 - fr->active_field;
                    return;
                }
                if (ev->key.keycode == KEY_ENTER) {
                    /* Find Next */
                    if (fr->find_len > 0 && tab && tab->text) {
                        int start = tab->cursor_pos;
                        if (start >= tab->text_len) start = 0;
                        int found = -1;
                        for (int i = start; i <= tab->text_len - fr->find_len; i++) {
                            if (ed_strncmp(&tab->text[i], fr->find_str, fr->find_len) == 0) {
                                found = i;
                                break;
                            }
                        }
                        if (found < 0) {
                            /* Wrap search */
                            for (int i = 0; i < start && i <= tab->text_len - fr->find_len; i++) {
                                if (ed_strncmp(&tab->text[i], fr->find_str, fr->find_len) == 0) {
                                    found = i;
                                    break;
                                }
                            }
                        }
                        if (found >= 0) {
                            tab->sel_start = found;
                            tab->sel_end = found + fr->find_len;
                            tab->cursor_pos = tab->sel_end;
                            ed_set_status(ed, "Match found.");
                        } else {
                            ed_set_status(ed, "String not found.");
                        }
                    }
                    return;
                }
                if (ev->key.keycode == KEY_BACKSPACE) {
                    if (fr->active_field == 0 && fr->find_len > 0) {
                        fr->find_str[--fr->find_len] = '\0';
                    } else if (fr->active_field == 1 && fr->replace_len > 0) {
                        fr->replace_str[--fr->replace_len] = '\0';
                    }
                    return;
                }
                if (ev->key.ascii >= ' ' && ev->key.ascii <= '~') {
                    if (fr->active_field == 0 && fr->find_len < (int)sizeof(fr->find_str) - 1) {
                        fr->find_str[fr->find_len++] = ev->key.ascii;
                        fr->find_str[fr->find_len] = '\0';
                    } else if (fr->active_field == 1 && fr->replace_len < (int)sizeof(fr->replace_str) - 1) {
                        fr->replace_str[fr->replace_len++] = ev->key.ascii;
                        fr->replace_str[fr->replace_len] = '\0';
                    }
                    return;
                }
            } else if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
                int rel_x = ev->mouse.x - win->client_x;
                int rel_y = ev->mouse.y - win->client_y;

                /* Next button */
                if (rel_x >= dx + dw - 180 && rel_x <= dx + dw - 98 && rel_y >= btn_y && rel_y <= btn_y + 24) {
                    if (fr->find_len > 0 && tab && tab->text) {
                        int start = tab->cursor_pos;
                        if (start >= tab->text_len) start = 0;
                        int found = -1;
                        for (int i = start; i <= tab->text_len - fr->find_len; i++) {
                            if (ed_strncmp(&tab->text[i], fr->find_str, fr->find_len) == 0) {
                                found = i;
                                break;
                            }
                        }
                        if (found < 0) {
                            for (int i = 0; i < start && i <= tab->text_len - fr->find_len; i++) {
                                if (ed_strncmp(&tab->text[i], fr->find_str, fr->find_len) == 0) {
                                    found = i;
                                    break;
                                }
                            }
                        }
                        if (found >= 0) {
                            tab->sel_start = found;
                            tab->sel_end = found + fr->find_len;
                            tab->cursor_pos = tab->sel_end;
                            ed_set_status(ed, "Match found.");
                        } else {
                            ed_set_status(ed, "String not found.");
                        }
                    }
                    return;
                }

                /* Replace button */
                if (ed->dialog_type == ED_DLG_REPLACE && rel_x >= dx + dw - 270 && rel_x <= dx + dw - 188 && rel_y >= btn_y && rel_y <= btn_y + 24) {
                    if (ed_has_selection(tab) && fr->find_len > 0) {
                        int s_min, s_max;
                        ed_get_sel_bounds(tab, &s_min, &s_max);
                        if (s_max - s_min == fr->find_len && ed_strncmp(&tab->text[s_min], fr->find_str, fr->find_len) == 0) {
                            ed_delete_selection(ed, tab);
                            editor_save_undo(tab);
                            for (int i = tab->text_len - 1; i >= tab->cursor_pos; i--) {
                                tab->text[i + fr->replace_len] = tab->text[i];
                            }
                            for (int i = 0; i < fr->replace_len; i++) {
                                tab->text[tab->cursor_pos + i] = fr->replace_str[i];
                            }
                            tab->text_len += fr->replace_len;
                            tab->cursor_pos += fr->replace_len;
                            tab->text[tab->text_len] = '\0';
                            tab->dirty = true;
                            tab->total_lines = ed_count_lines(tab->text, tab->text_len);
                            ed_set_status(ed, "Replaced match.");
                        }
                    }
                    return;
                }

                /* Close button */
                if (rel_x >= dx + dw - 90 && rel_x <= dx + dw - 14 && rel_y >= btn_y && rel_y <= btn_y + 24) {
                    ed->dialog_type = ED_DLG_NONE;
                    return;
                }
            }
            return;
        }
        return;
    }

    /* B. Mouse Wheel Scrolling */
    if (ev->type == INPUT_EVENT_MOUSE_WHEEL && tab) {
        if (ev->mouse.delta_z > 0) {
            tab->vscroll -= 3;
            if (tab->vscroll < 0) tab->vscroll = 0;
        } else if (ev->mouse.delta_z < 0) {
            tab->vscroll += 3;
            if (tab->vscroll > tab->total_lines - 1) tab->vscroll = tab->total_lines - 1;
            if (tab->vscroll < 0) tab->vscroll = 0;
        }
        return;
    }

    /* C. Mouse Move (Scrollbar Dragging & Text Selection) */
    if (ev->type == INPUT_EVENT_MOUSE_MOVE && tab) {
        int rel_y = ev->mouse.y - win->client_y;
        if (scrollview_handle_mouse_move(&tab->scrollview, rel_y)) {
            tab->vscroll = tab->scrollview.scroll_offset;
            return;
        }
    }

    /* D. Mouse Up */
    if (ev->type == INPUT_EVENT_MOUSE_UP && tab) {
        scrollview_handle_mouse_up(&tab->scrollview);
        return;
    }

    /* E. Keyboard Input */
    if (ev->type == INPUT_EVENT_KEY_DOWN && tab) {
        bool ctrl = (ev->key.modifiers & KEY_MOD_CTRL) != 0;
        bool shift = (ev->key.modifiers & KEY_MOD_SHIFT) != 0;
        u8 ascii = (u8)ev->key.ascii;
        u32 keycode = ev->key.keycode;

        /* Ctrl+A: Select All (Priority 2) */
        if (ctrl && (keycode == 'a' || keycode == 'A' || ascii == 1)) {
            tab->sel_start = 0;
            tab->sel_end = tab->text_len;
            tab->cursor_pos = tab->text_len;
            ed_set_status(ed, "Selected all text.");
            return;
        }

        /* Ctrl+N: New Tab */
        if (ctrl && (keycode == 'n' || keycode == 'N' || ascii == 14)) {
            editor_create_untitled_tab(ed);
            return;
        }

        /* Ctrl+O: Open File Browser Dialog (Priority 3) */
        if (ctrl && (keycode == 'o' || keycode == 'O' || ascii == 15)) {
            ed_browser_open_dialog(ed, false, false);
            return;
        }

        /* Ctrl+Shift+S: Save As (Priority 2) */
        if (ctrl && shift && (keycode == 's' || keycode == 'S' || ascii == 19)) {
            ed_browser_open_dialog(ed, true, false);
            return;
        }

        /* Ctrl+S: Save */
        if ((ctrl && !shift && (keycode == 's' || keycode == 'S' || ascii == 19)) || ascii == 19) {
            editor_save_active_tab(ed);
            return;
        }

        /* Ctrl+W: Close Tab */
        if (ctrl && (keycode == 'w' || keycode == 'W' || ascii == 23)) {
            if (tab->dirty) {
                ed->dialog_type = ED_DLG_CONFIRM_CLOSE;
                ed->close_tab_target = ed->active_tab;
            } else {
                editor_close_tab(ed, ed->active_tab);
            }
            return;
        }

        /* Ctrl+P: Quick Run in Terminal (Priority 2) */
        if ((ctrl && (keycode == 'p' || keycode == 'P' || ascii == 16)) || ascii == 16) {
            editor_trigger_run(ed);
            return;
        }

        /* Ctrl+Z: Undo */
        if ((ctrl && (keycode == 'z' || keycode == 'Z' || ascii == 26)) || ascii == 26) {
            editor_apply_undo(ed, tab);
            return;
        }

        /* Ctrl+Y: Redo (Priority 2) */
        if ((ctrl && (keycode == 'y' || keycode == 'Y' || ascii == 25)) || ascii == 25) {
            editor_apply_redo(ed, tab);
            return;
        }

        /* Ctrl+F: Find (Priority 2) */
        if ((ctrl && (keycode == 'f' || keycode == 'F' || ascii == 6)) || ascii == 6) {
            ed->dialog_type = ED_DLG_FIND;
            ed->fr.active_field = 0;
            return;
        }

        /* Ctrl+H: Replace (Priority 2) */
        if (ctrl && (keycode == 'h' || keycode == 'H' || ascii == 8)) {
            ed->dialog_type = ED_DLG_REPLACE;
            ed->fr.active_field = 0;
            return;
        }

        /* Ctrl+C: Copy (Selected text or entire line) */
        if ((ctrl && (keycode == 'c' || keycode == 'C' || ascii == 3)) || ascii == 3) {
            if (ed_has_selection(tab)) {
                int s_min, s_max;
                ed_get_sel_bounds(tab, &s_min, &s_max);
                size_t copy_len = (size_t)(s_max - s_min);
                if (copy_len > 0) {
                    clipboard_set_text(&tab->text[s_min], copy_len);
                    ed_set_status(ed, "Copied selection to clipboard.");
                }
            } else {
                /* Whole line copy behavior */
                int start = tab->cursor_pos;
                while (start > 0 && tab->text[start - 1] != '\n') start--;
                int end = tab->cursor_pos;
                while (end < tab->text_len && tab->text[end] != '\n') end++;
                if (end < tab->text_len && tab->text[end] == '\n') end++;
                size_t copy_len = (size_t)(end - start);
                if (copy_len > 0) {
                    clipboard_set_text(&tab->text[start], copy_len);
                    ed_set_status(ed, "Copied line to clipboard.");
                }
            }
            return;
        }

        /* Ctrl+X: Cut (Selected text or entire line) */
        if ((ctrl && (keycode == 'x' || keycode == 'X' || ascii == 24)) || ascii == 24) {
            if (ed_has_selection(tab)) {
                int s_min, s_max;
                ed_get_sel_bounds(tab, &s_min, &s_max);
                size_t cut_len = (size_t)(s_max - s_min);
                if (cut_len > 0) {
                    clipboard_set_text(&tab->text[s_min], cut_len);
                    ed_delete_selection(ed, tab);
                    ed_set_status(ed, "Cut selection to clipboard.");
                }
            } else {
                /* Whole line cut behavior */
                int start = tab->cursor_pos;
                while (start > 0 && tab->text[start - 1] != '\n') start--;
                int end = tab->cursor_pos;
                while (end < tab->text_len && tab->text[end] != '\n') end++;
                if (end < tab->text_len && tab->text[end] == '\n') end++;
                size_t cut_len = (size_t)(end - start);
                if (cut_len > 0 && tab->text) {
                    editor_save_undo(tab);
                    clipboard_set_text(&tab->text[start], cut_len);
                    for (int i = start; i <= tab->text_len - (int)cut_len; i++) {
                        tab->text[i] = tab->text[i + cut_len];
                    }
                    tab->text_len -= (int)cut_len;
                    tab->cursor_pos = start;
                    tab->text[tab->text_len] = '\0';
                    tab->dirty = true;
                    tab->total_lines = ed_count_lines(tab->text, tab->text_len);
                    ed_set_status(ed, "Cut line to clipboard.");
                }
            }
            return;
        }

        /* Ctrl+V: Paste at cursor (replaces selection if active) */
        if ((ctrl && (keycode == 'v' || keycode == 'V' || ascii == 22)) || ascii == 22) {
            size_t clip_len = 0;
            const char *clip_data = clipboard_get_text(&clip_len);
            if (clip_data && clip_len > 0 && tab->text) {
                if (ed_has_selection(tab)) {
                    ed_delete_selection(ed, tab);
                } else {
                    editor_save_undo(tab);
                }
                if (tab->text_len + (int)clip_len < EDITOR_TAB_BUF_SIZE - 2) {
                    for (int i = tab->text_len - 1; i >= tab->cursor_pos; i--) {
                        tab->text[i + clip_len] = tab->text[i];
                    }
                    for (size_t i = 0; i < clip_len; i++) {
                        tab->text[tab->cursor_pos + i] = clip_data[i];
                    }
                    tab->text_len += (int)clip_len;
                    tab->cursor_pos += (int)clip_len;
                    tab->text[tab->text_len] = '\0';
                    tab->dirty = true;
                    tab->total_lines = ed_count_lines(tab->text, tab->text_len);
                    ed_clear_selection(tab);
                    ed_set_status(ed, "Pasted from clipboard.");
                }
            }
            return;
        }

        /* Ctrl+D: Duplicate line */
        if (ctrl && (keycode == 'd' || keycode == 'D' || ascii == 4)) {
            int start = tab->cursor_pos;
            while (start > 0 && tab->text[start - 1] != '\n') start--;
            int end = tab->cursor_pos;
            while (end < tab->text_len && tab->text[end] != '\n') end++;
            int line_len = end - start;
            if (line_len > 0 && tab->text && tab->text_len + line_len + 1 < EDITOR_TAB_BUF_SIZE - 2) {
                editor_save_undo(tab);
                char line_buf[256];
                int cp_len = (line_len < 255) ? line_len : 255;
                for (int i = 0; i < cp_len; i++) line_buf[i] = tab->text[start + i];
                line_buf[cp_len++] = '\n';
                for (int i = tab->text_len - 1; i >= end; i--) {
                    tab->text[i + cp_len] = tab->text[i];
                }
                for (int i = 0; i < cp_len; i++) {
                    tab->text[end + i] = line_buf[i];
                }
                tab->text_len += cp_len;
                tab->cursor_pos = end + cp_len;
                tab->text[tab->text_len] = '\0';
                tab->dirty = true;
                tab->total_lines = ed_count_lines(tab->text, tab->text_len);
                ed_set_status(ed, "Line duplicated.");
            }
            return;
        }

        /* Ctrl+K: Delete line */
        if (ctrl && (keycode == 'k' || keycode == 'K' || ascii == 11)) {
            int start = tab->cursor_pos;
            while (start > 0 && tab->text[start - 1] != '\n') start--;
            int end = tab->cursor_pos;
            while (end < tab->text_len && tab->text[end] != '\n') end++;
            if (end < tab->text_len && tab->text[end] == '\n') end++;
            size_t del_len = (size_t)(end - start);
            if (del_len > 0 && tab->text) {
                editor_save_undo(tab);
                for (int i = start; i <= tab->text_len - (int)del_len; i++) {
                    tab->text[i] = tab->text[i + del_len];
                }
                tab->text_len -= (int)del_len;
                tab->cursor_pos = start;
                tab->text[tab->text_len] = '\0';
                tab->dirty = true;
                tab->total_lines = ed_count_lines(tab->text, tab->text_len);
                ed_set_status(ed, "Line deleted.");
            }
            return;
        }

        /* Navigation with selection tracking */
        if (keycode == KEY_PAGE_UP) {
            tab->vscroll -= 15;
            if (tab->vscroll < 0) tab->vscroll = 0;
            return;
        }
        if (keycode == KEY_PAGE_DOWN) {
            tab->vscroll += 15;
            if (tab->vscroll > tab->total_lines - 1) tab->vscroll = tab->total_lines - 1;
            if (tab->vscroll < 0) tab->vscroll = 0;
            return;
        }
        if (keycode == KEY_HOME) {
            if (shift && tab->sel_start < 0) tab->sel_start = tab->cursor_pos;
            while (tab->cursor_pos > 0 && tab->text[tab->cursor_pos - 1] != '\n') {
                tab->cursor_pos--;
            }
            if (shift) tab->sel_end = tab->cursor_pos;
            else ed_clear_selection(tab);
            return;
        }
        if (keycode == KEY_END) {
            if (shift && tab->sel_start < 0) tab->sel_start = tab->cursor_pos;
            while (tab->cursor_pos < tab->text_len && tab->text[tab->cursor_pos] != '\n') {
                tab->cursor_pos++;
            }
            if (shift) tab->sel_end = tab->cursor_pos;
            else ed_clear_selection(tab);
            return;
        }
        if (keycode == KEY_LEFT) {
            if (shift && tab->sel_start < 0) tab->sel_start = tab->cursor_pos;
            if (tab->cursor_pos > 0) tab->cursor_pos--;
            if (shift) tab->sel_end = tab->cursor_pos;
            else ed_clear_selection(tab);
            return;
        }
        if (keycode == KEY_RIGHT) {
            if (shift && tab->sel_start < 0) tab->sel_start = tab->cursor_pos;
            if (tab->cursor_pos < tab->text_len) tab->cursor_pos++;
            if (shift) tab->sel_end = tab->cursor_pos;
            else ed_clear_selection(tab);
            return;
        }
        if (keycode == KEY_UP) {
            if (shift && tab->sel_start < 0) tab->sel_start = tab->cursor_pos;
            int cur_c = 0;
            int p = tab->cursor_pos;
            while (p > 0 && tab->text[p - 1] != '\n') { p--; cur_c++; }
            if (p > 0) {
                int prev_line_end = p - 1;
                int prev_line_start = prev_line_end;
                while (prev_line_start > 0 && tab->text[prev_line_start - 1] != '\n') {
                    prev_line_start--;
                }
                int prev_len = prev_line_end - prev_line_start;
                int target_c = (cur_c < prev_len) ? cur_c : prev_len;
                tab->cursor_pos = prev_line_start + target_c;
            }
            if (shift) tab->sel_end = tab->cursor_pos;
            else ed_clear_selection(tab);
            return;
        }
        if (keycode == KEY_DOWN) {
            if (shift && tab->sel_start < 0) tab->sel_start = tab->cursor_pos;
            int cur_c = 0;
            int p = tab->cursor_pos;
            while (p > 0 && tab->text[p - 1] != '\n') { p--; cur_c++; }
            int next_line_start = tab->cursor_pos;
            while (next_line_start < tab->text_len && tab->text[next_line_start] != '\n') {
                next_line_start++;
            }
            if (next_line_start < tab->text_len) {
                next_line_start++;
                int next_line_end = next_line_start;
                while (next_line_end < tab->text_len && tab->text[next_line_end] != '\n') {
                    next_line_end++;
                }
                int next_len = next_line_end - next_line_start;
                int target_c = (cur_c < next_len) ? cur_c : next_len;
                tab->cursor_pos = next_line_start + target_c;
            }
            if (shift) tab->sel_end = tab->cursor_pos;
            else ed_clear_selection(tab);
            return;
        }

        /* Editing operations */
        if (keycode == KEY_BACKSPACE) {
            if (ed_has_selection(tab)) {
                ed_delete_selection(ed, tab);
            } else if (tab->cursor_pos > 0 && tab->text) {
                editor_save_undo(tab);
                for (int i = tab->cursor_pos - 1; i < tab->text_len - 1; i++) {
                    tab->text[i] = tab->text[i + 1];
                }
                tab->text_len--;
                tab->cursor_pos--;
                tab->text[tab->text_len] = '\0';
                tab->dirty = true;
                tab->total_lines = ed_count_lines(tab->text, tab->text_len);
            }
            return;
        }
        if (keycode == KEY_DELETE) {
            if (ed_has_selection(tab)) {
                ed_delete_selection(ed, tab);
            } else if (tab->cursor_pos < tab->text_len && tab->text) {
                editor_save_undo(tab);
                for (int i = tab->cursor_pos; i < tab->text_len - 1; i++) {
                    tab->text[i] = tab->text[i + 1];
                }
                tab->text_len--;
                tab->text[tab->text_len] = '\0';
                tab->dirty = true;
                tab->total_lines = ed_count_lines(tab->text, tab->text_len);
            }
            return;
        }
        if (keycode == KEY_ENTER) {
            if (ed_has_selection(tab)) {
                ed_delete_selection(ed, tab);
            }
            if (tab->text && tab->text_len < EDITOR_TAB_BUF_SIZE - 2) {
                editor_save_undo(tab);
                for (int i = tab->text_len; i > tab->cursor_pos; i--) {
                    tab->text[i] = tab->text[i - 1];
                }
                tab->text[tab->cursor_pos] = '\n';
                tab->text_len++;
                tab->cursor_pos++;
                tab->text[tab->text_len] = '\0';
                tab->dirty = true;
                tab->total_lines = ed_count_lines(tab->text, tab->text_len);
                ed_clear_selection(tab);
            }
            return;
        }
        if (keycode == KEY_TAB) {
            if (ed_has_selection(tab)) {
                ed_delete_selection(ed, tab);
            }
            if (tab->text && tab->text_len + 4 < EDITOR_TAB_BUF_SIZE - 2) {
                editor_save_undo(tab);
                for (int k = 0; k < 4; k++) {
                    for (int i = tab->text_len; i > tab->cursor_pos; i--) {
                        tab->text[i] = tab->text[i - 1];
                    }
                    tab->text[tab->cursor_pos] = ' ';
                    tab->text_len++;
                    tab->cursor_pos++;
                }
                tab->text[tab->text_len] = '\0';
                tab->dirty = true;
                tab->total_lines = ed_count_lines(tab->text, tab->text_len);
                ed_clear_selection(tab);
            }
            return;
        }

        /* Printable ASCII characters */
        if (!ctrl && ascii >= ' ' && ascii <= '~') {
            if (ed_has_selection(tab)) {
                ed_delete_selection(ed, tab);
            }
            if (tab->text && tab->text_len < EDITOR_TAB_BUF_SIZE - 2) {
                for (int i = tab->text_len; i > tab->cursor_pos; i--) {
                    tab->text[i] = tab->text[i - 1];
                }
                tab->text[tab->cursor_pos] = ascii;
                tab->text_len++;
                tab->cursor_pos++;
                tab->text[tab->text_len] = '\0';
                tab->dirty = true;
                tab->total_lines = ed_count_lines(tab->text, tab->text_len);
                ed_clear_selection(tab);
            }
            return;
        }
    }

    /* F. Mouse Click Handling */
    if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        int rel_x = ev->mouse.x - win->client_x;
        int rel_y = ev->mouse.y - win->client_y;

        /* Click on Top Header Action Buttons (y: 5..27) */
        if (rel_y >= 5 && rel_y <= 27) {
            /* [+ New] */
            if (rel_x >= 8 && rel_x <= 64) {
                editor_create_untitled_tab(ed);
                return;
            }
            /* [Open] */
            if (rel_x >= 68 && rel_x <= 120) {
                ed_browser_open_dialog(ed, false, false);
                return;
            }
            /* [Save] */
            if (rel_x >= 124 && rel_x <= 176) {
                editor_save_active_tab(ed);
                return;
            }
            /* [Save As] */
            if (rel_x >= 180 && rel_x <= 248) {
                ed_browser_open_dialog(ed, true, false);
                return;
            }
            /* [Run >] */
            if (rel_x >= 252 && rel_x <= 310) {
                editor_trigger_run(ed);
                return;
            }
            /* [Find] */
            if (rel_x >= 314 && rel_x <= 366) {
                ed->dialog_type = ED_DLG_FIND;
                ed->fr.active_field = 0;
                return;
            }
        }

        /* Click on Tab Strip (y: 33..60) */
        if (rel_y >= 33 && rel_y <= 60) {
            int tab_w = 120;
            for (int i = 0; i < ed->tab_count; i++) {
                int tx = 8 + i * (tab_w + 4);
                if (tx + tab_w > win->client_w - 40) break;

                /* Close button [x] on tab */
                if (rel_x >= tx + tab_w - 20 && rel_x <= tx + tab_w - 2) {
                    if (ed->tabs[i].dirty) {
                        ed->dialog_type = ED_DLG_CONFIRM_CLOSE;
                        ed->close_tab_target = i;
                    } else {
                        editor_close_tab(ed, i);
                    }
                    return;
                }

                /* Tab body click -> switch tab */
                if (rel_x >= tx && rel_x < tx + tab_w - 20) {
                    ed->active_tab = i;
                    return;
                }
            }

            /* Click on [+] tab button */
            if (ed->tab_count < EDITOR_MAX_TABS) {
                int add_x = 8 + ed->tab_count * (tab_w + 4);
                if (rel_x >= add_x && rel_x <= add_x + 24) {
                    editor_create_untitled_tab(ed);
                    return;
                }
            }
        }

        /* Click on Canvas Scrollbar */
        if (tab && scrollview_handle_mouse_down(&tab->scrollview, rel_x, rel_y)) {
            tab->vscroll = tab->scrollview.scroll_offset;
            return;
        }

        /* Click inside text canvas to position cursor or select */
        int gutter_w = 42;
        int canvas_top = 62;
        int sb_y = win->client_h - 24;
        if (tab && rel_x >= gutter_w && rel_y >= canvas_top && rel_y < sb_y) {
            int line_clicked = tab->vscroll + (rel_y - canvas_top - 6) / 18 + 1;
            int col_clicked = (rel_x - gutter_w - 10) / FONT_WIDTH + 1 + tab->hscroll;
            if (col_clicked < 1) col_clicked = 1;

            int target_p = 0;
            int cur_l = 1;
            int cur_c = 1;
            while (target_p < tab->text_len && cur_l < line_clicked) {
                if (tab->text[target_p] == '\n') cur_l++;
                target_p++;
            }
            while (target_p < tab->text_len && tab->text[target_p] != '\n' && cur_c < col_clicked) {
                cur_c++;
                target_p++;
            }
            tab->cursor_pos = target_p;
            ed_clear_selection(tab);
        }
    }
}

static void editor_on_destroy(wm_window_t *win) {
    if (!win) return;
    editor_state_t *ed = (editor_state_t *)win->user_data;
    if (ed) {
        for (int i = 0; i < ed->tab_count; i++) {
            editor_free_tab_buffers(&ed->tabs[i]);
        }
        ed->tab_count = 0;
        ed_browser_free_entries(&ed->browser);
    }
}

void app_editor_launch(const char *filepath) {
    if (ed_strcmp(filepath, "") == 0) filepath = NULL;

    if (g_editor.tab_count == 0) {
        if (filepath) {
            editor_open_file_in_tab(&g_editor, filepath);
        } else {
            editor_create_untitled_tab(&g_editor);
        }
    } else {
        if (filepath) {
            editor_open_file_in_tab(&g_editor, filepath);
        }
    }

    /* Reuse existing window if open */
    wm_window_t *existing = wm_find_by_title_substring("AstraOS Code Editor");
    if (existing) {
        existing->user_data = &g_editor;
        wm_bring_to_front(existing);
        return;
    }

    wm_window_t *win = wm_create_window("AstraOS Code Editor", 240, 60, 680, 440);
    if (!win) return;

    win->user_data = &g_editor;
    win->on_paint = editor_on_paint;
    win->on_event = editor_on_event;
    win->on_destroy = editor_on_destroy;
}
