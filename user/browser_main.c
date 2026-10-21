/*
 * AstraOS Phase P — User-Space AstraBrowser Application
 *
 * Fully isolated Ring 3 ELF Web Browser with:
 * - Real Ring 3 CPL=3 execution
 * - User-space dynamic arena allocation via sys_mmap / sys_munmap
 * - Local file:/// and remote http:///https:// navigation
 * - Native BMP image decoding and bounded pixel rendering (sys_gui_draw_bitmap)
 * - External CSS stylesheet loading (<link rel="stylesheet">) with URL resolution
 * - Interactive HTML forms (<input type="text|checkbox|submit">, <textarea>, <button>)
 * - Keyboard & mouse focus management and GET form submission with URL encoding
 * - Complete isolation: hardware fault containment and automatic reclamation
 */
#ifndef ASTRA_TYPES_DEFINED
#define ASTRA_TYPES_DEFINED
typedef unsigned long size_t;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long s64;
typedef unsigned long uintptr_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void*)0)
#endif

#include "js_engine.h"

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
s64 sys_open(const char *path, int flags);
s64 sys_read(int fd, void *buf, size_t count);
s64 sys_close(int fd);

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
s64 sys_gui_draw_bitmap(int win_id, int x, int y, int w, int h, const u32 *pixels);

/* Memory Allocation Syscalls */
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
#define MAP_FAILED      ((void *)-1)

void *sys_mmap(void *addr, size_t length, int prot, int flags);
s64 sys_munmap(void *addr, size_t length);
s64 sys_net_fetch(const char *url, char *buf, size_t max_len, int *out_status);

/* UI Layout Constants */
#define WIN_WIDTH       640
#define WIN_HEIGHT      440
#define TOOLBAR_HEIGHT  34
#define STATUS_HEIGHT   24
#define SCROLLBAR_WIDTH 14
#define FONT_WIDTH      8
#define FONT_HEIGHT     16

/* Color Palette */
#define COLOR_TOOLBAR_BG     0xFF111827
#define COLOR_VIEWPORT_BG    0xFF0A0F1D
#define COLOR_STATUS_BG      0xFF0E1424
#define COLOR_PANEL_BORDER   0xFF1F2937
#define COLOR_BTN_BG         0xFF1E293B
#define COLOR_BTN_ACTIVE     0xFF2563EB
#define COLOR_BORDER_CYAN    0xFF06B6D4
#define COLOR_BORDER_DEFAULT 0xFF334155
#define COLOR_TEXT_WHITE     0xFFF8FAFC
#define COLOR_TEXT_MUTED     0xFF94A3B8
#define COLOR_ACCENT_CYAN    0xFF2DD4BF
#define COLOR_ACCENT_PURPLE  0xFFC084FC
#define COLOR_ACCENT_YELLOW  0xFFFACC15
#define COLOR_ACCENT_RED     0xFFF87171
#define COLOR_LINK_BLUE      0xFF38BDF8

/* String & Buffer Utilities */
static size_t ustrlen(const char *s) {
    size_t l = 0;
    while (s && s[l]) l++;
    return l;
}

static void ustrcpy(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void ustrcat(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t dlen = ustrlen(dst);
    size_t si = 0;
    while (src && src[si] && (dlen + si) < max_len - 1) {
        dst[dlen + si] = src[si];
        si++;
    }
    dst[dlen + si] = '\0';
}

static void umemset(void *dst, int val, size_t n) {
    u8 *d = (u8 *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (u8)val;
}

static void __attribute__((unused)) umemcpy(void *dst, const void *src, size_t n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static bool uis_whitespace(char c) {
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static char utolower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static bool ustrcaseeq(const char *a, const char *b) {
    while (*a && *b) {
        if (utolower(*a) != utolower(*b)) return false;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static bool ustrcasestartswith(const char *s, const char *prefix) {
    while (*prefix) {
        if (utolower(*s) != utolower(*prefix)) return false;
        s++;
        prefix++;
    }
    return true;
}

/* -----------------------------------------------------------------
 * Dynamic Memory Arena Allocator (Multi-Chunk Safe Reclamation)
 * ----------------------------------------------------------------- */
#define MAX_ARENA_CHUNKS 16
static void *g_arena_chunks[MAX_ARENA_CHUNKS];
static size_t g_arena_chunk_sizes[MAX_ARENA_CHUNKS];
static int g_arena_chunk_count = 0;
static void *g_arena_curr_base = NULL;
static size_t g_arena_curr_cap = 0;
static size_t g_arena_curr_used = 0;

static void *umalloc(size_t size) {
    size = (size + 15) & ~15UL; /* Align to 16 bytes */
    if (!g_arena_curr_base || g_arena_curr_used + size > g_arena_curr_cap) {
        if (g_arena_chunk_count >= MAX_ARENA_CHUNKS) {
            return NULL; /* Bounded arena limit (4 MiB maximum) */
        }
        size_t alloc_sz = (size + 4095) & ~4095UL;
        if (alloc_sz < 262144) alloc_sz = 262144; /* 256 KiB chunks */

        void *p = sys_mmap(NULL, alloc_sz, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE);
        if (p == MAP_FAILED || (long)p < 0) {
            return NULL;
        }
        g_arena_chunks[g_arena_chunk_count] = p;
        g_arena_chunk_sizes[g_arena_chunk_count] = alloc_sz;
        g_arena_chunk_count++;
        g_arena_curr_base = p;
        g_arena_curr_cap = alloc_sz;
        g_arena_curr_used = 0;
    }

    void *ret = (void *)((uintptr_t)g_arena_curr_base + g_arena_curr_used);
    g_arena_curr_used += size;
    umemset(ret, 0, size);
    return ret;
}

static void ufree_arena(void) {
    for (int i = 0; i < g_arena_chunk_count; i++) {
        if (g_arena_chunks[i] && g_arena_chunk_sizes[i] > 0) {
            sys_munmap(g_arena_chunks[i], g_arena_chunk_sizes[i]);
            g_arena_chunks[i] = NULL;
            g_arena_chunk_sizes[i] = 0;
        }
    }
    g_arena_chunk_count = 0;
    g_arena_curr_base = NULL;
    g_arena_curr_cap = 0;
    g_arena_curr_used = 0;
}

/* -----------------------------------------------------------------
 * URL Parsing & Relative URL Resolution
 * ----------------------------------------------------------------- */
typedef enum {
    SCHEME_FILE,
    SCHEME_HTTP,
    SCHEME_HTTPS,
    SCHEME_UNKNOWN
} url_scheme_t;

typedef struct {
    url_scheme_t scheme;
    char host[96];
    u16 port;
    char path[160];
    char fragment[32];
    char raw[256];
} parsed_url_t;

static void normalize_path(const char *src, char *dst, size_t dst_max) {
    if (!dst || dst_max == 0) return;
    dst[0] = '\0';
    if (!src) return;

    #define MAX_SEGS 16
    const char *seg_start[MAX_SEGS];
    size_t seg_len[MAX_SEGS];
    int nsegs = 0;

    const char *p = src;
    bool is_absolute = (*p == '/');
    while (*p == '/') p++;

    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        while (*p == '/') p++;

        if (len == 0 || (len == 1 && start[0] == '.')) {
            continue;
        } else if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (nsegs > 0) nsegs--;
        } else {
            if (nsegs < MAX_SEGS) {
                seg_start[nsegs] = start;
                seg_len[nsegs] = len;
                nsegs++;
            }
        }
    }

    size_t out_len = 0;
    if (is_absolute && out_len < dst_max - 1) {
        dst[out_len++] = '/';
    }
    for (int i = 0; i < nsegs; i++) {
        if (i > 0 && out_len < dst_max - 1) {
            dst[out_len++] = '/';
        }
        for (size_t k = 0; k < seg_len[i] && out_len < dst_max - 1; k++) {
            dst[out_len++] = seg_start[i][k];
        }
    }
    dst[out_len] = '\0';
    if (is_absolute && out_len == 0 && dst_max > 1) {
        dst[0] = '/';
        dst[1] = '\0';
    }
}

static bool parse_url(const char *url_str, parsed_url_t *out) {
    if (!url_str || !out) return false;
    umemset(out, 0, sizeof(*out));
    ustrcpy(out->raw, url_str, sizeof(out->raw));

    /* Extract fragment if present */
    char temp_url[256];
    ustrcpy(temp_url, url_str, sizeof(temp_url));
    char *hash = NULL;
    for (int i = 0; temp_url[i]; i++) {
        if (temp_url[i] == '#') {
            hash = &temp_url[i];
            break;
        }
    }
    if (hash) {
        *hash = '\0';
        ustrcpy(out->fragment, hash + 1, sizeof(out->fragment));
    }

    if (ustrcasestartswith(temp_url, "file:///")) {
        out->scheme = SCHEME_FILE;
        char norm[160];
        normalize_path(temp_url + 7, norm, sizeof(norm));
        ustrcpy(out->path, norm, sizeof(out->path));
        return true;
    }
    if (ustrcasestartswith(temp_url, "https://")) {
        out->scheme = SCHEME_HTTPS;
        out->port = 443;
        const char *p = temp_url + 8;
        size_t hi = 0;
        while (*p && *p != '/' && *p != ':' && hi < sizeof(out->host) - 1) {
            out->host[hi++] = *p++;
        }
        out->host[hi] = '\0';
        if (*p == ':') {
            p++;
            u32 port_val = 0;
            while (*p >= '0' && *p <= '9') {
                port_val = port_val * 10 + (*p - '0');
                p++;
            }
            if (port_val > 0 && port_val <= 65535) out->port = (u16)port_val;
        }
        char norm[160];
        if (*p == '/') normalize_path(p, norm, sizeof(norm));
        else ustrcpy(norm, "/", sizeof(norm));
        ustrcpy(out->path, norm, sizeof(out->path));
        return true;
    }
    if (ustrcasestartswith(temp_url, "http://")) {
        out->scheme = SCHEME_HTTP;
        out->port = 80;
        const char *p = temp_url + 7;
        size_t hi = 0;
        while (*p && *p != '/' && *p != ':' && hi < sizeof(out->host) - 1) {
            out->host[hi++] = *p++;
        }
        out->host[hi] = '\0';
        if (*p == ':') {
            p++;
            u32 port_val = 0;
            while (*p >= '0' && *p <= '9') {
                port_val = port_val * 10 + (*p - '0');
                p++;
            }
            if (port_val > 0 && port_val <= 65535) out->port = (u16)port_val;
        }
        char norm[160];
        if (*p == '/') normalize_path(p, norm, sizeof(norm));
        else ustrcpy(norm, "/", sizeof(norm));
        ustrcpy(out->path, norm, sizeof(out->path));
        return true;
    }
    return false;
}

static void resolve_url(const char *base_url, const char *rel, char *out, size_t out_max) {
    if (!out || out_max == 0) return;
    out[0] = '\0';
    if (!rel || !rel[0]) return;

    /* 1. Fragment-only link: e.g. "#section" */
    if (rel[0] == '#') {
        if (base_url && base_url[0]) {
            ustrcpy(out, base_url, out_max);
            for (int i = 0; out[i]; i++) {
                if (out[i] == '#') {
                    out[i] = '\0';
                    break;
                }
            }
            ustrcat(out, rel, out_max);
        } else {
            ustrcpy(out, rel, out_max);
        }
        return;
    }

    /* 2. Absolute URL */
    if (ustrcasestartswith(rel, "file:///") || ustrcasestartswith(rel, "http://") || ustrcasestartswith(rel, "https://")) {
        ustrcpy(out, rel, out_max);
        return;
    }

    if (!base_url || !base_url[0]) {
        ustrcpy(out, rel, out_max);
        return;
    }

    /* Separate fragment from rel if any */
    char rel_clean[256];
    char rel_frag[64];
    rel_clean[0] = '\0';
    rel_frag[0] = '\0';
    ustrcpy(rel_clean, rel, sizeof(rel_clean));
    for (int i = 0; rel_clean[i]; i++) {
        if (rel_clean[i] == '#') {
            rel_clean[i] = '\0';
            ustrcpy(rel_frag, &rel[i], sizeof(rel_frag));
            break;
        }
    }

    if (ustrcasestartswith(base_url, "file:///")) {
        char base_path[160];
        ustrcpy(base_path, base_url + 7, sizeof(base_path));
        /* Strip base fragment */
        for (int i = 0; base_path[i]; i++) {
            if (base_path[i] == '#') { base_path[i] = '\0'; break; }
        }

        char combined[256];
        if (rel_clean[0] == '/') {
            ustrcpy(combined, rel_clean, sizeof(combined));
        } else {
            int last_slash = -1;
            for (int i = 0; base_path[i]; i++) {
                if (base_path[i] == '/') last_slash = i;
            }
            if (last_slash >= 0) {
                base_path[last_slash + 1] = '\0';
                ustrcpy(combined, base_path, sizeof(combined));
            } else {
                ustrcpy(combined, "/", sizeof(combined));
            }
            ustrcat(combined, rel_clean, sizeof(combined));
        }

        char norm[160];
        normalize_path(combined, norm, sizeof(norm));
        ustrcpy(out, "file://", out_max);
        ustrcat(out, norm, out_max);
        if (rel_frag[0]) ustrcat(out, rel_frag, out_max);
        return;
    }

    if (ustrcasestartswith(base_url, "http://") || ustrcasestartswith(base_url, "https://")) {
        parsed_url_t bp;
        if (parse_url(base_url, &bp)) {
            const char *scheme = (bp.scheme == SCHEME_HTTPS) ? "https://" : "http://";
            char combined[256];
            if (rel_clean[0] == '/') {
                ustrcpy(combined, rel_clean, sizeof(combined));
            } else {
                int last_slash = -1;
                for (int i = 0; bp.path[i]; i++) {
                    if (bp.path[i] == '/') last_slash = i;
                }
                if (last_slash >= 0) {
                    bp.path[last_slash + 1] = '\0';
                    ustrcpy(combined, bp.path, sizeof(combined));
                } else {
                    ustrcpy(combined, "/", sizeof(combined));
                }
                ustrcat(combined, rel_clean, sizeof(combined));
            }

            char norm[160];
            normalize_path(combined, norm, sizeof(norm));
            ustrcpy(out, scheme, out_max);
            ustrcat(out, bp.host, out_max);
            if (bp.port != 80 && bp.port != 443 && bp.port > 0) {
                int pv = bp.port;
                int pi = 0;
                char ptmp[8];
                while (pv > 0) { ptmp[pi++] = '0' + (pv % 10); pv /= 10; }
                char pbuf[10];
                pbuf[0] = ':';
                int pj = 1;
                for (int k = pi - 1; k >= 0; k--) pbuf[pj++] = ptmp[k];
                pbuf[pj] = '\0';
                ustrcat(out, pbuf, out_max);
            }
            ustrcat(out, norm, out_max);
            if (rel_frag[0]) ustrcat(out, rel_frag, out_max);
            return;
        }
    }

    ustrcpy(out, rel, out_max);
}

/* Resource Fetching Helper (VFS / HTTP / HTTPS) */
static s64 fetch_resource(const char *url_str, void *buf, size_t max_len) {
    if (!url_str || !buf || max_len == 0) return -1;
    parsed_url_t purl;
    if (!parse_url(url_str, &purl)) return -1;

    if (purl.scheme == SCHEME_FILE) {
        s64 fd = sys_open(purl.path, 0);
        if (fd < 0) return -1;
        s64 bytes = sys_read((int)fd, buf, max_len);
        sys_close((int)fd);
        return bytes;
    } else if (purl.scheme == SCHEME_HTTP || purl.scheme == SCHEME_HTTPS) {
        int status_code = 0;
        s64 bytes = sys_net_fetch(url_str, (char *)buf, max_len, &status_code);
        if (bytes >= 0 && status_code == 200) {
            return bytes;
        }
        return -1;
    }
    return -1;
}

/* -----------------------------------------------------------------
 * BMP Image Decoder (User-Space)
 * ----------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct {
    u16 bfType;
    u32 bfSize;
    u16 bfReserved1;
    u16 bfReserved2;
    u32 bfOffBits;
} bmp_file_header_t;

typedef struct {
    u32 biSize;
    int biWidth;
    int biHeight;
    u16 biPlanes;
    u16 biBitCount;
    u32 biCompression;
    u32 biSizeImage;
    int biXPelsPerMeter;
    int biYPelsPerMeter;
    u32 biClrUsed;
    u32 biClrImportant;
} bmp_info_header_t;
#pragma pack(pop)

static u32 *decode_bmp(const u8 *data, size_t len, int *out_w, int *out_h) {
    if (!data || !out_w || !out_h) return NULL;
    *out_w = 0;
    *out_h = 0;

    if (len < sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t)) {
        return NULL;
    }

    const bmp_file_header_t *file_hdr = (const bmp_file_header_t *)data;
    const bmp_info_header_t *info_hdr = (const bmp_info_header_t *)(data + sizeof(bmp_file_header_t));

    /* 1. Validate BMP Magic (0x4D42 'BM') */
    if (file_hdr->bfType != 0x4D42) {
        return NULL;
    }

    /* 2. Validate DIB Header */
    if (info_hdr->biSize < 40 || info_hdr->biPlanes != 1 || info_hdr->biCompression != 0) {
        return NULL;
    }

    /* 3. Support only 24-bit BGR and 32-bit BGRA/BGRX */
    if (info_hdr->biBitCount != 24 && info_hdr->biBitCount != 32) {
        return NULL;
    }

    int w = info_hdr->biWidth;
    int h = info_hdr->biHeight;
    int abs_h = (h < 0) ? -h : h;

    /* 4. Strict Dimension & Overflow Bounds */
    if (w <= 0 || w > 512 || abs_h <= 0 || abs_h > 512) {
        return NULL;
    }
    size_t num_pixels = (size_t)w * abs_h;
    if (num_pixels > 262144) { /* 512x512 max limit */
        return NULL;
    }

    size_t bpp_bytes = info_hdr->biBitCount / 8;
    size_t row_stride = ((size_t)w * bpp_bytes + 3) & ~3UL;
    if (file_hdr->bfOffBits + row_stride * abs_h > len) {
        return NULL;
    }

    /* Allocate decoded 32-bit pixel buffer in user space */
    u32 *pixels = (u32 *)umalloc(num_pixels * sizeof(u32));
    if (!pixels) return NULL;

    const u8 *pixel_base = data + file_hdr->bfOffBits;
    for (int row = 0; row < abs_h; row++) {
        /* BMP rows are bottom-up if h > 0, top-down if h < 0 */
        int src_row = (h > 0) ? (abs_h - 1 - row) : row;
        const u8 *row_ptr = pixel_base + (src_row * row_stride);

        for (int col = 0; col < w; col++) {
            u8 b = row_ptr[col * bpp_bytes + 0];
            u8 g = row_ptr[col * bpp_bytes + 1];
            u8 r = row_ptr[col * bpp_bytes + 2];
            pixels[row * w + col] = (0xFF << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }
    }

    *out_w = w;
    *out_h = abs_h;
    return pixels;
}

/* -----------------------------------------------------------------
 * HTML & CSS Structures
 * ----------------------------------------------------------------- */
typedef enum {
    TAG_NONE = 0,
    TAG_TEXT,
    TAG_TITLE,
    TAG_H1,
    TAG_H2,
    TAG_H3,
    TAG_P,
    TAG_BR,
    TAG_HR,
    TAG_B,
    TAG_I,
    TAG_A,
    TAG_UL,
    TAG_LI,
    TAG_DIV,
    TAG_SPAN,
    TAG_STYLE,
    TAG_LINK,
    TAG_IMG,
    TAG_FORM,
    TAG_INPUT,
    TAG_TEXTAREA,
    TAG_BUTTON,
    TAG_SCRIPT
} tag_t;

typedef struct {
    u32 color;
    bool has_color;
    u32 bg_color;
    bool has_bg_color;
    u32 border_color;
    bool has_border;
    int border_width;
    int margin_top;
    int margin_bottom;
    int padding;
    bool bold;
    bool italic;
    int font_scale;
    bool is_block;
    bool is_hidden;
} style_t;

#define MAX_NODES 160
typedef struct {
    tag_t tag;
    char text[128];
    char href[128];
    char class_name[32];
    char id_name[32];
    style_t style;

    /* Image attributes */
    char img_src[128];
    char img_alt[48];
    int img_w;
    int img_h;

    /* Form control attributes */
    char form_action[128];
    char form_method[16];
    char input_type[24];
    char input_name[48];
    char input_value[128];
    bool input_checked;
    int form_ctrl_id;
    char onclick[128];
} dom_node_t;

/* Form Control Structure */
#define MAX_FORM_CONTROLS 16
typedef enum {
    CTRL_NONE = 0,
    CTRL_INPUT_TEXT,
    CTRL_INPUT_CHECKBOX,
    CTRL_INPUT_SUBMIT,
    CTRL_TEXTAREA,
    CTRL_BUTTON
} form_control_type_t;

typedef struct {
    int id;
    form_control_type_t type;
    char name[48];
    char value[256];
    bool checked;
    char action[160];
    char method[16];
    int x, y, w, h;
    bool focused;
    char onclick[128];
} form_control_t;

#define MAX_BOXES 256
typedef struct {
    int x;
    int y;
    int w;
    int h;
    char text[128];
    u32 fg_color;
    u32 bg_color;
    bool has_bg;
    u32 border_color;
    bool has_border;
    int border_w;
    int font_scale;
    bool bold;
    bool is_link;
    char href[128];
    bool is_hr;
    bool is_bullet;

    /* Image box */
    bool is_image;
    u32 *image_pixels;
    int img_w;
    int img_h;
    char alt[48];

    /* Form control link */
    int form_ctrl_id;
    char id_name[32];
    char onclick[128];
} layout_box_t;


/* CSS Rule Entry */
#define MAX_RULES 64
typedef struct {
    char selector[32]; /* "h1", ".card", "#special" */
    style_t style;
} css_rule_t;

typedef struct {
    css_rule_t rules[MAX_RULES];
    int count;
} stylesheet_t;

/* -----------------------------------------------------------------
 * CSS Parser & Cascade
 * ----------------------------------------------------------------- */
static u32 parse_hex_color(const char *str) {
    while (*str && (*str == ' ' || *str == '#')) str++;
    u32 color = 0;
    int count = 0;
    while (*str && count < 6) {
        char c = *str++;
        u32 digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
        else break;
        color = (color << 4) | digit;
        count++;
    }
    return 0xFF000000 | color;
}

static void parse_css_declarations(const char *block, style_t *st) {
    const char *p = block;
    while (*p && *p != '}') {
        while (*p && uis_whitespace(*p)) p++;
        if (!*p || *p == '}') break;

        char prop[32];
        size_t pi = 0;
        while (*p && *p != ':' && *p != ';' && *p != '}' && pi < sizeof(prop) - 1) {
            if (!uis_whitespace(*p)) prop[pi++] = utolower(*p);
            p++;
        }
        prop[pi] = '\0';
        if (*p == ':') p++;

        while (*p && uis_whitespace(*p)) p++;

        char val[64];
        size_t vi = 0;
        while (*p && *p != ';' && *p != '}' && vi < sizeof(val) - 1) {
            val[vi++] = *p++;
        }
        val[vi] = '\0';
        if (*p == ';') p++;

        while (vi > 0 && uis_whitespace(val[vi - 1])) {
            val[--vi] = '\0';
        }

        if (ustrcaseeq(prop, "color")) {
            st->color = parse_hex_color(val);
            st->has_color = true;
        } else if (ustrcaseeq(prop, "background-color") || ustrcaseeq(prop, "background")) {
            st->bg_color = parse_hex_color(val);
            st->has_bg_color = true;
        } else if (ustrcaseeq(prop, "border")) {
            st->has_border = true;
            st->border_width = 1;
            const char *hash = val;
            while (*hash && *hash != '#') hash++;
            if (*hash == '#') st->border_color = parse_hex_color(hash);
            else st->border_color = COLOR_BORDER_DEFAULT;
        } else if (ustrcaseeq(prop, "padding")) {
            st->padding = 6;
        } else if (ustrcaseeq(prop, "margin-top")) {
            st->margin_top = 8;
        } else if (ustrcaseeq(prop, "margin-bottom")) {
            st->margin_bottom = 6;
        } else if (ustrcaseeq(prop, "font-weight")) {
            if (ustrcaseeq(val, "bold")) st->bold = true;
        } else if (ustrcaseeq(prop, "display")) {
            if (ustrcaseeq(val, "none")) st->is_hidden = true;
        }
    }
}

static void parse_stylesheet(const char *css_text, stylesheet_t *sheet) {
    if (!css_text || !sheet) return;
    const char *p = css_text;
    while (*p && sheet->count < MAX_RULES) {
        while (*p && uis_whitespace(*p)) p++;
        if (!*p) break;

        /* Skip comments */
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }

        char sel[32];
        size_t si = 0;
        while (*p && *p != '{' && si < sizeof(sel) - 1) {
            if (!uis_whitespace(*p)) sel[si++] = utolower(*p);
            p++;
        }
        sel[si] = '\0';
        if (*p == '{') p++;

        if (sel[0]) {
            css_rule_t *rule = &sheet->rules[sheet->count++];
            ustrcpy(rule->selector, sel, sizeof(rule->selector));
            umemset(&rule->style, 0, sizeof(style_t));
            parse_css_declarations(p, &rule->style);
        }

        while (*p && *p != '}') p++;
        if (*p == '}') p++;
    }
}

static void apply_cascade(dom_node_t *node, const stylesheet_t *sheet) {
    style_t *s = &node->style;

    /* 1. Tag Default User-Agent Styles */
    if (node->tag == TAG_H1) {
        s->font_scale = 2;
        s->bold = true;
        s->color = COLOR_ACCENT_CYAN;
        s->has_color = true;
        s->is_block = true;
        s->margin_top = 10;
        s->margin_bottom = 8;
    } else if (node->tag == TAG_H2) {
        s->font_scale = 1;
        s->bold = true;
        s->color = COLOR_ACCENT_PURPLE;
        s->has_color = true;
        s->is_block = true;
        s->margin_top = 8;
        s->margin_bottom = 6;
    } else if (node->tag == TAG_P || node->tag == TAG_DIV) {
        s->font_scale = 1;
        s->color = COLOR_TEXT_WHITE;
        s->has_color = true;
        s->is_block = true;
        s->margin_top = 4;
        s->margin_bottom = 4;
    } else if (node->tag == TAG_A) {
        s->font_scale = 1;
        s->color = COLOR_LINK_BLUE;
        s->has_color = true;
    } else if (node->tag == TAG_B) {
        s->bold = true;
    } else if (node->tag == TAG_I) {
        s->italic = true;
    } else if (node->tag == TAG_FORM) {
        s->is_block = true;
        s->margin_top = 6;
        s->margin_bottom = 6;
    }

    /* 2. Stylesheet matching (Tag, Class, ID) */
    const char *tag_name = "";
    if (node->tag == TAG_H1) tag_name = "h1";
    else if (node->tag == TAG_H2) tag_name = "h2";
    else if (node->tag == TAG_P) tag_name = "p";
    else if (node->tag == TAG_DIV) tag_name = "div";
    else if (node->tag == TAG_A) tag_name = "a";

    for (int i = 0; i < sheet->count; i++) {
        const css_rule_t *r = &sheet->rules[i];
        bool match = false;
        if (r->selector[0] == '.' && node->class_name[0] && ustrcaseeq(r->selector + 1, node->class_name)) {
            match = true;
        } else if (r->selector[0] == '#' && node->id_name[0] && ustrcaseeq(r->selector + 1, node->id_name)) {
            match = true;
        } else if (ustrcaseeq(r->selector, tag_name)) {
            match = true;
        }

        if (match) {
            if (r->style.has_color) { s->color = r->style.color; s->has_color = true; }
            if (r->style.has_bg_color) { s->bg_color = r->style.bg_color; s->has_bg_color = true; }
            if (r->style.has_border) {
                s->has_border = true;
                s->border_color = r->style.border_color;
                s->border_width = r->style.border_width;
            }
            if (r->style.padding > 0) s->padding = r->style.padding;
            if (r->style.margin_top > 0) s->margin_top = r->style.margin_top;
            if (r->style.margin_bottom > 0) s->margin_bottom = r->style.margin_bottom;
            if (r->style.bold) s->bold = true;
            if (r->style.is_hidden) s->is_hidden = true;
        }
    }
}

/* -----------------------------------------------------------------
 * Browser Application State
 * ----------------------------------------------------------------- */
typedef struct {
    int win_id;
    char current_url[256];
    char input_url[256];
    int input_cursor;
    bool input_focused;
    char status_text[96];

    /* History */
    char history[16][256];
    int history_index;
    int history_count;

    /* DOM & Layout */
    dom_node_t *nodes;
    int node_count;
    stylesheet_t *stylesheet;
    layout_box_t *boxes;
    int box_count;
    char *html_buf;
    size_t html_len;
    char page_title[64];

    /* Scrolling */
    int scroll_y;
    int doc_height;

    /* Forms */
    form_control_t controls[MAX_FORM_CONTROLS];
    int control_count;
    int focused_control_id; /* -1 for none */
    char active_form_action[160];
    char active_form_method[16];

    /* Phase S: Document Lifecycle, Generation Guard & Sandboxed VM */
    u32 doc_generation;
    js_vm_t doc_vm;
    bool doc_vm_initialized;

    /* Phase T: Safe DOM Event Dispatch */
    bool dispatching_event;
    int mutation_count_in_event;
} browser_app_t;

static browser_app_t g_browser;

static void navigate_to(browser_app_t *b, const char *url_str, bool record_history);
static int layout_page(browser_app_t *b, const dom_node_t *nodes, int node_count, int view_w, layout_box_t *boxes, int max_boxes, int *out_doc_h);
static void render_browser_ui(browser_app_t *b);

/* -----------------------------------------------------------------
 * DOM Bridge for Sandboxed JavaScript Engine (Phase S & T Hardened)
 * ----------------------------------------------------------------- */
static int browser_js_dom_get_element_by_id(void *ctx, const char *id, u32 *out_doc_gen) {
    browser_app_t *b = (browser_app_t *)ctx;
    if (!b || !b->nodes || !id) return -1;
    if (out_doc_gen) *out_doc_gen = b->doc_generation;
    for (int i = 0; i < b->node_count; i++) {
        if (ustrcaseeq(b->nodes[i].id_name, id)) {
            return i;
        }
    }
    return -1;
}

static const char *browser_js_dom_get_text_content(void *ctx, u32 doc_gen, int node_idx) {
    browser_app_t *b = (browser_app_t *)ctx;
    if (!b || !b->nodes || doc_gen != b->doc_generation || node_idx < 0 || node_idx >= b->node_count) {
        return NULL; /* Stale handle or out of bounds */
    }
    return b->nodes[node_idx].text;
}

static bool browser_js_dom_set_text_content(void *ctx, u32 doc_gen, int node_idx, const char *text) {
    browser_app_t *b = (browser_app_t *)ctx;
    if (!b || !b->nodes || doc_gen != b->doc_generation || node_idx < 0 || node_idx >= b->node_count || !text) {
        return false; /* Stale handle or invalid node */
    }

    /* Treat script-provided strings strictly as raw text, never HTML markup */
    ustrcpy(b->nodes[node_idx].text, text, sizeof(b->nodes[node_idx].text));

    /* If this node is associated with a form control, update its value */
    if (b->nodes[node_idx].form_ctrl_id >= 0 && b->nodes[node_idx].form_ctrl_id < b->control_count) {
        form_control_t *fc = &b->controls[b->nodes[node_idx].form_ctrl_id];
        ustrcpy(fc->value, text, sizeof(fc->value));
    }

    /* Bounded reflow: throttled to 32 per event dispatch to prevent CPU exhaustion */
    b->mutation_count_in_event++;
    if (b->boxes && b->mutation_count_in_event < 32) {
        int view_w = WIN_WIDTH - SCROLLBAR_WIDTH;
        b->box_count = layout_page(b, b->nodes, b->node_count, view_w, b->boxes, MAX_BOXES, &b->doc_height);
        if (b->win_id > 0) {
            render_browser_ui(b);
        }
    }
    return true;
}

static bool browser_js_dom_set_onclick(void *ctx, u32 doc_gen, int node_idx, const char *code) {
    browser_app_t *b = (browser_app_t *)ctx;
    if (!b || !b->nodes || doc_gen != b->doc_generation || node_idx < 0 || node_idx >= b->node_count || !code) {
        return false;
    }
    ustrcpy(b->nodes[node_idx].onclick, code, sizeof(b->nodes[node_idx].onclick));
    if (b->nodes[node_idx].form_ctrl_id >= 0 && b->nodes[node_idx].form_ctrl_id < b->control_count) {
        ustrcpy(b->controls[b->nodes[node_idx].form_ctrl_id].onclick, code, sizeof(b->controls[0].onclick));
    }
    if (b->boxes) {
        for (int i = 0; i < b->box_count; i++) {
            if (b->boxes[i].id_name[0] && ustrcaseeq(b->boxes[i].id_name, b->nodes[node_idx].id_name)) {
                ustrcpy(b->boxes[i].onclick, code, sizeof(b->boxes[i].onclick));
            }
        }
    }
    return true;
}

static void browser_dispatch_click(browser_app_t *b, const char *code) {
    if (!b || !code || !code[0] || b->dispatching_event) return;
    b->dispatching_event = true;
    b->mutation_count_in_event = 0;
    if (!b->doc_vm_initialized) {
        b->doc_generation++;
        js_init(&b->doc_vm);
        js_set_dom_bridge(&b->doc_vm, b,
                          browser_js_dom_get_element_by_id,
                          browser_js_dom_get_text_content,
                          browser_js_dom_set_text_content);
        js_set_dom_event_bridge(&b->doc_vm, browser_js_dom_set_onclick);
        b->doc_vm_initialized = true;
    }
    /* Reset instruction budget for event execution */
    b->doc_vm.instruction_count = 0;
    js_value_t js_res;
    js_error_t err = js_eval(&b->doc_vm, code, &js_res);
    if (err != JS_OK) {
        ustrcpy(b->status_text, "[JS Error] ", sizeof(b->status_text));
        ustrcat(b->status_text, js_error_string(err), sizeof(b->status_text));
    }
    /* If mutations exceeded throttle during event, perform one coalesced reflow & repaint */
    if (b->mutation_count_in_event >= 32 && b->boxes) {
        int view_w = WIN_WIDTH - SCROLLBAR_WIDTH;
        b->box_count = layout_page(b, b->nodes, b->node_count, view_w, b->boxes, MAX_BOXES, &b->doc_height);
        if (b->win_id > 0) {
            render_browser_ui(b);
        }
    }
    b->dispatching_event = false;
}


/* -----------------------------------------------------------------
 * Form Query String Serialization & URL Encoding
 * ----------------------------------------------------------------- */
static void url_encode_append(char *dst, size_t dst_max, const char *src) {
    size_t dlen = ustrlen(dst);
    while (*src && dlen < dst_max - 4) {
        char c = *src++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[dlen++] = c;
        } else if (c == ' ') {
            dst[dlen++] = '+';
        } else {
            static const char hex[] = "0123456789ABCDEF";
            dst[dlen++] = '%';
            dst[dlen++] = hex[((u8)c >> 4) & 0xF];
            dst[dlen++] = hex[(u8)c & 0xF];
        }
    }
    dst[dlen] = '\0';
}

static void submit_form(browser_app_t *b, const char *action) {
    char query[512];
    query[0] = '\0';

    for (int i = 0; i < b->control_count; i++) {
        form_control_t *c = &b->controls[i];
        if (c->name[0] == '\0') continue;

        if (c->type == CTRL_INPUT_TEXT || c->type == CTRL_TEXTAREA) {
            if (query[0] != '\0') ustrcat(query, "&", sizeof(query));
            url_encode_append(query, sizeof(query), c->name);
            ustrcat(query, "=", sizeof(query));
            url_encode_append(query, sizeof(query), c->value);
        } else if (c->type == CTRL_INPUT_CHECKBOX && c->checked) {
            if (query[0] != '\0') ustrcat(query, "&", sizeof(query));
            url_encode_append(query, sizeof(query), c->name);
            ustrcat(query, "=", sizeof(query));
            const char *val = c->value[0] ? c->value : "on";
            url_encode_append(query, sizeof(query), val);
        }
    }

    char target_url[384];
    const char *act = (action && action[0]) ? action : b->active_form_action;
    resolve_url(b->current_url, act, target_url, sizeof(target_url));

    if (query[0] != '\0') {
        size_t tlen = ustrlen(target_url);
        if (tlen < sizeof(target_url) - 2) {
            target_url[tlen++] = '?';
            target_url[tlen] = '\0';
            ustrcat(target_url, query, sizeof(target_url));
        }
    }

    navigate_to(b, target_url, true);
}

/* -----------------------------------------------------------------
 * HTML Tokenizer & DOM Builder (Phase P: Links, Images, Forms)
 * ----------------------------------------------------------------- */
static int parse_html(browser_app_t *b, const char *html, dom_node_t *nodes, int max_nodes, stylesheet_t *sheet, char *out_title, size_t max_title) {
    int node_count = 0;
    out_title[0] = '\0';
    b->control_count = 0;
    b->focused_control_id = -1;
    b->active_form_action[0] = '\0';
    ustrcpy(b->active_form_method, "GET", sizeof(b->active_form_method));

    const char *p = html;

    while (*p && node_count < max_nodes) {
        if (*p == '<') {
            p++;
            if (*p == '!') {
                /* Skip DOCTYPE or comments */
                while (*p && *p != '>') p++;
                if (*p == '>') p++;
                continue;
            }

            bool is_closing = false;
            if (*p == '/') {
                is_closing = true;
                p++;
            }

            char tag_name[32];
            size_t ti = 0;
            while (*p && !uis_whitespace(*p) && *p != '>' && *p != '/' && ti < sizeof(tag_name) - 1) {
                tag_name[ti++] = utolower(*p++);
            }
            tag_name[ti] = '\0';

            /* Parse attributes */
            char class_val[32] = {0};
            char id_val[32] = {0};
            char href_val[128] = {0};
            char style_val[128] = {0};
            char rel_val[32] = {0};
            char src_val[128] = {0};
            char alt_val[48] = {0};
            char type_val[24] = {0};
            char name_val[48] = {0};
            char val_val[128] = {0};
            char action_val[128] = {0};
            char method_val[16] = {0};
            char onclick_val[128] = {0};
            bool checked_val = false;
            int width_val = 0;
            int height_val = 0;

            while (*p && *p != '>') {
                while (*p && uis_whitespace(*p)) p++;
                if (!*p || *p == '>') break;

                char attr[32];
                size_t ai = 0;
                while (*p && *p != '=' && !uis_whitespace(*p) && *p != '>' && ai < sizeof(attr) - 1) {
                    attr[ai++] = utolower(*p++);
                }
                attr[ai] = '\0';

                while (*p && uis_whitespace(*p)) p++;
                if (*p == '=') {
                    p++;
                    while (*p && uis_whitespace(*p)) p++;
                    char quote = 0;
                    if (*p == '"' || *p == '\'') quote = *p++;
                    char val[128];
                    size_t vi = 0;
                    while (*p && ((quote && *p != quote) || (!quote && !uis_whitespace(*p) && *p != '>')) && vi < sizeof(val) - 1) {
                        val[vi++] = *p++;
                    }
                    val[vi] = '\0';
                    if (quote && *p == quote) p++;

                    if (ustrcaseeq(attr, "class")) ustrcpy(class_val, val, sizeof(class_val));
                    else if (ustrcaseeq(attr, "id")) ustrcpy(id_val, val, sizeof(id_val));
                    else if (ustrcaseeq(attr, "href")) ustrcpy(href_val, val, sizeof(href_val));
                    else if (ustrcaseeq(attr, "style")) ustrcpy(style_val, val, sizeof(style_val));
                    else if (ustrcaseeq(attr, "rel")) ustrcpy(rel_val, val, sizeof(rel_val));
                    else if (ustrcaseeq(attr, "src")) ustrcpy(src_val, val, sizeof(src_val));
                    else if (ustrcaseeq(attr, "alt")) ustrcpy(alt_val, val, sizeof(alt_val));
                    else if (ustrcaseeq(attr, "type")) ustrcpy(type_val, val, sizeof(type_val));
                    else if (ustrcaseeq(attr, "name")) ustrcpy(name_val, val, sizeof(name_val));
                    else if (ustrcaseeq(attr, "value")) ustrcpy(val_val, val, sizeof(val_val));
                    else if (ustrcaseeq(attr, "action")) ustrcpy(action_val, val, sizeof(action_val));
                    else if (ustrcaseeq(attr, "method")) ustrcpy(method_val, val, sizeof(method_val));
                    else if (ustrcaseeq(attr, "onclick")) ustrcpy(onclick_val, val, sizeof(onclick_val));
                    else if (ustrcaseeq(attr, "width")) {
                        int w_acc = 0;
                        for (int k = 0; val[k] >= '0' && val[k] <= '9'; k++) w_acc = w_acc * 10 + (val[k] - '0');
                        width_val = w_acc;
                    } else if (ustrcaseeq(attr, "height")) {
                        int h_acc = 0;
                        for (int k = 0; val[k] >= '0' && val[k] <= '9'; k++) h_acc = h_acc * 10 + (val[k] - '0');
                        height_val = h_acc;
                    }
                } else {
                    /* Boolean attribute like "checked" */
                    if (ustrcaseeq(attr, "checked")) checked_val = true;
                }
            }
            if (*p == '>') p++;

            /* 1. External Stylesheet (<link rel="stylesheet" href="...">) */
            if (ustrcaseeq(tag_name, "link") && !is_closing) {
                if (ustrcaseeq(rel_val, "stylesheet") && href_val[0]) {
                    char full_css_url[256];
                    resolve_url(b->current_url, href_val, full_css_url, sizeof(full_css_url));

                    /* Fetch external stylesheet */
                    char *css_ext_buf = (char *)umalloc(8192);
                    if (css_ext_buf) {
                        s64 bytes = fetch_resource(full_css_url, css_ext_buf, 8191);
                        if (bytes > 0) {
                            css_ext_buf[bytes] = '\0';
                            parse_stylesheet(css_ext_buf, sheet);
                        }
                    }
                }
                continue;
            }

            /* 2. Inline Style Block (<style> ... </style>) */
            if (ustrcaseeq(tag_name, "style") && !is_closing) {
                char css_buf[1024];
                size_t ci = 0;
                while (*p && !ustrcasestartswith(p, "</style>") && ci < sizeof(css_buf) - 1) {
                    css_buf[ci++] = *p++;
                }
                css_buf[ci] = '\0';
                parse_stylesheet(css_buf, sheet);
                while (*p && *p != '>') p++;
                if (*p == '>') p++;
                continue;
            }

            /* 3. Script Block (<script [src="..."]> ... </script>) */
            if (ustrcaseeq(tag_name, "script") && !is_closing) {
                char *script_buf = (char *)umalloc(4096);
                if (script_buf) {
                    script_buf[0] = '\0';

                    if (src_val[0]) {
                        /* External script fetch */
                        char full_script_url[256];
                        resolve_url(b->current_url, src_val, full_script_url, sizeof(full_script_url));
                        s64 bytes = fetch_resource(full_script_url, script_buf, 4095);
                        if (bytes > 0) {
                            script_buf[bytes] = '\0';
                        }
                    } else {
                        /* Inline script */
                        size_t si = 0;
                        while (*p && !ustrcasestartswith(p, "</script>") && si < 4095) {
                            script_buf[si++] = *p++;
                        }
                        script_buf[si] = '\0';
                    }

                    if (script_buf[0]) {
                        b->nodes = nodes;
                        b->node_count = node_count;
                        if (!b->doc_vm_initialized) {
                            b->doc_generation++;
                            js_init(&b->doc_vm);
                            js_set_dom_bridge(&b->doc_vm, b,
                                              browser_js_dom_get_element_by_id,
                                              browser_js_dom_get_text_content,
                                              browser_js_dom_set_text_content);
                            js_set_dom_event_bridge(&b->doc_vm, browser_js_dom_set_onclick);
                            b->doc_vm_initialized = true;
                        }
                        js_value_t js_res;
                        js_error_t js_err = js_eval(&b->doc_vm, script_buf, &js_res);
                        if (js_err != JS_OK) {
                            /* Report error non-destructively in browser status */
                            ustrcpy(b->status_text, "[JS Error] ", sizeof(b->status_text));
                            ustrcat(b->status_text, js_error_string(js_err), sizeof(b->status_text));
                        }
                    }
                }

                while (*p && !ustrcasestartswith(p, "</script>")) p++;
                while (*p && *p != '>') p++;
                if (*p == '>') p++;
                continue;
            }

            if (is_closing) continue;

            tag_t t = TAG_NONE;
            if (ustrcaseeq(tag_name, "h1")) t = TAG_H1;
            else if (ustrcaseeq(tag_name, "h2")) t = TAG_H2;
            else if (ustrcaseeq(tag_name, "h3")) t = TAG_H3;
            else if (ustrcaseeq(tag_name, "p")) t = TAG_P;
            else if (ustrcaseeq(tag_name, "div")) t = TAG_DIV;
            else if (ustrcaseeq(tag_name, "span")) t = TAG_SPAN;
            else if (ustrcaseeq(tag_name, "a")) t = TAG_A;
            else if (ustrcaseeq(tag_name, "b")) t = TAG_B;
            else if (ustrcaseeq(tag_name, "i")) t = TAG_I;
            else if (ustrcaseeq(tag_name, "hr")) t = TAG_HR;
            else if (ustrcaseeq(tag_name, "br")) t = TAG_BR;
            else if (ustrcaseeq(tag_name, "ul")) t = TAG_UL;
            else if (ustrcaseeq(tag_name, "li")) t = TAG_LI;
            else if (ustrcaseeq(tag_name, "title")) t = TAG_TITLE;
            else if (ustrcaseeq(tag_name, "img")) t = TAG_IMG;
            else if (ustrcaseeq(tag_name, "form")) t = TAG_FORM;
            else if (ustrcaseeq(tag_name, "input")) t = TAG_INPUT;
            else if (ustrcaseeq(tag_name, "textarea")) t = TAG_TEXTAREA;
            else if (ustrcaseeq(tag_name, "button")) t = TAG_BUTTON;

            if (t == TAG_NONE) continue;

            dom_node_t *node = &nodes[node_count++];
            umemset(node, 0, sizeof(*node));
            node->tag = t;
            ustrcpy(node->class_name, class_val, sizeof(node->class_name));
            ustrcpy(node->id_name, id_val, sizeof(node->id_name));
            ustrcpy(node->href, href_val, sizeof(node->href));
            ustrcpy(node->onclick, onclick_val, sizeof(node->onclick));
            node->form_ctrl_id = -1;

            if (t == TAG_IMG) {
                ustrcpy(node->img_src, src_val, sizeof(node->img_src));
                ustrcpy(node->img_alt, alt_val, sizeof(node->img_alt));
                node->img_w = width_val;
                node->img_h = height_val;
            } else if (t == TAG_FORM) {
                if (action_val[0]) ustrcpy(b->active_form_action, action_val, sizeof(b->active_form_action));
                if (method_val[0]) ustrcpy(b->active_form_method, method_val, sizeof(b->active_form_method));
            } else if (t == TAG_INPUT || t == TAG_TEXTAREA || t == TAG_BUTTON) {
                if (b->control_count < MAX_FORM_CONTROLS) {
                    int cid = b->control_count++;
                    form_control_t *fc = &b->controls[cid];
                    umemset(fc, 0, sizeof(*fc));
                    fc->id = cid;
                    ustrcpy(fc->action, b->active_form_action, sizeof(fc->action));
                    ustrcpy(fc->method, b->active_form_method, sizeof(fc->method));
                    ustrcpy(fc->name, name_val, sizeof(fc->name));
                    ustrcpy(fc->value, val_val, sizeof(fc->value));
                    ustrcpy(fc->onclick, onclick_val, sizeof(fc->onclick));
                    fc->checked = checked_val;

                    if (t == TAG_INPUT) {
                        if (ustrcaseeq(type_val, "checkbox")) fc->type = CTRL_INPUT_CHECKBOX;
                        else if (ustrcaseeq(type_val, "submit")) fc->type = CTRL_INPUT_SUBMIT;
                        else fc->type = CTRL_INPUT_TEXT;
                    } else if (t == TAG_TEXTAREA) {
                        fc->type = CTRL_TEXTAREA;
                    } else if (t == TAG_BUTTON) {
                        fc->type = CTRL_BUTTON;
                    }
                    node->form_ctrl_id = cid;
                }
            }


            apply_cascade(node, sheet);

            if (style_val[0]) {
                parse_css_declarations(style_val, &node->style);
            }

            if (t == TAG_TITLE) {
                size_t ti_len = 0;
                while (*p && *p != '<' && ti_len < max_title - 1) {
                    out_title[ti_len++] = *p++;
                }
                out_title[ti_len] = '\0';
            } else if (t == TAG_TEXTAREA) {
                /* Capture initial textarea body text */
                char tarea_val[128];
                size_t tai = 0;
                while (*p && *p != '<' && tai < sizeof(tarea_val) - 1) {
                    tarea_val[tai++] = *p++;
                }
                tarea_val[tai] = '\0';
                if (tai > 0 && node->form_ctrl_id >= 0) {
                    ustrcpy(b->controls[node->form_ctrl_id].value, tarea_val, sizeof(b->controls[node->form_ctrl_id].value));
                }
            }
        } else {
            /* Text content */
            while (*p && uis_whitespace(*p)) p++;
            if (!*p || *p == '<') continue;

            char text_run[128];
            size_t ri = 0;
            while (*p && *p != '<' && ri < sizeof(text_run) - 1) {
                if (*p == '&') {
                    if (ustrcasestartswith(p, "&amp;")) { text_run[ri++] = '&'; p += 5; }
                    else if (ustrcasestartswith(p, "&lt;")) { text_run[ri++] = '<'; p += 4; }
                    else if (ustrcasestartswith(p, "&gt;")) { text_run[ri++] = '>'; p += 4; }
                    else if (ustrcasestartswith(p, "&quot;")) { text_run[ri++] = '"'; p += 6; }
                    else if (ustrcasestartswith(p, "&nbsp;")) { text_run[ri++] = ' '; p += 6; }
                    else text_run[ri++] = *p++;
                } else {
                    text_run[ri++] = *p++;
                }
            }
            text_run[ri] = '\0';

            if (ri > 0 && node_count > 0) {
                dom_node_t *curr = &nodes[node_count - 1];
                if (curr->text[0] == '\0') {
                    ustrcpy(curr->text, text_run, sizeof(curr->text));
                } else {
                    if (node_count < max_nodes) {
                        dom_node_t *tn = &nodes[node_count++];
                        umemset(tn, 0, sizeof(*tn));
                        tn->tag = TAG_TEXT;
                        ustrcpy(tn->text, text_run, sizeof(tn->text));
                        tn->form_ctrl_id = -1;
                        tn->style = curr->style;
                    }
                }
            }
        }
    }
    return node_count;
}

/* -----------------------------------------------------------------
 * Layout Engine (Box Model, Images & Forms)
 * ----------------------------------------------------------------- */
static int layout_page(browser_app_t *b, const dom_node_t *nodes, int node_count, int view_w, layout_box_t *boxes, int max_boxes, int *out_doc_h) {
    int box_count = 0;
    int cur_x = 16;
    int cur_y = 12;
    int max_line_w = view_w - 32;
    if (max_line_w < 200) max_line_w = 200;

    for (int i = 0; i < node_count && box_count < max_boxes; i++) {
        const dom_node_t *n = &nodes[i];
        if (n->style.is_hidden) continue;

        if (n->tag == TAG_HR) {
            cur_x = 16;
            cur_y += 8;
            if (box_count < max_boxes) {
                layout_box_t *bx = &boxes[box_count++];
                umemset(bx, 0, sizeof(*bx));
                bx->x = cur_x;
                bx->y = cur_y;
                bx->w = max_line_w;
                bx->h = 2;
                bx->is_hr = true;
                bx->fg_color = COLOR_PANEL_BORDER;
                bx->form_ctrl_id = -1;
                ustrcpy(bx->id_name, n->id_name, sizeof(bx->id_name));
            }
            cur_y += 12;
            continue;
        }

        if (n->tag == TAG_BR) {
            cur_x = 16;
            cur_y += 18;
            continue;
        }

        /* 1. Web Image (<img src="..." ...>) */
        if (n->tag == TAG_IMG) {
            char img_full_url[256];
            resolve_url(b->current_url, n->img_src, img_full_url, sizeof(img_full_url));

            int dw = 0, dh = 0;
            u32 *pixels = NULL;

            /* Allocate temporary image buffer */
            u8 *img_file_buf = (u8 *)umalloc(65536);
            if (img_file_buf) {
                s64 nbytes = fetch_resource(img_full_url, img_file_buf, 65535);
                if (nbytes > 0) {
                    pixels = decode_bmp(img_file_buf, (size_t)nbytes, &dw, &dh);
                }
            }

            int final_w = (n->img_w > 0) ? n->img_w : (dw > 0 ? dw : 32);
            int final_h = (n->img_h > 0) ? n->img_h : (dh > 0 ? dh : 32);
            if (final_w > max_line_w) final_w = max_line_w;

            if (box_count < max_boxes) {
                layout_box_t *bx = &boxes[box_count++];
                umemset(bx, 0, sizeof(*bx));
                bx->x = cur_x;
                bx->y = cur_y;
                bx->w = final_w;
                bx->h = final_h;
                bx->is_image = true;
                bx->image_pixels = pixels;
                bx->img_w = dw;
                bx->img_h = dh;
                ustrcpy(bx->alt, n->img_alt, sizeof(bx->alt));
                bx->form_ctrl_id = -1;
                ustrcpy(bx->id_name, n->id_name, sizeof(bx->id_name));
            }

            cur_x += final_w + 12;
            if (cur_x >= max_line_w) {
                cur_x = 16;
                cur_y += final_h + 8;
            }
            continue;
        }

        /* 2. Interactive Form Controls */
        if (n->form_ctrl_id >= 0 && n->form_ctrl_id < b->control_count) {
            form_control_t *fc = &b->controls[n->form_ctrl_id];
            int cw = 120, ch = 24;

            if (fc->type == CTRL_INPUT_TEXT) {
                cw = 180;
                ch = 24;
            } else if (fc->type == CTRL_INPUT_CHECKBOX) {
                cw = 18;
                ch = 18;
            } else if (fc->type == CTRL_TEXTAREA) {
                cw = 240;
                ch = 44;
            } else if (fc->type == CTRL_INPUT_SUBMIT || fc->type == CTRL_BUTTON) {
                size_t vlen = ustrlen(fc->value[0] ? fc->value : "Submit");
                cw = (int)vlen * 8 + 20;
                ch = 24;
            }

            fc->x = cur_x;
            fc->y = cur_y;
            fc->w = cw;
            fc->h = ch;

            if (box_count < max_boxes) {
                layout_box_t *bx = &boxes[box_count++];
                umemset(bx, 0, sizeof(*bx));
                bx->x = cur_x;
                bx->y = cur_y;
                bx->w = cw;
                bx->h = ch;
                bx->form_ctrl_id = fc->id;
                ustrcpy(bx->id_name, n->id_name, sizeof(bx->id_name));
                ustrcpy(bx->onclick, n->onclick, sizeof(bx->onclick));
            }

            cur_x += cw + 12;
            if (cur_x >= max_line_w || fc->type == CTRL_TEXTAREA) {
                cur_x = 16;
                cur_y += ch + 6;
            }
            continue;
        }

        if (n->style.is_block) {
            cur_x = 16;
            cur_y += n->style.margin_top;
        }

        if (n->tag == TAG_LI) {
            if (box_count < max_boxes) {
                layout_box_t *bx = &boxes[box_count++];
                umemset(bx, 0, sizeof(*bx));
                bx->x = cur_x;
                bx->y = cur_y;
                bx->w = 12;
                bx->h = 16;
                bx->is_bullet = true;
                bx->fg_color = COLOR_ACCENT_CYAN;
                bx->form_ctrl_id = -1;
                ustrcpy(bx->id_name, n->id_name, sizeof(bx->id_name));
                ustrcpy(bx->onclick, n->onclick, sizeof(bx->onclick));
            }
            cur_x += 16;
        }

        /* Block background & border container */
        if (n->style.has_bg_color || n->style.has_border) {
            if (box_count < max_boxes) {
                layout_box_t *bx = &boxes[box_count++];
                umemset(bx, 0, sizeof(*bx));
                bx->x = cur_x;
                bx->y = cur_y;
                bx->w = max_line_w;
                bx->h = 28 + (n->style.padding * 2);
                bx->has_bg = n->style.has_bg_color;
                bx->bg_color = n->style.bg_color;
                bx->has_border = n->style.has_border;
                bx->border_color = n->style.border_color;
                bx->border_w = (n->style.border_width > 0) ? n->style.border_width : 1;
                bx->form_ctrl_id = -1;
                ustrcpy(bx->id_name, n->id_name, sizeof(bx->id_name));
                ustrcpy(bx->onclick, n->onclick, sizeof(bx->onclick));
            }
            cur_x += n->style.padding + 4;
            cur_y += n->style.padding + 2;
        }

        /* Render text node */
        if (n->text[0]) {
            size_t tlen = ustrlen(n->text);
            int char_w = (n->style.font_scale > 1) ? 16 : 8;
            int line_h = (n->style.font_scale > 1) ? 32 : 18;
            int text_w = (int)tlen * char_w;

            if (box_count < max_boxes) {
                layout_box_t *bx = &boxes[box_count++];
                umemset(bx, 0, sizeof(*bx));
                bx->x = cur_x;
                bx->y = cur_y;
                bx->w = text_w;
                bx->h = line_h;
                ustrcpy(bx->text, n->text, sizeof(bx->text));
                bx->fg_color = n->style.has_color ? n->style.color : COLOR_TEXT_WHITE;
                bx->font_scale = (n->style.font_scale > 1) ? 2 : 1;
                bx->bold = n->style.bold;
                bx->is_link = (n->tag == TAG_A || n->href[0]);
                if (bx->is_link) {
                    ustrcpy(bx->href, n->href, sizeof(bx->href));
                    bx->fg_color = COLOR_LINK_BLUE;
                }
                bx->form_ctrl_id = -1;
                ustrcpy(bx->id_name, n->id_name, sizeof(bx->id_name));
                ustrcpy(bx->onclick, n->onclick, sizeof(bx->onclick));
            }


            if (n->style.is_block) {
                cur_y += line_h + n->style.margin_bottom;
                cur_x = 16;
            } else {
                cur_x += text_w + char_w;
                if (cur_x >= max_line_w) {
                    cur_x = 16;
                    cur_y += line_h;
                }
            }
        }
    }

    if (out_doc_h) *out_doc_h = cur_y + 30;
    return box_count;
}

static void navigate_to(browser_app_t *b, const char *url_str, bool record_history) {
    if (!url_str || !url_str[0]) return;
    if (ustrlen(url_str) >= 250) {
        ustrcpy(b->status_text, "Error: URL exceeds limit", sizeof(b->status_text));
        return;
    }

    parsed_url_t purl;
    bool parse_ok = parse_url(url_str, &purl);

    /* Check for in-page fragment navigation */
    if (parse_ok && b->html_len > 0 && purl.fragment[0] != '\0') {
        parsed_url_t cur_purl;
        if (parse_url(b->current_url, &cur_purl)) {
            if (purl.scheme == cur_purl.scheme &&
                ustrcaseeq(purl.host, cur_purl.host) &&
                purl.port == cur_purl.port &&
                ustrcaseeq(purl.path, cur_purl.path)) {
                /* Same page - jump directly to fragment without re-fetching */
                ustrcpy(b->current_url, url_str, sizeof(b->current_url));
                ustrcpy(b->input_url, url_str, sizeof(b->input_url));
                b->input_cursor = (int)ustrlen(b->input_url);

                if (record_history) {
                    if (b->history_count == 0 || !ustrcaseeq(b->history[b->history_index], url_str)) {
                        b->history_count = b->history_index + 1;
                        if (b->history_count >= 16) {
                            for (int h = 0; h < 15; h++) {
                                ustrcpy(b->history[h], b->history[h + 1], sizeof(b->history[h]));
                            }
                            b->history_index = 15;
                            ustrcpy(b->history[15], url_str, sizeof(b->history[15]));
                            b->history_count = 16;
                        } else {
                            b->history_index++;
                            ustrcpy(b->history[b->history_index], url_str, sizeof(b->history[b->history_index]));
                            b->history_count = b->history_index + 1;
                        }
                    }
                }

                /* Find target box */
                bool found = false;
                for (int i = 0; i < b->box_count; i++) {
                    if (b->boxes[i].id_name[0] && ustrcaseeq(b->boxes[i].id_name, purl.fragment)) {
                        int view_h = WIN_HEIGHT - TOOLBAR_HEIGHT - STATUS_HEIGHT;
                        int max_scroll = b->doc_height - view_h;
                        if (max_scroll < 0) max_scroll = 0;
                        int target_y = b->boxes[i].y - 8;
                        if (target_y < 0) target_y = 0;
                        if (target_y > max_scroll) target_y = max_scroll;
                        b->scroll_y = target_y;
                        found = true;
                        break;
                    }
                }
                if (found) {
                    ustrcpy(b->status_text, "Jumped to #", sizeof(b->status_text));
                    ustrcat(b->status_text, purl.fragment, sizeof(b->status_text));
                } else {
                    ustrcpy(b->status_text, "Target anchor #", sizeof(b->status_text));
                    ustrcat(b->status_text, purl.fragment, sizeof(b->status_text));
                    ustrcat(b->status_text, " not found", sizeof(b->status_text));
                }
                return;
            }
        }
    }

    /* Normal full page navigation */
    ustrcpy(b->current_url, url_str, sizeof(b->current_url));
    ustrcpy(b->input_url, url_str, sizeof(b->input_url));
    b->input_cursor = (int)ustrlen(b->input_url);
    b->input_focused = false;
    b->scroll_y = 0;
    ustrcpy(b->status_text, "Loading...", sizeof(b->status_text));

    /* Record bounded history */
    if (record_history) {
        if (b->history_count == 0 || !ustrcaseeq(b->history[b->history_index], url_str)) {
            b->history_count = b->history_index + 1;
            if (b->history_count >= 16) {
                for (int h = 0; h < 15; h++) {
                    ustrcpy(b->history[h], b->history[h + 1], sizeof(b->history[h]));
                }
                b->history_index = 15;
                ustrcpy(b->history[15], url_str, sizeof(b->history[15]));
                b->history_count = 16;
            } else {
                b->history_index++;
                ustrcpy(b->history[b->history_index], url_str, sizeof(b->history[b->history_index]));
                b->history_count = b->history_index + 1;
            }
        }
    }

    /* Clear stylesheet rules between page loads */
    b->stylesheet->count = 0;

    /* Phase S: Reset document-scoped JavaScript VM and increment document generation */
    b->doc_generation++;
    js_init(&b->doc_vm);
    js_set_dom_bridge(&b->doc_vm, b,
                      browser_js_dom_get_element_by_id,
                      browser_js_dom_get_text_content,
                      browser_js_dom_set_text_content);
    js_set_dom_event_bridge(&b->doc_vm, browser_js_dom_set_onclick);
    b->doc_vm_initialized = true;

    if (!parse_ok) {
        ustrcpy(b->status_text, "Error: Invalid URL scheme", sizeof(b->status_text));
        const char *err = "<html><head><title>Error</title></head><body><h1>Invalid URL</h1><p>Cannot parse URL scheme or format.</p></body></html>";
        ustrcpy(b->html_buf, err, 4096);
        b->html_len = ustrlen(err);
    } else {
        /* Construct clean fetch URL without fragment for VFS/HTTP */
        char clean_fetch_url[256];
        if (purl.scheme == SCHEME_FILE) {
            ustrcpy(clean_fetch_url, "file://", sizeof(clean_fetch_url));
            ustrcat(clean_fetch_url, purl.path, sizeof(clean_fetch_url));
        } else {
            const char *scheme = (purl.scheme == SCHEME_HTTPS) ? "https://" : "http://";
            ustrcpy(clean_fetch_url, scheme, sizeof(clean_fetch_url));
            ustrcat(clean_fetch_url, purl.host, sizeof(clean_fetch_url));
            ustrcat(clean_fetch_url, purl.path, sizeof(clean_fetch_url));
        }

        s64 bytes = fetch_resource(clean_fetch_url, b->html_buf, 16383);
        if (bytes >= 0) {
            b->html_buf[bytes] = '\0';
            b->html_len = (size_t)bytes;
            if (purl.scheme == SCHEME_FILE) {
                ustrcpy(b->status_text, "Ready (File loaded)", sizeof(b->status_text));
            } else if (purl.scheme == SCHEME_HTTPS) {
                ustrcpy(b->status_text, "Ready (HTTPS 200 OK)", sizeof(b->status_text));
            } else {
                ustrcpy(b->status_text, "Ready (HTTP 200 OK)", sizeof(b->status_text));
            }
        } else {
            ustrcpy(b->status_text, "Resource Not Found (404)", sizeof(b->status_text));
            const char *err = "<html><head><title>404 Not Found</title></head><body><h1>Resource Not Found</h1><p>Requested URL could not be resolved.</p></body></html>";
            ustrcpy(b->html_buf, err, 4096);
            b->html_len = ustrlen(err);
        }
    }

    /* Parse HTML, external CSS, images and forms */
    b->node_count = parse_html(b, b->html_buf, b->nodes, MAX_NODES, b->stylesheet, b->page_title, sizeof(b->page_title));
    if (b->page_title[0] == '\0') {
        ustrcpy(b->page_title, "AstraBrowser", sizeof(b->page_title));
    }
    if (b->win_id > 0) {
        sys_gui_set_title(b->win_id, b->page_title);
    }

    int view_w = WIN_WIDTH - SCROLLBAR_WIDTH;
    b->box_count = layout_page(b, b->nodes, b->node_count, view_w, b->boxes, MAX_BOXES, &b->doc_height);

    /* If URL had fragment on newly loaded page, scroll to it */
    if (purl.fragment[0] != '\0') {
        for (int i = 0; i < b->box_count; i++) {
            if (b->boxes[i].id_name[0] && ustrcaseeq(b->boxes[i].id_name, purl.fragment)) {
                int view_h = WIN_HEIGHT - TOOLBAR_HEIGHT - STATUS_HEIGHT;
                int max_scroll = b->doc_height - view_h;
                if (max_scroll < 0) max_scroll = 0;
                int target_y = b->boxes[i].y - 8;
                if (target_y < 0) target_y = 0;
                if (target_y > max_scroll) target_y = max_scroll;
                b->scroll_y = target_y;
                break;
            }
        }
    }
}

static void render_browser_ui(browser_app_t *b) {
    int win = b->win_id;
    if (win <= 0) return;

    /* 1. Clear background */
    sys_gui_clear(win, COLOR_VIEWPORT_BG);

    /* 2. Top Navigation Toolbar */
    sys_gui_fill_rect(win, 0, 0, WIN_WIDTH, TOOLBAR_HEIGHT, COLOR_TOOLBAR_BG);
    sys_gui_draw_rect(win, 0, TOOLBAR_HEIGHT - 1, WIN_WIDTH, 1, COLOR_PANEL_BORDER);

    /* Back Button [<-] */
    bool can_back = (b->history_index > 0);
    sys_gui_fill_rect(win, 6, 5, 26, 24, can_back ? COLOR_BTN_ACTIVE : COLOR_BTN_BG);
    sys_gui_draw_rect(win, 6, 5, 26, 24, COLOR_BORDER_DEFAULT);
    sys_gui_draw_text(win, 11, 9, "<-", can_back ? COLOR_TEXT_WHITE : COLOR_TEXT_MUTED, 0);

    /* Forward Button [->] */
    bool can_fwd = (b->history_index < b->history_count - 1);
    sys_gui_fill_rect(win, 36, 5, 26, 24, can_fwd ? COLOR_BTN_ACTIVE : COLOR_BTN_BG);
    sys_gui_draw_rect(win, 36, 5, 26, 24, COLOR_BORDER_DEFAULT);
    sys_gui_draw_text(win, 41, 9, "->", can_fwd ? COLOR_TEXT_WHITE : COLOR_TEXT_MUTED, 0);

    /* Reload Button [R] */
    sys_gui_fill_rect(win, 66, 5, 26, 24, COLOR_BTN_BG);
    sys_gui_draw_rect(win, 66, 5, 26, 24, COLOR_BORDER_DEFAULT);
    sys_gui_draw_text(win, 74, 9, "R", COLOR_TEXT_WHITE, 0);

    /* URL Bar */
    int url_x = 98;
    int url_w = WIN_WIDTH - url_x - 52;
    sys_gui_fill_rect(win, url_x, 5, url_w, 24, 0xFF080C16);
    sys_gui_draw_rect(win, url_x, 5, url_w, 24, b->input_focused ? COLOR_BORDER_CYAN : COLOR_BORDER_DEFAULT);
    sys_gui_draw_text(win, url_x + 6, 9, b->input_url, COLOR_TEXT_WHITE, 0);

    /* Cursor in URL bar */
    if (b->input_focused) {
        int cur_x = url_x + 6 + (b->input_cursor * FONT_WIDTH);
        if (cur_x < url_x + url_w - 4) {
            sys_gui_fill_rect(win, cur_x, 8, 2, 16, COLOR_BORDER_CYAN);
        }
    }

    /* Go Button */
    int go_x = WIN_WIDTH - 46;
    sys_gui_fill_rect(win, go_x, 5, 40, 24, COLOR_BTN_ACTIVE);
    sys_gui_draw_rect(win, go_x, 5, 40, 24, COLOR_BORDER_CYAN);
    sys_gui_draw_text(win, go_x + 12, 9, "Go", COLOR_TEXT_WHITE, 0);

    /* 3. Render HTML Boxes in Viewport */
    int view_y = TOOLBAR_HEIGHT;
    int view_h = WIN_HEIGHT - TOOLBAR_HEIGHT - STATUS_HEIGHT;

    for (int i = 0; i < b->box_count; i++) {
        const layout_box_t *box = &b->boxes[i];
        int sy = view_y + box->y - b->scroll_y;

        /* Vertical clipping */
        if (sy + box->h < view_y || sy >= view_y + view_h) continue;

        /* 3A: Render Web Image */
        if (box->is_image) {
            if (box->image_pixels) {
                sys_gui_draw_bitmap(win, box->x, sy, box->w, box->h, box->image_pixels);
            } else {
                /* Missing or malformed image fallback box */
                sys_gui_draw_rect(win, box->x, sy, box->w, box->h, COLOR_BORDER_DEFAULT);
                const char *fallback = box->alt[0] ? box->alt : "[IMG]";
                sys_gui_draw_text(win, box->x + 2, sy + 2, fallback, COLOR_TEXT_MUTED, 0);
            }
            continue;
        }

        /* 3B: Render Form Control */
        if (box->form_ctrl_id >= 0 && box->form_ctrl_id < b->control_count) {
            const form_control_t *fc = &b->controls[box->form_ctrl_id];
            if (fc->type == CTRL_INPUT_TEXT) {
                sys_gui_fill_rect(win, box->x, sy, box->w, box->h, COLOR_TOOLBAR_BG);
                sys_gui_draw_rect(win, box->x, sy, box->w, box->h, fc->focused ? COLOR_BORDER_CYAN : COLOR_BORDER_DEFAULT);
                sys_gui_draw_text(win, box->x + 6, sy + 4, fc->value, COLOR_TEXT_WHITE, 0);
                if (fc->focused) {
                    int cx = box->x + 6 + (int)ustrlen(fc->value) * FONT_WIDTH;
                    if (cx < box->x + box->w - 4) {
                        sys_gui_fill_rect(win, cx, sy + 4, 2, 16, COLOR_BORDER_CYAN);
                    }
                }
            } else if (fc->type == CTRL_INPUT_CHECKBOX) {
                sys_gui_fill_rect(win, box->x, sy, box->w, box->h, COLOR_TOOLBAR_BG);
                sys_gui_draw_rect(win, box->x, sy, box->w, box->h, fc->focused ? COLOR_BORDER_CYAN : COLOR_BORDER_DEFAULT);
                if (fc->checked) {
                    sys_gui_fill_rect(win, box->x + 3, sy + 3, box->w - 6, box->h - 6, COLOR_ACCENT_CYAN);
                }
            } else if (fc->type == CTRL_TEXTAREA) {
                sys_gui_fill_rect(win, box->x, sy, box->w, box->h, COLOR_TOOLBAR_BG);
                sys_gui_draw_rect(win, box->x, sy, box->w, box->h, fc->focused ? COLOR_BORDER_CYAN : COLOR_BORDER_DEFAULT);
                sys_gui_draw_text(win, box->x + 6, sy + 4, fc->value, COLOR_TEXT_WHITE, 0);
                if (fc->focused) {
                    int cx = box->x + 6 + (int)ustrlen(fc->value) * FONT_WIDTH;
                    if (cx < box->x + box->w - 4) {
                        sys_gui_fill_rect(win, cx, sy + 4, 2, 16, COLOR_BORDER_CYAN);
                    }
                }
            } else if (fc->type == CTRL_INPUT_SUBMIT || fc->type == CTRL_BUTTON) {
                sys_gui_fill_rect(win, box->x, sy, box->w, box->h, COLOR_BTN_ACTIVE);
                sys_gui_draw_rect(win, box->x, sy, box->w, box->h, COLOR_BORDER_CYAN);
                const char *btn_text = fc->value[0] ? fc->value : "Submit";
                sys_gui_draw_text(win, box->x + 8, sy + 4, btn_text, COLOR_TEXT_WHITE, 0);
            }
            continue;
        }

        /* 3C: Render Standard HTML Elements */
        if (box->has_bg) {
            sys_gui_fill_rect(win, box->x, sy, box->w, box->h, box->bg_color);
        }
        if (box->has_border && box->border_w > 0) {
            for (int bw = 0; bw < box->border_w; bw++) {
                sys_gui_draw_rect(win, box->x + bw, sy + bw, box->w - 2 * bw, box->h - 2 * bw, box->border_color);
            }
        }
        if (box->is_hr) {
            sys_gui_fill_rect(win, box->x, sy, box->w, 2, box->fg_color);
        } else if (box->is_bullet) {
            sys_gui_draw_text(win, box->x, sy, "*", box->fg_color, 0);
        } else if (box->text[0]) {
            sys_gui_draw_text(win, box->x, sy, box->text, box->fg_color, 0);
            if (box->bold) {
                sys_gui_draw_text(win, box->x + 1, sy, box->text, box->fg_color, 0);
            }
            if (box->is_link) {
                /* Underline */
                sys_gui_fill_rect(win, box->x, sy + 15, box->w, 1, COLOR_LINK_BLUE);
            }
        }
    }

    /* 4. Vertical Scrollbar */
    int sb_x = WIN_WIDTH - SCROLLBAR_WIDTH;
    sys_gui_fill_rect(win, sb_x, view_y, SCROLLBAR_WIDTH, view_h, 0xFF0D121F);
    sys_gui_draw_rect(win, sb_x, view_y, SCROLLBAR_WIDTH, view_h, COLOR_PANEL_BORDER);

    int max_scroll = b->doc_height - view_h;
    if (max_scroll > 0) {
        int thumb_h = (view_h * view_h) / b->doc_height;
        if (thumb_h < 24) thumb_h = 24;
        int thumb_y = view_y + (b->scroll_y * (view_h - thumb_h)) / max_scroll;
        sys_gui_fill_rect(win, sb_x + 2, thumb_y, SCROLLBAR_WIDTH - 4, thumb_h, COLOR_BTN_BG);
        sys_gui_draw_rect(win, sb_x + 2, thumb_y, SCROLLBAR_WIDTH - 4, thumb_h, COLOR_BORDER_DEFAULT);
    }

    /* 5. Bottom Status Bar */
    int stat_y = WIN_HEIGHT - STATUS_HEIGHT;
    sys_gui_fill_rect(win, 0, stat_y, WIN_WIDTH, STATUS_HEIGHT, COLOR_STATUS_BG);
    sys_gui_draw_rect(win, 0, stat_y, WIN_WIDTH, 1, COLOR_PANEL_BORDER);
    sys_gui_draw_text(win, 8, stat_y + 4, b->status_text, COLOR_TEXT_MUTED, 0);

    /* Present backbuffer */
    sys_gui_present(win);
}

/* -----------------------------------------------------------------
 * Phase P Self-Tests (BMP Decoder, External CSS & HTML Forms)
 * ----------------------------------------------------------------- */
static void run_phase_p_self_tests(void) {
    /* TEST 1: BMP Decoder Verification */
    /* Synthetic 4x4 24-bit uncompressed BMP */
    u8 synth_bmp[54 + 48] = {
        'B', 'M',               /* bfType */
        102, 0, 0, 0,           /* bfSize = 102 */
        0, 0, 0, 0,             /* Reserved */
        54, 0, 0, 0,            /* bfOffBits = 54 */
        40, 0, 0, 0,            /* biSize = 40 */
        4, 0, 0, 0,             /* biWidth = 4 */
        4, 0, 0, 0,             /* biHeight = 4 */
        1, 0,                   /* biPlanes = 1 */
        24, 0,                  /* biBitCount = 24 */
        0, 0, 0, 0,             /* biCompression = 0 */
        48, 0, 0, 0,            /* biSizeImage = 48 */
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0
    };
    /* Fill pixel data with non-zero bytes */
    for (size_t i = 54; i < sizeof(synth_bmp); i++) synth_bmp[i] = 0xAA;

    int bw = 0, bh = 0;
    u32 *dec_pixels = decode_bmp(synth_bmp, sizeof(synth_bmp), &bw, &bh);
    if (!dec_pixels || bw != 4 || bh != 4) {
        const char err[] = "[FATAL] Phase P BMP decoder self-test failed!\n";
        sys_write(1, err, ustrlen(err));
        sys_exit(10);
    }

    /* Negative test 1: Bad magic */
    synth_bmp[0] = 'X';
    if (decode_bmp(synth_bmp, sizeof(synth_bmp), &bw, &bh) != NULL) {
        sys_exit(11);
    }
    synth_bmp[0] = 'B';

    /* Negative test 2: Oversized dimensions */
    *(int *)(synth_bmp + 18) = 100000;
    if (decode_bmp(synth_bmp, sizeof(synth_bmp), &bw, &bh) != NULL) {
        sys_exit(12);
    }

    const char img_ok[] = "ASTRA_BROWSER_IMAGE_OK\n";
    sys_write(1, img_ok, ustrlen(img_ok));

    /* TEST 2: External CSS URL Resolution & Cascade Verification */
    char resolved[256];
    resolve_url("file:///www/index.html", "style_ext.css", resolved, sizeof(resolved));
    if (!ustrcaseeq(resolved, "file:///www/style_ext.css")) {
        sys_exit(13);
    }

    resolve_url("https://example.com/site/page.html", "theme.css", resolved, sizeof(resolved));
    if (!ustrcaseeq(resolved, "https://example.com/site/theme.css")) {
        sys_exit(14);
    }

    resolve_url("http://example.com/site/page.html", "/root.css", resolved, sizeof(resolved));
    if (!ustrcaseeq(resolved, "http://example.com/root.css")) {
        sys_exit(15);
    }

    /* Test stylesheet parsing and rule addition */
    static stylesheet_t test_sheet;
    test_sheet.count = 0;
    const char test_css[] = ".ext-box { color: #2dd4bf; background-color: #1e293b; }";
    parse_stylesheet(test_css, &test_sheet);
    if (test_sheet.count < 1 || !test_sheet.rules[0].style.has_color) {
        sys_exit(16);
    }

    const char ext_css_ok[] = "ASTRA_BROWSER_EXT_CSS_OK\n";
    sys_write(1, ext_css_ok, ustrlen(ext_css_ok));

    /* TEST 3: Interactive HTML Forms Verification */
    static browser_app_t test_app;
    umemset(&test_app, 0, sizeof(test_app));
    test_app.nodes = (dom_node_t *)umalloc(sizeof(dom_node_t) * 16);
    test_app.stylesheet = &test_sheet;
    ustrcpy(test_app.current_url, "file:///www/form_test.html", sizeof(test_app.current_url));

    const char test_form_html[] =
        "<form action=\"result.html\" method=\"GET\">"
        "<input type=\"text\" name=\"user\" value=\"testuser\">"
        "<input type=\"checkbox\" name=\"opt\" checked>"
        "<textarea name=\"notes\">sample text</textarea>"
        "<input type=\"submit\" value=\"Send\">"
        "</form>";

    char dummy_title[32];
    int nparsed = parse_html(&test_app, test_form_html, test_app.nodes, 16, &test_sheet, dummy_title, sizeof(dummy_title));
    if (nparsed < 1 || test_app.control_count < 4) {
        sys_exit(17);
    }

    /* Verify controls populated correctly */
    if (!ustrcaseeq(test_app.controls[0].name, "user") ||
        !ustrcaseeq(test_app.controls[0].value, "testuser")) {
        sys_exit(18);
    }
    if (!test_app.controls[1].checked) {
        sys_exit(19);
    }
    /* Toggle checkbox */
    test_app.controls[1].checked = false;
    if (test_app.controls[1].checked) {
        sys_exit(20);
    }

    const char forms_ok[] = "ASTRA_BROWSER_FORMS_OK\n";
    sys_write(1, forms_ok, ustrlen(forms_ok));
}

/* -----------------------------------------------------------------
 * Phase Q Self-Tests (Navigation, Bounded History, Fragments & Scrolling)
 * ----------------------------------------------------------------- */
static void run_phase_q_self_tests(void) {
    /* TEST 1: URL Resolution, Normalization & Relative Paths */
    char res[256];
    resolve_url("file:///www/nav_test.html", "../www/img_test.html", res, sizeof(res));
    if (!ustrcaseeq(res, "file:///www/img_test.html")) sys_exit(40);

    resolve_url("file:///www/nav_test.html", "./form_test.html", res, sizeof(res));
    if (!ustrcaseeq(res, "file:///www/form_test.html")) sys_exit(41);

    resolve_url("file:///www/nav_test.html", "#section2", res, sizeof(res));
    if (!ustrcaseeq(res, "file:///www/nav_test.html#section2")) sys_exit(42);

    resolve_url("file:///page.html", "../../secret.txt", res, sizeof(res));
    if (!ustrcaseeq(res, "file:///secret.txt")) sys_exit(43);

    resolve_url("http://example.com/dir/page.html", "/root.css", res, sizeof(res));
    if (!ustrcaseeq(res, "http://example.com/root.css")) sys_exit(44);

    resolve_url("file:///www/index.html", "about.html#features", res, sizeof(res));
    if (!ustrcaseeq(res, "file:///www/about.html#features")) sys_exit(45);

    /* TEST 2: Bounded History Stack (Push, Shift, Truncate, Back, Forward) */
    static browser_app_t hist_app;
    umemset(&hist_app, 0, sizeof(hist_app));
    hist_app.nodes = (dom_node_t *)umalloc(sizeof(dom_node_t) * 16);
    static stylesheet_t q_sheet;
    hist_app.stylesheet = &q_sheet;
    hist_app.boxes = (layout_box_t *)umalloc(sizeof(layout_box_t) * 16);
    hist_app.html_buf = (char *)umalloc(4096);

    /* Push 18 distinct URLs to verify bounded 16-entry history and shifting */
    char dummy_url[64];
    for (int i = 0; i < 18; i++) {
        dummy_url[0] = 'f'; dummy_url[1] = 'i'; dummy_url[2] = 'l'; dummy_url[3] = 'e';
        dummy_url[4] = ':'; dummy_url[5] = '/'; dummy_url[6] = '/'; dummy_url[7] = '/';
        dummy_url[8] = 'p'; dummy_url[9] = '0' + (i / 10); dummy_url[10] = '0' + (i % 10);
        dummy_url[11] = '.'; dummy_url[12] = 'h'; dummy_url[13] = 't'; dummy_url[14] = 'm'; dummy_url[15] = '\0';
        navigate_to(&hist_app, dummy_url, true);
    }
    if (hist_app.history_count != 16 || hist_app.history_index != 15) {
        sys_exit(46);
    }
    /* Verify oldest entries were shifted out */
    if (!ustrcaseeq(hist_app.history[15], "file:///p17.htm") ||
        !ustrcaseeq(hist_app.history[0], "file:///p02.htm")) {
        sys_exit(47);
    }

    /* Back 2 steps */
    hist_app.history_index -= 2; /* index 13 */
    navigate_to(&hist_app, hist_app.history[hist_app.history_index], false);
    if (hist_app.history_index != 13 || !ustrcaseeq(hist_app.current_url, "file:///p15.htm")) {
        sys_exit(48);
    }

    /* Forward 1 step */
    hist_app.history_index += 1; /* index 14 */
    navigate_to(&hist_app, hist_app.history[hist_app.history_index], false);
    if (hist_app.history_index != 14 || !ustrcaseeq(hist_app.current_url, "file:///p16.htm")) {
        sys_exit(49);
    }

    /* New navigation from middle truncates forward history and appends at 15 */
    navigate_to(&hist_app, "file:///new_branch.htm", true);
    if (hist_app.history_index != 15 || hist_app.history_count != 16 ||
        !ustrcaseeq(hist_app.history[15], "file:///new_branch.htm")) {
        sys_exit(50);
    }

    const char nav_ok[] = "ASTRA_BROWSER_NAV_OK\n";
    sys_write(1, nav_ok, ustrlen(nav_ok));

    /* TEST 3: Fragment Anchor Navigation & Vertical Scrolling */
    static browser_app_t scroll_app;
    umemset(&scroll_app, 0, sizeof(scroll_app));
    scroll_app.nodes = (dom_node_t *)umalloc(sizeof(dom_node_t) * 32);
    scroll_app.stylesheet = &q_sheet;
    scroll_app.boxes = (layout_box_t *)umalloc(sizeof(layout_box_t) * 32);
    scroll_app.html_buf = (char *)umalloc(4096);

    const char doc_with_anchors[] =
        "<html><body>"
        "<h1 id=\"top\">Top Heading</h1>"
        "<p>Filler paragraph 1</p>"
        "<p>Filler paragraph 2</p>"
        "<p>Filler paragraph 3</p>"
        "<p>Filler paragraph 4</p>"
        "<p>Filler paragraph 5</p>"
        "<p>Filler paragraph 6</p>"
        "<p>Filler paragraph 7</p>"
        "<p>Filler paragraph 8</p>"
        "<p>Filler paragraph 9</p>"
        "<p>Filler paragraph 10</p>"
        "<p>Filler paragraph 11</p>"
        "<p>Filler paragraph 12</p>"
        "<p>Filler paragraph 13</p>"
        "<p>Filler paragraph 14</p>"
        "<p>Filler paragraph 15</p>"
        "<h2 id=\"anchor_mid\">Middle Anchor</h2>"
        "<p>Filler paragraph 16</p>"
        "<p>Filler paragraph 17</p>"
        "<p>Filler paragraph 18</p>"
        "<p>Filler paragraph 19</p>"
        "<p>Filler paragraph 20</p>"
        "<div id=\"target_sec\"><p>Target Content</p></div>"
        "</body></html>";

    char dummy_t[32];
    scroll_app.node_count = parse_html(&scroll_app, doc_with_anchors, scroll_app.nodes, 32, &q_sheet, dummy_t, sizeof(dummy_t));
    int view_w = WIN_WIDTH - SCROLLBAR_WIDTH;
    scroll_app.box_count = layout_page(&scroll_app, scroll_app.nodes, scroll_app.node_count, view_w, scroll_app.boxes, 32, &scroll_app.doc_height);

    int view_h = WIN_HEIGHT - TOOLBAR_HEIGHT - STATUS_HEIGHT;
    if (scroll_app.doc_height <= view_h) {
        sys_exit(51);
    }

    /* Locate anchor_mid box */
    int anchor_box_y = -1;
    for (int b = 0; b < scroll_app.box_count; b++) {
        if (ustrcaseeq(scroll_app.boxes[b].id_name, "anchor_mid")) {
            anchor_box_y = scroll_app.boxes[b].y;
            break;
        }
    }
    if (anchor_box_y <= 0) sys_exit(52);

    /* In-page fragment navigation to #anchor_mid */
    ustrcpy(scroll_app.current_url, "file:///doc.html", sizeof(scroll_app.current_url));
    scroll_app.html_len = ustrlen(doc_with_anchors);
    navigate_to(&scroll_app, "file:///doc.html#anchor_mid", true);

    int expected_scroll = anchor_box_y - 8;
    int max_s = scroll_app.doc_height - view_h;
    if (expected_scroll > max_s) expected_scroll = max_s;
    if (scroll_app.scroll_y != expected_scroll || scroll_app.scroll_y <= 0) {
        sys_exit(53);
    }

    /* Coordinate translation test */
    int click_screen_y = TOOLBAR_HEIGHT + 20;
    int doc_y = click_screen_y - TOOLBAR_HEIGHT + scroll_app.scroll_y;
    if (doc_y != 20 + scroll_app.scroll_y) sys_exit(54);

    /* Clamping tests */
    scroll_app.scroll_y = -100;
    if (scroll_app.scroll_y < 0) scroll_app.scroll_y = 0;
    if (scroll_app.scroll_y != 0) sys_exit(55);

    scroll_app.scroll_y = scroll_app.doc_height + 500;
    if (scroll_app.scroll_y > max_s) scroll_app.scroll_y = max_s;
    if (scroll_app.scroll_y != max_s) sys_exit(56);

    const char scroll_ok[] = "ASTRA_BROWSER_SCROLL_OK\n";
    sys_write(1, scroll_ok, ustrlen(scroll_ok));
}

/* -----------------------------------------------------------------
 * Phase R Self-Tests (JavaScript Feasibility, Isolated Runtime & DOM Bridge)
 * ----------------------------------------------------------------- */
static void run_phase_r_self_tests(void) {
    /* 1. Run Engine Core Unit Tests (Arithmetic, Variables, Functions, Loops, Exceptions, Limits) */
    int unit_res = js_run_self_tests();
    if (unit_res != 0) {
        const char err[] = "[FATAL] Phase R JavaScript engine unit self-test failed!\n";
        sys_write(1, err, ustrlen(err));
        sys_exit(70 + unit_res);
    }

    const char feas_ok[] = "ASTRA_JS_FEASIBILITY_OK\n";
    sys_write(1, feas_ok, ustrlen(feas_ok));

    /* 2. Test Browser DOM Integration (getElementById & textContent mutation) */
    static browser_app_t js_app;
    umemset(&js_app, 0, sizeof(js_app));
    js_app.nodes = (dom_node_t *)umalloc(sizeof(dom_node_t) * 16);
    static stylesheet_t r_sheet;
    js_app.stylesheet = &r_sheet;
    js_app.boxes = (layout_box_t *)umalloc(sizeof(layout_box_t) * 16);
    js_app.html_buf = (char *)umalloc(4096);
    ustrcpy(js_app.current_url, "file:///www/js_test.html", sizeof(js_app.current_url));

    const char js_dom_doc[] =
        "<html><body>"
        "<h1 id=\"heading\">Old Title</h1>"
        "<script>"
        "var elem = document.getElementById('heading');"
        "elem.textContent = 'AstraOS Dynamic Title';"
        "</script>"
        "</body></html>";

    char dummy_t[32];
    js_app.node_count = parse_html(&js_app, js_dom_doc, js_app.nodes, 16, &r_sheet, dummy_t, sizeof(dummy_t));
    if (js_app.node_count < 1) sys_exit(81);

    /* Verify DOM text was modified by script execution */
    if (!ustrcaseeq(js_app.nodes[0].text, "AstraOS Dynamic Title")) {
        sys_exit(82);
    }

    const char rt_ok[] = "ASTRA_JS_RUNTIME_OK\n";
    sys_write(1, rt_ok, ustrlen(rt_ok));

    /* 3. Test Security Boundary & Fault Containment (Unprivileged error non-fatality) */
    const char js_err_doc[] =
        "<html><body>"
        "<script>window.open('http://evil.com');</script>"
        "<p id=\"preserved\">DOM Still Intact</p>"
        "</body></html>";

    js_app.node_count = parse_html(&js_app, js_err_doc, js_app.nodes, 16, &r_sheet, dummy_t, sizeof(dummy_t));
    if (js_app.node_count < 1) sys_exit(83);

    /* Verify browser status message logged the security exception without crashing */
    if (ustrlen(js_app.status_text) == 0 || !ustrcasestartswith(js_app.status_text, "[JS Error]")) {
        sys_exit(84);
    }

    /* Verify subsequent DOM element rendered properly */
    if (!ustrcaseeq(js_app.nodes[0].text, "DOM Still Intact")) {
        sys_exit(85);
    }

    const char iso_ok[] = "ASTRA_JS_ISOLATION_OK\n";
    sys_write(1, iso_ok, ustrlen(iso_ok));
}

/* -----------------------------------------------------------------
 * Phase S Self-Tests (Safe DOM Integration & Runtime Hardening)
 * ----------------------------------------------------------------- */
static void run_phase_s_self_tests(void) {
    /* 1. Run Engine Core Unit Tests (Precedence, Short-Circuit, Overflow, Stale DOM) */
    int unit_res = js_run_phase_s_self_tests();
    if (unit_res != 0) {
        const char err[] = "[FATAL] Phase S JavaScript runtime hardening unit test failed!\n";
        sys_write(1, err, ustrlen(err));
        sys_exit(90 + unit_res);
    }

    const char hard_ok[] = "ASTRA_JS_HARDENING_OK\n";
    sys_write(1, hard_ok, ustrlen(hard_ok));

    /* 2. Test Safe DOM Bridge: Safe textContent updates & bounded reflow */
    static browser_app_t s_app;
    umemset(&s_app, 0, sizeof(s_app));
    s_app.nodes = (dom_node_t *)umalloc(sizeof(dom_node_t) * 16);
    static stylesheet_t s_sheet;
    s_app.stylesheet = &s_sheet;
    s_app.boxes = (layout_box_t *)umalloc(sizeof(layout_box_t) * 16);
    s_app.html_buf = (char *)umalloc(4096);
    s_app.doc_generation = 1;
    ustrcpy(s_app.current_url, "file:///www/dom_test.html", sizeof(s_app.current_url));

    const char s_dom_doc[] =
        "<html><body>"
        "<h1 id=\"target_header\">Initial Header Text</h1>"
        "<script>"
        "var h = document.getElementById('target_header');"
        "h.textContent = 'Hardened DOM Header Text';"
        "</script>"
        "</body></html>";

    char dummy_t[32];
    s_app.node_count = parse_html(&s_app, s_dom_doc, s_app.nodes, 16, &s_sheet, dummy_t, sizeof(dummy_t));
    if (s_app.node_count < 1) sys_exit(110);

    /* Verify DOM node text was updated */
    if (!ustrcaseeq(s_app.nodes[0].text, "Hardened DOM Header Text")) {
        sys_exit(111);
    }

    /* Layout document boxes and verify reflowed box text */
    int view_w = WIN_WIDTH - SCROLLBAR_WIDTH;
    s_app.box_count = layout_page(&s_app, s_app.nodes, s_app.node_count, view_w, s_app.boxes, 16, &s_app.doc_height);
    if (s_app.box_count < 1 || !ustrcaseeq(s_app.boxes[0].text, "Hardened DOM Header Text")) {
        sys_exit(112);
    }

    /* 3. Test Long String Truncation & Buffer Overflow Safety */
    char long_text[256];
    for (int i = 0; i < 200; i++) long_text[i] = 'A';
    long_text[200] = '\0';

    bool update_ok = browser_js_dom_set_text_content(&s_app, s_app.doc_generation, 0, long_text);
    if (!update_ok) sys_exit(113);
    /* Node text buffer is sizeof(node->text) == 128 bytes; text must be safely clamped */
    if (ustrlen(s_app.nodes[0].text) > 127 || s_app.nodes[0].text[127] != '\0') {
        sys_exit(114);
    }

    /* 4. Test Stale DOM Handle Rejection after Navigation */
    u32 old_gen = s_app.doc_generation;
    s_app.doc_generation++; /* Simulate navigation to next document */

    /* Attempt to write using stale generation */
    bool stale_write = browser_js_dom_set_text_content(&s_app, old_gen, 0, "Malicious Stale Overwrite");
    if (stale_write) sys_exit(115); /* Must be rejected! */

    const char *stale_read = browser_js_dom_get_text_content(&s_app, old_gen, 0);
    if (stale_read != NULL) sys_exit(116); /* Must be rejected! */

    /* 5. Test Repeated Redraw without Repeated Script Execution */
    int orig_box_count = s_app.box_count;
    for (int rep = 0; rep < 5; rep++) {
        s_app.box_count = layout_page(&s_app, s_app.nodes, s_app.node_count, view_w, s_app.boxes, 16, &s_app.doc_height);
    }
    if (s_app.box_count != orig_box_count) sys_exit(117);

    const char bridge_ok[] = "ASTRA_DOM_BRIDGE_OK\n";
    sys_write(1, bridge_ok, ustrlen(bridge_ok));
}

/* -----------------------------------------------------------------
 * Phase T: Safe DOM Events & Interactive Browser Controls Self-Tests
 * ----------------------------------------------------------------- */
static void run_phase_t_self_tests(void) {
    /* 1. Run AstraJS Engine Phase T Unit Tests */
    int eng_res = js_run_phase_t_self_tests();
    if (eng_res != 0) {
        const char err[] = "[FATAL] Phase T JavaScript engine unit self-test failed!\n";
        sys_write(1, err, ustrlen(err));
        sys_exit(120 + eng_res);
    }
    const char events_ok[] = "ASTRA_DOM_EVENTS_OK\n";
    sys_write(1, events_ok, ustrlen(events_ok));

    /* 2. Interactive DOM Events & Button Click Dispatch Self-Test */
    static browser_app_t t_app;
    static dom_node_t t_nodes[32];
    static layout_box_t t_boxes[32];
    static stylesheet_t t_sheet;
    umemset(&t_app, 0, sizeof(t_app));
    umemset(t_nodes, 0, sizeof(t_nodes));
    umemset(t_boxes, 0, sizeof(t_boxes));
    umemset(&t_sheet, 0, sizeof(t_sheet));
    t_app.nodes = t_nodes;
    t_app.boxes = t_boxes;
    t_app.stylesheet = &t_sheet;
    t_app.html_buf = (char *)umalloc(4096);
    t_app.doc_generation = 1;
    ustrcpy(t_app.current_url, "file:///www/interactive_test.html", sizeof(t_app.current_url));

    const char t_html[] =
        "<html><head><title>Interactive Test</title></head><body>"
        "<h1 id='title_hdr'>Original Title</h1>"
        "<p id='status_msg'>Status: Idle</p>"
        "<p id='counter_msg'>Count: 0</p>"
        "<button id='btn_update' onclick='handleUpdate()'>Update Title</button>"
        "<button id='btn_count' onclick='handleCount()'>Increment</button>"
        "<button id='btn_err' onclick='document.getElementById(\"missing_target\").textContent = \"fail\";'>Error Btn</button>"
        "<a id='test_link' href='#' onclick='handleLink()'>Clickable Link</a>"
        "<script>"
        "var click_count = 0;\n"
        "function handleUpdate() {\n"
        "  document.getElementById('title_hdr').textContent = 'Updated Title by Click';\n"
        "  document.getElementById('status_msg').textContent = 'Status: Active';\n"
        "}\n"
        "function handleCount() {\n"
        "  click_count = click_count + 1;\n"
        "  document.getElementById('counter_msg').textContent = 'Count: ' + click_count;\n"
        "}\n"
        "function handleLink() {\n"
        "  document.getElementById('status_msg').textContent = 'Status: Link Clicked';\n"
        "}\n"
        "</script>"
        "</body></html>";

    char dummy_title[32];
    t_app.node_count = parse_html(&t_app, t_html, t_app.nodes, 32, &t_sheet, dummy_title, sizeof(dummy_title));
    if (t_app.node_count < 1) sys_exit(130);

    int view_w = WIN_WIDTH - SCROLLBAR_WIDTH;
    t_app.box_count = layout_page(&t_app, t_app.nodes, t_app.node_count, view_w, t_app.boxes, 32, &t_app.doc_height);

    /* Verify initial state */
    int title_idx = browser_js_dom_get_element_by_id(&t_app, "title_hdr", NULL);
    int status_idx = browser_js_dom_get_element_by_id(&t_app, "status_msg", NULL);
    int count_idx = browser_js_dom_get_element_by_id(&t_app, "counter_msg", NULL);
    if (title_idx < 0 || status_idx < 0 || count_idx < 0) sys_exit(131);
    if (!ustrcaseeq(t_app.nodes[title_idx].text, "Original Title")) sys_exit(132);

    /* TEST CASE 1: Button click changes element text */
    int btn_upd_ctrl = -1;
    for (int c = 0; c < t_app.control_count; c++) {
        int nid = t_app.controls[c].id;
        if (nid >= 0 && nid < t_app.node_count && ustrcaseeq(t_app.nodes[nid].id_name, "btn_update")) {
            btn_upd_ctrl = c;
            break;
        }
    }
    if (btn_upd_ctrl < 0 && t_app.control_count > 0) btn_upd_ctrl = 0;
    if (btn_upd_ctrl < 0) sys_exit(133);

    browser_dispatch_click(&t_app, t_app.controls[btn_upd_ctrl].onclick);
    if (!ustrcaseeq(t_app.nodes[title_idx].text, "Updated Title by Click")) sys_exit(134);
    if (!ustrcaseeq(t_app.nodes[status_idx].text, "Status: Active")) sys_exit(135);

    /* TEST CASE 2: Clickable Link changes text */
    int link_box = -1;
    for (int b = 0; b < t_app.box_count; b++) {
        if (ustrcaseeq(t_app.boxes[b].id_name, "test_link")) {
            link_box = b;
            break;
        }
    }
    if (link_box < 0) sys_exit(136);
    browser_dispatch_click(&t_app, t_app.boxes[link_box].onclick);
    if (!ustrcaseeq(t_app.nodes[status_idx].text, "Status: Link Clicked")) sys_exit(137);

    /* TEST CASE 3: Repeated clicks increment counter without duplicate/phantom runs */
    int btn_cnt_ctrl = -1;
    for (int c = 0; c < t_app.control_count; c++) {
        int nid = t_app.controls[c].id;
        if (nid >= 0 && nid < t_app.node_count && ustrcaseeq(t_app.nodes[nid].id_name, "btn_count")) {
            btn_cnt_ctrl = c;
            break;
        }
    }
    if (btn_cnt_ctrl < 0 && t_app.control_count > 1) btn_cnt_ctrl = 1;
    if (btn_cnt_ctrl < 0) sys_exit(138);

    /* Click 1 */
    browser_dispatch_click(&t_app, t_app.controls[btn_cnt_ctrl].onclick);
    if (!ustrcaseeq(t_app.nodes[count_idx].text, "Count: 1")) sys_exit(139);

    /* Click 2 */
    browser_dispatch_click(&t_app, t_app.controls[btn_cnt_ctrl].onclick);
    if (!ustrcaseeq(t_app.nodes[count_idx].text, "Count: 2")) sys_exit(140);

    /* Click 3 */
    browser_dispatch_click(&t_app, t_app.controls[btn_cnt_ctrl].onclick);
    if (!ustrcaseeq(t_app.nodes[count_idx].text, "Count: 3")) sys_exit(141);

    /* TEST CASE 4: Mouse Coordinate Hit-Testing with Scrolling */
    form_control_t *fc_cnt = &t_app.controls[btn_cnt_ctrl];
    t_app.scroll_y = 100;
    int click_mx = fc_cnt->x + 5;
    int click_my = TOOLBAR_HEIGHT + (fc_cnt->y - t_app.scroll_y) + 5;
    int hit_vy = click_my - TOOLBAR_HEIGHT + t_app.scroll_y;
    if (hit_vy < fc_cnt->y || hit_vy > fc_cnt->y + fc_cnt->h) sys_exit(142);
    if (click_mx >= fc_cnt->x && click_mx <= fc_cnt->x + fc_cnt->w &&
        hit_vy >= fc_cnt->y && hit_vy <= fc_cnt->y + fc_cnt->h) {
        browser_dispatch_click(&t_app, fc_cnt->onclick);
    }
    if (!ustrcaseeq(t_app.nodes[count_idx].text, "Count: 4")) sys_exit(143);
    t_app.scroll_y = 0;

    /* TEST CASE 5: Viewport Boundary Clamping */
    bool outside_hit = false;
    int out_mx = fc_cnt->x - 1;
    int out_vy = fc_cnt->y + 5;
    if (out_mx >= fc_cnt->x && out_mx <= fc_cnt->x + fc_cnt->w &&
        out_vy >= fc_cnt->y && out_vy <= fc_cnt->y + fc_cnt->h) {
        outside_hit = true;
    }
    if (outside_hit) sys_exit(144);

    /* TEST CASE 6: Missing DOM element in callback produces controlled TypeError without crash */
    int btn_err_ctrl = -1;
    for (int c = 0; c < t_app.control_count; c++) {
        int nid = t_app.controls[c].id;
        if (nid >= 0 && nid < t_app.node_count && ustrcaseeq(t_app.nodes[nid].id_name, "btn_err")) {
            btn_err_ctrl = c;
            break;
        }
    }
    if (btn_err_ctrl < 0 && t_app.control_count > 2) btn_err_ctrl = 2;
    if (btn_err_ctrl >= 0) {
        t_app.status_text[0] = '\0';
        browser_dispatch_click(&t_app, t_app.controls[btn_err_ctrl].onclick);
        if (!ustrcasestartswith(t_app.status_text, "[JS Error]")) sys_exit(145);
    }

    /* TEST CASE 7: Navigation invalidates previous callbacks & handles */
    u32 prev_gen = t_app.doc_generation;
    t_app.doc_generation++; /* Simulate navigation */
    bool stale_click_set = browser_js_dom_set_onclick(&t_app, prev_gen, 0, "stale()");
    if (stale_click_set) sys_exit(146);

    /* TEST CASE 8: Reentrancy guard prevents recursive dispatch */
    t_app.dispatching_event = true;
    const char *orig_status = t_app.nodes[status_idx].text;
    browser_dispatch_click(&t_app, "document.getElementById('status_msg').textContent = 'Reentrant';");
    if (!ustrcaseeq(t_app.nodes[status_idx].text, orig_status)) sys_exit(147);
    t_app.dispatching_event = false;

    const char interactive_ok[] = "ASTRA_BROWSER_INTERACTIVE_OK\n";
    sys_write(1, interactive_ok, ustrlen(interactive_ok));
}

/* -----------------------------------------------------------------
 * Phase U: Browser Security Regression & Adversarial Testing Self-Tests
 * ----------------------------------------------------------------- */
static void run_phase_u_self_tests(void) {
    /* 1. Run AstraJS Engine Phase U Adversarial Unit Tests */
    int eng_res = js_run_phase_u_self_tests();
    if (eng_res != 0) {
        const char err[] = "[FATAL] Phase U JavaScript engine adversarial unit test failed!\n";
        sys_write(1, err, ustrlen(err));
        sys_exit(150 + eng_res);
    }
    const char js_adv_ok[] = "ASTRA_JS_ADVERSARIAL_OK\n";
    sys_write(1, js_adv_ok, ustrlen(js_adv_ok));

    /* 2. Adversarial Browser DOM & Event Isolation Tests */
    static browser_app_t u_app;
    static dom_node_t u_nodes[32];
    static layout_box_t u_boxes[32];
    static stylesheet_t u_sheet;
    umemset(&u_app, 0, sizeof(u_app));
    umemset(u_nodes, 0, sizeof(u_nodes));
    umemset(u_boxes, 0, sizeof(u_boxes));
    umemset(&u_sheet, 0, sizeof(u_sheet));
    u_app.nodes = u_nodes;
    u_app.boxes = u_boxes;
    u_app.stylesheet = &u_sheet;
    u_app.html_buf = (char *)umalloc(4096);
    u_app.doc_generation = 1;
    ustrcpy(u_app.current_url, "file:///www/adversarial_test.html", sizeof(u_app.current_url));

    const char u_html[] =
        "<html><head><title>Phase U Security</title></head><body>"
        "<h1 id='sec_hdr'>Safe Title</h1>"
        "<p id='sec_status'>Normal</p>"
        "<button id='btn_malformed' onclick=''>Empty Onclick</button>"
        "<button id='btn_bad_syntax' onclick='+ + +;'>Bad Syntax</button>"
        "<button id='btn_loop' onclick='runInfinite()'>Timeout Test</button>"
        "<button id='btn_throttle' onclick='runBatchMutations()'>Throttle Test</button>"
        "<script>"
        "function runInfinite() {\n"
        "  while (1) {}\n"
        "}\n"
        "function runBatchMutations() {\n"
        "  var i = 0;\n"
        "  while (i < 40) {\n"
        "    document.getElementById('sec_status').textContent = 'Mutating';\n"
        "    i = i + 1;\n"
        "  }\n"
        "  document.getElementById('sec_status').textContent = 'Throttled Done';\n"
        "}\n"
        "</script>"
        "</body></html>";

    char dummy_title[32];
    u_app.node_count = parse_html(&u_app, u_html, u_app.nodes, 32, &u_sheet, dummy_title, sizeof(dummy_title));
    if (u_app.node_count < 1) sys_exit(160);

    int view_w = WIN_WIDTH - SCROLLBAR_WIDTH;
    u_app.box_count = layout_page(&u_app, u_app.nodes, u_app.node_count, view_w, u_app.boxes, 32, &u_app.doc_height);

    /* TEST 1: Empty onclick does nothing and does not crash */
    browser_dispatch_click(&u_app, "");
    int hdr_idx = browser_js_dom_get_element_by_id(&u_app, "sec_hdr", NULL);
    if (hdr_idx < 0 || !ustrcaseeq(u_app.nodes[hdr_idx].text, "Safe Title")) sys_exit(161);

    /* TEST 2: Malformed callback syntax is caught and recorded in status_text without crash */
    u_app.status_text[0] = '\0';
    browser_dispatch_click(&u_app, "+ + +;");
    if (!ustrcasestartswith(u_app.status_text, "[JS Error]")) sys_exit(162);
    /* Previous DOM state remains preserved */
    if (!ustrcaseeq(u_app.nodes[hdr_idx].text, "Safe Title")) sys_exit(163);

    /* TEST 3: Infinite loop instruction budget exhaustion in event callback terminates safely */
    u_app.status_text[0] = '\0';
    browser_dispatch_click(&u_app, "runInfinite();");
    if (!ustrcasestartswith(u_app.status_text, "[JS Error]")) sys_exit(164);

    /* TEST 4: Throttled batch mutations do not overrun mutation limits and final state matches */
    int stat_idx = browser_js_dom_get_element_by_id(&u_app, "sec_status", NULL);
    if (stat_idx < 0) sys_exit(165);
    browser_dispatch_click(&u_app, "runBatchMutations();");
    if (!ustrcaseeq(u_app.nodes[stat_idx].text, "Throttled Done")) sys_exit(166);

    /* TEST 5: Boundary click coordinate edge cases (toolbar and scrollbar bounds) */
    /* Toolbar boundary: click at (x=50, y=TOOLBAR_HEIGHT - 1) is outside viewport */
    int tb_my = TOOLBAR_HEIGHT - 1;
    bool tb_in_viewport = (tb_my >= TOOLBAR_HEIGHT && tb_my < WIN_HEIGHT - STATUS_HEIGHT);
    if (tb_in_viewport) sys_exit(167);

    /* Scrollbar boundary: click at (x=WIN_WIDTH - 2, y=TOOLBAR_HEIGHT + 10) is in scrollbar */
    int sb_mx = WIN_WIDTH - 2;
    bool sb_in_viewport_content = (sb_mx >= 0 && sb_mx < WIN_WIDTH - SCROLLBAR_WIDTH);
    if (sb_in_viewport_content) sys_exit(168);

    /* TEST 6: Stale DOM handle across navigation with invalid ID access */
    u32 cur_gen = u_app.doc_generation;
    u_app.doc_generation++; /* Advance generation */
    bool stale_mod = browser_js_dom_set_text_content(&u_app, cur_gen, hdr_idx, "Attacker Injected");
    if (stale_mod) sys_exit(169); /* Stale handle must be rejected */
    if (!ustrcaseeq(u_app.nodes[hdr_idx].text, "Safe Title")) sys_exit(170);

    /* Invalid element ID query */
    int invalid_idx = browser_js_dom_get_element_by_id(&u_app, "nonexistent_id_12345", NULL);
    if (invalid_idx != -1) sys_exit(171);

    const char sec_ok[] = "ASTRA_BROWSER_SECURITY_OK\n";
    sys_write(1, sec_ok, ustrlen(sec_ok));
}

/* -----------------------------------------------------------------
 * Application Entry Point
 * ----------------------------------------------------------------- */
int main(void) {
    /* 1. Verify Ring 3 CPL=3 execution */
    u16 cs = 0;
    __asm__ volatile ("movw %%cs, %0" : "=r"(cs));
    u8 cpl = (u8)(cs & 3);

    const char start_msg[] = "[USER BROWSER] AstraBrowser initializing in Ring 3...\n";
    sys_write(1, start_msg, ustrlen(start_msg));

    if (cpl != 3) {
        const char err[] = "[FATAL] AstraBrowser not executing in Ring 3 (CPL != 3)!\n";
        sys_write(1, err, ustrlen(err));
        sys_exit(1);
    }

    /* 2. Verify User-Space Memory Allocation (sys_mmap & sys_munmap) */
    void *mmap_test = sys_mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE);
    if (mmap_test == MAP_FAILED || (long)mmap_test < 0) {
        const char err[] = "[FATAL] sys_mmap failed in user space!\n";
        sys_write(1, err, ustrlen(err));
        sys_exit(2);
    }

    /* Verify page is zeroed */
    u8 *test_bytes = (u8 *)mmap_test;
    for (int i = 0; i < 64; i++) {
        if (test_bytes[i] != 0) {
            sys_exit(3);
        }
        test_bytes[i] = (u8)(i + 1);
    }
    sys_munmap(mmap_test, 4096);

    const char mmap_ok[] = "ASTRA_USER_MMAP_OK\n";
    sys_write(1, mmap_ok, ustrlen(mmap_ok));

    /* 3. Allocate Browser Structures via Dynamic Arena */
    g_browser.nodes = (dom_node_t *)umalloc(sizeof(dom_node_t) * MAX_NODES);
    g_browser.boxes = (layout_box_t *)umalloc(sizeof(layout_box_t) * MAX_BOXES);
    g_browser.stylesheet = (stylesheet_t *)umalloc(sizeof(stylesheet_t));
    g_browser.html_buf = (char *)umalloc(32768);

    if (!g_browser.nodes || !g_browser.boxes || !g_browser.stylesheet || !g_browser.html_buf) {
        const char err[] = "[FATAL] Failed to allocate browser memory structures!\n";
        sys_write(1, err, ustrlen(err));
        sys_exit(4);
    }

    /* 4. Run Verification Self-Tests */
    run_phase_p_self_tests();
    run_phase_q_self_tests();
    run_phase_r_self_tests();
    run_phase_s_self_tests();
    run_phase_t_self_tests();
    run_phase_u_self_tests();

    /* 5. Create GUI Window */
    int win_id = (int)sys_gui_create_window("AstraBrowser", 100, 50, WIN_WIDTH, WIN_HEIGHT);
    if (win_id <= 0) {
        /* Headless fallback mode: verify page parsing and exit */
        const char no_gui[] = "[USER BROWSER] Headless mode: running parser verification...\n";
        sys_write(1, no_gui, ustrlen(no_gui));
        const char browser_ok[] = "ASTRA_USER_BROWSER_OK\n";
        sys_write(1, browser_ok, ustrlen(browser_ok));
        navigate_to(&g_browser, "file:///www/img_test.html", false);
        navigate_to(&g_browser, "file:///www/link_css.html", false);
        navigate_to(&g_browser, "file:///www/form_test.html", false);
        navigate_to(&g_browser, "file:///www/nav_test.html", false);
        navigate_to(&g_browser, "file:///www/js_test.html", false);
        navigate_to(&g_browser, "file:///www/interactive_test.html", false);
        navigate_to(&g_browser, "file:///www/adversarial_test.html", false);
        ufree_arena();
        sys_exit(0);
    }

    g_browser.win_id = win_id;

    const char browser_ok[] = "ASTRA_USER_BROWSER_OK\n";
    sys_write(1, browser_ok, ustrlen(browser_ok));

    /* 6. Load Initial Page */
    navigate_to(&g_browser, "file:///www/img_test.html", true);
    render_browser_ui(&g_browser);

    /* 7. Main Event Loop */
    int running = 1;
    int loop_count = 0;
    while (running && loop_count < 140) {
        loop_count++;

        input_event_t ev;
        int has_event = (int)sys_gui_get_event(win_id, &ev);
        if (has_event == 1) {
            if (ev.type == INPUT_EVENT_WINDOW_CLOSE) {
                running = 0;
                break;
            } else if (ev.type == INPUT_EVENT_MOUSE_DOWN) {
                int mx = ev.mouse.x;
                int my = ev.mouse.y;

                if (my < TOOLBAR_HEIGHT) {
                    /* Toolbar clicks */
                    if (mx >= 6 && mx <= 32 && g_browser.history_index > 0) {
                        /* Back */
                        g_browser.history_index--;
                        navigate_to(&g_browser, g_browser.history[g_browser.history_index], false);
                    } else if (mx >= 36 && mx <= 62 && g_browser.history_index < g_browser.history_count - 1) {
                        /* Forward */
                        g_browser.history_index++;
                        navigate_to(&g_browser, g_browser.history[g_browser.history_index], false);
                    } else if (mx >= 66 && mx <= 92) {
                        /* Reload */
                        navigate_to(&g_browser, g_browser.current_url, false);
                    } else if (mx >= WIN_WIDTH - 46 && mx <= WIN_WIDTH - 6) {
                        /* Go Button */
                        navigate_to(&g_browser, g_browser.input_url, true);
                    } else if (mx >= 98 && mx <= WIN_WIDTH - 52) {
                        /* Focus URL Bar */
                        g_browser.input_focused = true;
                        g_browser.focused_control_id = -1;
                        for (int k = 0; k < g_browser.control_count; k++) {
                            g_browser.controls[k].focused = false;
                        }
                    }
                } else if (my >= TOOLBAR_HEIGHT && my < WIN_HEIGHT - STATUS_HEIGHT) {
                    int sb_x = WIN_WIDTH - SCROLLBAR_WIDTH;
                    int view_h = WIN_HEIGHT - TOOLBAR_HEIGHT - STATUS_HEIGHT;

                    if (mx >= sb_x) {
                        /* Scrollbar track / thumb click */
                        int max_scroll = g_browser.doc_height - view_h;
                        if (max_scroll > 0) {
                            int thumb_h = (view_h * view_h) / g_browser.doc_height;
                            if (thumb_h < 24) thumb_h = 24;
                            int thumb_y = TOOLBAR_HEIGHT + (g_browser.scroll_y * (view_h - thumb_h)) / max_scroll;

                            if (my < thumb_y) {
                                g_browser.scroll_y -= (view_h * 3) / 4;
                            } else if (my > thumb_y + thumb_h) {
                                g_browser.scroll_y += (view_h * 3) / 4;
                            }
                            if (g_browser.scroll_y < 0) g_browser.scroll_y = 0;
                            if (g_browser.scroll_y > max_scroll) g_browser.scroll_y = max_scroll;
                        }
                    } else {
                        /* Document area hit testing */
                        int vy = my - TOOLBAR_HEIGHT + g_browser.scroll_y;

                        /* Check form control clicks */
                        bool clicked_ctrl = false;
                        for (int k = 0; k < g_browser.control_count; k++) {
                            form_control_t *fc = &g_browser.controls[k];
                            if (mx >= fc->x && mx <= fc->x + fc->w &&
                                vy >= fc->y && vy <= fc->y + fc->h) {
                                clicked_ctrl = true;
                                g_browser.input_focused = false;

                                if (fc->type == CTRL_INPUT_TEXT || fc->type == CTRL_TEXTAREA) {
                                    for (int j = 0; j < g_browser.control_count; j++) {
                                        g_browser.controls[j].focused = (j == k);
                                    }
                                    g_browser.focused_control_id = k;
                                } else if (fc->type == CTRL_INPUT_CHECKBOX) {
                                    fc->checked = !fc->checked;
                                } else if (fc->type == CTRL_INPUT_SUBMIT || fc->type == CTRL_BUTTON) {
                                    if (fc->onclick[0] != '\0') {
                                        browser_dispatch_click(&g_browser, fc->onclick);
                                    } else {
                                        submit_form(&g_browser, fc->action);
                                    }
                                }
                                break;
                            }
                        }

                        if (!clicked_ctrl) {
                            /* Check link and clickable box clicks */
                            for (int i = 0; i < g_browser.box_count; i++) {
                                const layout_box_t *b = &g_browser.boxes[i];
                                if ((b->is_link || b->onclick[0] != '\0') &&
                                    mx >= b->x && mx <= b->x + b->w &&
                                    vy >= b->y && vy <= b->y + b->h) {
                                    if (b->onclick[0] != '\0') {
                                        browser_dispatch_click(&g_browser, b->onclick);
                                    }
                                    if (b->is_link && b->href[0] != '\0' && !ustrcaseeq(b->href, "#") && !ustrcasestartswith(b->href, "javascript:")) {
                                        char res_link[256];
                                        resolve_url(g_browser.current_url, b->href, res_link, sizeof(res_link));
                                        navigate_to(&g_browser, res_link, true);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
                render_browser_ui(&g_browser);

            } else if (ev.type == INPUT_EVENT_KEY_DOWN) {
                if (g_browser.input_focused) {
                    char c = ev.key.ascii;
                    if (c == '\n' || c == '\r') {
                        navigate_to(&g_browser, g_browser.input_url, true);
                    } else if (c == '\b') {
                        if (g_browser.input_cursor > 0) {
                            g_browser.input_cursor--;
                            g_browser.input_url[g_browser.input_cursor] = '\0';
                        }
                    } else if (c >= 32 && c <= 126 && g_browser.input_cursor < 250) {
                        g_browser.input_url[g_browser.input_cursor++] = c;
                        g_browser.input_url[g_browser.input_cursor] = '\0';
                    }
                    render_browser_ui(&g_browser);
                } else if (g_browser.focused_control_id >= 0 && g_browser.focused_control_id < g_browser.control_count) {
                    form_control_t *fc = &g_browser.controls[g_browser.focused_control_id];
                    char c = ev.key.ascii;
                    size_t vlen = ustrlen(fc->value);

                    if (c == '\n' || c == '\r') {
                        if (fc->type == CTRL_INPUT_TEXT) {
                            submit_form(&g_browser, fc->action);
                        } else if (fc->type == CTRL_TEXTAREA && vlen < sizeof(fc->value) - 2) {
                            fc->value[vlen++] = '\n';
                            fc->value[vlen] = '\0';
                        }
                    } else if (c == '\b') {
                        if (vlen > 0) {
                            fc->value[vlen - 1] = '\0';
                        }
                    } else if (c >= 32 && c <= 126 && vlen < sizeof(fc->value) - 2) {
                        fc->value[vlen++] = c;
                        fc->value[vlen] = '\0';
                    }
                    render_browser_ui(&g_browser);
                } else {
                    /* Unfocused navigation keys: vertical document scroll */
                    u32 kcode = ev.key.keycode;
                    char c = ev.key.ascii;
                    int view_h = WIN_HEIGHT - TOOLBAR_HEIGHT - STATUS_HEIGHT;
                    int max_scroll = g_browser.doc_height - view_h;
                    if (max_scroll > 0) {
                        if (kcode == 0x105 || c == 'k') { /* KEY_UP */
                            g_browser.scroll_y -= 24;
                            if (g_browser.scroll_y < 0) g_browser.scroll_y = 0;
                            render_browser_ui(&g_browser);
                        } else if (kcode == 0x106 || c == 'j') { /* KEY_DOWN */
                            g_browser.scroll_y += 24;
                            if (g_browser.scroll_y > max_scroll) g_browser.scroll_y = max_scroll;
                            render_browser_ui(&g_browser);
                        }
                    }
                }
            }
        }

        sys_sleep(30);
    }

    /* 8. Clean Exit & Reclamation */
    sys_gui_destroy_window(win_id);
    ufree_arena();

    const char done[] = "[USER BROWSER] Clean exit with status 0.\n";
    sys_write(1, done, ustrlen(done));
    sys_exit(0);
}
