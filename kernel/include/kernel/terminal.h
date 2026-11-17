#ifndef ASTRA_KERNEL_TERMINAL_H
#define ASTRA_KERNEL_TERMINAL_H

#include <kernel/types.h>
#include <kernel/wm.h>
#include <kernel/gfx.h>
#include <kernel/input.h>

#define TERM_FONT_WIDTH         8
#define TERM_FONT_HEIGHT        16
#define TERM_DEFAULT_COLS       80
#define TERM_DEFAULT_ROWS       24
#define TERM_MAX_COLS           160
#define TERM_SCROLLBACK_DEFAULT 10000
#define TERM_SCROLLBACK_MAX     10000
#define TERM_INPUT_MAX          256
#define TERM_HISTORY_MAX        16

/* Terminal Theme Colors (Cosmic Vista Modern Palette) */
#define TERM_COLOR_BG           0xFF0E111AU /* Deep cosmic slate */
#define TERM_COLOR_FG           0xFFE2E8F0U /* Soft crisp white */
#define TERM_COLOR_MUTED        0xFF64748BU /* Muted slate */
#define TERM_COLOR_PROMPT       0xFF38BDF8U /* Electric cyan */
#define TERM_COLOR_PATH         0xFF818CF8U /* Electric indigo */
#define TERM_COLOR_SYMBOL       0xFFF472B6U /* Vibrant pink ($) */
#define TERM_COLOR_ERROR        0xFFF87171U /* Coral red */
#define TERM_COLOR_SUCCESS      0xFF34D399U /* Emerald green */
#define TERM_COLOR_WARN         0xFFFBBF24U /* Amber yellow */
#define TERM_COLOR_INFO         0xFF38BDF8U /* Cyan blue */
#define TERM_COLOR_CURSOR       0xFFA78BFAU /* Neon purple cursor */
#define TERM_COLOR_SEL_BG       0xFF2563EBU /* Blue selection highlight */
#define TERM_COLOR_SCROLL_TROUGH 0xFF161B26U /* Scrollbar trough */
#define TERM_COLOR_SCROLL_THUMB  0xFF475569U /* Scrollbar thumb */
#define TERM_COLOR_SCROLL_HOVER  0xFF818CF8U /* Scrollbar thumb hovered/dragged */

enum {
    TERM_PALETTE_DEFAULT = 0,
    TERM_PALETTE_PROMPT  = 1,
    TERM_PALETTE_PATH    = 2,
    TERM_PALETTE_SYMBOL  = 3,
    TERM_PALETTE_ERROR   = 4,
    TERM_PALETTE_SUCCESS = 5,
    TERM_PALETTE_WARN    = 6,
    TERM_PALETTE_INFO    = 7,
    TERM_PALETTE_MUTED   = 8,
    TERM_PALETTE_BLACK   = 9,
    TERM_PALETTE_RED     = 10,
    TERM_PALETTE_GREEN   = 11,
    TERM_PALETTE_YELLOW  = 12,
    TERM_PALETTE_BLUE    = 13,
    TERM_PALETTE_MAGENTA = 14,
    TERM_PALETTE_CYAN    = 15,
    TERM_PALETTE_WHITE   = 16,
    TERM_PALETTE_COUNT   = 17
};

static inline u8 term_color_to_idx(u32 color) {
    if (color == TERM_COLOR_PROMPT || color == TERM_COLOR_SUCCESS) return TERM_PALETTE_PROMPT;
    if (color == TERM_COLOR_PATH)   return TERM_PALETTE_PATH;
    if (color == TERM_COLOR_SYMBOL) return TERM_PALETTE_SYMBOL;
    if (color == TERM_COLOR_ERROR)  return TERM_PALETTE_ERROR;
    if (color == TERM_COLOR_WARN)   return TERM_PALETTE_WARN;
    if (color == TERM_COLOR_INFO)   return TERM_PALETTE_INFO;
    if (color == TERM_COLOR_MUTED)  return TERM_PALETTE_MUTED;
    return TERM_PALETTE_DEFAULT;
}

static inline u32 term_idx_to_color(u8 idx) {
    switch (idx) {
        case TERM_PALETTE_PROMPT:  return TERM_COLOR_PROMPT;
        case TERM_PALETTE_PATH:    return TERM_COLOR_PATH;
        case TERM_PALETTE_SYMBOL:  return TERM_COLOR_SYMBOL;
        case TERM_PALETTE_ERROR:   return TERM_COLOR_ERROR;
        case TERM_PALETTE_WARN:    return TERM_COLOR_WARN;
        case TERM_PALETTE_INFO:    return TERM_COLOR_INFO;
        case TERM_PALETTE_MUTED:   return TERM_COLOR_MUTED;
        case TERM_PALETTE_BLACK:   return 0xFF1E293BU;
        case TERM_PALETTE_RED:     return 0xFFEF4444U;
        case TERM_PALETTE_GREEN:   return 0xFF22C55EU;
        case TERM_PALETTE_YELLOW:  return 0xFFEAB308U;
        case TERM_PALETTE_BLUE:    return 0xFF3B82F6U;
        case TERM_PALETTE_MAGENTA: return 0xFFA855F7U;
        case TERM_PALETTE_CYAN:    return 0xFF06B6D4U;
        case TERM_PALETTE_WHITE:   return 0xFFF8FAFCU;
        default:                   return TERM_COLOR_FG;
    }
}

typedef struct term_line {
    char chars[TERM_MAX_COLS];
    u8   colors[TERM_MAX_COLS];
    u16  len;
    bool wrapped;
} term_line_t;

typedef struct terminal_state {
    wm_window_t *window;
    
    /* PMM-backed dynamic scrollback ring buffer */
    term_line_t *scrollback;
    phys_addr_t  scrollback_pa;
    size_t       scrollback_pages;
    int          scrollback_capacity;

    int          total_lines;     /* Monotonic count of lines committed */
    int          scroll_offset;   /* Offset from bottom (0 = following tail/prompt) */
    
    /* Interactive Line Editor */
    char         input_buf[TERM_INPUT_MAX];
    int          input_len;
    int          cursor_pos;
    
    /* Command History */
    char         history[TERM_HISTORY_MAX][TERM_INPUT_MAX];
    int          history_count;
    int          history_idx;
    char         history_temp[TERM_INPUT_MAX];
    
    /* Cursor state */
    bool         active;
    bool         cursor_visible;
    u64          last_blink_tick;
    
    /* Scrollbar state */
    bool         scrollbar_hover;
    bool         scrollbar_dragging;
    int          drag_start_y;
    int          drag_orig_offset;

    /* Text Selection state */
    bool         selecting;
    bool         has_selection;
    int          sel_start_line;
    int          sel_start_col;
    int          sel_end_line;
    int          sel_end_col;

    /* ANSI Parser state */
    bool         in_escape;
    char         esc_buf[32];
    int          esc_len;
    u8           current_attr_color;

    /* Shell context pointer */
    void        *shell_ctx;
} terminal_state_t;

void app_terminal_launch(void);
void app_terminal_launch_with_command(const char *cmd);
void terminal_print(terminal_state_t *term, const char *str, u32 color);
void terminal_println(terminal_state_t *term, const char *str, u32 color);
void terminal_prompt(terminal_state_t *term);
void terminal_clear(terminal_state_t *term);
void terminal_get_cursor_coords(const terminal_state_t *term, int screen_h, int *out_x, int *out_y);
bool terminal_self_test(void);

#endif /* ASTRA_KERNEL_TERMINAL_H */
