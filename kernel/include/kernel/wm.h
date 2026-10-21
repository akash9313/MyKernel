#ifndef ASTRA_KERNEL_WM_H
#define ASTRA_KERNEL_WM_H

#include <kernel/types.h>
#include <kernel/gfx.h>
#include <kernel/input.h>

#define WM_MAX_WINDOWS      16
#define WM_TITLEBAR_HEIGHT  26
#define WM_TASKBAR_HEIGHT   36
#define WM_BORDER_WIDTH     1
#define WM_EVENT_QUEUE_CAPACITY 32

typedef struct wm_window {
    int id;
    u32 owner_pid;          /* 0 for kernel-owned windows; >0 for user-space process */
    char title[64];
    int x;
    int y;
    int width;
    int height;
    
    int client_x;
    int client_y;
    int client_w;
    int client_h;

    bool visible;
    bool focused;
    bool minimized;
    bool is_dragging;
    int drag_offset_x;
    int drag_offset_y;

    u32 *client_buffer;
    gfx_surface_t client_surf;

    /* Per-window private event queue for user-space processes */
    input_event_t event_queue[WM_EVENT_QUEUE_CAPACITY];
    int event_head;
    int event_tail;
    int event_count;

    void (*on_paint)(struct wm_window *win, gfx_surface_t *s);
    void (*on_event)(struct wm_window *win, const input_event_t *ev);
    void *user_data;
} wm_window_t;

/* Window Manager Core API */
void wm_init(void);
wm_window_t *wm_create_window(const char *title, int x, int y, int w, int h);
wm_window_t *wm_create_window_owner(const char *title, int x, int y, int w, int h, u32 owner_pid);
void wm_destroy_window(wm_window_t *win);
void wm_destroy_all_for_pid(u32 pid);
bool wm_window_push_event(wm_window_t *win, const input_event_t *ev);
bool wm_window_pop_event(wm_window_t *win, input_event_t *out_ev);
void wm_bring_to_front(wm_window_t *win);
void wm_set_focus(wm_window_t *win);
wm_window_t *wm_get_focused(void);
wm_window_t *wm_find_by_id(int id);
wm_window_t *wm_window_at(int x, int y);

/* Compositor and Event Dispatch */
void wm_render(void);
void wm_handle_input_event(const input_event_t *ev);
bool wm_self_test(void);

/* Desktop Shell & Applications */
void desktop_init(void);
void desktop_render(void);
void desktop_handle_event(const input_event_t *ev);
bool desktop_self_test(void);

/* GUI Applications Launchers */
void app_editor_launch(const char *filepath);
void app_filemgr_launch(const char *initial_path);
bool app_gui_self_test(void);

#endif /* ASTRA_KERNEL_WM_H */
