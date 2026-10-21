#include <kernel/browser.h>
#include <kernel/css.h>
#include <kernel/gfx.h>
#include <kernel/mem.h>
#include <kernel/kprintf.h>

#define MARGIN_LEFT   14
#define MARGIN_TOP    12
#define LINE_H_NORMAL 18
#define LINE_H_H1     36
#define LINE_H_H2     26
#define LINE_H_H3     22

static size_t k_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

int browser_layout(const html_node_t *nodes, int node_count, int viewport_w, render_box_t *boxes, int max_boxes, const css_stylesheet_t *sheet, int *out_doc_h) {
    (void)sheet;
    if (!nodes || !boxes || max_boxes <= 0) {
        if (out_doc_h) *out_doc_h = 0;
        return 0;
    }

    kmemset(boxes, 0, sizeof(render_box_t) * (size_t)max_boxes);

    int cur_x = MARGIN_LEFT;
    int cur_y = MARGIN_TOP;
    int box_count = 0;
    int max_line_w = viewport_w - 28;
    if (max_line_w < 140) max_line_w = 140;

    for (int i = 0; i < node_count && box_count < max_boxes; i++) {
        const html_node_t *n = &nodes[i];

        /* Skip elements with display: none */
        if (n->computed_style.display == CSS_DISPLAY_NONE) {
            continue;
        }

        /* 1. Horizontal Rule <hr> */
        if (n->tag == HTML_TAG_HR) {
            if (cur_x > MARGIN_LEFT) {
                cur_x = MARGIN_LEFT;
                cur_y += LINE_H_NORMAL;
            }
            cur_y += 4;
            if (box_count < max_boxes) {
                render_box_t *b = &boxes[box_count++];
                b->x = MARGIN_LEFT;
                b->y = cur_y;
                b->w = max_line_w;
                b->h = 2;
                b->is_hr = true;
                b->color = n->computed_style.has_color ? n->computed_style.color : GFX_COLOR_PANEL_BORDER;
            }
            cur_y += 10;
            cur_x = MARGIN_LEFT;
            continue;
        }

        /* 2. Line Break <br> */
        if (n->tag == HTML_TAG_BR) {
            cur_x = MARGIN_LEFT;
            cur_y += LINE_H_NORMAL;
            continue;
        }

        /* Calculate Box Model Metrics */
        const css_style_t *st = &n->computed_style;
        int m_top = st->margin_top;
        int m_bottom = st->margin_bottom;
        int m_left = st->margin_left;
        int m_right = st->margin_right;

        int p_top = st->padding_top;
        int p_bottom = st->padding_bottom;
        int p_left = st->padding_left;
        int p_right = st->padding_right;

        int b_width = st->has_border ? ((st->border_width > 0) ? st->border_width : 1) : 0;

        /* Apply block top margin */
        if (n->is_block) {
            if (cur_x > MARGIN_LEFT) {
                cur_x = MARGIN_LEFT;
                cur_y += LINE_H_NORMAL;
            }
            cur_y += m_top;
        }

        int block_left = MARGIN_LEFT + m_left;
        int block_w = max_line_w - m_left - m_right;
        if (st->width > 0 && st->width < block_w) {
            block_w = st->width;
        }
        if (block_w < 60) block_w = 60;

        int block_start_y = cur_y;
        int block_box_idx = -1;

        /* If block container has background, border, or padding, reserve a background container box */
        if (n->is_block && (st->has_bg_color || st->has_border || p_top > 0 || p_bottom > 0 || p_left > 0 || p_right > 0)) {
            if (box_count < max_boxes) {
                block_box_idx = box_count++;
                render_box_t *bb = &boxes[block_box_idx];
                bb->x = block_left;
                bb->y = block_start_y;
                bb->w = block_w;
                bb->h = 0; /* will be adjusted after content layout */
                bb->is_block_box = true;
                bb->has_bg_color = st->has_bg_color;
                bb->bg_color = st->bg_color;
                bb->has_border = st->has_border;
                bb->border_w = b_width;
                bb->border_color = st->border_color;
            }
        }

        /* Content start position inside container */
        int content_left = block_left + p_left + b_width;
        int content_w = block_w - p_left - p_right - (2 * b_width);
        if (content_w < 40) content_w = 40;

        cur_x = content_left;
        cur_y += p_top + b_width;

        u8 scale = (st->font_scale > 0) ? st->font_scale : 1;
        int char_w = FONT_WIDTH * scale;
        int line_h = (scale == 2) ? LINE_H_H1 : LINE_H_NORMAL;

        u32 color = st->has_color ? st->color : GFX_COLOR_TEXT_PRIMARY;

        /* List item bullet indicator */
        if (n->tag == HTML_TAG_LI) {
            if (box_count < max_boxes) {
                render_box_t *bb = &boxes[box_count++];
                bb->x = content_left;
                bb->y = cur_y;
                bb->w = 12;
                bb->h = line_h;
                bb->text[0] = '*';
                bb->text[1] = '\0';
                bb->color = GFX_COLOR_ACCENT_BLUE;
                bb->is_bullet = true;
                bb->font_scale = scale;
            }
            content_left += 16;
            cur_x = content_left;
        }

        /* Word-wrapping layout for text */
        const char *text = n->text;
        size_t tlen = k_strlen(text);
        size_t ti = 0;

        int line_start_box = box_count;

        while (ti < tlen && box_count < max_boxes) {
            while (ti < tlen && text[ti] == ' ') ti++;
            if (ti >= tlen) break;

            size_t word_start = ti;
            while (ti < tlen && text[ti] != ' ') ti++;
            size_t word_len = ti - word_start;
            if (word_len == 0) break;

            int word_pixel_w = (int)word_len * char_w;

            /* Check if word overflows line */
            if (cur_x + word_pixel_w > content_left + content_w && cur_x > content_left) {
                /* Apply text alignment to current line */
                if (st->text_align != CSS_ALIGN_LEFT && line_start_box < box_count) {
                    int line_used_w = cur_x - content_left;
                    int shift = 0;
                    if (st->text_align == CSS_ALIGN_CENTER) {
                        shift = (content_w - line_used_w) / 2;
                    } else if (st->text_align == CSS_ALIGN_RIGHT) {
                        shift = content_w - line_used_w;
                    }
                    if (shift > 0) {
                        for (int k = line_start_box; k < box_count; k++) {
                            boxes[k].x += shift;
                        }
                    }
                }

                cur_x = content_left;
                cur_y += line_h;
                line_start_box = box_count;
            }

            /* Record word/token box */
            if (box_count < max_boxes) {
                render_box_t *b = &boxes[box_count++];
                b->x = cur_x;
                b->y = cur_y;
                b->w = word_pixel_w;
                b->h = line_h;
                b->color = color;
                b->font_scale = scale;
                b->bold = st->bold;
                b->italic = st->italic;
                b->text_align = st->text_align;

                size_t cpy_len = word_len;
                if (cpy_len >= sizeof(b->text)) cpy_len = sizeof(b->text) - 1;
                for (size_t k = 0; k < cpy_len; k++) {
                    b->text[k] = text[word_start + k];
                }
                b->text[cpy_len] = '\0';

                if (n->tag == HTML_TAG_A || n->href[0] != '\0') {
                    b->is_link = true;
                    size_t hi = 0;
                    while (n->href[hi] && hi < sizeof(b->href) - 1) {
                        b->href[hi] = n->href[hi];
                        hi++;
                    }
                    b->href[hi] = '\0';
                }

                cur_x += word_pixel_w + char_w;
            }
        }

        /* Apply text alignment to the final line of block */
        if (st->text_align != CSS_ALIGN_LEFT && line_start_box < box_count) {
            int line_used_w = cur_x - content_left;
            int shift = 0;
            if (st->text_align == CSS_ALIGN_CENTER) {
                shift = (content_w - line_used_w) / 2;
            } else if (st->text_align == CSS_ALIGN_RIGHT) {
                shift = content_w - line_used_w;
            }
            if (shift > 0) {
                for (int k = line_start_box; k < box_count; k++) {
                    boxes[k].x += shift;
                }
            }
        }

        /* Close block */
        if (n->is_block) {
            cur_y += line_h;
            cur_y += p_bottom + b_width;

            /* Set height of block background container */
            if (block_box_idx >= 0 && block_box_idx < box_count) {
                int total_h = cur_y - block_start_y;
                if (total_h < (line_h + p_top + p_bottom + (2 * b_width))) {
                    total_h = line_h + p_top + p_bottom + (2 * b_width);
                }
                boxes[block_box_idx].h = total_h;
            }

            cur_y += m_bottom;
            cur_x = MARGIN_LEFT;
        }
    }

    if (out_doc_h) {
        *out_doc_h = cur_y + LINE_H_NORMAL + 12;
    }

    return box_count;
}
