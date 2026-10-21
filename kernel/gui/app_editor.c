#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/vfs.h>
#include <kernel/mem.h>
#include <kernel/kprintf.h>

#define EDITOR_BUFFER_MAX 4096

typedef struct editor_state {
    char filepath[64];
    char text[EDITOR_BUFFER_MAX];
    int text_len;
    int cursor_pos;
    char status_msg[128];
} editor_state_t;

static editor_state_t g_editor = {0};

static void editor_on_paint(wm_window_t *win, gfx_surface_t *s) {
    if (!win || !s) return;
    editor_state_t *ed = (editor_state_t *)win->user_data;
    if (!ed) return;

    /* Background: Dark slate */
    gfx_fill_rect(s, 0, 0, s->width, s->height, GFX_COLOR_WINDOW_BG);

    /* Header Bar: File Path and Save Notice */
    gfx_fill_rect(s, 0, 0, s->width, 24, GFX_COLOR_PANEL_BG);
    gfx_draw_line(s, 0, 24, s->width, 24, GFX_COLOR_PANEL_BORDER);

    char title_buf[80] = "File: ";
    int t_len = 6;
    for (int i = 0; ed->filepath[i] && t_len < 70; i++) {
        title_buf[t_len++] = ed->filepath[i];
    }
    title_buf[t_len] = '\0';
    gfx_draw_string(s, 8, 4, title_buf, GFX_COLOR_ACCENT_BLUE, 0, true);

    /* Save Button */
    gfx_fill_rect(s, s->width - 70, 3, 60, 18, GFX_COLOR_TITLEBAR_ACTIVE);
    gfx_draw_rect(s, s->width - 70, 3, 60, 18, GFX_COLOR_ACCENT_BLUE);
    gfx_draw_string(s, s->width - 58, 4, "Save", GFX_COLOR_TEXT_PRIMARY, 0, true);

    /* Text Area: Draw text content */
    int text_x = 8;
    int text_y = 30;
    int cur_screen_x = text_x;
    int cur_screen_y = text_y;

    for (int i = 0; i <= ed->text_len; i++) {
        if (i == ed->cursor_pos) {
            /* Draw Cursor indicator */
            gfx_fill_rect(s, cur_screen_x, cur_screen_y, 2, 16, GFX_COLOR_ACCENT_BLUE);
        }
        if (i == ed->text_len) break;

        char c = ed->text[i];
        if (c == '\n') {
            cur_screen_x = text_x;
            cur_screen_y += 18;
            if (cur_screen_y + 18 >= s->height - 24) break;
        } else if (c == '\r') {
            cur_screen_x = text_x;
        } else if (c == '\t') {
            cur_screen_x += 4 * FONT_WIDTH;
        } else {
            gfx_draw_char(s, cur_screen_x, cur_screen_y, c, GFX_COLOR_TEXT_PRIMARY, 0, true);
            cur_screen_x += FONT_WIDTH;
            if (cur_screen_x + FONT_WIDTH >= s->width - 8) {
                cur_screen_x = text_x;
                cur_screen_y += 18;
            }
        }
    }

    /* Bottom Status Bar */
    int sb_y = s->height - 22;
    gfx_fill_rect(s, 0, sb_y, s->width, 22, GFX_COLOR_PANEL_BG);
    gfx_draw_line(s, 0, sb_y, s->width, sb_y, GFX_COLOR_PANEL_BORDER);

    gfx_draw_string(s, 8, sb_y + 3, ed->status_msg, GFX_COLOR_TEXT_MUTED, 0, true);
}

static void editor_on_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return;
    editor_state_t *ed = (editor_state_t *)win->user_data;
    if (!ed) return;

    if (ev->type == INPUT_EVENT_KEY_DOWN) {
        if (ev->key.keycode == KEY_BACKSPACE) {
            if (ed->cursor_pos > 0) {
                for (int i = ed->cursor_pos - 1; i < ed->text_len - 1; i++) {
                    ed->text[i] = ed->text[i + 1];
                }
                ed->text_len--;
                ed->cursor_pos--;
                ed->text[ed->text_len] = '\0';
            }
        } else if (ev->key.keycode == KEY_ENTER) {
            if (ed->text_len < EDITOR_BUFFER_MAX - 2) {
                for (int i = ed->text_len; i > ed->cursor_pos; i--) {
                    ed->text[i] = ed->text[i - 1];
                }
                ed->text[ed->cursor_pos] = '\n';
                ed->text_len++;
                ed->cursor_pos++;
                ed->text[ed->text_len] = '\0';
            }
        } else if (ev->key.keycode == KEY_LEFT) {
            if (ed->cursor_pos > 0) ed->cursor_pos--;
        } else if (ev->key.keycode == KEY_RIGHT) {
            if (ed->cursor_pos < ed->text_len) ed->cursor_pos++;
        } else if (ev->key.ascii >= ' ' && ev->key.ascii <= '~') {
            if (ed->text_len < EDITOR_BUFFER_MAX - 2) {
                for (int i = ed->text_len; i > ed->cursor_pos; i--) {
                    ed->text[i] = ed->text[i - 1];
                }
                ed->text[ed->cursor_pos] = ev->key.ascii;
                ed->text_len++;
                ed->cursor_pos++;
                ed->text[ed->text_len] = '\0';
            }
        }
    } else if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        /* Check click on Save button */
        int rel_x = ev->mouse.x - win->client_x;
        int rel_y = ev->mouse.y - win->client_y;
        if (rel_x >= win->client_w - 70 && rel_x <= win->client_w - 10 &&
            rel_y >= 3 && rel_y <= 21) {
            /* Inform user about read-only VFS root */
            const char *msg = "Notice: VFS root (initramfs) is Read-Only. Edits active in-memory.";
            size_t i = 0;
            while (msg[i] && i < sizeof(ed->status_msg) - 1) {
                ed->status_msg[i] = msg[i];
                i++;
            }
            ed->status_msg[i] = '\0';
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

        /* Read file from VFS */
        int fd = vfs_open(filepath, 0);
        if (fd >= 0) {
            ssize_t bytes_read = vfs_read(fd, g_editor.text, EDITOR_BUFFER_MAX - 1);
            if (bytes_read > 0) {
                g_editor.text_len = (int)bytes_read;
                g_editor.text[g_editor.text_len] = '\0';
                g_editor.cursor_pos = g_editor.text_len;
            }
            vfs_close(fd);
            const char *msg = "File loaded from VFS. Ready for editing.";
            size_t m = 0;
            while (msg[m] && m < sizeof(g_editor.status_msg) - 1) {
                g_editor.status_msg[m] = msg[m];
                m++;
            }
            g_editor.status_msg[m] = '\0';
        } else {
            const char *msg = "New document (File not found in VFS).";
            size_t m = 0;
            while (msg[m] && m < sizeof(g_editor.status_msg) - 1) {
                g_editor.status_msg[m] = msg[m];
                m++;
            }
            g_editor.status_msg[m] = '\0';
        }
    } else {
        g_editor.filepath[0] = '/';
        g_editor.filepath[1] = 'u';
        g_editor.filepath[2] = 'n';
        g_editor.filepath[3] = 't';
        g_editor.filepath[4] = 'i';
        g_editor.filepath[5] = 't';
        g_editor.filepath[6] = 'l';
        g_editor.filepath[7] = 'e';
        g_editor.filepath[8] = 'd';
        g_editor.filepath[9] = '\0';
        const char *msg = "Ready. Type text or click Save.";
        size_t m = 0;
        while (msg[m] && m < sizeof(g_editor.status_msg) - 1) {
            g_editor.status_msg[m] = msg[m];
            m++;
        }
        g_editor.status_msg[m] = '\0';
    }

    wm_window_t *win = wm_create_window("AstraOS Text Editor", 120, 60, 480, 320);
    if (!win) return;

    win->user_data = &g_editor;
    win->on_paint = editor_on_paint;
    win->on_event = editor_on_event;
}
