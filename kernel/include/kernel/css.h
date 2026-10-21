#ifndef ASTRA_KERNEL_CSS_H
#define ASTRA_KERNEL_CSS_H

#include <kernel/types.h>

#define CSS_MAX_RULES           64
#define CSS_MAX_SELECTOR_LEN    32
#define CSS_MAX_CLASS_LEN       32
#define CSS_MAX_ID_LEN          32
#define CSS_MAX_INLINE_STYLE    256

typedef enum {
    CSS_SELECTOR_TAG = 0,
    CSS_SELECTOR_CLASS,
    CSS_SELECTOR_ID,
    CSS_SELECTOR_UNIVERSAL
} css_selector_type_t;

typedef enum {
    CSS_DISPLAY_INLINE = 0,
    CSS_DISPLAY_BLOCK,
    CSS_DISPLAY_NONE
} css_display_t;

typedef enum {
    CSS_ALIGN_LEFT = 0,
    CSS_ALIGN_CENTER,
    CSS_ALIGN_RIGHT
} css_text_align_t;

/* Computed / Specified CSS Style */
typedef struct {
    u32 color;
    bool has_color;
    u32 bg_color;
    bool has_bg_color;

    u8 font_scale;          /* 1 = normal (8x16), 2 = large (16x32) */
    bool bold;
    bool italic;
    css_text_align_t text_align;
    css_display_t display;

    int margin_top;
    int margin_bottom;
    int margin_left;
    int margin_right;

    int padding_top;
    int padding_bottom;
    int padding_left;
    int padding_right;

    int border_width;
    u32 border_color;
    bool has_border;

    int width;              /* 0 = auto / 100% */
} css_style_t;

typedef struct {
    css_selector_type_t type;
    char name[CSS_MAX_SELECTOR_LEN];
    int specificity;
    css_style_t style;
} css_rule_t;

typedef struct {
    css_rule_t rules[CSS_MAX_RULES];
    int rule_count;
} css_stylesheet_t;

/* CSS Engine API */
void css_init_style(css_style_t *style);
void css_parse_stylesheet(const char *css, size_t len, css_stylesheet_t *sheet);
void css_parse_inline_style(const char *style_str, css_style_t *out_style);
void css_apply_style(css_style_t *dst, const css_style_t *src);
u32 css_parse_color(const char *val);
bool css_self_test(void);

#endif /* ASTRA_KERNEL_CSS_H */
