#ifndef ASTRA_KERNEL_INPUT_H
#define ASTRA_KERNEL_INPUT_H

#include <kernel/types.h>

#define INPUT_QUEUE_CAPACITY 256

/* Event Types */
typedef enum {
    INPUT_EVENT_NONE = 0,
    INPUT_EVENT_KEY_DOWN,
    INPUT_EVENT_KEY_UP,
    INPUT_EVENT_MOUSE_MOVE,
    INPUT_EVENT_MOUSE_DOWN,
    INPUT_EVENT_MOUSE_UP,
    INPUT_EVENT_WINDOW_CLOSE
} input_event_type_t;

/* Key Modifiers */
#define KEY_MOD_SHIFT (1 << 0)
#define KEY_MOD_CTRL  (1 << 1)
#define KEY_MOD_ALT   (1 << 2)
#define KEY_MOD_CAPS  (1 << 3)

/* Special Keycodes */
#define KEY_SPECIAL_BASE 0x100
#define KEY_BACKSPACE    (KEY_SPECIAL_BASE + 1)
#define KEY_TAB          (KEY_SPECIAL_BASE + 2)
#define KEY_ENTER        (KEY_SPECIAL_BASE + 3)
#define KEY_ESC          (KEY_SPECIAL_BASE + 4)
#define KEY_UP           (KEY_SPECIAL_BASE + 5)
#define KEY_DOWN         (KEY_SPECIAL_BASE + 6)
#define KEY_LEFT         (KEY_SPECIAL_BASE + 7)
#define KEY_RIGHT        (KEY_SPECIAL_BASE + 8)
#define KEY_LSHIFT       (KEY_SPECIAL_BASE + 9)
#define KEY_RSHIFT       (KEY_SPECIAL_BASE + 10)
#define KEY_LCTRL        (KEY_SPECIAL_BASE + 11)
#define KEY_LALT         (KEY_SPECIAL_BASE + 12)

/* Mouse Buttons */
#define MOUSE_BTN_LEFT   (1 << 0)
#define MOUSE_BTN_RIGHT  (1 << 1)
#define MOUSE_BTN_MIDDLE (1 << 2)

typedef struct input_event {
    input_event_type_t type;
    union {
        struct {
            u32 keycode;    /* ASCII code or KEY_* special constant */
            char ascii;     /* Resolved ASCII character (0 if non-printable) */
            u8 modifiers;   /* KEY_MOD_* bitmask */
        } key;
        struct {
            int x;          /* Absolute screen X */
            int y;          /* Absolute screen Y */
            int delta_x;    /* Relative X movement */
            int delta_y;    /* Relative Y movement */
            u8 buttons;     /* MOUSE_BTN_* bitmask */
        } mouse;
    };
} input_event_t;

/* Core Input Queue API (Interrupt Safe) */
void input_init(int screen_width, int screen_height);
bool input_push_event(const input_event_t *ev);
bool input_pop_event(input_event_t *out_ev);
bool input_has_event(void);
void input_get_mouse_state(int *x, int *y, u8 *buttons);
void input_feed_serial_char(char c);

/* PS/2 Driver Integration */
void ps2_init(void);
void ps2_handle_keyboard_irq(void);
void ps2_handle_mouse_irq(void);
void ps2_poll(void);

/* Input Subsystem Self-Test */
bool input_self_test(void);

#endif /* ASTRA_KERNEL_INPUT_H */
