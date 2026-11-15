#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/pmm.h>
#include <kernel/boot.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/drivers/apic.h>

static struct {
    wm_window_t windows[WM_MAX_WINDOWS];
    int window_count;
    int next_id;
    int focused_id;
} g_wm = {0};

static struct {
    char text[2048];
    size_t len;
} g_clipboard = {0};

void clipboard_set_text(const char *text, size_t len) {
    if (!text || len == 0) {
        g_clipboard.text[0] = '\0';
        g_clipboard.len = 0;
        return;
    }
    if (len >= sizeof(g_clipboard.text)) {
        len = sizeof(g_clipboard.text) - 1;
    }
    for (size_t i = 0; i < len; i++) {
        g_clipboard.text[i] = text[i];
    }
    g_clipboard.text[len] = '\0';
    g_clipboard.len = len;
}

const char *clipboard_get_text(size_t *out_len) {
    if (out_len) *out_len = g_clipboard.len;
    return g_clipboard.text;
}

static bool g_wm_initialized = false;

bool wm_is_initialized(void) {
    return g_wm_initialized;
}

int wm_get_window_count(void) {
    return g_wm.window_count;
}

void wm_init(void) {
    if (g_wm_initialized) {
        return; /* Initialize window manager exactly once */
    }
    kmemset(&g_wm, 0, sizeof(g_wm));
    g_wm.next_id = 1;
    g_wm.focused_id = 0;
    g_wm_initialized = true;
}

wm_window_t *wm_find_by_id(int id) {
    if (id <= 0) return NULL;
    for (int i = 0; i < g_wm.window_count; i++) {
        if (g_wm.windows[i].id == id) {
            return &g_wm.windows[i];
        }
    }
    return NULL;
}

wm_window_t *wm_find_by_title_substring(const char *sub) {
    if (!sub || !*sub) return NULL;
    for (int i = 0; i < g_wm.window_count; i++) {
        const char *h = g_wm.windows[i].title;
        for (int j = 0; h[j]; j++) {
            int k = 0;
            while (sub[k] && h[j + k] == sub[k]) k++;
            if (!sub[k]) return &g_wm.windows[i];
        }
    }
    return NULL;
}

wm_window_t *wm_get_focused(void) {
    return wm_find_by_id(g_wm.focused_id);
}

void wm_set_focus(wm_window_t *win) {
    if (!win) {
        g_wm.focused_id = 0;
        for (int i = 0; i < g_wm.window_count; i++) {
            g_wm.windows[i].focused = false;
        }
        return;
    }

    g_wm.focused_id = win->id;
    for (int i = 0; i < g_wm.window_count; i++) {
        g_wm.windows[i].focused = (g_wm.windows[i].id == win->id);
    }
}

void wm_bring_to_front(wm_window_t *win) {
    if (!win) return;

    if (g_wm.window_count <= 1) {
        wm_set_focus(win);
        return;
    }

    int idx = -1;
    for (int i = 0; i < g_wm.window_count; i++) {
        if (g_wm.windows[i].id == win->id) {
            idx = i;
            break;
        }
    }

    if (idx == -1 || idx == g_wm.window_count - 1) {
        wm_set_focus(win);
        return; /* Already at top */
    }

    wm_window_t target = g_wm.windows[idx];
    for (int i = idx; i < g_wm.window_count - 1; i++) {
        g_wm.windows[i] = g_wm.windows[i + 1];
    }
    g_wm.windows[g_wm.window_count - 1] = target;
    wm_set_focus(&g_wm.windows[g_wm.window_count - 1]);
}

wm_window_t *wm_window_at(int x, int y) {
    /* Search in reverse z-order (top-most windows first) */
    for (int i = g_wm.window_count - 1; i >= 0; i--) {
        wm_window_t *w = &g_wm.windows[i];
        if (!w->visible || w->minimized) continue;
        if (x >= w->x && x < w->x + w->width &&
            y >= w->y && y < w->y + w->height) {
            return w;
        }
    }
    return NULL;
}

wm_window_t *wm_create_window_owner(const char *title, int x, int y, int w, int h, pid_t owner_pid) {
    if (g_wm.window_count >= WM_MAX_WINDOWS) {
        kprintf("[WM WARN] Maximum window limit (%d) reached.\n", WM_MAX_WINDOWS);
        return NULL;
    }

    /* Enforce sane bounds */
    if (w < 160) w = 160;
    if (h < 80)  h = 80;

    int client_w = w - 2 * WM_BORDER_WIDTH;
    int client_h = h - WM_TITLEBAR_HEIGHT - WM_BORDER_WIDTH;
    size_t client_bytes = (size_t)client_w * client_h * sizeof(u32);
    size_t pages = (client_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    phys_addr_t buf_pa = 0;
    int pmm_res = pmm_alloc_contiguous(pages, &buf_pa);
    if (pmm_res != PMM_SUCCESS) {
        kprintf("[WM ERROR] Failed to allocate client surface memory (%llu bytes)\n",
                (unsigned long long)client_bytes);
        return NULL;
    }

    struct limine_hhdm_response *hhdm = boot_get_hhdm_response();
    u32 *client_va = (u32 *)(buf_pa + hhdm->offset);
    kmemset(client_va, 0, client_bytes);

    wm_window_t *win = &g_wm.windows[g_wm.window_count++];
    kmemset(win, 0, sizeof(wm_window_t));
    win->id = g_wm.next_id++;
    win->owner_pid = owner_pid;
    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;
    win->client_x = x + WM_BORDER_WIDTH;
    win->client_y = y + WM_TITLEBAR_HEIGHT;
    win->client_w = client_w;
    win->client_h = client_h;
    win->visible = true;
    win->focused = true;
    win->minimized = false;
    win->is_dragging = false;
    win->client_buffer = client_va;
    win->on_paint = NULL;
    win->on_event = NULL;
    win->user_data = NULL;
    win->event_head = 0;
    win->event_tail = 0;
    win->event_count = 0;

    /* Copy title safely */
    size_t ti = 0;
    if (title) {
        while (title[ti] && ti < sizeof(win->title) - 1) {
            win->title[ti] = title[ti];
            ti++;
        }
    }
    win->title[ti] = '\0';

    gfx_init_surface(&win->client_surf, client_va, client_w, client_h, client_w);

    int id = win->id;
    wm_bring_to_front(win);
    return wm_find_by_id(id);
}

wm_window_t *wm_create_window(const char *title, int x, int y, int w, int h) {
    return wm_create_window_owner(title, x, y, w, h, 0);
}

void wm_resize_window(wm_window_t *win, int new_w, int new_h) {
    if (!win) return;
    if (new_w < 200) new_w = 200;
    if (new_h < 120) new_h = 120;

    gfx_surface_t *bb = gfx_get_backbuffer();
    if (bb) {
        if (new_w > bb->width) new_w = bb->width;
        if (new_h > bb->height - WM_TITLEBAR_HEIGHT) new_h = bb->height - WM_TITLEBAR_HEIGHT;
    }

    int new_client_w = new_w - 2 * WM_BORDER_WIDTH;
    int new_client_h = new_h - WM_TITLEBAR_HEIGHT - WM_BORDER_WIDTH;

    size_t old_bytes = (size_t)win->client_w * win->client_h * sizeof(u32);
    size_t old_pages = (old_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    size_t new_bytes = (size_t)new_client_w * new_client_h * sizeof(u32);
    size_t new_pages = (new_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    struct limine_hhdm_response *hhdm = boot_get_hhdm_response();

    if (new_pages != old_pages || !win->client_buffer) {
        phys_addr_t new_pa = 0;
        int pmm_res = pmm_alloc_contiguous(new_pages, &new_pa);
        if (pmm_res != PMM_SUCCESS) {
            kprintf("[WM ERROR] Failed to reallocate client surface for resize\n");
            return;
        }

        u32 *new_va = (u32 *)(new_pa + hhdm->offset);
        kmemset(new_va, 0, new_bytes);

        if (win->client_buffer && hhdm) {
            phys_addr_t old_pa = (phys_addr_t)((uintptr_t)win->client_buffer - hhdm->offset);
            pmm_free_contiguous(old_pa, old_pages);
        }

        win->client_buffer = new_va;
    } else {
        kmemset(win->client_buffer, 0, new_bytes);
    }

    win->width = new_w;
    win->height = new_h;
    win->client_w = new_client_w;
    win->client_h = new_client_h;
    win->client_x = win->x + WM_BORDER_WIDTH;
    win->client_y = win->y + WM_TITLEBAR_HEIGHT;

    gfx_init_surface(&win->client_surf, win->client_buffer, win->client_w, win->client_h, win->client_w);

    if (win->on_paint) {
        win->on_paint(win, &win->client_surf);
    }
}

void wm_maximize_window(wm_window_t *win) {
    if (!win || win->maximized) return;

    win->saved_x = win->x;
    win->saved_y = win->y;
    win->saved_w = win->width;
    win->saved_h = win->height;
    win->maximized = true;

    gfx_surface_t *bb = gfx_get_backbuffer();
    int scr_w = bb ? bb->width : 1024;
    int scr_h = bb ? bb->height : 768;

    win->x = 0;
    win->y = 32; /* Below top panel */
    int max_h = scr_h - 32 - 58; /* Above bottom dock */
    if (max_h < 200) max_h = scr_h - 32;

    wm_resize_window(win, scr_w, max_h);
}

void wm_restore_window(wm_window_t *win) {
    if (!win || !win->maximized) return;

    win->maximized = false;
    win->x = win->saved_x;
    win->y = win->saved_y;
    int w = win->saved_w > 0 ? win->saved_w : 520;
    int h = win->saved_h > 0 ? win->saved_h : 340;

    wm_resize_window(win, w, h);
}

void wm_toggle_maximize(wm_window_t *win) {
    if (!win) return;
    if (win->maximized) {
        wm_restore_window(win);
    } else {
        wm_maximize_window(win);
    }
}

bool wm_window_push_event(wm_window_t *win, const input_event_t *ev) {
    if (!win || !ev) return false;
    if (win->event_count >= WM_EVENT_QUEUE_CAPACITY) {
        return false; /* Queue full */
    }
    win->event_queue[win->event_tail] = *ev;
    win->event_tail = (win->event_tail + 1) % WM_EVENT_QUEUE_CAPACITY;
    win->event_count++;
    return true;
}

bool wm_window_pop_event(wm_window_t *win, input_event_t *out_ev) {
    if (!win || !out_ev || win->event_count == 0) return false;
    *out_ev = win->event_queue[win->event_head];
    win->event_head = (win->event_head + 1) % WM_EVENT_QUEUE_CAPACITY;
    win->event_count--;
    return true;
}

void wm_destroy_all_for_pid(pid_t pid) {
    if (pid == 0) return;
    bool found = true;
    while (found) {
        found = false;
        for (int i = 0; i < g_wm.window_count; i++) {
            if (g_wm.windows[i].owner_pid == pid) {
                wm_destroy_window(&g_wm.windows[i]);
                found = true;
                break;
            }
        }
    }
}

void wm_destroy_window(wm_window_t *win) {
    if (!win) return;

    int idx = -1;
    for (int i = 0; i < g_wm.window_count; i++) {
        if (g_wm.windows[i].id == win->id) {
            idx = i;
            break;
        }
    }
    if (idx == -1) return;

    /* Free client buffer */
    if (win->client_buffer) {
        struct limine_hhdm_response *hhdm = boot_get_hhdm_response();
        phys_addr_t buf_pa = (phys_addr_t)((uintptr_t)win->client_buffer - hhdm->offset);
        size_t client_bytes = (size_t)win->client_w * win->client_h * sizeof(u32);
        size_t pages = (client_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        pmm_free_contiguous(buf_pa, pages);
        win->client_buffer = NULL;
    }

    /* Shift remaining windows in array */
    for (int i = idx; i < g_wm.window_count - 1; i++) {
        g_wm.windows[i] = g_wm.windows[i + 1];
    }
    g_wm.window_count--;

    /* Focus new top window */
    if (g_wm.window_count > 0) {
        wm_set_focus(&g_wm.windows[g_wm.window_count - 1]);
    } else {
        g_wm.focused_id = 0;
    }
}

void wm_handle_input_event(const input_event_t *ev) {
    if (!ev) return;

    if (ev->type == INPUT_EVENT_MOUSE_MOVE) {
        /* Check if any window is being resized */
        for (int i = 0; i < g_wm.window_count; i++) {
            wm_window_t *w = &g_wm.windows[i];
            if (w->is_resizing) {
                int dx = ev->mouse.x - w->resize_start_x;
                int dy = ev->mouse.y - w->resize_start_y;
                int nw = w->resize_orig_w + dx;
                int nh = w->resize_orig_h + dy;
                wm_resize_window(w, nw, nh);
                return;
            }
        }

        /* Check if any window is being dragged */
        for (int i = 0; i < g_wm.window_count; i++) {
            wm_window_t *w = &g_wm.windows[i];
            if (w->is_dragging) {
                w->x = ev->mouse.x - w->drag_offset_x;
                w->y = ev->mouse.y - w->drag_offset_y;

                /* Clamp to screen */
                gfx_surface_t *bb = gfx_get_backbuffer();
                if (bb) {
                    if (w->x < 0) w->x = 0;
                    if (w->y < 0) w->y = 0;
                    if (w->x + w->width > bb->width) w->x = bb->width - w->width;
                    if (w->y + w->height > bb->height - WM_TASKBAR_HEIGHT) {
                        w->y = bb->height - WM_TASKBAR_HEIGHT - w->height;
                    }
                }
                w->client_x = w->x + WM_BORDER_WIDTH;
                w->client_y = w->y + WM_TITLEBAR_HEIGHT;
                return;
            }
        }

        /* Forward to focused window client area if mouse is inside */
        wm_window_t *foc = wm_get_focused();
        if (foc) {
            if (ev->mouse.x >= foc->client_x && ev->mouse.x < foc->client_x + foc->client_w &&
                ev->mouse.y >= foc->client_y && ev->mouse.y < foc->client_y + foc->client_h) {
                if (foc->on_event) {
                    foc->on_event(foc, ev);
                }
                if (foc->owner_pid > 0) {
                    input_event_t cev = *ev;
                    cev.mouse.x = ev->mouse.x - foc->client_x;
                    cev.mouse.y = ev->mouse.y - foc->client_y;
                    wm_window_push_event(foc, &cev);
                }
            }
        }
    } else if (ev->type == INPUT_EVENT_MOUSE_DOWN) {
        wm_window_t *w = wm_window_at(ev->mouse.x, ev->mouse.y);
        if (w) {
            wm_bring_to_front(w);

            /* Check Close button [X] at top-right of titlebar */
            int btn_x = w->x + w->width - 22;
            int btn_y = w->y + 4;
            if (ev->mouse.x >= btn_x && ev->mouse.x < btn_x + 18 &&
                ev->mouse.y >= btn_y && ev->mouse.y < btn_y + 18) {
                if (w->owner_pid > 0) {
                    input_event_t close_ev;
                    kmemset(&close_ev, 0, sizeof(close_ev));
                    close_ev.type = INPUT_EVENT_WINDOW_CLOSE;
                    wm_window_push_event(w, &close_ev);
                    return;
                }
                wm_destroy_window(w);
                return;
            }

            /* Check Maximize / Restore button [+] (Green circle) */
            int max_btn_x = w->x + w->width - 44;
            if (ev->mouse.x >= max_btn_x && ev->mouse.x < max_btn_x + 18 &&
                ev->mouse.y >= btn_y && ev->mouse.y < btn_y + 18) {
                wm_toggle_maximize(w);
                return;
            }

            /* Check Minimize button [-] (Amber circle) */
            int min_x = w->x + w->width - 66;
            if (ev->mouse.x >= min_x && ev->mouse.x < min_x + 18 &&
                ev->mouse.y >= btn_y && ev->mouse.y < btn_y + 18) {
                w->minimized = true;
                w->focused = false;
                if (g_wm.window_count > 1) {
                    wm_set_focus(&g_wm.windows[g_wm.window_count - 2]);
                }
                return;
            }

            /* Check Resize Grip in bottom-right 16x16 corner */
            if (!w->maximized &&
                ev->mouse.x >= w->x + w->width - 16 && ev->mouse.x <= w->x + w->width &&
                ev->mouse.y >= w->y + w->height - 16 && ev->mouse.y <= w->y + w->height) {
                w->is_resizing = true;
                w->resize_start_x = ev->mouse.x;
                w->resize_start_y = ev->mouse.y;
                w->resize_orig_w = w->width;
                w->resize_orig_h = w->height;
                return;
            }

            /* Check Titlebar Click & Drag */
            if (ev->mouse.y >= w->y && ev->mouse.y < w->y + WM_TITLEBAR_HEIGHT) {
                static u64 s_last_title_click = 0;
                static int s_last_win_id = 0;
                u64 now_tick = apic_get_ticks();
                if (s_last_win_id == w->id && (now_tick - s_last_title_click < 40)) {
                    /* Double click on titlebar -> toggle maximize */
                    wm_toggle_maximize(w);
                    s_last_title_click = 0;
                    return;
                }
                s_last_title_click = now_tick;
                s_last_win_id = w->id;

                if (!w->maximized) {
                    w->is_dragging = true;
                    w->drag_offset_x = ev->mouse.x - w->x;
                    w->drag_offset_y = ev->mouse.y - w->y;
                }
                return;
            }

            /* Forward to client area */
            if (ev->mouse.x >= w->client_x && ev->mouse.x < w->client_x + w->client_w &&
                ev->mouse.y >= w->client_y && ev->mouse.y < w->client_y + w->client_h) {
                if (w->on_event) {
                    w->on_event(w, ev);
                }
                if (w->owner_pid > 0) {
                    input_event_t cev = *ev;
                    cev.mouse.x = ev->mouse.x - w->client_x;
                    cev.mouse.y = ev->mouse.y - w->client_y;
                    wm_window_push_event(w, &cev);
                }
            }
        }
    } else if (ev->type == INPUT_EVENT_MOUSE_UP) {
        for (int i = 0; i < g_wm.window_count; i++) {
            g_wm.windows[i].is_dragging = false;
            g_wm.windows[i].is_resizing = false;
        }
        wm_window_t *foc = wm_get_focused();
        if (foc) {
            if (foc->on_event) {
                foc->on_event(foc, ev);
            }
            if (foc->owner_pid > 0) {
                input_event_t cev = *ev;
                cev.mouse.x = ev->mouse.x - foc->client_x;
                cev.mouse.y = ev->mouse.y - foc->client_y;
                wm_window_push_event(foc, &cev);
            }
        }
    } else if (ev->type == INPUT_EVENT_KEY_DOWN || ev->type == INPUT_EVENT_KEY_UP) {
        wm_window_t *foc = wm_get_focused();
        if (foc) {
            if (foc->on_event) {
                foc->on_event(foc, ev);
            }
            if (foc->owner_pid > 0) {
                wm_window_push_event(foc, ev);
            }
        }
    }
}

void wm_render(void) {
    gfx_surface_t *bb = gfx_get_backbuffer();
    if (!bb) return;

    /* Render windows in z-order from back (0) to front (window_count - 1) */
    for (int i = 0; i < g_wm.window_count; i++) {
        wm_window_t *w = &g_wm.windows[i];
        if (!w->visible || w->minimized) continue;

        /* Window Outline and Outer Frame */
        u32 border_color = w->focused ? 0x38BDF8 : 0x223048;
        gfx_draw_rect(bb, w->x, w->y, w->width, w->height, border_color);

        /* Modern Title Bar (Sleek Dark Glass) */
        u32 tb_color = w->focused ? 0x0F172A : 0x182030;
        gfx_fill_rect(bb, w->x + 1, w->y + 1, w->width - 2, WM_TITLEBAR_HEIGHT - 1, tb_color);
        /* Subtle divider line between titlebar and client area */
        gfx_draw_line(bb, w->x + 1, w->y + WM_TITLEBAR_HEIGHT - 1, w->x + w->width - 2, w->y + WM_TITLEBAR_HEIGHT - 1, 0x1E293B);

        /* App mini-badge icon & title text */
        u32 badge_color = w->focused ? 0x38BDF8 : 0x64748B;
        gfx_fill_rounded_rect(bb, w->x + 8, w->y + 7, 10, 10, 3, badge_color);
        u32 title_text_color = w->focused ? 0xF8FAFC : 0x94A3B8;
        gfx_draw_string(bb, w->x + 24, w->y + 5, w->title, title_text_color, tb_color, true);

        /* Modern Traffic-Light Circular Window Controls */
        /* 1. Minimize Button (Amber Gold Circle with minus) */
        int min_x = w->x + w->width - 66;
        int min_y = w->y + 4;
        gfx_fill_rounded_rect(bb, min_x + 2, min_y + 2, 14, 14, 7, 0xF59E0B);
        gfx_draw_rounded_rect(bb, min_x + 2, min_y + 2, 14, 14, 7, 0xD97706);
        gfx_draw_line(bb, min_x + 5, min_y + 9, min_x + 13, min_y + 9, 0x78350F);

        /* 2. Maximize / Restore Button (Emerald Green Circle with square) */
        int max_btn_x = w->x + w->width - 44;
        int max_btn_y = w->y + 4;
        gfx_fill_rounded_rect(bb, max_btn_x + 2, max_btn_y + 2, 14, 14, 7, 0x10B981);
        gfx_draw_rounded_rect(bb, max_btn_x + 2, max_btn_y + 2, 14, 14, 7, 0x059669);
        if (w->maximized) {
            gfx_draw_rect(bb, max_btn_x + 6, max_btn_y + 6, 6, 6, 0x064E3B);
        } else {
            gfx_draw_rect(bb, max_btn_x + 5, max_btn_y + 5, 8, 8, 0x064E3B);
        }

        /* 3. Close Button (Coral Red Circle with cross) */
        int close_x = w->x + w->width - 22;
        int close_y = w->y + 4;
        gfx_fill_rounded_rect(bb, close_x + 2, close_y + 2, 14, 14, 7, 0xEF4444);
        gfx_draw_rounded_rect(bb, close_x + 2, close_y + 2, 14, 14, 7, 0xDC2626);
        gfx_draw_line(bb, close_x + 6, close_y + 6, close_x + 12, close_y + 12, 0x7F1D1D);
        gfx_draw_line(bb, close_x + 12, close_y + 6, close_x + 6, close_y + 12, 0x7F1D1D);

        /* Resize Grip in bottom-right corner when not maximized */
        if (!w->maximized) {
            int rx = w->x + w->width - 12;
            int ry = w->y + w->height - 12;
            u32 grip_col = w->focused ? 0x64748B : 0x334155;
            gfx_draw_line(bb, rx + 8, ry + 2, rx + 2, ry + 8, grip_col);
            gfx_draw_line(bb, rx + 8, ry + 5, rx + 5, ry + 8, grip_col);
            gfx_draw_line(bb, rx + 8, ry + 8, rx + 8, ry + 8, grip_col);
        }

        /* Paint client content */
        if (w->on_paint) {
            w->on_paint(w, &w->client_surf);
        }

        /* Blit client surface into backbuffer */
        gfx_blit(bb, w->client_x, w->client_y, &w->client_surf, 0, 0, w->client_w, w->client_h);
    }
}

bool wm_self_test(void) {
    kprintf("\n[WM TEST] Starting Window Manager Subsystem Self-Test...\n");
    wm_init();

    /* 1. Test Window Creation */
    kprintf("[WM TEST] 1. Creating windows and verifying properties...\n");
    wm_window_t *w1 = wm_create_window("Window 1", 100, 100, 300, 200);
    if (!w1 || w1->width != 300 || w1->height != 200) {
        kprintf("[WM TEST FAIL] Failed to create Window 1 correctly!\n");
        return false;
    }
    int id1 = w1->id;
    if (!w1->focused) {
        kprintf("[WM TEST FAIL] Newly created window is not focused!\n");
        return false;
    }

    wm_window_t *w2 = wm_create_window("Window 2", 150, 150, 400, 250);
    if (!w2) {
        kprintf("[WM TEST FAIL] Failed to create Window 2!\n");
        return false;
    }
    int id2 = w2->id;
    if (!w2->focused || w1->focused) {
        kprintf("[WM TEST FAIL] Z-order focus mismatch after creating Window 2!\n");
        return false;
    }
    kprintf("  PASS: Multiple window creation and initial focus verified.\n");

    /* 2. Test Z-Order and Focus Elevation */
    kprintf("[WM TEST] 2. Testing Z-order elevation (bring to front)...\n");
    w1 = wm_find_by_id(id1);
    wm_bring_to_front(w1);
    w1 = wm_find_by_id(id1);
    w2 = wm_find_by_id(id2);
    if (!w1 || !w2 || !w1->focused || w2->focused || g_wm.windows[g_wm.window_count - 1].id != id1) {
        kprintf("[WM TEST FAIL] Failed to bring Window 1 to front of Z-order!\n");
        return false;
    }
    kprintf("  PASS: Z-order elevation and active focus switching verified.\n");

    /* 3. Test Hit Testing */
    kprintf("[WM TEST] 3. Testing window coordinate hit testing...\n");
    wm_window_t *hit = wm_window_at(120, 120);
    if (!hit || hit->id != id1) {
        kprintf("[WM TEST FAIL] Hit testing returned incorrect window!\n");
        return false;
    }
    kprintf("  PASS: Window spatial hit-testing verified.\n");

    /* 4. Test Dragging and Move */
    kprintf("[WM TEST] 4. Testing window titlebar drag operations...\n");
    input_event_t md = {
        .type = INPUT_EVENT_MOUSE_DOWN,
        .mouse = { .x = 120, .y = 110, .buttons = MOUSE_BTN_LEFT }
    };
    wm_handle_input_event(&md);
    w1 = wm_find_by_id(id1);
    if (!w1 || !w1->is_dragging) {
        kprintf("[WM TEST FAIL] Titlebar click did not initiate drag!\n");
        return false;
    }

    input_event_t mm = {
        .type = INPUT_EVENT_MOUSE_MOVE,
        .mouse = { .x = 180, .y = 170, .buttons = MOUSE_BTN_LEFT }
    };
    wm_handle_input_event(&mm);
    w1 = wm_find_by_id(id1);
    if (!w1 || w1->x != 160 || w1->y != 160) {
        kprintf("[WM TEST FAIL] Window dragging did not update coordinates accurately!\n");
        return false;
    }

    input_event_t mu = {
        .type = INPUT_EVENT_MOUSE_UP,
        .mouse = { .x = 180, .y = 170, .buttons = 0 }
    };
    wm_handle_input_event(&mu);
    w1 = wm_find_by_id(id1);
    if (!w1 || w1->is_dragging) {
        kprintf("[WM TEST FAIL] Mouse up did not terminate drag!\n");
        return false;
    }
    kprintf("  PASS: Titlebar window dragging and coordinate translation verified.\n");

    /* 4b. Test Window Resize and Maximize / Restore */
    kprintf("[WM TEST] 4b. Testing window resize, maximize, and restore...\n");
    wm_resize_window(w1, 350, 220);
    if (!w1 || w1->width != 350 || w1->height != 220) {
        kprintf("[WM TEST FAIL] Window resize failed!\n");
        return false;
    }
    wm_maximize_window(w1);
    if (!w1 || !w1->maximized) {
        kprintf("[WM TEST FAIL] Window maximize failed!\n");
        return false;
    }
    wm_restore_window(w1);
    if (!w1 || w1->maximized || w1->width != 350 || w1->height != 220) {
        kprintf("[WM TEST FAIL] Window restore failed!\n");
        return false;
    }
    kprintf("  PASS: Window resize, maximize, and restore verified.\n");

    /* 5. Test Window Destruction & Resource Cleanup */
    kprintf("[WM TEST] 5. Testing window destruction and resource cleanup...\n");
    w1 = wm_find_by_id(id1);
    wm_destroy_window(w1);
    if (g_wm.window_count != 1 || wm_find_by_id(id1) != NULL) {
        kprintf("[WM TEST FAIL] Window 1 destruction did not clean up window list!\n");
        return false;
    }
    w2 = wm_find_by_id(id2);
    if (!w2 || !w2->focused) {
        kprintf("[WM TEST FAIL] Focus was not automatically passed to next window!\n");
        return false;
    }
    wm_destroy_window(w2);
    if (g_wm.window_count != 0) {
        kprintf("[WM TEST FAIL] Remaining window destruction failed!\n");
        return false;
    }
    kprintf("  PASS: Window destruction and resource cleanup verified.\n");

    kprintf("[WM TEST] All Window Manager tests passed successfully!\n");
    kprintf("ASTRA_WINDOW_MANAGER_OK\n\n");
    return true;
}
