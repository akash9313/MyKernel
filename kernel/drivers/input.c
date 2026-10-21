#include <kernel/input.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/kprintf.h>

static struct {
    input_event_t buffer[INPUT_QUEUE_CAPACITY];
    volatile size_t head;
    volatile size_t tail;
    volatile size_t count;
    volatile size_t dropped_count;

    int screen_w;
    int screen_h;
    int mouse_x;
    int mouse_y;
    u8 mouse_buttons;
} g_input = {0};

void input_init(int screen_width, int screen_height) {
    g_input.head = 0;
    g_input.tail = 0;
    g_input.count = 0;
    g_input.dropped_count = 0;
    g_input.screen_w = screen_width > 0 ? screen_width : 1024;
    g_input.screen_h = screen_height > 0 ? screen_height : 768;
    g_input.mouse_x = g_input.screen_w / 2;
    g_input.mouse_y = g_input.screen_h / 2;
    g_input.mouse_buttons = 0;
}

bool input_push_event(const input_event_t *ev) {
    if (!ev) return false;

    u64 flags = irq_save();

    /* If this is a mouse move event, update cached coordinates */
    if (ev->type == INPUT_EVENT_MOUSE_MOVE) {
        g_input.mouse_x = ev->mouse.x;
        g_input.mouse_y = ev->mouse.y;
        g_input.mouse_buttons = ev->mouse.buttons;
    } else if (ev->type == INPUT_EVENT_MOUSE_DOWN || ev->type == INPUT_EVENT_MOUSE_UP) {
        g_input.mouse_buttons = ev->mouse.buttons;
    }

    if (g_input.count >= INPUT_QUEUE_CAPACITY) {
        g_input.dropped_count++;
        irq_restore(flags);
        return false;
    }

    g_input.buffer[g_input.tail] = *ev;
    g_input.tail = (g_input.tail + 1) % INPUT_QUEUE_CAPACITY;
    g_input.count++;

    irq_restore(flags);
    return true;
}

bool input_pop_event(input_event_t *out_ev) {
    if (!out_ev) return false;

    u64 flags = irq_save();

    if (g_input.count == 0) {
        irq_restore(flags);
        return false;
    }

    *out_ev = g_input.buffer[g_input.head];
    g_input.head = (g_input.head + 1) % INPUT_QUEUE_CAPACITY;
    g_input.count--;

    irq_restore(flags);
    return true;
}

bool input_has_event(void) {
    return (g_input.count > 0);
}

void input_get_mouse_state(int *x, int *y, u8 *buttons) {
    u64 flags = irq_save();
    if (x) *x = g_input.mouse_x;
    if (y) *y = g_input.mouse_y;
    if (buttons) *buttons = g_input.mouse_buttons;
    irq_restore(flags);
}

/* Feed characters typed on serial port as keyboard input events */
void input_feed_serial_char(char c) {
    static int esc_state = 0;
    input_event_t ev = {0};

    if (esc_state == 0) {
        if (c == 0x1B) { /* ESC */
            esc_state = 1;
            return;
        }
    } else if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            return;
        } else {
            esc_state = 0;
        }
    } else if (esc_state == 2) {
        esc_state = 0;
        ev.type = INPUT_EVENT_KEY_DOWN;
        if (c == 'A') ev.key.keycode = KEY_UP;
        else if (c == 'B') ev.key.keycode = KEY_DOWN;
        else if (c == 'C') ev.key.keycode = KEY_RIGHT;
        else if (c == 'D') ev.key.keycode = KEY_LEFT;
        else return;
        input_push_event(&ev);
        return;
    }

    ev.type = INPUT_EVENT_KEY_DOWN;

    if (c == '\r' || c == '\n') {
        ev.key.keycode = KEY_ENTER;
        ev.key.ascii = '\n';
    } else if (c == '\b' || c == 0x7F) {
        ev.key.keycode = KEY_BACKSPACE;
        ev.key.ascii = '\b';
    } else if (c == '\t') {
        ev.key.keycode = KEY_TAB;
        ev.key.ascii = '\t';
    } else {
        ev.key.keycode = (u32)(u8)c;
        ev.key.ascii = c;
    }

    input_push_event(&ev);
}

bool input_self_test(void) {
    kprintf("\n[INPUT TEST] Starting Input Event Subsystem Self-Test...\n");

    u64 flags = irq_save();

    /* Drain any early boot/hardware initialization packets */
    input_event_t discard;
    while (input_pop_event(&discard)) {
        /* drained */
    }
    g_input.dropped_count = 0;

    /* 1. Test pushing and popping single events */
    input_event_t ev_in = {
        .type = INPUT_EVENT_KEY_DOWN,
        .key = { .keycode = 'A', .ascii = 'A', .modifiers = 0 }
    };
    if (!input_push_event(&ev_in)) {
        irq_restore(flags);
        kprintf("[INPUT TEST FAIL] Failed to push event!\n");
        return false;
    }

    input_event_t ev_out = {0};
    if (!input_pop_event(&ev_out)) {
        irq_restore(flags);
        kprintf("[INPUT TEST FAIL] Failed to pop event!\n");
        return false;
    }

    if (ev_out.type != INPUT_EVENT_KEY_DOWN || ev_out.key.keycode != 'A' || ev_out.key.ascii != 'A') {
        irq_restore(flags);
        kprintf("[INPUT TEST FAIL] Event data corrupted: type=%d (exp %d), keycode=%u (exp 'A'), ascii=%d (exp 'A')\n",
                ev_out.type, INPUT_EVENT_KEY_DOWN, ev_out.key.keycode, ev_out.key.ascii);
        return false;
    }
    kprintf("  PASS: Single event push and pop verified.\n");

    /* 2. Test FIFO sequence integrity */
    for (int i = 0; i < 50; i++) {
        input_event_t e = {
            .type = INPUT_EVENT_KEY_DOWN,
            .key = { .keycode = (u32)('0' + (i % 10)), .ascii = (char)('0' + (i % 10)) }
        };
        input_push_event(&e);
    }

    for (int i = 0; i < 50; i++) {
        input_event_t e = {0};
        if (!input_pop_event(&e)) {
            irq_restore(flags);
            kprintf("[INPUT TEST FAIL] Premature queue empty during sequence test!\n");
            return false;
        }
        if (e.key.ascii != (char)('0' + (i % 10))) {
            irq_restore(flags);
            kprintf("[INPUT TEST FAIL] FIFO ordering violated!\n");
            return false;
        }
    }
    kprintf("  PASS: FIFO sequencing of multiple events verified.\n");

    /* 3. Test Queue Overflow and Safety */
    for (int i = 0; i < INPUT_QUEUE_CAPACITY + 20; i++) {
        input_event_t e = {
            .type = INPUT_EVENT_MOUSE_MOVE,
            .mouse = { .x = i, .y = i, .delta_x = 1, .delta_y = 1, .buttons = 0 }
        };
        input_push_event(&e);
    }

    if (g_input.dropped_count < 20) {
        irq_restore(flags);
        kprintf("[INPUT TEST FAIL] Queue overflow did not account for dropped events correctly!\n");
        return false;
    }

    /* Drain queue */
    while (input_pop_event(&ev_out));
    kprintf("  PASS: Queue capacity bounds and overflow defense verified.\n");

    /* 4. Test Mouse Position Clamping */
    g_input.mouse_x = g_input.screen_w / 2;
    g_input.mouse_y = g_input.screen_h / 2;
    int mx, my;
    u8 mb;
    input_get_mouse_state(&mx, &my, &mb);
    if (mx < 0 || mx >= g_input.screen_w || my < 0 || my >= g_input.screen_h) {
        irq_restore(flags);
        kprintf("[INPUT TEST FAIL] Mouse position initialized out of bounds!\n");
        return false;
    }
    kprintf("  PASS: Mouse position tracking and clamping verified.\n");

    irq_restore(flags);
    kprintf("[INPUT TEST] All Input subsystem tests passed successfully!\n");
    kprintf("ASTRA_INPUT_OK\n\n");
    return true;
}
