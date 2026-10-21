/*
 * AstraOS User-Space GUI Application 2 — Counter GUI
 *
 * Demonstrates independent execution of a second Ring 3 GUI process
 * running concurrently in its own isolated address space with its own window.
 */

typedef unsigned long size_t;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long s64;
typedef _Bool bool;
#define true 1
#define false 0

#define NULL ((void*)0)

/* Input Event Types */
typedef enum {
    INPUT_EVENT_NONE = 0,
    INPUT_EVENT_KEY_DOWN,
    INPUT_EVENT_KEY_UP,
    INPUT_EVENT_MOUSE_MOVE,
    INPUT_EVENT_MOUSE_DOWN,
    INPUT_EVENT_MOUSE_UP,
    INPUT_EVENT_WINDOW_CLOSE
} input_event_type_t;

typedef struct input_event {
    input_event_type_t type;
    union {
        struct {
            u32 keycode;
            char ascii;
            u8 modifiers;
        } key;
        struct {
            int x;
            int y;
            int delta_x;
            int delta_y;
            u8 buttons;
        } mouse;
    };
} input_event_t;

/* Syscall Prototypes */
s64 sys_write(int fd, const void *buf, size_t count);
s64 sys_yield(void);
s64 sys_getpid(void);
void sys_exit(int status) __attribute__((noreturn));

s64 sys_gui_create_window(const char *title, int x, int y, int w, int h);
s64 sys_gui_destroy_window(int id);
s64 sys_gui_get_event(int win_id, void *ev);
s64 sys_gui_present(int win_id);
s64 sys_gui_draw_rect(int win_id, int x, int y, int w, int h, u32 color);
s64 sys_gui_fill_rect(int win_id, int x, int y, int w, int h, u32 color);
s64 sys_gui_draw_text(int win_id, int x, int y, const char *text, u32 fg, u32 bg);
s64 sys_gui_clear(int win_id, u32 color);
s64 sys_gui_set_title(int win_id, const char *title);
s64 sys_sleep(u32 ms);

#define COLOR_BG          0x0A0F1D
#define COLOR_PANEL       0x111C35
#define COLOR_BORDER      0xF59E0B
#define COLOR_TEXT_ACCENT 0xF59E0B
#define COLOR_TEXT_WHITE  0xFFFFFF
#define COLOR_BTN         0x0D9488
#define COLOR_PROGRESS    0x10B981

static size_t kstrlen(const char *s) {
    size_t l = 0;
    while (s && s[l]) l++;
    return l;
}

static void u32_to_str(u32 val, char *buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    char tmp[16];
    int i = 0;
    while (val > 0) {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

static void draw_counter_ui(int win_id, u32 ticks, u32 clicks) {
    sys_gui_clear(win_id, COLOR_BG);

    /* Panel Card */
    sys_gui_fill_rect(win_id, 10, 10, 330, 195, COLOR_PANEL);
    sys_gui_draw_rect(win_id, 10, 10, 330, 195, COLOR_BORDER);

    /* Title */
    sys_gui_draw_text(win_id, 24, 20, "Process 2: Multi-Process GUI", COLOR_TEXT_ACCENT, 0);
    sys_gui_draw_text(win_id, 24, 42, "Concurrent User Space Application", COLOR_TEXT_WHITE, 0);

    /* Dynamic Tick Counter */
    char tbuf[16];
    u32_to_str(ticks, tbuf);
    sys_gui_draw_text(win_id, 24, 75, "Scheduler Ticks: ", COLOR_TEXT_WHITE, 0);
    sys_gui_draw_text(win_id, 160, 75, tbuf, COLOR_TEXT_ACCENT, 0);

    /* Dynamic Click Counter */
    char cbuf[16];
    u32_to_str(clicks, cbuf);
    sys_gui_draw_text(win_id, 24, 100, "Window Clicks:   ", COLOR_TEXT_WHITE, 0);
    sys_gui_draw_text(win_id, 160, 100, cbuf, COLOR_PROGRESS, 0);

    /* Progress bar visual */
    int bar_w = (int)((ticks * 10) % 290);
    sys_gui_draw_rect(win_id, 24, 130, 290, 16, 0x334155);
    if (bar_w > 0) {
        sys_gui_fill_rect(win_id, 25, 131, bar_w, 14, COLOR_BTN);
    }

    sys_gui_draw_text(win_id, 24, 165, "Isolation: Separate Address Space", 0x94A3B8, 0);

    sys_gui_present(win_id);
}

int main(void) {
    const char banner[] = "[USER GUI 2] counter_gui starting in concurrent Ring 3 process...\n";
    sys_write(1, banner, kstrlen(banner));

    long pid = sys_getpid();
    if (pid <= 0) {
        sys_exit(201);
    }

    /* Create independent window */
    int win_id = (int)sys_gui_create_window("AstraOS Process 2: Counter", 470, 70, 350, 245);
    if (win_id <= 0) {
        const char err[] = "[USER GUI 2] Failed to create window!\n";
        sys_write(1, err, kstrlen(err));
        sys_exit(202);
    }

    const char win_ok[] = "[USER GUI 2] Window created successfully at CPL 3. Entering event loop...\n";
    sys_write(1, win_ok, kstrlen(win_ok));

    u32 ticks = 0;
    u32 clicks = 0;
    draw_counter_ui(win_id, ticks, clicks);

    int loops = 0;
    while (loops < 150) {
        loops++;
        ticks++;

        input_event_t ev;
        int has_event = (int)sys_gui_get_event(win_id, &ev);
        if (has_event == 1) {
            if (ev.type == INPUT_EVENT_WINDOW_CLOSE) {
                const char close_msg[] = "[USER GUI 2] Window close event received. Exiting...\n";
                sys_write(1, close_msg, kstrlen(close_msg));
                break;
            } else if (ev.type == INPUT_EVENT_MOUSE_DOWN) {
                clicks++;
            }
        }

        draw_counter_ui(win_id, ticks, clicks);
        sys_sleep(30);
    }

    sys_gui_destroy_window(win_id);
    const char done[] = "[USER GUI 2] Exiting cleanly with status 0.\n";
    sys_write(1, done, kstrlen(done));
    sys_exit(0);
}
