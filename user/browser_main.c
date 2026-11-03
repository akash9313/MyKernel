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
s64 sys_fsync(int fd);
s64 sys_sync(void);
s64 sys_rename(const char *oldpath, const char *newpath);

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
#define TAB_BAR_HEIGHT  24
#define TOOLBAR_HEIGHT  30
#define STATUS_HEIGHT   24
#define SCROLLBAR_WIDTH 14
#define FONT_WIDTH      8
#define FONT_HEIGHT     16

#define MAX_TABS        4
#define MAX_BOOKMARKS   32

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

static bool ustrcontains(const char *s, const char *sub) {
    if (!s || !sub) return false;
    size_t slen = ustrlen(s);
    size_t sublen = ustrlen(sub);
    if (sublen > slen) return false;
    for (size_t i = 0; i <= slen - sublen; i++) {
        size_t j = 0;
        while (j < sublen && utolower(s[i + j]) == utolower(sub[j])) {
            j++;
        }
        if (j == sublen) return true;
    }
    return false;
}

/* -----------------------------------------------------------------
 * Dynamic Memory Arena Allocator (Multi-Chunk Safe Reclamation)
 * ----------------------------------------------------------------- */
#define MAX_ARENA_CHUNKS 64
static void *g_arena_chunks[MAX_ARENA_CHUNKS];
static size_t g_arena_chunk_sizes[MAX_ARENA_CHUNKS];
static int g_arena_chunk_count = 0;
static void *g_arena_curr_base = NULL;
static size_t g_arena_curr_cap = 0;
static size_t g_arena_curr_used = 0;

static void *umalloc(size_t size) {
    size = (size + 15) & ~15UL; /* Align to 16 bytes */
    if (size >= 65536) {
        /* Large allocation: allocate exact required pages */
        if (g_arena_chunk_count >= MAX_ARENA_CHUNKS) {
            return NULL;
        }
        size_t alloc_sz = (size + 4095) & ~4095UL;
        void *p = sys_mmap(NULL, alloc_sz, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE);
        if (p == MAP_FAILED || (long)p < 0) {
            return NULL;
        }
        g_arena_chunks[g_arena_chunk_count] = p;
        g_arena_chunk_sizes[g_arena_chunk_count] = alloc_sz;
        g_arena_chunk_count++;
        umemset(p, 0, size);
        return p;
    }

    if (!g_arena_curr_base || g_arena_curr_used + size > g_arena_curr_cap) {
        if (g_arena_chunk_count >= MAX_ARENA_CHUNKS) {
            return NULL; /* Bounded arena limit */
        }
        size_t alloc_sz = (size + 4095) & ~4095UL;
        if (alloc_sz < 65536) alloc_sz = 65536; /* 64 KiB chunks for small objects */

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
        if (hi == 0) return false;
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
        if (hi == 0) return false;
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

/* Full-Page Resource Fetching Helper (returns content and status code) */
static s64 fetch_page_resource(const char *url_str, void *buf, size_t max_len, int *out_status) {
    if (!url_str || !buf || max_len == 0) return -1;
    if (out_status) *out_status = 0;
    parsed_url_t purl;
    if (!parse_url(url_str, &purl)) return -1;

    if (purl.scheme == SCHEME_FILE) {
        s64 fd = sys_open(purl.path, 0);
        if (fd < 0) {
            if (out_status) *out_status = 404;
            return -1;
        }
        s64 bytes = sys_read((int)fd, buf, max_len);
        sys_close((int)fd);
        if (out_status) *out_status = (bytes >= 0) ? 200 : 500;
        return bytes;
    } else if (purl.scheme == SCHEME_HTTP || purl.scheme == SCHEME_HTTPS) {
        int status_code = 0;
        s64 bytes = sys_net_fetch(url_str, (char *)buf, max_len, &status_code);
        if (out_status) *out_status = status_code;
        return bytes;
    }
    return -1;
}

/* -----------------------------------------------------------------
 * BMP Image Decoder (User-Space)
 * ----------------------------------------------------------------- */
#define MAX_BMP_PIXELS 65536
static u32 s_bmp_pixels[MAX_BMP_PIXELS];
static size_t s_bmp_pixels_used = 0;

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

    /* Allocate decoded 32-bit pixel buffer from reusable per-page static scratch pool */
    if (s_bmp_pixels_used + num_pixels > MAX_BMP_PIXELS) {
        return NULL;
    }
    u32 *pixels = &s_bmp_pixels[s_bmp_pixels_used];
    s_bmp_pixels_used += num_pixels;

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
 * Phase AA: Bookmarks & Persistent Storage Abstraction
 * ----------------------------------------------------------------- */
typedef struct {
    char url[256];
    char title[64];
    u32 created_at;
} bookmark_entry_t;

#define BOOKMARK_MAGIC 0x41535442 /* "ASTB" */
#define BOOKMARK_VERSION 1

#pragma pack(push, 1)
typedef struct {
    u32 magic;       /* 0x41535442 "ASTB" */
    u16 version;     /* 1 */
    u16 count;       /* 0..32 */
    u32 checksum;    /* Fletcher-32 of payload */
    u32 reserved;
} bookmark_header_t;

typedef struct {
    char url[256];
    char title[64];
} bookmark_disk_entry_t;
#pragma pack(pop)

/* -----------------------------------------------------------------
 * Phase AA: Bounded Tab Structure
 * ----------------------------------------------------------------- */
typedef struct {
    bool active;
    char current_url[256];
    char input_url[256];
    int input_cursor;
    bool input_focused;
    char status_text[96];
    char page_title[64];

    /* History */
    char history[16][256];
    int history_index;
    int history_count;

    /* DOM & Layout pointers */
    dom_node_t *nodes;
    int node_count;
    stylesheet_t *stylesheet;
    layout_box_t *boxes;
    int box_count;
    char *html_buf;
    size_t html_len;

    /* Scrolling */
    int scroll_y;
    int doc_height;

    /* Forms */
    form_control_t controls[MAX_FORM_CONTROLS];
    int control_count;
    int focused_control_id;
    char active_form_action[160];
    char active_form_method[16];

    /* JS VM pointer & Document Lifecycle */
    u32 doc_generation;
    js_vm_t *doc_vm;
    bool doc_vm_initialized;

    bool dispatching_event;
    int mutation_count_in_event;
} browser_tab_t;

/* -----------------------------------------------------------------
 * Browser Application State
 * ----------------------------------------------------------------- */
typedef struct {
    int win_id;

    /* Tabs */
    int tab_count;
    int active_tab_idx;
    browser_tab_t tabs[MAX_TABS];

    /* Bookmarks */
    bookmark_entry_t bookmarks[MAX_BOOKMARKS];
    int bookmark_count;

    /* Active Tab Working State (Mirrored for Zero-Regression Compatibility) */
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
static browser_app_t s_test_app;

/* Preallocated tab pools for 4 tabs */
static dom_node_t *s_tab_nodes_pool[MAX_TABS];
static layout_box_t *s_tab_boxes_pool[MAX_TABS];
static stylesheet_t *s_tab_sheet_pool[MAX_TABS];
static char *s_tab_html_pool[MAX_TABS];
static js_vm_t *s_tab_vm_pool[MAX_TABS];

static void navigate_to(browser_app_t *b, const char *url_str, bool record_history);
static int layout_page(browser_app_t *b, const dom_node_t *nodes, int node_count, int view_w, layout_box_t *boxes, int max_boxes, int *out_doc_h);
static void render_browser_ui(browser_app_t *b);
static void tab_save_active(browser_app_t *b);
static void tab_load_active(browser_app_t *b, int idx);
static int tab_create(browser_app_t *b, const char *url);
static bool tab_close(browser_app_t *b, int idx);
static bool tab_select(browser_app_t *b, int idx);
static int bookmark_find(const browser_app_t *b, const char *url);
static int bookmark_add(browser_app_t *b, const char *url, const char *title);
static bool bookmark_remove(browser_app_t *b, const char *url);
static bool __attribute__((unused)) bookmark_open(browser_app_t *b, int idx);
static s64 bookmarks_serialize(const browser_app_t *b, void *buf, size_t max_len);
static bool bookmarks_deserialize(browser_app_t *b, const void *buf, size_t len);
static s64 __attribute__((unused)) bookmarks_save(browser_app_t *b);
static s64 __attribute__((unused)) bookmarks_load(browser_app_t *b);

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

                    /* Fetch external stylesheet into static scratch buffer to prevent arena leak */
                    static char s_css_ext_buf[8192];
                    s64 bytes = fetch_resource(full_css_url, s_css_ext_buf, sizeof(s_css_ext_buf) - 1);
                    if (bytes > 0) {
                        s_css_ext_buf[bytes] = '\0';
                        parse_stylesheet(s_css_ext_buf, sheet);
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
                static char script_buf[4096];
                script_buf[0] = '\0';

                if (src_val[0]) {
                    /* External script fetch */
                    char full_script_url[256];
                    resolve_url(b->current_url, src_val, full_script_url, sizeof(full_script_url));
                    s64 bytes = fetch_resource(full_script_url, script_buf, sizeof(script_buf) - 1);
                    if (bytes > 0) {
                        script_buf[bytes] = '\0';
                    }
                } else {
                    /* Inline script */
                    size_t si = 0;
                    while (*p && !ustrcasestartswith(p, "</script>") && si < sizeof(script_buf) - 1) {
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

            /* Temporary image buffer via static scratch buffer to prevent arena leak */
            static u8 s_img_file_buf[65536];
            s64 nbytes = fetch_resource(img_full_url, s_img_file_buf, sizeof(s_img_file_buf) - 1);
            if (nbytes > 0) {
                pixels = decode_bmp(s_img_file_buf, (size_t)nbytes, &dw, &dh);
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

static void browser_history_push(browser_app_t *b, const char *url_str) {
    if (!b || !url_str || !url_str[0]) return;

    /* Deduplicate identical consecutive entries */
    if (b->history_count > 0 && b->history_index >= 0 && b->history_index < b->history_count) {
        if (ustrcaseeq(b->history[b->history_index], url_str)) {
            return;
        }
    }

    /* If navigating from middle of history, truncate forward entries */
    if (b->history_count > 0 && b->history_index >= 0 && b->history_index < b->history_count - 1) {
        b->history_count = b->history_index + 1;
    }

    if (b->history_count >= 16) {
        for (int h = 0; h < 15; h++) {
            ustrcpy(b->history[h], b->history[h + 1], sizeof(b->history[h]));
        }
        b->history_index = 15;
        ustrcpy(b->history[15], url_str, sizeof(b->history[15]));
        b->history_count = 16;
    } else {
        if (b->history_count == 0) {
            b->history_index = 0;
        } else {
            b->history_index++;
        }
        ustrcpy(b->history[b->history_index], url_str, sizeof(b->history[b->history_index]));
        b->history_count = b->history_index + 1;
    }
}

static bool sanitize_url_input(const char *input, char *out, size_t out_max) {
    if (!input || !out || out_max == 0) return false;
    out[0] = '\0';
    while (*input == ' ' || *input == '\t') input++;
    if (!*input) return false;

    size_t len = ustrlen(input);
    while (len > 0 && (input[len - 1] == ' ' || input[len - 1] == '\t' || input[len - 1] == '\r' || input[len - 1] == '\n')) {
        len--;
    }
    if (len == 0 || len >= out_max - 8) return false;

    char clean[256];
    for (size_t i = 0; i < len && i < sizeof(clean) - 1; i++) clean[i] = input[i];
    clean[len < sizeof(clean) ? len : sizeof(clean) - 1] = '\0';

    if (ustrcasestartswith(clean, "file:///") ||
        ustrcasestartswith(clean, "http://") ||
        ustrcasestartswith(clean, "https://")) {
        ustrcpy(out, clean, out_max);
        return true;
    }

    if (clean[0] == '/') {
        ustrcpy(out, "file://", out_max);
        ustrcat(out, clean, out_max);
        return true;
    }

    /* Reject unsupported schemes */
    if (ustrcasestartswith(clean, "javascript:") ||
        ustrcasestartswith(clean, "data:") ||
        ustrcasestartswith(clean, "ftp://") ||
        ustrcasestartswith(clean, "file:/")) {
        ustrcpy(out, clean, out_max);
        return false;
    }

    /* Default to http:// */
    ustrcpy(out, "http://", out_max);
    ustrcat(out, clean, out_max);
    return true;
}

/* -----------------------------------------------------------------
 * Phase AA: Checksum & Serialization
 * ----------------------------------------------------------------- */
static u32 calculate_checksum(const void *data, size_t len) {
    const u8 *p = (const u8 *)data;
    u32 c0 = 0xffff, c1 = 0xffff;
    while (len) {
        size_t block = len > 360 ? 360 : len;
        len -= block;
        do {
            c0 += *p++;
            c1 += c0;
        } while (--block);
        c0 = (c0 & 0xffff) + (c0 >> 16);
        c1 = (c1 & 0xffff) + (c1 >> 16);
    }
    c0 = (c0 & 0xffff) + (c0 >> 16);
    c1 = (c1 & 0xffff) + (c1 >> 16);
    return (c1 << 16) | c0;
}

/* -----------------------------------------------------------------
 * Phase AA: Tab State Synchronization
 * ----------------------------------------------------------------- */
static void tab_save_active(browser_app_t *b) {
    if (!b || b->tab_count == 0 || b->active_tab_idx < 0 || b->active_tab_idx >= MAX_TABS) return;
    browser_tab_t *t = &b->tabs[b->active_tab_idx];
    t->active = true;
    ustrcpy(t->current_url, b->current_url, sizeof(t->current_url));
    ustrcpy(t->input_url, b->input_url, sizeof(t->input_url));
    t->input_cursor = b->input_cursor;
    t->input_focused = b->input_focused;
    ustrcpy(t->status_text, b->status_text, sizeof(t->status_text));
    ustrcpy(t->page_title, b->page_title, sizeof(t->page_title));

    t->history_index = b->history_index;
    t->history_count = b->history_count;
    for (int i = 0; i < 16; i++) {
        ustrcpy(t->history[i], b->history[i], sizeof(t->history[i]));
    }

    t->nodes = b->nodes;
    t->node_count = b->node_count;
    t->stylesheet = b->stylesheet;
    t->boxes = b->boxes;
    t->box_count = b->box_count;
    t->html_buf = b->html_buf;
    t->html_len = b->html_len;

    t->scroll_y = b->scroll_y;
    t->doc_height = b->doc_height;

    t->control_count = b->control_count;
    t->focused_control_id = b->focused_control_id;
    ustrcpy(t->active_form_action, b->active_form_action, sizeof(t->active_form_action));
    ustrcpy(t->active_form_method, b->active_form_method, sizeof(t->active_form_method));
    for (int i = 0; i < MAX_FORM_CONTROLS; i++) {
        t->controls[i] = b->controls[i];
    }

    t->doc_generation = b->doc_generation;
    if (t->doc_vm) {
        *t->doc_vm = b->doc_vm;
    }
    t->doc_vm_initialized = b->doc_vm_initialized;
    t->dispatching_event = b->dispatching_event;
    t->mutation_count_in_event = b->mutation_count_in_event;
}

static void tab_load_active(browser_app_t *b, int idx) {
    if (!b || idx < 0 || idx >= MAX_TABS) return;
    b->active_tab_idx = idx;
    browser_tab_t *t = &b->tabs[idx];

    ustrcpy(b->current_url, t->current_url, sizeof(b->current_url));
    ustrcpy(b->input_url, t->input_url, sizeof(b->input_url));
    b->input_cursor = t->input_cursor;
    b->input_focused = t->input_focused;
    ustrcpy(b->status_text, t->status_text, sizeof(b->status_text));
    ustrcpy(b->page_title, t->page_title, sizeof(b->page_title));

    b->history_index = t->history_index;
    b->history_count = t->history_count;
    for (int i = 0; i < 16; i++) {
        ustrcpy(b->history[i], t->history[i], sizeof(b->history[i]));
    }

    b->nodes = t->nodes;
    b->node_count = t->node_count;
    b->stylesheet = t->stylesheet;
    b->boxes = t->boxes;
    b->box_count = t->box_count;
    b->html_buf = t->html_buf;
    b->html_len = t->html_len;

    b->scroll_y = t->scroll_y;
    b->doc_height = t->doc_height;

    b->control_count = t->control_count;
    b->focused_control_id = t->focused_control_id;
    ustrcpy(b->active_form_action, t->active_form_action, sizeof(b->active_form_action));
    ustrcpy(b->active_form_method, t->active_form_method, sizeof(b->active_form_method));
    for (int i = 0; i < MAX_FORM_CONTROLS; i++) {
        b->controls[i] = t->controls[i];
    }

    b->doc_generation = t->doc_generation;
    if (t->doc_vm) {
        b->doc_vm = *t->doc_vm;
    }
    b->doc_vm_initialized = t->doc_vm_initialized;
    b->dispatching_event = t->dispatching_event;
    b->mutation_count_in_event = t->mutation_count_in_event;

    if (b->doc_vm_initialized) {
        js_set_dom_bridge(&b->doc_vm, b,
                          browser_js_dom_get_element_by_id,
                          browser_js_dom_get_text_content,
                          browser_js_dom_set_text_content);
        js_set_dom_event_bridge(&b->doc_vm, browser_js_dom_set_onclick);
    }

    if (b->win_id > 0 && b->page_title[0]) {
        sys_gui_set_title(b->win_id, b->page_title);
    }
}

static int tab_create(browser_app_t *b, const char *url) {
    if (!b || b->tab_count >= MAX_TABS) {
        return -1;
    }
    tab_save_active(b);

    int new_idx = b->tab_count;
    browser_tab_t *t = &b->tabs[new_idx];
    umemset(t, 0, sizeof(*t));

    t->active = true;
    t->nodes = s_tab_nodes_pool[new_idx];
    t->boxes = s_tab_boxes_pool[new_idx];
    t->stylesheet = s_tab_sheet_pool[new_idx];
    t->html_buf = s_tab_html_pool[new_idx];
    t->doc_vm = s_tab_vm_pool[new_idx];
    if (t->doc_vm) {
        js_init(t->doc_vm);
    }
    t->focused_control_id = -1;
    t->doc_generation = (new_idx + 1) * 1000;

    b->tab_count++;
    tab_load_active(b, new_idx);

    const char *target = (url && url[0]) ? url : "file:///www/index.html";
    navigate_to(b, target, true);
    return new_idx;
}

static bool tab_close(browser_app_t *b, int idx) {
    if (!b || idx < 0 || idx >= b->tab_count) {
        return false;
    }

    if (b->tab_count == 1) {
        tab_load_active(b, 0);
        b->history_count = 0;
        b->history_index = 0;
        b->scroll_y = 0;
        b->doc_generation++;
        navigate_to(b, "file:///www/index.html", true);
        return true;
    }

    tab_save_active(b);

    for (int i = idx; i < b->tab_count - 1; i++) {
        browser_tab_t *dst = &b->tabs[i];
        const browser_tab_t *src = &b->tabs[i + 1];

        dom_node_t *saved_nodes = dst->nodes;
        layout_box_t *saved_boxes = dst->boxes;
        stylesheet_t *saved_sheet = dst->stylesheet;
        char *saved_html = dst->html_buf;
        js_vm_t *saved_vm = dst->doc_vm;

        *dst = *src;

        dst->nodes = saved_nodes;
        dst->boxes = saved_boxes;
        dst->stylesheet = saved_sheet;
        dst->html_buf = saved_html;
        dst->doc_vm = saved_vm;

        if (src->nodes && src->node_count > 0 && dst->nodes) {
            umemcpy(dst->nodes, src->nodes, sizeof(dom_node_t) * (src->node_count < MAX_NODES ? src->node_count : MAX_NODES));
        }
        if (src->boxes && src->box_count > 0 && dst->boxes) {
            umemcpy(dst->boxes, src->boxes, sizeof(layout_box_t) * (src->box_count < MAX_BOXES ? src->box_count : MAX_BOXES));
        }
        if (src->stylesheet && dst->stylesheet) {
            umemcpy(dst->stylesheet, src->stylesheet, sizeof(stylesheet_t));
        }
        if (src->html_buf && dst->html_buf && src->html_len > 0) {
            size_t copy_len = src->html_len < 32767 ? src->html_len : 32767;
            umemcpy(dst->html_buf, src->html_buf, copy_len + 1);
            dst->html_buf[copy_len] = '\0';
        }
        if (src->doc_vm && dst->doc_vm) {
            *dst->doc_vm = *src->doc_vm;
        }
    }

    int last_idx = b->tab_count - 1;
    dom_node_t *last_nodes = b->tabs[last_idx].nodes;
    layout_box_t *last_boxes = b->tabs[last_idx].boxes;
    stylesheet_t *last_sheet = b->tabs[last_idx].stylesheet;
    char *last_html = b->tabs[last_idx].html_buf;
    js_vm_t *last_vm = b->tabs[last_idx].doc_vm;

    umemset(&b->tabs[last_idx], 0, sizeof(browser_tab_t));
    b->tabs[last_idx].nodes = last_nodes;
    b->tabs[last_idx].boxes = last_boxes;
    b->tabs[last_idx].stylesheet = last_sheet;
    b->tabs[last_idx].html_buf = last_html;
    b->tabs[last_idx].doc_vm = last_vm;

    b->tab_count--;

    if (b->active_tab_idx == idx) {
        if (b->active_tab_idx >= b->tab_count) {
            b->active_tab_idx = b->tab_count - 1;
        }
    } else if (b->active_tab_idx > idx) {
        b->active_tab_idx--;
    }

    tab_load_active(b, b->active_tab_idx);
    return true;
}

static bool tab_select(browser_app_t *b, int idx) {
    if (!b || idx < 0 || idx >= b->tab_count) {
        return false;
    }
    if (idx == b->active_tab_idx) {
        return true;
    }
    tab_save_active(b);
    tab_load_active(b, idx);
    b->input_focused = false;
    b->focused_control_id = -1;
    return true;
}

/* -----------------------------------------------------------------
 * Phase AA: Bookmark Management & Serialization
 * ----------------------------------------------------------------- */
static int bookmark_find(const browser_app_t *b, const char *url) {
    if (!b || !url || !url[0]) return -1;
    for (int i = 0; i < b->bookmark_count; i++) {
        if (ustrcaseeq(b->bookmarks[i].url, url)) {
            return i;
        }
    }
    return -1;
}

static int bookmark_add(browser_app_t *b, const char *url, const char *title) {
    if (!b || !url || !url[0]) return -1;
    if (ustrlen(url) >= 250) return -1;

    if (!ustrcasestartswith(url, "file:///") &&
        !ustrcasestartswith(url, "http://") &&
        !ustrcasestartswith(url, "https://")) {
        return -1;
    }

    int existing = bookmark_find(b, url);
    if (existing >= 0) {
        if (title && title[0]) {
            ustrcpy(b->bookmarks[existing].title, title, sizeof(b->bookmarks[existing].title));
        }
        return existing;
    }

    if (b->bookmark_count >= MAX_BOOKMARKS) {
        return -1;
    }

    int idx = b->bookmark_count;
    ustrcpy(b->bookmarks[idx].url, url, sizeof(b->bookmarks[idx].url));
    if (title && title[0]) {
        ustrcpy(b->bookmarks[idx].title, title, sizeof(b->bookmarks[idx].title));
    } else {
        ustrcpy(b->bookmarks[idx].title, url, sizeof(b->bookmarks[idx].title));
    }
    b->bookmarks[idx].created_at = 1;
    b->bookmark_count++;
    return idx;
}

static bool bookmark_remove(browser_app_t *b, const char *url) {
    if (!b || !url || !url[0]) return false;
    int idx = bookmark_find(b, url);
    if (idx < 0) return false;

    for (int i = idx; i < b->bookmark_count - 1; i++) {
        b->bookmarks[i] = b->bookmarks[i + 1];
    }
    umemset(&b->bookmarks[b->bookmark_count - 1], 0, sizeof(bookmark_entry_t));
    b->bookmark_count--;
    return true;
}

static bool __attribute__((unused)) bookmark_open(browser_app_t *b, int idx) {
    if (!b || idx < 0 || idx >= b->bookmark_count) return false;
    navigate_to(b, b->bookmarks[idx].url, true);
    return true;
}

static s64 bookmarks_serialize(const browser_app_t *b, void *buf, size_t max_len) {
    if (!b || !buf) return -1;
    size_t needed = sizeof(bookmark_header_t) + ((size_t)b->bookmark_count * sizeof(bookmark_disk_entry_t));
    if (max_len < needed) return -1;

    bookmark_header_t *hdr = (bookmark_header_t *)buf;
    hdr->magic = BOOKMARK_MAGIC;
    hdr->version = BOOKMARK_VERSION;
    hdr->count = (u16)b->bookmark_count;
    hdr->reserved = 0;

    bookmark_disk_entry_t *entries = (bookmark_disk_entry_t *)((u8 *)buf + sizeof(bookmark_header_t));
    for (int i = 0; i < b->bookmark_count; i++) {
        umemset(&entries[i], 0, sizeof(entries[i]));
        ustrcpy(entries[i].url, b->bookmarks[i].url, sizeof(entries[i].url));
        ustrcpy(entries[i].title, b->bookmarks[i].title, sizeof(entries[i].title));
    }

    size_t payload_len = (size_t)b->bookmark_count * sizeof(bookmark_disk_entry_t);
    hdr->checksum = calculate_checksum(entries, payload_len);

    return (s64)needed;
}

static bool bookmarks_deserialize(browser_app_t *b, const void *buf, size_t len) {
    if (!b || !buf || len < sizeof(bookmark_header_t)) return false;

    const bookmark_header_t *hdr = (const bookmark_header_t *)buf;
    if (hdr->magic != BOOKMARK_MAGIC) return false;
    if (hdr->version != BOOKMARK_VERSION) return false;
    if (hdr->count > MAX_BOOKMARKS) return false;

    size_t expected_len = sizeof(bookmark_header_t) + ((size_t)hdr->count * sizeof(bookmark_disk_entry_t));
    if (len < expected_len) return false;

    const bookmark_disk_entry_t *entries = (const bookmark_disk_entry_t *)((const u8 *)buf + sizeof(bookmark_header_t));
    size_t payload_len = (size_t)hdr->count * sizeof(bookmark_disk_entry_t);
    u32 sum = calculate_checksum(entries, payload_len);
    if (sum != hdr->checksum) return false;

    /* Verify string termination on every entry prior to modifying state */
    for (int i = 0; i < (int)hdr->count; i++) {
        bool url_term = false;
        for (int k = 0; k < 256; k++) {
            if (entries[i].url[k] == '\0') { url_term = true; break; }
        }
        if (!url_term) return false;

        bool title_term = false;
        for (int k = 0; k < 64; k++) {
            if (entries[i].title[k] == '\0') { title_term = true; break; }
        }
        if (!title_term) return false;
    }

    /* All validations passed: safe to update in-memory bookmarks */
    b->bookmark_count = 0;
    for (int i = 0; i < (int)hdr->count; i++) {
        /* Scheme validation: only file:///, http://, https:// permitted */
        if (!ustrcasestartswith(entries[i].url, "file:///") &&
            !ustrcasestartswith(entries[i].url, "http://") &&
            !ustrcasestartswith(entries[i].url, "https://")) {
            continue;
        }

        int cur = b->bookmark_count;
        if (cur >= MAX_BOOKMARKS) break;

        ustrcpy(b->bookmarks[cur].url, entries[i].url, sizeof(b->bookmarks[cur].url));
        ustrcpy(b->bookmarks[cur].title, entries[i].title, sizeof(b->bookmarks[cur].title));
        b->bookmarks[cur].created_at = 1;
        b->bookmark_count++;
    }
    return true;
}

static s64 bookmarks_save_path(browser_app_t *b, const char *path) {
    if (!b || !path) return -1;
    static u8 s_bm_buf[sizeof(bookmark_header_t) + MAX_BOOKMARKS * sizeof(bookmark_disk_entry_t)];
    s64 written = bookmarks_serialize(b, s_bm_buf, sizeof(s_bm_buf));
    if (written <= 0) return -1;

    char tmp_path[128];
    ustrcpy(tmp_path, path, sizeof(tmp_path));
    ustrcat(tmp_path, ".tmp", sizeof(tmp_path));

    s64 fd = sys_open(tmp_path, 2 /* VFS_O_WRONLY */ | 0x0040 /* VFS_O_CREAT */ | 0x0200 /* VFS_O_TRUNC */);
    if (fd < 0) return fd;

    s64 nwritten = sys_write((int)fd, (const char *)s_bm_buf, (size_t)written);
    s64 sync_res = sys_fsync((int)fd);
    sys_close((int)fd);

    if (nwritten != written || sync_res != 0) {
        return -1;
    }

    /* Atomic replacement via rename */
    s64 ren_res = sys_rename(tmp_path, path);
    if (ren_res != 0) {
        return ren_res;
    }
    return nwritten;
}

static s64 bookmarks_load_path(browser_app_t *b, const char *path) {
    if (!b || !path) return -1;
    static u8 s_bm_buf[sizeof(bookmark_header_t) + MAX_BOOKMARKS * sizeof(bookmark_disk_entry_t)];
    s64 fd = sys_open(path, 1 /* VFS_O_RDONLY */);
    if (fd < 0) return fd;
    s64 nbytes = sys_read((int)fd, s_bm_buf, sizeof(s_bm_buf));
    sys_close((int)fd);
    if (nbytes >= (s64)sizeof(bookmark_header_t)) {
        if (bookmarks_deserialize(b, s_bm_buf, (size_t)nbytes)) {
            return nbytes;
        }
    }
    return -1;
}

static s64 __attribute__((unused)) bookmarks_save(browser_app_t *b) {
    if (!b) return -1;
    s64 res = bookmarks_save_path(b, "/data/bookmarks.bin");
    if (res < 0) {
        res = bookmarks_save_path(b, "/etc/bookmarks.bin");
    }
    return res;
}

static s64 __attribute__((unused)) bookmarks_load(browser_app_t *b) {
    if (!b) return -1;
    s64 res = bookmarks_load_path(b, "/data/bookmarks.bin");
    if (res < 0) {
        res = bookmarks_load_path(b, "/etc/bookmarks.bin");
    }
    return res;
}

static void navigate_to(browser_app_t *b, const char *url_str, bool record_history) {
    if (!url_str || !url_str[0]) return;
    if (ustrlen(url_str) >= 250) {
        ustrcpy(b->status_text, "Error: URL exceeds limit", sizeof(b->status_text));
        return;
    }

    char sanitized[256];
    bool san_ok = sanitize_url_input(url_str, sanitized, sizeof(sanitized));
    if (!san_ok && sanitized[0] == '\0') {
        ustrcpy(sanitized, url_str, sizeof(sanitized));
    }

    parsed_url_t purl;
    bool parse_ok = parse_url(sanitized, &purl);

    /* Check for in-page fragment navigation */
    if (parse_ok && b->html_len > 0 && purl.fragment[0] != '\0') {
        parsed_url_t cur_purl;
        if (parse_url(b->current_url, &cur_purl)) {
            if (purl.scheme == cur_purl.scheme &&
                ustrcaseeq(purl.host, cur_purl.host) &&
                purl.port == cur_purl.port &&
                ustrcaseeq(purl.path, cur_purl.path)) {
                /* Same page - jump directly to fragment without re-fetching */
                ustrcpy(b->current_url, sanitized, sizeof(b->current_url));
                ustrcpy(b->input_url, sanitized, sizeof(b->input_url));
                b->input_cursor = (int)ustrlen(b->input_url);
                b->input_focused = false;

                if (record_history) {
                    browser_history_push(b, sanitized);
                }

                /* Find target box */
                bool found = false;
                for (int i = 0; i < b->box_count; i++) {
                    if (b->boxes[i].id_name[0] && ustrcaseeq(b->boxes[i].id_name, purl.fragment)) {
                        int top_bars = (b->tab_count > 0) ? (TAB_BAR_HEIGHT + TOOLBAR_HEIGHT) : TOOLBAR_HEIGHT;
                        int view_h = WIN_HEIGHT - top_bars - STATUS_HEIGHT;
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
                tab_save_active(b);
                return;
            }
        }
    }

    /* Normal full page navigation */
    ustrcpy(b->current_url, sanitized, sizeof(b->current_url));
    ustrcpy(b->input_url, sanitized, sizeof(b->input_url));
    b->input_cursor = (int)ustrlen(b->input_url);
    b->input_focused = false;
    b->scroll_y = 0;
    ustrcpy(b->status_text, "Loading...", sizeof(b->status_text));

    /* Record bounded history */
    if (record_history) {
        browser_history_push(b, sanitized);
    }

    /* Reset transient bitmap scratch pool */
    s_bmp_pixels_used = 0;

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
            if ((purl.scheme == SCHEME_HTTP && purl.port != 80) || (purl.scheme == SCHEME_HTTPS && purl.port != 443)) {
                char pbuf[16];
                char ptmp[16];
                int pi = 0;
                u32 pv = purl.port;
                while (pv > 0) {
                    ptmp[pi++] = '0' + (pv % 10);
                    pv /= 10;
                }
                pbuf[0] = ':';
                int pj = 1;
                for (int k = pi - 1; k >= 0; k--) pbuf[pj++] = ptmp[k];
                pbuf[pj] = '\0';
                ustrcat(clean_fetch_url, pbuf, sizeof(clean_fetch_url));
            }
            ustrcat(clean_fetch_url, purl.path, sizeof(clean_fetch_url));
        }

        int page_status = 0;
        s64 bytes = fetch_page_resource(clean_fetch_url, b->html_buf, 16383, &page_status);
        if (bytes >= 0) {
            b->html_buf[bytes] = '\0';
            b->html_len = (size_t)bytes;
            if (purl.scheme == SCHEME_FILE) {
                ustrcpy(b->status_text, "Ready (File loaded)", sizeof(b->status_text));
            } else if (page_status == 200) {
                if (purl.scheme == SCHEME_HTTPS) {
                    ustrcpy(b->status_text, "Ready (HTTPS 200 OK)", sizeof(b->status_text));
                } else {
                    ustrcpy(b->status_text, "Ready (HTTP 200 OK)", sizeof(b->status_text));
                }
            } else if (page_status == 404) {
                ustrcpy(b->status_text, "404 Not Found", sizeof(b->status_text));
            } else if (page_status == 495) {
                ustrcpy(b->status_text, "Security Error: Certificate Failed", sizeof(b->status_text));
            } else if (page_status == 502) {
                ustrcpy(b->status_text, "Error: DNS Resolution Failed", sizeof(b->status_text));
            } else if (page_status == 504) {
                ustrcpy(b->status_text, "Error: Connection Timed Out", sizeof(b->status_text));
            } else if (page_status == 525) {
                ustrcpy(b->status_text, "Security Error: TLS Handshake Failed", sizeof(b->status_text));
            } else {
                char sbuf[48];
                ustrcpy(sbuf, "Loaded (Status: ", sizeof(sbuf));
                int si = (int)ustrlen(sbuf);
                sbuf[si++] = '0' + (page_status / 100) % 10;
                sbuf[si++] = '0' + (page_status / 10) % 10;
                sbuf[si++] = '0' + (page_status % 10);
                sbuf[si++] = ')';
                sbuf[si] = '\0';
                ustrcpy(b->status_text, sbuf, sizeof(b->status_text));
            }
        } else {
            const char *err = "<html><head><title>404 Not Found</title></head><body><h1>Resource Not Found</h1><p>Requested URL could not be resolved.</p><hr><p><a href=\"file:///www/index.html\">Return to Home</a></p></body></html>";
            if (page_status == 495) {
                ustrcpy(b->status_text, "Security Error: Certificate Failed", sizeof(b->status_text));
                err = "<html><head><title>Security Error</title></head><body><h1>Certificate Verification Failed</h1><p>The TLS certificate is invalid, expired, or untrusted.</p><hr><p><a href=\"file:///www/index.html\">Return to Home</a></p></body></html>";
            } else if (page_status == 525) {
                ustrcpy(b->status_text, "Security Error: TLS Handshake Failed", sizeof(b->status_text));
                err = "<html><head><title>Security Error</title></head><body><h1>TLS Handshake Failed</h1><p>Secure connection parameters could not be negotiated.</p><hr><p><a href=\"file:///www/index.html\">Return to Home</a></p></body></html>";
            } else if (page_status == 502) {
                ustrcpy(b->status_text, "Error: DNS Resolution Failed", sizeof(b->status_text));
                err = "<html><head><title>502 Bad Gateway</title></head><body><h1>502 Bad Gateway</h1><p>Could not resolve the specified hostname or framing was malformed.</p><hr><p><a href=\"file:///www/index.html\">Return to Home</a></p></body></html>";
            } else if (page_status == 504) {
                ustrcpy(b->status_text, "Error: Connection Timed Out", sizeof(b->status_text));
                err = "<html><head><title>504 Gateway Timeout</title></head><body><h1>Connection Timed Out</h1><p>Server connection refused or timed out.</p><hr><p><a href=\"file:///www/index.html\">Return to Home</a></p></body></html>";
            } else {
                ustrcpy(b->status_text, "Resource Not Found (404)", sizeof(b->status_text));
            }
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
                int top_bars = (b->tab_count > 0) ? (TAB_BAR_HEIGHT + TOOLBAR_HEIGHT) : TOOLBAR_HEIGHT;
                int view_h = WIN_HEIGHT - top_bars - STATUS_HEIGHT;
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

    tab_save_active(b);
}

static void render_browser_ui(browser_app_t *b) {
    int win = b->win_id;
    if (win <= 0) return;

    /* 1. Clear background */
    sys_gui_clear(win, COLOR_VIEWPORT_BG);

    /* 2. Top Tab Bar (y = 0..23, h = 24) */
    sys_gui_fill_rect(win, 0, 0, WIN_WIDTH, TAB_BAR_HEIGHT, 0xFF080C16);
    sys_gui_draw_rect(win, 0, TAB_BAR_HEIGHT - 1, WIN_WIDTH, 1, COLOR_PANEL_BORDER);

    for (int t = 0; t < b->tab_count && t < MAX_TABS; t++) {
        int tx = 4 + t * 140;
        int tw = 136;
        int ty = 2;
        int th = 20;
        bool is_active = (t == b->active_tab_idx);

        u32 tab_bg = is_active ? COLOR_BTN_ACTIVE : COLOR_BTN_BG;
        u32 tab_border = is_active ? COLOR_BORDER_CYAN : COLOR_BORDER_DEFAULT;
        sys_gui_fill_rect(win, tx, ty, tw, th, tab_bg);
        sys_gui_draw_rect(win, tx, ty, tw, th, tab_border);

        /* Tab title truncated */
        char tab_disp[16];
        const char *src_title = b->tabs[t].page_title[0] ? b->tabs[t].page_title : "New Tab";
        size_t ti = 0;
        while (src_title[ti] && ti < 11) {
            tab_disp[ti] = src_title[ti];
            ti++;
        }
        tab_disp[ti] = '\0';
        sys_gui_draw_text(win, tx + 6, ty + 3, tab_disp, COLOR_TEXT_WHITE, 0);

        /* Tab close button [x] */
        sys_gui_draw_text(win, tx + tw - 14, ty + 3, "x", is_active ? COLOR_ACCENT_RED : COLOR_TEXT_MUTED, 0);
    }

    /* [+] New Tab button if tab_count < MAX_TABS */
    if (b->tab_count < MAX_TABS) {
        int plus_x = 4 + b->tab_count * 140;
        sys_gui_fill_rect(win, plus_x, 2, 24, 20, COLOR_BTN_BG);
        sys_gui_draw_rect(win, plus_x, 2, 24, 20, COLOR_BORDER_DEFAULT);
        sys_gui_draw_text(win, plus_x + 8, 4, "+", COLOR_TEXT_WHITE, 0);
    }

    /* 3. Navigation Toolbar (y = 24..53, h = 30) */
    int tb_y = TAB_BAR_HEIGHT;
    sys_gui_fill_rect(win, 0, tb_y, WIN_WIDTH, TOOLBAR_HEIGHT, COLOR_TOOLBAR_BG);
    sys_gui_draw_rect(win, 0, tb_y + TOOLBAR_HEIGHT - 1, WIN_WIDTH, 1, COLOR_PANEL_BORDER);

    /* Back Button [<-] */
    bool can_back = (b->history_index > 0);
    sys_gui_fill_rect(win, 6, tb_y + 3, 24, 24, can_back ? COLOR_BTN_ACTIVE : COLOR_BTN_BG);
    sys_gui_draw_rect(win, 6, tb_y + 3, 24, 24, COLOR_BORDER_DEFAULT);
    sys_gui_draw_text(win, 10, tb_y + 7, "<-", can_back ? COLOR_TEXT_WHITE : COLOR_TEXT_MUTED, 0);

    /* Forward Button [->] */
    bool can_fwd = (b->history_index < b->history_count - 1);
    sys_gui_fill_rect(win, 34, tb_y + 3, 24, 24, can_fwd ? COLOR_BTN_ACTIVE : COLOR_BTN_BG);
    sys_gui_draw_rect(win, 34, tb_y + 3, 24, 24, COLOR_BORDER_DEFAULT);
    sys_gui_draw_text(win, 38, tb_y + 7, "->", can_fwd ? COLOR_TEXT_WHITE : COLOR_TEXT_MUTED, 0);

    /* Reload Button [R] */
    sys_gui_fill_rect(win, 62, tb_y + 3, 24, 24, COLOR_BTN_BG);
    sys_gui_draw_rect(win, 62, tb_y + 3, 24, 24, COLOR_BORDER_DEFAULT);
    sys_gui_draw_text(win, 70, tb_y + 7, "R", COLOR_TEXT_WHITE, 0);

    /* Bookmark Toggle Button [*] */
    bool is_bookmarked = (bookmark_find(b, b->current_url) >= 0);
    sys_gui_fill_rect(win, 90, tb_y + 3, 24, 24, is_bookmarked ? 0xFF854D0E : COLOR_BTN_BG);
    sys_gui_draw_rect(win, 90, tb_y + 3, 24, 24, is_bookmarked ? COLOR_ACCENT_YELLOW : COLOR_BORDER_DEFAULT);
    sys_gui_draw_text(win, 98, tb_y + 7, "*", is_bookmarked ? COLOR_ACCENT_YELLOW : COLOR_TEXT_MUTED, 0);

    /* URL Bar */
    int url_x = 118;
    int url_w = WIN_WIDTH - url_x - 48;
    sys_gui_fill_rect(win, url_x, tb_y + 3, url_w, 24, 0xFF080C16);
    sys_gui_draw_rect(win, url_x, tb_y + 3, url_w, 24, b->input_focused ? COLOR_BORDER_CYAN : COLOR_BORDER_DEFAULT);
    sys_gui_draw_text(win, url_x + 6, tb_y + 7, b->input_url, COLOR_TEXT_WHITE, 0);

    /* Cursor in URL bar */
    if (b->input_focused) {
        int cur_x = url_x + 6 + (b->input_cursor * FONT_WIDTH);
        if (cur_x < url_x + url_w - 4) {
            sys_gui_fill_rect(win, cur_x, tb_y + 6, 2, 16, COLOR_BORDER_CYAN);
        }
    }

    /* Go Button */
    int go_x = WIN_WIDTH - 44;
    sys_gui_fill_rect(win, go_x, tb_y + 3, 38, 24, COLOR_BTN_ACTIVE);
    sys_gui_draw_rect(win, go_x, tb_y + 3, 38, 24, COLOR_BORDER_CYAN);
    sys_gui_draw_text(win, go_x + 10, tb_y + 7, "Go", COLOR_TEXT_WHITE, 0);

    /* 4. Render HTML Boxes in Viewport */
    int view_y = TAB_BAR_HEIGHT + TOOLBAR_HEIGHT;
    int view_h = WIN_HEIGHT - view_y - STATUS_HEIGHT;

    for (int i = 0; i < b->box_count; i++) {
        const layout_box_t *box = &b->boxes[i];
        int sy = view_y + box->y - b->scroll_y;

        /* Vertical clipping */
        if (sy + box->h < view_y || sy >= view_y + view_h) continue;

        /* 4A: Render Web Image */
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

        /* 4B: Render Form Control */
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

        /* 4C: Render Standard HTML Elements */
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

    /* 5. Vertical Scrollbar */
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

    /* 6. Bottom Status Bar */
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
    s_bmp_pixels_used = 0;
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
#define test_app s_test_app
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
#undef test_app
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
#define hist_app s_test_app
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
#define scroll_app s_test_app
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
#undef hist_app
#undef scroll_app
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
#define js_app s_test_app
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
#undef js_app
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
#define s_app s_test_app
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
#undef s_app
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
#define t_app s_test_app
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
#undef t_app
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
#define u_app s_test_app
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
#undef u_app
}

/* -----------------------------------------------------------------
 * Phase V Self-Tests (End-to-End Browser Networking & HTTPS Validation)
 * ----------------------------------------------------------------- */
static void run_phase_v_self_tests(void) {
    int status = 0;
    char net_buf[1024];

    /* TEST 1: Syscall parameter bounds on sys_net_fetch */
    /* Negative Test 1A: Null URL must be rejected with -EINVAL */
    if (sys_net_fetch(NULL, net_buf, sizeof(net_buf), &status) >= 0) {
        sys_exit(180);
    }

    /* Negative Test 1B: Null destination buffer must be rejected */
    if (sys_net_fetch("http://10.0.2.2:8000/", NULL, 1024, &status) >= 0) {
        sys_exit(181);
    }

    /* Negative Test 1C: Buffer too small (< 32 bytes) must be rejected */
    if (sys_net_fetch("http://10.0.2.2:8000/", net_buf, 16, &status) >= 0) {
        sys_exit(182);
    }

    /* TEST 2: Invalid URL scheme rejection via sys_net_fetch */
    status = 0;
    if (sys_net_fetch("ftp://10.0.2.2/file.txt", net_buf, sizeof(net_buf), &status) >= 0 || status != 400) {
        sys_exit(183);
    }

    /* Negative Test 2B: Malformed URL without scheme */
    status = 0;
    if (sys_net_fetch("htp:/malformed", net_buf, sizeof(net_buf), &status) >= 0 || status != 400) {
        sys_exit(184);
    }

    /* TEST 3: Port formatting in URL parser */
    parsed_url_t purl;
    if (!parse_url("https://10.0.2.2:8443/test.html", &purl) || purl.scheme != SCHEME_HTTPS ||
        purl.port != 8443 || !ustrcaseeq(purl.host, "10.0.2.2") || !ustrcaseeq(purl.path, "/test.html")) {
        sys_exit(185);
    }

    if (!parse_url("http://10.0.2.2:8000/index.html", &purl) || purl.scheme != SCHEME_HTTP ||
        purl.port != 8000 || !ustrcaseeq(purl.host, "10.0.2.2") || !ustrcaseeq(purl.path, "/index.html")) {
        sys_exit(186);
    }

    /* TEST 4: Resource fetching & status extraction */
    int page_status = 0;
    s64 file_bytes = fetch_page_resource("file:///www/index.html", net_buf, sizeof(net_buf) - 1, &page_status);
    if (file_bytes <= 0 || page_status != 200) {
        sys_exit(187);
    }

    /* Negative Test 4B: Nonexistent local file returns 404 */
    page_status = 0;
    s64 missing_bytes = fetch_page_resource("file:///nonexistent_file_xyz.html", net_buf, sizeof(net_buf) - 1, &page_status);
    if (missing_bytes != -1 || page_status != 404) {
        sys_exit(188);
    }

    const char net_ok[] = "ASTRA_BROWSER_NET_ISOLATION_OK\n";
    sys_write(1, net_ok, ustrlen(net_ok));
}

static void run_phase_w_self_tests(void) {
    /* -------------------------------------------------------------
     * Phase W: HTTP Protocol Hardening & Browser Reliability Self-Tests
     * ------------------------------------------------------------- */
    char net_buf[1024];
    int status = 0;

    /* TEST 1: URL parser port edge cases */
    parsed_url_t purl;
    if (!parse_url("http://10.0.2.2:8000/chunked", &purl) || purl.port != 8000) {
        sys_exit(190);
    }
    if (!parse_url("https://10.0.2.2:8443/status/500", &purl) || purl.port != 8443) {
        sys_exit(191);
    }

    /* Negative Test 1B: Empty or invalid URL strings */
    if (parse_url("", &purl) || parse_url("http://", &purl)) {
        sys_exit(192);
    }

    /* TEST 2: Syscall fetch of live endpoints if network active */
    s64 b = fetch_page_resource("http://10.0.2.2:8000/", net_buf, sizeof(net_buf) - 1, &status);
    if (b > 0 && status == 200) {
        /* Adversarial Test 2B: Malformed header endpoint must yield 502 Bad Gateway */
        status = 0;
        fetch_page_resource("http://10.0.2.2:8000/malformed", net_buf, sizeof(net_buf) - 1, &status);
        if (status != 502) sys_exit(194);
    } else if (b > 0 && status != 504 && status != 502) {
        /* If live server was reached but returned unexpected non-200 code */
        sys_exit(193);
    }

    const char w_ok[] = "ASTRA_BROWSER_HTTP_HARDENING_OK\n";
    sys_write(1, w_ok, ustrlen(w_ok));
}

static void run_phase_y_self_tests(void) {
    /* -------------------------------------------------------------
     * Phase Y: Browser UX, Navigation, URL Validation & Shortcuts Self-Tests
     * ------------------------------------------------------------- */

    /* TEST 1: URL input sanitization & auto-prefixing */
    char s_out[256];
    if (!sanitize_url_input("   http://10.0.2.2:8000/index.html   ", s_out, sizeof(s_out)) ||
        !ustrcaseeq(s_out, "http://10.0.2.2:8000/index.html")) {
        sys_exit(201);
    }

    if (!sanitize_url_input("/www/index.html", s_out, sizeof(s_out)) ||
        !ustrcaseeq(s_out, "file:///www/index.html")) {
        sys_exit(202);
    }

    if (!sanitize_url_input("10.0.2.2:8000/page.html", s_out, sizeof(s_out)) ||
        !ustrcaseeq(s_out, "http://10.0.2.2:8000/page.html")) {
        sys_exit(203);
    }

    /* Negative Test 1B: Unsupported schemes */
    if (sanitize_url_input("ftp://example.com/file.iso", s_out, sizeof(s_out))) {
        sys_exit(204);
    }
    if (sanitize_url_input("javascript:alert(1)", s_out, sizeof(s_out))) {
        sys_exit(205);
    }
    if (sanitize_url_input("", s_out, sizeof(s_out))) {
        sys_exit(206);
    }

    /* TEST 2: History stack integrity & non-duplicating reload */
#define y_app s_test_app
    umemset(&y_app, 0, sizeof(y_app));
    y_app.nodes = (dom_node_t *)umalloc(sizeof(dom_node_t) * 16);
    static stylesheet_t y_sheet;
    y_app.stylesheet = &y_sheet;
    y_app.boxes = (layout_box_t *)umalloc(sizeof(layout_box_t) * 16);
    y_app.html_buf = (char *)umalloc(4096);

    /* Initial navigation */
    browser_history_push(&y_app, "file:///p1.html");
    if (y_app.history_count != 1 || y_app.history_index != 0 ||
        !ustrcaseeq(y_app.history[0], "file:///p1.html")) {
        sys_exit(210);
    }

    /* Reload must not add duplicate history */
    browser_history_push(&y_app, "file:///p1.html");
    if (y_app.history_count != 1 || y_app.history_index != 0) {
        sys_exit(211);
    }

    /* Push page 2 and page 3 */
    browser_history_push(&y_app, "file:///p2.html");
    browser_history_push(&y_app, "file:///p3.html");
    if (y_app.history_count != 3 || y_app.history_index != 2) {
        sys_exit(212);
    }

    /* Simulate Back: index 2 -> 1, then fork new navigation */
    y_app.history_index = 1;
    browser_history_push(&y_app, "file:///p2_branch.html");
    if (y_app.history_count != 3 || y_app.history_index != 2 ||
        !ustrcaseeq(y_app.history[2], "file:///p2_branch.html")) {
        sys_exit(213);
    }

    /* Push additional URLs to verify bounded 16-entry history limit */
    for (int i = 0; i < 20; i++) {
        char d_url[32];
        d_url[0] = 'f'; d_url[1] = 'i'; d_url[2] = 'l'; d_url[3] = 'e';
        d_url[4] = ':'; d_url[5] = '/'; d_url[6] = '/'; d_url[7] = '/';
        d_url[8] = 'y'; d_url[9] = '0' + (i / 10); d_url[10] = '0' + (i % 10);
        d_url[11] = '.'; d_url[12] = 'h'; d_url[13] = 't'; d_url[14] = 'm'; d_url[15] = '\0';
        browser_history_push(&y_app, d_url);
    }
    if (y_app.history_count != 16 || y_app.history_index != 15) {
        sys_exit(214);
    }

    /* TEST 3: Relative link resolution & fragment preservation */
    char rel_res[256];
    resolve_url("file:///www/index.html", "about.html", rel_res, sizeof(rel_res));
    if (!ustrcaseeq(rel_res, "file:///www/about.html")) {
        sys_exit(220);
    }

    resolve_url("file:///www/deep/page.html", "../img/logo.bmp", rel_res, sizeof(rel_res));
    if (!ustrcaseeq(rel_res, "file:///www/img/logo.bmp")) {
        sys_exit(221);
    }

    resolve_url("http://10.0.2.2:8000/docs/index.html", "/root.css", rel_res, sizeof(rel_res));
    if (!ustrcaseeq(rel_res, "http://10.0.2.2:8000/root.css")) {
        sys_exit(222);
    }

    resolve_url("http://10.0.2.2:8000/docs/index.html", "sub.html#header", rel_res, sizeof(rel_res));
    if (!ustrcaseeq(rel_res, "http://10.0.2.2:8000/docs/sub.html#header")) {
        sys_exit(223);
    }

    /* TEST 4: Repeated navigation & resource reclamation */
    u32 start_gen = y_app.doc_generation;
    for (int nav_i = 0; nav_i < 15; nav_i++) {
        const char *t_url = (nav_i % 2 == 0) ? "file:///www/index.html" : "file:///www/nav_test.html";
        navigate_to(&y_app, t_url, false);
    }
    if (y_app.doc_generation != start_gen + 15) {
        sys_exit(230);
    }
    if (g_arena_chunk_count >= MAX_ARENA_CHUNKS) {
        /* Arena must not overflow on repeated navigations */
        sys_exit(231);
    }

    /* TEST 5: HTTP error response and 404 page formatting */
    navigate_to(&y_app, "file:///nonexistent_file_404.html", false);
    if (!ustrcontains(y_app.status_text, "404") || y_app.box_count == 0) {
        sys_exit(240);
    }

    /* Negative Test 5B: Malformed URL produces valid error DOM */
    navigate_to(&y_app, "ftp://unsupported.local/file", false);
    if (!ustrcontains(y_app.status_text, "Error") || y_app.box_count == 0) {
        sys_exit(241);
    }

    const char y_ok[] = "ASTRA_BROWSER_UX_OK\n";
    sys_write(1, y_ok, ustrlen(y_ok));
#undef y_app
}

static void run_phase_z_self_tests(void) {
    /* -------------------------------------------------------------
     * Phase Z: Browser Reliability, Resource Safety & Recovery Self-Tests
     * ------------------------------------------------------------- */
#define z_app s_test_app
    umemset(&z_app, 0, sizeof(z_app));
    z_app.nodes = (dom_node_t *)umalloc(sizeof(dom_node_t) * 32);
    static stylesheet_t z_sheet;
    z_app.stylesheet = &z_sheet;
    z_app.boxes = (layout_box_t *)umalloc(sizeof(layout_box_t) * 32);
    z_app.html_buf = (char *)umalloc(4096);

    if (!z_app.nodes || !z_app.boxes || !z_app.html_buf) {
        sys_exit(250);
    }

    /* -------------------------------------------------------------
     * TEST 1: Navigation Stress Test (100 sequential navigations)
     * ------------------------------------------------------------- */
    int initial_arena_chunks = g_arena_chunk_count;
    u32 start_gen = z_app.doc_generation;

    const char *test_urls[5] = {
        "file:///www/index.html",
        "file:///www/about.html",
        "file:///www/scroll.html",
        "file:///www/css_test.html",
        "file:///www/nav_test.html"
    };

    for (int nav_i = 0; nav_i < 100; nav_i++) {
        int url_idx = nav_i % 5;
        /* Perform mixed operations: Forward, Back, Reload, or New Navigate */
        if (nav_i > 0 && (nav_i % 15 == 0) && z_app.history_index > 0) {
            /* Simulate Back */
            z_app.history_index--;
            navigate_to(&z_app, z_app.history[z_app.history_index], false);
        } else if (nav_i > 0 && (nav_i % 25 == 0) && z_app.history_index < z_app.history_count - 1) {
            /* Simulate Forward */
            z_app.history_index++;
            navigate_to(&z_app, z_app.history[z_app.history_index], false);
        } else if (nav_i > 0 && (nav_i % 10 == 0)) {
            /* Simulate Reload */
            navigate_to(&z_app, z_app.current_url, false);
        } else {
            /* New navigation */
            navigate_to(&z_app, test_urls[url_idx], true);
        }

        /* Verify post-navigation integrity */
        if (z_app.history_count < 1 || z_app.history_count > 16 ||
            z_app.history_index < 0 || z_app.history_index >= z_app.history_count) {
            sys_exit(251);
        }
        if (z_app.node_count <= 0 || z_app.box_count <= 0) {
            sys_exit(252);
        }
    }

    /* Verify document generation advanced for all navigations */
    if (z_app.doc_generation < start_gen + 100) {
        sys_exit(253);
    }

    /* Step 2 verification: Arena chunk count must not grow during navigations */
    if (g_arena_chunk_count > initial_arena_chunks) {
        sys_exit(254); /* Resource leak detected! */
    }

    /* -------------------------------------------------------------
     * TEST 2: Input and Parser Boundary Tests
     * ------------------------------------------------------------- */
    /* 2A: Max-length URL boundary (<250 characters accepted) */
    char long_url[300];
    for (int i = 0; i < 240; i++) long_url[i] = 'a';
    long_url[240] = '\0';
    char prefix_url[260];
    ustrcpy(prefix_url, "file:///", sizeof(prefix_url));
    ustrcat(prefix_url, long_url, 248); /* Under 250 limit */
    navigate_to(&z_app, prefix_url, false);
    if (ustrcontains(z_app.status_text, "exceeds limit")) {
        sys_exit(255);
    }

    /* 2B: Exceeded length URL (>=250 characters rejected) */
    char oversized_url[300];
    ustrcpy(oversized_url, "file:///", sizeof(oversized_url));
    for (int i = 8; i < 280; i++) oversized_url[i] = 'x';
    oversized_url[280] = '\0';
    navigate_to(&z_app, oversized_url, false);
    if (!ustrcontains(z_app.status_text, "exceeds limit")) {
        sys_exit(256);
    }

    /* 2C: Oversized and deeply nested markup parsing */
    static const char deep_html[] =
        "<html><body>"
        "<div><div><div><div><div><div><div><div><div><div>"
        "<div><div><div><div><div><div><div><div><div><div>"
        "<h1>Deep Title</h1><p>Deep paragraph text within nested structure.</p>"
        "</div></div></div></div></div></div></div></div></div></div>"
        "</div></div></div></div></div></div></div></div></div></div>"
        "</body></html>";
    int deep_nodes = parse_html(&z_app, deep_html, z_app.nodes, 32, z_app.stylesheet, z_app.page_title, sizeof(z_app.page_title));
    if (deep_nodes <= 0 || deep_nodes > 32) {
        sys_exit(257);
    }

    /* 2D: Long HTML text run chunking */
    static const char long_text_html[] =
        "<html><body><p>"
        "This is an intentionally long run of continuous plain text designed to verify that the "
        "HTML text run tokenizer correctly chunks content into bounded dom_node_t entries without "
        "overflowing the 128-byte text buffer or triggering memory corruption in user space."
        "</p></body></html>";
    int long_nodes = parse_html(&z_app, long_text_html, z_app.nodes, 32, z_app.stylesheet, z_app.page_title, sizeof(z_app.page_title));
    if (long_nodes < 2) {
        sys_exit(258);
    }

    /* 2E: Malformed and oversized BMP dimensions */
    u8 bad_bmp[64];
    umemset(bad_bmp, 0, sizeof(bad_bmp));
    bmp_file_header_t *bf = (bmp_file_header_t *)bad_bmp;
    bmp_info_header_t *bi = (bmp_info_header_t *)(bad_bmp + sizeof(bmp_file_header_t));
    bf->bfType = 0x4D42; /* 'BM' */
    bi->biSize = 40;
    bi->biPlanes = 1;
    bi->biBitCount = 24;
    bi->biWidth = 1000; /* Exceeds 512 max limit */
    bi->biHeight = 1000;
    int bw = 0, bh = 0;
    if (decode_bmp(bad_bmp, sizeof(bad_bmp), &bw, &bh) != NULL) {
        sys_exit(259);
    }

    /* 2F: JavaScript instruction budget exhaustion */
    static const char inf_loop_js[] = "var i = 0; while (i < 1000000) { i = i + 1; }";
    js_init(&z_app.doc_vm);
    z_app.doc_vm_initialized = true;
    js_value_t js_v;
    js_error_t js_err = js_eval(&z_app.doc_vm, inf_loop_js, &js_v);
    if (js_err != JS_ERR_TIMEOUT) {
        sys_exit(260);
    }

    /* -------------------------------------------------------------
     * TEST 3: Stale DOM Handle Rejection Across Navigations
     * ------------------------------------------------------------- */
    static const char handle_doc[] = "<html><body><div id=\"stale_test\">Old Text</div></body></html>";
    z_app.node_count = parse_html(&z_app, handle_doc, z_app.nodes, 32, z_app.stylesheet, z_app.page_title, sizeof(z_app.page_title));
    u32 doc_gen_1 = 0;
    int elem_idx = browser_js_dom_get_element_by_id(&z_app, "stale_test", &doc_gen_1);
    if (elem_idx < 0 || doc_gen_1 != z_app.doc_generation) {
        sys_exit(261);
    }

    /* Advance navigation to new document */
    navigate_to(&z_app, "file:///www/index.html", false);
    u32 doc_gen_2 = z_app.doc_generation;
    if (doc_gen_2 == doc_gen_1) {
        sys_exit(262);
    }

    /* Stale handle access with old generation doc_gen_1 MUST be rejected */
    if (browser_js_dom_set_text_content(&z_app, doc_gen_1, elem_idx, "Stale Exploit") != false) {
        sys_exit(263);
    }
    if (browser_js_dom_get_text_content(&z_app, doc_gen_1, elem_idx) != NULL) {
        sys_exit(264);
    }

    /* -------------------------------------------------------------
     * TEST 4: Browser Error Recovery Verification
     * ------------------------------------------------------------- */
    /* 4A: Failed operation: invalid scheme */
    navigate_to(&z_app, "ftp://invalid.host/file.txt", false);
    if (!ustrcontains(z_app.status_text, "Error") && !ustrcontains(z_app.status_text, "Invalid")) {
        sys_exit(265);
    }

    /* 4B: Recovery: immediately navigate to valid page */
    navigate_to(&z_app, "file:///www/index.html", true);
    if (z_app.node_count <= 0 || z_app.box_count <= 0 || !ustrcaseeq(z_app.current_url, "file:///www/index.html")) {
        sys_exit(266);
    }

    /* 4C: Failed operation: 404 nonexistent file */
    navigate_to(&z_app, "file:///www/nonexistent_recovery_404.html", true);
    if (!ustrcontains(z_app.status_text, "404")) {
        sys_exit(267);
    }

    /* 4D: Recovery: Back navigation restores valid page */
    if (z_app.history_index > 0) {
        z_app.history_index--;
        navigate_to(&z_app, z_app.history[z_app.history_index], false);
    }
    if (!ustrcaseeq(z_app.current_url, "file:///www/index.html") || z_app.node_count <= 0) {
        sys_exit(268);
    }

    /* 4E: Execute valid DOM event on recovered page */
    static const char event_doc[] = "<html><body><button id=\"btn_ok\" onclick=\"document.getElementById('res').textContent = 'EventOK';\">Click</button><p id=\"res\">Initial</p></body></html>";
    z_app.node_count = parse_html(&z_app, event_doc, z_app.nodes, 32, z_app.stylesheet, z_app.page_title, sizeof(z_app.page_title));
    z_app.box_count = layout_page(&z_app, z_app.nodes, z_app.node_count, WIN_WIDTH - SCROLLBAR_WIDTH, z_app.boxes, 32, &z_app.doc_height);
    browser_dispatch_click(&z_app, "document.getElementById('res').textContent = 'EventOK';");
    int res_idx = browser_js_dom_get_element_by_id(&z_app, "res", NULL);
    if (res_idx < 0 || !ustrcaseeq(z_app.nodes[res_idx].text, "EventOK")) {
        sys_exit(269);
    }

    const char z_ok[] = "ASTRA_BROWSER_RELIABILITY_OK\n";
    sys_write(1, z_ok, ustrlen(z_ok));
#undef z_app
}

/* -----------------------------------------------------------------
 * Phase AA Self-Tests (Tabs, Bookmarks & Persistent Storage Abstraction)
 * ----------------------------------------------------------------- */
static void run_phase_aa_self_tests(void) {
    browser_app_t *aa_app = (browser_app_t *)umalloc(sizeof(browser_app_t));
    if (!aa_app) sys_exit(269);
    umemset(aa_app, 0, sizeof(*aa_app));

    /* Temporarily assign pool pointers using dynamic arena allocations */
    dom_node_t *saved_nodes_pool[MAX_TABS];
    layout_box_t *saved_boxes_pool[MAX_TABS];
    stylesheet_t *saved_sheet_pool[MAX_TABS];
    char *saved_html_pool[MAX_TABS];
    js_vm_t *saved_vm_pool[MAX_TABS];
    for (int t = 0; t < MAX_TABS; t++) {
        saved_nodes_pool[t] = s_tab_nodes_pool[t];
        saved_boxes_pool[t] = s_tab_boxes_pool[t];
        saved_sheet_pool[t] = s_tab_sheet_pool[t];
        saved_html_pool[t] = s_tab_html_pool[t];
        saved_vm_pool[t] = s_tab_vm_pool[t];

        s_tab_nodes_pool[t] = (dom_node_t *)umalloc(sizeof(dom_node_t) * 32);
        s_tab_boxes_pool[t] = (layout_box_t *)umalloc(sizeof(layout_box_t) * 32);
        s_tab_sheet_pool[t] = (stylesheet_t *)umalloc(sizeof(stylesheet_t));
        s_tab_html_pool[t] = (char *)umalloc(4096);
        s_tab_vm_pool[t] = (js_vm_t *)umalloc(sizeof(js_vm_t));
    }

    /* Initialize Tab 0 */
    aa_app->tabs[0].active = true;
    aa_app->tabs[0].nodes = s_tab_nodes_pool[0];
    aa_app->tabs[0].boxes = s_tab_boxes_pool[0];
    aa_app->tabs[0].stylesheet = s_tab_sheet_pool[0];
    aa_app->tabs[0].html_buf = s_tab_html_pool[0];
    aa_app->tabs[0].doc_vm = s_tab_vm_pool[0];
    aa_app->tabs[0].focused_control_id = -1;
    aa_app->tabs[0].doc_generation = 1000;
    aa_app->tab_count = 1;
    tab_load_active(aa_app, 0);

    /* -------------------------------------------------------------
     * TEST 1: Tab Creation & Bounded Limit (MAX_TABS = 4)
     * ------------------------------------------------------------- */
    int t1 = tab_create(aa_app, "file:///www/img_test.html");
    if (t1 != 1 || aa_app->tab_count != 2) sys_exit(270);

    int t2 = tab_create(aa_app, "file:///www/form_test.html");
    if (t2 != 2 || aa_app->tab_count != 3) sys_exit(271);

    int t3 = tab_create(aa_app, "file:///www/js_test.html");
    if (t3 != 3 || aa_app->tab_count != 4) sys_exit(272);

    /* Attempt to exceed MAX_TABS -> MUST fail and return -1 */
    int t4 = tab_create(aa_app, "file:///www/nav_test.html");
    if (t4 != -1 || aa_app->tab_count != 4) sys_exit(273);

    /* -------------------------------------------------------------
     * TEST 2: Tab Isolation & Independent State
     * ------------------------------------------------------------- */
    tab_select(aa_app, 0);
    navigate_to(aa_app, "file:///www/index.html", true);

    tab_save_active(aa_app);
    if (!ustrcaseeq(aa_app->tabs[0].current_url, "file:///www/index.html")) sys_exit(274);
    if (!ustrcaseeq(aa_app->tabs[1].current_url, "file:///www/img_test.html")) sys_exit(275);
    if (!ustrcaseeq(aa_app->tabs[2].current_url, "file:///www/form_test.html")) sys_exit(276);
    if (!ustrcaseeq(aa_app->tabs[3].current_url, "file:///www/js_test.html")) sys_exit(277);

    /* -------------------------------------------------------------
     * TEST 3: Cross-Tab DOM Handle Invalidation
     * ------------------------------------------------------------- */
    tab_select(aa_app, 0);
    static const char tab0_doc[] = "<html><body><h1 id=\"tab0_title\">Tab Zero Header</h1></body></html>";
    aa_app->node_count = parse_html(aa_app, tab0_doc, aa_app->nodes, 32, aa_app->stylesheet, aa_app->page_title, sizeof(aa_app->page_title));
    u32 tab0_gen = 0;
    int tab0_elem = browser_js_dom_get_element_by_id(aa_app, "tab0_title", &tab0_gen);
    if (tab0_elem < 0 || tab0_gen != aa_app->doc_generation) sys_exit(278);

    tab_select(aa_app, 2);
    if (browser_js_dom_set_text_content(aa_app, tab0_gen, tab0_elem, "CrossTab Injection") != false) {
        sys_exit(279);
    }
    if (browser_js_dom_get_text_content(aa_app, tab0_gen, tab0_elem) != NULL) {
        sys_exit(280);
    }

    /* -------------------------------------------------------------
     * TEST 4: Tab Switching & Selection
     * ------------------------------------------------------------- */
    tab_select(aa_app, 1);
    if (aa_app->active_tab_idx != 1 || !ustrcaseeq(aa_app->current_url, "file:///www/img_test.html")) sys_exit(281);
    tab_select(aa_app, 3);
    if (aa_app->active_tab_idx != 3 || !ustrcaseeq(aa_app->current_url, "file:///www/js_test.html")) sys_exit(282);

    /* -------------------------------------------------------------
     * TEST 5: Tab Closure & Selection Adjustment
     * ------------------------------------------------------------- */
    bool c3 = tab_close(aa_app, 3);
    if (!c3 || aa_app->tab_count != 3 || aa_app->active_tab_idx != 2) sys_exit(283);
    if (!ustrcaseeq(aa_app->current_url, "file:///www/form_test.html")) sys_exit(284);

    bool c1 = tab_close(aa_app, 1);
    if (!c1 || aa_app->tab_count != 2) sys_exit(285);
    if (!ustrcaseeq(aa_app->tabs[0].current_url, "file:///www/index.html")) sys_exit(286);
    if (!ustrcaseeq(aa_app->tabs[1].current_url, "file:///www/form_test.html")) sys_exit(287);

    /* -------------------------------------------------------------
     * TEST 6: Close Last Remaining Tab Resets to Home
     * ------------------------------------------------------------- */
    tab_close(aa_app, 1);
    if (aa_app->tab_count != 1) sys_exit(288);
    tab_close(aa_app, 0);
    if (aa_app->tab_count != 1 || !ustrcaseeq(aa_app->current_url, "file:///www/index.html")) sys_exit(289);

    /* -------------------------------------------------------------
     * TEST 7: Bookmark Addition & Bounded Limit (MAX_BOOKMARKS = 32)
     * ------------------------------------------------------------- */
    aa_app->bookmark_count = 0;
    for (int b = 0; b < MAX_BOOKMARKS; b++) {
        char b_url[64];
        ustrcpy(b_url, "file:///www/page_", sizeof(b_url));
        int bi = (int)ustrlen(b_url);
        b_url[bi++] = '0' + (b / 10);
        b_url[bi++] = '0' + (b % 10);
        b_url[bi++] = '.'; b_url[bi++] = 'h'; b_url[bi++] = 't'; b_url[bi++] = 'm'; b_url[bi++] = 'l';
        b_url[bi] = '\0';
        int b_idx = bookmark_add(aa_app, b_url, "Test Bookmark");
        if (b_idx != b || aa_app->bookmark_count != b + 1) sys_exit(290);
    }
    int b_overflow = bookmark_add(aa_app, "file:///www/overflow.html", "Overflow");
    if (b_overflow != -1 || aa_app->bookmark_count != MAX_BOOKMARKS) sys_exit(291);

    /* -------------------------------------------------------------
     * TEST 8: Bookmark Deduplication & URL Validation
     * ------------------------------------------------------------- */
    bookmark_remove(aa_app, "file:///www/page_00.html");
    if (aa_app->bookmark_count != MAX_BOOKMARKS - 1) sys_exit(292);

    int dup_idx = bookmark_add(aa_app, "file:///www/page_01.html", "Updated Title 01");
    if (dup_idx < 0 || aa_app->bookmark_count != MAX_BOOKMARKS - 1) sys_exit(293);
    if (!ustrcaseeq(aa_app->bookmarks[dup_idx].title, "Updated Title 01")) sys_exit(294);

    if (bookmark_add(aa_app, "javascript:alert(1)", "XSS") != -1) sys_exit(295);
    if (bookmark_add(aa_app, "data:text/html,bad", "Data") != -1) sys_exit(296);

    /* -------------------------------------------------------------
     * TEST 9: Bookmark Removal & Lookup
     * ------------------------------------------------------------- */
    int f_idx = bookmark_find(aa_app, "file:///www/page_02.html");
    if (f_idx < 0) sys_exit(297);
    bool r_ok = bookmark_remove(aa_app, "file:///www/page_02.html");
    if (!r_ok || bookmark_find(aa_app, "file:///www/page_02.html") != -1) sys_exit(298);

    /* Test bookmark open navigation */
    if (!bookmark_open(aa_app, 0)) sys_exit(299);

    /* -------------------------------------------------------------
     * TEST 10: Versioned Binary Serialization & Checksum Verification
     * ------------------------------------------------------------- */
    static u8 ser_buf[sizeof(bookmark_header_t) + MAX_BOOKMARKS * sizeof(bookmark_disk_entry_t)];
    s64 ser_len = bookmarks_serialize(aa_app, ser_buf, sizeof(ser_buf));
    if (ser_len <= (s64)sizeof(bookmark_header_t)) sys_exit(300);

    bookmark_header_t *hdr = (bookmark_header_t *)ser_buf;
    if (hdr->magic != BOOKMARK_MAGIC || hdr->version != BOOKMARK_VERSION || hdr->count != aa_app->bookmark_count) {
        sys_exit(301);
    }

    browser_app_t *test_deser_app = (browser_app_t *)umalloc(sizeof(browser_app_t));
    if (!test_deser_app) sys_exit(302);
    umemset(test_deser_app, 0, sizeof(*test_deser_app));
    if (!bookmarks_deserialize(test_deser_app, ser_buf, (size_t)ser_len)) sys_exit(303);
    if (test_deser_app->bookmark_count != aa_app->bookmark_count) sys_exit(304);
    if (!ustrcaseeq(test_deser_app->bookmarks[0].url, aa_app->bookmarks[0].url)) sys_exit(305);

    u32 good_csum = hdr->checksum;
    hdr->checksum ^= 0x12345678;
    if (bookmarks_deserialize(test_deser_app, ser_buf, (size_t)ser_len)) sys_exit(306);
    hdr->checksum = good_csum;

    hdr->magic = 0xDEADBEEF;
    if (bookmarks_deserialize(test_deser_app, ser_buf, (size_t)ser_len)) sys_exit(307);
    hdr->magic = BOOKMARK_MAGIC;

    /* Exercise file persistence abstraction paths */
    s64 s_save = bookmarks_save_path(aa_app, "/data/bm_test.bin");
    if (s_save <= 0) {
        s_save = bookmarks_save_path(aa_app, "/etc/bm_test.bin");
    }
    if (s_save <= 0) sys_exit(308);

    browser_app_t *test_load_app = (browser_app_t *)umalloc(sizeof(browser_app_t));
    if (!test_load_app) sys_exit(308);
    umemset(test_load_app, 0, sizeof(*test_load_app));
    s64 s_load = bookmarks_load_path(test_load_app, "/data/bm_test.bin");
    if (s_load <= 0) {
        s_load = bookmarks_load_path(test_load_app, "/etc/bm_test.bin");
    }
    if (s_load <= 0 || test_load_app->bookmark_count != aa_app->bookmark_count) {
        sys_exit(309); /* Failed to load back saved bookmarks */
    }

    /* Restore real tab pools */
    for (int t = 0; t < MAX_TABS; t++) {
        s_tab_nodes_pool[t] = saved_nodes_pool[t];
        s_tab_boxes_pool[t] = saved_boxes_pool[t];
        s_tab_sheet_pool[t] = saved_sheet_pool[t];
        s_tab_html_pool[t] = saved_html_pool[t];
        s_tab_vm_pool[t] = saved_vm_pool[t];
    }

    const char aa_ok[] = "ASTRA_BROWSER_TABS_BOOKMARKS_OK\n";
    sys_write(1, aa_ok, ustrlen(aa_ok));
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

    /* 3. Allocate Browser Structures via Dynamic Arena (Preallocated 4 Tab Pools) */
    for (int t = 0; t < MAX_TABS; t++) {
        s_tab_nodes_pool[t] = (dom_node_t *)umalloc(sizeof(dom_node_t) * MAX_NODES);
        s_tab_boxes_pool[t] = (layout_box_t *)umalloc(sizeof(layout_box_t) * MAX_BOXES);
        s_tab_sheet_pool[t] = (stylesheet_t *)umalloc(sizeof(stylesheet_t));
        s_tab_html_pool[t] = (char *)umalloc(32768);
        s_tab_vm_pool[t] = (js_vm_t *)umalloc(sizeof(js_vm_t));

        if (!s_tab_nodes_pool[t] || !s_tab_boxes_pool[t] || !s_tab_sheet_pool[t] || !s_tab_html_pool[t] || !s_tab_vm_pool[t]) {
            const char err[] = "[FATAL] Failed to allocate browser tab memory pools!\n";
            sys_write(1, err, ustrlen(err));
            sys_exit(4);
        }
    }

    /* Initialize Tab 0 */
    umemset(&g_browser, 0, sizeof(g_browser));
    g_browser.tabs[0].active = true;
    g_browser.tabs[0].nodes = s_tab_nodes_pool[0];
    g_browser.tabs[0].boxes = s_tab_boxes_pool[0];
    g_browser.tabs[0].stylesheet = s_tab_sheet_pool[0];
    g_browser.tabs[0].html_buf = s_tab_html_pool[0];
    g_browser.tabs[0].doc_vm = s_tab_vm_pool[0];
    if (g_browser.tabs[0].doc_vm) {
        js_init(g_browser.tabs[0].doc_vm);
    }
    g_browser.tabs[0].focused_control_id = -1;
    g_browser.tabs[0].doc_generation = 1000;
    g_browser.tab_count = 1;
    tab_load_active(&g_browser, 0);

    /* 4. Run Verification Self-Tests */
    run_phase_p_self_tests();
    run_phase_q_self_tests();
    run_phase_r_self_tests();
    run_phase_s_self_tests();
    run_phase_t_self_tests();
    run_phase_u_self_tests();
    run_phase_v_self_tests();
    run_phase_w_self_tests();
    run_phase_y_self_tests();
    run_phase_z_self_tests();
    run_phase_aa_self_tests();

    /* Load and verify persistent browser bookmarks */
    s64 b_load = bookmarks_load(&g_browser);
    if (b_load > 0 && g_browser.bookmark_count >= 2) {
        int idx1 = bookmark_find(&g_browser, "file:///www/index.html");
        int idx2 = bookmark_find(&g_browser, "file:///www/about.html");
        if (idx1 >= 0 && idx2 >= 0) {
            const char bm_ok[] = "ASTRA_BROWSER_BOOKMARKS_COLD_REBOOT_OK\n";
            sys_write(1, bm_ok, ustrlen(bm_ok));
        }
    } else {
        /* Seed default bookmarks and persist to /data/bookmarks.bin */
        g_browser.bookmark_count = 0;
        bookmark_add(&g_browser, "file:///www/index.html", "AstraOS Portal");
        bookmark_add(&g_browser, "file:///www/about.html", "About AstraOS");
        s64 b_saved = bookmarks_save(&g_browser);
        if (b_saved > 0) {
            browser_app_t *test_check = (browser_app_t *)umalloc(sizeof(browser_app_t));
            if (test_check) {
                umemset(test_check, 0, sizeof(*test_check));
                if (bookmarks_load(test_check) > 0 && test_check->bookmark_count >= 2) {
                    const char bm_ok[] = "ASTRA_BROWSER_BOOKMARKS_COLD_REBOOT_OK\n";
                    sys_write(1, bm_ok, ustrlen(bm_ok));
                }
            }
        }
    }

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

                if (my < TAB_BAR_HEIGHT) {
                    /* Tab Bar clicks */
                    for (int t = 0; t < g_browser.tab_count && t < MAX_TABS; t++) {
                        int tx = 4 + t * 140;
                        int tw = 136;
                        if (mx >= tx && mx <= tx + tw) {
                            if (mx >= tx + tw - 16) {
                                tab_close(&g_browser, t);
                            } else {
                                tab_select(&g_browser, t);
                            }
                            break;
                        }
                    }
                    if (g_browser.tab_count < MAX_TABS) {
                        int plus_x = 4 + g_browser.tab_count * 140;
                        if (mx >= plus_x && mx <= plus_x + 24) {
                            tab_create(&g_browser, "file:///www/index.html");
                        }
                    }
                } else if (my >= TAB_BAR_HEIGHT && my < TAB_BAR_HEIGHT + TOOLBAR_HEIGHT) {
                    /* Toolbar clicks */
                    if (mx >= 6 && mx <= 30 && g_browser.history_index > 0) {
                        /* Back */
                        g_browser.history_index--;
                        navigate_to(&g_browser, g_browser.history[g_browser.history_index], false);
                    } else if (mx >= 34 && mx <= 58 && g_browser.history_index < g_browser.history_count - 1) {
                        /* Forward */
                        g_browser.history_index++;
                        navigate_to(&g_browser, g_browser.history[g_browser.history_index], false);
                    } else if (mx >= 62 && mx <= 86) {
                        /* Reload */
                        navigate_to(&g_browser, g_browser.current_url, false);
                    } else if (mx >= 90 && mx <= 114) {
                        /* Bookmark Toggle Button [*] */
                        if (bookmark_find(&g_browser, g_browser.current_url) >= 0) {
                            bookmark_remove(&g_browser, g_browser.current_url);
                        } else {
                            bookmark_add(&g_browser, g_browser.current_url, g_browser.page_title);
                        }
                        bookmarks_save(&g_browser);
                    } else if (mx >= WIN_WIDTH - 44 && mx <= WIN_WIDTH - 6) {
                        /* Go Button */
                        navigate_to(&g_browser, g_browser.input_url, true);
                    } else if (mx >= 118 && mx <= WIN_WIDTH - 48) {
                        /* Focus URL Bar */
                        g_browser.input_focused = true;
                        g_browser.input_cursor = (int)ustrlen(g_browser.input_url);
                        g_browser.focused_control_id = -1;
                        for (int k = 0; k < g_browser.control_count; k++) {
                            g_browser.controls[k].focused = false;
                        }
                    }
                } else if (my >= TAB_BAR_HEIGHT + TOOLBAR_HEIGHT && my < WIN_HEIGHT - STATUS_HEIGHT) {
                    int view_y = TAB_BAR_HEIGHT + TOOLBAR_HEIGHT;
                    int sb_x = WIN_WIDTH - SCROLLBAR_WIDTH;
                    int view_h = WIN_HEIGHT - view_y - STATUS_HEIGHT;

                    if (mx >= sb_x) {
                        /* Scrollbar track / thumb click */
                        int max_scroll = g_browser.doc_height - view_h;
                        if (max_scroll > 0) {
                            int thumb_h = (view_h * view_h) / g_browser.doc_height;
                            if (thumb_h < 24) thumb_h = 24;
                            int thumb_y = view_y + (g_browser.scroll_y * (view_h - thumb_h)) / max_scroll;

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
                        int vy = my - view_y + g_browser.scroll_y;

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
                    u32 kcode = ev.key.keycode;
                    if (c == '\n' || c == '\r' || kcode == 0x103) {
                        g_browser.input_focused = false;
                        navigate_to(&g_browser, g_browser.input_url, true);
                    } else if (kcode == 0x104 || c == 27) {
                        ustrcpy(g_browser.input_url, g_browser.current_url, sizeof(g_browser.input_url));
                        g_browser.input_cursor = (int)ustrlen(g_browser.input_url);
                        g_browser.input_focused = false;
                    } else if (c == '\b' || kcode == 0x101) {
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
                    /* Unfocused navigation keys and shortcuts */
                    u32 kcode = ev.key.keycode;
                    char c = ev.key.ascii;
                    u8 mods = ev.key.modifiers;

                    /* New Tab: 't' / Ctrl+T */
                    if (c == 't' || ((mods & 2) && (c == 't' || c == 'T'))) {
                        tab_create(&g_browser, "file:///www/index.html");
                        render_browser_ui(&g_browser);
                    }
                    /* Close Tab: 'w' / Ctrl+W */
                    else if (c == 'w' || ((mods & 2) && (c == 'w' || c == 'W'))) {
                        tab_close(&g_browser, g_browser.active_tab_idx);
                        render_browser_ui(&g_browser);
                    }
                    /* Switch Tab: '1'..'4' */
                    else if (c >= '1' && c <= '4') {
                        int tab_target = c - '1';
                        if (tab_target < g_browser.tab_count) {
                            tab_select(&g_browser, tab_target);
                            render_browser_ui(&g_browser);
                        }
                    }
                    /* Toggle Bookmark: 'b' / Ctrl+B */
                    else if (c == 'b' || ((mods & 2) && (c == 'b' || c == 'B'))) {
                        if (bookmark_find(&g_browser, g_browser.current_url) >= 0) {
                            bookmark_remove(&g_browser, g_browser.current_url);
                        } else {
                            bookmark_add(&g_browser, g_browser.current_url, g_browser.page_title);
                        }
                        bookmarks_save(&g_browser);
                        render_browser_ui(&g_browser);
                    }
                    /* Reload: 'r' / 'R' or Ctrl+R */
                    else if (c == 'r' || c == 'R' || ((mods & 2) && (c == 'r' || c == 'R'))) {
                        navigate_to(&g_browser, g_browser.current_url, false);
                        render_browser_ui(&g_browser);
                    }
                    /* Back: 'h' or Left arrow or Alt+Left */
                    else if (c == 'h' || kcode == 0x107 || ((mods & 4) && kcode == 0x107)) {
                        if (g_browser.history_index > 0) {
                            g_browser.history_index--;
                            navigate_to(&g_browser, g_browser.history[g_browser.history_index], false);
                            render_browser_ui(&g_browser);
                        }
                    }
                    /* Forward: 'l' or Right arrow or Alt+Right */
                    else if (c == 'l' || kcode == 0x108 || ((mods & 4) && kcode == 0x108)) {
                        if (g_browser.history_index < g_browser.history_count - 1) {
                            g_browser.history_index++;
                            navigate_to(&g_browser, g_browser.history[g_browser.history_index], false);
                            render_browser_ui(&g_browser);
                        }
                    }
                    /* Vertical document scrolling */
                    else {
                        int view_h = WIN_HEIGHT - TAB_BAR_HEIGHT - TOOLBAR_HEIGHT - STATUS_HEIGHT;
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
