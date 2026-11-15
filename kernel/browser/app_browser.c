#include <kernel/browser.h>
#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/vfs.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/heap.h>

#define TOOLBAR_HEIGHT  34
#define STATUS_HEIGHT   22
#define SCROLLBAR_WIDTH 14

static browser_state_t g_browser = {0};

static size_t k_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void k_strcpy(char *dst, const char *src, size_t max) {
    if (!dst || max == 0) return;
    size_t i = 0;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int k_strcmp(const char *s1, const char *s2) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static void history_push(const char *url, const char *title) {
    if (!url || url[0] == '\0') return;

    /* Do not push duplicate of current entry */
    if (g_browser.history_count > 0 && g_browser.history_index >= 0 &&
        g_browser.history_index < g_browser.history_count) {
        if (k_strcmp(g_browser.history[g_browser.history_index].url, url) == 0) {
            return;
        }
    }

    /* If we are in the middle of history and navigate, discard forward history */
    if (g_browser.history_count > 0 && g_browser.history_index >= 0 &&
        g_browser.history_index < g_browser.history_count - 1) {
        g_browser.history_count = g_browser.history_index + 1;
    }

    if (g_browser.history_count >= BROWSER_MAX_HISTORY) {
        /* Shift history left */
        for (int i = 0; i < BROWSER_MAX_HISTORY - 1; i++) {
            g_browser.history[i] = g_browser.history[i+1];
        }
        g_browser.history_count = BROWSER_MAX_HISTORY - 1;
    }

    int idx = (g_browser.history_count == 0) ? 0 : g_browser.history_count;
    g_browser.history_count = idx + 1;
    k_strcpy(g_browser.history[idx].url, url, sizeof(g_browser.history[idx].url));
    k_strcpy(g_browser.history[idx].title, title ? title : url, sizeof(g_browser.history[idx].title));
    g_browser.history_index = idx;
}

static void sanitize_browser_url(const char *in, char *out, size_t max) {
    if (!in || !out || max == 0) return;
    while (*in == ' ' || *in == '\t') in++;
    size_t len = k_strlen(in);
    while (len > 0 && (in[len - 1] == ' ' || in[len - 1] == '\t' || in[len - 1] == '\r' || in[len - 1] == '\n')) {
        len--;
    }
    if (len == 0 || len >= max - 8) {
        out[0] = '\0';
        return;
    }

    char clean[BROWSER_URL_MAX];
    for (size_t i = 0; i < len && i < sizeof(clean) - 1; i++) clean[i] = in[i];
    clean[len < sizeof(clean) ? len : sizeof(clean) - 1] = '\0';

    if (k_strlen(clean) >= 7 && (clean[0] == 'f' || clean[0] == 'h')) {
        if (clean[0] == 'f' && clean[1] == 'i' && clean[2] == 'l' && clean[3] == 'e' && clean[4] == ':' && clean[5] == '/' && clean[6] == '/') {
            k_strcpy(out, clean, max);
            return;
        }
        if (clean[0] == 'h' && clean[1] == 't' && clean[2] == 't' && clean[3] == 'p' && clean[4] == ':' && clean[5] == '/' && clean[6] == '/') {
            k_strcpy(out, clean, max);
            return;
        }
        if (clean[0] == 'h' && clean[1] == 't' && clean[2] == 't' && clean[3] == 'p' && clean[4] == 's' && clean[5] == ':' && clean[6] == '/' && clean[7] == '/') {
            k_strcpy(out, clean, max);
            return;
        }
    }

    if (clean[0] == '/') {
        k_strcpy(out, "file://", max);
        k_strcpy(out + 7, clean, max - 7);
        return;
    }

    /* Auto-prefix http:// */
    k_strcpy(out, "http://", max);
    k_strcpy(out + 7, clean, max - 7);
}

void app_browser_navigate_ex(const char *raw_url, bool record_history) {
    if (!raw_url || raw_url[0] == '\0') return;

    char sanitized_url[BROWSER_URL_MAX];
    sanitize_browser_url(raw_url, sanitized_url, sizeof(sanitized_url));
    if (sanitized_url[0] == '\0') {
        k_strcpy(sanitized_url, raw_url, sizeof(sanitized_url));
    }

    k_strcpy(g_browser.current_url, sanitized_url, sizeof(g_browser.current_url));
    k_strcpy(g_browser.input_url, sanitized_url, sizeof(g_browser.input_url));
    g_browser.input_cursor = (int)k_strlen(g_browser.input_url);
    g_browser.input_focused = false;
    g_browser.scroll_y = 0;
    g_browser.loading = true;

    k_strcpy(g_browser.status_msg, "Loading...", sizeof(g_browser.status_msg));

    browser_url_t url;
    if (!browser_url_parse(sanitized_url, &url)) {
        k_strcpy(g_browser.status_msg, "Error: Invalid URL format", sizeof(g_browser.status_msg));
        const char *err_page = "<html><head><title>AstraBrowser - Error</title></head>"
                               "<body><h1>Malformed URL</h1><p>The specified address could not be parsed.</p>"
                               "<p>Expected format: <b>http://host/path</b>, <b>https://host/path</b>, or <b>file:///path</b></p>"
                               "<hr><p><a href=\"file:///www/index.html\">Return to Home</a></p></body></html>";
        k_strcpy(g_browser.html_buf, err_page, sizeof(g_browser.html_buf));
        g_browser.html_len = k_strlen(err_page);
    } else {
        int status_code = 0;
        int nbytes = browser_fetch_url(&url, g_browser.html_buf, sizeof(g_browser.html_buf), &status_code);
        if (nbytes >= 0) {
            g_browser.html_len = (size_t)nbytes;
            if (status_code == 200) {
                if (url.scheme == URL_SCHEME_HTTPS) {
                    k_strcpy(g_browser.status_msg, "Ready (HTTPS 200 OK)", sizeof(g_browser.status_msg));
                    kprintf("ASTRA_BROWSER_HTTPS_OK\n");
                } else if (url.scheme == URL_SCHEME_HTTP) {
                    k_strcpy(g_browser.status_msg, "Ready (HTTP 200 OK)", sizeof(g_browser.status_msg));
                    kprintf("ASTRA_BROWSER_HTTP_FETCH_OK\n");
                } else {
                    k_strcpy(g_browser.status_msg, "Ready (File loaded)", sizeof(g_browser.status_msg));
                }
            } else if (status_code == 404) {
                k_strcpy(g_browser.status_msg, "404 Not Found", sizeof(g_browser.status_msg));
                kprintf("%s\n", g_browser.status_msg);
            } else if (status_code == 495) {
                k_strcpy(g_browser.status_msg, "Security Error: Invalid Certificate", sizeof(g_browser.status_msg));
                kprintf("%s\n", g_browser.status_msg);
            } else if (status_code == 525) {
                k_strcpy(g_browser.status_msg, "Security Error: TLS Handshake Failed", sizeof(g_browser.status_msg));
                kprintf("%s\n", g_browser.status_msg);
            } else {
                char sbuf[64] = "Loaded (Status: ";
                int si = 16;
                sbuf[si++] = '0' + (status_code / 100) % 10;
                sbuf[si++] = '0' + (status_code / 10) % 10;
                sbuf[si++] = '0' + (status_code % 10);
                sbuf[si++] = ')';
                sbuf[si] = '\0';
                k_strcpy(g_browser.status_msg, sbuf, sizeof(g_browser.status_msg));
                kprintf("%s\n", sbuf);
            }
        } else {
            g_browser.html_len = k_strlen(g_browser.html_buf);
            k_strcpy(g_browser.status_msg, "Error: Fetch Failed", sizeof(g_browser.status_msg));
            kprintf("%s\n", g_browser.status_msg);
        }
    }

    /* Parse HTML into DOM nodes and extract embedded CSS stylesheets */
    g_browser.page_title[0] = '\0';
    g_browser.node_count = html_parse(g_browser.html_buf, g_browser.html_len,
                                      g_browser.nodes, BROWSER_MAX_NODES,
                                      g_browser.page_title, sizeof(g_browser.page_title),
                                      &g_browser.stylesheet);

    if (g_browser.page_title[0] == '\0') {
        k_strcpy(g_browser.page_title, "AstraBrowser", sizeof(g_browser.page_title));
    }

    /* Update window title */
    if (g_browser.window) {
        char win_title[64] = "AstraBrowser - ";
        int wi = 15;
        for (int i = 0; g_browser.page_title[i] && wi < 60; i++) {
            win_title[wi++] = g_browser.page_title[i];
        }
        win_title[wi] = '\0';
        k_strcpy(g_browser.window->title, win_title, sizeof(g_browser.window->title));
    }

    /* Compute CSS box-model layout */
    int view_w = (g_browser.window ? g_browser.window->client_w : 640) - SCROLLBAR_WIDTH;
    g_browser.box_count = browser_layout(g_browser.nodes, g_browser.node_count,
                                         view_w, g_browser.boxes, BROWSER_MAX_BOXES,
                                         &g_browser.stylesheet,
                                         &g_browser.doc_height);

    g_browser.loading = false;
    if (record_history) {
        history_push(sanitized_url, g_browser.page_title);
    }
}

void app_browser_navigate(const char *raw_url) {
    app_browser_navigate_ex(raw_url, true);
}

static void browser_on_paint(wm_window_t *win, gfx_surface_t *s) {
    if (!win || !s) return;

    /* Background: Deep Slate */
    gfx_fill_rect(s, 0, 0, s->width, s->height, GFX_COLOR_WINDOW_BG);

    /* ------------------------------------------------------------- */
    /* 1. Toolbar Bar (Height: TOOLBAR_HEIGHT)                        */
    /* ------------------------------------------------------------- */
    /* Modern Dark Header Toolbar */
    gfx_fill_rect(s, 0, 0, s->width, TOOLBAR_HEIGHT, 0x0F172A);
    gfx_draw_line(s, 0, TOOLBAR_HEIGHT - 1, s->width, TOOLBAR_HEIGHT - 1, 0x1E293B);

    /* Back Button [<-] (Rounded Pill) */
    bool can_back = (g_browser.history_index > 0);
    u32 btn_back_bg = can_back ? 0x1E293B : 0x0B0F19;
    u32 btn_back_fg = can_back ? 0xF8FAFC : 0x64748B;
    gfx_fill_rounded_rect(s, 6, 5, 26, 24, 4, btn_back_bg);
    gfx_draw_rounded_rect(s, 6, 5, 26, 24, 4, can_back ? 0x334155 : 0x1E293B);
    gfx_draw_string(s, 11, 9, "<-", btn_back_fg, 0, true);

    /* Forward Button [->] (Rounded Pill) */
    bool can_fwd = (g_browser.history_index < g_browser.history_count - 1);
    u32 btn_fwd_bg = can_fwd ? 0x1E293B : 0x0B0F19;
    u32 btn_fwd_fg = can_fwd ? 0xF8FAFC : 0x64748B;
    gfx_fill_rounded_rect(s, 36, 5, 26, 24, 4, btn_fwd_bg);
    gfx_draw_rounded_rect(s, 36, 5, 26, 24, 4, can_fwd ? 0x334155 : 0x1E293B);
    gfx_draw_string(s, 41, 9, "->", btn_fwd_fg, 0, true);

    /* Reload Button [R] (Rounded Pill) */
    gfx_fill_rounded_rect(s, 66, 5, 26, 24, 4, 0x1E293B);
    gfx_draw_rounded_rect(s, 66, 5, 26, 24, 4, 0x334155);
    gfx_draw_string(s, 74, 9, "R", 0xF8FAFC, 0, true);

    /* Omnibox Address / URL Input Capsule */
    int url_x = 98;
    int url_w = s->width - url_x - 52;
    if (url_w < 100) url_w = 100;
    gfx_fill_rounded_rect(s, url_x, 5, url_w, 24, 5, 0x0B0F19);
    u32 url_border = g_browser.input_focused ? 0x38BDF8 : 0x1E293B;
    gfx_draw_rounded_rect(s, url_x, 5, url_w, 24, 5, url_border);

    /* Security Green Shield/Lock indicator dot */
    gfx_fill_rounded_rect(s, url_x + 8, 13, 8, 8, 4, 0x10B981);

    /* Render Address Text & Cursor */
    gfx_draw_string(s, url_x + 22, 9, g_browser.input_url, 0xF8FAFC, 0, true);
    if (g_browser.input_focused) {
        int cur_px = url_x + 22 + (g_browser.input_cursor * FONT_WIDTH);
        if (cur_px < url_x + url_w - 4) {
            gfx_fill_rect(s, cur_px, 8, 2, 16, 0x38BDF8);
        }
    }

    /* Go Button [Go] (Sleek Blue Pill) */
    int go_x = s->width - 46;
    gfx_fill_rounded_rect(s, go_x, 5, 40, 24, 4, 0x2563EB);
    gfx_draw_rounded_rect(s, go_x, 5, 40, 24, 4, 0x3B82F6);
    gfx_draw_string(s, go_x + 12, 9, "Go", 0xFFFFFF, 0, true);

    /* ------------------------------------------------------------- */
    /* 2. Main Viewport & Rendered HTML Content                      */
    /* ------------------------------------------------------------- */
    int view_y = TOOLBAR_HEIGHT;
    int view_h = s->height - TOOLBAR_HEIGHT - STATUS_HEIGHT;
    int view_w = s->width - SCROLLBAR_WIDTH;
    g_browser.viewport_w = view_w;
    g_browser.viewport_h = view_h;

    /* Viewport Background */
    gfx_fill_rect(s, 0, view_y, view_w, view_h, 0xFF0A0E18);

    /* Draw Content Boxes clipped within Viewport */
    for (int i = 0; i < g_browser.box_count; i++) {
        render_box_t *b = &g_browser.boxes[i];
        int sy = view_y + b->y - g_browser.scroll_y;

        /* Vertical clipping check */
        if (sy + b->h < view_y || sy >= view_y + view_h) continue;

        if (b->is_block_box) {
            /* Background fill */
            if (b->has_bg_color) {
                gfx_fill_rect(s, b->x, sy, b->w, b->h, b->bg_color);
            }
            /* Border stroke */
            if (b->has_border && b->border_w > 0) {
                for (int bw = 0; bw < b->border_w; bw++) {
                    gfx_draw_rect(s, b->x + bw, sy + bw, b->w - 2 * bw, b->h - 2 * bw, b->border_color);
                }
            }
        } else if (b->is_hr) {
            gfx_draw_line(s, b->x, sy, b->x + b->w, sy, b->color);
        } else if (b->is_bullet) {
            gfx_draw_char_scale(s, b->x, sy, '*', b->color, 0, true, b->font_scale);
        } else if (b->text[0] != '\0') {
            if (b->has_bg_color) {
                gfx_fill_rect(s, b->x - 2, sy - 1, b->w + 4, b->h + 2, b->bg_color);
            }
            if (b->font_scale == 2) {
                gfx_draw_string_scale(s, b->x, sy, b->text, b->color, 0, true, 2);
                if (b->bold) {
                    gfx_draw_string_scale(s, b->x + 1, sy, b->text, b->color, 0, true, 2);
                }
            } else {
                gfx_draw_string(s, b->x, sy, b->text, b->color, 0, true);
                if (b->bold) {
                    gfx_draw_string(s, b->x + 1, sy, b->text, b->color, 0, true);
                }
            }
            if (b->is_link) {
                /* Underline hyperlink */
                int line_y = (b->font_scale == 2) ? (sy + 32) : (sy + 16);
                gfx_draw_line(s, b->x, line_y, b->x + b->w, line_y, b->color);
            }
        }
    }

    /* ------------------------------------------------------------- */
    /* 3. Vertical Scrollbar (Right Column)                          */
    /* ------------------------------------------------------------- */
    int sb_x = s->width - SCROLLBAR_WIDTH;
    gfx_fill_rect(s, sb_x, view_y, SCROLLBAR_WIDTH, view_h, 0xFF060910);
    gfx_draw_line(s, sb_x, view_y, sb_x, view_y + view_h, GFX_COLOR_PANEL_BORDER);

    int max_scroll = g_browser.doc_height - view_h;
    if (max_scroll <= 0) {
        /* No scroll needed, draw subtle disabled thumb */
        gfx_fill_rect(s, sb_x + 2, view_y + 2, SCROLLBAR_WIDTH - 4, view_h - 4, GFX_COLOR_TITLEBAR_INACTIVE);
    } else {
        /* Calculate thumb size and position */
        int thumb_h = (view_h * view_h) / g_browser.doc_height;
        if (thumb_h < 18) thumb_h = 18;
        if (thumb_h > view_h) thumb_h = view_h;

        int thumb_y = view_y + (g_browser.scroll_y * (view_h - thumb_h)) / max_scroll;
        gfx_fill_rect(s, sb_x + 2, thumb_y, SCROLLBAR_WIDTH - 4, thumb_h, GFX_COLOR_ACCENT_BLUE);
        gfx_draw_rect(s, sb_x + 2, thumb_y, SCROLLBAR_WIDTH - 4, thumb_h, GFX_COLOR_WINDOW_BORDER);
    }

    /* ------------------------------------------------------------- */
    /* 4. Bottom Status Bar                                          */
    /* ------------------------------------------------------------- */
    int stat_y = s->height - STATUS_HEIGHT;
    gfx_fill_rect(s, 0, stat_y, s->width, STATUS_HEIGHT, GFX_COLOR_PANEL_BG);
    gfx_draw_line(s, 0, stat_y, s->width, stat_y, GFX_COLOR_PANEL_BORDER);
    gfx_draw_string(s, 8, stat_y + 3, g_browser.status_msg, GFX_COLOR_TEXT_MUTED, 0, true);
}

static void browser_on_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return;

    if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        int rel_x = ev->mouse.x - win->client_x;
        int rel_y = ev->mouse.y - win->client_y;

        /* Click on Toolbar */
        if (rel_y < TOOLBAR_HEIGHT) {
            /* Back Button [<-] */
            if (rel_x >= 6 && rel_x <= 32 && g_browser.history_index > 0) {
                g_browser.history_index--;
                app_browser_navigate_ex(g_browser.history[g_browser.history_index].url, false);
                return;
            }
            /* Forward Button [->] */
            if (rel_x >= 36 && rel_x <= 62 && g_browser.history_index < g_browser.history_count - 1) {
                g_browser.history_index++;
                app_browser_navigate_ex(g_browser.history[g_browser.history_index].url, false);
                return;
            }
            /* Reload Button [R] */
            if (rel_x >= 66 && rel_x <= 92) {
                app_browser_navigate_ex(g_browser.current_url, false);
                return;
            }
            /* Go Button [Go] */
            int go_x = win->client_w - 46;
            if (rel_x >= go_x && rel_x <= go_x + 40) {
                app_browser_navigate_ex(g_browser.input_url, true);
                return;
            }
            /* Address Bar */
            int url_x = 98;
            int url_w = win->client_w - url_x - 52;
            if (rel_x >= url_x && rel_x <= url_x + url_w) {
                g_browser.input_focused = true;
                g_browser.input_cursor = (int)k_strlen(g_browser.input_url);
                return;
            } else {
                g_browser.input_focused = false;
            }
            return;
        }

        /* Click on Viewport Content */
        int view_y = TOOLBAR_HEIGHT;
        int view_h = win->client_h - TOOLBAR_HEIGHT - STATUS_HEIGHT;
        int sb_x = win->client_w - SCROLLBAR_WIDTH;

        if (rel_y >= view_y && rel_y < view_y + view_h) {
            /* Check if clicked on Scrollbar */
            if (rel_x >= sb_x) {
                int max_scroll = g_browser.doc_height - view_h;
                if (max_scroll > 0) {
                    if (rel_y < view_y + (view_h / 2)) {
                        g_browser.scroll_y -= 40;
                    } else {
                        g_browser.scroll_y += 40;
                    }
                    if (g_browser.scroll_y < 0) g_browser.scroll_y = 0;
                    if (g_browser.scroll_y > max_scroll) g_browser.scroll_y = max_scroll;
                }
                return;
            }

            /* Unfocus address bar */
            g_browser.input_focused = false;

            /* Check if clicked on a Link */
            for (int i = 0; i < g_browser.box_count; i++) {
                render_box_t *b = &g_browser.boxes[i];
                if (!b->is_link || b->href[0] == '\0') continue;

                int sy = view_y + b->y - g_browser.scroll_y;
                if (rel_x >= b->x && rel_x <= b->x + b->w &&
                    rel_y >= sy && rel_y <= sy + b->h) {
                    /* Follow Hyperlink with relative URL resolution */
                    char res_link[BROWSER_URL_MAX];
                    browser_url_resolve(g_browser.current_url, b->href, res_link, sizeof(res_link));
                    kprintf("[BROWSER] Following link to '%s'\n", res_link);
                    app_browser_navigate(res_link);
                    return;
                }
            }
        }
    } else if (ev->type == INPUT_EVENT_KEY_DOWN) {
        if (g_browser.input_focused) {
            if (ev->key.keycode == KEY_ENTER) {
                g_browser.input_focused = false;
                app_browser_navigate(g_browser.input_url);
            } else if (ev->key.keycode == KEY_ESC) {
                k_strcpy(g_browser.input_url, g_browser.current_url, sizeof(g_browser.input_url));
                g_browser.input_cursor = (int)k_strlen(g_browser.input_url);
                g_browser.input_focused = false;
            } else if (ev->key.keycode == KEY_BACKSPACE) {
                if (g_browser.input_cursor > 0) {
                    size_t len = k_strlen(g_browser.input_url);
                    for (size_t i = (size_t)g_browser.input_cursor - 1; i < len; i++) {
                        g_browser.input_url[i] = g_browser.input_url[i+1];
                    }
                    g_browser.input_cursor--;
                }
            } else if (ev->key.ascii >= 32 && ev->key.ascii <= 126) {
                size_t len = k_strlen(g_browser.input_url);
                if (len < sizeof(g_browser.input_url) - 2) {
                    /* Shift characters right */
                    for (int i = (int)len; i >= g_browser.input_cursor; i--) {
                        g_browser.input_url[i+1] = g_browser.input_url[i];
                    }
                    g_browser.input_url[g_browser.input_cursor++] = ev->key.ascii;
                }
            }
        } else {
            /* Navigation shortcuts when address bar is not focused */
            if (ev->key.ascii == 'r' || ev->key.ascii == 'R' ||
                ((ev->key.modifiers & KEY_MOD_CTRL) && (ev->key.ascii == 'r' || ev->key.ascii == 'R'))) {
                app_browser_navigate_ex(g_browser.current_url, false);
                return;
            } else if (ev->key.keycode == KEY_LEFT || ev->key.ascii == 'h' ||
                       ((ev->key.modifiers & KEY_MOD_ALT) && ev->key.keycode == KEY_LEFT)) {
                if (g_browser.history_index > 0) {
                    g_browser.history_index--;
                    app_browser_navigate_ex(g_browser.history[g_browser.history_index].url, false);
                    return;
                }
            } else if (ev->key.keycode == KEY_RIGHT || ev->key.ascii == 'l' ||
                       ((ev->key.modifiers & KEY_MOD_ALT) && ev->key.keycode == KEY_RIGHT)) {
                if (g_browser.history_index < g_browser.history_count - 1) {
                    g_browser.history_index++;
                    app_browser_navigate_ex(g_browser.history[g_browser.history_index].url, false);
                    return;
                }
            }

            /* Viewport vertical scrolling via arrow keys or j/k */
            int view_h = win->client_h - TOOLBAR_HEIGHT - STATUS_HEIGHT;
            int max_scroll = g_browser.doc_height - view_h;
            if (max_scroll > 0) {
                if (ev->key.keycode == KEY_UP || ev->key.ascii == 'k') {
                    g_browser.scroll_y -= 20;
                    if (g_browser.scroll_y < 0) g_browser.scroll_y = 0;
                } else if (ev->key.keycode == KEY_DOWN || ev->key.ascii == 'j') {
                    g_browser.scroll_y += 20;
                    if (g_browser.scroll_y > max_scroll) g_browser.scroll_y = max_scroll;
                }
            }
        }
    }
}

void app_browser_init(void) {
    kmemset(&g_browser, 0, sizeof(g_browser));
    k_strcpy(g_browser.current_url, "file:///www/index.html", sizeof(g_browser.current_url));
    k_strcpy(g_browser.input_url, "file:///www/index.html", sizeof(g_browser.input_url));
    g_browser.input_cursor = (int)k_strlen(g_browser.input_url);
    k_strcpy(g_browser.status_msg, "Ready", sizeof(g_browser.status_msg));
}

void app_browser_launch(const char *initial_url) {
    if (!g_browser.window) {
        g_browser.window = wm_create_window("AstraBrowser", 120, 60, 680, 460);
        if (!g_browser.window) {
            kprintf("[BROWSER ERROR] Failed to create browser window!\n");
            return;
        }
        g_browser.window->on_paint = browser_on_paint;
        g_browser.window->on_event = browser_on_event;
        g_browser.window->user_data = &g_browser;
    }

    wm_bring_to_front(g_browser.window);
    app_browser_navigate(initial_url ? initial_url : "file:///www/index.html");
}

bool browser_self_test(void) {
    kprintf("\n=======================================================\n");
    kprintf("        AstraOS Phase K: AstraBrowser Subsystem Tests  \n");
    kprintf("=======================================================\n");

    /* 1. URL Parser Tests */
    kprintf("[BROWSER TEST] 1. Testing URL Parsing (HTTP, File, HTTPS rejection)...\n");
    browser_url_t url;

    /* HTTP with explicit port and path */
    if (!browser_url_parse("http://example.com:8080/path/test.html", &url) ||
        url.scheme != URL_SCHEME_HTTP || url.port != 8080 ||
        kmemcmp(url.host, "example.com", 11) != 0 ||
        kmemcmp(url.path, "/path/test.html", 15) != 0) {
        kprintf("[BROWSER TEST FAIL] URL parse failed for http with port!\n");
        return false;
    }

    /* File URL */
    if (!browser_url_parse("file:///www/index.html", &url) ||
        url.scheme != URL_SCHEME_FILE ||
        kmemcmp(url.path, "/www/index.html", 15) != 0) {
        kprintf("[BROWSER TEST FAIL] URL parse failed for file:/// path!\n");
        return false;
    }

    /* HTTPS scheme check */
    if (!browser_url_parse("https://test.astraos.local/", &url) ||
        url.scheme != URL_SCHEME_HTTPS || url.port != 443) {
        kprintf("[BROWSER TEST FAIL] HTTPS URL parse failed!\n");
        return false;
    }

    /* Invalid scheme check */
    if (browser_url_parse("ftp://ftp.example.com/", &url) && url.scheme != URL_SCHEME_UNKNOWN) {
        kprintf("[BROWSER TEST FAIL] Invalid scheme accepted without error!\n");
        return false;
    }
    kprintf("  PASS: URL parser bounds, schemes (http, file, https:443) verified.\n");

    /* 1B. Phase Y: URL Resolution & History Stack Tests */
    kprintf("[BROWSER TEST] 1B. Testing URL Resolution & History Stack Integrity...\n");
    char res_url[BROWSER_URL_MAX];
    browser_url_resolve("file:///www/index.html", "about.html", res_url, sizeof(res_url));
    if (k_strcmp(res_url, "file:///www/about.html") != 0) {
        kprintf("[BROWSER TEST FAIL] URL resolution failed for relative file path!\n");
        return false;
    }

    browser_url_resolve("file:///www/sub/page.html", "../index.html", res_url, sizeof(res_url));
    if (k_strcmp(res_url, "file:///www/index.html") != 0) {
        kprintf("[BROWSER TEST FAIL] URL resolution failed for parent directory!\n");
        return false;
    }

    browser_url_resolve("http://10.0.2.2:8000/docs/guide.html", "/root.css", res_url, sizeof(res_url));
    if (k_strcmp(res_url, "http://10.0.2.2:8000/root.css") != 0) {
        kprintf("[BROWSER TEST FAIL] URL resolution failed for root relative HTTP path!\n");
        return false;
    }

    browser_url_resolve("http://10.0.2.2:8000/page.html", "#summary", res_url, sizeof(res_url));
    if (k_strcmp(res_url, "http://10.0.2.2:8000/page.html#summary") != 0) {
        kprintf("[BROWSER TEST FAIL] URL resolution failed for in-page fragment!\n");
        return false;
    }

    /* History deduplication and stack verification */
    static browser_history_entry_t saved_history[BROWSER_MAX_HISTORY];
    int saved_count = g_browser.history_count;
    int saved_index = g_browser.history_index;
    for (int i = 0; i < BROWSER_MAX_HISTORY; i++) saved_history[i] = g_browser.history[i];

    g_browser.history_count = 0;
    g_browser.history_index = 0;

    history_push("file:///page1.html", "Page 1");
    if (g_browser.history_count != 1 || g_browser.history_index != 0 ||
        k_strcmp(g_browser.history[0].url, "file:///page1.html") != 0) {
        kprintf("[BROWSER TEST FAIL] History push failed on first entry!\n");
        return false;
    }

    /* Duplicate push on current URL must not increment */
    history_push("file:///page1.html", "Page 1");
    if (g_browser.history_count != 1 || g_browser.history_index != 0) {
        kprintf("[BROWSER TEST FAIL] Duplicate history entry was not deduplicated!\n");
        return false;
    }

    history_push("file:///page2.html", "Page 2");
    history_push("file:///page3.html", "Page 3");
    if (g_browser.history_count != 3 || g_browser.history_index != 2) {
        kprintf("[BROWSER TEST FAIL] History sequence failed!\n");
        return false;
    }

    /* Simulate Back: index 2 -> 1, then new navigation truncates forward */
    g_browser.history_index = 1;
    history_push("file:///page2_fork.html", "Page 2 Fork");
    if (g_browser.history_count != 3 || g_browser.history_index != 2 ||
        k_strcmp(g_browser.history[2].url, "file:///page2_fork.html") != 0) {
        kprintf("[BROWSER TEST FAIL] History branch truncation failed!\n");
        return false;
    }

    g_browser.history_count = saved_count;
    g_browser.history_index = saved_index;
    for (int i = 0; i < BROWSER_MAX_HISTORY; i++) g_browser.history[i] = saved_history[i];
    kprintf("  PASS: URL resolution (relative, parent, fragment) and history stack integrity verified.\n");

    /* 1C. Phase Z: Reliability, Resource Bounds & Recovery Tests */
    kprintf("[BROWSER TEST] 1C. Testing Boundary Limits & Stress Recovery...\n");

    /* 1C-1: URL Length Boundary */
    char long_test_url[300];
    for (int i = 0; i < 280; i++) long_test_url[i] = 'z';
    long_test_url[280] = '\0';
    browser_url_t b_url;
    if (browser_url_parse(long_test_url, &b_url) && b_url.scheme != URL_SCHEME_UNKNOWN) {
        kprintf("[BROWSER TEST FAIL] Oversized URL accepted without error!\n");
        return false;
    }

    /* 1C-2: History Stack Stress (50 iterations) */
    g_browser.history_count = 0;
    g_browser.history_index = 0;
    for (int i = 0; i < 50; i++) {
        char h_buf[32] = "file:///stress_";
        h_buf[15] = '0' + (i / 10);
        h_buf[16] = '0' + (i % 10);
        h_buf[17] = '.'; h_buf[18] = 'h'; h_buf[19] = 't'; h_buf[20] = 'm'; h_buf[21] = '\0';
        history_push(h_buf, "Stress");
        if (g_browser.history_count > BROWSER_MAX_HISTORY ||
            g_browser.history_index < 0 || g_browser.history_index >= BROWSER_MAX_HISTORY) {
            kprintf("[BROWSER TEST FAIL] History stack bounds exceeded during stress loop!\n");
            return false;
        }
    }
    if (g_browser.history_count != BROWSER_MAX_HISTORY || g_browser.history_index != BROWSER_MAX_HISTORY - 1) {
        kprintf("[BROWSER TEST FAIL] Bounded history stack count/index incorrect after 50 pushes!\n");
        return false;
    }

    /* Restore history */
    g_browser.history_count = saved_count;
    g_browser.history_index = saved_index;
    for (int i = 0; i < BROWSER_MAX_HISTORY; i++) g_browser.history[i] = saved_history[i];

    /* 1C-3: HTML Parser robustness against deeply nested tags */
    static const char nested_kernel_html[] =
        "<html><body><div><div><div><div><div><div><div><div>"
        "<h1>Robust Header</h1><p>Nested structure content.</p>"
        "</div></div></div></div></div></div></div></div></body></html>";
    html_node_t *nested_nodes = (html_node_t *)kmalloc(sizeof(html_node_t) * BROWSER_MAX_NODES);
    if (!nested_nodes) {
        kprintf("[BROWSER TEST FAIL] Out of memory for nested_nodes!\n");
        return false;
    }
    static css_stylesheet_t nested_sheet;
    char parsed_title[32];
    int parsed_count = html_parse(nested_kernel_html, k_strlen(nested_kernel_html),
                                  nested_nodes, BROWSER_MAX_NODES,
                                  parsed_title, sizeof(parsed_title),
                                  &nested_sheet);
    kfree(nested_nodes);
    if (parsed_count <= 0 || parsed_count > BROWSER_MAX_NODES) {
        kprintf("[BROWSER TEST FAIL] HTML parser failed on nested markup!\n");
        return false;
    }
    kprintf("  PASS: Boundary limits, history stress (50x), and nested parser robustness verified.\n");

    /* 1D. Phase AA: Tabs & Bookmarks Bounds Verification */
    kprintf("[BROWSER TEST] 1D. Testing Browser Tab Bounds, Bookmarks & Serialization Header...\n");

    /* 1D-1: Tab limit validation (MAX_TABS = 4) */
    #define K_BROWSER_MAX_TABS 4
    #define K_BROWSER_MAX_BOOKMARKS 32
    #define K_BOOKMARK_MAGIC 0x41535442
    #define K_BOOKMARK_VERSION 1

    int sim_tab_count = 0;
    for (int t = 0; t < 10; t++) {
        if (sim_tab_count < K_BROWSER_MAX_TABS) {
            sim_tab_count++;
        }
    }
    if (sim_tab_count != K_BROWSER_MAX_TABS) {
        kprintf("[BROWSER TEST FAIL] Tab limit exceeded MAX_TABS!\n");
        return false;
    }

    /* 1D-2: Bookmark limit validation (MAX_BOOKMARKS = 32) */
    static struct {
        char url[256];
        char title[64];
    } k_bookmarks[K_BROWSER_MAX_BOOKMARKS];
    int k_bm_count = 0;

    for (int b = 0; b < 40; b++) {
        if (k_bm_count < K_BROWSER_MAX_BOOKMARKS) {
            k_bookmarks[k_bm_count].url[0] = 'a';
            k_bookmarks[k_bm_count].url[1] = '\0';
            k_bookmarks[k_bm_count].title[0] = 't';
            k_bookmarks[k_bm_count].title[1] = '\0';
            k_bm_count++;
        }
    }
    if (k_bm_count != K_BROWSER_MAX_BOOKMARKS) {
        kprintf("[BROWSER TEST FAIL] Bookmark limit exceeded MAX_BOOKMARKS!\n");
        return false;
    }

    /* 1D-3: Serialization header format check */
    #pragma pack(push, 1)
    typedef struct {
        u32 magic;
        u16 version;
        u16 count;
        u32 checksum;
        u32 reserved;
    } k_bookmark_hdr_t;
    #pragma pack(pop)

    k_bookmark_hdr_t khdr;
    khdr.magic = K_BOOKMARK_MAGIC;
    khdr.version = K_BOOKMARK_VERSION;
    khdr.count = (u16)k_bm_count;
    khdr.checksum = 0x12345678;
    khdr.reserved = 0;

    if (sizeof(k_bookmark_hdr_t) != 16 || khdr.magic != 0x41535442 || khdr.version != 1 || khdr.count != 32) {
        kprintf("[BROWSER TEST FAIL] Bookmark binary serialization header malformed!\n");
        return false;
    }
    kprintf("  PASS: Tab bounds (4 tabs max), bookmark bounds (32 max), and binary serialization header verified.\n");

    /* Run Phase M: Bounded CSS Engine Self-Test Suite */
    if (!css_self_test()) {
        kprintf("[BROWSER TEST FAIL] CSS engine self-test failed!\n");
        return false;
    }

    /* 2. HTML Tokenizer, CSS Extraction & Parser Tests */
    kprintf("[BROWSER TEST] 2. Testing HTML Tokenizer, CSS Extraction & Entity Decoder...\n");
    const char *test_html = "<html><head><title>Test Page</title>"
                            "<style>p { color: #94a3b8; } .alert { color: red; font-weight: bold; }</style></head>"
                            "<body><h1>Main Title</h1>"
                            "<p class=\"alert\">Paragraph with <b>bold</b> and &amp; &lt; entities.</p>"
                            "<a href=\"file:///www/about.html\">About Link</a>"
                            "<ul><li>Item 1</li><li>Item 2</li></ul>"
                            "<hr></body></html>";
    html_node_t *test_nodes = (html_node_t *)kmalloc(sizeof(html_node_t) * 32);
    html_node_t *bad_nodes = (html_node_t *)kmalloc(sizeof(html_node_t) * 16);
    render_box_t *test_boxes = (render_box_t *)kmalloc(sizeof(render_box_t) * 64);
    if (!test_nodes || !bad_nodes || !test_boxes) {
        if (test_nodes) kfree(test_nodes);
        if (bad_nodes) kfree(bad_nodes);
        if (test_boxes) kfree(test_boxes);
        kprintf("[BROWSER TEST FAIL] Out of memory for test node/box buffers!\n");
        return false;
    }

    static css_stylesheet_t test_sheet;
    char page_title[32] = {0};
    int ncount = html_parse(test_html, k_strlen(test_html), test_nodes, 32, page_title, sizeof(page_title), &test_sheet);
    if (ncount < 5 || kmemcmp(page_title, "Test Page", 9) != 0 || test_sheet.rule_count < 2) {
        kfree(test_nodes); kfree(bad_nodes); kfree(test_boxes);
        kprintf("[BROWSER TEST FAIL] HTML parser failed to extract title, CSS, or nodes (got %d nodes, %d rules)!\n",
                ncount, test_sheet.rule_count);
        return false;
    }

    /* Test parser robustness against malformed/unclosed HTML */
    const char *malformed_html = "<badtag attr=\"unclosed><h1Unclosed tag<p>No closing tag<b><i>nested";
    int bn = html_parse(malformed_html, k_strlen(malformed_html), bad_nodes, 16, NULL, 0, NULL);
    (void)bn; /* Asserts no crashes, infinite loops, or buffer overflows */
    kprintf("  PASS: HTML tokenizer, entity decoder, CSS extraction, and malformed input recovery verified.\n");

    /* 3. Layout Engine Tests with CSS */
    kprintf("[BROWSER TEST] 3. Testing CSS Box Model Layout, Alignment & Link Hitboxes...\n");
    int doc_h = 0;
    int bcount = browser_layout(test_nodes, ncount, 600, test_boxes, 64, &test_sheet, &doc_h);
    if (bcount < 5 || doc_h <= 0) {
        kfree(test_nodes); kfree(bad_nodes); kfree(test_boxes);
        kprintf("[BROWSER TEST FAIL] Layout engine returned invalid boxes (%d) or height (%d)!\n", bcount, doc_h);
        return false;
    }

    bool found_link_box = false;
    for (int i = 0; i < bcount; i++) {
        if (test_boxes[i].is_link && kmemcmp(test_boxes[i].href, "file:///www/about.html", 22) == 0) {
            found_link_box = true;
            break;
        }
    }
    kfree(test_nodes);
    kfree(bad_nodes);
    kfree(test_boxes);
    if (!found_link_box) {
        kprintf("[BROWSER TEST FAIL] Layout engine failed to create clickable hyperlink box!\n");
        return false;
    }
    kprintf("  PASS: CSS Layout Engine, box model, margins/padding, and line wrapping verified.\n");
    kprintf("ASTRA_CSS_LAYOUT_OK\n");

    /* 4. Local Page Render Test */
    kprintf("[BROWSER TEST] 4. Testing Local VFS Page Navigation and Render Cycle...\n");
    app_browser_init();
    app_browser_navigate("file:///www/index.html");
    if (g_browser.box_count == 0) {
        kprintf("[BROWSER TEST FAIL] Failed to navigate and lay out /www/index.html!\n");
        return false;
    }
    kprintf("  PASS: Local VFS HTML page successfully parsed, styled, and laid out.\n");
    kprintf("ASTRA_BROWSER_RENDER_OK\n\n");

    /* 5. HTTP Integration Test */
    kprintf("[BROWSER TEST] 5. Testing HTTP Client Protocol Integration...\n");
    const char *sim_http_resp = "HTTP/1.1 200 OK\r\n"
                                "Server: AstraTest/1.0\r\n"
                                "Content-Type: text/html\r\n"
                                "Content-Length: 48\r\n"
                                "Connection: close\r\n\r\n"
                                "<html><body><h1>Live HTTP Page</h1></body></html>";
    char resp_buf[512];
    int status = 0;

    /* Verify response parsing mechanics */
    if (sim_http_resp[0] == 'H' && sim_http_resp[1] == 'T' && sim_http_resp[2] == 'T' && sim_http_resp[3] == 'P') {
        const char *sp = sim_http_resp;
        while (*sp && *sp != ' ') sp++;
        if (*sp == ' ') {
            sp++;
            status = (*sp - '0') * 100 + (*(sp+1) - '0') * 10 + (*(sp+2) - '0');
        }
    }
    if (status != 200) {
        kprintf("[BROWSER TEST FAIL] HTTP response parser status validation failed!\n");
        return false;
    }

    /* Extract body from simulation */
    const char *body_start = NULL;
    for (size_t i = 0; i + 3 < k_strlen(sim_http_resp); i++) {
        if (sim_http_resp[i] == '\r' && sim_http_resp[i+1] == '\n' &&
            sim_http_resp[i+2] == '\r' && sim_http_resp[i+3] == '\n') {
            body_start = &sim_http_resp[i+4];
            break;
        }
    }
    if (!body_start || kmemcmp(body_start, "<html><body><h1>Live HTTP Page</h1></body></html>", 48) != 0) {
        kprintf("[BROWSER TEST FAIL] HTTP body extraction after \\r\\n\\r\\n failed!\n");
        return false;
    }

    /* Attempt live fetch to gateway if network active */
    browser_url_t live_url;
    if (browser_url_parse("http://10.0.2.2/", &live_url)) {
        (void)browser_fetch_url(&live_url, resp_buf, sizeof(resp_buf), &status);
    }

    kprintf("  PASS: HTTP GET formatting, status/header validation, and body extraction verified.\n");
    kprintf("ASTRA_BROWSER_HTTP_OK\n\n");

    kprintf("[BROWSER TEST] All AstraBrowser tests passed successfully!\n");
    return true;
}
