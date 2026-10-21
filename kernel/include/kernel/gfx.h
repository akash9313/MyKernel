#ifndef ASTRA_KERNEL_GFX_H
#define ASTRA_KERNEL_GFX_H

#include <kernel/types.h>
#include <limine.h>

/* Modern Dark Futuristic Color Palette (32-bit XRGB) */
#define GFX_COLOR_DESKTOP_BG       0x080D1A /* Deep dark space navy */
#define GFX_COLOR_DESKTOP_GRID     0x0E172E /* Grid lines on desktop */
#define GFX_COLOR_PANEL_BG         0x101728 /* Taskbar / Dock background */
#define GFX_COLOR_PANEL_BORDER     0x1E293B /* Panel outline border */
#define GFX_COLOR_ACCENT_BLUE      0x38BDF8 /* Vibrant electric cyan-blue */
#define GFX_COLOR_ACCENT_PURPLE    0x818CF8 /* Futuristic indigo-purple */
#define GFX_COLOR_ACCENT_CYAN      0x2DD4BF /* Neon teal accent */
#define GFX_COLOR_WINDOW_BG        0x0B1120 /* Dark slate window background */
#define GFX_COLOR_WINDOW_BORDER    0x334155 /* Window border frame */
#define GFX_COLOR_TITLEBAR_ACTIVE  0x1E293B /* Focused window titlebar */
#define GFX_COLOR_TITLEBAR_INACTIVE 0x0F172A /* Unfocused titlebar */
#define GFX_COLOR_TEXT_PRIMARY     0xF8FAFC /* Crisp white text */
#define GFX_COLOR_TEXT_MUTED       0x94A3B8 /* Muted silver text */
#define GFX_COLOR_TEXT_ACCENT      0x38BDF8 /* Highlighted text */
#define GFX_COLOR_BTN_CLOSE        0xEF4444 /* Red close button */
#define GFX_COLOR_BTN_CLOSE_HOV    0xDC2626 /* Darker red on hover */
#define GFX_COLOR_BTN_MIN          0xEAB308 /* Yellow minimize button */
#define GFX_COLOR_BTN_MAX          0x22C55E /* Green maximize button */
#define GFX_COLOR_WHITE            0xFFFFFF
#define GFX_COLOR_BLACK            0x000000

#define FONT_WIDTH                 8
#define FONT_HEIGHT                16

typedef struct gfx_rect {
    int x;
    int y;
    int w;
    int h;
} gfx_rect_t;

typedef struct gfx_surface {
    u32 *buffer;
    int width;
    int height;
    int pitch_pixels;
    gfx_rect_t clip;
} gfx_surface_t;

/* Surface and Clipping Management */
void gfx_init_surface(gfx_surface_t *s, u32 *buf, int w, int h, int pitch);
void gfx_set_clip(gfx_surface_t *s, int x, int y, int w, int h);
void gfx_reset_clip(gfx_surface_t *s);

/* Safe Drawing Primitives (Bounds and Clip Protected) */
void gfx_put_pixel(gfx_surface_t *s, int x, int y, u32 color);
u32 gfx_get_pixel(const gfx_surface_t *s, int x, int y);
void gfx_fill_rect(gfx_surface_t *s, int x, int y, int w, int h, u32 color);
void gfx_draw_rect(gfx_surface_t *s, int x, int y, int w, int h, u32 color);
void gfx_draw_line(gfx_surface_t *s, int x0, int y0, int x1, int y1, u32 color);
void gfx_draw_char(gfx_surface_t *s, int x, int y, char c, u32 fg, u32 bg, bool transparent_bg);
void gfx_draw_string(gfx_surface_t *s, int x, int y, const char *str, u32 fg, u32 bg, bool transparent_bg);
void gfx_draw_char_scale(gfx_surface_t *s, int x, int y, char c, u32 fg, u32 bg, bool transparent_bg, int scale);
void gfx_draw_string_scale(gfx_surface_t *s, int x, int y, const char *str, u32 fg, u32 bg, bool transparent_bg, int scale);
void gfx_blit(gfx_surface_t *dst, int dst_x, int dst_y, const gfx_surface_t *src, int src_x, int src_y, int w, int h);
void gfx_draw_bitmap(gfx_surface_t *s, int x, int y, int w, int h, const u32 *pixels);
void gfx_draw_cursor(gfx_surface_t *s, int x, int y);

/* Core Graphics Subsystem & Double Buffering */
int gfx_init(void);
bool gfx_is_available(void);
gfx_surface_t *gfx_get_backbuffer(void);
void gfx_flip(void);
void gfx_flip_rect(int x, int y, int w, int h);
bool gfx_self_test(void);

#endif /* ASTRA_KERNEL_GFX_H */
