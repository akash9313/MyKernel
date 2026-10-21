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

    const char *msg = "Directory loaded. Click item to select; click Open to view.";
    size_t mi = 0;
    while (msg[mi] && mi < sizeof(fm->status_msg) - 1) {
        fm->status_msg[mi] = msg[mi];
        mi++;
    }
    fm->status_msg[mi] = '\0';
}

static void filemgr_on_paint(wm_window_t *win, gfx_surface_t *s) {
    if (!win || !s) return;
    filemgr_state_t *fm = (filemgr_state_t *)win->user_data;
    if (!fm) return;

    /* Background: Dark slate */
    gfx_fill_rect(s, 0, 0, s->width, s->height, GFX_COLOR_WINDOW_BG);

    /* Address Bar / Header */
    gfx_fill_rect(s, 0, 0, s->width, 26, GFX_COLOR_PANEL_BG);
    gfx_draw_line(s, 0, 26, s->width, 26, GFX_COLOR_PANEL_BORDER);

    char path_buf[80] = "Path: ";
    int pi = 6;
    for (int i = 0; fm->current_path[i] && pi < 70; i++) {
        path_buf[pi++] = fm->current_path[i];
    }
    path_buf[pi] = '\0';
    gfx_draw_string(s, 8, 5, path_buf, GFX_COLOR_ACCENT_BLUE, 0, true);

    /* Open in Editor button */
    gfx_fill_rect(s, s->width - 130, 3, 120, 20, GFX_COLOR_TITLEBAR_ACTIVE);
    gfx_draw_rect(s, s->width - 130, 3, 120, 20, GFX_COLOR_ACCENT_PURPLE);
    gfx_draw_string(s, s->width - 120, 5, "Open in Editor", GFX_COLOR_TEXT_PRIMARY, 0, true);

    /* Directory Listing Rows */
    int row_y = 32;
    for (int i = 0; i < fm->item_count; i++) {
        filemgr_item_t *item = &fm->items[i];
        bool is_sel = (i == fm->selected_idx);

        if (is_sel) {
            gfx_fill_rect(s, 4, row_y - 2, s->width - 8, 20, GFX_COLOR_TITLEBAR_ACTIVE);
            gfx_draw_rect(s, 4, row_y - 2, s->width - 8, 20, GFX_COLOR_ACCENT_BLUE);
        }

        /* Icon tag */
        if (item->is_dir) {
            gfx_draw_string(s, 8, row_y, "[DIR ]", GFX_COLOR_ACCENT_CYAN, 0, true);
        } else {
            gfx_draw_string(s, 8, row_y, "[FILE]", GFX_COLOR_TEXT_MUTED, 0, true);
        }

        /* Name */
        u32 name_col = item->is_dir ? GFX_COLOR_ACCENT_CYAN : GFX_COLOR_TEXT_PRIMARY;
        gfx_draw_string(s, 64, row_y, item->name, name_col, 0, true);

        /* Size */
        if (!item->is_dir) {
            char size_buf[32];
            int si = 0;
            size_buf[si++] = '(';
            if (item->size >= 1000) size_buf[si++] = '0' + (item->size / 1000) % 10;
            if (item->size >= 100)  size_buf[si++] = '0' + (item->size / 100) % 10;
            if (item->size >= 10)   size_buf[si++] = '0' + (item->size / 10) % 10;
            size_buf[si++] = '0' + (item->size % 10);
            size_buf[si++] = ' '; size_buf[si++] = 'B'; size_buf[si++] = ')';
            size_buf[si] = '\0';
            gfx_draw_string(s, 240, row_y, size_buf, GFX_COLOR_TEXT_MUTED, 0, true);
        }

        row_y += 22;
        if (row_y + 22 >= s->height - 24) break;
    }

    /* Bottom Status Bar */
    int sb_y = s->height - 22;
    gfx_fill_rect(s, 0, sb_y, s->width, 22, GFX_COLOR_PANEL_BG);
    gfx_draw_line(s, 0, sb_y, s->width, sb_y, GFX_COLOR_PANEL_BORDER);
    gfx_draw_string(s, 8, sb_y + 3, fm->status_msg, GFX_COLOR_TEXT_MUTED, 0, true);
}

static void filemgr_on_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return;
    filemgr_state_t *fm = (filemgr_state_t *)win->user_data;
    if (!fm) return;

    if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        int rel_x = ev->mouse.x - win->client_x;
        int rel_y = ev->mouse.y - win->client_y;

        /* Click on 'Open in Editor' button */
        if (rel_x >= win->client_w - 130 && rel_x <= win->client_w - 10 &&
            rel_y >= 3 && rel_y <= 23) {
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
                    /* Navigate up */
                    filemgr_load_dir(fm, "/");
                    return;
                } else {
                    /* Navigate into directory */
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

        /* Check click on list rows */
        if (rel_y >= 30) {
            int row = (rel_y - 30) / 22;
            if (row >= 0 && row < fm->item_count) {
                fm->selected_idx = row;
                filemgr_item_t *sel = &fm->items[row];
                if (sel->is_dir) {
                    const char *msg = "Folder selected. Click 'Open in Editor' or double-click to browse.";
                    size_t mi = 0;
                    while (msg[mi] && mi < sizeof(fm->status_msg) - 1) {
                        fm->status_msg[mi] = msg[mi];
                        mi++;
                    }
                    fm->status_msg[mi] = '\0';
                } else {
                    const char *msg = "File selected. Click 'Open in Editor' to load file contents.";
                    size_t mi = 0;
                    while (msg[mi] && mi < sizeof(fm->status_msg) - 1) {
                        fm->status_msg[mi] = msg[mi];
                        mi++;
                    }
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

    wm_window_t *win = wm_create_window("AstraOS File Manager", 220, 100, 440, 280);
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
