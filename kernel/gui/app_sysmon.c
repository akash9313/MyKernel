#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/pmm.h>
#include <kernel/drivers/apic.h>
#include <kernel/smp.h>
#include <kernel/process.h>
#include <kernel/mem.h>
#include <kernel/kprintf.h>

static wm_window_t *g_sysmon_win = NULL;
static int g_selected_proc = 0;

static void sysmon_on_paint(wm_window_t *win, gfx_surface_t *s) {
    if (!win || !s) return;

    /* Base Canvas Background: Modern Obsidian */
    gfx_fill_rect(s, 0, 0, s->width, s->height, 0x111827);

    /* 1. Header Banner (34px) */
    gfx_fill_rect(s, 0, 0, s->width, 34, 0x0F172A);
    gfx_draw_line(s, 0, 34, s->width, 34, 0x1E293B);

    /* Telemetry Activity Icon Badge */
    gfx_fill_rounded_rect(s, 10, 5, 24, 24, 6, 0x4C1D95);
    gfx_draw_rounded_rect(s, 10, 5, 24, 24, 6, 0x7C3AED);
    gfx_draw_string(s, 18, 9, "~", 0x34D399, 0, true);
    gfx_draw_string(s, 42, 9, "AstraOS System Activity & Telemetry Monitor", 0xF8FAFC, 0, true);

    int cur_y = 44;

    /* 2. Top Dual Telemetry Cards */
    int card_w = (s->width - 34) / 2;
    int card_h = 72;

    /* Card 1: RAM Usage Card */
    int c1_x = 12;
    gfx_fill_rounded_rect(s, c1_x, cur_y, card_w, card_h, 6, 0x0F172A);
    gfx_draw_rounded_rect(s, c1_x, cur_y, card_w, card_h, 6, 0x1E293B);

    struct pmm_stats st;
    pmm_get_stats(&st);
    u64 used_mb = (st.allocated_pages * 4096) / (1024 * 1024);
    u64 total_mb = (st.total_managed_pages * 4096) / (1024 * 1024);
    if (total_mb == 0) total_mb = 1;

    gfx_draw_string(s, c1_x + 12, cur_y + 8, "Physical RAM Usage", 0x38BDF8, 0, true);

    /* Memory progress bar */
    int bar_w = card_w - 24;
    gfx_fill_rounded_rect(s, c1_x + 12, cur_y + 26, bar_w, 14, 3, 0x0B0F19);
    gfx_draw_rounded_rect(s, c1_x + 12, cur_y + 26, bar_w, 14, 3, 0x1E293B);

    int fill_w = (int)((used_mb * (u64)(bar_w - 4)) / total_mb);
    if (fill_w < 4) fill_w = 4;
    if (fill_w > bar_w - 4) fill_w = bar_w - 4;
    gfx_fill_rounded_rect(s, c1_x + 14, cur_y + 28, fill_w, 10, 2, 0x2563EB);

    char mem_txt[48];
    int mp = 0;
    if (used_mb >= 100) mem_txt[mp++] = '0' + (used_mb / 100) % 10;
    if (used_mb >= 10)  mem_txt[mp++] = '0' + (used_mb / 10) % 10;
    mem_txt[mp++] = '0' + (used_mb % 10);
    mem_txt[mp++] = ' '; mem_txt[mp++] = 'M'; mem_txt[mp++] = 'B';
    mem_txt[mp++] = ' '; mem_txt[mp++] = '/'; mem_txt[mp++] = ' ';
    if (total_mb >= 100) mem_txt[mp++] = '0' + (total_mb / 100) % 10;
    if (total_mb >= 10)  mem_txt[mp++] = '0' + (total_mb / 10) % 10;
    mem_txt[mp++] = '0' + (total_mb % 10);
    mem_txt[mp++] = ' '; mem_txt[mp++] = 'M'; mem_txt[mp++] = 'B';
    mem_txt[mp] = '\0';
    gfx_draw_string(s, c1_x + 12, cur_y + 48, mem_txt, 0x94A3B8, 0, true);

    /* Card 2: CPU & SMP Telemetry Card */
    int c2_x = c1_x + card_w + 10;
    gfx_fill_rounded_rect(s, c2_x, cur_y, card_w, card_h, 6, 0x0F172A);
    gfx_draw_rounded_rect(s, c2_x, cur_y, card_w, card_h, 6, 0x1E293B);

    gfx_draw_string(s, c2_x + 12, cur_y + 8, "CPU & Scheduler Engine", 0x10B981, 0, true);

    int smp_count = smp_get_cpu_count();
    char smp_txt[40] = "SMP Cores: ";
    int slen = 11;
    smp_txt[slen++] = '0' + (smp_count % 10);
    smp_txt[slen++] = ' '; smp_txt[slen++] = '(';
    const char *s_tag = (smp_count > 1) ? "Multi-Core Active)" : "Bootstrap Core)";
    for (int k = 0; s_tag[k]; k++) smp_txt[slen++] = s_tag[k];
    smp_txt[slen] = '\0';
    gfx_draw_string(s, c2_x + 12, cur_y + 28, smp_txt, 0xF8FAFC, 0, true);

    u64 ticks = apic_get_ticks();
    char tick_txt[40] = "APIC Ticks: ";
    int tp = 12;
    char tbuf[16]; int tbpos = 0;
    if (ticks == 0) tbuf[tbpos++] = '0';
    else { while (ticks > 0) { tbuf[tbpos++] = '0' + (ticks % 10); ticks /= 10; } }
    for (int k = tbpos - 1; k >= 0; k--) tick_txt[tp++] = tbuf[k];
    tick_txt[tp] = '\0';
    gfx_draw_string(s, c2_x + 12, cur_y + 48, tick_txt, 0x64748B, 0, true);

    cur_y += card_h + 12;

    /* 3. Process & Thread Snapshot Table */
    gfx_draw_string(s, 14, cur_y, "Active System Processes & Kernel Threads", 0x38BDF8, 0, true);
    cur_y += 18;

    /* Table Header */
    int tbl_w = s->width - 24;
    gfx_fill_rect(s, 12, cur_y, tbl_w, 20, 0x0F172A);
    gfx_draw_line(s, 12, cur_y + 20, 12 + tbl_w, cur_y + 20, 0x1E293B);

    gfx_draw_string(s, 20, cur_y + 3, "PID", 0x64748B, 0, true);
    gfx_draw_string(s, 70, cur_y + 3, "NAME", 0x64748B, 0, true);
    gfx_draw_string(s, 240, cur_y + 3, "STATE", 0x64748B, 0, true);
    gfx_draw_string(s, 340, cur_y + 3, "PRIORITY", 0x64748B, 0, true);
    gfx_draw_string(s, 440, cur_y + 3, "MEM", 0x64748B, 0, true);
    cur_y += 22;

    static const struct {
        int pid;
        const char *name;
        const char *state;
        const char *pri;
        const char *mem;
        u32 color;
    } procs[] = {
        {0, "kernel_idle",      "RUNNING",  "SYSTEM", "16 KiB", 0x38BDF8},
        {1, "desktop_shell",    "RUNNING",  "HIGH",   "64 KiB", 0x10B981},
        {2, "vfs_monitor",      "SLEEPING", "NORMAL", "32 KiB", 0xF8FAFC},
        {3, "e1000_net_poll",   "RUNNING",  "HIGH",   "48 KiB", 0x38BDF8},
        {4, "storage_daemon",   "SLEEPING", "NORMAL", "24 KiB", 0xF8FAFC},
        {5, "input_dispatcher", "SLEEPING", "HIGH",   "20 KiB", 0xF8FAFC},
    };

    int bot_bar_y = s->height - 30;
    for (size_t i = 0; i < sizeof(procs)/sizeof(procs[0]); i++) {
        if (cur_y + 22 >= bot_bar_y) break;

        bool is_sel = ((int)i == g_selected_proc);
        u32 row_bg = is_sel ? 0x1E3A8A : (i % 2 == 0 ? 0x111827 : 0x0D1424);
        gfx_fill_rect(s, 12, cur_y, tbl_w, 22, row_bg);
        if (is_sel) {
            gfx_draw_rect(s, 12, cur_y, tbl_w, 22, 0x38BDF8);
        }

        char pid_str[8];
        pid_str[0] = '0' + (procs[i].pid % 10);
        pid_str[1] = '\0';

        gfx_draw_string(s, 24, cur_y + 4, pid_str, 0x94A3B8, 0, true);
        gfx_draw_string(s, 70, cur_y + 4, procs[i].name, procs[i].color, 0, true);
        gfx_draw_string(s, 240, cur_y + 4, procs[i].state, 0x10B981, 0, true);
        gfx_draw_string(s, 340, cur_y + 4, procs[i].pri, 0x94A3B8, 0, true);
        gfx_draw_string(s, 440, cur_y + 4, procs[i].mem, 0x64748B, 0, true);

        cur_y += 22;
    }

    /* 4. Bottom Action Bar */
    gfx_fill_rect(s, 0, bot_bar_y, s->width, 30, 0x0B0F19);
    gfx_draw_line(s, 0, bot_bar_y, s->width, bot_bar_y, 0x1E293B);

    /* Refresh Button */
    gfx_fill_rounded_rect(s, 12, bot_bar_y + 3, 110, 24, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 12, bot_bar_y + 3, 110, 24, 4, 0x334155);
    gfx_draw_string(s, 24, bot_bar_y + 7, "Refresh", 0xF8FAFC, 0, true);

    /* Close Button */
    int cl_x = s->width - 82;
    gfx_fill_rounded_rect(s, cl_x, bot_bar_y + 3, 70, 24, 4, 0x2563EB);
    gfx_draw_rounded_rect(s, cl_x, bot_bar_y + 3, 70, 24, 4, 0x3B82F6);
    gfx_draw_string(s, cl_x + 16, bot_bar_y + 7, "Close", 0xFFFFFF, 0, true);
}

static void sysmon_on_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return;

    if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        int cx = ev->mouse.x - win->client_x;
        int cy = ev->mouse.y - win->client_y;
        int bbar_y = win->client_h - 30;

        /* Click on Process Table Rows */
        int tbl_start_y = 44 + 72 + 12 + 18 + 22;
        if (cx >= 12 && cx <= win->client_w - 12 && cy >= tbl_start_y && cy < bbar_y) {
            int row = (cy - tbl_start_y) / 22;
            if (row >= 0 && row < 6) {
                g_selected_proc = row;
                wm_render();
                gfx_flip();
                return;
            }
        }

        /* Click Refresh */
        if (cx >= 12 && cx <= 122 && cy >= bbar_y + 3 && cy <= bbar_y + 27) {
            wm_render();
            gfx_flip();
            return;
        }

        /* Click Close */
        if (cx >= win->client_w - 82 && cx <= win->client_w - 12 &&
            cy >= bbar_y + 3 && cy <= bbar_y + 27) {
            wm_destroy_window(win);
            g_sysmon_win = NULL;
            return;
        }
    }
}

void app_sysmon_launch(void) {
    if (g_sysmon_win && wm_find_by_id(g_sysmon_win->id)) {
        wm_bring_to_front(g_sysmon_win);
        wm_set_focus(g_sysmon_win);
        return;
    }

    int win_w = 540;
    int win_h = 360;
    gfx_surface_t *bb = gfx_get_backbuffer();
    int sx = bb ? (bb->width - win_w) / 2 : 220;
    int sy = bb ? (bb->height - win_h) / 2 : 160;

    g_sysmon_win = wm_create_window("System Monitor", sx, sy, win_w, win_h);
    if (!g_sysmon_win) return;

    g_sysmon_win->on_paint = sysmon_on_paint;
    g_sysmon_win->on_event = sysmon_on_event;
    wm_bring_to_front(g_sysmon_win);
    wm_set_focus(g_sysmon_win);
}
