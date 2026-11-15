#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/pmm.h>
#include <kernel/net.h>
#include <kernel/rtc.h>
#include <kernel/smp.h>
#include <kernel/mem.h>
#include <kernel/kprintf.h>

static wm_window_t *g_settings_win = NULL;
static int g_settings_tab = 0; /* 0: Display, 1: System, 2: Hardware, 3: Network, 4: About */

static void settings_on_paint(wm_window_t *win, gfx_surface_t *s) {
    if (!win || !s) return;

    /* Base Canvas Background: Modern Obsidian */
    gfx_fill_rect(s, 0, 0, s->width, s->height, 0x111827);

    /* 1. Header Banner (34px) */
    gfx_fill_rect(s, 0, 0, s->width, 34, 0x0F172A);
    gfx_draw_line(s, 0, 34, s->width, 34, 0x1E293B);

    /* Titanium Gear Emblem */
    gfx_fill_rounded_rect(s, 10, 5, 24, 24, 6, 0x334155);
    gfx_draw_rounded_rect(s, 10, 5, 24, 24, 6, 0x64748B);
    gfx_draw_string(s, 18, 9, "*", 0x38BDF8, 0, true);
    gfx_draw_string(s, 42, 9, "AstraOS System Preferences & Settings", 0xF8FAFC, 0, true);

    /* 2. Left Category Navigation Sidebar (130px) */
    int side_w = 130;
    int bot_y = s->height - 30;
    gfx_fill_rect(s, 0, 35, side_w, bot_y - 35, 0x0B0F19);
    gfx_draw_line(s, side_w, 35, side_w, bot_y, 0x1E293B);

    static const char *tab_names[] = {
        "Display & HD",
        "System & Core",
        "Memory (RAM)",
        "Network & TLS",
        "About AstraOS"
    };

    for (int t = 0; t < 5; t++) {
        int ty = 46 + t * 30;
        bool is_sel = (t == g_settings_tab);

        if (is_sel) {
            gfx_fill_rounded_rect(s, 6, ty - 2, side_w - 12, 24, 4, 0x1E293B);
            gfx_draw_rounded_rect(s, 6, ty - 2, side_w - 12, 24, 4, 0x38BDF8);
        }

        u32 t_col = is_sel ? 0x38BDF8 : 0x94A3B8;
        gfx_draw_string(s, 12, ty + 2, tab_names[t], t_col, 0, true);
    }

    /* 3. Right Content Pane */
    int pane_x = side_w + 12;
    int pane_w = s->width - pane_x - 12;
    int cur_y = 46;

    if (g_settings_tab == 0) {
        /* TAB 0: Display & HD */
        gfx_fill_rounded_rect(s, pane_x, cur_y, pane_w, 190, 6, 0x0F172A);
        gfx_draw_rounded_rect(s, pane_x, cur_y, pane_w, 190, 6, 0x1E293B);

        gfx_draw_string(s, pane_x + 14, cur_y + 12, "Display & Framebuffer Pipeline", 0x38BDF8, 0, true);
        gfx_draw_line(s, pane_x + 14, cur_y + 30, pane_x + pane_w - 14, cur_y + 30, 0x1E293B);

        int dy = cur_y + 40;
        gfx_draw_string(s, pane_x + 14, dy, "Current Resolution:", 0x94A3B8, 0, true);
        gfx_draw_string(s, pane_x + 160, dy, "1920 x 1080 Full HD (1080p)", 0x10B981, 0, true);
        dy += 22;

        gfx_draw_string(s, pane_x + 14, dy, "Color Format:", 0x94A3B8, 0, true);
        gfx_draw_string(s, pane_x + 160, dy, "32-bit True Color (XRGB / BGRA)", 0xF8FAFC, 0, true);
        dy += 22;

        gfx_draw_string(s, pane_x + 14, dy, "Video Driver:", 0x94A3B8, 0, true);
        gfx_draw_string(s, pane_x + 160, dy, "EFI GOP + Contiguous Double-Buffer", 0xF8FAFC, 0, true);
        dy += 22;

        gfx_draw_string(s, pane_x + 14, dy, "Desktop Wallpaper:", 0x94A3B8, 0, true);
        gfx_draw_string(s, pane_x + 160, dy, "Cosmic Vista (Preprocessed XRGB)", 0xA855F7, 0, true);
        dy += 22;

        gfx_draw_string(s, pane_x + 14, dy, "Aspect Ratio:", 0x94A3B8, 0, true);
        gfx_draw_string(s, pane_x + 160, dy, "16:9 Widescreen Standard", 0xF8FAFC, 0, true);
    } else if (g_settings_tab == 1) {
        /* TAB 1: System & Core */
        gfx_fill_rounded_rect(s, pane_x, cur_y, pane_w, 190, 6, 0x0F172A);
        gfx_draw_rounded_rect(s, pane_x, cur_y, pane_w, 190, 6, 0x1E293B);

        gfx_draw_string(s, pane_x + 14, cur_y + 12, "Operating System & Kernel Architecture", 0x38BDF8, 0, true);
        gfx_draw_line(s, pane_x + 14, cur_y + 30, pane_x + pane_w - 14, cur_y + 30, 0x1E293B);

        int dy = cur_y + 40;
        gfx_draw_string(s, pane_x + 14, dy, "Operating System:", 0x94A3B8, 0, true);
        gfx_draw_string(s, pane_x + 160, dy, "AstraOS v1.0.0 (Release Candidate)", 0xF8FAFC, 0, true);
        dy += 22;

        gfx_draw_string(s, pane_x + 14, dy, "Target Architecture:", 0x94A3B8, 0, true);
        gfx_draw_string(s, pane_x + 160, dy, "x86-64 Freestanding Long Mode", 0xF8FAFC, 0, true);
        dy += 22;

        gfx_draw_string(s, pane_x + 14, dy, "Bootloader Protocol:", 0x94A3B8, 0, true);
        gfx_draw_string(s, pane_x + 160, dy, "Limine UEFI Boot Protocol v3", 0xF8FAFC, 0, true);
        dy += 22;

        gfx_draw_string(s, pane_x + 14, dy, "SMP Multi-Processing:", 0x94A3B8, 0, true);
        int cores = smp_get_cpu_count();
        char c_buf[32] = "Active (";
        int cp = 8;
        c_buf[cp++] = '0' + (cores % 10);
        c_buf[cp++] = ' '; c_buf[cp++] = 'C'; c_buf[cp++] = 'P'; c_buf[cp++] = 'U'; c_buf[cp++] = 's'; c_buf[cp++] = ')'; c_buf[cp] = '\0';
        gfx_draw_string(s, pane_x + 160, dy, c_buf, 0x10B981, 0, true);
        dy += 22;

        gfx_draw_string(s, pane_x + 14, dy, "Process Security:", 0x94A3B8, 0, true);
        gfx_draw_string(s, pane_x + 160, dy, "Ring 3 Separation & Guard Page Enabled", 0xF8FAFC, 0, true);
    } else if (g_settings_tab == 2) {
        /* TAB 2: Memory (RAM) */
        gfx_fill_rounded_rect(s, pane_x, cur_y, pane_w, 190, 6, 0x0F172A);
        gfx_draw_rounded_rect(s, pane_x, cur_y, pane_w, 190, 6, 0x1E293B);

        gfx_draw_string(s, pane_x + 14, cur_y + 12, "Physical & Virtual Memory Management", 0x38BDF8, 0, true);
        gfx_draw_line(s, pane_x + 14, cur_y + 30, pane_x + pane_w - 14, cur_y + 30, 0x1E293B);

        struct pmm_stats st;
        pmm_get_stats(&st);
        u64 used_mb = (st.allocated_pages * 4096) / (1024 * 1024);
        u64 total_mb = (st.total_managed_pages * 4096) / (1024 * 1024);
        if (total_mb == 0) total_mb = 1;

        int dy = cur_y + 40;
        gfx_draw_string(s, pane_x + 14, dy, "Memory Usage Meter:", 0x94A3B8, 0, true);
        dy += 20;

        /* Progress Bar */
        int bar_w = pane_w - 28;
        gfx_fill_rounded_rect(s, pane_x + 14, dy, bar_w, 16, 4, 0x0B0F19);
        gfx_draw_rounded_rect(s, pane_x + 14, dy, bar_w, 16, 4, 0x1E293B);

        int fill_w = (int)((used_mb * (u64)(bar_w - 4)) / total_mb);
        if (fill_w < 4) fill_w = 4;
        if (fill_w > bar_w - 4) fill_w = bar_w - 4;
        gfx_fill_rounded_rect(s, pane_x + 16, dy + 2, fill_w, 12, 3, 0x2563EB);
        dy += 26;

        char stat_str[64];
        int sp = 0;
        const char *m_lbl = "Allocated: ";
        for (int k = 0; m_lbl[k]; k++) stat_str[sp++] = m_lbl[k];
        if (used_mb >= 100) stat_str[sp++] = '0' + (used_mb / 100) % 10;
        if (used_mb >= 10)  stat_str[sp++] = '0' + (used_mb / 10) % 10;
        stat_str[sp++] = '0' + (used_mb % 10);
        stat_str[sp++] = ' '; stat_str[sp++] = 'M'; stat_str[sp++] = 'B';
        const char *t_lbl = " / Total: ";
        for (int k = 0; t_lbl[k]; k++) stat_str[sp++] = t_lbl[k];
        if (total_mb >= 100) stat_str[sp++] = '0' + (total_mb / 100) % 10;
        if (total_mb >= 10)  stat_str[sp++] = '0' + (total_mb / 10) % 10;
        stat_str[sp++] = '0' + (total_mb % 10);
        stat_str[sp++] = ' '; stat_str[sp++] = 'M'; stat_str[sp++] = 'B';
        stat_str[sp] = '\0';
        gfx_draw_string(s, pane_x + 14, dy, stat_str, 0xF8FAFC, 0, true);
        dy += 22;

        gfx_draw_string(s, pane_x + 14, dy, "Allocator Engine:", 0x94A3B8, 0, true);
        gfx_draw_string(s, pane_x + 160, dy, "PMM Buddy / Slab Kernel Heap", 0x10B981, 0, true);
    } else if (g_settings_tab == 3) {
        /* TAB 3: Network & TLS */
        gfx_fill_rounded_rect(s, pane_x, cur_y, pane_w, 190, 6, 0x0F172A);
        gfx_draw_rounded_rect(s, pane_x, cur_y, pane_w, 190, 6, 0x1E293B);

        gfx_draw_string(s, pane_x + 14, cur_y + 12, "Network Adapter & Stack Telemetry", 0x38BDF8, 0, true);
        gfx_draw_line(s, pane_x + 14, cur_y + 30, pane_x + pane_w - 14, cur_y + 30, 0x1E293B);

        net_if_t *nif = net_get_interface();
        int dy = cur_y + 40;

        gfx_draw_string(s, pane_x + 14, dy, "Interface Status:", 0x94A3B8, 0, true);
        if (nif && nif->link_up && nif->configured) {
            gfx_draw_string(s, pane_x + 160, dy, "Online (Connected)", 0x10B981, 0, true);
        } else if (nif && nif->link_up) {
            gfx_draw_string(s, pane_x + 160, dy, "Link Up (Negotiating DHCP)", 0x38BDF8, 0, true);
        } else {
            gfx_draw_string(s, pane_x + 160, dy, "Disconnected / No Link", 0xEF4444, 0, true);
        }
        dy += 22;

        gfx_draw_string(s, pane_x + 14, dy, "IPv4 Address:", 0x94A3B8, 0, true);
        if (nif && nif->configured) {
            char ip_buf[20];
            ip_to_str(nif->ip, ip_buf, sizeof(ip_buf));
            gfx_draw_string(s, pane_x + 160, dy, ip_buf, 0xF8FAFC, 0, true);
        } else {
            gfx_draw_string(s, pane_x + 160, dy, "0.0.0.0", 0x64748B, 0, true);
        }
        dy += 22;

        gfx_draw_string(s, pane_x + 14, dy, "Hardware NIC:", 0x94A3B8, 0, true);
        gfx_draw_string(s, pane_x + 160, dy, "Intel 82540EM / AMD PCnet (PCI)", 0xF8FAFC, 0, true);
        dy += 22;

        gfx_draw_string(s, pane_x + 14, dy, "Crypto & TLS Engine:", 0x94A3B8, 0, true);
        gfx_draw_string(s, pane_x + 160, dy, "TLS 1.3 / AES-256-GCM / SHA-256", 0x10B981, 0, true);
    } else {
        /* TAB 4: About AstraOS */
        gfx_fill_rounded_rect(s, pane_x, cur_y, pane_w, 190, 6, 0x0F172A);
        gfx_draw_rounded_rect(s, pane_x, cur_y, pane_w, 190, 6, 0x1E293B);

        gfx_draw_string(s, pane_x + 14, cur_y + 12, "About AstraOS", 0x38BDF8, 0, true);
        gfx_draw_line(s, pane_x + 14, cur_y + 30, pane_x + pane_w - 14, cur_y + 30, 0x1E293B);

        int dy = cur_y + 42;
        gfx_draw_string(s, pane_x + 14, dy, "AstraOS - Industrial Operating System", 0xF8FAFC, 0, true);
        dy += 22;
        gfx_draw_string(s, pane_x + 14, dy, "Version 1.0.0 (x86-64 Freestanding)", 0x94A3B8, 0, true);
        dy += 22;
        gfx_draw_string(s, pane_x + 14, dy, "Engineering: Google DeepMind Antigravity Team", 0x38BDF8, 0, true);
        dy += 22;
        gfx_draw_string(s, pane_x + 14, dy, "Wallpaper: AstraOS Cosmic Vista HD", 0xA855F7, 0, true);
        dy += 22;
        gfx_draw_string(s, pane_x + 14, dy, "(c) 2026 AstraOS Foundation. All rights reserved.", 0x64748B, 0, true);
    }

    /* 4. Bottom Action Bar */
    int bbar_y = s->height - 28;
    gfx_fill_rect(s, 0, bbar_y, s->width, 28, 0x0B0F19);
    gfx_draw_line(s, 0, bbar_y, s->width, bbar_y, 0x1E293B);

    /* Refresh Button */
    gfx_fill_rounded_rect(s, 10, bbar_y + 2, 110, 24, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 10, bbar_y + 2, 110, 24, 4, 0x334155);
    gfx_draw_string(s, 20, bbar_y + 6, "Refresh", 0xF8FAFC, 0, true);

    /* Close Button */
    int cl_x = s->width - 80;
    gfx_fill_rounded_rect(s, cl_x, bbar_y + 2, 70, 24, 4, 0x2563EB);
    gfx_draw_rounded_rect(s, cl_x, bbar_y + 2, 70, 24, 4, 0x3B82F6);
    gfx_draw_string(s, cl_x + 16, bbar_y + 6, "Close", 0xFFFFFF, 0, true);
}

static void settings_on_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return;

    if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        int cx = ev->mouse.x - win->client_x;
        int cy = ev->mouse.y - win->client_y;
        int bbar_y = win->client_h - 28;

        /* Click on Left Sidebar Tabs */
        if (cx >= 6 && cx <= 124 && cy >= 44 && cy <= 196) {
            int clicked_tab = (cy - 44) / 30;
            if (clicked_tab >= 0 && clicked_tab < 5) {
                g_settings_tab = clicked_tab;
                wm_render();
                gfx_flip();
                return;
            }
        }

        /* Click Refresh Stats */
        if (cx >= 10 && cx <= 120 && cy >= bbar_y + 2 && cy <= bbar_y + 26) {
            wm_render();
            gfx_flip();
            return;
        }

        /* Click Close */
        if (cx >= win->client_w - 80 && cx <= win->client_w - 10 &&
            cy >= bbar_y + 2 && cy <= bbar_y + 26) {
            wm_destroy_window(win);
            g_settings_win = NULL;
            return;
        }
    }
}

void app_settings_launch(void) {
    if (g_settings_win && wm_find_by_id(g_settings_win->id)) {
        wm_bring_to_front(g_settings_win);
        wm_set_focus(g_settings_win);
        return;
    }

    int win_w = 480;
    int win_h = 280;
    gfx_surface_t *bb = gfx_get_backbuffer();
    int sx = bb ? (bb->width - win_w) / 2 : 200;
    int sy = bb ? (bb->height - win_h) / 2 - 20 : 150;

    g_settings_win = wm_create_window("System Settings", sx, sy, win_w, win_h);
    if (!g_settings_win) return;

    g_settings_win->on_paint = settings_on_paint;
    g_settings_win->on_event = settings_on_event;
    wm_bring_to_front(g_settings_win);
    wm_set_focus(g_settings_win);
}
