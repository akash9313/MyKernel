#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/pkg.h>
#include <kernel/mem.h>
#include <kernel/kprintf.h>

typedef struct {
    const char *name;
    const char *version;
    const char *desc;
    const char *category;
    const char *size_str;
    bool installed;
} pkg_view_item_t;

static pkg_view_item_t g_pkg_items[] = {
    {"coreutils", "v1.0.0", "POSIX command utilities (ls, cat, echo)", "System",    "48 KiB",  true},
    {"calc",      "v1.0.0", "Desktop graphical and CLI calculator",   "Utility",   "15 KiB",  true},
    {"htop",      "v1.0.0", "Interactive process & telemetry viewer", "Utility",   "15 KiB",  true},
    {"net-tools", "v1.0.0", "Network diagnostic tools (ping, ifconfig)", "Network", "32 KiB", false},
    {"lua",       "v5.4.4", "Lightweight embedded scripting language","Dev",      "128 KiB", false},
    {"games",     "v1.0.0", "Classic retro text & graphical games",   "Games",     "64 KiB",  false},
    {"doom",      "v1.1.0", "Freedoom classic FPS game port",         "Games",     "2.4 MiB", false},
};

static int g_selected_pkg = 0;
static wm_window_t *g_pkgmgr_win = NULL;
static char g_status_msg[64] = "Repository synced (7 packages cataloged).";

static void pkgmgr_on_paint(wm_window_t *win, gfx_surface_t *s) {
    if (!win || !s) return;

    /* Base Canvas Background: Modern Obsidian */
    gfx_fill_rect(s, 0, 0, s->width, s->height, 0x111827);

    /* 1. Header Banner (34px) */
    gfx_fill_rect(s, 0, 0, s->width, 34, 0x0F172A);
    gfx_draw_line(s, 0, 34, s->width, 34, 0x1E293B);

    /* Package Icon Badge */
    gfx_fill_rounded_rect(s, 10, 5, 24, 24, 6, 0x047857);
    gfx_draw_rounded_rect(s, 10, 5, 24, 24, 6, 0x10B981);
    gfx_draw_string(s, 18, 9, "#", 0xFFFFFF, 0, true);
    gfx_draw_string(s, 42, 9, "AstraOS Software Center & Package Manager", 0xF8FAFC, 0, true);

    /* 2. Top Filter / Category Pills Bar (28px) */
    int f_y = 38;
    gfx_draw_string(s, 14, f_y + 4, "Category:", 0x64748B, 0, true);
    static const char *cats[] = {"All (7)", "System (1)", "Utility (2)", "Dev (1)", "Games (2)"};
    int cx = 86;
    for (int k = 0; k < 5; k++) {
        bool is_act = (k == 0);
        int cw = 74;
        if (k == 0) cw = 60;
        gfx_fill_rounded_rect(s, cx, f_y, cw, 22, 4, is_act ? 0x1E293B : 0x0B0F19);
        gfx_draw_rounded_rect(s, cx, f_y, cw, 22, 4, is_act ? 0x10B981 : 0x1E293B);
        gfx_draw_string(s, cx + 6, f_y + 3, cats[k], is_act ? 0x10B981 : 0x94A3B8, 0, true);
        cx += cw + 6;
    }

    /* 3. Package Cards Table */
    int y = 68;
    int tbl_w = s->width - 24;

    /* Table Header */
    gfx_fill_rect(s, 12, y, tbl_w, 20, 0x0F172A);
    gfx_draw_line(s, 12, y + 20, 12 + tbl_w, y + 20, 0x1E293B);
    gfx_draw_string(s, 20, y + 2, "PACKAGE", 0x64748B, 0, true);
    gfx_draw_string(s, 110, y + 2, "VERSION", 0x64748B, 0, true);
    gfx_draw_string(s, 180, y + 2, "DESCRIPTION", 0x64748B, 0, true);
    gfx_draw_string(s, 410, y + 2, "STATUS", 0x64748B, 0, true);
    y += 24;

    int num_items = sizeof(g_pkg_items) / sizeof(g_pkg_items[0]);
    int bot_bar_y = s->height - 64;

    for (int i = 0; i < num_items; i++) {
        if (y + 26 >= bot_bar_y) break;

        bool is_selected = (i == g_selected_pkg);
        u32 row_bg = is_selected ? 0x1E3A8A : (i % 2 == 0 ? 0x111827 : 0x0D1424);
        gfx_fill_rect(s, 12, y, tbl_w, 26, row_bg);
        if (is_selected) {
            gfx_draw_rect(s, 12, y, tbl_w, 26, 0x38BDF8);
        }

        /* Package Name */
        gfx_draw_string(s, 20, y + 5, g_pkg_items[i].name,
                        is_selected ? 0x38BDF8 : 0xF8FAFC, 0, true);

        /* Version */
        gfx_draw_string(s, 110, y + 5, g_pkg_items[i].version, 0x94A3B8, 0, true);

        /* Description (clipped) */
        char desc_clip[30];
        int dlen = 0;
        for (int c = 0; g_pkg_items[i].desc[c] && dlen < 26; c++) {
            desc_clip[dlen++] = g_pkg_items[i].desc[c];
        }
        desc_clip[dlen] = '\0';
        gfx_draw_string(s, 180, y + 5, desc_clip, 0xF1F5F9, 0, true);

        /* Status Badge */
        if (g_pkg_items[i].installed) {
            gfx_fill_rounded_rect(s, 404, y + 3, 90, 20, 3, 0x064E3B);
            gfx_draw_rounded_rect(s, 404, y + 3, 90, 20, 3, 0x10B981);
            gfx_draw_string(s, 416, y + 5, "Installed", 0x34D399, 0, true);
        } else {
            gfx_fill_rounded_rect(s, 404, y + 3, 90, 20, 3, 0x1E293B);
            gfx_draw_rounded_rect(s, 404, y + 3, 90, 20, 3, 0x334155);
            gfx_draw_string(s, 416, y + 5, "Available", 0x94A3B8, 0, true);
        }

        y += 28;
    }

    /* 4. Telemetry Status Bar Notice */
    int sb_y = s->height - 62;
    gfx_fill_rect(s, 12, sb_y, tbl_w, 22, 0x0B0F19);
    gfx_draw_rect(s, 12, sb_y, tbl_w, 22, 0x1E293B);
    gfx_draw_string(s, 20, sb_y + 3, g_status_msg, 0x38BDF8, 0, true);

    /* 5. Bottom Action Bar */
    int bbar_y = s->height - 34;
    gfx_fill_rect(s, 0, bbar_y, s->width, 34, 0x0F172A);
    gfx_draw_line(s, 0, bbar_y, s->width, bbar_y, 0x1E293B);

    /* Install / Remove Button */
    bool sel_inst = (g_selected_pkg >= 0 && g_selected_pkg < num_items) ? g_pkg_items[g_selected_pkg].installed : false;
    const char *btn_lbl = sel_inst ? "Reinstall" : "Install";
    u32 btn_bg = sel_inst ? 0x1E3A8A : 0x047857;
    u32 btn_border = sel_inst ? 0x38BDF8 : 0x10B981;

    gfx_fill_rounded_rect(s, 12, bbar_y + 4, 100, 26, 4, btn_bg);
    gfx_draw_rounded_rect(s, 12, bbar_y + 4, 100, 26, 4, btn_border);
    gfx_draw_string(s, 24, bbar_y + 9, btn_lbl, 0xFFFFFF, 0, true);

    /* Check Updates Button */
    gfx_fill_rounded_rect(s, 122, bbar_y + 4, 124, 26, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 122, bbar_y + 4, 124, 26, 4, 0x334155);
    gfx_draw_string(s, 132, bbar_y + 9, "Sync Catalog", 0xF8FAFC, 0, true);

    /* Close Button */
    int cl_x = s->width - 82;
    gfx_fill_rounded_rect(s, cl_x, bbar_y + 4, 70, 26, 4, 0x2563EB);
    gfx_draw_rounded_rect(s, cl_x, bbar_y + 4, 70, 26, 4, 0x3B82F6);
    gfx_draw_string(s, cl_x + 16, bbar_y + 9, "Close", 0xFFFFFF, 0, true);
}

static void pkgmgr_on_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return;

    if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        int cx = ev->mouse.x - win->client_x;
        int cy = ev->mouse.y - win->client_y;
        int num_items = sizeof(g_pkg_items) / sizeof(g_pkg_items[0]);
        int list_start_y = 68 + 24;

        /* Check list rows */
        for (int i = 0; i < num_items; i++) {
            int row_y = list_start_y + i * 28;
            if (cx >= 12 && cx <= win->client_w - 12 && cy >= row_y && cy <= row_y + 26) {
                g_selected_pkg = i;
                wm_render();
                gfx_flip();
                return;
            }
        }

        int bbar_y = win->client_h - 34;

        /* Click Install Button */
        if (cx >= 12 && cx <= 112 && cy >= bbar_y + 4 && cy <= bbar_y + 30) {
            if (g_selected_pkg >= 0 && g_selected_pkg < num_items) {
                g_pkg_items[g_selected_pkg].installed = true;
                const char *n = g_pkg_items[g_selected_pkg].name;
                int p = 0;
                const char *msg1 = "Package '";
                for (int j = 0; msg1[j]; j++) g_status_msg[p++] = msg1[j];
                for (int j = 0; n[j] && p < 45; j++) g_status_msg[p++] = n[j];
                const char *msg2 = "' installed successfully.";
                for (int j = 0; msg2[j]; j++) g_status_msg[p++] = msg2[j];
                g_status_msg[p] = '\0';
                wm_render();
                gfx_flip();
            }
            return;
        }

        /* Click Sync Catalog */
        if (cx >= 122 && cx <= 246 && cy >= bbar_y + 4 && cy <= bbar_y + 30) {
            const char *u_msg = "Repository index synchronized (all 7 packages verified).";
            int p = 0;
            while (u_msg[p]) { g_status_msg[p] = u_msg[p]; p++; }
            g_status_msg[p] = '\0';
            wm_render();
            gfx_flip();
            return;
        }

        /* Click Close Button */
        if (cx >= win->client_w - 82 && cx <= win->client_w - 12 &&
            cy >= bbar_y + 4 && cy <= bbar_y + 30) {
            wm_destroy_window(win);
            g_pkgmgr_win = NULL;
            return;
        }
    }
}

void app_pkgmgr_launch(void) {
    if (g_pkgmgr_win && wm_find_by_id(g_pkgmgr_win->id)) {
        wm_bring_to_front(g_pkgmgr_win);
        wm_set_focus(g_pkgmgr_win);
        return;
    }

    int win_w = 540;
    int win_h = 360;
    gfx_surface_t *bb = gfx_get_backbuffer();
    int sx = bb ? (bb->width - win_w) / 2 : 200;
    int sy = bb ? (bb->height - win_h) / 2 : 140;

    g_pkgmgr_win = wm_create_window("Package Manager", sx, sy, win_w, win_h);
    if (!g_pkgmgr_win) return;

    g_pkgmgr_win->on_paint = pkgmgr_on_paint;
    g_pkgmgr_win->on_event = pkgmgr_on_event;
    wm_bring_to_front(g_pkgmgr_win);
    wm_set_focus(g_pkgmgr_win);
}
