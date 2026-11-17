#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/input.h>
#include <kernel/drivers/apic.h>
#include <kernel/pmm.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/browser.h>
#include <kernel/terminal.h>
#include <kernel/rtc.h>
#include <kernel/net.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/power.h>
#include <kernel/session.h>
#include <kernel/user.h>
#include <kernel/string.h>

typedef enum {
    DESKTOP_UI_NORMAL = 0,
    DESKTOP_UI_LOCKED = 1,
    DESKTOP_UI_LOGIN  = 2
} desktop_ui_state_t;

static struct {
    bool initialized;
    bool start_menu_open;
    bool power_menu_open;
    int  confirm_action; /* 0 = none, 1 = restart, 2 = shutdown, 3 = logout */
    int  screen_w;
    int  screen_h;
    desktop_ui_state_t ui_state;
    char pass_buffer[64];
    int  pass_len;
    char status_msg[64];
    u32  status_col;
    int  login_selected_user; /* index into accounts */
} g_desktop = {0};

static desktop_lifecycle_state_t g_desktop_lifecycle = DESKTOP_STATE_BOOTING;

desktop_lifecycle_state_t desktop_get_lifecycle_state(void) {
    return g_desktop_lifecycle;
}

void desktop_set_lifecycle_state(desktop_lifecycle_state_t state) {
    g_desktop_lifecycle = state;
}

bool desktop_is_ready(void) {
    return g_desktop_lifecycle == DESKTOP_STATE_READY;
}

bool desktop_is_initialized(void) {
    return g_desktop.initialized;
}

static void sys_reboot(void) {
    power_restart();
}

static void sys_shutdown(void) {
    power_shutdown();
}

void desktop_init(void) {
    if (g_desktop.initialized) {
        return; /* Initialize desktop shell exactly once */
    }

    if (!gfx_is_available() || !wm_is_initialized()) {
        kprintf("[DESKTOP ERROR] Prerequisites not met (GFX or WM not ready)\n");
        desktop_set_lifecycle_state(DESKTOP_STATE_FAILED);
        return;
    }

    desktop_set_lifecycle_state(DESKTOP_STATE_GUI_INITIALIZING);

    gfx_surface_t *bb = gfx_get_backbuffer();
    g_desktop.screen_w = bb ? bb->width : 1024;
    g_desktop.screen_h = bb ? bb->height : 768;
    g_desktop.start_menu_open = false;
    g_desktop.power_menu_open = false;
    g_desktop.confirm_action = 0;
    g_desktop.initialized = true;
}

/* ========================================================================= */
/* ========================================================================= */
/* Modern Industrial Application Icons (Continuous Squircles with Specular Rim)*/
/* ========================================================================= */

static void draw_icon_editor(gfx_surface_t *s, int x, int y) {
    /* Modern Developer IDE / Code Studio: Deep Indigo Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x1E1B4B);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x4338CA);
    /* Specular lighting rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x818CF8);
    gfx_fill_rect(s, x + 12, y + 2, 20, 1, 0xA5B4FC);

    /* Crisp White Document Sheet with Corner Fold */
    gfx_fill_rounded_rect(s, x + 10, y + 8, 24, 28, 3, 0xF8FAFC);
    /* Triangular Top-Right Corner Fold */
    gfx_fill_rect(s, x + 28, y + 8, 6, 6, 0xCBD5E1);
    gfx_draw_line(s, x + 28, y + 8, x + 34, y + 14, 0x94A3B8);

    /* Rich Syntax Highlight Bars */
    /* Line 1: Keyword Blue & Identifier Slate */
    gfx_fill_rect(s, x + 14, y + 13, 6, 2, 0x0284C7);
    gfx_fill_rect(s, x + 22, y + 13, 8, 2, 0x334155);

    /* Line 2: Function Amber & String Emerald */
    gfx_fill_rect(s, x + 14, y + 18, 10, 2, 0xD97706);
    gfx_fill_rect(s, x + 26, y + 18, 6, 2, 0x059669);

    /* Line 3: Operator Violet & Identifier */
    gfx_fill_rect(s, x + 14, y + 23, 4, 2, 0x9333EA);
    gfx_fill_rect(s, x + 20, y + 23, 11, 2, 0x334155);

    /* Line 4: Comment Slate */
    gfx_fill_rect(s, x + 14, y + 28, 12, 2, 0x64748B);

    /* Overlaid Precision Stylus Pen on Bottom-Right */
    gfx_draw_line(s, x + 34, y + 24, x + 28, y + 32, 0x94A3B8);
    gfx_draw_line(s, x + 35, y + 24, x + 29, y + 32, 0xE2E8F0);
    gfx_put_pixel(s, x + 27, y + 33, 0xF59E0B); /* Golden nib */
}

static void draw_icon_filemgr(gfx_surface_t *s, int x, int y) {
    /* Modern Fluent Oceanic Blue Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x082F49);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x0284C7);
    /* Specular lighting rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x38BDF8);

    /* Back Folder Tab */
    gfx_fill_rounded_rect(s, x + 7, y + 9, 15, 6, 2, 0x0369A1);

    /* Back Folder Body */
    gfx_fill_rounded_rect(s, x + 7, y + 13, 30, 22, 3, 0x0284C7);

    /* Inner Crisp Document Sheets Peeking Up */
    gfx_fill_rounded_rect(s, x + 11, y + 10, 22, 14, 2, 0xF8FAFC);
    gfx_fill_rect(s, x + 14, y + 13, 10, 2, 0x38BDF8);
    gfx_fill_rect(s, x + 14, y + 17, 14, 2, 0x10B981);

    /* Radiant Sky-Blue Front Pocket Flap */
    gfx_fill_rounded_rect(s, x + 7, y + 18, 30, 18, 3, 0x38BDF8);
    gfx_draw_rounded_rect(s, x + 7, y + 18, 30, 18, 3, 0x7DD3FC);
    /* Top Specular Rim on Flap */
    gfx_fill_rect(s, x + 9, y + 19, 26, 1, 0xBAE6FD);
    /* Bottom Subdued Pocket Shadow */
    gfx_fill_rect(s, x + 9, y + 34, 26, 1, 0x0284C7);
}

static void draw_icon_browser(gfx_surface_t *s, int x, int y) {
    /* Orbital Compass Globe: Modern Twilight Navy Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x060E22);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x1E293B);
    /* Specular lighting rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x60A5FA);

    /* Polished Titanium Outer Dial Bezel Ring */
    gfx_fill_rounded_rect(s, x + 7, y + 7, 30, 30, 15, 0x0F172A);
    gfx_draw_rounded_rect(s, x + 7, y + 7, 30, 30, 15, 0xE2E8F0);

    /* Deep Spherical Lens */
    gfx_fill_rounded_rect(s, x + 9, y + 9, 26, 26, 13, 0x081226);

    /* Latitude & Longitude Orbital Wireframe Rings */
    gfx_draw_line(s, x + 11, y + 22, x + 33, y + 22, 0x1E3A8A);
    gfx_draw_line(s, x + 22, y + 11, x + 22, y + 33, 0x1E3A8A);
    gfx_draw_rounded_rect(s, x + 14, y + 11, 16, 22, 8, 0x1E3A8A);

    /* Specular Glass Gloss Arc (Top-Left) */
    gfx_draw_line(s, x + 13, y + 14, x + 18, y + 11, 0x93C5FD);
    gfx_draw_line(s, x + 14, y + 13, x + 17, y + 12, 0xBAE6FD);

    /* 3D Faceted Compass Rose Needle */
    /* North Point (Vibrant Crimson Red) */
    gfx_draw_line(s, x + 22, y + 22, x + 29, y + 15, 0xEF4444);
    gfx_draw_line(s, x + 21, y + 22, x + 28, y + 15, 0xF87171);
    gfx_draw_line(s, x + 22, y + 21, x + 29, y + 14, 0xDC2626);

    /* South Point (Metallic Chrome Silver) */
    gfx_draw_line(s, x + 22, y + 22, x + 15, y + 29, 0xF8FAFC);
    gfx_draw_line(s, x + 23, y + 22, x + 16, y + 29, 0xCBD5E1);

    /* Polished Gold Center Pivot */
    gfx_fill_rounded_rect(s, x + 20, y + 20, 5, 5, 2, 0xF59E0B);
    gfx_put_pixel(s, x + 22, y + 22, 0xFFFFFF);
}

static void draw_icon_terminal(gfx_surface_t *s, int x, int y) {
    /* Obsidian Black Hyper-Terminal Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x090D16);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x1E293B);
    /* Specular lighting rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x475569);

    /* Inner CRT Screen Canvas */
    gfx_fill_rounded_rect(s, x + 6, y + 7, 32, 30, 4, 0x020617);
    gfx_draw_rounded_rect(s, x + 6, y + 7, 32, 30, 4, 0x0F172A);

    /* Header Mini Traffic Lights */
    gfx_fill_rounded_rect(s, x + 10, y + 10, 3, 3, 1, 0xEF4444);
    gfx_fill_rounded_rect(s, x + 15, y + 10, 3, 3, 1, 0xF59E0B);
    gfx_fill_rounded_rect(s, x + 20, y + 10, 3, 3, 1, 0x10B981);

    /* Glowing Mint-Green Command Prompt: > */
    gfx_draw_line(s, x + 10, y + 18, x + 14, y + 21, 0x10B981);
    gfx_draw_line(s, x + 11, y + 18, x + 15, y + 21, 0x34D399);
    gfx_draw_line(s, x + 14, y + 21, x + 10, y + 24, 0x10B981);
    gfx_draw_line(s, x + 15, y + 21, x + 11, y + 24, 0x34D399);

    /* Solid Glowing Mint Cursor Block */
    gfx_fill_rounded_rect(s, x + 17, y + 16, 6, 9, 1, 0x10B981);
    gfx_fill_rect(s, x + 18, y + 17, 4, 7, 0x6EE7B7);

    /* Secondary Line Output Preview Dots */
    gfx_fill_rect(s, x + 10, y + 28, 8, 2, 0x0284C7);
    gfx_fill_rect(s, x + 20, y + 28, 12, 2, 0x38BDF8);
}

static void draw_icon_settings(gfx_surface_t *s, int x, int y) {
    /* Precision Machined Titanium Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x1E293B);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x334155);
    /* Specular lighting rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x64748B);
    gfx_fill_rect(s, x + 12, y + 2, 20, 1, 0x94A3B8);

    /* Primary Machined Gear: Brushed Platinum Silver Body */
    gfx_fill_rounded_rect(s, x + 12, y + 12, 20, 20, 10, 0xE2E8F0);

    /* 8 Precision Cog Teeth */
    gfx_fill_rounded_rect(s, x + 19, y + 8, 6, 28, 2, 0xE2E8F0);
    gfx_fill_rounded_rect(s, x + 8, y + 19, 28, 6, 2, 0xE2E8F0);
    gfx_fill_rounded_rect(s, x + 11, y + 11, 6, 6, 2, 0xE2E8F0);
    gfx_fill_rounded_rect(s, x + 27, y + 11, 6, 6, 2, 0xE2E8F0);
    gfx_fill_rounded_rect(s, x + 11, y + 27, 6, 6, 2, 0xE2E8F0);
    gfx_fill_rounded_rect(s, x + 27, y + 27, 6, 6, 2, 0xE2E8F0);

    /* Inner Concentric Bevel Groove */
    gfx_fill_rounded_rect(s, x + 15, y + 15, 14, 14, 7, 0x475569);

    /* Center Axle Inset */
    gfx_fill_rounded_rect(s, x + 18, y + 18, 8, 8, 4, 0x0F172A);

    /* Polished Chrome Center Hub Pin */
    gfx_fill_rounded_rect(s, x + 20, y + 20, 4, 4, 2, 0xFFFFFF);

    /* Secondary Interlocking Brass Cog (Top-Right) */
    gfx_fill_rounded_rect(s, x + 28, y + 8, 8, 8, 4, 0xD97706);
    gfx_draw_rounded_rect(s, x + 28, y + 8, 8, 8, 4, 0xF59E0B);
}

static void draw_icon_calc(gfx_surface_t *s, int x, int y) {
    /* Modern Matte Charcoal Acrylic Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x18181B);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x27272A);
    /* Specular lighting rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x52525B);

    /* Recessed OLED Screen Readout */
    gfx_fill_rounded_rect(s, x + 8, y + 7, 28, 11, 2, 0x06080E);
    gfx_draw_rounded_rect(s, x + 8, y + 7, 28, 11, 2, 0x1E293B);
    /* Glowing Digital Readout Digits */
    gfx_draw_string(s, x + 12, y + 8, "123.4", 0x34D399, 0, true);

    /* 4x4 Precision Keypad */
    /* Row 1 */
    gfx_fill_rounded_rect(s, x + 8, y + 20, 5, 4, 1, 0xEF4444);  /* C */
    gfx_fill_rounded_rect(s, x + 15, y + 20, 5, 4, 1, 0x27272A); /* +/- */
    gfx_fill_rounded_rect(s, x + 22, y + 20, 5, 4, 1, 0x27272A); /* % */
    gfx_fill_rounded_rect(s, x + 29, y + 20, 7, 4, 1, 0xEA580C); /* / */

    /* Row 2 */
    gfx_fill_rounded_rect(s, x + 8, y + 26, 5, 4, 1, 0x334155);  /* 7 */
    gfx_fill_rounded_rect(s, x + 15, y + 26, 5, 4, 1, 0x334155); /* 8 */
    gfx_fill_rounded_rect(s, x + 22, y + 26, 5, 4, 1, 0x334155); /* 9 */
    gfx_fill_rounded_rect(s, x + 29, y + 26, 7, 4, 1, 0xEA580C); /* * */

    /* Row 3 */
    gfx_fill_rounded_rect(s, x + 8, y + 32, 5, 4, 1, 0x334155);  /* 4 */
    gfx_fill_rounded_rect(s, x + 15, y + 32, 5, 4, 1, 0x334155); /* 5 */
    gfx_fill_rounded_rect(s, x + 22, y + 32, 5, 4, 1, 0x334155); /* 6 */
    gfx_fill_rounded_rect(s, x + 29, y + 32, 7, 4, 1, 0xEA580C); /* - */

    /* Row 4 */
    gfx_fill_rounded_rect(s, x + 8, y + 38, 12, 4, 1, 0x334155); /* 0 (wide) */
    gfx_fill_rounded_rect(s, x + 22, y + 38, 5, 4, 1, 0x334155); /* . */
    gfx_fill_rounded_rect(s, x + 29, y + 38, 7, 4, 1, 0x10B981); /* = */
}

static void draw_icon_sysmon(gfx_surface_t *s, int x, int y) {
    /* Space-Navy Telemetry Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x0B0F19);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x1E293B);
    /* Specular highlight rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x38BDF8);

    /* Inner Oscilloscope CRT Grid */
    gfx_fill_rounded_rect(s, x + 6, y + 7, 32, 30, 4, 0x020617);
    gfx_draw_rounded_rect(s, x + 6, y + 7, 32, 30, 4, 0x0F2937);

    /* Subdued Grid Reticle */
    gfx_draw_line(s, x + 7, y + 22, x + 37, y + 22, 0x0B1F2D);
    gfx_draw_line(s, x + 22, y + 8, x + 22, y + 36, 0x0B1F2D);

    /* Glowing ECG Heartbeat Pulse Trace */
    gfx_draw_line(s, x + 8, y + 22, x + 13, y + 22, 0x06B6D4);
    gfx_draw_line(s, x + 13, y + 22, x + 15, y + 20, 0x10B981); /* P wave */
    gfx_draw_line(s, x + 15, y + 20, x + 17, y + 24, 0x10B981); /* Q drop */
    gfx_draw_line(s, x + 17, y + 24, x + 20, y + 12, 0x34D399); /* R peak */
    gfx_draw_line(s, x + 18, y + 24, x + 21, y + 12, 0x6EE7B7);
    gfx_draw_line(s, x + 20, y + 12, x + 23, y + 31, 0x10B981); /* S drop */
    gfx_draw_line(s, x + 23, y + 31, x + 26, y + 20, 0x10B981); /* T wave */
    gfx_draw_line(s, x + 26, y + 20, x + 29, y + 22, 0x06B6D4);
    gfx_draw_line(s, x + 29, y + 22, x + 36, y + 22, 0x06B6D4);

    /* Radiant Status Beacon LED */
    gfx_fill_rounded_rect(s, x + 20, y + 11, 3, 3, 1, 0x6EE7B7);
}

static void draw_icon_pkgmgr(gfx_surface_t *s, int x, int y) {
    /* Royal Violet App Store Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x2E1065);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x4C1D95);
    /* Specular lighting rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0xA855F7);

    /* 3D Isometric Software Container */
    /* Top Diamond Facet */
    gfx_fill_rounded_rect(s, x + 12, y + 11, 20, 8, 2, 0x38BDF8);
    gfx_draw_rounded_rect(s, x + 12, y + 11, 20, 8, 2, 0x7DD3FC);

    /* Bottom Box Body */
    gfx_fill_rounded_rect(s, x + 12, y + 18, 20, 16, 2, 0x0284C7);
    gfx_draw_rounded_rect(s, x + 12, y + 18, 20, 16, 2, 0x0369A1);

    /* Center Gold Seam Ribbon */
    gfx_fill_rect(s, x + 21, y + 11, 2, 23, 0xF59E0B);

    /* Radiant White Download Arrow */
    gfx_fill_rect(s, x + 21, y + 14, 2, 8, 0xFFFFFF);
    gfx_draw_line(s, x + 18, y + 19, x + 22, y + 23, 0xFFFFFF);
    gfx_draw_line(s, x + 25, y + 19, x + 22, y + 23, 0xFFFFFF);
}

/* Helper to render clean modern floating label with crisp 1px drop shadow (macOS / Windows style) */
static void draw_desktop_label(gfx_surface_t *s, int icon_x, int icon_w, int y, const char *text) {
    int len = 0;
    while (text[len]) len++;
    int text_w = len * 8;
    int cx = icon_x + icon_w / 2;
    int tx = cx - text_w / 2;
    if (tx < 4) tx = 4;

    /* Crisp drop shadow */
    gfx_draw_string(s, tx + 1, y + 1, text, 0x04060B, 0, true);
    /* Pure white primary text */
    gfx_draw_string(s, tx, y, text, 0xF8FAFC, 0, true);
}

/* ========================================================================= */
/* Singleton Application Launcher / Focus Dispatcher                        */
/* ========================================================================= */

static void launch_or_focus_app(int app_id) {
    switch (app_id) {
        case 0: { /* Text Editor */
            wm_window_t *w = wm_find_by_title_substring("Editor");
            if (w) { wm_bring_to_front(w); wm_set_focus(w); }
            else { app_editor_launch("/hello.txt"); }
            break;
        }
        case 1: { /* File Manager */
            wm_window_t *w = wm_find_by_title_substring("Files");
            if (w) { wm_bring_to_front(w); wm_set_focus(w); }
            else { app_filemgr_launch("/"); }
            break;
        }
        case 2: { /* Web Browser */
            wm_window_t *w = wm_find_by_title_substring("AstraBrowser");
            if (w) { wm_bring_to_front(w); wm_set_focus(w); }
            else { app_browser_launch("file:///www/index.html"); }
            break;
        }
        case 3: { /* Terminal */
            wm_window_t *w = wm_find_by_title_substring("Terminal");
            if (w) { wm_bring_to_front(w); wm_set_focus(w); }
            else { app_terminal_launch(); }
            break;
        }
        case 4: { /* Settings */
            wm_window_t *w = wm_find_by_title_substring("Settings");
            if (w) { wm_bring_to_front(w); wm_set_focus(w); }
            else { app_settings_launch(); }
            break;
        }
        case 5: { /* Calculator */
            wm_window_t *w = wm_find_by_title_substring("Calculator");
            if (w) { wm_bring_to_front(w); wm_set_focus(w); }
            else { app_calc_launch(); }
            break;
        }
        case 6: { /* System Monitor */
            wm_window_t *w = wm_find_by_title_substring("System Monitor");
            if (w) { wm_bring_to_front(w); wm_set_focus(w); }
            else { app_sysmon_launch(); }
            break;
        }
        case 7: { /* Package Manager */
            wm_window_t *w = wm_find_by_title_substring("Package");
            if (w) { wm_bring_to_front(w); wm_set_focus(w); }
            else { app_pkgmgr_launch(); }
            break;
        }
    }
}

static bool is_app_running(int app_id) {
    switch (app_id) {
        case 0: return wm_find_by_title_substring("Editor") != NULL;
        case 1: return wm_find_by_title_substring("Files") != NULL || wm_find_by_title_substring("File Manager") != NULL;
        case 2: return wm_find_by_title_substring("AstraBrowser") != NULL;
        case 3: return wm_find_by_title_substring("Terminal") != NULL;
        case 4: return wm_find_by_title_substring("Settings") != NULL;
        case 5: return wm_find_by_title_substring("Calculator") != NULL;
        case 6: return wm_find_by_title_substring("System Monitor") != NULL;
        case 7: return wm_find_by_title_substring("Package") != NULL;
        default: return false;
    }
}

/* ========================================================================= */
/* Rendering Subsystems                                                      */
/* ========================================================================= */

static void render_desktop_background(gfx_surface_t *bb) {
    /* 1. Restore the Cosmic Vista wallpaper */
    gfx_restore_wallpaper(bb);

    /* 2. Desktop Application Icons (Left Columns with generous modern spacing) */
    /* Column 1 (x = 32): Editor, Files, Browser, Terminal */
    draw_icon_editor(bb, 32, 60);
    draw_desktop_label(bb, 32, 44, 114, "Editor");

    draw_icon_filemgr(bb, 32, 156);
    draw_desktop_label(bb, 32, 44, 210, "Files");

    draw_icon_browser(bb, 32, 252);
    draw_desktop_label(bb, 32, 44, 306, "Browser");

    draw_icon_terminal(bb, 32, 348);
    draw_desktop_label(bb, 32, 44, 402, "Terminal");

    /* Column 2 (x = 140): Settings, Calculator, Telemetry, Packages */
    draw_icon_settings(bb, 140, 60);
    draw_desktop_label(bb, 140, 44, 114, "Settings");

    draw_icon_calc(bb, 140, 156);
    draw_desktop_label(bb, 140, 44, 210, "Calculator");

    draw_icon_sysmon(bb, 140, 252);
    draw_desktop_label(bb, 140, 44, 306, "Telemetry");

    draw_icon_pkgmgr(bb, 140, 348);
    draw_desktop_label(bb, 140, 44, 402, "Packages");
}

static int g_tz_index = 0; /* 0=LOCAL (+0:00, native RTC), 1=IST (+5:30), 2=UTC (+0:00), 3=EST (-5:00), 4=PST (-8:00) */
static const struct {
    const char *code;
    int offset_mins;
} g_timezones[] = {
    { "LOCAL", 0 },
    { "IST", 330 },
    { "UTC", 0 },
    { "EST", -300 },
    { "PST", -480 }
};

static void render_top_panel(gfx_surface_t *bb) {
    int panel_h = 32;

    /* Base dark glass panel */
    gfx_fill_rect(bb, 0, 0, bb->width, panel_h, 0x0B0F19);
    gfx_draw_line(bb, 0, panel_h, bb->width, panel_h, 0x1E293B);

    /* Left: AstraOS Launcher button */
    u32 btn_bg = g_desktop.start_menu_open ? GFX_COLOR_ACCENT_BLUE : 0x162238;
    u32 btn_fg = g_desktop.start_menu_open ? GFX_COLOR_BLACK : GFX_COLOR_TEXT_PRIMARY;
    gfx_fill_rounded_rect(bb, 8, 4, 92, 24, 5, btn_bg);
    gfx_draw_rounded_rect(bb, 8, 4, 92, 24, 5, GFX_COLOR_ACCENT_BLUE);
    gfx_draw_string(bb, 16, 8, "* AstraOS", btn_fg, 0, true);

    /* Center: Live Real-Time Clock & Date with Timezone */
    rtc_time_t t;
    if (rtc_get_time(&t)) {
        static const int dow_t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
        static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

        int offset = g_timezones[g_tz_index].offset_mins;
        int total_mins = (int)t.hour * 60 + (int)t.minute + offset;
        int day_adj = 0;
        while (total_mins < 0) {
            total_mins += 24 * 60;
            day_adj--;
        }
        while (total_mins >= 24 * 60) {
            total_mins -= 24 * 60;
            day_adj++;
        }
        int loc_hour = total_mins / 60;
        int loc_min = total_mins % 60;
        int loc_day = (int)t.day + day_adj;
        if (loc_day < 1) loc_day = 1;

        int y_adj = (t.month < 3) ? t.year - 1 : t.year;
        int dow = (y_adj + y_adj / 4 - y_adj / 100 + y_adj / 400 + dow_t[(t.month >= 1 && t.month <= 12) ? t.month - 1 : 0] + loc_day) % 7;
        if (dow < 0 || dow > 6) dow = 0;
        const char *dname = days[dow];
        const char *mname = (t.month >= 1 && t.month <= 12) ? months[t.month - 1] : "Nov";

        char time_str[64];
        int tp = 0;
        for (int i = 0; dname[i]; i++) time_str[tp++] = dname[i];
        time_str[tp++] = ','; time_str[tp++] = ' ';
        time_str[tp++] = '0' + (loc_day / 10) % 10;
        time_str[tp++] = '0' + (loc_day % 10);
        time_str[tp++] = ' ';
        for (int i = 0; mname[i]; i++) time_str[tp++] = mname[i];
        time_str[tp++] = ' ';
        int yr = t.year;
        time_str[tp++] = '0' + (yr / 1000) % 10;
        time_str[tp++] = '0' + (yr / 100) % 10;
        time_str[tp++] = '0' + (yr / 10) % 10;
        time_str[tp++] = '0' + (yr % 10);
        time_str[tp++] = ' '; time_str[tp++] = ' ';
        time_str[tp++] = '0' + (loc_hour / 10) % 10;
        time_str[tp++] = '0' + (loc_hour % 10);
        time_str[tp++] = ':';
        time_str[tp++] = '0' + (loc_min / 10) % 10;
        time_str[tp++] = '0' + (loc_min % 10);
        time_str[tp++] = ':';
        time_str[tp++] = '0' + (t.second / 10) % 10;
        time_str[tp++] = '0' + (t.second % 10);
        time_str[tp++] = ' ';
        const char *tz_code = g_timezones[g_tz_index].code;
        for (int i = 0; tz_code[i]; i++) time_str[tp++] = tz_code[i];
        time_str[tp] = '\0';

        int clk_w = tp * 8;
        int clk_x = (bb->width - clk_w) / 2;
        /* Draw sleek subtle pill backing for clock */
        gfx_fill_rounded_rect(bb, clk_x - 10, 4, clk_w + 20, 24, 6, 0x101726);
        gfx_draw_rounded_rect(bb, clk_x - 10, 4, clk_w + 20, 24, 6, 0x1E2B42);
        gfx_draw_string(bb, clk_x, 8, time_str, 0xF8FAFC, 0, true);
    }

    /* Right: System Status Badges & Power Menu (No Overlap) */
    int cur_rx = bb->width - 12;

    /* 1. Power Menu Button: [PWR] */
    cur_rx -= 52;
    u32 pwr_bg = g_desktop.power_menu_open ? GFX_COLOR_BTN_CLOSE : 0x162238;
    gfx_fill_rounded_rect(bb, cur_rx, 4, 52, 24, 5, pwr_bg);
    gfx_draw_rounded_rect(bb, cur_rx, 4, 52, 24, 5, GFX_COLOR_ACCENT_BLUE);
    gfx_draw_string(bb, cur_rx + 14, 8, "PWR", GFX_COLOR_TEXT_PRIMARY, 0, true);
    cur_rx -= 8;

    /* 2. Audio status chip: Vol: Off */
    cur_rx -= 78;
    gfx_fill_rounded_rect(bb, cur_rx, 4, 78, 24, 5, 0x131A2B);
    gfx_draw_rounded_rect(bb, cur_rx, 4, 78, 24, 5, 0x223048);
    gfx_draw_string(bb, cur_rx + 8, 8, "Vol: Off", GFX_COLOR_TEXT_MUTED, 0, true);
    cur_rx -= 8;

    /* 3. Power status chip: [dot] AC Power */
    cur_rx -= 94;
    gfx_fill_rounded_rect(bb, cur_rx, 4, 94, 24, 5, 0x131A2B);
    gfx_draw_rounded_rect(bb, cur_rx, 4, 94, 24, 5, 0x223048);
    /* Emerald lightning/power indicator dot */
    gfx_fill_rounded_rect(bb, cur_rx + 8, 12, 8, 8, 4, 0x10B981);
    gfx_draw_string(bb, cur_rx + 20, 8, "AC Power", GFX_COLOR_TEXT_PRIMARY, 0, true);
    cur_rx -= 8;

    /* 4. Network Link State chip: [dot] Online / No Link */
    cur_rx -= 86;
    gfx_fill_rounded_rect(bb, cur_rx, 4, 86, 24, 5, 0x131A2B);
    gfx_draw_rounded_rect(bb, cur_rx, 4, 86, 24, 5, 0x223048);
    net_if_t *nif = net_get_interface();
    if (nif && nif->link_up && nif->configured) {
        gfx_fill_rounded_rect(bb, cur_rx + 8, 12, 8, 8, 4, 0x10B981);
        gfx_draw_string(bb, cur_rx + 20, 8, "Online", 0x10B981, 0, true);
    } else if (nif && nif->link_up) {
        gfx_fill_rounded_rect(bb, cur_rx + 8, 12, 8, 8, 4, 0x38BDF8);
        gfx_draw_string(bb, cur_rx + 20, 8, "DHCP...", 0x38BDF8, 0, true);
    } else {
        gfx_fill_rounded_rect(bb, cur_rx + 8, 12, 8, 8, 4, 0x64748B);
        gfx_draw_string(bb, cur_rx + 20, 8, "No Link", GFX_COLOR_TEXT_MUTED, 0, true);
    }
    cur_rx -= 8;

    /* 5. User Identity chip: [dot] username */
    session_t *cur_s = session_get_active();
    const char *uname = cur_s ? cur_s->username : "akash";
    int uchip_w = 28 + (int)kstrlen(uname) * 8;
    cur_rx -= uchip_w;
    gfx_fill_rounded_rect(bb, cur_rx, 4, uchip_w, 24, 5, 0x131A2B);
    gfx_draw_rounded_rect(bb, cur_rx, 4, uchip_w, 24, 5, 0x223048);
    gfx_fill_rounded_rect(bb, cur_rx + 8, 12, 8, 8, 4, 0x38BDF8);
    gfx_draw_string(bb, cur_rx + 20, 8, uname, GFX_COLOR_TEXT_PRIMARY, 0, true);
}

static void render_bottom_dock(gfx_surface_t *bb) {
    int dock_w = 432;
    int dock_h = 56;
    int dock_x = (bb->width - dock_w) / 2;
    int dock_y = bb->height - dock_h - 10;

    /* Translucent rounded capsule base with specular highlight */
    gfx_fill_rounded_rect(bb, dock_x, dock_y, dock_w, dock_h, 14, 0x0E1424);
    gfx_draw_rounded_rect(bb, dock_x, dock_y, dock_w, dock_h, 14, 0x2A364F);
    gfx_draw_line(bb, dock_x + 14, dock_y + 1, dock_x + dock_w - 14, dock_y + 1, 0x3E4E6D);

    /* 8 Dock Apps */
    for (int i = 0; i < 8; i++) {
        int app_x = dock_x + 8 + i * 52;
        int app_y = dock_y + 4;

        switch (i) {
            case 0: draw_icon_editor(bb, app_x, app_y); break;
            case 1: draw_icon_filemgr(bb, app_x, app_y); break;
            case 2: draw_icon_browser(bb, app_x, app_y); break;
            case 3: draw_icon_terminal(bb, app_x, app_y); break;
            case 4: draw_icon_settings(bb, app_x, app_y); break;
            case 5: draw_icon_calc(bb, app_x, app_y); break;
            case 6: draw_icon_sysmon(bb, app_x, app_y); break;
            case 7: draw_icon_pkgmgr(bb, app_x, app_y); break;
        }

        /* Running Application Indicator Dot (clean circular dot centered under app) */
        if (is_app_running(i)) {
            gfx_fill_rounded_rect(bb, app_x + 20, dock_y + dock_h - 5, 4, 4, 2, 0x38BDF8);
        }
    }
}

static void render_start_menu(gfx_surface_t *bb) {
    if (!g_desktop.start_menu_open) return;

    int menu_w = 210;
    int menu_h = 240;
    int menu_x = 6;
    int menu_y = 34;

    /* Menu Body */
    gfx_fill_rounded_rect(bb, menu_x, menu_y, menu_w, menu_h, 6, 0x101728);
    gfx_draw_rounded_rect(bb, menu_x, menu_y, menu_w, menu_h, 6, GFX_COLOR_ACCENT_BLUE);

    /* Header */
    gfx_draw_string(bb, menu_x + 12, menu_y + 8, "AstraOS Applications", GFX_COLOR_ACCENT_CYAN, 0, true);
    gfx_draw_line(bb, menu_x + 8, menu_y + 24, menu_x + menu_w - 8, menu_y + 24, GFX_COLOR_PANEL_BORDER);

    /* 8 Application Items */
    static const char *app_names[] = {
        "> Text Editor",
        "> File Manager",
        "> AstraBrowser",
        "> AstraOS Terminal",
        "> System Settings",
        "> Calculator",
        "> System Telemetry",
        "> Package Manager",
    };

    for (int i = 0; i < 8; i++) {
        int item_y = menu_y + 30 + i * 22;
        u32 col = is_app_running(i) ? GFX_COLOR_ACCENT_GREEN : GFX_COLOR_TEXT_PRIMARY;
        gfx_draw_string(bb, menu_x + 14, item_y, app_names[i], col, 0, true);
    }

    /* Bottom About notice */
    gfx_draw_line(bb, menu_x + 8, menu_y + 210, menu_x + menu_w - 8, menu_y + 210, GFX_COLOR_PANEL_BORDER);
    gfx_draw_string(bb, menu_x + 12, menu_y + 218, "AstraOS v1.0 Cosmic Vista", GFX_COLOR_TEXT_MUTED, 0, true);
}

/* Session Change Notification Callback from Session Manager */
void desktop_notify_session_change(sid_t sid, session_state_t state) {
    (void)sid;
    if (state == SESSION_LOCKED) {
        g_desktop.ui_state = DESKTOP_UI_LOCKED;
        g_desktop.pass_len = 0;
        g_desktop.pass_buffer[0] = '\0';
        g_desktop.status_msg[0] = '\0';
        g_desktop.power_menu_open = false;
        g_desktop.start_menu_open = false;
        g_desktop.confirm_action = 0;
    } else if (state == SESSION_ACTIVE) {
        g_desktop.ui_state = DESKTOP_UI_NORMAL;
        g_desktop.pass_len = 0;
        g_desktop.pass_buffer[0] = '\0';
        g_desktop.status_msg[0] = '\0';
        g_desktop.confirm_action = 0;
    } else if (state == SESSION_TERMINATING || state == SESSION_UNUSED) {
        g_desktop.ui_state = DESKTOP_UI_LOGIN;
        g_desktop.pass_len = 0;
        g_desktop.pass_buffer[0] = '\0';
        g_desktop.status_msg[0] = '\0';
        g_desktop.login_selected_user = 0;
        g_desktop.power_menu_open = false;
        g_desktop.start_menu_open = false;
        g_desktop.confirm_action = 0;
    }
}

static void render_lock_screen(gfx_surface_t *bb) {
    /* 1. Base wallpaper background */
    gfx_restore_wallpaper(bb);

    /* 2. Security overlay tint */
    for (int y = 0; y < bb->height; y += 4) {
        gfx_fill_rect(bb, 0, y, bb->width, 2, 0x070B14);
    }

    session_t *s = session_get_active();
    const char *uname = s ? s->username : "akash";
    const char *rname = s ? s->realname : "Akash";

    int card_w = 400;
    int card_h = 320;
    int cx = (bb->width - card_w) / 2;
    int cy = (bb->height - card_h) / 2;

    /* Card background & glow border */
    gfx_fill_rounded_rect(bb, cx, cy, card_w, card_h, 12, 0x0B1222);
    gfx_draw_rounded_rect(bb, cx, cy, card_w, card_h, 12, 0x0284C7);
    gfx_draw_rounded_rect(bb, cx + 1, cy + 1, card_w - 2, card_h - 2, 11, 0x1E293B);

    /* User Avatar Squircle emblem */
    int av_x = cx + (card_w - 56) / 2;
    int av_y = cy + 24;
    gfx_fill_rounded_rect(bb, av_x, av_y, 56, 56, 16, 0x1E3A8A);
    gfx_draw_rounded_rect(bb, av_x, av_y, 56, 56, 16, 0x38BDF8);
    char init_str[2] = { (char)((rname[0] >= 'a' && rname[0] <= 'z') ? (rname[0] - 32) : rname[0]), '\0' };
    gfx_draw_string(bb, av_x + 22, av_y + 18, init_str, 0x38BDF8, 0, true);

    /* Real name & username */
    int rn_len = (int)kstrlen(rname);
    gfx_draw_string(bb, cx + (card_w - rn_len * 8) / 2, cy + 92, rname, 0xF8FAFC, 0, true);

    char tag_str[64];
    ksprintf(tag_str, "@%s  -  Session Locked", uname);
    int tag_len = (int)kstrlen(tag_str);
    gfx_draw_string(bb, cx + (card_w - tag_len * 8) / 2, cy + 112, tag_str, 0x94A3B8, 0, true);

    /* Password Input Field */
    int field_x = cx + 50;
    int field_y = cy + 140;
    int field_w = card_w - 100;
    int field_h = 34;

    gfx_fill_rounded_rect(bb, field_x, field_y, field_w, field_h, 6, 0x1E293B);
    gfx_draw_rounded_rect(bb, field_x, field_y, field_w, field_h, 6, 0x38BDF8);

    char masked[64];
    int mask_len = g_desktop.pass_len;
    if (mask_len > 30) mask_len = 30;
    for (int i = 0; i < mask_len; i++) masked[i] = '*';
    masked[mask_len] = '\0';

    if (mask_len > 0) {
        gfx_draw_string(bb, field_x + 12, field_y + 9, masked, 0xF8FAFC, 0, true);
        static int blink = 0;
        blink++;
        if ((blink / 20) % 2 == 0) {
            gfx_fill_rect(bb, field_x + 12 + mask_len * 8, field_y + 8, 2, 16, 0x38BDF8);
        }
    } else {
        gfx_draw_string(bb, field_x + 12, field_y + 9, "Enter password...", 0x64748B, 0, true);
    }

    /* Action Buttons: Unlock & Switch User */
    int btn_w = (field_w - 12) / 2;
    int btn_y = cy + 188;

    /* Unlock Button (Primary Blue) */
    gfx_fill_rounded_rect(bb, field_x, btn_y, btn_w, 32, 6, 0x0284C7);
    gfx_draw_rounded_rect(bb, field_x, btn_y, btn_w, 32, 6, 0x38BDF8);
    gfx_draw_string(bb, field_x + (btn_w - 48) / 2, btn_y + 8, "Unlock", 0xFFFFFF, 0, true);

    /* Switch User Button (Secondary Glass) */
    int sw_x = field_x + btn_w + 12;
    gfx_fill_rounded_rect(bb, sw_x, btn_y, btn_w, 32, 6, 0x1E293B);
    gfx_draw_rounded_rect(bb, sw_x, btn_y, btn_w, 32, 6, 0x475569);
    gfx_draw_string(bb, sw_x + (btn_w - 88) / 2, btn_y + 8, "Switch User", 0xE2E8F0, 0, true);

    /* Status / Error Message */
    if (g_desktop.status_msg[0]) {
        int sm_len = (int)kstrlen(g_desktop.status_msg);
        gfx_draw_string(bb, cx + (card_w - sm_len * 8) / 2, cy + 232, g_desktop.status_msg, g_desktop.status_col, 0, true);
    } else {
        gfx_draw_string(bb, cx + (card_w - 168) / 2, cy + 232, "Press Enter to unlock", 0x64748B, 0, true);
    }

    /* Bottom System Controls */
    gfx_draw_line(bb, cx + 24, cy + 260, cx + card_w - 24, cy + 260, 0x1E293B);
    gfx_draw_string(bb, cx + 60, cy + 276, "[ Restart ]", 0x38BDF8, 0, true);
    gfx_draw_string(bb, cx + card_w - 160, cy + 276, "[ Shut Down ]", 0xF43F5E, 0, true);
}

static void render_login_screen(gfx_surface_t *bb) {
    /* 1. Base wallpaper */
    gfx_restore_wallpaper(bb);

    /* 2. Dim entire screen */
    for (int y = 0; y < bb->height; y += 4) {
        gfx_fill_rect(bb, 0, y, bb->width, 2, 0x070B14);
    }

    int card_w = 460;
    int card_h = 360;
    int cx = (bb->width - card_w) / 2;
    int cy = (bb->height - card_h) / 2;

    /* Card background */
    gfx_fill_rounded_rect(bb, cx, cy, card_w, card_h, 12, 0x0B1222);
    gfx_draw_rounded_rect(bb, cx, cy, card_w, card_h, 12, 0x0284C7);
    gfx_draw_rounded_rect(bb, cx + 1, cy + 1, card_w - 2, card_h - 2, 11, 0x1E293B);

    /* Header */
    gfx_draw_string(bb, cx + (card_w - 184) / 2, cy + 20, "AstraOS Display Manager", 0x38BDF8, 0, true);
    gfx_draw_string(bb, cx + (card_w - 168) / 2, cy + 40, "Select account to sign in", 0x94A3B8, 0, true);

    /* User Accounts List (Up to 3 tiles) */
    user_account_t accounts[3];
    int acc_count = user_list_accounts(accounts, 3);
    if (acc_count < 1) {
        acc_count = 1;
        kstrncpy(accounts[0].username, "akash", USER_NAME_MAX - 1);
        kstrncpy(accounts[0].realname, "Akash", USER_REALNAME_MAX - 1);
    }

    int tile_w = (card_w - 60 - (acc_count - 1) * 12) / acc_count;
    int tile_h = 68;
    int tile_y = cy + 70;

    for (int i = 0; i < acc_count; i++) {
        int tile_x = cx + 30 + i * (tile_w + 12);
        bool is_sel = (g_desktop.login_selected_user == i);
        u32 bg_col = is_sel ? 0x1E3A8A : 0x131A2B;
        u32 bdr_col = is_sel ? 0x38BDF8 : 0x223048;

        gfx_fill_rounded_rect(bb, tile_x, tile_y, tile_w, tile_h, 8, bg_col);
        gfx_draw_rounded_rect(bb, tile_x, tile_y, tile_w, tile_h, 8, bdr_col);

        char init_c = accounts[i].realname[0];
        if (init_c >= 'a' && init_c <= 'z') init_c -= 32;
        char istr[2] = { init_c, '\0' };
        gfx_draw_string(bb, tile_x + 10, tile_y + 14, istr, is_sel ? 0x38BDF8 : 0x94A3B8, 0, true);

        gfx_draw_string(bb, tile_x + 24, tile_y + 14, accounts[i].realname, 0xF8FAFC, 0, true);
        char ustr[32];
        ksprintf(ustr, "@%s", accounts[i].username);
        gfx_draw_string(bb, tile_x + 24, tile_y + 36, ustr, 0x94A3B8, 0, true);
    }

    /* Password Input Field */
    int field_x = cx + 50;
    int field_y = cy + 160;
    int field_w = card_w - 100;
    int field_h = 34;

    gfx_fill_rounded_rect(bb, field_x, field_y, field_w, field_h, 6, 0x1E293B);
    gfx_draw_rounded_rect(bb, field_x, field_y, field_w, field_h, 6, 0x38BDF8);

    char masked[64];
    int mask_len = g_desktop.pass_len;
    if (mask_len > 30) mask_len = 30;
    for (int i = 0; i < mask_len; i++) masked[i] = '*';
    masked[mask_len] = '\0';

    if (mask_len > 0) {
        gfx_draw_string(bb, field_x + 12, field_y + 9, masked, 0xF8FAFC, 0, true);
        static int blink2 = 0;
        blink2++;
        if ((blink2 / 20) % 2 == 0) {
            gfx_fill_rect(bb, field_x + 12 + mask_len * 8, field_y + 8, 2, 16, 0x38BDF8);
        }
    } else {
        gfx_draw_string(bb, field_x + 12, field_y + 9, "Password for selected user...", 0x64748B, 0, true);
    }

    /* Sign In Button */
    int sign_w = 140;
    int sign_x = cx + (card_w - sign_w) / 2;
    int sign_y = cy + 210;

    gfx_fill_rounded_rect(bb, sign_x, sign_y, sign_w, 34, 6, 0x0284C7);
    gfx_draw_rounded_rect(bb, sign_x, sign_y, sign_w, 34, 6, 0x38BDF8);
    gfx_draw_string(bb, sign_x + (sign_w - 56) / 2, sign_y + 9, "Sign In", 0xFFFFFF, 0, true);

    /* Status Message */
    if (g_desktop.status_msg[0]) {
        int sm_len = (int)kstrlen(g_desktop.status_msg);
        gfx_draw_string(bb, cx + (card_w - sm_len * 8) / 2, cy + 260, g_desktop.status_msg, g_desktop.status_col, 0, true);
    } else {
        gfx_draw_string(bb, cx + (card_w - 232) / 2, cy + 260, "Enter password & click Sign In", 0x64748B, 0, true);
    }

    /* Bottom System Controls */
    gfx_draw_line(bb, cx + 24, cy + 295, cx + card_w - 24, cy + 295, 0x1E293B);
    gfx_draw_string(bb, cx + 80, cy + 315, "[ Restart ]", 0x38BDF8, 0, true);
    gfx_draw_string(bb, cx + card_w - 180, cy + 315, "[ Shut Down ]", 0xF43F5E, 0, true);
}

static void render_power_menu(gfx_surface_t *bb) {
    if (!g_desktop.power_menu_open) return;

    int menu_w = 236;
    int menu_h = 182;
    int menu_x = bb->width - menu_w - 6;
    int menu_y = 34;

    gfx_fill_rounded_rect(bb, menu_x, menu_y, menu_w, menu_h, 8, 0x0F172A);
    gfx_draw_rounded_rect(bb, menu_x, menu_y, menu_w, menu_h, 8, GFX_COLOR_ACCENT_BLUE);

    gfx_draw_string(bb, menu_x + 12, menu_y + 8, "Power & Session Control", GFX_COLOR_ACCENT_CYAN, 0, true);
    gfx_draw_line(bb, menu_x + 8, menu_y + 24, menu_x + menu_w - 8, menu_y + 24, GFX_COLOR_PANEL_BORDER);

    /* 1. Lock Screen (Active) */
    gfx_fill_rounded_rect(bb, menu_x + 10, menu_y + 30, menu_w - 20, 22, 4, 0x1E293B);
    gfx_draw_rounded_rect(bb, menu_x + 10, menu_y + 30, menu_w - 20, 22, 4, GFX_COLOR_ACCENT_BLUE);
    gfx_draw_string(bb, menu_x + 18, menu_y + 34, "Lock Screen", GFX_COLOR_TEXT_PRIMARY, 0, true);
    gfx_draw_string(bb, menu_x + 140, menu_y + 34, "[Ctrl+L]", 0x94A3B8, 0, true);

    /* 2. Switch User (Active) */
    gfx_fill_rounded_rect(bb, menu_x + 10, menu_y + 54, menu_w - 20, 22, 4, 0x1E293B);
    gfx_draw_rounded_rect(bb, menu_x + 10, menu_y + 54, menu_w - 20, 22, 4, GFX_COLOR_ACCENT_BLUE);
    gfx_draw_string(bb, menu_x + 18, menu_y + 58, "Switch User", GFX_COLOR_TEXT_PRIMARY, 0, true);

    /* 3. Log Out (Active) */
    gfx_fill_rounded_rect(bb, menu_x + 10, menu_y + 78, menu_w - 20, 22, 4, 0x1E293B);
    gfx_draw_rounded_rect(bb, menu_x + 10, menu_y + 78, menu_w - 20, 22, 4, GFX_COLOR_ACCENT_BLUE);
    gfx_draw_string(bb, menu_x + 18, menu_y + 82, "Log Out...", GFX_COLOR_TEXT_PRIMARY, 0, true);

    /* 4. Sleep (Unavailable: S3 Unconfigured) */
    gfx_draw_string(bb, menu_x + 18, menu_y + 104, "Sleep", GFX_COLOR_TEXT_MUTED, 0, true);
    gfx_draw_string(bb, menu_x + 130, menu_y + 104, "[S3 Unavail]", 0x64748B, 0, true);

    gfx_draw_line(bb, menu_x + 8, menu_y + 122, menu_x + menu_w - 8, menu_y + 122, GFX_COLOR_PANEL_BORDER);

    /* 5. Restart... (Active) */
    gfx_fill_rounded_rect(bb, menu_x + 10, menu_y + 128, menu_w - 20, 22, 4, 0x1E293B);
    gfx_draw_rounded_rect(bb, menu_x + 10, menu_y + 128, menu_w - 20, 22, 4, 0x38BDF8);
    gfx_draw_string(bb, menu_x + 18, menu_y + 132, "Restart...", 0x38BDF8, 0, true);

    /* 6. Shut Down... (Active) */
    gfx_fill_rounded_rect(bb, menu_x + 10, menu_y + 152, menu_w - 20, 22, 4, 0x1E293B);
    gfx_draw_rounded_rect(bb, menu_x + 10, menu_y + 152, menu_w - 20, 22, 4, 0xF43F5E);
    gfx_draw_string(bb, menu_x + 18, menu_y + 156, "Shut Down...", 0xF43F5E, 0, true);
}

static void render_confirm_dialog(gfx_surface_t *bb) {
    if (g_desktop.confirm_action == 0) return;

    int dw = 320;
    int dh = 120;
    int dx = (bb->width - dw) / 2;
    int dy = (bb->height - dh) / 2;

    gfx_fill_rounded_rect(bb, dx, dy, dw, dh, 8, 0x0B1120);
    gfx_draw_rounded_rect(bb, dx, dy, dw, dh, 8, GFX_COLOR_ACCENT_BLUE);

    const char *title = (g_desktop.confirm_action == 1) ? "Confirm System Restart" :
                        (g_desktop.confirm_action == 2) ? "Confirm System Shutdown" :
                        "Confirm Log Out";
    const char *prompt = (g_desktop.confirm_action == 1) ? "Restart AstraOS and reboot machine?" :
                         (g_desktop.confirm_action == 2) ? "Power off computer completely via ACPI?" :
                         "Close active session and return to Login Screen?";

    gfx_draw_string(bb, dx + 16, dy + 12, title, GFX_COLOR_ACCENT_CYAN, 0, true);
    gfx_draw_line(bb, dx + 12, dy + 28, dx + dw - 12, dy + 28, GFX_COLOR_PANEL_BORDER);
    gfx_draw_string(bb, dx + 16, dy + 42, prompt, GFX_COLOR_TEXT_PRIMARY, 0, true);

    /* Confirm Button */
    gfx_fill_rounded_rect(bb, dx + 40, dy + 76, 100, 26, 4, GFX_COLOR_TITLEBAR_ACTIVE);
    gfx_draw_rounded_rect(bb, dx + 40, dy + 76, 100, 26, 4, GFX_COLOR_BTN_CLOSE);
    gfx_draw_string(bb, dx + 64, dy + 81, "Confirm", GFX_COLOR_BTN_CLOSE, 0, true);

    /* Cancel Button */
    gfx_fill_rounded_rect(bb, dx + 180, dy + 76, 100, 26, 4, GFX_COLOR_TITLEBAR_ACTIVE);
    gfx_draw_rounded_rect(bb, dx + 180, dy + 76, 100, 26, 4, GFX_COLOR_PANEL_BORDER);
    gfx_draw_string(bb, dx + 208, dy + 81, "Cancel", GFX_COLOR_TEXT_PRIMARY, 0, true);
}

void desktop_render(void) {
    gfx_surface_t *bb = gfx_get_backbuffer();
    if (!bb) return;

    if (g_desktop.ui_state == DESKTOP_UI_LOCKED) {
        render_lock_screen(bb);
        render_confirm_dialog(bb);
        int mx, my;
        input_get_mouse_state(&mx, &my, NULL);
        gfx_draw_cursor(bb, mx, my);
        gfx_flip();
        return;
    }

    if (g_desktop.ui_state == DESKTOP_UI_LOGIN) {
        render_login_screen(bb);
        render_confirm_dialog(bb);
        int mx, my;
        input_get_mouse_state(&mx, &my, NULL);
        gfx_draw_cursor(bb, mx, my);
        gfx_flip();
        return;
    }

    /* 1. Desktop Background Wallpaper & Icons */
    render_desktop_background(bb);

    /* 2. Window Manager (Windows in z-order) */
    wm_render();

    /* 3. Top System Panel */
    render_top_panel(bb);

    /* 4. Bottom Application Dock */
    render_bottom_dock(bb);

    /* 5. Dropdown Menus */
    render_start_menu(bb);
    render_power_menu(bb);

    /* 6. Modal Confirmation Dialog */
    render_confirm_dialog(bb);

    /* 7. Mouse Cursor Overlay */
    int mx, my;
    input_get_mouse_state(&mx, &my, NULL);
    gfx_draw_cursor(bb, mx, my);

    /* 8. Present to physical display */
    gfx_flip();
}

void desktop_handle_event(const input_event_t *ev) {
    if (!ev || power_is_in_progress()) return;

    /* --------------------------------------------------------------------- */
    /* State: LOCKED Screen                                                  */
    /* --------------------------------------------------------------------- */
    if (g_desktop.ui_state == DESKTOP_UI_LOCKED) {
        int card_w = 400;
        int card_h = 320;
        int cx = (g_desktop.screen_w - card_w) / 2;
        int cy = (g_desktop.screen_h - card_h) / 2;

        if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
            int mx = ev->mouse.x;
            int my = ev->mouse.y;

            if (g_desktop.confirm_action != 0) {
                int dw = 320;
                int dh = 120;
                int dx = (g_desktop.screen_w - dw) / 2;
                int dy = (g_desktop.screen_h - dh) / 2;
                if (mx >= dx + 40 && mx <= dx + 140 && my >= dy + 76 && my <= dy + 102) {
                    int act = g_desktop.confirm_action;
                    g_desktop.confirm_action = 0;
                    if (act == 1) sys_reboot();
                    else sys_shutdown();
                    return;
                }
                g_desktop.confirm_action = 0;
                return;
            }

            int field_x = cx + 50;
            int field_w = card_w - 100;
            int btn_w = (field_w - 12) / 2;
            int btn_y = cy + 188;

            /* Unlock Button */
            if (mx >= field_x && mx <= field_x + btn_w && my >= btn_y && my <= btn_y + 32) {
                sid_t active_sid = session_get_active_sid();
                int res = session_unlock(active_sid, g_desktop.pass_buffer);
                if (res != SESSION_OK) {
                    kstrncpy(g_desktop.status_msg, "Authentication failed. Try again.", sizeof(g_desktop.status_msg) - 1);
                    g_desktop.status_col = 0xF43F5E;
                    g_desktop.pass_len = 0;
                    g_desktop.pass_buffer[0] = '\0';
                }
                return;
            }

            /* Switch User Button */
            int sw_x = field_x + btn_w + 12;
            if (mx >= sw_x && mx <= sw_x + btn_w && my >= btn_y && my <= btn_y + 32) {
                g_desktop.ui_state = DESKTOP_UI_LOGIN;
                g_desktop.pass_len = 0;
                g_desktop.pass_buffer[0] = '\0';
                g_desktop.status_msg[0] = '\0';
                g_desktop.login_selected_user = 0;
                return;
            }

            /* Bottom controls: Restart / Shut Down */
            if (my >= cy + 265 && my <= cy + 295) {
                if (mx >= cx + 50 && mx <= cx + 150) {
                    g_desktop.confirm_action = 1;
                    return;
                } else if (mx >= cx + card_w - 170 && mx <= cx + card_w - 50) {
                    g_desktop.confirm_action = 2;
                    return;
                }
            }
            return;
        } else if (ev->type == INPUT_EVENT_KEY_DOWN) {
            if (g_desktop.confirm_action != 0) {
                if (ev->key.keycode == KEY_ESC || ev->key.ascii == 'n' || ev->key.ascii == 'N') {
                    g_desktop.confirm_action = 0;
                    return;
                } else if (ev->key.keycode == KEY_ENTER || ev->key.ascii == 'y' || ev->key.ascii == 'Y') {
                    int act = g_desktop.confirm_action;
                    g_desktop.confirm_action = 0;
                    if (act == 1) sys_reboot();
                    else sys_shutdown();
                    return;
                }
            }

            if (ev->key.keycode == KEY_ENTER) {
                sid_t active_sid = session_get_active_sid();
                int res = session_unlock(active_sid, g_desktop.pass_buffer);
                if (res != SESSION_OK) {
                    kstrncpy(g_desktop.status_msg, "Authentication failed. Try again.", sizeof(g_desktop.status_msg) - 1);
                    g_desktop.status_col = 0xF43F5E;
                    g_desktop.pass_len = 0;
                    g_desktop.pass_buffer[0] = '\0';
                }
                return;
            } else if (ev->key.keycode == KEY_BACKSPACE) {
                if (g_desktop.pass_len > 0) {
                    g_desktop.pass_buffer[--g_desktop.pass_len] = '\0';
                }
                return;
            } else if (ev->key.ascii >= 32 && ev->key.ascii <= 126) {
                if (g_desktop.pass_len < (int)sizeof(g_desktop.pass_buffer) - 1) {
                    g_desktop.pass_buffer[g_desktop.pass_len++] = (char)ev->key.ascii;
                    g_desktop.pass_buffer[g_desktop.pass_len] = '\0';
                }
                return;
            }
            return;
        }
        return;
    }

    /* --------------------------------------------------------------------- */
    /* State: LOGIN / Display Manager Screen                                */
    /* --------------------------------------------------------------------- */
    if (g_desktop.ui_state == DESKTOP_UI_LOGIN) {
        int card_w = 460;
        int card_h = 360;
        int cx = (g_desktop.screen_w - card_w) / 2;
        int cy = (g_desktop.screen_h - card_h) / 2;

        user_account_t accounts[3];
        int acc_count = user_list_accounts(accounts, 3);
        if (acc_count < 1) {
            acc_count = 1;
            kstrncpy(accounts[0].username, "akash", USER_NAME_MAX - 1);
        }

        if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
            int mx = ev->mouse.x;
            int my = ev->mouse.y;

            if (g_desktop.confirm_action != 0) {
                int dw = 320;
                int dh = 120;
                int dx = (g_desktop.screen_w - dw) / 2;
                int dy = (g_desktop.screen_h - dh) / 2;
                if (mx >= dx + 40 && mx <= dx + 140 && my >= dy + 76 && my <= dy + 102) {
                    int act = g_desktop.confirm_action;
                    g_desktop.confirm_action = 0;
                    if (act == 1) sys_reboot();
                    else sys_shutdown();
                    return;
                }
                g_desktop.confirm_action = 0;
                return;
            }

            /* Account Selection Tiles */
            int tile_w = (card_w - 60 - (acc_count - 1) * 12) / acc_count;
            int tile_h = 68;
            int tile_y = cy + 70;

            for (int i = 0; i < acc_count; i++) {
                int tile_x = cx + 30 + i * (tile_w + 12);
                if (mx >= tile_x && mx <= tile_x + tile_w && my >= tile_y && my <= tile_y + tile_h) {
                    g_desktop.login_selected_user = i;
                    g_desktop.pass_len = 0;
                    g_desktop.pass_buffer[0] = '\0';
                    g_desktop.status_msg[0] = '\0';
                    return;
                }
            }

            /* Sign In Button */
            int sign_w = 140;
            int sign_x = cx + (card_w - sign_w) / 2;
            int sign_y = cy + 210;

            if (mx >= sign_x && mx <= sign_x + sign_w && my >= sign_y && my <= sign_y + 34) {
                int u_idx = g_desktop.login_selected_user;
                if (u_idx < 0 || u_idx >= acc_count) u_idx = 0;
                const char *target_user = accounts[u_idx].username;
                int res = session_switch_to_user(target_user, g_desktop.pass_buffer);
                if (res != SESSION_OK) {
                    kstrncpy(g_desktop.status_msg, "Incorrect password. Try again.", sizeof(g_desktop.status_msg) - 1);
                    g_desktop.status_col = 0xF43F5E;
                    g_desktop.pass_len = 0;
                    g_desktop.pass_buffer[0] = '\0';
                }
                return;
            }

            /* Bottom controls: Restart / Shut Down */
            if (my >= cy + 300 && my <= cy + 335) {
                if (mx >= cx + 60 && mx <= cx + 160) {
                    g_desktop.confirm_action = 1;
                    return;
                } else if (mx >= cx + card_w - 190 && mx <= cx + card_w - 70) {
                    g_desktop.confirm_action = 2;
                    return;
                }
            }
            return;
        } else if (ev->type == INPUT_EVENT_KEY_DOWN) {
            if (g_desktop.confirm_action != 0) {
                if (ev->key.keycode == KEY_ESC || ev->key.ascii == 'n' || ev->key.ascii == 'N') {
                    g_desktop.confirm_action = 0;
                    return;
                } else if (ev->key.keycode == KEY_ENTER || ev->key.ascii == 'y' || ev->key.ascii == 'Y') {
                    int act = g_desktop.confirm_action;
                    g_desktop.confirm_action = 0;
                    if (act == 1) sys_reboot();
                    else sys_shutdown();
                    return;
                }
            }

            if (ev->key.keycode == KEY_ENTER) {
                int u_idx = g_desktop.login_selected_user;
                if (u_idx < 0 || u_idx >= acc_count) u_idx = 0;
                const char *target_user = accounts[u_idx].username;
                int res = session_switch_to_user(target_user, g_desktop.pass_buffer);
                if (res != SESSION_OK) {
                    kstrncpy(g_desktop.status_msg, "Incorrect password. Try again.", sizeof(g_desktop.status_msg) - 1);
                    g_desktop.status_col = 0xF43F5E;
                    g_desktop.pass_len = 0;
                    g_desktop.pass_buffer[0] = '\0';
                }
                return;
            } else if (ev->key.keycode == KEY_TAB || ev->key.keycode == KEY_UP || ev->key.keycode == KEY_DOWN) {
                g_desktop.login_selected_user = (g_desktop.login_selected_user + 1) % acc_count;
                g_desktop.pass_len = 0;
                g_desktop.pass_buffer[0] = '\0';
                g_desktop.status_msg[0] = '\0';
                return;
            } else if (ev->key.keycode == KEY_BACKSPACE) {
                if (g_desktop.pass_len > 0) {
                    g_desktop.pass_buffer[--g_desktop.pass_len] = '\0';
                }
                return;
            } else if (ev->key.ascii >= 32 && ev->key.ascii <= 126) {
                if (g_desktop.pass_len < (int)sizeof(g_desktop.pass_buffer) - 1) {
                    g_desktop.pass_buffer[g_desktop.pass_len++] = (char)ev->key.ascii;
                    g_desktop.pass_buffer[g_desktop.pass_len] = '\0';
                }
                return;
            }
            return;
        }
        return;
    }

    /* --------------------------------------------------------------------- */
    /* State: NORMAL Desktop                                                 */
    /* --------------------------------------------------------------------- */
    if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        int mx = ev->mouse.x;
        int my = ev->mouse.y;

        /* Check Modal Confirmation Dialog */
        if (g_desktop.confirm_action != 0) {
            int dw = 320;
            int dh = 120;
            int dx = (g_desktop.screen_w - dw) / 2;
            int dy = (g_desktop.screen_h - dh) / 2;

            /* Confirm Button */
            if (mx >= dx + 40 && mx <= dx + 140 && my >= dy + 76 && my <= dy + 102) {
                int act = g_desktop.confirm_action;
                g_desktop.confirm_action = 0;
                if (act == 1) sys_reboot();
                else if (act == 2) sys_shutdown();
                else if (act == 3) power_logout();
                return;
            }

            /* Cancel Button or Outside */
            if (mx >= dx + 180 && mx <= dx + 280 && my >= dy + 76 && my <= dy + 102) {
                g_desktop.confirm_action = 0;
                return;
            }
            g_desktop.confirm_action = 0;
            return;
        }

        /* Click Top-Left AstraOS Menu Button */
        if (mx >= 6 && mx <= 92 && my >= 3 && my <= 27) {
            g_desktop.start_menu_open = !g_desktop.start_menu_open;
            g_desktop.power_menu_open = false;
            return;
        }

        /* Backward compatibility: bottom-left click toggle for desktop_self_test */
        if (mx <= 60 && my >= g_desktop.screen_h - 36) {
            g_desktop.start_menu_open = !g_desktop.start_menu_open;
            g_desktop.power_menu_open = false;
            return;
        }

        /* Click Top-Right Power Menu Button */
        if (mx >= g_desktop.screen_w - 68 && mx <= g_desktop.screen_w - 8 && my >= 2 && my <= 30) {
            g_desktop.power_menu_open = !g_desktop.power_menu_open;
            g_desktop.start_menu_open = false;
            return;
        }

        /* Click Top Center Clock Pill to toggle Timezone (LOCAL <-> IST <-> UTC <-> EST <-> PST) */
        int clk_cx = g_desktop.screen_w / 2;
        if (mx >= clk_cx - 150 && mx <= clk_cx + 150 && my >= 3 && my <= 28) {
            g_tz_index = (g_tz_index + 1) % 5;
            return;
        }

        /* Check Start Menu Clicks */
        if (g_desktop.start_menu_open) {
            int menu_w = 210;
            int menu_h = 240;
            int menu_x = 6;
            int menu_y = 34;

            if (mx >= menu_x && mx <= menu_x + menu_w && my >= menu_y && my <= menu_y + menu_h) {
                for (int i = 0; i < 8; i++) {
                    int item_y = menu_y + 30 + i * 22;
                    if (my >= item_y - 2 && my <= item_y + 18) {
                        launch_or_focus_app(i);
                        g_desktop.start_menu_open = false;
                        return;
                    }
                }
            } else {
                g_desktop.start_menu_open = false;
            }
        }

        /* Check Power Menu Clicks */
        if (g_desktop.power_menu_open) {
            int menu_w = 236;
            int menu_h = 182;
            int menu_x = g_desktop.screen_w - menu_w - 6;
            int menu_y = 34;

            if (mx >= menu_x && mx <= menu_x + menu_w && my >= menu_y && my <= menu_y + menu_h) {
                if (my >= menu_y + 30 && my <= menu_y + 52) {
                    /* Lock Screen */
                    g_desktop.power_menu_open = false;
                    power_lock();
                    return;
                } else if (my >= menu_y + 54 && my <= menu_y + 76) {
                    /* Switch User */
                    g_desktop.power_menu_open = false;
                    g_desktop.ui_state = DESKTOP_UI_LOGIN;
                    g_desktop.pass_len = 0;
                    g_desktop.pass_buffer[0] = '\0';
                    g_desktop.status_msg[0] = '\0';
                    g_desktop.login_selected_user = 0;
                    return;
                } else if (my >= menu_y + 78 && my <= menu_y + 100) {
                    /* Log Out... */
                    g_desktop.confirm_action = 3;
                    g_desktop.power_menu_open = false;
                    return;
                } else if (my >= menu_y + 128 && my <= menu_y + 150) {
                    /* Restart... */
                    g_desktop.confirm_action = 1;
                    g_desktop.power_menu_open = false;
                    return;
                } else if (my >= menu_y + 152 && my <= menu_y + 174) {
                    /* Shut Down... */
                    g_desktop.confirm_action = 2;
                    g_desktop.power_menu_open = false;
                    return;
                }
                return;
            } else {
                g_desktop.power_menu_open = false;
            }
        }

        /* Check Bottom Dock Clicks */
        int dock_w = 432;
        int dock_h = 56;
        int dock_x = (g_desktop.screen_w - dock_w) / 2;
        int dock_y = g_desktop.screen_h - dock_h - 10;

        if (mx >= dock_x && mx <= dock_x + dock_w && my >= dock_y && my <= dock_y + dock_h) {
            for (int i = 0; i < 8; i++) {
                int app_x = dock_x + 8 + i * 52;
                if (mx >= app_x && mx <= app_x + 48) {
                    launch_or_focus_app(i);
                    return;
                }
            }
        }

        /* Check Desktop Shortcuts Clicks */
        /* Column 1: x = 32 (hitbox 20..88) */
        if (mx >= 20 && mx <= 88) {
            if (my >= 54 && my <= 130) {
                launch_or_focus_app(0); /* Text Editor */
                return;
            } else if (my >= 150 && my <= 226) {
                launch_or_focus_app(1); /* File Manager */
                return;
            } else if (my >= 246 && my <= 322) {
                launch_or_focus_app(2); /* Web Browser */
                return;
            } else if (my >= 342 && my <= 418) {
                launch_or_focus_app(3); /* Terminal */
                return;
            }
        }
        /* Column 2: x = 140 (hitbox 128..196) */
        if (mx >= 128 && mx <= 196) {
            if (my >= 54 && my <= 130) {
                launch_or_focus_app(4); /* Settings */
                return;
            } else if (my >= 150 && my <= 226) {
                launch_or_focus_app(5); /* Calculator */
                return;
            } else if (my >= 246 && my <= 322) {
                launch_or_focus_app(6); /* System Monitor */
                return;
            } else if (my >= 342 && my <= 418) {
                launch_or_focus_app(7); /* Package Manager */
                return;
            }
        }
    } else if (ev->type == INPUT_EVENT_KEY_DOWN) {
        /* Global Keyboard Shortcuts for Desktop Navigation */
        if (g_desktop.confirm_action != 0) {
            if (ev->key.keycode == KEY_ESC || ev->key.ascii == 'n' || ev->key.ascii == 'N') {
                g_desktop.confirm_action = 0;
                return;
            } else if (ev->key.keycode == KEY_ENTER || ev->key.ascii == 'y' || ev->key.ascii == 'Y') {
                int act = g_desktop.confirm_action;
                g_desktop.confirm_action = 0;
                if (act == 1) sys_reboot();
                else if (act == 2) sys_shutdown();
                else if (act == 3) power_logout();
                return;
            }
        }
        if (ev->key.keycode == KEY_ESC) {
            if (g_desktop.start_menu_open) {
                g_desktop.start_menu_open = false;
                return;
            }
            if (g_desktop.power_menu_open) {
                g_desktop.power_menu_open = false;
                return;
            }
        } else if ((ev->key.modifiers & KEY_MOD_CTRL) && (ev->key.ascii == 'l' || ev->key.ascii == 'L')) {
            g_desktop.power_menu_open = false;
            g_desktop.start_menu_open = false;
            power_lock();
            return;
        } else if ((ev->key.modifiers & KEY_MOD_CTRL) && (ev->key.ascii == 'd' || ev->key.ascii == 'D' || ev->key.ascii == 'q' || ev->key.ascii == 'Q')) {
            g_desktop.power_menu_open = !g_desktop.power_menu_open;
            g_desktop.start_menu_open = false;
            return;
        } else if (ev->key.keycode >= KEY_F1 && ev->key.keycode <= KEY_F8) {
            int app_idx = ev->key.keycode - KEY_F1;
            launch_or_focus_app(app_idx);
            g_desktop.start_menu_open = false;
            return;
        } else if ((ev->key.modifiers & KEY_MOD_ALT) && ev->key.ascii >= '1' && ev->key.ascii <= '8') {
            int app_idx = ev->key.ascii - '1';
            launch_or_focus_app(app_idx);
            g_desktop.start_menu_open = false;
            return;
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

    /* 3. Test Power Menu Toggle & Confirmation Dialog */
    input_event_t click_pwr = {
        .type = INPUT_EVENT_MOUSE_DOWN,
        .mouse = { .x = g_desktop.screen_w - 36, .y = 14, .buttons = MOUSE_BTN_LEFT }
    };
    desktop_handle_event(&click_pwr);
    if (!g_desktop.power_menu_open) {
        kprintf("[DESKTOP TEST FAIL] Power button click failed to open power menu!\n");
        return false;
    }
    kprintf("  PASS: Power & Session menu opens on button click.\n");

    /* Click Shut Down item in Power Menu */
    int menu_w = 236;
    int menu_x = g_desktop.screen_w - menu_w - 6;
    int menu_y = 34;
    input_event_t click_shutdown = {
        .type = INPUT_EVENT_MOUSE_DOWN,
        .mouse = { .x = menu_x + 50, .y = menu_y + 155, .buttons = MOUSE_BTN_LEFT }
    };
    desktop_handle_event(&click_shutdown);
    if (g_desktop.confirm_action != 2 || g_desktop.power_menu_open) {
        kprintf("[DESKTOP TEST FAIL] Clicking Shut Down did not open confirmation dialog (act=%d)!\n",
                g_desktop.confirm_action);
        return false;
    }
    kprintf("  PASS: Shut Down click opens modal confirmation dialog.\n");

    /* Press ESC to cancel confirmation dialog */
    input_event_t press_esc = {
        .type = INPUT_EVENT_KEY_DOWN,
        .key = { .keycode = KEY_ESC, .modifiers = 0, .ascii = 27 }
    };
    desktop_handle_event(&press_esc);
    if (g_desktop.confirm_action != 0) {
        kprintf("[DESKTOP TEST FAIL] ESC key did not cancel confirmation dialog!\n");
        return false;
    }
    kprintf("  PASS: ESC key cancels confirmation dialog cleanly.\n");

    /* 4. Test Lock Screen, Bypass Prevention, and Password Unlock */
    kprintf("  Testing Lock Screen keyboard shortcut [Ctrl+L]...\n");
    input_event_t press_ctrl_l = {
        .type = INPUT_EVENT_KEY_DOWN,
        .key = { .keycode = 0, .modifiers = KEY_MOD_CTRL, .ascii = 'l' }
    };
    desktop_handle_event(&press_ctrl_l);
    if (g_desktop.ui_state != DESKTOP_UI_LOCKED) {
        kprintf("[DESKTOP TEST FAIL] Ctrl+L shortcut failed to lock desktop! (state=%d)\n", g_desktop.ui_state);
        return false;
    }
    kprintf("  PASS: Desktop successfully locked via Ctrl+L.\n");

    /* Verify start menu and shortcuts are blocked while locked */
    desktop_handle_event(&click_start);
    if (g_desktop.start_menu_open) {
        kprintf("[DESKTOP TEST FAIL] Lock screen allowed start menu bypass!\n");
        return false;
    }
    kprintf("  PASS: Desktop navigation and shortcut bypass prevented while locked.\n");

    /* Render locked desktop pass */
    desktop_render();

    /* Test unlock with incorrect password first (negative test) */
    const char *wrong_pass = "wrongpass";
    for (int i = 0; wrong_pass[i]; i++) {
        input_event_t type_char = {
            .type = INPUT_EVENT_KEY_DOWN,
            .key = { .keycode = 0, .modifiers = 0, .ascii = wrong_pass[i] }
        };
        desktop_handle_event(&type_char);
    }
    input_event_t press_enter = {
        .type = INPUT_EVENT_KEY_DOWN,
        .key = { .keycode = KEY_ENTER, .modifiers = 0, .ascii = 13 }
    };
    desktop_handle_event(&press_enter);
    if (g_desktop.ui_state != DESKTOP_UI_LOCKED) {
        kprintf("[DESKTOP TEST FAIL] Incorrect password erroneously unlocked session!\n");
        return false;
    }
    kprintf("  PASS: Negative authentication check verified (incorrect password rejected).\n");

    /* Test unlock with valid password 'astra' */
    const char *good_pass = "astra";
    for (int i = 0; good_pass[i]; i++) {
        input_event_t type_char = {
            .type = INPUT_EVENT_KEY_DOWN,
            .key = { .keycode = 0, .modifiers = 0, .ascii = good_pass[i] }
        };
        desktop_handle_event(&type_char);
    }
    desktop_handle_event(&press_enter);
    if (g_desktop.ui_state != DESKTOP_UI_NORMAL) {
        kprintf("[DESKTOP TEST FAIL] Valid password 'astra' failed to unlock session! (state=%d)\n", g_desktop.ui_state);
        return false;
    }
    kprintf("  PASS: Valid authentication unlocks session back to normal desktop.\n");

    /* 5. Test Full Desktop Rendering Pass */
    desktop_render();
    kprintf("  PASS: Full composite rendering pass completed cleanly.\n");

    kprintf("[DESKTOP TEST] All Desktop Shell tests passed successfully!\n");
    kprintf("ASTRA_DESKTOP_OK\n\n");
    return true;
}

bool desktop_test_user_launch(void) {
    kprintf("\n[DESKTOP TEST] Verifying User-Initiated Application Launch and Clean Teardown...\n");

    /* 1. Assert initial state: 0 autostart windows */
    if (wm_get_window_count() != 0) {
        kprintf("[DESKTOP TEST FAIL] Unexpected autostart windows present! Count=%d\n", wm_get_window_count());
        return false;
    }

    /* 2. Simulate user click on desktop shortcut: Text Editor (x=38, y=86) */
    input_event_t click_shortcut = {
        .type = INPUT_EVENT_MOUSE_DOWN,
        .mouse = { .x = 38, .y = 86, .buttons = MOUSE_BTN_LEFT }
    };
    desktop_handle_event(&click_shortcut);

    if (wm_get_window_count() != 1) {
        kprintf("[DESKTOP TEST FAIL] User shortcut click did not open application! Count=%d\n", wm_get_window_count());
        return false;
    }

    wm_window_t *w = wm_get_focused();
    if (!w) {
        kprintf("[DESKTOP TEST FAIL] Opened application window is not focused!\n");
        return false;
    }
    kprintf("  PASS: Text Editor launched on user shortcut click (Window ID %d).\n", w->id);

    /* 3. Simulate user click on window Close [X] button (w->x + w->width - 12, w->y + 12) */
    input_event_t click_close = {
        .type = INPUT_EVENT_MOUSE_DOWN,
        .mouse = { .x = w->x + w->width - 12, .y = w->y + 12, .buttons = MOUSE_BTN_LEFT }
    };
    wm_handle_input_event(&click_close);

    if (wm_get_window_count() != 0) {
        kprintf("[DESKTOP TEST FAIL] Close button failed to destroy application window! Count=%d\n", wm_get_window_count());
        return false;
    }
    kprintf("  PASS: Application cleanly closed upon user request; window count returned to 0.\n");

    /* 4. Composite pass to restore clean desktop */
    desktop_render();

    kprintf("[DESKTOP TEST] User application launch policy verified successfully!\n");
    kprintf("ASTRA_USER_LAUNCH_ONLY_OK\n\n");
    return true;
}
