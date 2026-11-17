#ifndef ASTRA_KERNEL_SCROLLVIEW_H
#define ASTRA_KERNEL_SCROLLVIEW_H

#include <kernel/types.h>
#include <kernel/gfx.h>
#include <kernel/input.h>

typedef struct scrollview {
    int total_items;
    int visible_items;
    int scroll_offset;     /* 0 .. max(0, total_items - visible_items) */

    /* Scrollbar geometry */
    int track_x;
    int track_y;
    int track_w;
    int track_h;

    bool is_dragging;
    int drag_start_mouse_y;
    int drag_start_offset;
} scrollview_t;

static inline void scrollview_init(scrollview_t *sv, int total, int visible, int x, int y, int w, int h) {
    if (!sv) return;
    sv->total_items = total;
    sv->visible_items = visible > 0 ? visible : 1;
    sv->track_x = x;
    sv->track_y = y;
    sv->track_w = w > 0 ? w : 12;
    sv->track_h = h > 0 ? h : 100;
    int max_s = sv->total_items - sv->visible_items;
    if (max_s < 0) max_s = 0;
    if (sv->scroll_offset > max_s) sv->scroll_offset = max_s;
    if (sv->scroll_offset < 0) sv->scroll_offset = 0;
}

static inline void scrollview_handle_wheel(scrollview_t *sv, int delta_z) {
    if (!sv) return;
    int max_s = sv->total_items - sv->visible_items;
    if (max_s < 0) max_s = 0;
    if (delta_z > 0) sv->scroll_offset -= 3;
    else if (delta_z < 0) sv->scroll_offset += 3;
    if (sv->scroll_offset < 0) sv->scroll_offset = 0;
    if (sv->scroll_offset > max_s) sv->scroll_offset = max_s;
}

static inline bool scrollview_handle_mouse_down(scrollview_t *sv, int mx, int my) {
    if (!sv) return false;
    int max_s = sv->total_items - sv->visible_items;
    if (max_s <= 0) return false;
    if (mx < sv->track_x || mx > sv->track_x + sv->track_w ||
        my < sv->track_y || my > sv->track_y + sv->track_h) {
        return false;
    }

    int thumb_h = (sv->visible_items * sv->track_h) / sv->total_items;
    if (thumb_h < 24) thumb_h = 24;
    if (thumb_h > sv->track_h) thumb_h = sv->track_h;

    int thumb_y = sv->track_y + (sv->scroll_offset * (sv->track_h - thumb_h)) / max_s;
    if (my >= thumb_y && my <= thumb_y + thumb_h) {
        sv->is_dragging = true;
        sv->drag_start_mouse_y = my;
        sv->drag_start_offset = sv->scroll_offset;
    } else if (my < thumb_y) {
        sv->scroll_offset -= sv->visible_items;
        if (sv->scroll_offset < 0) sv->scroll_offset = 0;
    } else {
        sv->scroll_offset += sv->visible_items;
        if (sv->scroll_offset > max_s) sv->scroll_offset = max_s;
    }
    return true;
}

static inline bool scrollview_handle_mouse_move(scrollview_t *sv, int my) {
    if (!sv || !sv->is_dragging) return false;
    int max_s = sv->total_items - sv->visible_items;
    if (max_s <= 0) return false;
    int thumb_h = (sv->visible_items * sv->track_h) / sv->total_items;
    if (thumb_h < 24) thumb_h = 24;
    int travel = sv->track_h - thumb_h;
    if (travel <= 0) return false;

    int dy = my - sv->drag_start_mouse_y;
    int delta_items = (dy * max_s) / travel;
    int new_offset = sv->drag_start_offset + delta_items;
    if (new_offset < 0) new_offset = 0;
    if (new_offset > max_s) new_offset = max_s;
    sv->scroll_offset = new_offset;
    return true;
}

static inline void scrollview_handle_mouse_up(scrollview_t *sv) {
    if (sv) sv->is_dragging = false;
}

static inline void scrollview_render(const scrollview_t *sv, gfx_surface_t *s, u32 track_col, u32 thumb_col) {
    if (!sv || !s) return;
    int max_s = sv->total_items - sv->visible_items;
    gfx_fill_rounded_rect(s, sv->track_x, sv->track_y, sv->track_w, sv->track_h, 3, track_col);
    if (max_s <= 0) return;

    int thumb_h = (sv->visible_items * sv->track_h) / sv->total_items;
    if (thumb_h < 24) thumb_h = 24;
    if (thumb_h > sv->track_h) thumb_h = sv->track_h;
    int thumb_y = sv->track_y + (sv->scroll_offset * (sv->track_h - thumb_h)) / max_s;

    gfx_fill_rounded_rect(s, sv->track_x + 1, thumb_y, sv->track_w - 2, thumb_h, 3, thumb_col);
}

#endif /* ASTRA_KERNEL_SCROLLVIEW_H */
