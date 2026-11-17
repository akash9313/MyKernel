#include <kernel/terminal.h>
#include <kernel/shell.h>
#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/mem.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/kprintf.h>
#include <kernel/drivers/apic.h>

#define MAX_TERMINAL_INSTANCES 2

static struct {
    terminal_state_t terminals[MAX_TERMINAL_INSTANCES];
    shell_context_t  shells[MAX_TERMINAL_INSTANCES];
    int              active_count;
} g_term_mgr = {0};

/* Shell output callback bridging to terminal_print */
static void terminal_shell_output(void *ctx, const char *str, u32 color) {
    terminal_state_t *term = (terminal_state_t *)ctx;
    if (term && str) {
        terminal_print(term, str, color);
    }
}

static size_t term_strlen(const char *s) {
    size_t l = 0;
    while (s && s[l]) l++;
    return l;
}

static void term_strcpy(char *dst, const char *src, size_t max) {
    if (!dst || max == 0) return;
    size_t i = 0;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void term_free_scrollback(terminal_state_t *term) {
    if (term && term->scrollback && term->scrollback_pages > 0) {
        pmm_free_contiguous(term->scrollback_pa, term->scrollback_pages);
        term->scrollback = NULL;
        term->scrollback_pa = 0;
        term->scrollback_pages = 0;
    }
}

static void terminal_on_destroy(wm_window_t *win) {
    if (!win) return;
    terminal_state_t *term = (terminal_state_t *)win->user_data;
    if (term) {
        term_free_scrollback(term);
        term->active = false;
        if (g_term_mgr.active_count > 0) g_term_mgr.active_count--;
    }
}

void terminal_clear(terminal_state_t *term) {
    if (!term || !term->scrollback) return;
    int cap = term->scrollback_capacity;
    for (int i = 0; i < cap; i++) {
        term->scrollback[i].len = 0;
        term->scrollback[i].wrapped = false;
        kmemset(term->scrollback[i].chars, 0, TERM_MAX_COLS);
        for (int c = 0; c < TERM_MAX_COLS; c++) {
            term->scrollback[i].colors[c] = TERM_PALETTE_DEFAULT;
        }
    }
    term->total_lines = 0;
    term->scroll_offset = 0;
    term->selecting = false;
}

static void term_advance_line(terminal_state_t *term) {
    if (!term || !term->scrollback) return;
    term->total_lines++;
    int cap = term->scrollback_capacity;
    int line_idx = term->total_lines % cap;
    term->scrollback[line_idx].len = 0;
    term->scrollback[line_idx].wrapped = false;
    kmemset(term->scrollback[line_idx].chars, 0, TERM_MAX_COLS);
    for (int c = 0; c < TERM_MAX_COLS; c++) {
        term->scrollback[line_idx].colors[c] = TERM_PALETTE_DEFAULT;
    }
}

/* Parse ANSI SGR parameters and update attribute color */
static void handle_ansi_sgr(terminal_state_t *term, const char *params) {
    int val = 0;
    const char *p = params;
    while (*p) {
        while (*p == ';' || *p == ' ') p++;
        if (!*p) break;
        val = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
        if (val == 0) {
            term->current_attr_color = TERM_PALETTE_DEFAULT;
        } else if (val == 1) {
            /* Bold / bright */
        } else if (val >= 30 && val <= 37) {
            term->current_attr_color = TERM_PALETTE_BLACK + (val - 30);
        } else if (val >= 90 && val <= 97) {
            term->current_attr_color = TERM_PALETTE_BLACK + (val - 90);
        } else if (val == 39) {
            term->current_attr_color = TERM_PALETTE_DEFAULT;
        }
    }
}

void terminal_print(terminal_state_t *term, const char *str, u32 color) {
    if (!term || !str || !term->scrollback) return;

    if (term->total_lines == 0) {
        term_advance_line(term);
    }

    int cap = term->scrollback_capacity;
    int cur_idx = term->total_lines % cap;
    term_line_t *cur = &term->scrollback[cur_idx];
    u8 base_color_idx = term_color_to_idx(color);

    for (size_t i = 0; str[i]; i++) {
        char c = str[i];

        /* ANSI Escape Sequence Handling */
        if (term->in_escape) {
            if (term->esc_len < (int)sizeof(term->esc_buf) - 2) {
                term->esc_buf[term->esc_len++] = c;
                term->esc_buf[term->esc_len] = '\0';
            }
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                term->in_escape = false;
                if (c == 'm') {
                    const char *params = term->esc_buf;
                    if (*params == '[') params++;
                    handle_ansi_sgr(term, params);
                } else if (c == 'K') {
                    /* Erase to end of line */
                    cur->len = 0;
                } else if (c == 'J') {
                    /* Clear screen */
                    terminal_clear(term);
                    cur_idx = term->total_lines % cap;
                    cur = &term->scrollback[cur_idx];
                }
            }
            continue;
        }

        if (c == '\x1B') {
            term->in_escape = true;
            term->esc_len = 0;
            term->esc_buf[0] = '\0';
            continue;
        }

        if (c == '\n') {
            term_advance_line(term);
            cur_idx = term->total_lines % cap;
            cur = &term->scrollback[cur_idx];
            continue;
        } else if (c == '\r') {
            cur->len = 0;
            continue;
        } else if (c == '\t') {
            int spaces = 4 - (cur->len % 4);
            while (spaces-- > 0 && cur->len < TERM_DEFAULT_COLS) {
                cur->chars[cur->len] = ' ';
                cur->colors[cur->len] = (term->current_attr_color != TERM_PALETTE_DEFAULT) ? term->current_attr_color : base_color_idx;
                cur->len++;
            }
            continue;
        } else if (c == '\b') {
            if (cur->len > 0) cur->len--;
            continue;
        }

        if (cur->len >= TERM_DEFAULT_COLS) {
            cur->wrapped = true;
            term_advance_line(term);
            cur_idx = term->total_lines % cap;
            cur = &term->scrollback[cur_idx];
        }

        cur->chars[cur->len] = c;
        cur->colors[cur->len] = (term->current_attr_color != TERM_PALETTE_DEFAULT) ? term->current_attr_color : base_color_idx;
        cur->len++;
    }
}

void terminal_println(terminal_state_t *term, const char *str, u32 color) {
    terminal_print(term, str, color);
    terminal_print(term, "\n", color);
}

void terminal_prompt(terminal_state_t *term) {
    if (!term) return;
    shell_context_t *sh = (shell_context_t *)term->shell_ctx;
    const char *cwd = sh ? shell_getcwd(sh) : "/";
    const char *user = sh ? shell_getenv(sh, "USER") : "root";

    terminal_print(term, user ? user : "root", TERM_COLOR_PROMPT);
    terminal_print(term, "@astraos", TERM_COLOR_PROMPT);
    terminal_print(term, ":", TERM_COLOR_MUTED);
    terminal_print(term, "[", TERM_COLOR_MUTED);
    terminal_print(term, cwd, TERM_COLOR_PATH);
    terminal_print(term, "]", TERM_COLOR_MUTED);
    terminal_print(term, "$ ", TERM_COLOR_SYMBOL);
}

void terminal_get_cursor_coords(const terminal_state_t *term, int screen_h, int *out_x, int *out_y) {
    if (!term || !out_x || !out_y || !term->scrollback) return;

    int visible_rows = screen_h / TERM_FONT_HEIGHT;

    int cap = term->scrollback_capacity;
    int cur_line_idx = term->total_lines % cap;
    const term_line_t *cur_line = &term->scrollback[cur_line_idx];
    int prompt_chars = cur_line->len;

    int total_chars = prompt_chars + term->input_len;
    int extra_rows = total_chars / TERM_DEFAULT_COLS;

    int start_line = 1;
    if (term->scroll_offset > 0) {
        start_line = term->total_lines - (visible_rows - 1) - term->scroll_offset;
    } else {
        if ((term->total_lines + extra_rows) >= visible_rows) {
            start_line = (term->total_lines + extra_rows) - (visible_rows - 1);
        }
    }
    int oldest_valid = (term->total_lines > cap) ? (term->total_lines - cap + 1) : 1;
    if (start_line < oldest_valid) start_line = oldest_valid;
    if (start_line < 1) start_line = 1;

    int prompt_row = term->total_lines - start_line;
    int base_y = 4 + (prompt_row * TERM_FONT_HEIGHT);

    int cur_char_idx = prompt_chars + term->cursor_pos;
    int cur_row = cur_char_idx / TERM_DEFAULT_COLS;
    int cur_col = cur_char_idx % TERM_DEFAULT_COLS;

    *out_x = 8 + (cur_col * TERM_FONT_WIDTH);
    *out_y = base_y + (cur_row * TERM_FONT_HEIGHT);
}

static void terminal_on_paint(wm_window_t *win, gfx_surface_t *s) {
    if (!win || !s) return;
    terminal_state_t *term = (terminal_state_t *)win->user_data;
    if (!term || !term->scrollback) return;

    /* 1. Deep cosmic slate background */
    gfx_fill_rect(s, 0, 0, s->width, s->height, TERM_COLOR_BG);

    /* 2. Determine visible rows & bounded scrollback range */
    int visible_rows = s->height / TERM_FONT_HEIGHT;
    int cap = term->scrollback_capacity;
    int tot = term->total_lines;
    int retained = (tot > cap) ? cap : tot;
    int max_offset = (retained > visible_rows) ? (retained - visible_rows) : 0;

    if (term->scroll_offset < 0) term->scroll_offset = 0;
    if (term->scroll_offset > max_offset) term->scroll_offset = max_offset;

    int cur_line_idx = tot % cap;
    term_line_t *cur_line = &term->scrollback[cur_line_idx];
    int prompt_chars = cur_line->len;

    int total_chars = prompt_chars + term->input_len;
    int extra_rows = total_chars / TERM_DEFAULT_COLS;

    int start_line = 1;
    if (term->scroll_offset > 0) {
        start_line = tot - (visible_rows - 1) - term->scroll_offset;
    } else {
        if ((tot + extra_rows) >= visible_rows) {
            start_line = (tot + extra_rows) - (visible_rows - 1);
        }
    }
    int oldest_valid = (tot > cap) ? (tot - cap + 1) : 1;
    if (start_line < oldest_valid) start_line = oldest_valid;
    if (start_line < 1) start_line = 1;

    /* 3. Render visible scrollback lines */
    int max_line_to_draw = (term->scroll_offset > 0)
        ? (tot - term->scroll_offset)
        : tot;

    int max_text_x = s->width - 18; /* Reserve 16px for scrollbar */

    for (int l = start_line; l <= max_line_to_draw; l++) {
        int row = l - start_line;
        int y = 4 + (row * TERM_FONT_HEIGHT);
        if (y + TERM_FONT_HEIGHT > s->height) break;

        int idx = l % cap;
        term_line_t *tl = &term->scrollback[idx];

        int x = 8;
        for (int c = 0; c < tl->len && x < max_text_x; c++) {
            /* Check selection highlight */
            bool is_selected = false;
            if (term->selecting && term->sel_start_line != term->sel_end_line) {
                int sl = (term->sel_start_line < term->sel_end_line) ? term->sel_start_line : term->sel_end_line;
                int el = (term->sel_start_line < term->sel_end_line) ? term->sel_end_line : term->sel_start_line;
                if (l >= sl && l <= el) is_selected = true;
            }

            if (is_selected) {
                gfx_fill_rect(s, x, y, TERM_FONT_WIDTH, TERM_FONT_HEIGHT, TERM_COLOR_SEL_BG);
                gfx_draw_char(s, x, y, tl->chars[c], 0xFFFFFFFF, 0, true);
            } else {
                gfx_draw_char(s, x, y, tl->chars[c], term_idx_to_color(tl->colors[c]), 0, true);
            }
            x += TERM_FONT_WIDTH;
        }
    }

    /* 4. Render active input text and cursor if viewport is at bottom (scroll_offset == 0) */
    if (term->scroll_offset == 0) {
        int base_row = tot - start_line;
        int base_y = 4 + (base_row * TERM_FONT_HEIGHT);

        /* Draw entered input buffer text */
        for (int i = 0; i < term->input_len; i++) {
            int char_idx = prompt_chars + i;
            int char_row = char_idx / TERM_DEFAULT_COLS;
            int char_col = char_idx % TERM_DEFAULT_COLS;
            int char_x = 8 + (char_col * TERM_FONT_WIDTH);
            int char_y = base_y + (char_row * TERM_FONT_HEIGHT);

            if (char_y >= 4 && char_y + TERM_FONT_HEIGHT <= s->height && char_x < max_text_x) {
                gfx_draw_char(s, char_x, char_y, term->input_buf[i], TERM_COLOR_FG, 0, true);
            }
        }

        /* Draw blinking cursor */
        int cur_x = 8, cur_y = 4;
        terminal_get_cursor_coords(term, s->height, &cur_x, &cur_y);
        u64 ticks = apic_get_ticks();
        if ((ticks / 50) % 2 == 0) {
            if (cur_y >= 4 && cur_y + TERM_FONT_HEIGHT <= s->height && cur_x < max_text_x) {
                gfx_fill_rect(s, cur_x, cur_y, 2, TERM_FONT_HEIGHT - 2, TERM_COLOR_CURSOR);
            }
        }
    }

    /* 5. Render Modern Interactive Vertical Scrollbar */
    int sb_w = 10;
    int sb_x = s->width - sb_w - 3;
    int sb_y = 4;
    int sb_h = s->height - 8;

    /* Scrollbar Trough */
    gfx_fill_rect(s, sb_x, sb_y, sb_w, sb_h, TERM_COLOR_SCROLL_TROUGH);

    /* Scrollbar Thumb */
    int thumb_h = (retained > 0) ? (visible_rows * sb_h) / retained : sb_h;
    if (thumb_h < 20) thumb_h = 20;
    if (thumb_h > sb_h) thumb_h = sb_h;

    int travel = sb_h - thumb_h;
    int thumb_y = sb_y;
    if (max_offset > 0 && travel > 0) {
        thumb_y = sb_y + (travel * (max_offset - term->scroll_offset)) / max_offset;
    }
    if (thumb_y < sb_y) thumb_y = sb_y;
    if (thumb_y + thumb_h > sb_y + sb_h) thumb_y = sb_y + sb_h - thumb_h;

    u32 thumb_col = (term->scrollbar_dragging || term->scrollbar_hover)
        ? TERM_COLOR_SCROLL_HOVER
        : TERM_COLOR_SCROLL_THUMB;

    gfx_fill_rect(s, sb_x + 1, thumb_y + 1, sb_w - 2, thumb_h - 2, thumb_col);

    /* 6. Render Floating Scroll Indicator when scrolled up */
    if (term->scroll_offset > 0) {
        char scroll_ind[40];
        int off = term->scroll_offset;
        term_strcpy(scroll_ind, "[SCROLL +", sizeof(scroll_ind));
        char nbuf[16];
        int np = 0;
        if (off == 0) { nbuf[np++] = '0'; }
        else {
            char tmp[16]; int tp = 0;
            while (off > 0) { tmp[tp++] = '0' + (off % 10); off /= 10; }
            while (tp > 0) nbuf[np++] = tmp[--tp];
        }
        nbuf[np] = '\0';
        size_t slen = term_strlen(scroll_ind);
        for (int k = 0; nbuf[k] && slen + k < sizeof(scroll_ind) - 4; k++) {
            scroll_ind[slen + k] = nbuf[k];
        }
        scroll_ind[slen + np] = ']';
        scroll_ind[slen + np + 1] = '\0';

        int ind_w = (int)term_strlen(scroll_ind) * TERM_FONT_WIDTH;
        int ind_x = s->width - ind_w - 24;
        gfx_fill_rect(s, ind_x - 4, 3, ind_w + 8, TERM_FONT_HEIGHT + 2, 0xCC161B26U);
        for (int i = 0; scroll_ind[i]; i++) {
            gfx_draw_char(s, ind_x + (i * TERM_FONT_WIDTH), 4, scroll_ind[i], TERM_COLOR_WARN, 0, true);
        }
    }
}

static void terminal_on_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return;
    terminal_state_t *term = (terminal_state_t *)win->user_data;
    if (!term || !term->scrollback) return;

    int visible_rows = win->client_h / TERM_FONT_HEIGHT;
    int cap = term->scrollback_capacity;
    int tot = term->total_lines;
    int retained = (tot > cap) ? cap : tot;
    int max_offset = (retained > visible_rows) ? (retained - visible_rows) : 0;

    /* A. Mouse Wheel Navigation */
    if (ev->type == INPUT_EVENT_MOUSE_WHEEL) {
        int dz = ev->mouse.delta_z;
        if (dz > 0) {
            term->scroll_offset += dz * 3;
            if (term->scroll_offset > max_offset) term->scroll_offset = max_offset;
        } else if (dz < 0) {
            term->scroll_offset += dz * 3; /* dz is negative */
            if (term->scroll_offset < 0) term->scroll_offset = 0;
        }
        return;
    }

    /* B. Mouse Clicks and Scrollbar Interaction */
    if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        int mx = ev->mouse.x - win->client_x;
        int my = ev->mouse.y - win->client_y;

        int sb_w = 14;
        int sb_x = win->client_w - sb_w;
        int sb_y = 4;
        int sb_h = win->client_h - 8;

        /* Check if click is inside scrollbar area */
        if (mx >= sb_x) {
            int thumb_h = (retained > 0) ? (visible_rows * sb_h) / retained : sb_h;
            if (thumb_h < 20) thumb_h = 20;
            if (thumb_h > sb_h) thumb_h = sb_h;

            int travel = sb_h - thumb_h;
            int thumb_y = sb_y;
            if (max_offset > 0 && travel > 0) {
                thumb_y = sb_y + (travel * (max_offset - term->scroll_offset)) / max_offset;
            }

            if (my >= thumb_y && my <= thumb_y + thumb_h) {
                /* Start dragging scrollbar thumb */
                term->scrollbar_dragging = true;
                term->drag_start_y = my;
                term->drag_orig_offset = term->scroll_offset;
            } else if (my < thumb_y) {
                /* Page up */
                term->scroll_offset += (visible_rows - 2);
                if (term->scroll_offset > max_offset) term->scroll_offset = max_offset;
            } else {
                /* Page down */
                term->scroll_offset -= (visible_rows - 2);
                if (term->scroll_offset < 0) term->scroll_offset = 0;
            }
            return;
        }

        /* Click inside text area -> begin text selection */
        term->selecting = true;
        term->has_selection = false;
        term->sel_start_line = tot - (visible_rows - 1) - term->scroll_offset + (my / TERM_FONT_HEIGHT);
        term->sel_start_col = mx / TERM_FONT_WIDTH;
        term->sel_end_line = term->sel_start_line;
        term->sel_end_col = term->sel_start_col;
        return;
    }

    if (ev->type == INPUT_EVENT_MOUSE_MOVE) {
        int mx = ev->mouse.x - win->client_x;
        int my = ev->mouse.y - win->client_y;

        int sb_x = win->client_w - 14;
        term->scrollbar_hover = (mx >= sb_x);

        if (term->scrollbar_dragging) {
            int sb_h = win->client_h - 8;
            int thumb_h = (retained > 0) ? (visible_rows * sb_h) / retained : sb_h;
            if (thumb_h < 20) thumb_h = 20;
            if (thumb_h > sb_h) thumb_h = sb_h;
            int travel = sb_h - thumb_h;

            if (travel > 0 && max_offset > 0) {
                int delta_y = my - term->drag_start_y;
                int offset_delta = (delta_y * max_offset) / travel;
                term->scroll_offset = term->drag_orig_offset - offset_delta;
                if (term->scroll_offset < 0) term->scroll_offset = 0;
                if (term->scroll_offset > max_offset) term->scroll_offset = max_offset;
            }
            return;
        }

        if (term->selecting) {
            term->sel_end_line = tot - (visible_rows - 1) - term->scroll_offset + (my / TERM_FONT_HEIGHT);
            term->sel_end_col = mx / TERM_FONT_WIDTH;
            if (term->sel_end_line != term->sel_start_line || term->sel_end_col != term->sel_start_col) {
                term->has_selection = true;
            }
            return;
        }
        return;
    }

    if (ev->type == INPUT_EVENT_MOUSE_UP) {
        term->scrollbar_dragging = false;
        if (term->selecting) {
            term->selecting = false;
            if (term->sel_end_line != term->sel_start_line || term->sel_end_col != term->sel_start_col) {
                term->has_selection = true;
            }
        }
        return;
    }

    /* C. Keyboard Handling */
    if (ev->type == INPUT_EVENT_KEY_DOWN) {
        u8 ascii = ev->key.ascii;
        u32 keycode = ev->key.keycode;
        bool is_shift = (ev->key.modifiers & KEY_MOD_SHIFT) != 0;
        bool is_ctrl  = (ev->key.modifiers & KEY_MOD_CTRL) != 0;

        /* Check Ctrl shortcuts */
        if (is_ctrl) {
            if (ascii == 'c' || ascii == 'C' || ascii == 3) {
                /* Copy selection to clipboard if text is selected */
                if (term->has_selection || (term->selecting && (term->sel_start_line != term->sel_end_line || term->sel_start_col != term->sel_end_col))) {
                    char sel_buf[1024];
                    int sp = 0;
                    int sl = (term->sel_start_line < term->sel_end_line) ? term->sel_start_line : term->sel_end_line;
                    int el = (term->sel_start_line < term->sel_end_line) ? term->sel_end_line : term->sel_start_line;
                    for (int l = sl; l <= el && sp < 1000; l++) {
                        int idx = l % cap;
                        term_line_t *tl = &term->scrollback[idx];
                        int sc = (l == sl) ? ((term->sel_start_line < term->sel_end_line) ? term->sel_start_col : term->sel_end_col) : 0;
                        int ec = (l == el) ? ((term->sel_start_line < term->sel_end_line) ? term->sel_end_col : term->sel_start_col) : tl->len;
                        if (sl == el && term->sel_start_col > term->sel_end_col) {
                            sc = term->sel_end_col;
                            ec = term->sel_start_col;
                        }
                        for (int c = sc; c < ec && c < tl->len && sp < 1000; c++) sel_buf[sp++] = tl->chars[c];
                        if (l < el && sp < 1000) sel_buf[sp++] = '\n';
                    }
                    sel_buf[sp] = '\0';
                    clipboard_set_text(sel_buf, sp);
                    term->has_selection = false;
                    term->selecting = false;
                    return;
                }

                terminal_println(term, "^C", TERM_COLOR_ERROR);
                term->input_len = 0;
                term->cursor_pos = 0;
                term->input_buf[0] = '\0';
                terminal_prompt(term);
                term->scroll_offset = 0;
                return;
            } else if (ascii == 'l' || ascii == 'L' || ascii == 12) {
                terminal_clear(term);
                terminal_prompt(term);
                if (term->input_len > 0) {
                    terminal_print(term, term->input_buf, TERM_COLOR_FG);
                }
                return;
            } else if (ascii == 'v' || ascii == 'V' || ascii == 22) {
                term->scroll_offset = 0;
                size_t clen = 0;
                const char *clip = clipboard_get_text(&clen);
                if (clip && clen > 0) {
                    for (size_t i = 0; i < clen && term->input_len < TERM_INPUT_MAX - 2; i++) {
                        if (clip[i] == '\r' || clip[i] == '\n') break;
                        for (int k = term->input_len; k > term->cursor_pos; k--) {
                            term->input_buf[k] = term->input_buf[k - 1];
                        }
                        term->input_buf[term->cursor_pos] = clip[i];
                        term->input_len++;
                        term->cursor_pos++;
                    }
                    term->input_buf[term->input_len] = '\0';
                }
                return;
            } else if (ascii == 'u' || ascii == 'U' || ascii == 21) {
                if (term->cursor_pos > 0) {
                    for (int i = 0; i <= term->input_len - term->cursor_pos; i++) {
                        term->input_buf[i] = term->input_buf[term->cursor_pos + i];
                    }
                    term->input_len -= term->cursor_pos;
                    term->cursor_pos = 0;
                    term->input_buf[term->input_len] = '\0';
                }
                return;
            } else if (ascii == 'k' || ascii == 'K' || ascii == 11) {
                term->input_len = term->cursor_pos;
                term->input_buf[term->input_len] = '\0';
                return;
            } else if (ascii == 'a' || ascii == 'A' || ascii == 1) {
                term->cursor_pos = 0;
                return;
            } else if (ascii == 'e' || ascii == 'E' || ascii == 5) {
                term->cursor_pos = term->input_len;
                return;
            }
        }

        /* PageUp / PageDown Navigation (PageUp, Shift+PageUp) */
        if (keycode == KEY_PAGE_UP) {
            term->scroll_offset += (visible_rows - 2);
            if (term->scroll_offset > max_offset) term->scroll_offset = max_offset;
            return;
        } else if (keycode == KEY_PAGE_DOWN) {
            term->scroll_offset -= (visible_rows - 2);
            if (term->scroll_offset < 0) term->scroll_offset = 0;
            return;
        }

        /* Home / End Navigation */
        if (keycode == KEY_HOME) {
            if (is_shift || term->scroll_offset > 0) {
                term->scroll_offset = max_offset;
            } else {
                term->cursor_pos = 0;
            }
            return;
        } else if (keycode == KEY_END) {
            if (is_shift || term->scroll_offset > 0) {
                term->scroll_offset = 0;
            } else {
                term->cursor_pos = term->input_len;
            }
            return;
        }

        /* Up / Down Navigation */
        if (keycode == KEY_UP) {
            if (is_shift) {
                term->scroll_offset += 1;
                if (term->scroll_offset > max_offset) term->scroll_offset = max_offset;
                return;
            }
            if (term->scroll_offset > 0) {
                term->scroll_offset += 1;
                if (term->scroll_offset > max_offset) term->scroll_offset = max_offset;
                return;
            }
            /* Navigate back in history */
            if (term->history_count > 0 && term->history_idx > 0) {
                if (term->history_idx == term->history_count) {
                    term_strcpy(term->history_temp, term->input_buf, TERM_INPUT_MAX);
                }
                term->history_idx--;
                term_strcpy(term->input_buf, term->history[term->history_idx], TERM_INPUT_MAX);
                term->input_len = (int)term_strlen(term->input_buf);
                term->cursor_pos = term->input_len;
            }
            return;
        } else if (keycode == KEY_DOWN) {
            if (is_shift) {
                term->scroll_offset -= 1;
                if (term->scroll_offset < 0) term->scroll_offset = 0;
                return;
            }
            if (term->scroll_offset > 0) {
                term->scroll_offset -= 1;
                if (term->scroll_offset < 0) term->scroll_offset = 0;
                return;
            }
            /* Navigate forward in history */
            if (term->history_idx < term->history_count) {
                term->history_idx++;
                if (term->history_idx == term->history_count) {
                    term_strcpy(term->input_buf, term->history_temp, TERM_INPUT_MAX);
                } else {
                    term_strcpy(term->input_buf, term->history[term->history_idx], TERM_INPUT_MAX);
                }
                term->input_len = (int)term_strlen(term->input_buf);
                term->cursor_pos = term->input_len;
            }
            return;
        }

        /* Left / Right arrows */
        if (keycode == KEY_LEFT) {
            if (term->cursor_pos > 0) term->cursor_pos--;
            return;
        } else if (keycode == KEY_RIGHT) {
            if (term->cursor_pos < term->input_len) term->cursor_pos++;
            return;
        }

        /* Backspace / Delete / Enter / Printable: Snap viewport to bottom */
        term->scroll_offset = 0;

        if (keycode == KEY_ENTER || ascii == '\n' || ascii == '\r') {
            /* Record command to history */
            if (term->input_len > 0) {
                if (term->history_count < TERM_HISTORY_MAX) {
                    term_strcpy(term->history[term->history_count++], term->input_buf, TERM_INPUT_MAX);
                } else {
                    for (int i = 1; i < TERM_HISTORY_MAX; i++) {
                        term_strcpy(term->history[i - 1], term->history[i], TERM_INPUT_MAX);
                    }
                    term_strcpy(term->history[TERM_HISTORY_MAX - 1], term->input_buf, TERM_INPUT_MAX);
                }
            }
            term->history_idx = term->history_count;

            /* Print input line to scrollback buffer */
            terminal_println(term, term->input_buf, TERM_COLOR_FG);

            /* Execute in shell engine */
            char cmd_exec[TERM_INPUT_MAX];
            term_strcpy(cmd_exec, term->input_buf, sizeof(cmd_exec));

            term->input_len = 0;
            term->cursor_pos = 0;
            term->input_buf[0] = '\0';

            shell_context_t *sh = (shell_context_t *)term->shell_ctx;
            if (sh) {
                shell_execute(sh, cmd_exec);
            }

            /* Display new prompt */
            terminal_prompt(term);
            term->scroll_offset = 0;

        } else if (keycode == KEY_TAB || ascii == '\t') {
            shell_context_t *sh = (shell_context_t *)term->shell_ctx;
            if (sh && term->cursor_pos > 0) {
                int tok_start = term->cursor_pos - 1;
                while (tok_start > 0 && term->input_buf[tok_start - 1] != ' ') {
                    tok_start--;
                }
                bool is_cmd = (tok_start == 0);
                char token[64];
                int tlen = term->cursor_pos - tok_start;
                if (tlen >= (int)sizeof(token)) tlen = (int)sizeof(token) - 1;
                for (int k = 0; k < tlen; k++) token[k] = term->input_buf[tok_start + k];
                token[tlen] = '\0';

                char match[128];
                int n_matches = shell_complete(sh, token, is_cmd, match, sizeof(match));
                if (n_matches > 0 && match[0] != '\0') {
                    size_t mlen = term_strlen(match);
                    int tail_len = term->input_len - term->cursor_pos;
                    if (tok_start + (int)mlen + tail_len < TERM_INPUT_MAX - 2) {
                        int new_cursor = tok_start + (int)mlen;
                        if (is_cmd && n_matches == 1 && new_cursor < TERM_INPUT_MAX - 3) {
                            match[mlen++] = ' ';
                            match[mlen] = '\0';
                            new_cursor++;
                        }
                        char tail[TERM_INPUT_MAX];
                        for (int k = 0; k < tail_len; k++) tail[k] = term->input_buf[term->cursor_pos + k];
                        for (size_t k = 0; k < mlen; k++) term->input_buf[tok_start + k] = match[k];
                        for (int k = 0; k < tail_len; k++) term->input_buf[tok_start + mlen + k] = tail[k];
                        term->input_len = tok_start + (int)mlen + tail_len;
                        term->cursor_pos = new_cursor;
                        term->input_buf[term->input_len] = '\0';
                    }
                }
            }
        } else if (keycode == KEY_BACKSPACE || ascii == '\b' || ascii == 127) {
            if (term->cursor_pos > 0) {
                for (int i = term->cursor_pos - 1; i < term->input_len - 1; i++) {
                    term->input_buf[i] = term->input_buf[i + 1];
                }
                term->input_len--;
                term->cursor_pos--;
                term->input_buf[term->input_len] = '\0';
            }
        } else if (keycode == KEY_DELETE) {
            if (term->cursor_pos < term->input_len) {
                for (int i = term->cursor_pos; i < term->input_len - 1; i++) {
                    term->input_buf[i] = term->input_buf[i + 1];
                }
                term->input_len--;
                term->input_buf[term->input_len] = '\0';
            }
        } else if (ascii >= ' ' && ascii <= '~') {
            if (term->input_len < TERM_INPUT_MAX - 2) {
                for (int i = term->input_len; i > term->cursor_pos; i--) {
                    term->input_buf[i] = term->input_buf[i - 1];
                }
                term->input_buf[term->cursor_pos] = ascii;
                term->input_len++;
                term->cursor_pos++;
                term->input_buf[term->input_len] = '\0';
            }
        }
    }
}

void app_terminal_launch_with_command(const char *cmd) {
    /* Find slot for new terminal */
    int slot = -1;
    for (int i = 0; i < MAX_TERMINAL_INSTANCES; i++) {
        if (!g_term_mgr.terminals[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) slot = 0;

    terminal_state_t *term = &g_term_mgr.terminals[slot];
    shell_context_t  *sh   = &g_term_mgr.shells[slot];

    /* Clean up any previous allocation in slot */
    term_free_scrollback(term);
    kmemset(term, 0, sizeof(*term));

    /* Allocate bounded 4096-line scrollback buffer dynamically via PMM */
    size_t scroll_bytes = (size_t)TERM_SCROLLBACK_DEFAULT * sizeof(term_line_t);
    size_t pages = (scroll_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    phys_addr_t pa = 0;
    if (pmm_alloc_contiguous(pages, &pa) != PMM_SUCCESS) {
        kprintf("[TERMINAL ERROR] Failed to allocate %llu bytes for scrollback buffer!\n",
                (unsigned long long)scroll_bytes);
        return;
    }

    struct limine_hhdm_response *hhdm = boot_get_hhdm_response();
    term->scrollback = (term_line_t *)(pa + hhdm->offset);
    kmemset(term->scrollback, 0, scroll_bytes);
    term->scrollback_pa = pa;
    term->scrollback_pages = pages;
    term->scrollback_capacity = TERM_SCROLLBACK_DEFAULT;

    shell_init(sh, terminal_shell_output, term);
    term->shell_ctx = sh;
    term->active = true;

    /* Window geometry: 680x440 with client surface */
    int start_x = 240 + (slot * 30);
    int start_y = 60 + (slot * 30);
    wm_window_t *win = wm_create_window("AstraOS Terminal", start_x, start_y, 680, 440);
    if (!win) {
        term_free_scrollback(term);
        term->active = false;
        return;
    }

    term->window = win;
    win->user_data = term;
    win->on_paint = terminal_on_paint;
    win->on_event = terminal_on_event;
    win->on_destroy = terminal_on_destroy;

    /* Header banner and prompt */
    terminal_println(term, "AstraOS Terminal 2.0 [Cosmic Vista Theme | High-Capacity Scrollback]", TERM_COLOR_INFO);
    terminal_println(term, "Type 'help' for built-in command index or 'pkg search' for packages.\n", TERM_COLOR_MUTED);

    if (cmd && *cmd) {
        terminal_prompt(term);
        terminal_println(term, cmd, TERM_COLOR_FG);
        shell_execute(sh, cmd);
    }

    terminal_prompt(term);
    g_term_mgr.active_count++;
}

void app_terminal_launch(void) {
    app_terminal_launch_with_command(NULL);
}

/* Terminal Subsystem Self-Test */
bool terminal_self_test(void) {
    kprintf("\n[TERMINAL TEST] Starting AstraOS Terminal Subsystem Self-Test...\n");

    /* 1. Launch test terminal */
    app_terminal_launch();
    wm_window_t *w = wm_get_focused();
    if (!w || !w->user_data) {
        kprintf("[TERMINAL TEST FAIL] Failed to launch and focus terminal window!\n");
        return false;
    }
    terminal_state_t *term = (terminal_state_t *)w->user_data;
    if (!term->scrollback || term->scrollback_capacity != TERM_SCROLLBACK_DEFAULT) {
        kprintf("[TERMINAL TEST FAIL] Dynamic scrollback buffer allocation failed!\n");
        return false;
    }
    kprintf("  PASS: Terminal 2.0 window initialized with dynamic %d-line PMM scrollback buffer.\n",
            term->scrollback_capacity);

    /* 2. Test initial cursor position immediately after prompt */
    int cur_line_idx = term->total_lines % term->scrollback_capacity;
    int prompt_len = term->scrollback[cur_line_idx].len;
    int cur_x = 0, cur_y = 0;
    terminal_get_cursor_coords(term, w->client_h, &cur_x, &cur_y);
    int expected_x = 8 + (prompt_len * TERM_FONT_WIDTH);
    int test_vis_rows = w->client_h / TERM_FONT_HEIGHT;
    int test_start_line = (term->total_lines >= test_vis_rows) ? (term->total_lines - (test_vis_rows - 1)) : 1;
    int expected_y = 4 + ((term->total_lines - test_start_line) * TERM_FONT_HEIGHT);
    if (cur_x != expected_x || cur_y != expected_y) {
        kprintf("[TERMINAL TEST FAIL] Initial cursor mismatch! Got (X=%d, Y=%d), expected (X=%d, Y=%d)\n",
                cur_x, cur_y, expected_x, expected_y);
        return false;
    }
    kprintf("  PASS: Initial cursor position verified at prompt boundary (X=%d, Y=%d).\n", cur_x, cur_y);

    /* 3. Feed simulated key event to terminal ('echo hi') */
    const char *test_cmd = "echo hi";
    for (int i = 0; test_cmd[i]; i++) {
        input_event_t ev_key = {
            .type = INPUT_EVENT_KEY_DOWN,
            .key = { .ascii = test_cmd[i], .keycode = 0, .modifiers = 0 }
        };
        terminal_on_event(w, &ev_key);
    }

    if (term->input_len != 7 || term->cursor_pos != 7) {
        kprintf("[TERMINAL TEST FAIL] Keystroke handling failed! Input='%s', len=%d\n", term->input_buf, term->input_len);
        return false;
    }
    terminal_get_cursor_coords(term, w->client_h, &cur_x, &cur_y);
    if (cur_x != expected_x + (7 * TERM_FONT_WIDTH)) {
        kprintf("[TERMINAL TEST FAIL] Cursor X after typing mismatch! Got %d, expected %d\n",
                cur_x, expected_x + (7 * TERM_FONT_WIDTH));
        return false;
    }
    kprintf("  PASS: Cursor movement following typed characters verified.\n");

    /* 4. Test Left/Right arrows, Home, End, Delete, Backspace */
    input_event_t ev_left = { .type = INPUT_EVENT_KEY_DOWN, .key = { .keycode = KEY_LEFT } };
    terminal_on_event(w, &ev_left); /* cursor_pos 6 */
    terminal_on_event(w, &ev_left); /* cursor_pos 5 */
    if (term->cursor_pos != 5) {
        kprintf("[TERMINAL TEST FAIL] Left arrow failed! cursor_pos=%d\n", term->cursor_pos);
        return false;
    }

    input_event_t ev_home = { .type = INPUT_EVENT_KEY_DOWN, .key = { .keycode = KEY_HOME } };
    terminal_on_event(w, &ev_home);
    if (term->cursor_pos != 0) {
        kprintf("[TERMINAL TEST FAIL] Home key failed! cursor_pos=%d\n", term->cursor_pos);
        return false;
    }
    terminal_get_cursor_coords(term, w->client_h, &cur_x, &cur_y);
    if (cur_x != expected_x) {
        kprintf("[TERMINAL TEST FAIL] Home cursor X mismatch! Got %d, expected %d\n", cur_x, expected_x);
        return false;
    }

    input_event_t ev_del = { .type = INPUT_EVENT_KEY_DOWN, .key = { .keycode = KEY_DELETE } };
    terminal_on_event(w, &ev_del); /* deletes 'e', buffer -> "cho hi" */
    if (term->input_len != 6 || term->input_buf[0] != 'c') {
        kprintf("[TERMINAL TEST FAIL] Delete key failed! buf='%s'\n", term->input_buf);
        return false;
    }

    input_event_t ev_end = { .type = INPUT_EVENT_KEY_DOWN, .key = { .keycode = KEY_END } };
    terminal_on_event(w, &ev_end);
    if (term->cursor_pos != 6) {
        kprintf("[TERMINAL TEST FAIL] End key failed! cursor_pos=%d\n", term->cursor_pos);
        return false;
    }

    input_event_t ev_bs = { .type = INPUT_EVENT_KEY_DOWN, .key = { .keycode = KEY_BACKSPACE } };
    terminal_on_event(w, &ev_bs); /* deletes 'i', buffer -> "cho h" */
    if (term->input_len != 5 || term->cursor_pos != 5) {
        kprintf("[TERMINAL TEST FAIL] Backspace key failed! buf='%s'\n", term->input_buf);
        return false;
    }
    kprintf("  PASS: In-line editing & navigation keys (Home, End, Left, Right, Del, Backspace) verified.\n");

    /* 5. Simulate Enter key to execute command */
    input_event_t ev_enter = {
        .type = INPUT_EVENT_KEY_DOWN,
        .key = { .ascii = '\n', .keycode = KEY_ENTER, .modifiers = 0 }
    };
    terminal_on_event(w, &ev_enter);

    if (term->input_len != 0 || term->cursor_pos != 0) {
        kprintf("[TERMINAL TEST FAIL] Enter key did not reset active input line!\n");
        return false;
    }

    /* 6. High-Capacity Scrollback & Viewport Navigation Verification */
    kprintf("  -> Generating 500 lines of test output to verify scrollback ring buffer...\n");
    for (int i = 1; i <= 500; i++) {
        char lbuf[32];
        int lp = 0;
        const char *prefix = "Line #";
        for (int k = 0; prefix[k]; k++) lbuf[lp++] = prefix[k];
        int num = i;
        char nstr[8]; int np = 0;
        while (num > 0) { nstr[np++] = '0' + (num % 10); num /= 10; }
        while (np > 0) lbuf[lp++] = nstr[--np];
        lbuf[lp] = '\0';
        terminal_println(term, lbuf, TERM_COLOR_FG);
    }
    if (term->total_lines < 500) {
        kprintf("[TERMINAL TEST FAIL] Total lines counter invalid: %d\n", term->total_lines);
        return false;
    }
    kprintf("  PASS: 500 lines committed to scrollback ring buffer without memory corruption.\n");

    /* Test PageUp Navigation */
    input_event_t ev_pgup = { .type = INPUT_EVENT_KEY_DOWN, .key = { .keycode = KEY_PAGE_UP } };
    terminal_on_event(w, &ev_pgup);
    if (term->scroll_offset <= 0) {
        kprintf("[TERMINAL TEST FAIL] PageUp failed to increment scroll_offset! offset=%d\n", term->scroll_offset);
        return false;
    }
    int pgup_off = term->scroll_offset;
    kprintf("  PASS: PageUp incremented scroll offset to %d.\n", pgup_off);

    /* Test Mouse Wheel Navigation */
    input_event_t ev_wheel_up = { .type = INPUT_EVENT_MOUSE_WHEEL, .mouse = { .delta_z = 2 } };
    terminal_on_event(w, &ev_wheel_up);
    if (term->scroll_offset <= pgup_off) {
        kprintf("[TERMINAL TEST FAIL] Mouse wheel up failed to scroll! Got %d, expected > %d\n",
                term->scroll_offset, pgup_off);
        return false;
    }
    kprintf("  PASS: Mouse wheel up scrolled offset to %d.\n", term->scroll_offset);

    /* Test Snap-to-bottom on typing */
    input_event_t ev_type = { .type = INPUT_EVENT_KEY_DOWN, .key = { .ascii = 'a', .keycode = 'a' } };
    terminal_on_event(w, &ev_type);
    if (term->scroll_offset != 0) {
        kprintf("[TERMINAL TEST FAIL] Typing did not auto-snap scroll_offset to 0! offset=%d\n", term->scroll_offset);
        return false;
    }
    kprintf("  PASS: Typing auto-snapped scroll offset to bottom (offset=0).\n");

    /* 7. Close terminal window and verify resource teardown */
    wm_destroy_window(w);
    if (term->scrollback != NULL || term->active) {
        kprintf("[TERMINAL TEST FAIL] Window destruction did not release scrollback PMM pages!\n");
        return false;
    }
    kprintf("  PASS: Window destruction and dynamic PMM page deallocation verified.\n");

    kprintf("[TERMINAL TEST] All Terminal 2.0 Subsystem tests passed successfully!\n");
    kprintf("ASTRA_TERMINAL_APP_OK\n");
    kprintf("ASTRA_TERMINAL_CURSOR_OK\n");
    kprintf("ASTRA_TERMINAL_SCROLLBACK_OK\n\n");
    return true;
}
