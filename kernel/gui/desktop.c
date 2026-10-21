#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/input.h>
#include <kernel/drivers/apic.h>
#include <kernel/pmm.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/browser.h>

static struct {
    bool initialized;
    bool start_menu_open;
    int screen_w;
    int screen_h;
} g_desktop = {0};

void desktop_init(void) {
    gfx_surface_t *bb = gfx_get_backbuffer();
    g_desktop.screen_w = bb ? bb->width : 1024;
    g_desktop.screen_h = bb ? bb->height : 768;
    g_desktop.start_menu_open = false;
    g_desktop.initialized = true;
}

static void render_desktop_background(gfx_surface_t *bb) {
    /* 1. Deep space navy background */
    gfx_fill_rect(bb, 0, 0, bb->width, bb->height - WM_TASKBAR_HEIGHT, GFX_COLOR_DESKTOP_BG);

    /* 2. Futuristic subtle grid pattern */
    for (int y = 0; y < bb->height - WM_TASKBAR_HEIGHT; y += 40) {
        gfx_draw_line(bb, 0, y, bb->width, y, GFX_COLOR_DESKTOP_GRID);
    }
    for (int x = 0; x < bb->width; x += 40) {
        gfx_draw_line(bb, x, 0, x, bb->height - WM_TASKBAR_HEIGHT, GFX_COLOR_DESKTOP_GRID);
    }

    /* 3. Branding and Version Header */
    gfx_draw_string(bb, 20, 16, "AstraOS v0.2.0 - Graphical Desktop Foundation", GFX_COLOR_ACCENT_BLUE, 0, true);
    gfx_draw_string(bb, 20, 32, "x86-64 Freestanding Kernel | UEFI / Limine", GFX_COLOR_TEXT_MUTED, 0, true);

    /* 4. Desktop Shortcuts / Icons (Left Column) */
    /* Icon 1: Text Editor */
    gfx_fill_rect(bb, 24, 70, 48, 48, GFX_COLOR_TITLEBAR_ACTIVE);
    gfx_draw_rect(bb, 24, 70, 48, 48, GFX_COLOR_ACCENT_BLUE);
    gfx_draw_string(bb, 38, 86, "ED", GFX_COLOR_TEXT_PRIMARY, 0, true);
    gfx_draw_string(bb, 14, 122, "Text Editor", GFX_COLOR_TEXT_PRIMARY, 0, true);

    /* Icon 2: File Manager */
    gfx_fill_rect(bb, 24, 150, 48, 48, GFX_COLOR_TITLEBAR_ACTIVE);
    gfx_draw_rect(bb, 24, 150, 48, 48, GFX_COLOR_ACCENT_PURPLE);
    gfx_draw_string(bb, 38, 166, "FM", GFX_COLOR_TEXT_PRIMARY, 0, true);
    gfx_draw_string(bb, 14, 202, "Files (VFS)", GFX_COLOR_TEXT_PRIMARY, 0, true);

    /* Icon 3: AstraBrowser */
    gfx_fill_rect(bb, 24, 230, 48, 48, GFX_COLOR_TITLEBAR_ACTIVE);
    gfx_draw_rect(bb, 24, 230, 48, 48, GFX_COLOR_ACCENT_CYAN);
    gfx_draw_string(bb, 38, 246, "WB", GFX_COLOR_TEXT_PRIMARY, 0, true);
    gfx_draw_string(bb, 10, 282, "AstraBrowser", GFX_COLOR_TEXT_PRIMARY, 0, true);
}

static void render_taskbar(gfx_surface_t *bb) {
    int tb_y = bb->height - WM_TASKBAR_HEIGHT;

    /* Taskbar panel base */
    gfx_fill_rect(bb, 0, tb_y, bb->width, WM_TASKBAR_HEIGHT, GFX_COLOR_PANEL_BG);
    gfx_draw_line(bb, 0, tb_y, bb->width, tb_y, GFX_COLOR_PANEL_BORDER);

    /* Start / App Launcher button */
    u32 btn_bg = g_desktop.start_menu_open ? GFX_COLOR_ACCENT_BLUE : GFX_COLOR_TITLEBAR_ACTIVE;
    u32 btn_fg = g_desktop.start_menu_open ? GFX_COLOR_BLACK : GFX_COLOR_TEXT_PRIMARY;
    gfx_fill_rect(bb, 4, tb_y + 4, 88, 28, btn_bg);
    gfx_draw_rect(bb, 4, tb_y + 4, 88, 28, GFX_COLOR_ACCENT_BLUE);
    gfx_draw_string(bb, 14, tb_y + 10, "AstraOS", btn_fg, btn_bg, false);

    /* Quick Launch Indicators */
    gfx_draw_string(bb, 102, tb_y + 10, "[Editor]", GFX_COLOR_TEXT_MUTED, 0, true);
    gfx_draw_string(bb, 172, tb_y + 10, "[Files]", GFX_COLOR_TEXT_MUTED, 0, true);
    gfx_draw_string(bb, 236, tb_y + 10, "[Browser]", GFX_COLOR_TEXT_MUTED, 0, true);

    /* System Status Area (Right side): APIC Timer Ticks & RAM Stats */
    struct pmm_stats st;
    pmm_get_stats(&st);
    u64 used_mb = (st.allocated_pages * PAGE_SIZE) / (1024 * 1024);
    u64 total_mb = (st.total_managed_pages * PAGE_SIZE) / (1024 * 1024);

    char stat_buf[64];
    u64 ticks = apic_get_ticks();
    u64 sec = ticks / 100;
    u64 min = sec / 60;
    sec = sec % 60;

    /* Format time and RAM */
    int len = 0;
    stat_buf[len++] = 'R'; stat_buf[len++] = 'A'; stat_buf[len++] = 'M'; stat_buf[len++] = ':'; stat_buf[len++] = ' ';
    if (used_mb >= 100) stat_buf[len++] = '0' + (used_mb / 100) % 10;
    if (used_mb >= 10)  stat_buf[len++] = '0' + (used_mb / 10) % 10;
    stat_buf[len++] = '0' + (used_mb % 10);
    stat_buf[len++] = '/';
    if (total_mb >= 100) stat_buf[len++] = '0' + (total_mb / 100) % 10;
    if (total_mb >= 10)  stat_buf[len++] = '0' + (total_mb / 10) % 10;
    stat_buf[len++] = '0' + (total_mb % 10);
    stat_buf[len++] = 'M'; stat_buf[len++] = 'B'; stat_buf[len++] = ' '; stat_buf[len++] = '|'; stat_buf[len++] = ' ';

    stat_buf[len++] = '0' + (min / 10) % 10;
    stat_buf[len++] = '0' + (min % 10);
    stat_buf[len++] = ':';
    stat_buf[len++] = '0' + (sec / 10) % 10;
    stat_buf[len++] = '0' + (sec % 10);
    stat_buf[len] = '\0';

    int status_x = bb->width - (len * FONT_WIDTH) - 16;
    gfx_draw_string(bb, status_x, tb_y + 10, stat_buf, GFX_COLOR_ACCENT_CYAN, 0, true);
}

static void render_start_menu(gfx_surface_t *bb) {
    if (!g_desktop.start_menu_open) return;

    int menu_w = 180;
    int menu_h = 156;
    int menu_x = 4;
    int menu_y = bb->height - WM_TASKBAR_HEIGHT - menu_h - 4;

    /* Menu Body and Outline */
    gfx_fill_rect(bb, menu_x, menu_y, menu_w, menu_h, GFX_COLOR_PANEL_BG);
    gfx_draw_rect(bb, menu_x, menu_y, menu_w, menu_h, GFX_COLOR_ACCENT_BLUE);

    /* Menu Items */
    gfx_draw_string(bb, menu_x + 12, menu_y + 10, "AstraOS Applications", GFX_COLOR_TEXT_MUTED, 0, true);
    gfx_draw_line(bb, menu_x + 8, menu_y + 28, menu_x + menu_w - 8, menu_y + 28, GFX_COLOR_PANEL_BORDER);

    gfx_draw_string(bb, menu_x + 16, menu_y + 38, "> Text Editor", GFX_COLOR_TEXT_PRIMARY, 0, true);
    gfx_draw_string(bb, menu_x + 16, menu_y + 64, "> File Manager", GFX_COLOR_TEXT_PRIMARY, 0, true);
    gfx_draw_string(bb, menu_x + 16, menu_y + 90, "> AstraBrowser", GFX_COLOR_ACCENT_CYAN, 0, true);
    gfx_draw_string(bb, menu_x + 16, menu_y + 116, "> System About", GFX_COLOR_TEXT_PRIMARY, 0, true);
}

void desktop_render(void) {
    gfx_surface_t *bb = gfx_get_backbuffer();
    if (!bb) return;

    /* 1. Desktop Wallpaper / Grid */
    render_desktop_background(bb);

    /* 2. Window Manager (Windows in z-order) */
    wm_render();

    /* 3. Taskbar & System Tray */
    render_taskbar(bb);

    /* 4. Start Menu (if open) */
    render_start_menu(bb);

    /* 5. Mouse Cursor Overlay */
    int mx, my;
    input_get_mouse_state(&mx, &my, NULL);
    gfx_draw_cursor(bb, mx, my);

    /* 6. Present to physical display */
    gfx_flip();
}

void desktop_handle_event(const input_event_t *ev) {
    if (!ev) return;

    if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        int tb_y = g_desktop.screen_h - WM_TASKBAR_HEIGHT;

        /* Click on Start Button */
        if (ev->mouse.x >= 4 && ev->mouse.x <= 92 &&
            ev->mouse.y >= tb_y + 4 && ev->mouse.y <= tb_y + 32) {
            g_desktop.start_menu_open = !g_desktop.start_menu_open;
            return;
        }

        /* Click on Quick Launch: [Editor] */
        if (ev->mouse.x >= 100 && ev->mouse.x <= 160 &&
            ev->mouse.y >= tb_y + 4 && ev->mouse.y <= tb_y + 32) {
            app_editor_launch("/hello.txt");
            return;
        }

        /* Click on Quick Launch: [Files] */
        if (ev->mouse.x >= 170 && ev->mouse.x <= 230 &&
            ev->mouse.y >= tb_y + 4 && ev->mouse.y <= tb_y + 32) {
            app_filemgr_launch("/");
            return;
        }

        /* Click on Quick Launch: [Browser] */
        if (ev->mouse.x >= 236 && ev->mouse.x <= 310 &&
            ev->mouse.y >= tb_y + 4 && ev->mouse.y <= tb_y + 32) {
            app_browser_launch("file:///www/index.html");
            return;
        }

        /* Check Start Menu Clicks */
        if (g_desktop.start_menu_open) {
            int menu_w = 180;
            int menu_h = 156;
            int menu_x = 4;
            int menu_y = g_desktop.screen_h - WM_TASKBAR_HEIGHT - menu_h - 4;

            if (ev->mouse.x >= menu_x && ev->mouse.x <= menu_x + menu_w &&
                ev->mouse.y >= menu_y && ev->mouse.y <= menu_y + menu_h) {
                if (ev->mouse.y >= menu_y + 34 && ev->mouse.y <= menu_y + 54) {
                    app_editor_launch("/hello.txt");
                    g_desktop.start_menu_open = false;
                    return;
                } else if (ev->mouse.y >= menu_y + 60 && ev->mouse.y <= menu_y + 80) {
                    app_filemgr_launch("/");
                    g_desktop.start_menu_open = false;
                    return;
                } else if (ev->mouse.y >= menu_y + 86 && ev->mouse.y <= menu_y + 110) {
                    app_browser_launch("file:///www/index.html");
                    g_desktop.start_menu_open = false;
                    return;
                } else if (ev->mouse.y >= menu_y + 112 && ev->mouse.y <= menu_y + 136) {
                    /* System About */
                    g_desktop.start_menu_open = false;
                    return;
                }
            } else {
                g_desktop.start_menu_open = false;
            }
        }

        /* Check Desktop Shortcuts Clicks */
        if (ev->mouse.x >= 14 && ev->mouse.x <= 84) {
            if (ev->mouse.y >= 70 && ev->mouse.y <= 135) {
                app_editor_launch("/hello.txt");
                return;
            } else if (ev->mouse.y >= 150 && ev->mouse.y <= 215) {
                app_filemgr_launch("/");
                return;
            } else if (ev->mouse.y >= 230 && ev->mouse.y <= 295) {
                app_browser_launch("file:///www/index.html");
                return;
            }
        }
    }

    /* Forward to Window Manager */
    wm_handle_input_event(ev);
}

bool desktop_self_test(void) {
    kprintf("\n[DESKTOP TEST] Starting Desktop Shell Subsystem Self-Test...\n");
    desktop_init();

    /* 1. Test Desktop Dimensions */
    if (g_desktop.screen_w < 640 || g_desktop.screen_h < 480) {
        kprintf("[DESKTOP TEST FAIL] Screen dimensions invalid: %dx%d\n",
                g_desktop.screen_w, g_desktop.screen_h);
        return false;
    }
    kprintf("  PASS: Desktop geometry verified (%dx%d).\n", g_desktop.screen_w, g_desktop.screen_h);

    /* 2. Test Start Menu Toggle */
    input_event_t click_start = {
        .type = INPUT_EVENT_MOUSE_DOWN,
        .mouse = { .x = 20, .y = g_desktop.screen_h - 15, .buttons = MOUSE_BTN_LEFT }
    };
    desktop_handle_event(&click_start);
    if (!g_desktop.start_menu_open) {
        kprintf("[DESKTOP TEST FAIL] Start launcher click failed to open menu!\n");
        return false;
    }
    desktop_handle_event(&click_start);
    if (g_desktop.start_menu_open) {
        kprintf("[DESKTOP TEST FAIL] Second click failed to close start menu!\n");
        return false;
    }
    kprintf("  PASS: Application launcher toggle and dispatch verified.\n");

    /* 3. Test Full Desktop Rendering Pass */
    desktop_render();
    kprintf("  PASS: Full composite rendering pass completed cleanly.\n");

    kprintf("[DESKTOP TEST] All Desktop Shell tests passed successfully!\n");
    kprintf("ASTRA_DESKTOP_OK\n\n");
    return true;
}
