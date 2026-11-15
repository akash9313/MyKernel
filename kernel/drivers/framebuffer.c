#include <kernel/framebuffer.h>
#include <kernel/boot.h>
#include <kernel/initramfs.h>
#include <kernel/mem.h>
#include "font.h"

static struct fb_info g_fb = {0};
static u64 g_cursor_x = 0;
static u64 g_cursor_y = 0;
static u32 g_fg_color = FB_COLOR_WHITE;
static u32 g_bg_color = FB_COLOR_BLACK;

static const void *g_cached_wallpaper_pixels = NULL;
static u32 g_cached_wallpaper_w = 0;
static u32 g_cached_wallpaper_h = 0;
static bool g_fb_console_enabled = true;

bool fb_is_console_enabled(void) {
    return g_fb_console_enabled;
}

void fb_set_console_enabled(bool enabled) {
    g_fb_console_enabled = enabled;
}

const void *fb_get_wallpaper_payload(u32 *out_w, u32 *out_h) {
    if (out_w) *out_w = g_cached_wallpaper_w;
    if (out_h) *out_h = g_cached_wallpaper_h;
    return g_cached_wallpaper_pixels;
}

static int fb_strcmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return -1;
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static int fb_strncmp(const char *s1, const char *s2, size_t n) {
    if (!s1 || !s2) return -1;
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0') {
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        }
    }
    return 0;
}

static inline u32 fb_convert_color(u32 rgb) {
    u8 r = (rgb >> 16) & 0xFF;
    u8 g = (rgb >> 8) & 0xFF;
    u8 b = rgb & 0xFF;

    u32 cr = (u32)(r >> (8 - g_fb.red_mask_size)) << g_fb.red_mask_shift;
    u32 cg = (u32)(g >> (8 - g_fb.green_mask_size)) << g_fb.green_mask_shift;
    u32 cb = (u32)(b >> (8 - g_fb.blue_mask_size)) << g_fb.blue_mask_shift;

    return cr | cg | cb;
}

bool fb_render_wallpaper_early(const void *initramfs_data, size_t initramfs_size) {
    if (!g_fb.valid || !initramfs_data || initramfs_size < sizeof(astra_initramfs_header_t)) {
        return false;
    }

    const astra_initramfs_header_t *hdr = (const astra_initramfs_header_t *)initramfs_data;
    if (fb_strncmp(hdr->magic, "ASTRAFS1", 8) != 0) {
        return false;
    }

    const astra_initramfs_entry_t *entries = (const astra_initramfs_entry_t *)((const u8 *)initramfs_data + hdr->entries_offset);
    const astra_initramfs_entry_t *target = NULL;

    for (u32 i = 0; i < hdr->entry_count; i++) {
        if (fb_strcmp(entries[i].name, "/assets/cosmic_vista.raw") == 0) {
            target = &entries[i];
            break;
        }
    }

    if (!target || target->size < 20) {
        return false;
    }

    const u8 *payload = ((const u8 *)initramfs_data) + target->data_offset;
    if (payload[0] != 'A' || payload[1] != 'W' || payload[2] != 'P' || payload[3] != '1') {
        return false;
    }

    u32 src_w = *(const u32 *)(payload + 4);
    u32 src_h = *(const u32 *)(payload + 8);
    const u32 *src_pixels = (const u32 *)(payload + 20);

    if (src_w == 0 || src_h == 0) {
        return false;
    }

    g_cached_wallpaper_pixels = src_pixels;
    g_cached_wallpaper_w = src_w;
    g_cached_wallpaper_h = src_h;

    u64 fb_w = g_fb.width;
    u64 fb_h = g_fb.height;

    /* Aspect-ratio preserving centered crop */
    u64 crop_w, crop_h;
    if (fb_w * src_h <= fb_h * src_w) {
        crop_w = (fb_w * src_h) / fb_h;
        crop_h = src_h;
    } else {
        crop_w = src_w;
        crop_h = (fb_h * src_w) / fb_w;
    }

    u64 start_x = (src_w > crop_w) ? (src_w - crop_w) / 2 : 0;
    u64 start_y = (src_h > crop_h) ? (src_h - crop_h) / 2 : 0;

    for (u64 y = 0; y < fb_h; y++) {
        u64 sy = start_y + (y * crop_h) / fb_h;
        if (sy >= src_h) sy = src_h - 1;
        const u32 *src_row = &src_pixels[sy * src_w];
        u32 *dst_row = &g_fb.buffer[y * g_fb.pitch_pixels];

        if (crop_w == fb_w && start_x == 0 &&
            g_fb.red_mask_shift == 16 && g_fb.green_mask_shift == 8 && g_fb.blue_mask_shift == 0) {
            kmemcpy(dst_row, src_row, fb_w * sizeof(u32));
        } else {
            for (u64 x = 0; x < fb_w; x++) {
                u64 sx = start_x + (x * crop_w) / fb_w;
                if (sx >= src_w) sx = src_w - 1;
                dst_row[x] = fb_convert_color(src_row[sx]);
            }
        }
    }

    /* Suppress early console font glyph rectangles onto the wallpaper */
    g_fb_console_enabled = false;
    return true;
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
    g_fb_console_enabled = true;

    /* Render wallpaper immediately if initramfs module is present */
    struct limine_module_response *mod_resp = boot_get_module_response();
    bool wp_ok = false;
    if (mod_resp && mod_resp->module_count > 0 && mod_resp->modules && mod_resp->modules[0]) {
        wp_ok = fb_render_wallpaper_early(mod_resp->modules[0]->address, (size_t)mod_resp->modules[0]->size);
    }
    if (!wp_ok) {
        fb_clear(FB_COLOR_BLACK);
    }
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
    if (!g_fb.valid || !g_fb_console_enabled) return;

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
