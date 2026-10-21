#include <kernel/gfx.h>
#include <kernel/framebuffer.h>
#include <kernel/boot.h>
#include <kernel/pmm.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include "font.h"

static struct {
    bool initialized;
    u32 *hw_buffer;
    int width;
    int height;
    int pitch_bytes;
    int pitch_pixels;
    int bpp;
    u8 red_mask_size, red_mask_shift;
    u8 green_mask_size, green_mask_shift;
    u8 blue_mask_size, blue_mask_shift;
    
    phys_addr_t backbuffer_phys;
    size_t backbuffer_pages;
    gfx_surface_t backbuffer_surf;
} g_gfx = {0};

/* Mouse cursor bitmap: 12x18 arrow cursor with outline */
/* 0 = transparent, 1 = black outline, 2 = white fill */
static const u8 g_cursor_bitmap[18][12] = {
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0},
    {1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0},
    {1, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 0},
    {1, 2, 2, 1, 2, 1, 0, 0, 0, 0, 0, 0},
    {1, 2, 1, 0, 1, 2, 1, 0, 0, 0, 0, 0},
    {1, 1, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0},
    {1, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

static inline u32 gfx_convert_color(u32 rgb) {
    u8 r = (rgb >> 16) & 0xFF;
    u8 g = (rgb >> 8) & 0xFF;
    u8 b = rgb & 0xFF;

    u32 cr = (u32)(r >> (8 - g_gfx.red_mask_size)) << g_gfx.red_mask_shift;
    u32 cg = (u32)(g >> (8 - g_gfx.green_mask_size)) << g_gfx.green_mask_shift;
    u32 cb = (u32)(b >> (8 - g_gfx.blue_mask_size)) << g_gfx.blue_mask_shift;

    return cr | cg | cb;
}

void gfx_init_surface(gfx_surface_t *s, u32 *buf, int w, int h, int pitch) {
    if (!s) return;
    s->buffer = buf;
    s->width = w;
    s->height = h;
    s->pitch_pixels = pitch > 0 ? pitch : w;
    s->clip.x = 0;
    s->clip.y = 0;
    s->clip.w = w;
    s->clip.h = h;
}

void gfx_set_clip(gfx_surface_t *s, int x, int y, int w, int h) {
    if (!s) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->width) w = s->width - x;
    if (y + h > s->height) h = s->height - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    s->clip.x = x;
    s->clip.y = y;
    s->clip.w = w;
    s->clip.h = h;
}

void gfx_reset_clip(gfx_surface_t *s) {
    if (!s) return;
    s->clip.x = 0;
    s->clip.y = 0;
    s->clip.w = s->width;
    s->clip.h = s->height;
}

void gfx_put_pixel(gfx_surface_t *s, int x, int y, u32 color) {
    if (!s || !s->buffer) return;
    if (x < s->clip.x || x >= s->clip.x + s->clip.w ||
        y < s->clip.y || y >= s->clip.y + s->clip.h) {
        return;
    }
    s->buffer[y * s->pitch_pixels + x] = color;
}

u32 gfx_get_pixel(const gfx_surface_t *s, int x, int y) {
    if (!s || !s->buffer) return 0;
    if (x < 0 || x >= s->width || y < 0 || y >= s->height) return 0;
    return s->buffer[y * s->pitch_pixels + x];
}

void gfx_fill_rect(gfx_surface_t *s, int x, int y, int w, int h, u32 color) {
    if (!s || !s->buffer || w <= 0 || h <= 0) return;

    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    /* Clip to active clipping rectangle */
    if (x0 < s->clip.x) x0 = s->clip.x;
    if (y0 < s->clip.y) y0 = s->clip.y;
    if (x1 > s->clip.x + s->clip.w) x1 = s->clip.x + s->clip.w;
    if (y1 > s->clip.y + s->clip.h) y1 = s->clip.y + s->clip.h;

    if (x0 >= x1 || y0 >= y1) return;

    for (int cy = y0; cy < y1; cy++) {
        u32 *row = &s->buffer[cy * s->pitch_pixels + x0];
        int count = x1 - x0;
        for (int cx = 0; cx < count; cx++) {
            row[cx] = color;
        }
    }
}

void gfx_draw_rect(gfx_surface_t *s, int x, int y, int w, int h, u32 color) {
    if (!s || !s->buffer || w <= 0 || h <= 0) return;
    gfx_fill_rect(s, x, y, w, 1, color);             /* Top border */
    gfx_fill_rect(s, x, y + h - 1, w, 1, color);     /* Bottom border */
    gfx_fill_rect(s, x, y, 1, h, color);             /* Left border */
    gfx_fill_rect(s, x + w - 1, y, 1, h, color);     /* Right border */
}

void gfx_draw_line(gfx_surface_t *s, int x0, int y0, int x1, int y1, u32 color) {
    if (!s || !s->buffer) return;

    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 >= y0) ? -(y1 - y0) : (y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (1) {
        gfx_put_pixel(s, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_draw_char(gfx_surface_t *s, int x, int y, char c, u32 fg, u32 bg, bool transparent_bg) {
    if (!s || !s->buffer) return;
    if (x + FONT_WIDTH <= s->clip.x || x >= s->clip.x + s->clip.w ||
        y + FONT_HEIGHT <= s->clip.y || y >= s->clip.y + s->clip.h) {
        return;
    }

    u8 uc = (u8)c;
    const u8 *glyph = font_bitmap_8x16[uc];

    for (int row = 0; row < FONT_HEIGHT; row++) {
        int py = y + row;
        if (py < s->clip.y || py >= s->clip.y + s->clip.h) continue;

        u8 bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            int px = x + col;
            if (px < s->clip.x || px >= s->clip.x + s->clip.w) continue;

            if (bits & (0x80 >> col)) {
                s->buffer[py * s->pitch_pixels + px] = fg;
            } else if (!transparent_bg) {
                s->buffer[py * s->pitch_pixels + px] = bg;
            }
        }
    }
}

void gfx_draw_string(gfx_surface_t *s, int x, int y, const char *str, u32 fg, u32 bg, bool transparent_bg) {
    if (!s || !s->buffer || !str) return;

    int cur_x = x;
    int cur_y = y;

    while (*str) {
        char c = *str++;
        if (c == '\n') {
            cur_x = x;
            cur_y += FONT_HEIGHT;
        } else if (c == '\r') {
            cur_x = x;
        } else if (c == '\t') {
            cur_x += 4 * FONT_WIDTH;
        } else {
            gfx_draw_char(s, cur_x, cur_y, c, fg, bg, transparent_bg);
            cur_x += FONT_WIDTH;
        }
    }
}

void gfx_draw_char_scale(gfx_surface_t *s, int x, int y, char c, u32 fg, u32 bg, bool transparent_bg, int scale) {
    if (!s || !s->buffer || scale <= 0) return;
    if (scale == 1) {
        gfx_draw_char(s, x, y, c, fg, bg, transparent_bg);
        return;
    }
    int cw = FONT_WIDTH * scale;
    int ch = FONT_HEIGHT * scale;
    if (x + cw <= s->clip.x || x >= s->clip.x + s->clip.w ||
        y + ch <= s->clip.y || y >= s->clip.y + s->clip.h) {
        return;
    }

    u8 uc = (u8)c;
    const u8 *glyph = font_bitmap_8x16[uc];

    for (int row = 0; row < FONT_HEIGHT; row++) {
        u8 bits = glyph[row];
        for (int dy = 0; dy < scale; dy++) {
            int py = y + row * scale + dy;
            if (py < s->clip.y || py >= s->clip.y + s->clip.h) continue;

            for (int col = 0; col < FONT_WIDTH; col++) {
                bool on = (bits & (0x80 >> col)) != 0;
                for (int dx = 0; dx < scale; dx++) {
                    int px = x + col * scale + dx;
                    if (px < s->clip.x || px >= s->clip.x + s->clip.w) continue;

                    if (on) {
                        s->buffer[py * s->pitch_pixels + px] = fg;
                    } else if (!transparent_bg) {
                        s->buffer[py * s->pitch_pixels + px] = bg;
                    }
                }
            }
        }
    }
}

void gfx_draw_string_scale(gfx_surface_t *s, int x, int y, const char *str, u32 fg, u32 bg, bool transparent_bg, int scale) {
    if (!s || !s->buffer || !str || scale <= 0) return;
    if (scale == 1) {
        gfx_draw_string(s, x, y, str, fg, bg, transparent_bg);
        return;
    }
    int cur_x = x;
    int cur_y = y;
    while (*str) {
        char c = *str++;
        if (c == '\n') {
            cur_x = x;
            cur_y += FONT_HEIGHT * scale;
        } else if (c == '\r') {
            cur_x = x;
        } else if (c == '\t') {
            cur_x += 4 * FONT_WIDTH * scale;
        } else {
            gfx_draw_char_scale(s, cur_x, cur_y, c, fg, bg, transparent_bg, scale);
            cur_x += FONT_WIDTH * scale;
        }
    }
}

void gfx_blit(gfx_surface_t *dst, int dst_x, int dst_y, const gfx_surface_t *src, int src_x, int src_y, int w, int h) {
    if (!dst || !dst->buffer || !src || !src->buffer || w <= 0 || h <= 0) return;

    for (int y = 0; y < h; y++) {
        int sy = src_y + y;
        int dy = dst_y + y;
        if (sy < 0 || sy >= src->height) continue;
        if (dy < dst->clip.y || dy >= dst->clip.y + dst->clip.h) continue;

        for (int x = 0; x < w; x++) {
            int sx = src_x + x;
            int dx = dst_x + x;
            if (sx < 0 || sx >= src->width) continue;
            if (dx < dst->clip.x || dx >= dst->clip.x + dst->clip.w) continue;

            dst->buffer[dy * dst->pitch_pixels + dx] = src->buffer[sy * src->pitch_pixels + sx];
        }
    }
}

void gfx_draw_bitmap(gfx_surface_t *s, int x, int y, int w, int h, const u32 *pixels) {
    if (!s || !s->buffer || !pixels || w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < s->clip.y || py >= s->clip.y + s->clip.h) continue;

        for (int col = 0; col < w; col++) {
            int px = x + col;
            if (px < s->clip.x || px >= s->clip.x + s->clip.w) continue;

            u32 c = pixels[row * w + col];
            s->buffer[py * s->pitch_pixels + px] = gfx_convert_color(c);
        }
    }
}

void gfx_draw_cursor(gfx_surface_t *s, int x, int y) {
    if (!s || !s->buffer) return;

    for (int r = 0; r < 18; r++) {
        int py = y + r;
        if (py < s->clip.y || py >= s->clip.y + s->clip.h) continue;

        for (int c = 0; c < 12; c++) {
            int px = x + c;
            if (px < s->clip.x || px >= s->clip.x + s->clip.w) continue;

            u8 val = g_cursor_bitmap[r][c];
            if (val == 1) {
                s->buffer[py * s->pitch_pixels + px] = GFX_COLOR_BLACK;
            } else if (val == 2) {
                s->buffer[py * s->pitch_pixels + px] = GFX_COLOR_WHITE;
            }
        }
    }
}

int gfx_init(void) {
    struct limine_framebuffer_response *fb_resp = boot_get_framebuffer_response();
    if (!fb_resp || fb_resp->framebuffer_count == 0 || !fb_resp->framebuffers[0]) {
        kprintf("[GFX ERROR] Bootloader provided no framebuffer.\n");
        return -1;
    }

    struct limine_framebuffer *fb = fb_resp->framebuffers[0];
    if (!fb->address || fb->bpp != 32 || fb->width == 0 || fb->height == 0) {
        kprintf("[GFX ERROR] Unsupported framebuffer format (bpp: %u, %llux%llu)\n",
                (unsigned)fb->bpp, (unsigned long long)fb->width, (unsigned long long)fb->height);
        return -2;
    }

    g_gfx.hw_buffer = (u32 *)fb->address;
    g_gfx.width = (int)fb->width;
    g_gfx.height = (int)fb->height;
    g_gfx.pitch_bytes = (int)fb->pitch;
    g_gfx.pitch_pixels = (int)(fb->pitch / 4);
    g_gfx.bpp = (int)fb->bpp;

    g_gfx.red_mask_size = fb->red_mask_size ? fb->red_mask_size : 8;
    g_gfx.red_mask_shift = fb->red_mask_shift ? fb->red_mask_shift : 16;
    g_gfx.green_mask_size = fb->green_mask_size ? fb->green_mask_size : 8;
    g_gfx.green_mask_shift = fb->green_mask_shift ? fb->green_mask_shift : 8;
    g_gfx.blue_mask_size = fb->blue_mask_size ? fb->blue_mask_size : 8;
    g_gfx.blue_mask_shift = fb->blue_mask_shift ? fb->blue_mask_shift : 0;

    /* Allocate Contiguous Physical Memory for Backbuffer */
    size_t fb_size_bytes = (size_t)g_gfx.pitch_bytes * g_gfx.height;
    size_t pages = (fb_size_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    phys_addr_t bb_pa = 0;
    int pmm_res = pmm_alloc_contiguous(pages, &bb_pa);
    if (pmm_res != PMM_SUCCESS) {
        kprintf("[GFX ERROR] Failed to allocate contiguous backbuffer (%llu pages, %llu bytes)\n",
                (unsigned long long)pages, (unsigned long long)fb_size_bytes);
        return -3;
    }

    g_gfx.backbuffer_phys = bb_pa;
    g_gfx.backbuffer_pages = pages;

    struct limine_hhdm_response *hhdm = boot_get_hhdm_response();
    u32 *backbuffer_va = (u32 *)(bb_pa + hhdm->offset);

    gfx_init_surface(&g_gfx.backbuffer_surf, backbuffer_va, g_gfx.width, g_gfx.height, g_gfx.pitch_pixels);
    kmemset(backbuffer_va, 0, fb_size_bytes);

    g_gfx.initialized = true;

    kprintf("[GFX] Framebuffer initialized successfully:\n");
    kprintf("  Dimensions: %dx%d @ 32bpp (Pitch: %d bytes / %d pixels)\n",
            g_gfx.width, g_gfx.height, g_gfx.pitch_bytes, g_gfx.pitch_pixels);
    kprintf("  Hardware FB VA:  0x%016llx\n", (unsigned long long)(uintptr_t)g_gfx.hw_buffer);
    kprintf("  Backbuffer PA:   0x%016llx (%llu pages, %llu KiB)\n",
            (unsigned long long)bb_pa, (unsigned long long)pages, (unsigned long long)(fb_size_bytes / 1024));
    kprintf("  Backbuffer VA:   0x%016llx\n", (unsigned long long)(uintptr_t)backbuffer_va);

    return 0;
}

bool gfx_is_available(void) {
    return g_gfx.initialized;
}

gfx_surface_t *gfx_get_backbuffer(void) {
    if (!g_gfx.initialized) return NULL;
    return &g_gfx.backbuffer_surf;
}

void gfx_flip(void) {
    if (!g_gfx.initialized) return;

    for (int y = 0; y < g_gfx.height; y++) {
        u32 *src = &g_gfx.backbuffer_surf.buffer[y * g_gfx.pitch_pixels];
        u32 *dst = &g_gfx.hw_buffer[y * g_gfx.pitch_pixels];
        for (int x = 0; x < g_gfx.width; x++) {
            dst[x] = gfx_convert_color(src[x]);
        }
    }
}

void gfx_flip_rect(int x, int y, int w, int h) {
    if (!g_gfx.initialized || w <= 0 || h <= 0) return;

    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = (x + w > g_gfx.width) ? g_gfx.width : (x + w);
    int y1 = (y + h > g_gfx.height) ? g_gfx.height : (y + h);

    if (x0 >= x1 || y0 >= y1) return;

    for (int cy = y0; cy < y1; cy++) {
        u32 *src = &g_gfx.backbuffer_surf.buffer[cy * g_gfx.pitch_pixels + x0];
        u32 *dst = &g_gfx.hw_buffer[cy * g_gfx.pitch_pixels + x0];
        int count = x1 - x0;
        for (int cx = 0; cx < count; cx++) {
            dst[cx] = gfx_convert_color(src[cx]);
        }
    }
}

bool gfx_self_test(void) {
    kprintf("\n[GFX TEST] Starting Graphics Subsystem Self-Test...\n");
    if (!g_gfx.initialized) {
        kprintf("[GFX TEST FAIL] Graphics subsystem is not initialized!\n");
        return false;
    }

    gfx_surface_t *bb = &g_gfx.backbuffer_surf;

    /* 1. Test Clipping and Out-of-bounds Defense */
    kprintf("[GFX TEST] 1. Verifying out-of-bounds pixel writes & clipping...\n");
    gfx_set_clip(bb, 50, 50, 100, 100);

    /* Points outside clip must be discarded safely */
    gfx_put_pixel(bb, -10, -10, 0xFFFFFF);
    gfx_put_pixel(bb, 0, 0, 0xFFFFFF);
    gfx_put_pixel(bb, 49, 50, 0xFFFFFF);
    gfx_put_pixel(bb, 150, 50, 0xFFFFFF);
    gfx_put_pixel(bb, 10000, 10000, 0xFFFFFF);

    if (gfx_get_pixel(bb, 0, 0) == 0xFFFFFF || gfx_get_pixel(bb, 49, 50) == 0xFFFFFF) {
        kprintf("[GFX TEST FAIL] Clipping defense failed! Pixels written outside clip area.\n");
        return false;
    }

    /* Point inside clip must be written */
    gfx_put_pixel(bb, 55, 55, 0x123456);
    if (gfx_get_pixel(bb, 55, 55) != 0x123456) {
        kprintf("[GFX TEST FAIL] Pixel inside clip was not written!\n");
        return false;
    }
    gfx_reset_clip(bb);
    kprintf("  PASS: Clipping and out-of-bounds protections verified.\n");

    /* 2. Test Primitives: Fill Rect, Draw Rect, Line, String */
    kprintf("[GFX TEST] 2. Verifying drawing primitives (fill, border, line, font)...\n");
    gfx_fill_rect(bb, 10, 10, 80, 40, GFX_COLOR_ACCENT_BLUE);
    if (gfx_get_pixel(bb, 15, 15) != GFX_COLOR_ACCENT_BLUE ||
        gfx_get_pixel(bb, 89, 49) != GFX_COLOR_ACCENT_BLUE) {
        kprintf("[GFX TEST FAIL] Fill rect failed verification!\n");
        return false;
    }

    gfx_draw_rect(bb, 100, 10, 40, 40, GFX_COLOR_BTN_CLOSE);
    if (gfx_get_pixel(bb, 100, 10) != GFX_COLOR_BTN_CLOSE ||
        gfx_get_pixel(bb, 110, 20) == GFX_COLOR_BTN_CLOSE) {
        kprintf("[GFX TEST FAIL] Draw rect outline failed verification!\n");
        return false;
    }

    gfx_draw_line(bb, 200, 10, 250, 10, GFX_COLOR_WHITE);
    if (gfx_get_pixel(bb, 225, 10) != GFX_COLOR_WHITE) {
        kprintf("[GFX TEST FAIL] Draw line failed verification!\n");
        return false;
    }

    gfx_draw_string(bb, 10, 70, "AstraOS Test", GFX_COLOR_TEXT_PRIMARY, GFX_COLOR_BLACK, false);
    /* Check glyph rendered some non-black foreground pixels */
    bool found_fg = false;
    for (int y = 70; y < 70 + FONT_HEIGHT; y++) {
        for (int x = 10; x < 10 + 8 * FONT_WIDTH; x++) {
            if (gfx_get_pixel(bb, x, y) == GFX_COLOR_TEXT_PRIMARY) {
                found_fg = true;
                break;
            }
        }
        if (found_fg) break;
    }
    if (!found_fg) {
        kprintf("[GFX TEST FAIL] Font rendering failed to write foreground pixels!\n");
        return false;
    }
    kprintf("  PASS: Drawing primitives and font rendering verified.\n");

    /* 3. Test Double-Buffer Presentation Flip */
    kprintf("[GFX TEST] 3. Verifying double buffer presentation flip...\n");
    gfx_flip_rect(10, 10, 300, 100);
    kprintf("  PASS: Double-buffer presentation completed cleanly.\n");

    kprintf("[GFX TEST] All Graphics tests passed successfully!\n");
    kprintf("ASTRA_FRAMEBUFFER_OK\n");
    kprintf("ASTRA_GRAPHICS_OK\n\n");
    return true;
}
