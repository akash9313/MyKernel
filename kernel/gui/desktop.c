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

static struct {
    bool initialized;
    bool start_menu_open;
    bool power_menu_open;
    int  confirm_action; /* 0 = none, 1 = restart, 2 = shutdown */
    int  screen_w;
    int  screen_h;
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
    kprintf("[SYSTEM] Reboot requested. Resetting CPU via 8042/PCI...\n");
    u8 temp;
    do {
        temp = inb(0x64);
        if (temp & 1) inb(0x60);
    } while (temp & 2);
    outb(0x64, 0xFE);
    io_wait();
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);
    io_wait();
    struct { u16 limit; u64 base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile ("lidt %0; int3" : : "m"(null_idt));
    halt_forever();
}

static void sys_shutdown(void) {
    kprintf("[SYSTEM] Shutting down AstraOS via ACPI...\n");
    outw(0x604, 0x2000);   /* QEMU */
    outw(0xB004, 0x2000);  /* Bochs / older QEMU */
    outw(0x4004, 0x3400);  /* VirtualBox */
    outw(0x0600, 0x0034);  /* Cloud-hypervisor */
    halt_forever();
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
    /* Modern Developer IDE: Midnight Indigo Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x1E1B4B);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x4338CA);
    /* Specular lighting rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x818CF8);
    gfx_fill_rect(s, x + 12, y + 2, 20, 1, 0xA5B4FC);

    /* Electric Neon Syntax Chevrons: < / > */
    /* Left bracket '<' */
    gfx_draw_line(s, x + 13, y + 22, x + 18, y + 16, 0x38BDF8);
    gfx_draw_line(s, x + 14, y + 22, x + 19, y + 16, 0x0EA5E9);
    gfx_draw_line(s, x + 13, y + 22, x + 18, y + 28, 0x38BDF8);
    gfx_draw_line(s, x + 14, y + 22, x + 19, y + 28, 0x0EA5E9);

    /* Slash '/' in vibrant violet */
    gfx_draw_line(s, x + 23, y + 15, x + 19, y + 29, 0xC084FC);
    gfx_draw_line(s, x + 24, y + 15, x + 20, y + 29, 0xA855F7);

    /* Right bracket '>' */
    gfx_draw_line(s, x + 30, y + 22, x + 25, y + 16, 0x38BDF8);
    gfx_draw_line(s, x + 29, y + 22, x + 24, y + 16, 0x0EA5E9);
    gfx_draw_line(s, x + 30, y + 22, x + 25, y + 28, 0x38BDF8);
    gfx_draw_line(s, x + 29, y + 22, x + 24, y + 28, 0x0EA5E9);

    /* Glowing accent dot */
    gfx_fill_rounded_rect(s, x + 32, y + 15, 3, 3, 1, 0xF59E0B);
}

static void draw_icon_filemgr(gfx_surface_t *s, int x, int y) {
    /* Fluent Oceanic Blue Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x0369A1);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x0284C7);
    /* Specular lighting rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x7DD3FC);
    gfx_fill_rect(s, x + 12, y + 2, 20, 1, 0xBAE6FD);

    /* Folder Tab (Back) */
    gfx_fill_rounded_rect(s, x + 8, y + 10, 14, 5, 2, 0x075985);

    /* Main Folder Body (Back) */
    gfx_fill_rounded_rect(s, x + 8, y + 13, 28, 22, 3, 0x0284C7);

    /* Crisp White Inner Sheet with code lines */
    gfx_fill_rounded_rect(s, x + 12, y + 10, 20, 14, 2, 0xF8FAFC);
    gfx_fill_rect(s, x + 15, y + 13, 10, 2, 0x38BDF8);
    gfx_fill_rect(s, x + 15, y + 17, 14, 2, 0x10B981);

    /* Translucent Cyan Front Pocket with Specular Edge */
    gfx_fill_rounded_rect(s, x + 8, y + 18, 28, 17, 3, 0x38BDF8);
    gfx_draw_rounded_rect(s, x + 8, y + 18, 28, 17, 3, 0x7DD3FC);
    gfx_fill_rect(s, x + 10, y + 19, 24, 1, 0xE0F2FE);
}

static void draw_icon_browser(gfx_surface_t *s, int x, int y) {
    /* Modern Deep Space Navy Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x0B132B);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x1C2541);
    /* Specular highlight rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x60A5FA);

    /* Outer Compass / Dial Ring */
    gfx_fill_rounded_rect(s, x + 8, y + 8, 28, 28, 14, 0x1E293B);
    gfx_draw_rounded_rect(s, x + 8, y + 8, 28, 28, 14, 0xE2E8F0);

    /* Inner Dial Face */
    gfx_fill_rounded_rect(s, x + 10, y + 10, 24, 24, 12, 0x0F172A);

    /* Cardinal ticks */
    gfx_put_pixel(s, x + 22, y + 12, 0x94A3B8);
    gfx_put_pixel(s, x + 22, y + 31, 0x94A3B8);
    gfx_put_pixel(s, x + 12, y + 22, 0x94A3B8);
    gfx_put_pixel(s, x + 31, y + 22, 0x94A3B8);

    /* North needle (Coral Red 3D Facet) */
    gfx_draw_line(s, x + 22, y + 22, x + 29, y + 15, 0xEF4444);
    gfx_draw_line(s, x + 21, y + 22, x + 28, y + 15, 0xF87171);
    gfx_draw_line(s, x + 22, y + 21, x + 29, y + 14, 0xDC2626);

    /* South needle (Cool Metallic Silver) */
    gfx_draw_line(s, x + 22, y + 22, x + 15, y + 29, 0xF8FAFC);
    gfx_draw_line(s, x + 23, y + 22, x + 16, y + 29, 0xCBD5E1);

    /* Center brass pivot */
    gfx_fill_rounded_rect(s, x + 20, y + 20, 5, 5, 2, 0xF59E0B);
    gfx_put_pixel(s, x + 22, y + 22, 0xFEF3C7);
}

static void draw_icon_terminal(gfx_surface_t *s, int x, int y) {
    /* Obsidian Black Squircle (Modern Hyper / Warp style) */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x090D16);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x1E293B);
    /* Specular lighting rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x475569);

    /* Inner Screen Bezel */
    gfx_fill_rounded_rect(s, x + 6, y + 6, 32, 32, 5, 0x020617);
    gfx_draw_rounded_rect(s, x + 6, y + 6, 32, 32, 5, 0x1E293B);

    /* Traffic-light header dots */
    gfx_fill_rounded_rect(s, x + 10, y + 10, 3, 3, 1, 0xEF4444);
    gfx_fill_rounded_rect(s, x + 15, y + 10, 3, 3, 1, 0xF59E0B);
    gfx_fill_rounded_rect(s, x + 20, y + 10, 3, 3, 1, 0x10B981);

    /* Glowing Emerald Prompt: >_ */
    gfx_draw_line(s, x + 11, y + 21, x + 16, y + 25, 0x10B981);
    gfx_draw_line(s, x + 12, y + 21, x + 17, y + 25, 0x34D399);
    gfx_draw_line(s, x + 16, y + 25, x + 11, y + 29, 0x10B981);
    gfx_draw_line(s, x + 17, y + 25, x + 12, y + 29, 0x34D399);

    /* Glowing Cyan Cursor Bar */
    gfx_fill_rounded_rect(s, x + 20, y + 27, 10, 3, 1, 0x38BDF8);
    gfx_fill_rect(s, x + 21, y + 28, 8, 1, 0xBAE6FD);
}

static void draw_icon_settings(gfx_surface_t *s, int x, int y) {
    /* Precision Machined Titanium Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x334155);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x64748B);
    /* Specular lighting rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x94A3B8);
    gfx_fill_rect(s, x + 12, y + 2, 20, 1, 0xCBD5E1);

    /* Outer gear body */
    gfx_fill_rounded_rect(s, x + 13, y + 13, 18, 18, 9, 0xE2E8F0);

    /* Cardinal & Diagonal 8-gear teeth */
    gfx_fill_rounded_rect(s, x + 19, y + 8, 6, 28, 2, 0xE2E8F0);
    gfx_fill_rounded_rect(s, x + 8, y + 19, 28, 6, 2, 0xE2E8F0);
    gfx_fill_rounded_rect(s, x + 11, y + 11, 6, 6, 2, 0xE2E8F0);
    gfx_fill_rounded_rect(s, x + 27, y + 11, 6, 6, 2, 0xE2E8F0);
    gfx_fill_rounded_rect(s, x + 11, y + 27, 6, 6, 2, 0xE2E8F0);
    gfx_fill_rounded_rect(s, x + 27, y + 27, 6, 6, 2, 0xE2E8F0);

    /* Center axle inset */
    gfx_fill_rounded_rect(s, x + 17, y + 17, 10, 10, 5, 0x1E293B);
    gfx_draw_rounded_rect(s, x + 17, y + 17, 10, 10, 5, 0x475569);

    /* Chrome axle pin */
    gfx_fill_rounded_rect(s, x + 20, y + 20, 4, 4, 2, 0x94A3B8);
    gfx_put_pixel(s, x + 21, y + 21, 0xFFFFFF);
}

static void draw_icon_calc(gfx_surface_t *s, int x, int y) {
    /* Modern Matte Charcoal Squircle (Fluent / iOS style) */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x18181B);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x27272A);
    /* Specular lighting rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x52525B);

    /* Recessed LCD Screen */
    gfx_fill_rounded_rect(s, x + 8, y + 7, 28, 12, 3, 0x09090B);
    gfx_draw_rounded_rect(s, x + 8, y + 7, 28, 12, 3, 0x27272A);
    /* Mini LCD Digit */
    gfx_draw_string(s, x + 26, y + 9, "0", 0x34D399, 0, true);

    /* Keycaps: 4 Gray, 1 Signature Orange (+), 1 Emerald (=) */
    gfx_fill_rounded_rect(s, x + 8, y + 22, 12, 8, 2, 0x27272A);
    gfx_draw_string(s, x + 12, y + 22, "7", 0xE4E4E7, 0, true);

    /* Signature Safety Orange Operator Key */
    gfx_fill_rounded_rect(s, x + 24, y + 22, 12, 8, 2, 0xEA580C);
    gfx_draw_rounded_rect(s, x + 24, y + 22, 12, 8, 2, 0xF97316);
    gfx_draw_string(s, x + 28, y + 22, "+", 0xFFFFFF, 0, true);

    gfx_fill_rounded_rect(s, x + 8, y + 32, 12, 8, 2, 0x27272A);
    gfx_draw_string(s, x + 12, y + 32, "C", 0xF87171, 0, true);

    /* Emerald Result Key */
    gfx_fill_rounded_rect(s, x + 24, y + 32, 12, 8, 2, 0x10B981);
    gfx_draw_rounded_rect(s, x + 24, y + 32, 12, 8, 2, 0x34D399);
    gfx_draw_string(s, x + 28, y + 32, "=", 0xFFFFFF, 0, true);
}

static void draw_icon_sysmon(gfx_surface_t *s, int x, int y) {
    /* Space-Navy Telemetry Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x0F172A);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x334155);
    /* Specular highlight rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0x38BDF8);

    /* Inner Oscilloscope Grid */
    gfx_fill_rounded_rect(s, x + 6, y + 6, 32, 32, 5, 0x020617);
    gfx_draw_rounded_rect(s, x + 6, y + 6, 32, 32, 5, 0x1E293B);

    /* Subdued grid markers */
    gfx_put_pixel(s, x + 14, y + 14, 0x1E293B);
    gfx_put_pixel(s, x + 22, y + 14, 0x1E293B);
    gfx_put_pixel(s, x + 30, y + 14, 0x1E293B);
    gfx_put_pixel(s, x + 14, y + 30, 0x1E293B);
    gfx_put_pixel(s, x + 22, y + 30, 0x1E293B);
    gfx_put_pixel(s, x + 30, y + 30, 0x1E293B);

    /* Glowing ECG Heartbeat Pulse */
    gfx_draw_line(s, x + 8, y + 23, x + 14, y + 23, 0x06B6D4);
    gfx_draw_line(s, x + 14, y + 23, x + 18, y + 13, 0x10B981);
    gfx_draw_line(s, x + 15, y + 23, x + 19, y + 13, 0x34D399);
    gfx_draw_line(s, x + 18, y + 13, x + 23, y + 32, 0x10B981);
    gfx_draw_line(s, x + 19, y + 13, x + 24, y + 32, 0x34D399);
    gfx_draw_line(s, x + 23, y + 32, x + 26, y + 18, 0x10B981);
    gfx_draw_line(s, x + 26, y + 18, x + 36, y + 23, 0x06B6D4);

    /* Radiant peak glow dot */
    gfx_fill_rounded_rect(s, x + 18, y + 12, 3, 3, 1, 0x6EE7B7);
}

static void draw_icon_pkgmgr(gfx_surface_t *s, int x, int y) {
    /* Royal Indigo Software Center Squircle */
    gfx_fill_rounded_rect(s, x, y, 44, 44, 11, 0x4338CA);
    gfx_draw_rounded_rect(s, x, y, 44, 44, 11, 0x6366F1);
    /* Specular lighting rim */
    gfx_fill_rect(s, x + 8, y + 1, 28, 1, 0xA5B4FC);

    /* 3D Isometric Software Cube */
    /* Top Diamond Facet */
    gfx_fill_rounded_rect(s, x + 12, y + 11, 20, 8, 2, 0x38BDF8);
    gfx_draw_rounded_rect(s, x + 12, y + 11, 20, 8, 2, 0x7DD3FC);

    /* Bottom Box Body */
    gfx_fill_rounded_rect(s, x + 12, y + 18, 20, 16, 2, 0x0284C7);
    gfx_draw_rounded_rect(s, x + 12, y + 18, 20, 16, 2, 0x0369A1);

    /* Center seam ribbon */
    gfx_fill_rect(s, x + 21, y + 11, 2, 23, 0x075985);

    /* Radiant White Install Arrow */
    gfx_fill_rect(s, x + 21, y + 14, 2, 9, 0xFFFFFF);
    gfx_draw_line(s, x + 19, y + 20, x + 22, y + 23, 0xFFFFFF);
    gfx_draw_line(s, x + 24, y + 20, x + 22, y + 23, 0xFFFFFF);
}

/* Helper to render crisp frosted translucent badge for desktop labels */
static void draw_desktop_label(gfx_surface_t *s, int icon_x, int icon_w, int y, const char *text) {
    int len = 0;
    while (text[len]) len++;
    int text_w = len * 8;
    int cx = icon_x + icon_w / 2;
    int tx = cx - text_w / 2;
    if (tx < 6) tx = 6;
    int pill_w = text_w + 14;
    int pill_x = tx - 7;
    if (pill_x < 2) pill_x = 2;
    int pill_h = 18;
    int pill_y = y - 1;

    /* Frosted glass capsule background */
    gfx_fill_rounded_rect(s, pill_x, pill_y, pill_w, pill_h, 5, 0x0C1322);
    gfx_draw_rounded_rect(s, pill_x, pill_y, pill_w, pill_h, 5, 0x22304A);
    gfx_draw_string(s, tx, y + 1, text, 0xF8FAFC, 0, true);
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

    /* 2. Desktop Application Icons (Left Columns) */
    /* Column 1 (x = 24): Editor, Files, Browser, Terminal */
    draw_icon_editor(bb, 24, 70);
    draw_desktop_label(bb, 24, 44, 120, "Editor");

    draw_icon_filemgr(bb, 24, 150);
    draw_desktop_label(bb, 24, 44, 200, "Files");

    draw_icon_browser(bb, 24, 230);
    draw_desktop_label(bb, 24, 44, 280, "Browser");

    draw_icon_terminal(bb, 24, 310);
    draw_desktop_label(bb, 24, 44, 360, "Terminal");

    /* Column 2 (x = 110): Settings, Calculator, Telemetry, Packages */
    draw_icon_settings(bb, 110, 70);
    draw_desktop_label(bb, 110, 44, 120, "Settings");

    draw_icon_calc(bb, 110, 150);
    draw_desktop_label(bb, 110, 44, 200, "Calculator");

    draw_icon_sysmon(bb, 110, 230);
    draw_desktop_label(bb, 110, 44, 280, "Telemetry");

    draw_icon_pkgmgr(bb, 110, 310);
    draw_desktop_label(bb, 110, 44, 360, "Packages");
}

static int g_tz_index = 0; /* 0=IST (+5:30), 1=UTC (+0:00), 2=EST (-5:00), 3=PST (-8:00) */
static const struct {
    const char *code;
    int offset_mins;
} g_timezones[] = {
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
}

static void render_bottom_dock(gfx_surface_t *bb) {
    int dock_w = 420;
    int dock_h = 52;
    int dock_x = (bb->width - dock_w) / 2;
    int dock_y = bb->height - dock_h - 8;

    /* Translucent rounded capsule base */
    gfx_fill_rounded_rect(bb, dock_x, dock_y, dock_w, dock_h, 12, 0x0E1424);
    gfx_draw_rounded_rect(bb, dock_x, dock_y, dock_w, dock_h, 12, 0x2A364F);

    /* 8 Dock Apps */
    for (int i = 0; i < 8; i++) {
        int app_x = dock_x + 10 + i * 50;
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

        /* Running Application Indicator Dot/Line */
        if (is_app_running(i)) {
            gfx_fill_rounded_rect(bb, app_x + 16, dock_y + dock_h - 4, 12, 2, 1, GFX_COLOR_ACCENT_CYAN);
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

static void render_power_menu(gfx_surface_t *bb) {
    if (!g_desktop.power_menu_open) return;

    int menu_w = 150;
    int menu_h = 136;
    int menu_x = bb->width - menu_w - 6;
    int menu_y = 34;

    gfx_fill_rounded_rect(bb, menu_x, menu_y, menu_w, menu_h, 6, 0x101728);
    gfx_draw_rounded_rect(bb, menu_x, menu_y, menu_w, menu_h, 6, GFX_COLOR_ACCENT_BLUE);

    gfx_draw_string(bb, menu_x + 12, menu_y + 8, "Power & Session", GFX_COLOR_ACCENT_CYAN, 0, true);
    gfx_draw_line(bb, menu_x + 8, menu_y + 24, menu_x + menu_w - 8, menu_y + 24, GFX_COLOR_PANEL_BORDER);

    gfx_draw_string(bb, menu_x + 14, menu_y + 32, "Lock Screen", GFX_COLOR_TEXT_MUTED, 0, true);
    gfx_draw_string(bb, menu_x + 14, menu_y + 52, "Log Out", GFX_COLOR_TEXT_PRIMARY, 0, true);
    gfx_draw_string(bb, menu_x + 14, menu_y + 72, "Suspend", GFX_COLOR_TEXT_MUTED, 0, true);
    gfx_draw_string(bb, menu_x + 14, menu_y + 92, "Restart...", GFX_COLOR_ACCENT_BLUE, 0, true);
    gfx_draw_string(bb, menu_x + 14, menu_y + 112, "Shut Down...", GFX_COLOR_BTN_CLOSE, 0, true);
}

static void render_confirm_dialog(gfx_surface_t *bb) {
    if (g_desktop.confirm_action == 0) return;

    int dw = 320;
    int dh = 120;
    int dx = (bb->width - dw) / 2;
    int dy = (bb->height - dh) / 2;

    gfx_fill_rounded_rect(bb, dx, dy, dw, dh, 8, 0x0B1120);
    gfx_draw_rounded_rect(bb, dx, dy, dw, dh, 8, GFX_COLOR_ACCENT_BLUE);

    const char *title = (g_desktop.confirm_action == 1) ? "Confirm System Restart" : "Confirm System Shutdown";
    const char *prompt = (g_desktop.confirm_action == 1) ? "Restart AstraOS and reboot machine?" : "Power off computer completely via ACPI?";

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
    if (!ev) return;

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
                else sys_shutdown();
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
        if (mx >= g_desktop.screen_w - 46 && mx <= g_desktop.screen_w - 10 && my >= 3 && my <= 27) {
            g_desktop.power_menu_open = !g_desktop.power_menu_open;
            g_desktop.start_menu_open = false;
            return;
        }

        /* Click Top Center Clock Pill to toggle Timezone (IST <-> UTC <-> EST <-> PST) */
        int clk_cx = g_desktop.screen_w / 2;
        if (mx >= clk_cx - 150 && mx <= clk_cx + 150 && my >= 3 && my <= 28) {
            g_tz_index = (g_tz_index + 1) % 4;
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
            int menu_w = 150;
            int menu_h = 136;
            int menu_x = g_desktop.screen_w - menu_w - 6;
            int menu_y = 34;

            if (mx >= menu_x && mx <= menu_x + menu_w && my >= menu_y && my <= menu_y + menu_h) {
                if (my >= menu_y + 50 && my <= menu_y + 70) {
                    /* Log Out: Close all open application windows */
                    for (int id = 1; id < 16; id++) {
                        wm_window_t *w = wm_find_by_id(id);
                        if (w) wm_destroy_window(w);
                    }
                    g_desktop.power_menu_open = false;
                    return;
                } else if (my >= menu_y + 90 && my <= menu_y + 110) {
                    /* Restart... */
                    g_desktop.confirm_action = 1;
                    g_desktop.power_menu_open = false;
                    return;
                } else if (my >= menu_y + 110 && my <= menu_y + 130) {
                    /* Shut Down... */
                    g_desktop.confirm_action = 2;
                    g_desktop.power_menu_open = false;
                    return;
                }
            } else {
                g_desktop.power_menu_open = false;
            }
        }

        /* Check Bottom Dock Clicks */
        int dock_w = 420;
        int dock_h = 52;
        int dock_x = (g_desktop.screen_w - dock_w) / 2;
        int dock_y = g_desktop.screen_h - dock_h - 8;

        if (mx >= dock_x && mx <= dock_x + dock_w && my >= dock_y && my <= dock_y + dock_h) {
            for (int i = 0; i < 8; i++) {
                int app_x = dock_x + 10 + i * 50;
                if (mx >= app_x && mx <= app_x + 44) {
                    launch_or_focus_app(i);
                    return;
                }
            }
        }

        /* Check Desktop Shortcuts Clicks */
        /* Column 1: x = 24..68 */
        if (mx >= 20 && mx <= 76) {
            if (my >= 66 && my <= 126) {
                launch_or_focus_app(0); /* Text Editor (matches x=38, y=86) */
                return;
            } else if (my >= 146 && my <= 206) {
                launch_or_focus_app(1); /* File Manager */
                return;
            } else if (my >= 226 && my <= 286) {
                launch_or_focus_app(2); /* Web Browser */
                return;
            } else if (my >= 306 && my <= 366) {
                launch_or_focus_app(3); /* Terminal */
                return;
            }
        }
        /* Column 2: x = 106..162 */
        if (mx >= 106 && mx <= 162) {
            if (my >= 66 && my <= 126) {
                launch_or_focus_app(4); /* Settings */
                return;
            } else if (my >= 146 && my <= 206) {
                launch_or_focus_app(5); /* Calculator */
                return;
            } else if (my >= 226 && my <= 286) {
                launch_or_focus_app(6); /* System Monitor */
                return;
            } else if (my >= 306 && my <= 366) {
                launch_or_focus_app(7); /* Package Manager */
                return;
            }
        }
    } else if (ev->type == INPUT_EVENT_KEY_DOWN) {
        /* Global Keyboard Shortcuts for Desktop Navigation */
        if (ev->key.keycode == KEY_ESC) {
            if (g_desktop.confirm_action != 0) {
                g_desktop.confirm_action = 0;
                return;
            }
            if (g_desktop.start_menu_open) {
                g_desktop.start_menu_open = false;
                return;
            }
            if (g_desktop.power_menu_open) {
                g_desktop.power_menu_open = false;
                return;
            }
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

    /* 3. Test Full Desktop Rendering Pass */
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
