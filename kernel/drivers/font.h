/* 8x16 Bitmap Font Header - Sourced from Linux kernel lib/fonts/font_8x16.c (GPL-2.0) */
#ifndef ASTRA_DRIVERS_FONT_H
#define ASTRA_DRIVERS_FONT_H

#include <kernel/types.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

extern const u8 font_bitmap_8x16[256][16];

#endif /* ASTRA_DRIVERS_FONT_H */
