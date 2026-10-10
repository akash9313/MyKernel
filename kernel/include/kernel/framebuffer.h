#ifndef ASTRA_KERNEL_FRAMEBUFFER_H
#define ASTRA_KERNEL_FRAMEBUFFER_H

#include <kernel/types.h>
#include <limine.h>

#define FB_COLOR_BLACK       0x00000000
#define FB_COLOR_BLUE        0x000000AA
#define FB_COLOR_GREEN       0x0000AA00
#define FB_COLOR_CYAN        0x0000AAAA
#define FB_COLOR_RED         0x00AA0000
#define FB_COLOR_MAGENTA     0x00AA00AA
#define FB_COLOR_BROWN       0x00AA5500
#define FB_COLOR_LIGHT_GREY  0x00AAAAAA
#define FB_COLOR_DARK_GREY   0x00555555
#define FB_COLOR_BRIGHT_BLUE 0x005555FF
#define FB_COLOR_BRIGHT_GREEN 0x0055FF55
#define FB_COLOR_BRIGHT_CYAN 0x0055FFFF
#define FB_COLOR_BRIGHT_RED  0x00FF5555
#define FB_COLOR_BRIGHT_MAGENTA 0x00FF55FF
#define FB_COLOR_YELLOW      0x00FFFF55
#define FB_COLOR_WHITE       0x00FFFFFF

struct fb_info {
    u32 *buffer;
    u64 width;
    u64 height;
    u64 pitch_bytes;
    u64 pitch_pixels;
    u16 bpp;
    u8 red_mask_size;
    u8 red_mask_shift;
    u8 green_mask_size;
    u8 green_mask_shift;
    u8 blue_mask_size;
    u8 blue_mask_shift;
    bool valid;
};

int fb_init(struct limine_framebuffer *limine_fb);
bool fb_is_available(void);
void fb_clear(u32 color);
void fb_put_pixel(u64 x, u64 y, u32 color);
void fb_draw_char(u64 x, u64 y, char c, u32 fg, u32 bg);
void fb_putchar(char c);
void fb_puts(const char *s);
void fb_get_resolution(u64 *width, u64 *height);

#endif /* ASTRA_KERNEL_FRAMEBUFFER_H */
