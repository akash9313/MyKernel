#include <kernel/input.h>
#include <kernel/arch/x86_64/cpu.h>
#include <kernel/kprintf.h>
#include <kernel/drivers/apic.h>

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

/* Status Register Bits */
#define PS2_STATUS_OUTPUT_FULL (1 << 0)
#define PS2_STATUS_INPUT_FULL  (1 << 1)
#define PS2_STATUS_MOUSE_DATA  (1 << 5)

/* Keyboard State */
static bool g_shift = false;
static bool g_ctrl = false;
static bool g_alt = false;
static bool g_caps = false;
static bool g_extended = false;

/* Mouse State */
static u8 g_mouse_cycle = 0;
static u8 g_mouse_packet[3] = {0};
static u8 g_prev_buttons = 0;

/* PS/2 Scan Code Set 1 Table (Unshifted and Shifted) */
static const char g_scancode_unshifted[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

static const char g_scancode_shifted[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

static inline void ps2_wait_write(void) {
    int timeout = 100000;
    while ((inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) && --timeout) {
        io_wait();
    }
}

static inline bool ps2_wait_read(void) {
    int timeout = 100000;
    while (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) && --timeout) {
        io_wait();
    }
    return (timeout > 0);
}

static void ps2_write_device(u8 cmd) {
    ps2_wait_write();
    outb(PS2_DATA_PORT, cmd);
}

static void ps2_write_mouse(u8 cmd) {
    ps2_wait_write();
    outb(PS2_CMD_PORT, 0xD4); /* Direct next byte to mouse */
    ps2_wait_write();
    outb(PS2_DATA_PORT, cmd);
}

static u8 ps2_read_data(void) {
    ps2_wait_read();
    return inb(PS2_DATA_PORT);
}

static void ps2_process_keyboard_byte(u8 scancode) {
    if (scancode == 0xE0) {
        g_extended = true;
        return;
    }

    bool key_up = (scancode & 0x80) != 0;
    u8 code = scancode & 0x7F;

    if (g_extended) {
        g_extended = false;
        input_event_t ev = {
            .type = key_up ? INPUT_EVENT_KEY_UP : INPUT_EVENT_KEY_DOWN
        };
        switch (code) {
            case 0x48: ev.key.keycode = KEY_UP; break;
            case 0x50: ev.key.keycode = KEY_DOWN; break;
            case 0x4B: ev.key.keycode = KEY_LEFT; break;
            case 0x4D: ev.key.keycode = KEY_RIGHT; break;
            default: return;
        }
        input_push_event(&ev);
        return;
    }

    /* Track Modifiers */
    if (code == 0x2A || code == 0x36) {
        g_shift = !key_up;
        return;
    } else if (code == 0x1D) {
        g_ctrl = !key_up;
        return;
    } else if (code == 0x38) {
        g_alt = !key_up;
        return;
    } else if (code == 0x3A && !key_up) {
        g_caps = !g_caps;
        return;
    }

    /* Standard keys */
    if (code < 128) {
        input_event_t ev = {
            .type = key_up ? INPUT_EVENT_KEY_UP : INPUT_EVENT_KEY_DOWN
        };
        u8 mods = 0;
        if (g_shift) mods |= KEY_MOD_SHIFT;
        if (g_ctrl)  mods |= KEY_MOD_CTRL;
        if (g_alt)   mods |= KEY_MOD_ALT;
        if (g_caps)  mods |= KEY_MOD_CAPS;
        ev.key.modifiers = mods;

        char c = (g_shift ^ g_caps) ? g_scancode_shifted[code] : g_scancode_unshifted[code];
        ev.key.ascii = c;
        if (c >= ' ' && c <= '~') {
            ev.key.keycode = (u32)c;
        } else if (code == 0x01) {
            ev.key.keycode = KEY_ESC;
        } else if (code == 0x0E) {
            ev.key.keycode = KEY_BACKSPACE;
            ev.key.ascii = '\b';
        } else if (code == 0x0F) {
            ev.key.keycode = KEY_TAB;
            ev.key.ascii = '\t';
        } else if (code == 0x1C) {
            ev.key.keycode = KEY_ENTER;
            ev.key.ascii = '\n';
        } else {
            ev.key.keycode = (u32)code;
        }

        input_push_event(&ev);
    }
}

static void ps2_process_mouse_byte(u8 data) {
    if (g_mouse_cycle == 0) {
        /* Byte 0 must have bit 3 set to 1 in standard PS/2 packet */
        if ((data & 0x08) == 0) {
            return; /* Out of sync, wait for next byte */
        }
        g_mouse_packet[0] = data;
        g_mouse_cycle = 1;
    } else if (g_mouse_cycle == 1) {
        g_mouse_packet[1] = data;
        g_mouse_cycle = 2;
    } else if (g_mouse_cycle == 2) {
        g_mouse_packet[2] = data;
        g_mouse_cycle = 0;

        u8 flags = g_mouse_packet[0];
        s32 dx = (s32)g_mouse_packet[1];
        s32 dy = (s32)g_mouse_packet[2];

        /* Sign extend 9-bit deltas */
        if (flags & (1 << 4)) dx |= ~0xFF;
        if (flags & (1 << 5)) dy |= ~0xFF;

        /* Invert dy for standard screen coordinates (positive Y is down) */
        dy = -dy;

        /* Extract buttons */
        u8 buttons = 0;
        if (flags & 0x01) buttons |= MOUSE_BTN_LEFT;
        if (flags & 0x02) buttons |= MOUSE_BTN_RIGHT;
        if (flags & 0x04) buttons |= MOUSE_BTN_MIDDLE;

        int mx, my;
        input_get_mouse_state(&mx, &my, NULL);

        mx += dx;
        my += dy;

        /* Clamp to screen boundary (1024x768 default) */
        if (mx < 0) mx = 0;
        if (mx > 1023) mx = 1023;
        if (my < 0) my = 0;
        if (my > 767) my = 767;

        /* Check for button press / release events */
        if ((buttons & MOUSE_BTN_LEFT) && !(g_prev_buttons & MOUSE_BTN_LEFT)) {
            input_event_t ev = {
                .type = INPUT_EVENT_MOUSE_DOWN,
                .mouse = { .x = mx, .y = my, .delta_x = dx, .delta_y = dy, .buttons = buttons }
            };
            input_push_event(&ev);
        } else if (!(buttons & MOUSE_BTN_LEFT) && (g_prev_buttons & MOUSE_BTN_LEFT)) {
            input_event_t ev = {
                .type = INPUT_EVENT_MOUSE_UP,
                .mouse = { .x = mx, .y = my, .delta_x = dx, .delta_y = dy, .buttons = buttons }
            };
            input_push_event(&ev);
        }

        if (dx != 0 || dy != 0) {
            input_event_t ev = {
                .type = INPUT_EVENT_MOUSE_MOVE,
                .mouse = { .x = mx, .y = my, .delta_x = dx, .delta_y = dy, .buttons = buttons }
            };
            input_push_event(&ev);
        }

        g_prev_buttons = buttons;
    }
}

void ps2_poll(void) {
    u8 status = inb(PS2_STATUS_PORT);
    while (status & PS2_STATUS_OUTPUT_FULL) {
        u8 data = inb(PS2_DATA_PORT);
        if (status & PS2_STATUS_MOUSE_DATA) {
            ps2_process_mouse_byte(data);
        } else {
            ps2_process_keyboard_byte(data);
        }
        status = inb(PS2_STATUS_PORT);
    }
}

void ps2_handle_keyboard_irq(void) {
    u8 status = inb(PS2_STATUS_PORT);
    if (status & PS2_STATUS_OUTPUT_FULL) {
        u8 data = inb(PS2_DATA_PORT);
        if (!(status & PS2_STATUS_MOUSE_DATA)) {
            ps2_process_keyboard_byte(data);
        }
    }
}

void ps2_handle_mouse_irq(void) {
    u8 status = inb(PS2_STATUS_PORT);
    if (status & PS2_STATUS_OUTPUT_FULL) {
        u8 data = inb(PS2_DATA_PORT);
        if (status & PS2_STATUS_MOUSE_DATA) {
            ps2_process_mouse_byte(data);
        }
    }
}

void ps2_init(void) {
    kprintf("[PS/2] Initializing PS/2 Keyboard and Mouse controller...\n");

    /* 1. Remap 8259 PIC to vectors 32..47 so IRQs don't conflict with CPU exceptions.
     * Must disable interrupts (cli) while remapping to prevent spurious interrupt delivery
     * at uninitialized PIC vector offsets (e.g. Vector 0 / #DE in VirtualBox).
     */
    u64 flags = read_rflags();
    cli();

    outb(0x20, 0x11); /* ICW1: Init Master PIC */
    outb(0xA0, 0x11); /* ICW1: Init Slave PIC */
    io_wait();
    outb(0x21, 32);   /* ICW2: Master offset = 32 */
    outb(0xA1, 40);   /* ICW2: Slave offset = 40 */
    io_wait();
    outb(0x21, 0x04); /* ICW3: Master has slave at IRQ2 */
    outb(0xA1, 0x02); /* ICW3: Slave cascade identity */
    io_wait();
    outb(0x21, 0x01); /* ICW4: 8086 mode */
    outb(0xA1, 0x01); /* ICW4: 8086 mode */
    io_wait();

    /* Unmask IRQ1 (kbd), IRQ2 (cascade), IRQ12 (mouse); mask all others including legacy PIT IRQ0 */
    outb(0x21, ~(0x02 | 0x04)); /* Unmask IRQ1, IRQ2 on Master */
    outb(0xA1, ~(0x10));        /* Unmask IRQ12 on Slave */
    io_wait();

    if (flags & (1 << 9)) {
        sti();
    }

    /* 2. Configure PS/2 Controller */
    ps2_wait_write();
    outb(PS2_CMD_PORT, 0xAD); /* Disable first PS/2 port */
    ps2_wait_write();
    outb(PS2_CMD_PORT, 0xA7); /* Disable second PS/2 port */

    /* Flush output buffer */
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
        inb(PS2_DATA_PORT);
    }

    /* Read controller configuration */
    ps2_wait_write();
    outb(PS2_CMD_PORT, 0x20);
    u8 config = ps2_read_data();

    /* Enable IRQ1 (bit 0), IRQ12 (bit 1), enable translation (bit 6) */
    config |= (1 << 0) | (1 << 1) | (1 << 6);
    ps2_wait_write();
    outb(PS2_CMD_PORT, 0x60);
    ps2_wait_write();
    outb(PS2_DATA_PORT, config);

    /* Enable PS/2 ports */
    ps2_wait_write();
    outb(PS2_CMD_PORT, 0xAE); /* Enable keyboard */
    ps2_wait_write();
    outb(PS2_CMD_PORT, 0xA8); /* Enable mouse */

    /* Reset Keyboard */
    ps2_write_device(0xFF);
    ps2_wait_read();
    (void)inb(PS2_DATA_PORT); /* Read ACK or BAT */

    /* Initialize Mouse to standard streaming mode */
    ps2_write_mouse(0xF6); /* Set default settings */
    ps2_wait_read();
    (void)inb(PS2_DATA_PORT);

    ps2_write_mouse(0xF4); /* Enable data reporting */
    ps2_wait_read();
    (void)inb(PS2_DATA_PORT);

    kprintf("[PS/2] Keyboard and Mouse enabled (IRQ1=33, IRQ12=44, streaming active).\n");
}
