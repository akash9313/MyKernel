#include <kernel/framebuffer.h>
#include "font.h"

static struct fb_info g_fb = {0};
static u64 g_cursor_x = 0;
static u64 g_cursor_y = 0;
static u32 g_fg_color = FB_COLOR_WHITE;
static u32 g_bg_color = FB_COLOR_BLACK;

static inline u32 fb_convert_color(u32 rgb) {
    u8 r = (rgb >> 16) & 0xFF;
    u8 g = (rgb >> 8) & 0xFF;
    u8 b = rgb & 0xFF;

    u32 cr = (u32)(r >> (8 - g_fb.red_mask_size)) << g_fb.red_mask_shift;
    u32 cg = (u32)(g >> (8 - g_fb.green_mask_size)) << g_fb.green_mask_shift;
    u32 cb = (u32)(b >> (8 - g_fb.blue_mask_size)) << g_fb.blue_mask_shift;

    return cr | cg | cb;
}

int fb_init(struct limine_framebuffer *limine_fb) {
    if (!limine_fb || !limine_fb->address) {
        g_fb.valid = false;
        return -1;
    }

    if (limine_fb->bpp != 32) {
        /* Currently 32bpp is supported */
        g_fb.valid = false;
        return -2;
    }

    g_fb.buffer = (u32 *)limine_fb->address;
    g_fb.width = limine_fb->width;
    g_fb.height = limine_fb->height;
    g_fb.pitch_bytes = limine_fb->pitch;
    g_fb.pitch_pixels = limine_fb->pitch / 4;
    g_fb.bpp = limine_fb->bpp;

    g_fb.red_mask_size = limine_fb->red_mask_size ? limine_fb->red_mask_size : 8;
    g_fb.red_mask_shift = limine_fb->red_mask_shift ? limine_fb->red_mask_shift : 16;
    g_fb.green_mask_size = limine_fb->green_mask_size ? limine_fb->green_mask_size : 8;
    g_fb.green_mask_shift = limine_fb->green_mask_shift ? limine_fb->green_mask_shift : 8;
    g_fb.blue_mask_size = limine_fb->blue_mask_size ? limine_fb->blue_mask_size : 8;
    g_fb.blue_mask_shift = limine_fb->blue_mask_shift ? limine_fb->blue_mask_shift : 0;

    g_fb.valid = true;
    g_cursor_x = 0;
    g_cursor_y = 0;

    fb_clear(FB_COLOR_BLACK);
    return 0;
}

bool fb_is_available(void) {
    return g_fb.valid;
}

void fb_get_resolution(u64 *width, u64 *height) {
    if (width) *width = g_fb.width;
    if (height) *height = g_fb.height;
}

void fb_put_pixel(u64 x, u64 y, u32 color) {
    if (!g_fb.valid || x >= g_fb.width || y >= g_fb.height) {
        return;
    }
    g_fb.buffer[y * g_fb.pitch_pixels + x] = fb_convert_color(color);
}

void fb_clear(u32 color) {
    if (!g_fb.valid) {
        return;
    }

    u32 c = fb_convert_color(color);
    for (u64 y = 0; y < g_fb.height; y++) {
        u32 *row = &g_fb.buffer[y * g_fb.pitch_pixels];
        for (u64 x = 0; x < g_fb.width; x++) {
            row[x] = c;
        }
    }
    g_cursor_x = 0;
    g_cursor_y = 0;
}

static void fb_scroll(void) {
    if (!g_fb.valid) return;

    u64 scroll_lines = FONT_HEIGHT;
    u64 copy_lines = g_fb.height - scroll_lines;

    for (u64 y = 0; y < copy_lines; y++) {
        u32 *dest = &g_fb.buffer[y * g_fb.pitch_pixels];
        u32 *src = &g_fb.buffer[(y + scroll_lines) * g_fb.pitch_pixels];
        for (u64 x = 0; x < g_fb.width; x++) {
            dest[x] = src[x];
        }
    }

    u32 bg = fb_convert_color(g_bg_color);
    for (u64 y = copy_lines; y < g_fb.height; y++) {
        u32 *dest = &g_fb.buffer[y * g_fb.pitch_pixels];
        for (u64 x = 0; x < g_fb.width; x++) {
            dest[x] = bg;
        }
    }
}

void fb_draw_char(u64 x, u64 y, char c, u32 fg, u32 bg) {
    if (!g_fb.valid) return;

    u8 uc = (u8)c;
    const u8 *glyph = font_bitmap_8x16[uc];
    u32 cfg = fb_convert_color(fg);
    u32 cbg = fb_convert_color(bg);

    for (u64 row = 0; row < FONT_HEIGHT; row++) {
        if (y + row >= g_fb.height) break;
        u8 bits = glyph[row];
        u32 *pixel_ptr = &g_fb.buffer[(y + row) * g_fb.pitch_pixels + x];

        for (u64 col = 0; col < FONT_WIDTH; col++) {
            if (x + col >= g_fb.width) break;
            if (bits & (0x80 >> col)) {
                pixel_ptr[col] = cfg;
            } else {
                pixel_ptr[col] = cbg;
            }
        }
    }
}

void fb_putchar(char c) {
    if (!g_fb.valid) return;

    if (c == '\n') {
        g_cursor_x = 0;
        g_cursor_y += FONT_HEIGHT;
    } else if (c == '\r') {
        g_cursor_x = 0;
    } else if (c == '\t') {
        g_cursor_x = (g_cursor_x + 8 * FONT_WIDTH) & ~(8 * FONT_WIDTH - 1);
    } else if (c == '\b') {
        if (g_cursor_x >= FONT_WIDTH) {
            g_cursor_x -= FONT_WIDTH;
            fb_draw_char(g_cursor_x, g_cursor_y, ' ', g_fg_color, g_bg_color);
        }
    } else {
        fb_draw_char(g_cursor_x, g_cursor_y, c, g_fg_color, g_bg_color);
        g_cursor_x += FONT_WIDTH;
    }

    if (g_cursor_x + FONT_WIDTH > g_fb.width) {
        g_cursor_x = 0;
        g_cursor_y += FONT_HEIGHT;
    }

    while (g_cursor_y + FONT_HEIGHT > g_fb.height) {
        fb_scroll();
        g_cursor_y -= FONT_HEIGHT;
    }
}

void fb_puts(const char *s) {
    if (!s) return;
    while (*s) {
        fb_putchar(*s++);
    }
}
