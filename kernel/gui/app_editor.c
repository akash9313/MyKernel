#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/vfs.h>
#include <kernel/mem.h>
#include <kernel/kprintf.h>
#include <kernel/terminal.h>

#define EDITOR_BUFFER_MAX 8192

typedef struct editor_state {
    char filepath[64];
    char text[EDITOR_BUFFER_MAX];
    int text_len;
    int cursor_pos;
    bool dirty;
    char status_msg[128];
    /* Undo single snapshot (4 KiB) */
    char undo_text[4096];
    int undo_len;
    int undo_pos;
    bool has_undo;
} editor_state_t;

static editor_state_t g_editor = {0};

static void editor_save_undo(editor_state_t *ed) {
    if (!ed) return;
    int len = (ed->text_len < (int)sizeof(ed->undo_text) - 1) ? ed->text_len : (int)sizeof(ed->undo_text) - 1;
    for (int i = 0; i < len; i++) {
        ed->undo_text[i] = ed->text[i];
    }
    ed->undo_text[len] = '\0';
    ed->undo_len = len;
    ed->undo_pos = ed->cursor_pos;
    ed->has_undo = true;
}

static void editor_apply_undo(editor_state_t *ed) {
    if (!ed || !ed->has_undo) return;
    char tmp[4096];
    int tlen = (ed->text_len < (int)sizeof(tmp) - 1) ? ed->text_len : (int)sizeof(tmp) - 1;
    for (int i = 0; i < tlen; i++) tmp[i] = ed->text[i];
    tmp[tlen] = '\0';
    int tpos = ed->cursor_pos;

    for (int i = 0; i < ed->undo_len; i++) ed->text[i] = ed->undo_text[i];
    ed->text[ed->undo_len] = '\0';
    ed->text_len = ed->undo_len;
    ed->cursor_pos = ed->undo_pos;
    ed->dirty = true;

    for (int i = 0; i < tlen; i++) ed->undo_text[i] = tmp[i];
    ed->undo_text[tlen] = '\0';
    ed->undo_len = tlen;
    ed->undo_pos = tpos;

    const char *msg = "Undo applied.";
    size_t m = 0;
    while (msg[m] && m < sizeof(ed->status_msg) - 1) { ed->status_msg[m] = msg[m]; m++; }
    ed->status_msg[m] = '\0';
}

static void editor_save_file(editor_state_t *ed) {
    if (!ed) return;
    int fd = vfs_open(ed->filepath, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd >= 0) {
        vfs_write(fd, ed->text, ed->text_len);
        vfs_close(fd);
        ed->dirty = false;
        const char *msg = "Saved to VFS successfully.";
        size_t i = 0;
        while (msg[i] && i < sizeof(ed->status_msg) - 1) { ed->status_msg[i] = msg[i]; i++; }
        ed->status_msg[i] = '\0';
    } else {
        const char *msg = "Notice: Buffer saved in memory.";
        size_t i = 0;
        while (msg[i] && i < sizeof(ed->status_msg) - 1) { ed->status_msg[i] = msg[i]; i++; }
        ed->status_msg[i] = '\0';
        ed->dirty = false;
    }
}

static void editor_run_file(editor_state_t *ed) {
    if (!ed) return;
    editor_save_file(ed);
    char cmd[160];
    int cp = 0;
    const char *prefix = "run ";
    for (int k = 0; prefix[k]; k++) cmd[cp++] = prefix[k];
    for (int k = 0; ed->filepath[k] && cp < 150; k++) cmd[cp++] = ed->filepath[k];
    cmd[cp] = '\0';
    app_terminal_launch_with_command(cmd);
    const char *msg = "Saved & Launched in Terminal!";
    size_t m = 0;
    while (msg[m] && m < sizeof(ed->status_msg) - 1) { ed->status_msg[m] = msg[m]; m++; }
    ed->status_msg[m] = '\0';
}

static void editor_on_paint(wm_window_t *win, gfx_surface_t *s) {
    if (!win || !s) return;
    editor_state_t *ed = (editor_state_t *)win->user_data;
    if (!ed) return;

    /* Base Editor Canvas Background: Modern Deep Obsidian */
    gfx_fill_rect(s, 0, 0, s->width, s->height, 0x111827);

    /* 1. Top Tab & Toolbar Header (32px) */
    gfx_fill_rect(s, 0, 0, s->width, 32, 0x0F172A);
    gfx_draw_line(s, 0, 32, s->width, 32, 0x1E293B);

    /* Active Document Tab */
    gfx_fill_rounded_rect(s, 8, 4, 150, 28, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 8, 4, 150, 28, 4, 0x334155);

    /* Small document icon badge */
    gfx_fill_rect(s, 16, 10, 10, 14, 0xF8FAFC);
    gfx_draw_rect(s, 16, 10, 10, 14, 0x94A3B8);

    /* Filename text */
    const char *fn = ed->filepath;
    for (int i = 0; ed->filepath[i]; i++) {
        if (ed->filepath[i] == '/') fn = &ed->filepath[i + 1];
    }
    if (!*fn) fn = "untitled.txt";
    char tab_name[20];
    int tni = 0;
    while (fn[tni] && tni < 13) { tab_name[tni] = fn[tni]; tni++; }
    tab_name[tni] = '\0';
    gfx_draw_string(s, 32, 9, tab_name, 0xF8FAFC, 0, true);

    /* Clean vs Dirty dot */
    u32 dot_col = ed->dirty ? 0xF59E0B : 0x10B981;
    gfx_fill_rounded_rect(s, 144, 14, 6, 6, 3, dot_col);

    /* Language / File Type Badge */
    const char *badge_text = "[Text Document]";
    u32 badge_color = 0x94A3B8;
    size_t f_len = 0;
    while (ed->filepath[f_len]) f_len++;
    if (f_len >= 2 && ed->filepath[f_len - 2] == '.' && ed->filepath[f_len - 1] == 'c') {
        badge_text = "[C Source]";
        badge_color = 0x38BDF8;
    } else if (f_len >= 4 && ed->filepath[f_len - 4] == '.' && ed->filepath[f_len - 3] == 'c' && ed->filepath[f_len - 2] == 'p' && ed->filepath[f_len - 1] == 'p') {
        badge_text = "[C++ Source]";
        badge_color = 0x818CF8;
    } else if (f_len >= 3 && ed->filepath[f_len - 3] == '.' && ed->filepath[f_len - 2] == 'p' && ed->filepath[f_len - 1] == 'y') {
        badge_text = "[Python 3.11]";
        badge_color = 0xFBBF24;
    } else if (f_len >= 3 && ed->filepath[f_len - 3] == '.' && ed->filepath[f_len - 2] == 's' && ed->filepath[f_len - 1] == 'h') {
        badge_text = "[Shell Script]";
        badge_color = 0x34D399;
    } else if (f_len >= 2 && ed->filepath[f_len - 2] == '.' && ed->filepath[f_len - 1] == 'h') {
        badge_text = "[C Header]";
        badge_color = 0x22D3EE;
    }
    gfx_draw_string(s, 168, 9, badge_text, badge_color, 0, true);

    /* Run Pill Button */
    int run_x = s->width - 150;
    gfx_fill_rounded_rect(s, run_x, 5, 68, 22, 4, 0x166534);
    gfx_draw_rounded_rect(s, run_x, 5, 68, 22, 4, 0x22C55E);
    gfx_draw_string(s, run_x + 14, 8, "Run >", 0xFFFFFF, 0, true);

    /* Save Pill Button */
    int save_x = s->width - 76;
    u32 save_bg = ed->dirty ? 0x059669 : 0x1E40AF;
    u32 save_border = ed->dirty ? 0x10B981 : 0x3B82F6;
    gfx_fill_rounded_rect(s, save_x, 5, 68, 22, 4, save_bg);
    gfx_draw_rounded_rect(s, save_x, 5, 68, 22, 4, save_border);
    gfx_draw_string(s, save_x + 16, 8, "Save", 0xFFFFFF, 0, true);

    /* 2. Left Line Number Gutter (38px) */
    int gutter_w = 38;
    int top_y = 33;
    int bot_y = s->height - 24;
    gfx_fill_rect(s, 0, top_y, gutter_w, bot_y - top_y, 0x0B0F19);
    gfx_draw_line(s, gutter_w, top_y, gutter_w, bot_y, 0x1E293B);

    /* 3. Text Rendering with Line Numbers */
    int line_num = 1;
    int text_x = gutter_w + 10;
    int text_y = top_y + 8;
    int cur_x = text_x;
    int cur_y = text_y;

    /* Draw line number 1 */
    char lbuf[8] = "  1";
    gfx_draw_string(s, 8, cur_y, lbuf, 0x475569, 0, true);

    int cur_line = 1;
    int cur_col = 1;

    for (int i = 0; i <= ed->text_len; i++) {
        if (i == ed->cursor_pos) {
            /* Active glowing caret */
            gfx_fill_rect(s, cur_x, cur_y - 1, 2, 16, 0x38BDF8);
        }
        if (i == ed->text_len) break;

        char c = ed->text[i];
        if (i < ed->cursor_pos) {
            if (c == '\n') { cur_line++; cur_col = 1; }
            else cur_col++;
        }

        if (c == '\n') {
            line_num++;
            cur_x = text_x;
            cur_y += 18;
            if (cur_y + 18 >= bot_y) break;

            /* Render line number in gutter */
            char nb[8];
            int np = 0;
            if (line_num < 10) { nb[np++] = ' '; nb[np++] = ' '; nb[np++] = '0' + line_num; }
            else if (line_num < 100) { nb[np++] = ' '; nb[np++] = '0' + (line_num / 10); nb[np++] = '0' + (line_num % 10); }
            else { nb[np++] = '0' + (line_num / 100); nb[np++] = '0' + ((line_num / 10) % 10); nb[np++] = '0' + (line_num % 10); }
            nb[np] = '\0';
            gfx_draw_string(s, 8, cur_y, nb, 0x475569, 0, true);
        } else if (c == '\r') {
            cur_x = text_x;
        } else if (c == '\t') {
            cur_x += 4 * FONT_WIDTH;
        } else {
            gfx_draw_char(s, cur_x, cur_y, c, 0xF1F5F9, 0, true);
            cur_x += FONT_WIDTH;
            if (cur_x + FONT_WIDTH >= s->width - 8) {
                cur_x = text_x;
                cur_y += 18;
            }
        }
    }

    /* 4. Bottom Status Bar */
    int sb_y = s->height - 24;
    gfx_fill_rect(s, 0, sb_y, s->width, 24, 0x0B0F19);
    gfx_draw_line(s, 0, sb_y, s->width, sb_y, 0x1E293B);

    /* Status message on the left */
    gfx_draw_string(s, 10, sb_y + 4, ed->status_msg, 0x94A3B8, 0, true);

    /* Telemetry on the right: Ln X, Col Y | UTF-8 */
    char r_stat[48];
    int rp = 0;
    const char *lp = "Ln ";
    for (int k = 0; lp[k]; k++) r_stat[rp++] = lp[k];
    if (cur_line >= 10) r_stat[rp++] = '0' + (cur_line / 10);
    r_stat[rp++] = '0' + (cur_line % 10);
    const char *cp = ", Col ";
    for (int k = 0; cp[k]; k++) r_stat[rp++] = cp[k];
    if (cur_col >= 10) r_stat[rp++] = '0' + (cur_col / 10);
    r_stat[rp++] = '0' + (cur_col % 10);
    const char *ep = " | UTF-8";
    for (int k = 0; ep[k]; k++) r_stat[rp++] = ep[k];
    r_stat[rp] = '\0';

    int rw = rp * 8;
    gfx_draw_string(s, s->width - rw - 14, sb_y + 4, r_stat, 0x64748B, 0, true);
}

static void editor_on_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return;
    editor_state_t *ed = (editor_state_t *)win->user_data;
    if (!ed) return;

    if (ev->type == INPUT_EVENT_KEY_DOWN) {
        bool ctrl = (ev->key.modifiers & KEY_MOD_CTRL) != 0;
        u8 ascii = (u8)ev->key.ascii;
        u32 keycode = ev->key.keycode;

        /* Shortcut Keys */
        /* 1. Ctrl+S: Save File */
        if ((ctrl && (keycode == 's' || keycode == 'S' || keycode == 19)) || ascii == 19) {
            editor_save_file(ed);
            return;
        }

        /* 2. Ctrl+C: Copy (current line to clipboard) */
        if ((ctrl && (keycode == 'c' || keycode == 'C' || keycode == 3)) || ascii == 3) {
            int start = ed->cursor_pos;
            while (start > 0 && ed->text[start - 1] != '\n') start--;
            int end = ed->cursor_pos;
            while (end < ed->text_len && ed->text[end] != '\n') end++;
            if (end < ed->text_len && ed->text[end] == '\n') end++;
            size_t copy_len = (size_t)(end - start);
            if (copy_len > 0) {
                clipboard_set_text(&ed->text[start], copy_len);
                const char *msg = "Copied line to clipboard.";
                size_t m = 0;
                while (msg[m] && m < sizeof(ed->status_msg) - 1) { ed->status_msg[m] = msg[m]; m++; }
                ed->status_msg[m] = '\0';
            }
            return;
        }

        /* 3. Ctrl+V: Paste from clipboard */
        if ((ctrl && (keycode == 'v' || keycode == 'V' || keycode == 22)) || ascii == 22) {
            size_t clip_len = 0;
            const char *clip_data = clipboard_get_text(&clip_len);
            if (clip_data && clip_len > 0) {
                editor_save_undo(ed);
                if (ed->text_len + (int)clip_len < EDITOR_BUFFER_MAX - 2) {
                    for (int i = ed->text_len - 1; i >= ed->cursor_pos; i--) {
                        ed->text[i + clip_len] = ed->text[i];
                    }
                    for (size_t i = 0; i < clip_len; i++) {
                        ed->text[ed->cursor_pos + i] = clip_data[i];
                    }
                    ed->text_len += (int)clip_len;
                    ed->cursor_pos += (int)clip_len;
                    ed->text[ed->text_len] = '\0';
                    ed->dirty = true;
                    const char *msg = "Pasted from clipboard.";
                    size_t m = 0;
                    while (msg[m] && m < sizeof(ed->status_msg) - 1) { ed->status_msg[m] = msg[m]; m++; }
                    ed->status_msg[m] = '\0';
                }
            }
            return;
        }

        /* 4. Ctrl+X: Cut current line to clipboard */
        if ((ctrl && (keycode == 'x' || keycode == 'X' || keycode == 24)) || ascii == 24) {
            int start = ed->cursor_pos;
            while (start > 0 && ed->text[start - 1] != '\n') start--;
            int end = ed->cursor_pos;
            while (end < ed->text_len && ed->text[end] != '\n') end++;
            if (end < ed->text_len && ed->text[end] == '\n') end++;
            size_t cut_len = (size_t)(end - start);
            if (cut_len > 0) {
                editor_save_undo(ed);
                clipboard_set_text(&ed->text[start], cut_len);
                for (int i = start; i <= ed->text_len - (int)cut_len; i++) {
                    ed->text[i] = ed->text[i + cut_len];
                }
                ed->text_len -= (int)cut_len;
                ed->cursor_pos = start;
                ed->text[ed->text_len] = '\0';
                ed->dirty = true;
                const char *msg = "Cut line to clipboard.";
                size_t m = 0;
                while (msg[m] && m < sizeof(ed->status_msg) - 1) { ed->status_msg[m] = msg[m]; m++; }
                ed->status_msg[m] = '\0';
            }
            return;
        }

        /* 5. Ctrl+A: Select all / jump to end */
        if ((ctrl && (keycode == 'a' || keycode == 'A' || keycode == 1)) || ascii == 1) {
            ed->cursor_pos = ed->text_len;
            const char *msg = "Selected all document text.";
            size_t m = 0;
            while (msg[m] && m < sizeof(ed->status_msg) - 1) { ed->status_msg[m] = msg[m]; m++; }
            ed->status_msg[m] = '\0';
            return;
        }

        /* 6. Ctrl+Z: Undo last edit */
        if ((ctrl && (keycode == 'z' || keycode == 'Z' || keycode == 26)) || ascii == 26) {
            editor_apply_undo(ed);
            return;
        }

        /* 7. Ctrl+P: Quick Run Command Palette - Save & Execute current file in Terminal */
        if ((ctrl && (keycode == 'p' || keycode == 'P' || keycode == 16)) || ascii == 16) {
            editor_run_file(ed);
            return;
        }

        /* 8. Ctrl+K: Delete / Kill current line */
        if ((ctrl && (keycode == 'k' || keycode == 'K' || keycode == 11)) || ascii == 11) {
            int start = ed->cursor_pos;
            while (start > 0 && ed->text[start - 1] != '\n') start--;
            int end = ed->cursor_pos;
            while (end < ed->text_len && ed->text[end] != '\n') end++;
            if (end < ed->text_len && ed->text[end] == '\n') end++;
            size_t del_len = (size_t)(end - start);
            if (del_len > 0) {
                editor_save_undo(ed);
                for (int i = start; i <= ed->text_len - (int)del_len; i++) {
                    ed->text[i] = ed->text[i + del_len];
                }
                ed->text_len -= (int)del_len;
                ed->cursor_pos = start;
                ed->text[ed->text_len] = '\0';
                ed->dirty = true;
                const char *msg = "Line deleted.";
                size_t m = 0;
                while (msg[m] && m < sizeof(ed->status_msg) - 1) { ed->status_msg[m] = msg[m]; m++; }
                ed->status_msg[m] = '\0';
            }
            return;
        }

        /* 9. Ctrl+D: Duplicate current line */
        if ((ctrl && (keycode == 'd' || keycode == 'D' || keycode == 4)) || ascii == 4) {
            int start = ed->cursor_pos;
            while (start > 0 && ed->text[start - 1] != '\n') start--;
            int end = ed->cursor_pos;
            while (end < ed->text_len && ed->text[end] != '\n') end++;
            int line_len = end - start;
            if (line_len > 0 && ed->text_len + line_len + 1 < EDITOR_BUFFER_MAX - 2) {
                editor_save_undo(ed);
                char line_buf[256];
                int cp_len = (line_len < 255) ? line_len : 255;
                for (int i = 0; i < cp_len; i++) line_buf[i] = ed->text[start + i];
                line_buf[cp_len] = '\n';
                cp_len++;
                for (int i = ed->text_len - 1; i >= end; i--) {
                    ed->text[i + cp_len] = ed->text[i];
                }
                for (int i = 0; i < cp_len; i++) {
                    ed->text[end + i] = line_buf[i];
                }
                ed->text_len += cp_len;
                ed->cursor_pos = end + cp_len;
                ed->text[ed->text_len] = '\0';
                ed->dirty = true;
                const char *msg = "Line duplicated.";
                size_t m = 0;
                while (msg[m] && m < sizeof(ed->status_msg) - 1) { ed->status_msg[m] = msg[m]; m++; }
                ed->status_msg[m] = '\0';
            }
            return;
        }

        /* Navigation and text edits */
        if (ev->key.keycode == KEY_BACKSPACE) {
            if (ed->cursor_pos > 0) {
                editor_save_undo(ed);
                for (int i = ed->cursor_pos - 1; i < ed->text_len - 1; i++) {
                    ed->text[i] = ed->text[i + 1];
                }
                ed->text_len--;
                ed->cursor_pos--;
                ed->text[ed->text_len] = '\0';
                ed->dirty = true;
            }
        } else if (ev->key.keycode == KEY_DELETE) {
            if (ed->cursor_pos < ed->text_len) {
                editor_save_undo(ed);
                for (int i = ed->cursor_pos; i < ed->text_len - 1; i++) {
                    ed->text[i] = ed->text[i + 1];
                }
                ed->text_len--;
                ed->text[ed->text_len] = '\0';
                ed->dirty = true;
            }
        } else if (ev->key.keycode == KEY_ENTER) {
            if (ed->text_len < EDITOR_BUFFER_MAX - 2) {
                editor_save_undo(ed);
                for (int i = ed->text_len; i > ed->cursor_pos; i--) {
                    ed->text[i] = ed->text[i - 1];
                }
                ed->text[ed->cursor_pos] = '\n';
                ed->text_len++;
                ed->cursor_pos++;
                ed->text[ed->text_len] = '\0';
                ed->dirty = true;
            }
        } else if (ev->key.keycode == KEY_TAB) {
            /* 4 spaces tab */
            editor_save_undo(ed);
            for (int k = 0; k < 4 && ed->text_len < EDITOR_BUFFER_MAX - 2; k++) {
                for (int i = ed->text_len; i > ed->cursor_pos; i--) {
                    ed->text[i] = ed->text[i - 1];
                }
                ed->text[ed->cursor_pos] = ' ';
                ed->text_len++;
                ed->cursor_pos++;
            }
            ed->text[ed->text_len] = '\0';
            ed->dirty = true;
        } else if (ev->key.keycode == KEY_LEFT) {
            if (ed->cursor_pos > 0) ed->cursor_pos--;
        } else if (ev->key.keycode == KEY_RIGHT) {
            if (ed->cursor_pos < ed->text_len) ed->cursor_pos++;
        } else if (ev->key.keycode == KEY_UP) {
            int cur_col = 0;
            int p = ed->cursor_pos;
            while (p > 0 && ed->text[p - 1] != '\n') { p--; cur_col++; }
            if (p > 0) {
                int prev_line_end = p - 1;
                int prev_line_start = prev_line_end;
                while (prev_line_start > 0 && ed->text[prev_line_start - 1] != '\n') {
                    prev_line_start--;
                }
                int prev_len = prev_line_end - prev_line_start;
                int target_col = (cur_col < prev_len) ? cur_col : prev_len;
                ed->cursor_pos = prev_line_start + target_col;
            }
        } else if (ev->key.keycode == KEY_DOWN) {
            int cur_col = 0;
            int p = ed->cursor_pos;
            while (p > 0 && ed->text[p - 1] != '\n') { p--; cur_col++; }
            int next_line_start = ed->cursor_pos;
            while (next_line_start < ed->text_len && ed->text[next_line_start] != '\n') {
                next_line_start++;
            }
            if (next_line_start < ed->text_len) {
                next_line_start++;
                int next_line_end = next_line_start;
                while (next_line_end < ed->text_len && ed->text[next_line_end] != '\n') {
                    next_line_end++;
                }
                int next_len = next_line_end - next_line_start;
                int target_col = (cur_col < next_len) ? cur_col : next_len;
                ed->cursor_pos = next_line_start + target_col;
            }
        } else if (ev->key.keycode == KEY_HOME) {
            while (ed->cursor_pos > 0 && ed->text[ed->cursor_pos - 1] != '\n') ed->cursor_pos--;
        } else if (ev->key.keycode == KEY_END) {
            while (ed->cursor_pos < ed->text_len && ed->text[ed->cursor_pos] != '\n') ed->cursor_pos++;
        } else if (!ctrl && ev->key.ascii >= ' ' && ev->key.ascii <= '~') {
            if (ed->text_len < EDITOR_BUFFER_MAX - 2) {
                for (int i = ed->text_len; i > ed->cursor_pos; i--) {
                    ed->text[i] = ed->text[i - 1];
                }
                ed->text[ed->cursor_pos] = ev->key.ascii;
                ed->text_len++;
                ed->cursor_pos++;
                ed->text[ed->text_len] = '\0';
                ed->dirty = true;
            }
        }
    } else if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        int rel_x = ev->mouse.x - win->client_x;
        int rel_y = ev->mouse.y - win->client_y;
        /* Click on [Run >] button */
        if (rel_x >= win->client_w - 150 && rel_x <= win->client_w - 82 &&
            rel_y >= 5 && rel_y <= 27) {
            editor_run_file(ed);
        }
        /* Click on [Save] button */
        else if (rel_x >= win->client_w - 76 && rel_x <= win->client_w - 8 &&
                 rel_y >= 5 && rel_y <= 27) {
            editor_save_file(ed);
        }
    }
}

void app_editor_launch(const char *filepath) {
    kmemset(&g_editor, 0, sizeof(g_editor));

    if (filepath) {
        size_t i = 0;
        while (filepath[i] && i < sizeof(g_editor.filepath) - 1) {
            g_editor.filepath[i] = filepath[i];
            i++;
        }
        g_editor.filepath[i] = '\0';

        /* Read file from VFS with VFS_O_RDONLY */
        int fd = vfs_open(filepath, VFS_O_RDONLY);
        if (fd >= 0) {
            ssize_t bytes_read = vfs_read(fd, g_editor.text, EDITOR_BUFFER_MAX - 1);
            if (bytes_read > 0) {
                g_editor.text_len = (int)bytes_read;
                g_editor.text[g_editor.text_len] = '\0';
                g_editor.cursor_pos = g_editor.text_len;
            }
            vfs_close(fd);
            const char *msg = "Loaded from VFS. Ready for editing.";
            size_t m = 0;
            while (msg[m] && m < sizeof(g_editor.status_msg) - 1) { g_editor.status_msg[m] = msg[m]; m++; }
            g_editor.status_msg[m] = '\0';
        } else {
            const char *msg = "New document created.";
            size_t m = 0;
            while (msg[m] && m < sizeof(g_editor.status_msg) - 1) { g_editor.status_msg[m] = msg[m]; m++; }
            g_editor.status_msg[m] = '\0';
        }
    } else {
        const char *def_path = "/data/notes.txt";
        for (int i = 0; def_path[i]; i++) g_editor.filepath[i] = def_path[i];
        const char *msg = "Ready. Type code or notes.";
        size_t m = 0;
        while (msg[m] && m < sizeof(g_editor.status_msg) - 1) { g_editor.status_msg[m] = msg[m]; m++; }
        g_editor.status_msg[m] = '\0';
    }

    /* Reuse existing editor window if open */
    wm_window_t *existing = wm_find_by_title_substring("Code Editor");
    if (existing) {
        existing->user_data = &g_editor;
        wm_bring_to_front(existing);
        return;
    }

    wm_window_t *win = wm_create_window("AstraOS Code Editor", 120, 60, 640, 420);
    if (!win) return;

    win->user_data = &g_editor;
    win->on_paint = editor_on_paint;
    win->on_event = editor_on_event;
}
