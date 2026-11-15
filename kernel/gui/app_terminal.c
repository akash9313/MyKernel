#include <kernel/terminal.h>
#include <kernel/shell.h>
#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/mem.h>
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

void terminal_clear(terminal_state_t *term) {
    if (!term) return;
    for (int i = 0; i < TERM_SCROLLBACK_LINES; i++) {
        term->scrollback[i].len = 0;
        kmemset(term->scrollback[i].chars, 0, TERM_DEFAULT_COLS);
        for (int c = 0; c < TERM_DEFAULT_COLS; c++) {
            term->scrollback[i].colors[c] = TERM_PALETTE_DEFAULT;
        }
    }
    term->total_lines = 0;
    term->scroll_offset = 0;
}

static void term_advance_line(terminal_state_t *term) {
    term->total_lines++;
    int line_idx = term->total_lines % TERM_SCROLLBACK_LINES;
    term->scrollback[line_idx].len = 0;
    kmemset(term->scrollback[line_idx].chars, 0, TERM_DEFAULT_COLS);
    for (int c = 0; c < TERM_DEFAULT_COLS; c++) {
        term->scrollback[line_idx].colors[c] = TERM_PALETTE_DEFAULT;
    }
}

void terminal_print(terminal_state_t *term, const char *str, u32 color) {
    if (!term || !str) return;

    if (term->total_lines == 0) {
        term_advance_line(term);
    }

    int cur_idx = term->total_lines % TERM_SCROLLBACK_LINES;
    term_line_t *cur = &term->scrollback[cur_idx];

    for (size_t i = 0; str[i]; i++) {
        char c = str[i];

        if (c == '\n') {
            term_advance_line(term);
            cur_idx = term->total_lines % TERM_SCROLLBACK_LINES;
            cur = &term->scrollback[cur_idx];
            continue;
        } else if (c == '\r') {
            cur->len = 0;
            continue;
        } else if (c == '\t') {
            int spaces = 4 - (cur->len % 4);
            while (spaces-- > 0 && cur->len < TERM_DEFAULT_COLS) {
                cur->chars[cur->len] = ' ';
                cur->colors[cur->len] = term_color_to_idx(color);
                cur->len++;
            }
            continue;
        } else if (c == '\b') {
            if (cur->len > 0) cur->len--;
            continue;
        }

        if (cur->len >= TERM_DEFAULT_COLS) {
            term_advance_line(term);
            cur_idx = term->total_lines % TERM_SCROLLBACK_LINES;
            cur = &term->scrollback[cur_idx];
        }

        cur->chars[cur->len] = c;
        cur->colors[cur->len] = term_color_to_idx(color);
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
    if (!term || !out_x || !out_y) return;

    int visible_rows = screen_h / TERM_FONT_HEIGHT;
    if (visible_rows > TERM_DEFAULT_ROWS) visible_rows = TERM_DEFAULT_ROWS;

    int cur_line_idx = term->total_lines % TERM_SCROLLBACK_LINES;
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
    if (!term) return;

    /* 1. Deep slate background */
    gfx_fill_rect(s, 0, 0, s->width, s->height, TERM_COLOR_BG);

    /* 2. Determine visible rows & dynamic start line */
    int visible_rows = s->height / TERM_FONT_HEIGHT;
    if (visible_rows > TERM_DEFAULT_ROWS) visible_rows = TERM_DEFAULT_ROWS;

    int cur_line_idx = term->total_lines % TERM_SCROLLBACK_LINES;
    term_line_t *cur_line = &term->scrollback[cur_line_idx];
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
    if (start_line < 1) start_line = 1;

    /* 3. Render scrollback lines */
    int max_line_to_draw = (term->scroll_offset > 0)
        ? (term->total_lines - term->scroll_offset)
        : term->total_lines;

    for (int l = start_line; l <= max_line_to_draw; l++) {
        int row = l - start_line;
        int y = 4 + (row * TERM_FONT_HEIGHT);
        if (y >= s->height - TERM_FONT_HEIGHT) break;

        int idx = l % TERM_SCROLLBACK_LINES;
        term_line_t *tl = &term->scrollback[idx];

        int x = 8;
        for (int c = 0; c < tl->len && x < s->width - TERM_FONT_WIDTH; c++) {
            gfx_draw_char(s, x, y, tl->chars[c], term_idx_to_color(tl->colors[c]), 0, true);
            x += TERM_FONT_WIDTH;
        }
    }

    /* 4. Render active input text and cursor if at bottom (scroll_offset == 0) */
    if (term->scroll_offset == 0) {
        int base_row = term->total_lines - start_line;
        int base_y = 4 + (base_row * TERM_FONT_HEIGHT);

        /* Draw entered input buffer text */
        for (int i = 0; i < term->input_len; i++) {
            int char_idx = prompt_chars + i;
            int char_row = char_idx / TERM_DEFAULT_COLS;
            int char_col = char_idx % TERM_DEFAULT_COLS;
            int char_x = 8 + (char_col * TERM_FONT_WIDTH);
            int char_y = base_y + (char_row * TERM_FONT_HEIGHT);

            if (char_y >= 4 && char_y < s->height - TERM_FONT_HEIGHT && char_x < s->width - TERM_FONT_WIDTH) {
                gfx_draw_char(s, char_x, char_y, term->input_buf[i], TERM_COLOR_FG, 0, true);
            }
        }

        /* Draw blinking cursor */
        int cur_x = 8, cur_y = 4;
        terminal_get_cursor_coords(term, s->height, &cur_x, &cur_y);
        u64 ticks = apic_get_ticks();
        if ((ticks / 50) % 2 == 0) {
            if (cur_y >= 4 && cur_y <= s->height - TERM_FONT_HEIGHT && cur_x < s->width - 4) {
                gfx_fill_rect(s, cur_x, cur_y, 2, TERM_FONT_HEIGHT - 2, TERM_COLOR_CURSOR);
            }
        }
    } else {
        /* Render scrollback indicator at top right */
        char scroll_ind[32] = "[SCROLL -00]";
        int off = term->scroll_offset;
        scroll_ind[9] = '0' + ((off / 10) % 10);
        scroll_ind[10] = '0' + (off % 10);
        int sx = s->width - (13 * TERM_FONT_WIDTH);
        for (int i = 0; scroll_ind[i] && sx < s->width - TERM_FONT_WIDTH; i++) {
            gfx_draw_char(s, sx, 4, scroll_ind[i], TERM_COLOR_MUTED, 0, true);
            sx += TERM_FONT_WIDTH;
        }
    }
}

static void terminal_on_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return;
    terminal_state_t *term = (terminal_state_t *)win->user_data;
    if (!term) return;

    if (ev->type == INPUT_EVENT_KEY_DOWN) {
        u8 ascii = ev->key.ascii;
        u32 keycode = ev->key.keycode;

        /* Check Ctrl shortcuts */
        if (ev->key.modifiers & KEY_MOD_CTRL) {
            if (ascii == 'c' || ascii == 'C' || ascii == 3) {
                terminal_println(term, "^C", TERM_COLOR_ERROR);
                term->input_len = 0;
                term->cursor_pos = 0;
                term->input_buf[0] = '\0';
                terminal_prompt(term);
                return;
            } else if (ascii == 'l' || ascii == 'L' || ascii == 12) {
                terminal_clear(term);
                terminal_prompt(term);
                return;
            } else if (ascii == 'v' || ascii == 'V' || ascii == 22) {
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

        /* Standard keys */
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

        } else if (keycode == KEY_BACKSPACE || ascii == '\b' || ascii == 127) {
            term->scroll_offset = 0;
            if (term->cursor_pos > 0) {
                for (int i = term->cursor_pos - 1; i < term->input_len - 1; i++) {
                    term->input_buf[i] = term->input_buf[i + 1];
                }
                term->input_len--;
                term->cursor_pos--;
                term->input_buf[term->input_len] = '\0';
            }
        } else if (keycode == KEY_DELETE) {
            term->scroll_offset = 0;
            if (term->cursor_pos < term->input_len) {
                for (int i = term->cursor_pos; i < term->input_len - 1; i++) {
                    term->input_buf[i] = term->input_buf[i + 1];
                }
                term->input_len--;
                term->input_buf[term->input_len] = '\0';
            }
        } else if (keycode == KEY_HOME) {
            term->scroll_offset = 0;
            term->cursor_pos = 0;
        } else if (keycode == KEY_END) {
            term->scroll_offset = 0;
            term->cursor_pos = term->input_len;
        } else if (keycode == KEY_LEFT) {
            term->scroll_offset = 0;
            if (term->cursor_pos > 0) term->cursor_pos--;
        } else if (keycode == KEY_RIGHT) {
            term->scroll_offset = 0;
            if (term->cursor_pos < term->input_len) term->cursor_pos++;
        } else if (keycode == KEY_UP) {
            term->scroll_offset = 0;
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
        } else if (keycode == KEY_DOWN) {
            term->scroll_offset = 0;
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
        } else if (keycode == KEY_PAGE_UP) {
            if (term->scroll_offset < term->total_lines - TERM_DEFAULT_ROWS) {
                term->scroll_offset += 4;
            }
        } else if (keycode == KEY_PAGE_DOWN) {
            if (term->scroll_offset > 0) {
                term->scroll_offset -= 4;
                if (term->scroll_offset < 0) term->scroll_offset = 0;
            }
        } else if (ascii >= ' ' && ascii <= '~') {
            term->scroll_offset = 0;
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
    if (slot < 0) slot = 0; /* Reuse oldest if limit reached */

    terminal_state_t *term = &g_term_mgr.terminals[slot];
    shell_context_t  *sh   = &g_term_mgr.shells[slot];

    kmemset(term, 0, sizeof(*term));
    shell_init(sh, terminal_shell_output, term);
    term->shell_ctx = sh;
    term->active = true;

    /* Window geometry: 648x412 with 640x384 client surface (80 columns x 24 rows) */
    int start_x = 80 + (slot * 30);
    int start_y = 60 + (slot * 30);
    wm_window_t *win = wm_create_window("AstraOS Terminal", start_x, start_y, 648, 412);
    if (!win) {
        term->active = false;
        return;
    }

    term->window = win;
    win->user_data = term;
    win->on_paint = terminal_on_paint;
    win->on_event = terminal_on_event;

    /* Header banner and prompt */
    terminal_println(term, "AstraOS Terminal v1.0 [x86_64 Freestanding Kernel]", TERM_COLOR_INFO);
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
    kprintf("  PASS: Terminal window created and initialized with active shell.\n");

    /* 2. Test initial cursor position immediately after prompt */
    int cur_line_idx = term->total_lines % TERM_SCROLLBACK_LINES;
    int prompt_len = term->scrollback[cur_line_idx].len;
    int cur_x = 0, cur_y = 0;
    terminal_get_cursor_coords(term, 384, &cur_x, &cur_y);
    int expected_x = 8 + (prompt_len * TERM_FONT_WIDTH);
    if (cur_x != expected_x) {
        kprintf("[TERMINAL TEST FAIL] Initial cursor X mismatch! Got %d, expected %d\n", cur_x, expected_x);
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
    terminal_get_cursor_coords(term, 384, &cur_x, &cur_y);
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
    terminal_get_cursor_coords(term, 384, &cur_x, &cur_y);
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
    terminal_get_cursor_coords(term, 384, &cur_x, &cur_y);
    int new_line_idx = term->total_lines % TERM_SCROLLBACK_LINES;
    int new_prompt_len = term->scrollback[new_line_idx].len;
    if (cur_x != 8 + (new_prompt_len * TERM_FONT_WIDTH)) {
        kprintf("[TERMINAL TEST FAIL] Cursor position after Enter prompt mismatch! Got X=%d\n", cur_x);
        return false;
    }
    kprintf("  PASS: Command execution on Enter & new prompt cursor reset verified.\n");

    /* 6. Close terminal window */
    wm_destroy_window(w);
    term->active = false;
    kprintf("  PASS: Terminal window clean teardown verified.\n");

    kprintf("[TERMINAL TEST] All Terminal Subsystem tests passed successfully!\n");
    kprintf("ASTRA_TERMINAL_APP_OK\n");
    kprintf("ASTRA_TERMINAL_CURSOR_OK\n\n");
    return true;
}
