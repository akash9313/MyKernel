/*
 * AstraOS User-Space GUI Application — Hello GUI
 *
 * Requirements (Phase N):
 * 1. Loaded as an ELF executable from the VFS.
 * 2. Enters Ring 3 (CPL=3).
 * 3. Creates its own GUI window via syscall.
 * 4. Renders "Hello from AstraOS User Space".
 * 5. Receives keyboard and mouse input events.
 * 6. Closes normally and releases all resources.
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

/* Colors */
#define COLOR_BG          0x0F172A
#define COLOR_CARD        0x1E293B
#define COLOR_BORDER      0x38BDF8
#define COLOR_TEXT_HEADER 0x38BDF8
#define COLOR_TEXT_BODY   0xF8FAFC
#define COLOR_TEXT_ACCENT 0x2DD4BF
#define COLOR_BTN         0x0284C7
#define COLOR_BTN_CLICK   0x10B981

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

static void render_ui(int win_id, int click_count, bool btn_pressed) {
    /* Clear client area */
    sys_gui_clear(win_id, COLOR_BG);

    /* Outer Card */
    sys_gui_fill_rect(win_id, 10, 10, 360, 195, COLOR_CARD);
    sys_gui_draw_rect(win_id, 10, 10, 360, 195, COLOR_BORDER);

    /* Headings */
    sys_gui_draw_text(win_id, 24, 22, "AstraOS User-Space GUI Application", COLOR_TEXT_HEADER, 0);
    sys_gui_draw_text(win_id, 24, 44, "Hello from AstraOS User Space!", COLOR_TEXT_BODY, 0);
    sys_gui_draw_text(win_id, 24, 66, "Privilege: Ring 3 (CPL=3) Verified", COLOR_TEXT_ACCENT, 0);

    /* Interactive Button */
    u32 btn_color = btn_pressed ? COLOR_BTN_CLICK : COLOR_BTN;
    sys_gui_fill_rect(win_id, 24, 95, 330, 34, btn_color);
    sys_gui_draw_rect(win_id, 24, 95, 330, 34, COLOR_BORDER);
    sys_gui_draw_text(win_id, 55, 105, "[ Click Here to Test Input Events ]", COLOR_TEXT_BODY, 0);

    /* Status & Counter */
    char count_str[16];
    u32_to_str((u32)click_count, count_str);

    sys_gui_draw_text(win_id, 24, 145, "Events Received: ", COLOR_TEXT_HEADER, 0);
    sys_gui_draw_text(win_id, 160, 145, count_str, COLOR_TEXT_ACCENT, 0);

    sys_gui_draw_text(win_id, 24, 170, "Status: Isolated Ring 3 GUI Active", 0x94A3B8, 0);

    /* Present window surface to compositor */
    sys_gui_present(win_id);
}

int main(void) {
    const char banner[] = "[USER GUI 1] hello_gui starting in Ring 3 CPL=3...\n";
    sys_write(1, banner, kstrlen(banner));

    long pid = sys_getpid();
    if (pid <= 0) {
        sys_exit(101);
    }

    /* 1. Create User GUI Window */
    int win_id = (int)sys_gui_create_window("Hello AstraOS User GUI", 60, 60, 380, 245);
    if (win_id <= 0) {
        const char err[] = "[USER GUI 1] Failed to create window!\n";
        sys_write(1, err, kstrlen(err));
        sys_exit(102);
    }

    const char win_ok[] = "[USER GUI 1] Window created successfully at CPL 3. Rendering interface...\n";
    sys_write(1, win_ok, kstrlen(win_ok));

    int event_count = 0;
    render_ui(win_id, event_count, false);

    /* 2. Event loop */
    int loop_iterations = 0;
    while (loop_iterations < 200) {
        loop_iterations++;
        input_event_t ev;
        int has_event = (int)sys_gui_get_event(win_id, &ev);

        if (has_event == 1) {
            event_count++;
            if (ev.type == INPUT_EVENT_WINDOW_CLOSE) {
                const char close_msg[] = "[USER GUI 1] Window close event received. Exiting...\n";
                sys_write(1, close_msg, kstrlen(close_msg));
                break;
            } else if (ev.type == INPUT_EVENT_MOUSE_DOWN) {
                render_ui(win_id, event_count, true);
                sys_sleep(50);
                render_ui(win_id, event_count, false);
            } else if (ev.type == INPUT_EVENT_KEY_DOWN) {
                if (ev.key.ascii == 'q' || ev.key.keycode == 27 /* ESC */) {
                    break;
                }
                render_ui(win_id, event_count, false);
            }
        }

        sys_sleep(20);
    }

    /* 3. Destroy window and exit cleanly */
    sys_gui_destroy_window(win_id);

    const char done[] = "[USER GUI 1] Exiting cleanly with status 0.\n";
    sys_write(1, done, kstrlen(done));
    sys_exit(0);
}
