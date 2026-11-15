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
#define TERM_SCROLLBACK_LINES   256
#define TERM_INPUT_MAX          256
#define TERM_HISTORY_MAX        32

/* Terminal Theme Colors */
#define TERM_COLOR_BG           0xFF10121CU /* Deep terminal black/slate */
#define TERM_COLOR_FG           0xFFE0E4F0U /* Crisp soft white */
#define TERM_COLOR_MUTED        0xFF8088A0U /* Muted gray/indigo */
#define TERM_COLOR_PROMPT       0xFF50FA7BU /* Vibrant green */
#define TERM_COLOR_PATH         0xFF8BE9FDU /* Cyan */
#define TERM_COLOR_SYMBOL       0xFFFF79C6U /* Pink prompt symbol ($) */
#define TERM_COLOR_ERROR        0xFFFF5555U /* Coral Red */
#define TERM_COLOR_SUCCESS      0xFF50FA7BU /* Green */
#define TERM_COLOR_WARN         0xFFF1FA8CU /* Yellow */
#define TERM_COLOR_INFO         0xFF8BE9FDU /* Cyan */
#define TERM_COLOR_CURSOR       0xFFBD93F9U /* Purple/Lilac cursor */

enum {
    TERM_PALETTE_DEFAULT = 0,
    TERM_PALETTE_PROMPT  = 1,
    TERM_PALETTE_PATH    = 2,
    TERM_PALETTE_SYMBOL  = 3,
    TERM_PALETTE_ERROR   = 4,
    TERM_PALETTE_SUCCESS = 5,
    TERM_PALETTE_WARN    = 6,
    TERM_PALETTE_INFO    = 7,
    TERM_PALETTE_MUTED   = 8
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
        default:                   return TERM_COLOR_FG;
    }
}

typedef struct term_line {
    char chars[TERM_DEFAULT_COLS];
    u8   colors[TERM_DEFAULT_COLS];
    u16  len;
} term_line_t;

typedef struct terminal_state {
    wm_window_t *window;
    term_line_t  scrollback[TERM_SCROLLBACK_LINES];
    int          total_lines;     /* Total lines committed */
    int          scroll_offset;   /* Offset from bottom (0 = following tail) */
    
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
    
    /* Shell context pointer */
    void        *shell_ctx;
} terminal_state_t;

void app_terminal_launch(void);
void app_terminal_launch_with_command(const char *cmd);
void terminal_print(terminal_state_t *term, const char *str, u32 color);
void terminal_println(terminal_state_t *term, const char *str, u32 color);
void terminal_clear(terminal_state_t *term);
void terminal_prompt(terminal_state_t *term);
void terminal_get_cursor_coords(const terminal_state_t *term, int screen_h, int *out_x, int *out_y);
bool terminal_self_test(void);

#endif /* ASTRA_KERNEL_TERMINAL_H */
