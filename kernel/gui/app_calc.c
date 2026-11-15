#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/mem.h>
#include <kernel/kprintf.h>

typedef struct {
    char display[32];
    s64 accumulator;
    char pending_op;
    bool clear_on_next_digit;
    bool has_error;
} calc_state_t;

static calc_state_t g_calc = {
    .display = "0",
    .accumulator = 0,
    .pending_op = 0,
    .clear_on_next_digit = false,
    .has_error = false
};

static wm_window_t *g_calc_win = NULL;

static void calc_reset(void) {
    g_calc.display[0] = '0';
    g_calc.display[1] = '\0';
    g_calc.accumulator = 0;
    g_calc.pending_op = 0;
    g_calc.clear_on_next_digit = false;
    g_calc.has_error = false;
}

static s64 calc_parse_display(void) {
    s64 val = 0;
    bool neg = false;
    int i = 0;
    if (g_calc.display[0] == '-') {
        neg = true;
        i = 1;
    }
    while (g_calc.display[i]) {
        if (g_calc.display[i] >= '0' && g_calc.display[i] <= '9') {
            val = val * 10 + (g_calc.display[i] - '0');
        }
        i++;
    }
    return neg ? -val : val;
}

static void calc_set_display(s64 val) {
    char buf[32];
    int pos = 0;
    bool neg = false;
    if (val < 0) {
        neg = true;
        val = -val;
    }
    if (val == 0) {
        buf[pos++] = '0';
    } else {
        while (val > 0 && pos < 28) {
            buf[pos++] = '0' + (val % 10);
            val /= 10;
        }
    }
    if (neg) buf[pos++] = '-';

    int d = 0;
    for (int i = pos - 1; i >= 0; i--) {
        g_calc.display[d++] = buf[i];
    }
    g_calc.display[d] = '\0';
}

static void calc_append_digit(char digit) {
    if (g_calc.has_error || g_calc.clear_on_next_digit) {
        g_calc.display[0] = digit;
        g_calc.display[1] = '\0';
        g_calc.clear_on_next_digit = false;
        g_calc.has_error = false;
        return;
    }

    size_t len = 0;
    while (g_calc.display[len]) len++;
    if (len == 1 && g_calc.display[0] == '0') {
        g_calc.display[0] = digit;
        g_calc.display[1] = '\0';
        return;
    }
    if (len < 14) {
        g_calc.display[len] = digit;
        g_calc.display[len + 1] = '\0';
    }
}

static void calc_evaluate(void) {
    if (g_calc.pending_op == 0) return;

    s64 current = calc_parse_display();
    s64 result = 0;

    switch (g_calc.pending_op) {
        case '+': result = g_calc.accumulator + current; break;
        case '-': result = g_calc.accumulator - current; break;
        case '*': result = g_calc.accumulator * current; break;
        case '/':
            if (current == 0) {
                g_calc.display[0] = 'E'; g_calc.display[1] = 'r'; g_calc.display[2] = 'r';
                g_calc.display[3] = 'o'; g_calc.display[4] = 'r'; g_calc.display[5] = '\0';
                g_calc.has_error = true;
                g_calc.pending_op = 0;
                return;
            }
            result = g_calc.accumulator / current;
            break;
        default: result = current; break;
    }

    calc_set_display(result);
    g_calc.accumulator = result;
    g_calc.pending_op = 0;
    g_calc.clear_on_next_digit = true;
}

static void calc_handle_action(const char *action) {
    if (!action || !*action) return;

    if (action[0] >= '0' && action[0] <= '9') {
        calc_append_digit(action[0]);
    } else if (action[0] == 'C') {
        calc_reset();
    } else if (action[0] == '+' || action[0] == '-' || action[0] == '*' || action[0] == '/') {
        if (g_calc.pending_op != 0 && !g_calc.clear_on_next_digit) {
            calc_evaluate();
        } else {
            g_calc.accumulator = calc_parse_display();
        }
        g_calc.pending_op = action[0];
        g_calc.clear_on_next_digit = true;
    } else if (action[0] == '=') {
        calc_evaluate();
    } else if (action[0] == 'N') {
        /* +/- negate */
        s64 v = calc_parse_display();
        calc_set_display(-v);
    }
}

static void calc_on_paint(wm_window_t *win, gfx_surface_t *s) {
    if (!win || !s) return;

    /* Sleek Industrial Slate Background */
    gfx_fill_rect(s, 0, 0, s->width, s->height, 0x0F172A);

    /* Recessed OLED Screen Display Box */
    gfx_fill_rounded_rect(s, 12, 10, s->width - 24, 46, 6, 0x060911);
    gfx_draw_rounded_rect(s, 12, 10, s->width - 24, 46, 6, 0x1E293B);

    /* Operation indicator pill */
    if (g_calc.pending_op) {
        char op_buf[4] = { '[', g_calc.pending_op, ']', '\0' };
        gfx_draw_string(s, 18, 14, op_buf, 0xEA580C, 0, true);
    }

    /* Right-aligned Crisp LCD Digits */
    size_t disp_len = 0;
    while (g_calc.display[disp_len]) disp_len++;
    int text_x = s->width - 28 - ((int)disp_len * 8 * 2);
    if (text_x < 24) text_x = 24;
    gfx_draw_string_scale(s, text_x, 18, g_calc.display, 0x34D399, 0, true, 2);

    /* Button Grid Layout with Distinct Visual Hierarchy */
    static const struct {
        const char *label;
        const char *action;
        int col, row, w_span;
        u32 color, bg_color, border_col;
    } buttons[] = {
        {"C",   "C", 0, 0, 1, 0xEF4444, 0x334155, 0x475569},
        {"+/-", "N", 1, 0, 1, 0xF1F5F9, 0x334155, 0x475569},
        {"%",   "%", 2, 0, 1, 0xF1F5F9, 0x334155, 0x475569},
        {"/",   "/", 3, 0, 1, 0xFFFFFF, 0xEA580C, 0xF97316},

        {"7",   "7", 0, 1, 1, 0xF8FAFC, 0x1E293B, 0x334155},
        {"8",   "8", 1, 1, 1, 0xF8FAFC, 0x1E293B, 0x334155},
        {"9",   "9", 2, 1, 1, 0xF8FAFC, 0x1E293B, 0x334155},
        {"*",   "*", 3, 1, 1, 0xFFFFFF, 0xEA580C, 0xF97316},

        {"4",   "4", 0, 2, 1, 0xF8FAFC, 0x1E293B, 0x334155},
        {"5",   "5", 1, 2, 1, 0xF8FAFC, 0x1E293B, 0x334155},
        {"6",   "6", 2, 2, 1, 0xF8FAFC, 0x1E293B, 0x334155},
        {"-",   "-", 3, 2, 1, 0xFFFFFF, 0xEA580C, 0xF97316},

        {"1",   "1", 0, 3, 1, 0xF8FAFC, 0x1E293B, 0x334155},
        {"2",   "2", 1, 3, 1, 0xF8FAFC, 0x1E293B, 0x334155},
        {"3",   "3", 2, 3, 1, 0xF8FAFC, 0x1E293B, 0x334155},
        {"+",   "+", 3, 3, 1, 0xFFFFFF, 0xEA580C, 0xF97316},

        {"0",   "0", 0, 4, 2, 0xF8FAFC, 0x1E293B, 0x334155},
        {".",   ".", 2, 4, 1, 0xF8FAFC, 0x1E293B, 0x334155},
        {"=",   "=", 3, 4, 1, 0xFFFFFF, 0x10B981, 0x34D399},
    };

    int btn_start_y = 68;
    int btn_w = 52;
    int btn_h = 42;
    int gap = 8;
    int start_x = 12;

    for (size_t i = 0; i < sizeof(buttons)/sizeof(buttons[0]); i++) {
        int bx = start_x + buttons[i].col * (btn_w + gap);
        int by = btn_start_y + buttons[i].row * (btn_h + gap);
        int bw = buttons[i].w_span == 2 ? (btn_w * 2 + gap) : btn_w;

        gfx_fill_rounded_rect(s, bx, by, bw, btn_h, 7, buttons[i].bg_color);
        gfx_draw_rounded_rect(s, bx, by, bw, btn_h, 7, buttons[i].border_col);

        /* Center label */
        size_t l_len = 0;
        while (buttons[i].label[l_len]) l_len++;
        int lx = bx + (bw - (int)l_len * 8) / 2;
        int ly = by + (btn_h - 16) / 2;
        gfx_draw_string(s, lx, ly, buttons[i].label, buttons[i].color, 0, true);
    }
}

static void calc_on_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return;

    if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        int cx = ev->mouse.x - win->client_x;
        int cy = ev->mouse.y - win->client_y;

        int btn_start_y = 66;
        int btn_w = 52;
        int btn_h = 42;
        int gap = 8;
        int start_x = 12;

        static const char *grid[5][4] = {
            {"C", "N", "%", "/"},
            {"7", "8", "9", "*"},
            {"4", "5", "6", "-"},
            {"1", "2", "3", "+"},
            {"0", "0", ".", "="}
        };

        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 4; c++) {
                int bx = start_x + c * (btn_w + gap);
                int by = btn_start_y + r * (btn_h + gap);
                int bw = (r == 4 && c == 0) ? (btn_w * 2 + gap) : btn_w;

                if (cx >= bx && cx <= bx + bw && cy >= by && cy <= by + btn_h) {
                    calc_handle_action(grid[r][c]);
                    wm_render();
                    gfx_flip();
                    return;
                }
            }
        }
    } else if (ev->type == INPUT_EVENT_KEY_DOWN) {
        char k = (char)ev->key.ascii;
        if (k >= '0' && k <= '9') {
            calc_append_digit(k);
        } else if (k == '+' || k == '-' || k == '*' || k == '/') {
            char act[2] = {k, 0};
            calc_handle_action(act);
        } else if (k == '\n' || k == '\r' || k == '=') {
            calc_handle_action("=");
        } else if (k == 27 || k == 'c' || k == 'C') {
            calc_handle_action("C");
        }
        wm_render();
        gfx_flip();
    }
}

void app_calc_launch(void) {
    if (g_calc_win && wm_find_by_id(g_calc_win->id)) {
        wm_bring_to_front(g_calc_win);
        wm_set_focus(g_calc_win);
        return;
    }

    int win_w = 264;
    int win_h = 330;
    gfx_surface_t *bb = gfx_get_backbuffer();
    int sx = bb ? (bb->width - win_w) / 2 : 250;
    int sy = bb ? (bb->height - win_h) / 2 : 180;

    g_calc_win = wm_create_window("Calculator", sx, sy, win_w, win_h);
    if (!g_calc_win) return;

    g_calc_win->on_paint = calc_on_paint;
    g_calc_win->on_event = calc_on_event;
    wm_bring_to_front(g_calc_win);
    wm_set_focus(g_calc_win);
}

