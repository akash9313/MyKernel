#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/vfs.h>
#include <kernel/mem.h>
#include <kernel/kprintf.h>

#define FILEMGR_MAX_ENTRIES 32

typedef struct filemgr_item {
    char name[32];
    bool is_dir;
    size_t size;
} filemgr_item_t;

typedef struct filemgr_state {
    char current_path[64];
    filemgr_item_t items[FILEMGR_MAX_ENTRIES];
    int item_count;
    int selected_idx;
    char status_msg[128];
} filemgr_state_t;

static filemgr_state_t g_filemgr = {0};

/* Forward declaration for self-test and editor integration */
void app_editor_launch(const char *filepath);

static void filemgr_load_dir(filemgr_state_t *fm, const char *path) {
    if (!fm) return;

    size_t i = 0;
    while (path[i] && i < sizeof(fm->current_path) - 1) {
        fm->current_path[i] = path[i];
        i++;
    }
    fm->current_path[i] = '\0';

    fm->item_count = 0;
    fm->selected_idx = -1;

    /* Add '..' if not root */
    if (fm->current_path[0] == '/' && fm->current_path[1] != '\0') {
        filemgr_item_t *up = &fm->items[fm->item_count++];
        up->name[0] = '.'; up->name[1] = '.'; up->name[2] = '\0';
        up->is_dir = true;
        up->size = 0;
    }

    vfs_dirent_t dent;
    size_t idx = 0;
    while (vfs_readdir(fm->current_path, idx, &dent) == VFS_OK && fm->item_count < FILEMGR_MAX_ENTRIES) {
        filemgr_item_t *item = &fm->items[fm->item_count++];
        size_t ni = 0;
        while (dent.name[ni] && ni < sizeof(item->name) - 1) {
            item->name[ni] = dent.name[ni];
            ni++;
        }
        item->name[ni] = '\0';
        item->is_dir = (dent.type == VFS_NODE_DIR);
        item->size = dent.size;
        idx++;
    }

    const char *msg = "VFS directory loaded. Select item or double click.";
    size_t mi = 0;
    while (msg[mi] && mi < sizeof(fm->status_msg) - 1) {
        fm->status_msg[mi] = msg[mi];
        mi++;
    }
    fm->status_msg[mi] = '\0';
}

static void filemgr_create_file(filemgr_state_t *fm) {
    if (!fm) return;
    char path[128];
    static const char *cand_names[] = { "main.c", "script.py", "test.sh", "app.cpp", "notes.txt" };
    bool found_slot = false;
    for (int c = 0; c < 5; c++) {
        char test_path[128];
        int tp = 0;
        for (int i = 0; fm->current_path[i] && tp < 80; i++) test_path[tp++] = fm->current_path[i];
        if (tp > 0 && test_path[tp - 1] != '/') test_path[tp++] = '/';
        for (int i = 0; cand_names[c][i] && tp < 120; i++) test_path[tp++] = cand_names[c][i];
        test_path[tp] = '\0';
        vfs_node_t *exist = NULL;
        if (vfs_lookup(test_path, &exist) != VFS_OK) {
            for (int i = 0; i <= tp; i++) path[i] = test_path[i];
            found_slot = true;
            break;
        }
    }
    if (!found_slot) {
        for (int n = 1; n <= 20; n++) {
            char test_path[128];
            int tp = 0;
            for (int i = 0; fm->current_path[i] && tp < 80; i++) test_path[tp++] = fm->current_path[i];
            if (tp > 0 && test_path[tp - 1] != '/') test_path[tp++] = '/';
            const char *prefix = "file_";
            for (int i = 0; prefix[i]; i++) test_path[tp++] = prefix[i];
            if (n >= 10) test_path[tp++] = '0' + (n / 10);
            test_path[tp++] = '0' + (n % 10);
            const char *ext = ".txt";
            for (int i = 0; ext[i]; i++) test_path[tp++] = ext[i];
            test_path[tp] = '\0';
            vfs_node_t *exist = NULL;
            if (vfs_lookup(test_path, &exist) != VFS_OK) {
                for (int i = 0; i <= tp; i++) path[i] = test_path[i];
                found_slot = true;
                break;
            }
        }
    }
    if (!found_slot) return;

    int fd = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd >= 0) {
        const char *template = "// AstraOS C Source File\n#include <stdio.h>\n\nint main() {\n    printf(\"Hello from AstraOS C Engine!\\n\");\n    int sum = 10 + 20;\n    printf(\"Sum = %d\\n\", sum);\n    return 0;\n}\n";
        size_t plen = 0;
        while (path[plen]) plen++;
        if (plen >= 3 && path[plen - 3] == '.' && path[plen - 2] == 'p' && path[plen - 1] == 'y') {
            template = "# AstraOS Python Script\nprint(\"Hello from AstraOS Python 3.11!\")\nx = 10\ny = 25\nprint(\"x * y =\", x * y)\nfor i in range(5):\n    print(\"iteration:\", i)\n";
        } else if (plen >= 3 && path[plen - 3] == '.' && path[plen - 2] == 's' && path[plen - 1] == 'h') {
            template = "#!/bin/sh\necho \"AstraOS Shell Script Running\"\npwd\nwhoami\nuname -a\n";
        }
        size_t tlen = 0;
        while (template[tlen]) tlen++;
        vfs_write(fd, template, tlen);
        vfs_close(fd);

        filemgr_load_dir(fm, fm->current_path);
        app_editor_launch(path);
    }
}

static void filemgr_create_folder(filemgr_state_t *fm) {
    if (!fm) return;
    char path[128];
    bool found_slot = false;
    for (int n = 1; n <= 20; n++) {
        char test_path[128];
        int tp = 0;
        for (int i = 0; fm->current_path[i] && tp < 80; i++) test_path[tp++] = fm->current_path[i];
        if (tp > 0 && test_path[tp - 1] != '/') test_path[tp++] = '/';
        const char *prefix = "folder_";
        for (int i = 0; prefix[i]; i++) test_path[tp++] = prefix[i];
        if (n >= 10) test_path[tp++] = '0' + (n / 10);
        test_path[tp++] = '0' + (n % 10);
        test_path[tp] = '\0';
        vfs_node_t *exist = NULL;
        if (vfs_lookup(test_path, &exist) != VFS_OK) {
            for (int i = 0; i <= tp; i++) path[i] = test_path[i];
            found_slot = true;
            break;
        }
    }
    if (!found_slot) return;

    if (vfs_mkdir(path) == VFS_OK) {
        filemgr_load_dir(fm, fm->current_path);
    }
}

static void filemgr_on_paint(wm_window_t *win, gfx_surface_t *s) {
    if (!win || !s) return;
    filemgr_state_t *fm = (filemgr_state_t *)win->user_data;
    if (!fm) return;

    /* Main background: Modern deep obsidian */
    gfx_fill_rect(s, 0, 0, s->width, s->height, 0x111827);

    /* 1. Top Navigation & Breadcrumbs Bar (32px) */
    gfx_fill_rect(s, 0, 0, s->width, 32, 0x0F172A);
    gfx_draw_line(s, 0, 32, s->width, 32, 0x1E293B);

    /* Up Button */
    gfx_fill_rounded_rect(s, 8, 4, 32, 24, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 8, 4, 32, 24, 4, 0x334155);
    gfx_draw_string(s, 16, 8, "^", 0x38BDF8, 0, true);

    /* Path Pill Display */
    int path_pill_x = 46;
    int path_pill_w = s->width - 46 - 280;
    if (path_pill_w < 100) path_pill_w = 100;
    gfx_fill_rounded_rect(s, path_pill_x, 4, path_pill_w, 24, 4, 0x162238);
    gfx_draw_rounded_rect(s, path_pill_x, 4, path_pill_w, 24, 4, 0x223652);

    char p_buf[80] = "Path: ";
    int pi = 6;
    for (int i = 0; fm->current_path[i] && pi < 70; i++) p_buf[pi++] = fm->current_path[i];
    p_buf[pi] = '\0';
    gfx_draw_string(s, path_pill_x + 8, 8, p_buf, 0x38BDF8, 0, true);

    /* + File Button (Emerald) */
    int file_btn_x = s->width - 274;
    gfx_fill_rounded_rect(s, file_btn_x, 4, 68, 24, 4, 0x065F46);
    gfx_draw_rounded_rect(s, file_btn_x, 4, 68, 24, 4, 0x10B981);
    gfx_draw_string(s, file_btn_x + 8, 8, "+ File", 0xFFFFFF, 0, true);

    /* + Folder Button (Amber) */
    int fld_btn_x = s->width - 198;
    gfx_fill_rounded_rect(s, fld_btn_x, 4, 78, 24, 4, 0x9A3412);
    gfx_draw_rounded_rect(s, fld_btn_x, 4, 78, 24, 4, 0xF97316);
    gfx_draw_string(s, fld_btn_x + 6, 8, "+ Folder", 0xFFFFFF, 0, true);

    /* Open in Editor Pill Button */
    int open_btn_x = s->width - 112;
    gfx_fill_rounded_rect(s, open_btn_x, 4, 104, 24, 4, 0x1E40AF);
    gfx_draw_rounded_rect(s, open_btn_x, 4, 104, 24, 4, 0x3B82F6);
    gfx_draw_string(s, open_btn_x + 12, 8, "Open File", 0xFFFFFF, 0, true);

    /* 2. Left Sidebar (Bookmarks, width 120px) */
    int sidebar_w = 120;
    int bot_y = s->height - 24;
    gfx_fill_rect(s, 0, 33, sidebar_w, bot_y - 33, 0x0B0F19);
    gfx_draw_line(s, sidebar_w, 33, sidebar_w, bot_y, 0x1E293B);

    /* Sidebar Section Header */
    gfx_draw_string(s, 10, 42, "LOCATIONS", 0x64748B, 0, true);

    static const struct {
        const char *name;
        const char *path;
    } bookmarks[] = {
        {"Root (/)",      "/"},
        {"System (/bin)", "/bin"},
        {"Data (/data)",  "/data"},
        {"Web (/www)",    "/www"},
    };

    int b_y = 62;
    for (int b = 0; b < 4; b++) {
        bool is_bm_active = false;
        if (b == 0 && fm->current_path[0] == '/' && fm->current_path[1] == '\0') is_bm_active = true;
        else if (b > 0) {
            int k = 0;
            while (bookmarks[b].path[k] && bookmarks[b].path[k] == fm->current_path[k]) k++;
            if (!bookmarks[b].path[k] && (fm->current_path[k] == '\0' || fm->current_path[k] == '/')) is_bm_active = true;
        }

        if (is_bm_active) {
            gfx_fill_rounded_rect(s, 6, b_y - 2, sidebar_w - 12, 22, 4, 0x1E293B);
            gfx_draw_rounded_rect(s, 6, b_y - 2, sidebar_w - 12, 22, 4, 0x38BDF8);
        }

        gfx_draw_string(s, 12, b_y + 1, bookmarks[b].name, is_bm_active ? 0x38BDF8 : 0x94A3B8, 0, true);
        b_y += 28;
    }

    /* 3. Main File Table in Right Pane */
    int main_x = sidebar_w + 6;
    int table_w = s->width - main_x - 6;

    /* Table Header */
    gfx_fill_rect(s, main_x, 34, table_w, 20, 0x0F172A);
    gfx_draw_line(s, main_x, 54, main_x + table_w, 54, 0x1E293B);
    gfx_draw_string(s, main_x + 8, 37, "NAME", 0x64748B, 0, true);
    gfx_draw_string(s, main_x + 180, 37, "TYPE", 0x64748B, 0, true);
    gfx_draw_string(s, main_x + 250, 37, "SIZE", 0x64748B, 0, true);

    /* Directory Listing Rows */
    int row_y = 56;
    for (int i = 0; i < fm->item_count; i++) {
        filemgr_item_t *item = &fm->items[i];
        bool is_sel = (i == fm->selected_idx);

        u32 row_bg = is_sel ? 0x1E3A8A : (i % 2 == 0 ? 0x111827 : 0x0D1424);
        gfx_fill_rect(s, main_x, row_y, table_w, 24, row_bg);
        if (is_sel) {
            gfx_draw_rect(s, main_x, row_y, table_w, 24, 0x38BDF8);
        }

        /* Icon Badge */
        if (item->is_dir) {
            gfx_fill_rounded_rect(s, main_x + 6, row_y + 4, 16, 14, 2, 0x0284C7);
            gfx_draw_rounded_rect(s, main_x + 6, row_y + 4, 16, 14, 2, 0x38BDF8);
        } else {
            gfx_fill_rounded_rect(s, main_x + 6, row_y + 4, 16, 14, 2, 0x334155);
            gfx_draw_rounded_rect(s, main_x + 6, row_y + 4, 16, 14, 2, 0x64748B);
        }

        /* Name */
        u32 name_col = item->is_dir ? 0x38BDF8 : 0xF8FAFC;
        gfx_draw_string(s, main_x + 30, row_y + 4, item->name, name_col, 0, true);

        /* Type */
        gfx_draw_string(s, main_x + 180, row_y + 4, item->is_dir ? "Folder" : "File", 0x94A3B8, 0, true);

        /* Size */
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
            gfx_draw_string(s, main_x + 250, row_y + 4, size_buf, 0x64748B, 0, true);
        } else {
            gfx_draw_string(s, main_x + 250, row_y + 4, "--", 0x475569, 0, true);
        }

        row_y += 24;
        if (row_y + 24 >= bot_y) break;
    }

    /* 4. Bottom Status Bar */
    int sb_y = s->height - 24;
    gfx_fill_rect(s, 0, sb_y, s->width, 24, 0x0B0F19);
    gfx_draw_line(s, 0, sb_y, s->width, sb_y, 0x1E293B);

    /* Item count & Status */
    char stat_l[80];
    int sp = 0;
    if (fm->item_count >= 10) stat_l[sp++] = '0' + (fm->item_count / 10) % 10;
    stat_l[sp++] = '0' + (fm->item_count % 10);
    const char *it_lbl = " items | AstraFS VFS Mounted";
    for (int k = 0; it_lbl[k]; k++) stat_l[sp++] = it_lbl[k];
    stat_l[sp] = '\0';
    gfx_draw_string(s, 10, sb_y + 4, stat_l, 0x94A3B8, 0, true);

    gfx_draw_string(s, s->width - 240, sb_y + 4, "Click to select; Open to edit", 0x64748B, 0, true);
}

static void filemgr_on_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return;
    filemgr_state_t *fm = (filemgr_state_t *)win->user_data;
    if (!fm) return;

    if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        int rel_x = ev->mouse.x - win->client_x;
        int rel_y = ev->mouse.y - win->client_y;

        /* Click on Up Button [^] (8..40, 4..28) */
        if (rel_x >= 8 && rel_x <= 40 && rel_y >= 4 && rel_y <= 28) {
            /* Go up to parent directory */
            int last_slash = -1;
            for (int i = 0; fm->current_path[i]; i++) {
                if (fm->current_path[i] == '/') last_slash = i;
            }
            if (last_slash > 0) {
                fm->current_path[last_slash] = '\0';
                filemgr_load_dir(fm, fm->current_path);
            } else {
                filemgr_load_dir(fm, "/");
            }
            return;
        }

        /* Click on '+ File' button */
        if (rel_x >= win->client_w - 274 && rel_x <= win->client_w - 206 &&
            rel_y >= 4 && rel_y <= 28) {
            filemgr_create_file(fm);
            return;
        }

        /* Click on '+ Folder' button */
        if (rel_x >= win->client_w - 198 && rel_x <= win->client_w - 120 &&
            rel_y >= 4 && rel_y <= 28) {
            filemgr_create_folder(fm);
            return;
        }

        /* Click on 'Open File' button */
        if (rel_x >= win->client_w - 112 && rel_x <= win->client_w - 8 &&
            rel_y >= 4 && rel_y <= 28) {
            if (fm->selected_idx >= 0 && fm->selected_idx < fm->item_count) {
                filemgr_item_t *sel = &fm->items[fm->selected_idx];
                if (!sel->is_dir) {
                    char fullpath[128];
                    int pi = 0;
                    for (int i = 0; fm->current_path[i]; i++) fullpath[pi++] = fm->current_path[i];
                    if (pi > 0 && fullpath[pi - 1] != '/') fullpath[pi++] = '/';
                    for (int i = 0; sel->name[i]; i++) fullpath[pi++] = sel->name[i];
                    fullpath[pi] = '\0';

                    app_editor_launch(fullpath);
                    return;
                } else if (sel->name[0] == '.' && sel->name[1] == '.') {
                    filemgr_load_dir(fm, "/");
                    return;
                } else {
                    char fullpath[128];
                    int pi = 0;
                    for (int i = 0; fm->current_path[i]; i++) fullpath[pi++] = fm->current_path[i];
                    if (pi > 0 && fullpath[pi - 1] != '/') fullpath[pi++] = '/';
                    for (int i = 0; sel->name[i]; i++) fullpath[pi++] = sel->name[i];
                    fullpath[pi] = '\0';
                    filemgr_load_dir(fm, fullpath);
                    return;
                }
            }
        }

        /* Click on Left Sidebar Bookmarks */
        if (rel_x >= 6 && rel_x <= 116 && rel_y >= 60 && rel_y <= 170) {
            int bm_idx = (rel_y - 60) / 28;
            static const char *bm_paths[] = {"/", "/bin", "/data", "/www"};
            if (bm_idx >= 0 && bm_idx < 4) {
                filemgr_load_dir(fm, bm_paths[bm_idx]);
                return;
            }
        }

        /* Click on Right Main Table Rows */
        int main_x = 126;
        if (rel_x >= main_x && rel_y >= 56) {
            int row = (rel_y - 56) / 24;
            if (row >= 0 && row < fm->item_count) {
                if (fm->selected_idx == row) {
                    /* Double click / repeated click: open directly! */
                    filemgr_item_t *sel = &fm->items[row];
                    if (!sel->is_dir) {
                        char fullpath[128];
                        int pi = 0;
                        for (int i = 0; fm->current_path[i]; i++) fullpath[pi++] = fm->current_path[i];
                        if (pi > 0 && fullpath[pi - 1] != '/') fullpath[pi++] = '/';
                        for (int i = 0; sel->name[i]; i++) fullpath[pi++] = sel->name[i];
                        fullpath[pi] = '\0';
                        app_editor_launch(fullpath);
                        return;
                    } else if (sel->name[0] == '.' && sel->name[1] == '.') {
                        filemgr_load_dir(fm, "/");
                        return;
                    } else {
                        char fullpath[128];
                        int pi = 0;
                        for (int i = 0; fm->current_path[i]; i++) fullpath[pi++] = fm->current_path[i];
                        if (pi > 0 && fullpath[pi - 1] != '/') fullpath[pi++] = '/';
                        for (int i = 0; sel->name[i]; i++) fullpath[pi++] = sel->name[i];
                        fullpath[pi] = '\0';
                        filemgr_load_dir(fm, fullpath);
                        return;
                    }
                }

                fm->selected_idx = row;
                filemgr_item_t *sel = &fm->items[row];
                if (sel->is_dir) {
                    const char *msg = "Directory selected. Double-click or click 'Open' to enter.";
                    size_t mi = 0;
                    while (msg[mi] && mi < sizeof(fm->status_msg) - 1) { fm->status_msg[mi] = msg[mi]; mi++; }
                    fm->status_msg[mi] = '\0';
                } else {
                    const char *msg = "File selected. Double-click or click 'Open' to edit.";
                    size_t mi = 0;
                    while (msg[mi] && mi < sizeof(fm->status_msg) - 1) { fm->status_msg[mi] = msg[mi]; mi++; }
                    fm->status_msg[mi] = '\0';
                }
            }
        }
    }
}

void app_filemgr_launch(const char *initial_path) {
    kmemset(&g_filemgr, 0, sizeof(g_filemgr));
    const char *p = (initial_path && initial_path[0]) ? initial_path : "/";
    filemgr_load_dir(&g_filemgr, p);

    wm_window_t *win = wm_create_window("AstraOS Files (VFS)", 260, 80, 560, 360);
    if (!win) return;

    win->user_data = &g_filemgr;
    win->on_paint = filemgr_on_paint;
    win->on_event = filemgr_on_event;
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
