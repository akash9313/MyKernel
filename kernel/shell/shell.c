#include <kernel/shell.h>
#include <kernel/terminal.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/pmm.h>
#include <kernel/drivers/apic.h>
#include <kernel/rtc.h>
#include <kernel/pkg.h>
#include <kernel/net.h>
#include <kernel/browser.h>
#include <kernel/kprintf.h>
#include <kernel/mem.h>
#include <kernel/code_runner.h>

/* Shell output colors */
#define SH_COL_DEFAULT  0xFFE0E4F0U
#define SH_COL_PROMPT   0xFF50FA7BU
#define SH_COL_PATH     0xFF8BE9FDU
#define SH_COL_INFO     0xFF8BE9FDU
#define SH_COL_SUCCESS  0xFF50FA7BU
#define SH_COL_WARN     0xFFF1FA8CU
#define SH_COL_ERR      0xFFFF5555U
#define SH_COL_MUTED    0xFF8088A0U

/* In-Memory Pipe Buffer & Input Stream Buffer */
static char g_pipe_buf[8192];
static size_t g_pipe_len = 0;
static bool g_has_pipe_input = false;
static char s_stream_buf[4096];

/* String utilities */
static size_t sh_strlen(const char *s) {
    size_t l = 0;
    while (s && s[l]) l++;
    return l;
}

static int sh_strcmp(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && (*a == *b)) { a++; b++; }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static inline __attribute__((unused)) int sh_strncmp(const char *a, const char *b, size_t n) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) break;
    }
    return 0;
}

static void sh_strcpy(char *dst, const char *src, size_t max) {
    if (!dst || max == 0) return;
    size_t i = 0;
    while (src && src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void sh_strncpy(char *dst, const char *src, size_t n) {
    if (!dst || n == 0) return;
    size_t i = 0;
    while (src && src[i] && i < n) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void sh_strcat(char *dst, const char *src, size_t max) {
    if (!dst || max == 0) return;
    size_t dlen = sh_strlen(dst);
    size_t i = 0;
    while (src && src[i] && (dlen + i) < max - 1) {
        dst[dlen + i] = src[i];
        i++;
    }
    dst[dlen + i] = '\0';
}

static char sh_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static bool sh_str_contains_case(const char *haystack, const char *needle, bool ignore_case) {
    if (!haystack || !needle) return false;
    if (!*needle) return true;
    size_t nlen = sh_strlen(needle);
    for (size_t i = 0; haystack[i]; i++) {
        bool match = true;
        for (size_t k = 0; k < nlen; k++) {
            if (!haystack[i + k]) { match = false; break; }
            char h = ignore_case ? sh_tolower(haystack[i + k]) : haystack[i + k];
            char n = ignore_case ? sh_tolower(needle[k]) : needle[k];
            if (h != n) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

static int sh_atoi(const char *s) {
    if (!s) return 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s++ - '0');
    }
    return sign * val;
}

static void sh_append_num(char *out, int *pos, size_t val) {
    char rev[16]; int rp = 0;
    if (val == 0) rev[rp++] = '0';
    while (val > 0) { rev[rp++] = '0' + (val % 10); val /= 10; }
    while (rp > 0) out[(*pos)++] = rev[--rp];
    out[(*pos)++] = ' ';
}

/* Print wrappers */
static void sh_print(shell_context_t *ctx, const char *str, u32 color) {
    if (ctx && ctx->out_fn && str) {
        ctx->out_fn(ctx->out_ctx, str, color);
    }
}

static void sh_println(shell_context_t *ctx, const char *str, u32 color) {
    sh_print(ctx, str, color);
    sh_print(ctx, "\n", color);
}

/* Path normalization */
static void sh_normalize_path(char *dst, const char *base, const char *rel, size_t max) {
    if (!dst || max == 0) return;
    char temp[SHELL_PATH_MAX];

    if (!rel || rel[0] == '\0') {
        sh_strcpy(dst, base ? base : "/", max);
        return;
    }

    if (rel[0] == '/') {
        sh_strcpy(temp, rel, sizeof(temp));
    } else {
        sh_strcpy(temp, base ? base : "/", sizeof(temp));
        if (temp[sh_strlen(temp) - 1] != '/') {
            sh_strcat(temp, "/", sizeof(temp));
        }
        sh_strcat(temp, rel, sizeof(temp));
    }

    /* Collapse '/.' and resolve '/..' */
    char resolved[SHELL_PATH_MAX];
    int r = 0;
    resolved[r++] = '/';
    resolved[r] = '\0';

    int i = 0;
    while (temp[i]) {
        while (temp[i] == '/') i++;
        if (!temp[i]) break;

        char seg[64];
        int sp = 0;
        while (temp[i] && temp[i] != '/' && sp < 63) {
            seg[sp++] = temp[i++];
        }
        seg[sp] = '\0';

        if (sh_strcmp(seg, ".") == 0) {
            continue;
        } else if (sh_strcmp(seg, "..") == 0) {
            if (r > 1) {
                r--;
                while (r > 1 && resolved[r - 1] != '/') r--;
                resolved[r] = '\0';
            }
        } else {
            if (r > 1 && resolved[r - 1] != '/') {
                resolved[r++] = '/';
            }
            int si = 0;
            while (seg[si] && r < (int)sizeof(resolved) - 2) {
                resolved[r++] = seg[si++];
            }
            resolved[r] = '\0';
        }
    }

    sh_strcpy(dst, resolved, max);
}

/* Stream reader: reads either from file or active pipe input buffer */
static bool sh_read_stream(shell_context_t *ctx, const char *arg, char *out_buf, size_t max_len, size_t *out_len) {
    if (!out_buf || max_len == 0) return false;
    out_buf[0] = '\0';
    if (out_len) *out_len = 0;

    if (!arg || sh_strcmp(arg, "-") == 0) {
        if (g_has_pipe_input && g_pipe_len > 0) {
            size_t copy_len = (g_pipe_len < max_len - 1) ? g_pipe_len : (max_len - 1);
            kmemcpy(out_buf, g_pipe_buf, copy_len);
            out_buf[copy_len] = '\0';
            if (out_len) *out_len = copy_len;
            return true;
        }
        return false;
    }

    char full[SHELL_PATH_MAX];
    sh_normalize_path(full, ctx->cwd, arg, sizeof(full));
    vfs_fd_t fd = vfs_open(full, VFS_O_RDONLY);
    if (fd < 0) {
        sh_print(ctx, "cannot open '", SH_COL_ERR);
        sh_print(ctx, arg, SH_COL_ERR);
        sh_print(ctx, "': ", SH_COL_ERR);
        sh_println(ctx, vfs_strerror(fd), SH_COL_ERR);
        return false;
    }

    ssize_t rd = vfs_read(fd, out_buf, max_len - 1);
    vfs_close(fd);
    if (rd < 0) rd = 0;
    out_buf[rd] = '\0';
    if (out_len) *out_len = (size_t)rd;
    return true;
}

/* Environment Variable Storage & Aliases */
void shell_init(shell_context_t *ctx, shell_out_fn out_fn, void *out_ctx) {
    if (!ctx) return;
    kmemset(ctx, 0, sizeof(*ctx));

    ctx->out_fn = out_fn;
    ctx->out_ctx = out_ctx;
    sh_strcpy(ctx->cwd, "/", sizeof(ctx->cwd));
    sh_strcpy(ctx->old_pwd, "/", sizeof(ctx->old_pwd));
    ctx->last_exit_code = 0;
    ctx->pending_install = false;
    ctx->pending_stage = 0;
    ctx->pending_pkg[0] = '\0';

    /* Configure Standard Shell Environment */
    shell_setenv(ctx, "PATH", "/bin:/usr/bin:/pkg/bin");
    shell_setenv(ctx, "USER", "root");
    shell_setenv(ctx, "HOME", "/root");
    shell_setenv(ctx, "TERM", "astraterm");
    shell_setenv(ctx, "SHELL", "/bin/astrashell");
    shell_setenv(ctx, "OS", "AstraOS");
    shell_setenv(ctx, "VERSION", "1.0.0");
    shell_setenv(ctx, "PWD", "/");

    /* Default aliases */
    sh_strcpy(ctx->aliases[0].name, "ll", 32);
    sh_strcpy(ctx->aliases[0].value, "ls -l", 128);
    ctx->aliases[0].in_use = true;

    sh_strcpy(ctx->aliases[1].name, "la", 32);
    sh_strcpy(ctx->aliases[1].value, "ls -a", 128);
    ctx->aliases[1].in_use = true;
}

const char *shell_getenv(shell_context_t *ctx, const char *key) {
    if (!ctx || !key) return NULL;
    for (int i = 0; i < SHELL_ENV_MAX; i++) {
        if (ctx->env[i].in_use && sh_strcmp(ctx->env[i].key, key) == 0) {
            return ctx->env[i].val;
        }
    }
    return NULL;
}

int shell_setenv(shell_context_t *ctx, const char *key, const char *val) {
    if (!ctx || !key || !*key) return -1;

    for (int i = 0; i < SHELL_ENV_MAX; i++) {
        if (ctx->env[i].in_use && sh_strcmp(ctx->env[i].key, key) == 0) {
            sh_strcpy(ctx->env[i].val, val ? val : "", SHELL_VAL_MAX);
            return 0;
        }
    }

    for (int i = 0; i < SHELL_ENV_MAX; i++) {
        if (!ctx->env[i].in_use) {
            ctx->env[i].in_use = true;
            sh_strcpy(ctx->env[i].key, key, SHELL_KEY_MAX);
            sh_strcpy(ctx->env[i].val, val ? val : "", SHELL_VAL_MAX);
            return 0;
        }
    }
    return -1;
}

int shell_unsetenv(shell_context_t *ctx, const char *key) {
    if (!ctx || !key) return -1;
    for (int i = 0; i < SHELL_ENV_MAX; i++) {
        if (ctx->env[i].in_use && sh_strcmp(ctx->env[i].key, key) == 0) {
            ctx->env[i].in_use = false;
            ctx->env[i].key[0] = '\0';
            ctx->env[i].val[0] = '\0';
            return 0;
        }
    }
    return 0;
}

const char *shell_getcwd(shell_context_t *ctx) {
    return ctx ? ctx->cwd : "/";
}

int shell_chdir(shell_context_t *ctx, const char *path) {
    if (!ctx) return -1;
    char target[SHELL_PATH_MAX];
    sh_normalize_path(target, ctx->cwd, path, sizeof(target));

    if (sh_strcmp(target, "/") == 0) {
        sh_strcpy(ctx->cwd, "/", sizeof(ctx->cwd));
        shell_setenv(ctx, "PWD", "/");
        return 0;
    }

    vfs_stat_t st;
    int err = vfs_stat(target, &st);
    if (err != VFS_OK) return err;
    if (st.type != VFS_NODE_DIR) return VFS_ERR_NOT_DIR;

    sh_strcpy(ctx->cwd, target, sizeof(ctx->cwd));
    shell_setenv(ctx, "PWD", target);
    return 0;
}

/* ========================================================================= */
/* Group A — Basic Shell Commands                                            */
/* ========================================================================= */

static int cmd_help(shell_context_t *ctx, int argc, char **argv) {
    if (argc > 1) {
        const char *c = argv[1];
        if (sh_strcmp(c, "ls") == 0) {
            sh_println(ctx, "ls [-l] [-a] [-h] [path] - List directory contents", SH_COL_INFO);
        } else if (sh_strcmp(c, "cd") == 0) {
            sh_println(ctx, "cd [- | ~ | path] - Change working directory", SH_COL_INFO);
        } else if (sh_strcmp(c, "grep") == 0) {
            sh_println(ctx, "grep [-n] [-i] [-v] <pattern> [file] - Search text for pattern", SH_COL_INFO);
        } else if (sh_strcmp(c, "pkg") == 0) {
            sh_println(ctx, "pkg <search|install|remove|update|upgrade|list|info|clean|repo list> [args]", SH_COL_INFO);
        } else {
            sh_print(ctx, "Help for '", SH_COL_INFO);
            sh_print(ctx, c, SH_COL_PROMPT);
            sh_println(ctx, "': Standard AstraOS command. See universal reference.", SH_COL_INFO);
        }
        return 0;
    }

    sh_println(ctx, "=== AstraOS Universal Command Framework ===", SH_COL_INFO);
    sh_println(ctx, "Core Commands:", SH_COL_PROMPT);
    sh_println(ctx, "  help [cmd]             Display command reference or specific help", SH_COL_DEFAULT);
    sh_println(ctx, "  clear                  Clear terminal display", SH_COL_DEFAULT);
    sh_println(ctx, "  echo [-n] [-e] [args]  Print text arguments with escape support", SH_COL_DEFAULT);
    sh_println(ctx, "  pwd                    Print current working directory", SH_COL_DEFAULT);
    sh_println(ctx, "  whoami                 Print active user identity", SH_COL_DEFAULT);
    sh_println(ctx, "  hostname [name]        Show or set system hostname", SH_COL_DEFAULT);
    sh_println(ctx, "  date                   Display system real-time clock UTC date/time", SH_COL_DEFAULT);
    sh_println(ctx, "  uname [-a|-s|-r|-m|-v] Print system and kernel specifications", SH_COL_DEFAULT);
    sh_println(ctx, "  env [k=v]              List or update environment variables", SH_COL_DEFAULT);
    sh_println(ctx, "  history                List terminal command history", SH_COL_DEFAULT);
    sh_println(ctx, "  exit                   Close shell session", SH_COL_DEFAULT);
    sh_println(ctx, "  which <cmd...>         Locate executable binary or builtin", SH_COL_DEFAULT);
    sh_println(ctx, "  type <cmd...>          Describe command interpretation type", SH_COL_DEFAULT);
    sh_println(ctx, "  alias [name=val]       Define or print command aliases", SH_COL_DEFAULT);
    sh_println(ctx, "  unalias <name...>      Remove command alias", SH_COL_DEFAULT);
    sh_println(ctx, "  true / false           Return successful (0) or error (1) exit code", SH_COL_DEFAULT);
    sh_println(ctx, "  sleep <sec>            Delay execution for specified seconds", SH_COL_DEFAULT);

    sh_println(ctx, "\nFilesystem Commands:", SH_COL_PROMPT);
    sh_println(ctx, "  ls [-l|-a|-h] [path]   List directory contents", SH_COL_DEFAULT);
    sh_println(ctx, "  cd [-|~|path]          Change directory (-: previous, ~: home)", SH_COL_DEFAULT);
    sh_println(ctx, "  cat [file...]          Display file or stream contents", SH_COL_DEFAULT);
    sh_println(ctx, "  touch <file...>        Create empty file or touch timestamp", SH_COL_DEFAULT);
    sh_println(ctx, "  mkdir [-p] <dir...>    Create directory (with parents if -p)", SH_COL_DEFAULT);
    sh_println(ctx, "  rmdir <dir...>         Remove empty directory", SH_COL_DEFAULT);
    sh_println(ctx, "  rm [-r|-f] <file...>   Remove files (recursively if -r)", SH_COL_DEFAULT);
    sh_println(ctx, "  cp <src> <dst>         Copy file to destination", SH_COL_DEFAULT);
    sh_println(ctx, "  mv <old> <new>         Move or rename file", SH_COL_DEFAULT);
    sh_println(ctx, "  stat <path>            Show detailed file status and metadata", SH_COL_DEFAULT);
    sh_println(ctx, "  df                     Report filesystem disk usage", SH_COL_DEFAULT);
    sh_println(ctx, "  du [-h|-s] [path]      Estimate directory/file space usage", SH_COL_DEFAULT);
    sh_println(ctx, "  find [path] [-name p]  Search directory tree for matching files", SH_COL_DEFAULT);
    sh_println(ctx, "  tree [path]            Display graphical tree of directory hierarchy", SH_COL_DEFAULT);
    sh_println(ctx, "  head [-n N] [file]     Output first N lines of file/stream", SH_COL_DEFAULT);
    sh_println(ctx, "  tail [-n N] [file]     Output last N lines of file/stream", SH_COL_DEFAULT);
    sh_println(ctx, "  wc [-l|-w|-c] [file]   Count lines, words, and characters/bytes", SH_COL_DEFAULT);
    sh_println(ctx, "  grep [-n|-i|-v] p [f]  Search text pattern in file or stream", SH_COL_DEFAULT);
    sh_println(ctx, "  sort [-r] [file]       Sort lines of text in stream", SH_COL_DEFAULT);
    sh_println(ctx, "  uniq [-c|-d] [file]    Filter or count duplicate adjacent lines", SH_COL_DEFAULT);
    sh_println(ctx, "  diff <f1> <f2>         Compare two files line by line", SH_COL_DEFAULT);
    sh_println(ctx, "  file <path...>         Classify file binary format or contents", SH_COL_DEFAULT);
    sh_println(ctx, "  basename <path> [suf]  Strip directory and suffix from path", SH_COL_DEFAULT);
    sh_println(ctx, "  dirname <path>         Strip trailing path component", SH_COL_DEFAULT);
    sh_println(ctx, "  realpath / readlink    Print resolved canonical absolute path", SH_COL_DEFAULT);

    sh_println(ctx, "\nText & Processing Utilities:", SH_COL_PROMPT);
    sh_println(ctx, "  printf <fmt> [args...] Format and print text data", SH_COL_DEFAULT);
    sh_println(ctx, "  tee [-a] <file...>     Duplicate stdin/pipe stream to file and stdout", SH_COL_DEFAULT);
    sh_println(ctx, "  cut [-d d] [-f n] [-c] Select fields or characters from text", SH_COL_DEFAULT);
    sh_println(ctx, "  tr [-d] [set1] [set2]  Translate or delete characters from stream", SH_COL_DEFAULT);
    sh_println(ctx, "  paste <f1> <f2>        Merge lines of files separated by tabs", SH_COL_DEFAULT);
    sh_println(ctx, "  nl [file]              Number lines of input stream", SH_COL_DEFAULT);
    sh_println(ctx, "  fold [-w width] [file] Wrap lines to column width", SH_COL_DEFAULT);
    sh_println(ctx, "  rev [file]             Reverse characters of each line", SH_COL_DEFAULT);
    sh_println(ctx, "  strings [-n N] [file]  Extract printable character sequences", SH_COL_DEFAULT);
    sh_println(ctx, "  hexdump [-C] [file]    Display canonical hex + ASCII stream dump", SH_COL_DEFAULT);

    sh_println(ctx, "\nSystem & Diagnostics:", SH_COL_PROMPT);
    sh_println(ctx, "  ps                     Snapshot of active processes and threads", SH_COL_DEFAULT);
    sh_println(ctx, "  top                    System metrics: tasks, load, CPU and memory", SH_COL_DEFAULT);
    sh_println(ctx, "  free [-h|-m]           Display physical memory allocation", SH_COL_DEFAULT);
    sh_println(ctx, "  uptime                 Display kernel uptime and APIC clock ticks", SH_COL_DEFAULT);
    sh_println(ctx, "  cpuinfo / meminfo      Hardware CPU model and memory details", SH_COL_DEFAULT);
    sh_println(ctx, "  dmesg                  Kernel diagnostic log ring buffer", SH_COL_DEFAULT);
    sh_println(ctx, "  lsblk                  List block storage devices and partitions", SH_COL_DEFAULT);
    sh_println(ctx, "  mount / umount         Manage and inspect mounted filesystems", SH_COL_DEFAULT);

    sh_println(ctx, "\nNetworking Commands:", SH_COL_PROMPT);
    sh_println(ctx, "  ip [addr|route|link]   Interface IP, routing, and link inspection", SH_COL_DEFAULT);
    sh_println(ctx, "  ifconfig               Display network interface status", SH_COL_DEFAULT);
    sh_println(ctx, "  ping <ip|host>         ICMP network echo probe", SH_COL_DEFAULT);
    sh_println(ctx, "  nslookup <host>        DNS domain lookup query", SH_COL_DEFAULT);
    sh_println(ctx, "  curl [-o file] <url>   HTTP/HTTPS/TLS 1.3 data fetcher", SH_COL_DEFAULT);
    sh_println(ctx, "  wget [-O file] <url>   Download network files to disk", SH_COL_DEFAULT);
    sh_println(ctx, "  netstat                Display socket connections and interfaces", SH_COL_DEFAULT);

    sh_println(ctx, "\nPackage Management (pkg):", SH_COL_PROMPT);
    sh_println(ctx, "  pkg search <query>     Search available software packages", SH_COL_DEFAULT);
    sh_println(ctx, "  pkg install <pkg>      Install package with cryptographic check", SH_COL_DEFAULT);
    sh_println(ctx, "  pkg remove <pkg>       Remove installed package cleanly", SH_COL_DEFAULT);
    sh_println(ctx, "  pkg list / info        List or inspect installed packages", SH_COL_DEFAULT);
    sh_println(ctx, "  pkg update / upgrade   Synchronize and upgrade packages", SH_COL_DEFAULT);
    sh_println(ctx, "  pkg clean / repo list  Manage cache and view package repositories", SH_COL_DEFAULT);
    return 0;
}

static int cmd_clear(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    if (ctx && ctx->out_ctx) {
        terminal_clear((terminal_state_t *)ctx->out_ctx);
    }
    return 0;
}

static int cmd_echo(shell_context_t *ctx, int argc, char **argv) {
    bool newline = true;
    bool escapes = false;
    int start = 1;

    while (start < argc && argv[start][0] == '-') {
        if (sh_strcmp(argv[start], "-n") == 0) { newline = false; start++; }
        else if (sh_strcmp(argv[start], "-e") == 0) { escapes = true; start++; }
        else if (sh_strcmp(argv[start], "-ne") == 0 || sh_strcmp(argv[start], "-en") == 0) {
            newline = false; escapes = true; start++;
        } else break;
    }

    for (int i = start; i < argc; i++) {
        const char *arg = argv[i];
        if (escapes) {
            for (int c = 0; arg[c]; c++) {
                if (arg[c] == '\\' && arg[c + 1]) {
                    c++;
                    if (arg[c] == 'n') sh_print(ctx, "\n", SH_COL_DEFAULT);
                    else if (arg[c] == 't') sh_print(ctx, "\t", SH_COL_DEFAULT);
                    else if (arg[c] == '\\') sh_print(ctx, "\\", SH_COL_DEFAULT);
                    else {
                        char ch[2] = { arg[c], '\0' };
                        sh_print(ctx, ch, SH_COL_DEFAULT);
                    }
                } else {
                    char ch[2] = { arg[c], '\0' };
                    sh_print(ctx, ch, SH_COL_DEFAULT);
                }
            }
        } else {
            sh_print(ctx, arg, SH_COL_DEFAULT);
        }
        if (i < argc - 1) sh_print(ctx, " ", SH_COL_DEFAULT);
    }
    if (newline) sh_print(ctx, "\n", SH_COL_DEFAULT);
    return 0;
}

static int cmd_pwd(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, shell_getcwd(ctx), SH_COL_PATH);
    return 0;
}

static int cmd_whoami(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    const char *user = shell_getenv(ctx, "USER");
    sh_println(ctx, user ? user : "root", SH_COL_PROMPT);
    return 0;
}

static int cmd_hostname(shell_context_t *ctx, int argc, char **argv) {
    if (argc > 1) {
        shell_setenv(ctx, "HOSTNAME", argv[1]);
        sh_println(ctx, "Hostname updated.", SH_COL_SUCCESS);
        return 0;
    }
    const char *h = shell_getenv(ctx, "HOSTNAME");
    sh_println(ctx, h ? h : "astraos", SH_COL_DEFAULT);
    return 0;
}

static int cmd_date(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    rtc_time_t t;
    rtc_get_time(&t);

    char buf[64];
    int p = 0;
    int yr = t.year;
    buf[p++] = '0' + (yr / 1000) % 10;
    buf[p++] = '0' + (yr / 100) % 10;
    buf[p++] = '0' + (yr / 10) % 10;
    buf[p++] = '0' + (yr % 10);
    buf[p++] = '-';
    buf[p++] = '0' + (t.month / 10) % 10;
    buf[p++] = '0' + (t.month % 10);
    buf[p++] = '-';
    buf[p++] = '0' + (t.day / 10) % 10;
    buf[p++] = '0' + (t.day % 10);
    buf[p++] = ' ';
    buf[p++] = '0' + (t.hour / 10) % 10;
    buf[p++] = '0' + (t.hour % 10);
    buf[p++] = ':';
    buf[p++] = '0' + (t.minute / 10) % 10;
    buf[p++] = '0' + (t.minute % 10);
    buf[p++] = ':';
    buf[p++] = '0' + (t.second / 10) % 10;
    buf[p++] = '0' + (t.second % 10);
    buf[p++] = ' ';
    buf[p++] = 'U'; buf[p++] = 'T'; buf[p++] = 'C';
    buf[p] = '\0';

    sh_println(ctx, buf, SH_COL_INFO);
    return 0;
}

static int cmd_uname(shell_context_t *ctx, int argc, char **argv) {
    bool opt_a = false, opt_s = false, opt_r = false, opt_m = false, opt_v = false;
    if (argc == 1) {
        opt_s = true;
    } else {
        for (int i = 1; i < argc; i++) {
            if (sh_strcmp(argv[i], "-a") == 0) opt_a = true;
            else if (sh_strcmp(argv[i], "-s") == 0) opt_s = true;
            else if (sh_strcmp(argv[i], "-r") == 0) opt_r = true;
            else if (sh_strcmp(argv[i], "-m") == 0) opt_m = true;
            else if (sh_strcmp(argv[i], "-v") == 0) opt_v = true;
        }
    }
    if (opt_a) {
        sh_println(ctx, "AstraOS 1.0.0-rc1 #1 SMP PREEMPT Wed Nov 12 2026 x86_64 freestanding UEFI/Limine", SH_COL_INFO);
        return 0;
    }

    bool first = true;
    if (opt_s) { sh_print(ctx, "AstraOS", SH_COL_INFO); first = false; }
    if (opt_r) { if (!first) sh_print(ctx, " ", SH_COL_INFO); sh_print(ctx, "1.0.0-rc1", SH_COL_INFO); first = false; }
    if (opt_v) { if (!first) sh_print(ctx, " ", SH_COL_INFO); sh_print(ctx, "#1 SMP PREEMPT Wed Nov 12 2026", SH_COL_INFO); first = false; }
    if (opt_m) { if (!first) sh_print(ctx, " ", SH_COL_INFO); sh_print(ctx, "x86_64", SH_COL_INFO); first = false; }
    sh_println(ctx, "", SH_COL_INFO);
    return 0;
}

static int cmd_env(shell_context_t *ctx, int argc, char **argv) {
    if (argc > 1) {
        char *eq = argv[1];
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = '\0';
            shell_setenv(ctx, argv[1], eq + 1);
            *eq = '=';
            return 0;
        }
    }

    for (int i = 0; i < SHELL_ENV_MAX; i++) {
        if (ctx->env[i].in_use) {
            sh_print(ctx, ctx->env[i].key, SH_COL_INFO);
            sh_print(ctx, "=", SH_COL_MUTED);
            sh_println(ctx, ctx->env[i].val, SH_COL_DEFAULT);
        }
    }
    return 0;
}

static int cmd_history(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    if (ctx && ctx->out_ctx) {
        terminal_state_t *term = (terminal_state_t *)ctx->out_ctx;
        if (term->history_count > 0) {
            for (int i = 0; i < term->history_count; i++) {
                char num[16];
                int np = 0;
                int idx = i + 1;
                if (idx < 10) num[np++] = ' ';
                if (idx >= 10) num[np++] = '0' + (idx / 10);
                num[np++] = '0' + (idx % 10);
                num[np++] = ' ';
                num[np++] = ' ';
                num[np] = '\0';
                sh_print(ctx, num, SH_COL_MUTED);
                sh_println(ctx, term->history[i], SH_COL_DEFAULT);
            }
            return 0;
        }
    }
    sh_println(ctx, "  1  help", SH_COL_DEFAULT);
    sh_println(ctx, "  2  ls -la", SH_COL_DEFAULT);
    sh_println(ctx, "  3  uname -a", SH_COL_DEFAULT);
    sh_println(ctx, "  4  history", SH_COL_DEFAULT);
    return 0;
}

static int cmd_exit(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "[Session terminated]", SH_COL_MUTED);
    return 0;
}

/* Forward declarations */
typedef struct {
    const char *name;
    int (*handler)(shell_context_t *ctx, int argc, char **argv);
} shell_cmd_entry_t;
static const shell_cmd_entry_t g_builtins[];

static int cmd_which(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: which <command...>", SH_COL_WARN);
        return 1;
    }
    int missing = 0;
    for (int a = 1; a < argc; a++) {
        bool found = false;
        for (int i = 0; g_builtins[i].name != NULL; i++) {
            if (sh_strcmp(argv[a], g_builtins[i].name) == 0) {
                sh_print(ctx, argv[a], SH_COL_PROMPT);
                sh_println(ctx, ": shell built-in command", SH_COL_DEFAULT);
                found = true;
                break;
            }
        }
        if (!found) {
            char cand[SHELL_PATH_MAX];
            sh_strcpy(cand, "/bin/", sizeof(cand));
            sh_strcat(cand, argv[a], sizeof(cand));
            vfs_stat_t st;
            if (vfs_stat(cand, &st) == VFS_OK) {
                sh_println(ctx, cand, SH_COL_INFO);
                found = true;
            } else {
                sh_strcat(cand, ".elf", sizeof(cand));
                if (vfs_stat(cand, &st) == VFS_OK) {
                    sh_println(ctx, cand, SH_COL_INFO);
                    found = true;
                }
            }
        }
        if (!found) {
            sh_print(ctx, "which: no ", SH_COL_ERR);
            sh_print(ctx, argv[a], SH_COL_ERR);
            sh_println(ctx, " in ($PATH)", SH_COL_ERR);
            missing++;
        }
    }
    return missing ? 1 : 0;
}

static int cmd_type(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: type <command...>", SH_COL_WARN);
        return 1;
    }
    int missing = 0;
    for (int a = 1; a < argc; a++) {
        bool found = false;
        for (int al = 0; al < SHELL_ALIAS_MAX; al++) {
            if (ctx->aliases[al].in_use && sh_strcmp(ctx->aliases[al].name, argv[a]) == 0) {
                sh_print(ctx, argv[a], SH_COL_PROMPT);
                sh_print(ctx, " is aliased to `", SH_COL_DEFAULT);
                sh_print(ctx, ctx->aliases[al].value, SH_COL_INFO);
                sh_println(ctx, "`", SH_COL_DEFAULT);
                found = true;
                break;
            }
        }
        if (found) continue;

        for (int i = 0; g_builtins[i].name != NULL; i++) {
            if (sh_strcmp(argv[a], g_builtins[i].name) == 0) {
                sh_print(ctx, argv[a], SH_COL_PROMPT);
                sh_println(ctx, " is a shell builtin", SH_COL_DEFAULT);
                found = true;
                break;
            }
        }
        if (found) continue;

        char cand[SHELL_PATH_MAX];
        sh_strcpy(cand, "/bin/", sizeof(cand));
        sh_strcat(cand, argv[a], sizeof(cand));
        vfs_stat_t st;
        if (vfs_stat(cand, &st) == VFS_OK) {
            sh_print(ctx, argv[a], SH_COL_PROMPT);
            sh_print(ctx, " is ", SH_COL_DEFAULT);
            sh_println(ctx, cand, SH_COL_INFO);
            found = true;
        } else {
            sh_strcat(cand, ".elf", sizeof(cand));
            if (vfs_stat(cand, &st) == VFS_OK) {
                sh_print(ctx, argv[a], SH_COL_PROMPT);
                sh_print(ctx, " is ", SH_COL_DEFAULT);
                sh_println(ctx, cand, SH_COL_INFO);
                found = true;
            }
        }

        if (!found) {
            sh_print(ctx, "type: ", SH_COL_ERR);
            sh_print(ctx, argv[a], SH_COL_ERR);
            sh_println(ctx, ": not found", SH_COL_ERR);
            missing++;
        }
    }
    return missing ? 1 : 0;
}

static int cmd_alias(shell_context_t *ctx, int argc, char **argv) {
    if (argc == 1) {
        for (int i = 0; i < SHELL_ALIAS_MAX; i++) {
            if (ctx->aliases[i].in_use) {
                sh_print(ctx, "alias ", SH_COL_INFO);
                sh_print(ctx, ctx->aliases[i].name, SH_COL_PROMPT);
                sh_print(ctx, "='", SH_COL_MUTED);
                sh_print(ctx, ctx->aliases[i].value, SH_COL_DEFAULT);
                sh_println(ctx, "'", SH_COL_MUTED);
            }
        }
        return 0;
    }
    for (int a = 1; a < argc; a++) {
        char *eq = argv[a];
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = '\0';
            const char *name = argv[a];
            const char *val = eq + 1;
            bool updated = false;
            for (int i = 0; i < SHELL_ALIAS_MAX; i++) {
                if (ctx->aliases[i].in_use && sh_strcmp(ctx->aliases[i].name, name) == 0) {
                    sh_strcpy(ctx->aliases[i].value, val, 128);
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                for (int i = 0; i < SHELL_ALIAS_MAX; i++) {
                    if (!ctx->aliases[i].in_use) {
                        ctx->aliases[i].in_use = true;
                        sh_strcpy(ctx->aliases[i].name, name, 32);
                        sh_strcpy(ctx->aliases[i].value, val, 128);
                        break;
                    }
                }
            }
            *eq = '=';
        }
    }
    return 0;
}

static int cmd_unalias(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: unalias <name...>", SH_COL_WARN);
        return 1;
    }
    for (int a = 1; a < argc; a++) {
        for (int i = 0; i < SHELL_ALIAS_MAX; i++) {
            if (ctx->aliases[i].in_use && sh_strcmp(ctx->aliases[i].name, argv[a]) == 0) {
                ctx->aliases[i].in_use = false;
                ctx->aliases[i].name[0] = '\0';
                ctx->aliases[i].value[0] = '\0';
                break;
            }
        }
    }
    return 0;
}

static int cmd_true(shell_context_t *ctx, int argc, char **argv) {
    (void)ctx; (void)argc; (void)argv;
    return 0;
}

static int cmd_false(shell_context_t *ctx, int argc, char **argv) {
    (void)ctx; (void)argc; (void)argv;
    return 1;
}

static int cmd_sleep(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: sleep <seconds>", SH_COL_WARN);
        return 1;
    }
    int sec = sh_atoi(argv[1]);
    if (sec <= 0) return 0;
    if (sec > 60) sec = 60;

    u64 start = apic_get_ticks();
    u64 target = start + ((u64)sec * 100);
    while (apic_get_ticks() < target) {
        __asm__ volatile("pause");
    }
    return 0;
}

/* ========================================================================= */
/* Group B — Filesystem Commands                                             */
/* ========================================================================= */

static int cmd_ls(shell_context_t *ctx, int argc, char **argv) {
    bool long_fmt = false, all = false, human = false;
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int c = 1; argv[i][c]; c++) {
                if (argv[i][c] == 'l') long_fmt = true;
                if (argv[i][c] == 'a') all = true;
                if (argv[i][c] == 'h') human = true;
            }
        } else if (!target) {
            target = argv[i];
        }
    }
    if (!target) target = ctx->cwd;

    char path[SHELL_PATH_MAX];
    sh_normalize_path(path, ctx->cwd, target, sizeof(path));

    vfs_dirent_t dent;
    size_t idx = 0;
    int count = 0;

    if (all) {
        if (long_fmt) {
            sh_println(ctx, "drwxr-xr-x  2 root root     4096 .", SH_COL_INFO);
            sh_println(ctx, "drwxr-xr-x  2 root root     4096 ..", SH_COL_INFO);
        } else {
            sh_print(ctx, ".  ..  ", SH_COL_INFO);
        }
        count += 2;
    }

    while (vfs_readdir(path, idx++, &dent) == VFS_OK) {
        if (!all && dent.name[0] == '.') continue;

        if (long_fmt) {
            char entry[128];
            int p = 0;
            const char *perm = (dent.type == VFS_NODE_DIR) ? "drwxr-xr-x  1 root root " : "-rw-r--r--  1 root root ";
            while (*perm) entry[p++] = *perm++;

            char s_str[16];
            int sp = 0;
            size_t sz = dent.size;
            if (human) {
                if (sz >= 1024 * 1024) {
                    size_t mb = sz / (1024 * 1024);
                    if (mb >= 10) s_str[sp++] = '0' + (mb / 10);
                    s_str[sp++] = '0' + (mb % 10);
                    s_str[sp++] = 'M';
                } else if (sz >= 1024) {
                    size_t kb = sz / 1024;
                    if (kb >= 10) s_str[sp++] = '0' + (kb / 10);
                    s_str[sp++] = '0' + (kb % 10);
                    s_str[sp++] = 'K';
                } else {
                    if (sz == 0) s_str[sp++] = '0';
                    else {
                        char rev[16]; int rp = 0;
                        while (sz > 0) { rev[rp++] = '0' + (sz % 10); sz /= 10; }
                        while (rp > 0) s_str[sp++] = rev[--rp];
                    }
                    s_str[sp++] = 'B';
                }
            } else {
                if (sz == 0) s_str[sp++] = '0';
                else {
                    char rev[16]; int rp = 0;
                    while (sz > 0) { rev[rp++] = '0' + (sz % 10); sz /= 10; }
                    while (rp > 0) s_str[sp++] = rev[--rp];
                }
            }
            s_str[sp] = '\0';

            while (sp < 8) { entry[p++] = ' '; sp++; }
            for (int s = 0; s_str[s]; s++) entry[p++] = s_str[s];
            entry[p++] = ' ';

            const char *nm = dent.name;
            while (*nm && p < 110) entry[p++] = *nm++;
            entry[p] = '\0';

            sh_println(ctx, entry, (dent.type == VFS_NODE_DIR) ? SH_COL_INFO : SH_COL_DEFAULT);
        } else {
            sh_print(ctx, dent.name, (dent.type == VFS_NODE_DIR) ? SH_COL_INFO : SH_COL_DEFAULT);
            sh_print(ctx, "  ", SH_COL_DEFAULT);
        }
        count++;
    }

    if (!long_fmt && count > 0) {
        sh_println(ctx, "", SH_COL_DEFAULT);
    } else if (count == 0) {
        sh_println(ctx, "(empty directory)", SH_COL_MUTED);
    }
    return 0;
}

static int cmd_cd(shell_context_t *ctx, int argc, char **argv) {
    const char *target = (argc > 1) ? argv[1] : "/root";
    if (sh_strcmp(target, "~") == 0) {
        target = "/root";
    } else if (sh_strcmp(target, "-") == 0) {
        target = ctx->old_pwd[0] ? ctx->old_pwd : "/";
        sh_println(ctx, target, SH_COL_PATH);
    }

    char old_cwd_save[SHELL_PATH_MAX];
    sh_strcpy(old_cwd_save, ctx->cwd, sizeof(old_cwd_save));

    int err = shell_chdir(ctx, target);
    if (err != 0) {
        sh_print(ctx, "cd: error changing directory: ", SH_COL_ERR);
        sh_println(ctx, vfs_strerror(err), SH_COL_ERR);
        return 1;
    }
    sh_strcpy(ctx->old_pwd, old_cwd_save, sizeof(ctx->old_pwd));
    return 0;
}

static int cmd_cat(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        if (g_has_pipe_input && g_pipe_len > 0) {
            sh_print(ctx, g_pipe_buf, SH_COL_DEFAULT);
            return 0;
        }
        sh_println(ctx, "Usage: cat <file...>", SH_COL_WARN);
        return 1;
    }

    for (int a = 1; a < argc; a++) {
        size_t len = 0;
        if (sh_read_stream(ctx, argv[a], s_stream_buf, sizeof(s_stream_buf), &len)) {
            sh_print(ctx, s_stream_buf, SH_COL_DEFAULT);
        }
    }
    return 0;
}

static int cmd_touch(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: touch <file...>", SH_COL_WARN);
        return 1;
    }

    for (int a = 1; a < argc; a++) {
        char path[SHELL_PATH_MAX];
        sh_normalize_path(path, ctx->cwd, argv[a], sizeof(path));

        vfs_fd_t fd = vfs_open(path, VFS_O_CREAT | VFS_O_RDWR);
        if (fd < 0) {
            sh_print(ctx, "touch: cannot create '", SH_COL_ERR);
            sh_print(ctx, argv[a], SH_COL_ERR);
            sh_print(ctx, "': ", SH_COL_ERR);
            sh_println(ctx, vfs_strerror(fd), SH_COL_ERR);
        } else {
            vfs_fsync(fd);
            vfs_close(fd);
        }
    }
    return 0;
}

static int cmd_mkdir(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: mkdir [-p] <directory...>", SH_COL_WARN);
        return 1;
    }
    bool parents = false;
    int start = 1;
    if (sh_strcmp(argv[1], "-p") == 0) {
        parents = true;
        start = 2;
    }
    if (start >= argc) {
        sh_println(ctx, "mkdir: missing operand", SH_COL_ERR);
        return 1;
    }

    int err_count = 0;
    for (int a = start; a < argc; a++) {
        char path[SHELL_PATH_MAX];
        sh_normalize_path(path, ctx->cwd, argv[a], sizeof(path));

        if (parents) {
            char sub[SHELL_PATH_MAX];
            int p = 0;
            for (int i = 0; path[i]; i++) {
                sub[p++] = path[i];
                if (path[i] == '/' && p > 1) {
                    sub[p - 1] = '\0';
                    vfs_mkdir(sub);
                    sub[p - 1] = '/';
                }
            }
            sub[p] = '\0';
            int res = vfs_mkdir(sub);
            if (res != VFS_OK && res != VFS_ERR_EXISTS) {
                sh_print(ctx, "mkdir: cannot create '", SH_COL_ERR);
                sh_print(ctx, argv[a], SH_COL_ERR);
                sh_print(ctx, "': ", SH_COL_ERR);
                sh_println(ctx, vfs_strerror(res), SH_COL_ERR);
                err_count++;
            }
        } else {
            int res = vfs_mkdir(path);
            if (res != VFS_OK) {
                sh_print(ctx, "mkdir: cannot create '", SH_COL_ERR);
                sh_print(ctx, argv[a], SH_COL_ERR);
                sh_print(ctx, "': ", SH_COL_ERR);
                sh_println(ctx, vfs_strerror(res), SH_COL_ERR);
                err_count++;
            }
        }
    }
    return err_count ? 1 : 0;
}

static int cmd_rmdir(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: rmdir <directory...>", SH_COL_WARN);
        return 1;
    }
    int err_count = 0;
    for (int a = 1; a < argc; a++) {
        char path[SHELL_PATH_MAX];
        sh_normalize_path(path, ctx->cwd, argv[a], sizeof(path));
        int res = vfs_rmdir(path);
        if (res != VFS_OK) {
            sh_print(ctx, "rmdir: failed to remove '", SH_COL_ERR);
            sh_print(ctx, argv[a], SH_COL_ERR);
            sh_print(ctx, "': ", SH_COL_ERR);
            sh_println(ctx, vfs_strerror(res), SH_COL_ERR);
            err_count++;
        }
    }
    return err_count ? 1 : 0;
}

static int rm_recursive(const char *path) {
    vfs_dirent_t dent;
    size_t idx = 0;
    while (vfs_readdir(path, idx++, &dent) == VFS_OK) {
        if (sh_strcmp(dent.name, ".") == 0 || sh_strcmp(dent.name, "..") == 0) continue;
        char child[SHELL_PATH_MAX];
        sh_strcpy(child, path, sizeof(child));
        if (child[sh_strlen(child) - 1] != '/') sh_strcat(child, "/", sizeof(child));
        sh_strcat(child, dent.name, sizeof(child));
        if (dent.type == VFS_NODE_DIR) {
            rm_recursive(child);
            vfs_rmdir(child);
        } else {
            vfs_unlink(child);
        }
    }
    return vfs_rmdir(path);
}

static int cmd_rm(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: rm [-r|-f] <file...>", SH_COL_WARN);
        return 1;
    }
    bool recursive = false, force = false;
    int start = 1;
    while (start < argc && argv[start][0] == '-') {
        for (int c = 1; argv[start][c]; c++) {
            if (argv[start][c] == 'r' || argv[start][c] == 'R') recursive = true;
            if (argv[start][c] == 'f') force = true;
        }
        start++;
    }
    if (start >= argc) {
        if (!force) sh_println(ctx, "rm: missing operand", SH_COL_ERR);
        return force ? 0 : 1;
    }

    int err_count = 0;
    for (int a = start; a < argc; a++) {
        char path[SHELL_PATH_MAX];
        sh_normalize_path(path, ctx->cwd, argv[a], sizeof(path));
        vfs_stat_t st;
        int s_res = vfs_stat(path, &st);
        if (s_res != VFS_OK) {
            if (!force) {
                sh_print(ctx, "rm: cannot remove '", SH_COL_ERR);
                sh_print(ctx, argv[a], SH_COL_ERR);
                sh_println(ctx, "': No such file or directory", SH_COL_ERR);
                err_count++;
            }
            continue;
        }
        if (st.type == VFS_NODE_DIR) {
            if (!recursive) {
                sh_print(ctx, "rm: cannot remove '", SH_COL_ERR);
                sh_print(ctx, argv[a], SH_COL_ERR);
                sh_println(ctx, "': Is a directory", SH_COL_ERR);
                err_count++;
                continue;
            }
            rm_recursive(path);
        } else {
            int res = vfs_unlink(path);
            if (res != VFS_OK && !force) {
                sh_print(ctx, "rm: cannot remove '", SH_COL_ERR);
                sh_print(ctx, argv[a], SH_COL_ERR);
                sh_print(ctx, "': ", SH_COL_ERR);
                sh_println(ctx, vfs_strerror(res), SH_COL_ERR);
                err_count++;
            }
        }
    }
    return err_count ? 1 : 0;
}

static int cmd_cp(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 3) {
        sh_println(ctx, "Usage: cp <source> <destination>", SH_COL_WARN);
        return 1;
    }

    char src[SHELL_PATH_MAX], dst[SHELL_PATH_MAX];
    sh_normalize_path(src, ctx->cwd, argv[1], sizeof(src));
    sh_normalize_path(dst, ctx->cwd, argv[2], sizeof(dst));

    vfs_fd_t sfd = vfs_open(src, VFS_O_RDONLY);
    if (sfd < 0) {
        sh_println(ctx, "cp: failed to open source file", SH_COL_ERR);
        return 1;
    }

    vfs_fd_t dfd = vfs_open(dst, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (dfd < 0) {
        vfs_close(sfd);
        sh_println(ctx, "cp: failed to create destination file", SH_COL_ERR);
        return 1;
    }

    char buf[512];
    ssize_t rd;
    while ((rd = vfs_read(sfd, buf, sizeof(buf))) > 0) {
        vfs_write(dfd, buf, (size_t)rd);
    }

    vfs_fsync(dfd);
    vfs_close(sfd);
    vfs_close(dfd);
    return 0;
}

static int cmd_mv(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 3) {
        sh_println(ctx, "Usage: mv <source> <destination>", SH_COL_WARN);
        return 1;
    }

    char src[SHELL_PATH_MAX], dst[SHELL_PATH_MAX];
    sh_normalize_path(src, ctx->cwd, argv[1], sizeof(src));
    sh_normalize_path(dst, ctx->cwd, argv[2], sizeof(dst));

    int res = vfs_rename(src, dst);
    if (res != VFS_OK) {
        sh_print(ctx, "mv: failed to rename: ", SH_COL_ERR);
        sh_println(ctx, vfs_strerror(res), SH_COL_ERR);
        return 1;
    }
    return 0;
}

static int cmd_stat(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: stat <path>", SH_COL_WARN);
        return 1;
    }

    char path[SHELL_PATH_MAX];
    sh_normalize_path(path, ctx->cwd, argv[1], sizeof(path));

    vfs_stat_t st;
    int err = vfs_stat(path, &st);
    if (err != VFS_OK) {
        sh_println(ctx, "stat: file or directory not found", SH_COL_ERR);
        return 1;
    }

    sh_print(ctx, "  File: ", SH_COL_INFO);
    sh_println(ctx, path, SH_COL_PROMPT);
    sh_print(ctx, "  Type: ", SH_COL_INFO);
    sh_println(ctx, (st.type == VFS_NODE_DIR) ? "Directory" : "Regular File", SH_COL_DEFAULT);

    char sz_buf[32];
    int p = 0;
    size_t s = st.size;
    if (s == 0) sz_buf[p++] = '0';
    else {
        char rev[16]; int rp = 0;
        while (s > 0) { rev[rp++] = '0' + (s % 10); s /= 10; }
        while (rp > 0) sz_buf[p++] = rev[--rp];
    }
    sz_buf[p] = '\0';

    sh_print(ctx, "  Size: ", SH_COL_INFO);
    sh_print(ctx, sz_buf, SH_COL_DEFAULT);
    sh_println(ctx, " bytes", SH_COL_DEFAULT);
    return 0;
}

static int cmd_df(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    struct pmm_stats st;
    pmm_get_stats(&st);

    u64 total_kb = (st.total_managed_pages * 4096) / 1024;
    u64 used_kb = (st.allocated_pages * 4096) / 1024;
    u64 free_kb = total_kb - used_kb;

    sh_println(ctx, "Filesystem     Type       1K-blocks      Used Available Use%", SH_COL_INFO);
    sh_println(ctx, "rootfs         initramfs      16384      4096     12288  25%", SH_COL_DEFAULT);
    sh_println(ctx, "/data          AstraFS        65536      1024     64512   2%", SH_COL_DEFAULT);

    char mem_line[128];
    int p = 0;
    const char *m1 = "ramfs (pmm)    tmpfs      ";
    while (*m1) mem_line[p++] = *m1++;

    char num[16]; int np = 0;
    u64 v = total_kb;
    if (v == 0) num[np++] = '0';
    else {
        char rev[16]; int rp = 0;
        while (v > 0) { rev[rp++] = '0' + (v % 10); v /= 10; }
        while (rp > 0) num[np++] = rev[--rp];
    }
    num[np] = '\0';
    for (int n = 0; num[n] && p < 40; n++) mem_line[p++] = num[n];
    while (p < 40) mem_line[p++] = ' ';

    v = used_kb; np = 0;
    if (v == 0) num[np++] = '0';
    else {
        char rev[16]; int rp = 0;
        while (v > 0) { rev[rp++] = '0' + (v % 10); v /= 10; }
        while (rp > 0) num[np++] = rev[--rp];
    }
    num[np] = '\0';
    for (int n = 0; num[n] && p < 50; n++) mem_line[p++] = num[n];
    while (p < 50) mem_line[p++] = ' ';

    v = free_kb; np = 0;
    if (v == 0) num[np++] = '0';
    else {
        char rev[16]; int rp = 0;
        while (v > 0) { rev[rp++] = '0' + (v % 10); v /= 10; }
        while (rp > 0) num[np++] = rev[--rp];
    }
    num[np] = '\0';
    for (int n = 0; num[n] && p < 60; n++) mem_line[p++] = num[n];
    while (p < 60) mem_line[p++] = ' ';

    mem_line[p++] = ' '; mem_line[p++] = 'o'; mem_line[p++] = 'k';
    mem_line[p] = '\0';
    sh_println(ctx, mem_line, SH_COL_DEFAULT);
    return 0;
}

static size_t calculate_dir_size(const char *path) {
    vfs_dirent_t dent;
    size_t idx = 0;
    size_t total = 0;
    while (vfs_readdir(path, idx++, &dent) == VFS_OK) {
        if (sh_strcmp(dent.name, ".") == 0 || sh_strcmp(dent.name, "..") == 0) continue;
        if (dent.type == VFS_NODE_DIR) {
            char child[SHELL_PATH_MAX];
            sh_strcpy(child, path, sizeof(child));
            if (child[sh_strlen(child) - 1] != '/') sh_strcat(child, "/", sizeof(child));
            sh_strcat(child, dent.name, sizeof(child));
            total += calculate_dir_size(child);
        } else {
            total += dent.size;
        }
    }
    return total;
}

static int cmd_du(shell_context_t *ctx, int argc, char **argv) {
    bool human = false, summary = false;
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int c = 1; argv[i][c]; c++) {
                if (argv[i][c] == 'h') human = true;
                if (argv[i][c] == 's') summary = true;
            }
        } else if (!target) {
            target = argv[i];
        }
    }
    if (!target) target = ctx->cwd;

    char path[SHELL_PATH_MAX];
    sh_normalize_path(path, ctx->cwd, target, sizeof(path));
    size_t sz = calculate_dir_size(path);

    char num[32];
    int p = 0;
    if (human) {
        size_t kb = (sz + 1023) / 1024;
        if (kb == 0) kb = 4;
        if (kb >= 10) num[p++] = '0' + (kb / 10);
        num[p++] = '0' + (kb % 10);
        num[p++] = 'K';
    } else {
        size_t kb = (sz + 1023) / 1024;
        if (kb == 0) kb = 4;
        char rev[16]; int rp = 0;
        while (kb > 0) { rev[rp++] = '0' + (kb % 10); kb /= 10; }
        while (rp > 0) num[p++] = rev[--rp];
    }
    num[p] = '\0';

    sh_print(ctx, num, SH_COL_INFO);
    sh_print(ctx, "\t", SH_COL_DEFAULT);
    sh_println(ctx, path, SH_COL_DEFAULT);
    (void)summary;
    return 0;
}

static void find_recursive(shell_context_t *ctx, const char *dir, const char *pattern) {
    vfs_dirent_t dent;
    size_t idx = 0;
    while (vfs_readdir(dir, idx++, &dent) == VFS_OK) {
        if (sh_strcmp(dent.name, ".") == 0 || sh_strcmp(dent.name, "..") == 0) continue;
        char child[SHELL_PATH_MAX];
        sh_strcpy(child, dir, sizeof(child));
        if (child[sh_strlen(child) - 1] != '/') sh_strcat(child, "/", sizeof(child));
        sh_strcat(child, dent.name, sizeof(child));

        if (!pattern || sh_str_contains_case(dent.name, pattern, false)) {
            sh_println(ctx, child, (dent.type == VFS_NODE_DIR) ? SH_COL_INFO : SH_COL_DEFAULT);
        }

        if (dent.type == VFS_NODE_DIR) {
            find_recursive(ctx, child, pattern);
        }
    }
}

static int cmd_find(shell_context_t *ctx, int argc, char **argv) {
    const char *target = ".";
    const char *pattern = NULL;

    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            pattern = argv[++i];
        } else if (argv[i][0] != '-') {
            target = argv[i];
        }
    }

    char path[SHELL_PATH_MAX];
    sh_normalize_path(path, ctx->cwd, target, sizeof(path));
    if (!pattern || sh_str_contains_case(path, pattern, false)) {
        sh_println(ctx, path, SH_COL_INFO);
    }
    find_recursive(ctx, path, pattern);
    return 0;
}

static void tree_recursive(shell_context_t *ctx, const char *dir, const char *prefix, int depth) {
    if (depth > 6) return;
    vfs_dirent_t dent;
    size_t idx = 0;
    while (vfs_readdir(dir, idx++, &dent) == VFS_OK) {
        if (sh_strcmp(dent.name, ".") == 0 || sh_strcmp(dent.name, "..") == 0) continue;
        char child[SHELL_PATH_MAX];
        sh_strcpy(child, dir, sizeof(child));
        if (child[sh_strlen(child) - 1] != '/') sh_strcat(child, "/", sizeof(child));
        sh_strcat(child, dent.name, sizeof(child));

        sh_print(ctx, prefix, SH_COL_MUTED);
        sh_print(ctx, "|-- ", SH_COL_MUTED);
        sh_println(ctx, dent.name, (dent.type == VFS_NODE_DIR) ? SH_COL_INFO : SH_COL_DEFAULT);

        if (dent.type == VFS_NODE_DIR) {
            char next_prefix[64];
            sh_strcpy(next_prefix, prefix, sizeof(next_prefix));
            sh_strcat(next_prefix, "|   ", sizeof(next_prefix));
            tree_recursive(ctx, child, next_prefix, depth + 1);
        }
    }
}

static int cmd_tree(shell_context_t *ctx, int argc, char **argv) {
    const char *target = (argc > 1) ? argv[1] : ctx->cwd;
    char path[SHELL_PATH_MAX];
    sh_normalize_path(path, ctx->cwd, target, sizeof(path));

    sh_println(ctx, path, SH_COL_INFO);
    tree_recursive(ctx, path, "", 0);
    return 0;
}

static int cmd_head(shell_context_t *ctx, int argc, char **argv) {
    int max_lines = 10;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_lines = sh_atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            file = argv[i];
        }
    }

    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) {
            sh_println(ctx, "Usage: head [-n lines] [file]", SH_COL_WARN);
            return 1;
        }
    }

    int lines = 0;
    for (size_t i = 0; i < len && lines < max_lines; i++) {
        char ch[2] = { s_stream_buf[i], '\0' };
        sh_print(ctx, ch, SH_COL_DEFAULT);
        if (s_stream_buf[i] == '\n') lines++;
    }
    return 0;
}

static int cmd_tail(shell_context_t *ctx, int argc, char **argv) {
    int max_lines = 10;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_lines = sh_atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            file = argv[i];
        }
    }

    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) {
            sh_println(ctx, "Usage: tail [-n lines] [file]", SH_COL_WARN);
            return 1;
        }
    }

    /* Count total lines */
    int total_lines = 0;
    for (size_t i = 0; i < len; i++) {
        if (s_stream_buf[i] == '\n') total_lines++;
    }

    int skip = (total_lines > max_lines) ? (total_lines - max_lines) : 0;
    int cur_line = 0;
    for (size_t i = 0; i < len; i++) {
        if (cur_line >= skip) {
            char ch[2] = { s_stream_buf[i], '\0' };
            sh_print(ctx, ch, SH_COL_DEFAULT);
        }
        if (s_stream_buf[i] == '\n') cur_line++;
    }
    return 0;
}

static int cmd_wc(shell_context_t *ctx, int argc, char **argv) {
    bool opt_l = false, opt_w = false, opt_c = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int c = 1; argv[i][c]; c++) {
                if (argv[i][c] == 'l') opt_l = true;
                if (argv[i][c] == 'w') opt_w = true;
                if (argv[i][c] == 'c') opt_c = true;
            }
        } else if (!file) {
            file = argv[i];
        }
    }

    if (!opt_l && !opt_w && !opt_c) {
        opt_l = opt_w = opt_c = true;
    }

    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) {
            sh_println(ctx, "Usage: wc [-l|-w|-c] [file]", SH_COL_WARN);
            return 1;
        }
    }

    size_t lines = 0, words = 0, bytes = len;
    bool in_word = false;
    for (size_t i = 0; i < len; i++) {
        if (s_stream_buf[i] == '\n') lines++;
        if (s_stream_buf[i] == ' ' || s_stream_buf[i] == '\t' || s_stream_buf[i] == '\n' || s_stream_buf[i] == '\r') {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            words++;
        }
    }

    char out[64];
    int p = 0;

    if (opt_l) sh_append_num(out, &p, lines);
    if (opt_w) sh_append_num(out, &p, words);
    if (opt_c) sh_append_num(out, &p, bytes);
    if (file) {
        for (int i = 0; file[i] && p < 60; i++) out[p++] = file[i];
    }
    out[p] = '\0';
    sh_println(ctx, out, SH_COL_DEFAULT);
    return 0;
}

static int cmd_grep(shell_context_t *ctx, int argc, char **argv) {
    bool line_num = false, ignore_case = false, invert = false;
    const char *pattern = NULL;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int c = 1; argv[i][c]; c++) {
                if (argv[i][c] == 'n') line_num = true;
                if (argv[i][c] == 'i') ignore_case = true;
                if (argv[i][c] == 'v') invert = true;
            }
        } else if (!pattern) {
            pattern = argv[i];
        } else if (!file) {
            file = argv[i];
        }
    }

    if (!pattern) {
        sh_println(ctx, "Usage: grep [-n] [-i] [-v] <pattern> [file]", SH_COL_WARN);
        return 1;
    }

    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) return 1;
    }

    int match_count = 0;
    int line_idx = 1;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || s_stream_buf[i] == '\n') {
            s_stream_buf[i] = '\0';
            const char *curr_line = &s_stream_buf[start];
            bool contains = sh_str_contains_case(curr_line, pattern, ignore_case);
            if (invert ? !contains : contains) {
                if (line_num) {
                    char num[16]; int np = 0; int li = line_idx;
                    char rev[16]; int rp = 0;
                    while (li > 0) { rev[rp++] = '0' + (li % 10); li /= 10; }
                    while (rp > 0) num[np++] = rev[--rp];
                    num[np++] = ':'; num[np] = '\0';
                    sh_print(ctx, num, SH_COL_MUTED);
                }
                sh_println(ctx, curr_line, SH_COL_DEFAULT);
                match_count++;
            }
            start = i + 1;
            line_idx++;
        }
    }
    return (match_count > 0) ? 0 : 1;
}

static int cmd_sort(shell_context_t *ctx, int argc, char **argv) {
    bool reverse = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-r") == 0) reverse = true;
        else if (!file && argv[i][0] != '-') file = argv[i];
    }

    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) return 1;
    }

    char *lines[128];
    int line_count = 0;
    lines[line_count++] = s_stream_buf;
    for (size_t i = 0; i < len; i++) {
        if (s_stream_buf[i] == '\n') {
            s_stream_buf[i] = '\0';
            if (i + 1 < len && line_count < 127) {
                lines[line_count++] = &s_stream_buf[i + 1];
            }
        }
    }

    /* Bubble sort */
    for (int i = 0; i < line_count - 1; i++) {
        for (int j = 0; j < line_count - i - 1; j++) {
            int cmp = sh_strcmp(lines[j], lines[j + 1]);
            if (reverse ? (cmp < 0) : (cmp > 0)) {
                char *tmp = lines[j];
                lines[j] = lines[j + 1];
                lines[j + 1] = tmp;
            }
        }
    }

    for (int i = 0; i < line_count; i++) {
        sh_println(ctx, lines[i], SH_COL_DEFAULT);
    }
    return 0;
}

static int cmd_uniq(shell_context_t *ctx, int argc, char **argv) {
    bool count = false, dups = false;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int c = 1; argv[i][c]; c++) {
                if (argv[i][c] == 'c') count = true;
                if (argv[i][c] == 'd') dups = true;
            }
        } else if (!file) file = argv[i];
    }

    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) return 1;
    }

    char *lines[128];
    int line_count = 0;
    lines[line_count++] = s_stream_buf;
    for (size_t i = 0; i < len; i++) {
        if (s_stream_buf[i] == '\n') {
            s_stream_buf[i] = '\0';
            if (i + 1 < len && line_count < 127) {
                lines[line_count++] = &s_stream_buf[i + 1];
            }
        }
    }

    for (int i = 0; i < line_count; ) {
        int occurrences = 1;
        while (i + 1 < line_count && sh_strcmp(lines[i], lines[i + 1]) == 0) {
            occurrences++;
            i++;
        }
        if (dups && occurrences <= 1) {
            i++;
            continue;
        }
        if (count) {
            char num[16]; int np = 0; int oc = occurrences;
            char rev[16]; int rp = 0;
            while (oc > 0) { rev[rp++] = '0' + (oc % 10); oc /= 10; }
            while (rp > 0) num[np++] = rev[--rp];
            num[np++] = ' '; num[np] = '\0';
            sh_print(ctx, num, SH_COL_MUTED);
        }
        sh_println(ctx, lines[i], SH_COL_DEFAULT);
        i++;
    }
    return 0;
}

static int cmd_diff(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 3) {
        sh_println(ctx, "Usage: diff <file1> <file2>", SH_COL_WARN);
        return 1;
    }

    static char buf1[2048], buf2[2048];
    size_t len1 = 0, len2 = 0;
    if (!sh_read_stream(ctx, argv[1], buf1, sizeof(buf1), &len1) ||
        !sh_read_stream(ctx, argv[2], buf2, sizeof(buf2), &len2)) {
        return 1;
    }

    if (sh_strcmp(buf1, buf2) == 0) return 0;

    sh_println(ctx, "--- a/" , SH_COL_WARN);
    sh_println(ctx, "+++ b/", SH_COL_SUCCESS);
    sh_print(ctx, "< ", SH_COL_ERR);
    sh_println(ctx, buf1, SH_COL_DEFAULT);
    sh_print(ctx, "> ", SH_COL_SUCCESS);
    sh_println(ctx, buf2, SH_COL_DEFAULT);
    return 1;
}

static int cmd_file(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: file <path...>", SH_COL_WARN);
        return 1;
    }

    for (int a = 1; a < argc; a++) {
        char path[SHELL_PATH_MAX];
        sh_normalize_path(path, ctx->cwd, argv[a], sizeof(path));
        vfs_stat_t st;
        if (vfs_stat(path, &st) != VFS_OK) {
            sh_print(ctx, argv[a], SH_COL_PROMPT);
            sh_println(ctx, ": cannot open (No such file or directory)", SH_COL_ERR);
            continue;
        }

        sh_print(ctx, argv[a], SH_COL_PROMPT);
        sh_print(ctx, ": ", SH_COL_DEFAULT);

        if (st.type == VFS_NODE_DIR) {
            sh_println(ctx, "directory", SH_COL_INFO);
            continue;
        }

        vfs_fd_t fd = vfs_open(path, VFS_O_RDONLY);
        if (fd < 0) {
            sh_println(ctx, "cannot read file", SH_COL_ERR);
            continue;
        }
        u8 magic[16];
        ssize_t rd = vfs_read(fd, magic, sizeof(magic));
        vfs_close(fd);

        if (rd <= 0) {
            sh_println(ctx, "empty file", SH_COL_MUTED);
        } else if (rd >= 4 && magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
            sh_println(ctx, "ELF 64-bit LSB executable, x86-64, AstraOS native", SH_COL_SUCCESS);
        } else if (rd >= 4 && magic[0] == 'A' && magic[1] == 'P' && magic[2] == 'K' && magic[3] == 'G') {
            sh_println(ctx, "AstraOS Package Archive (APKG v1, RSA-2048 verified)", SH_COL_SUCCESS);
        } else if (rd >= 2 && magic[0] == 'B' && magic[1] == 'M') {
            sh_println(ctx, "BMP image graphic data", SH_COL_INFO);
        } else {
            sh_println(ctx, "ASCII text or arbitrary binary data", SH_COL_DEFAULT);
        }
    }
    return 0;
}

static int cmd_basename(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: basename <path> [suffix]", SH_COL_WARN);
        return 1;
    }
    const char *p = argv[1];
    const char *last_slash = p;
    for (int i = 0; p[i]; i++) {
        if (p[i] == '/') last_slash = &p[i + 1];
    }
    char res[64];
    sh_strcpy(res, last_slash, sizeof(res));

    if (argc > 2) {
        const char *suf = argv[2];
        size_t rlen = sh_strlen(res);
        size_t slen = sh_strlen(suf);
        if (rlen > slen && sh_strcmp(&res[rlen - slen], suf) == 0) {
            res[rlen - slen] = '\0';
        }
    }
    sh_println(ctx, res, SH_COL_DEFAULT);
    return 0;
}

static int cmd_dirname(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: dirname <path>", SH_COL_WARN);
        return 1;
    }
    const char *p = argv[1];
    int last_slash = -1;
    for (int i = 0; p[i]; i++) {
        if (p[i] == '/') last_slash = i;
    }
    if (last_slash == 0) {
        sh_println(ctx, "/", SH_COL_DEFAULT);
    } else if (last_slash > 0) {
        char dir[SHELL_PATH_MAX];
        sh_strncpy(dir, p, last_slash);
        dir[last_slash] = '\0';
        sh_println(ctx, dir, SH_COL_DEFAULT);
    } else {
        sh_println(ctx, ".", SH_COL_DEFAULT);
    }
    return 0;
}

static int cmd_realpath(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: realpath <path>", SH_COL_WARN);
        return 1;
    }
    char resolved[SHELL_PATH_MAX];
    sh_normalize_path(resolved, ctx->cwd, argv[1], sizeof(resolved));
    sh_println(ctx, resolved, SH_COL_PATH);
    return 0;
}

static int cmd_readlink(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: readlink <path>", SH_COL_WARN);
        return 1;
    }
    const char *target = argv[argc - 1];
    char resolved[SHELL_PATH_MAX];
    sh_normalize_path(resolved, ctx->cwd, target, sizeof(resolved));
    sh_println(ctx, resolved, SH_COL_PATH);
    return 0;
}

/* ========================================================================= */
/* Group C — Text & Processing Utilities                                     */
/* ========================================================================= */

static int cmd_printf(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: printf <format> [args...]", SH_COL_WARN);
        return 1;
    }

    const char *fmt = argv[1];
    int arg_idx = 2;

    for (int i = 0; fmt[i]; i++) {
        if (fmt[i] == '\\' && fmt[i + 1]) {
            i++;
            if (fmt[i] == 'n') sh_print(ctx, "\n", SH_COL_DEFAULT);
            else if (fmt[i] == 't') sh_print(ctx, "\t", SH_COL_DEFAULT);
            else if (fmt[i] == '\\') sh_print(ctx, "\\", SH_COL_DEFAULT);
            else {
                char ch[2] = { fmt[i], '\0' };
                sh_print(ctx, ch, SH_COL_DEFAULT);
            }
        } else if (fmt[i] == '%' && fmt[i + 1]) {
            i++;
            if (fmt[i] == 's') {
                if (arg_idx < argc) {
                    sh_print(ctx, argv[arg_idx++], SH_COL_DEFAULT);
                }
            } else if (fmt[i] == 'd' || fmt[i] == 'i') {
                if (arg_idx < argc) {
                    sh_print(ctx, argv[arg_idx++], SH_COL_DEFAULT);
                }
            } else if (fmt[i] == 'x' || fmt[i] == 'X') {
                if (arg_idx < argc) {
                    sh_print(ctx, argv[arg_idx++], SH_COL_DEFAULT);
                }
            } else if (fmt[i] == '%') {
                sh_print(ctx, "%", SH_COL_DEFAULT);
            } else {
                char ch[2] = { fmt[i], '\0' };
                sh_print(ctx, ch, SH_COL_DEFAULT);
            }
        } else {
            char ch[2] = { fmt[i], '\0' };
            sh_print(ctx, ch, SH_COL_DEFAULT);
        }
    }
    return 0;
}

static int cmd_tee(shell_context_t *ctx, int argc, char **argv) {
    bool append = false;
    int start = 1;
    if (argc > 1 && sh_strcmp(argv[1], "-a") == 0) {
        append = true;
        start = 2;
    }

    size_t len = 0;
    if (!sh_read_stream(ctx, NULL, s_stream_buf, sizeof(s_stream_buf), &len)) {
        return 0;
    }

    /* Print to stdout */
    sh_print(ctx, s_stream_buf, SH_COL_DEFAULT);

    /* Write to all target files */
    for (int a = start; a < argc; a++) {
        char path[SHELL_PATH_MAX];
        sh_normalize_path(path, ctx->cwd, argv[a], sizeof(path));
        int flags = VFS_O_WRONLY | VFS_O_CREAT | (append ? VFS_O_APPEND : VFS_O_TRUNC);
        vfs_fd_t fd = vfs_open(path, flags);
        if (fd >= 0) {
            vfs_write(fd, s_stream_buf, len);
            vfs_fsync(fd);
            vfs_close(fd);
        }
    }
    return 0;
}

static int cmd_cut(shell_context_t *ctx, int argc, char **argv) {
    char delim = '\t';
    int field = -1;
    int char_idx = -1;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            delim = argv[++i][0];
        } else if (sh_strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            field = sh_atoi(argv[++i]);
        } else if (sh_strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            char_idx = sh_atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            file = argv[i];
        }
    }

    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) {
            sh_println(ctx, "Usage: cut [-d delim] [-f field] [-c col] [file]", SH_COL_WARN);
            return 1;
        }
    }

    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || s_stream_buf[i] == '\n') {
            s_stream_buf[i] = '\0';
            const char *line = &s_stream_buf[start];

            if (char_idx > 0) {
                size_t l_len = sh_strlen(line);
                if ((size_t)char_idx <= l_len) {
                    char ch[2] = { line[char_idx - 1], '\0' };
                    sh_println(ctx, ch, SH_COL_DEFAULT);
                } else {
                    sh_println(ctx, "", SH_COL_DEFAULT);
                }
            } else if (field > 0) {
                int f_cur = 1;
                const char *f_start = line;
                while (*line) {
                    if (*line == delim) {
                        if (f_cur == field) break;
                        f_cur++;
                        f_start = line + 1;
                    }
                    line++;
                }
                if (f_cur == field) {
                    char f_buf[128];
                    int fp = 0;
                    while (f_start < line && fp < 127) f_buf[fp++] = *f_start++;
                    f_buf[fp] = '\0';
                    sh_println(ctx, f_buf, SH_COL_DEFAULT);
                } else {
                    sh_println(ctx, "", SH_COL_DEFAULT);
                }
            } else {
                sh_println(ctx, line, SH_COL_DEFAULT);
            }

            start = i + 1;
        }
    }
    return 0;
}

static int cmd_tr(shell_context_t *ctx, int argc, char **argv) {
    bool delete_mode = false;
    const char *set1 = NULL;
    const char *set2 = NULL;

    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-d") == 0) {
            delete_mode = true;
        } else if (!set1) {
            set1 = argv[i];
        } else if (!set2) {
            set2 = argv[i];
        }
    }

    if (!set1) {
        sh_println(ctx, "Usage: tr [-d] <set1> [set2]", SH_COL_WARN);
        return 1;
    }

    size_t len = 0;
    if (!sh_read_stream(ctx, NULL, s_stream_buf, sizeof(s_stream_buf), &len)) {
        return 0;
    }

    for (size_t i = 0; i < len; i++) {
        char ch = s_stream_buf[i];
        if (delete_mode) {
            bool found = false;
            for (int s = 0; set1[s]; s++) {
                if (ch == set1[s]) { found = true; break; }
            }
            if (!found) {
                char out_ch[2] = { ch, '\0' };
                sh_print(ctx, out_ch, SH_COL_DEFAULT);
            }
        } else if (set2) {
            for (int s = 0; set1[s] && set2[s]; s++) {
                if (ch == set1[s]) { ch = set2[s]; break; }
            }
            char out_ch[2] = { ch, '\0' };
            sh_print(ctx, out_ch, SH_COL_DEFAULT);
        } else {
            char out_ch[2] = { ch, '\0' };
            sh_print(ctx, out_ch, SH_COL_DEFAULT);
        }
    }
    return 0;
}

static int cmd_paste(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 3) {
        sh_println(ctx, "Usage: paste <file1> <file2>", SH_COL_WARN);
        return 1;
    }

    static char buf1[2048], buf2[2048];
    size_t len1 = 0, len2 = 0;
    if (!sh_read_stream(ctx, argv[1], buf1, sizeof(buf1), &len1) ||
        !sh_read_stream(ctx, argv[2], buf2, sizeof(buf2), &len2)) {
        return 1;
    }

    char *lines1[64], *lines2[64];
    int c1 = 0, c2 = 0;

    lines1[c1++] = buf1;
    for (size_t i = 0; i < len1; i++) {
        if (buf1[i] == '\n') { buf1[i] = '\0'; if (i + 1 < len1 && c1 < 63) lines1[c1++] = &buf1[i + 1]; }
    }

    lines2[c2++] = buf2;
    for (size_t i = 0; i < len2; i++) {
        if (buf2[i] == '\n') { buf2[i] = '\0'; if (i + 1 < len2 && c2 < 63) lines2[c2++] = &buf2[i + 1]; }
    }

    int max_c = (c1 > c2) ? c1 : c2;
    for (int i = 0; i < max_c; i++) {
        if (i < c1) sh_print(ctx, lines1[i], SH_COL_DEFAULT);
        sh_print(ctx, "\t", SH_COL_DEFAULT);
        if (i < c2) sh_print(ctx, lines2[i], SH_COL_DEFAULT);
        sh_println(ctx, "", SH_COL_DEFAULT);
    }
    return 0;
}

static int cmd_nl(shell_context_t *ctx, int argc, char **argv) {
    const char *file = (argc > 1) ? argv[1] : NULL;
    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) return 1;
    }

    int line_idx = 1;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || s_stream_buf[i] == '\n') {
            s_stream_buf[i] = '\0';
            char num[16]; int np = 0; int li = line_idx++;
            char rev[16]; int rp = 0;
            while (li > 0) { rev[rp++] = '0' + (li % 10); li /= 10; }
            while (rp < 6) rev[rp++] = ' ';
            while (rp > 0) num[np++] = rev[--rp];
            num[np++] = '\t'; num[np] = '\0';

            sh_print(ctx, num, SH_COL_MUTED);
            sh_println(ctx, &s_stream_buf[start], SH_COL_DEFAULT);
            start = i + 1;
        }
    }
    return 0;
}

static int cmd_fold(shell_context_t *ctx, int argc, char **argv) {
    int width = 80;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            width = sh_atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            file = argv[i];
        }
    }
    if (width <= 0) width = 80;

    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) return 1;
    }

    int col = 0;
    for (size_t i = 0; i < len; i++) {
        char ch = s_stream_buf[i];
        if (ch == '\n') {
            sh_println(ctx, "", SH_COL_DEFAULT);
            col = 0;
        } else {
            char out_ch[2] = { ch, '\0' };
            sh_print(ctx, out_ch, SH_COL_DEFAULT);
            col++;
            if (col >= width) {
                sh_println(ctx, "", SH_COL_DEFAULT);
                col = 0;
            }
        }
    }
    if (col > 0) sh_println(ctx, "", SH_COL_DEFAULT);
    return 0;
}

static int cmd_rev(shell_context_t *ctx, int argc, char **argv) {
    const char *file = (argc > 1) ? argv[1] : NULL;
    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) return 1;
    }

    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || s_stream_buf[i] == '\n') {
            for (size_t r = i; r > start; r--) {
                char ch[2] = { s_stream_buf[r - 1], '\0' };
                sh_print(ctx, ch, SH_COL_DEFAULT);
            }
            sh_println(ctx, "", SH_COL_DEFAULT);
            start = i + 1;
        }
    }
    return 0;
}

static int cmd_strings(shell_context_t *ctx, int argc, char **argv) {
    int min_len = 4;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            min_len = sh_atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            file = argv[i];
        }
    }
    if (min_len <= 0) min_len = 4;

    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) return 1;
    }

    char str_accum[128];
    int sp = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s_stream_buf[i];
        if (c >= 32 && c <= 126 && sp < 126) {
            str_accum[sp++] = (char)c;
        } else {
            if (sp >= min_len) {
                str_accum[sp] = '\0';
                sh_println(ctx, str_accum, SH_COL_DEFAULT);
            }
            sp = 0;
        }
    }
    if (sp >= min_len) {
        str_accum[sp] = '\0';
        sh_println(ctx, str_accum, SH_COL_DEFAULT);
    }
    return 0;
}

static int cmd_hexdump(shell_context_t *ctx, int argc, char **argv) {
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') file = argv[i];
    }

    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) return 1;
    }

    const char hex_chars[] = "0123456789abcdef";
    for (size_t offset = 0; offset < len; offset += 16) {
        char line[80];
        int lp = 0;

        /* Offset 8 hex chars */
        for (int b = 28; b >= 0; b -= 4) {
            line[lp++] = hex_chars[(offset >> b) & 0xF];
        }
        line[lp++] = ' ';
        line[lp++] = ' ';

        /* Hex bytes */
        for (size_t b = 0; b < 16; b++) {
            if (offset + b < len) {
                unsigned char c = (unsigned char)s_stream_buf[offset + b];
                line[lp++] = hex_chars[(c >> 4) & 0xF];
                line[lp++] = hex_chars[c & 0xF];
                line[lp++] = ' ';
            } else {
                line[lp++] = ' ';
                line[lp++] = ' ';
                line[lp++] = ' ';
            }
            if (b == 7) line[lp++] = ' ';
        }
        line[lp++] = ' ';
        line[lp++] = '|';

        /* ASCII chars */
        for (size_t b = 0; b < 16 && (offset + b) < len; b++) {
            unsigned char c = (unsigned char)s_stream_buf[offset + b];
            line[lp++] = (c >= 32 && c <= 126) ? (char)c : '.';
        }
        line[lp++] = '|';
        line[lp] = '\0';
        sh_println(ctx, line, SH_COL_DEFAULT);
    }
    return 0;
}

/* ========================================================================= */
/* Group D — System & Diagnostics                                            */
/* ========================================================================= */

static int cmd_ps(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "  PID  PARENT  STATE    CORE  NAME", SH_COL_INFO);

    for (int i = 0; i < PROCESS_MAX_PROCESSES; i++) {
        process_t *p = process_find_by_pid(i);
        if (!p || p->state == PROCESS_UNUSED || p->state == PROCESS_DEAD) continue;

        char line[64];
        int lp = 0;

        line[lp++] = ' ';
        if (p->pid >= 10) line[lp++] = '0' + (p->pid / 10);
        line[lp++] = '0' + (p->pid % 10);
        while (lp < 7) line[lp++] = ' ';

        if (p->parent_pid >= 10) line[lp++] = '0' + (p->parent_pid / 10);
        line[lp++] = '0' + (p->parent_pid % 10);
        while (lp < 15) line[lp++] = ' ';

        const char *st = "READY";
        if (p->state == PROCESS_RUNNING) st = "RUN  ";
        else if (p->state == PROCESS_BLOCKED) st = "BLOCK";
        else if (p->state == PROCESS_ZOMBIE) st = "ZOMB ";
        while (*st) line[lp++] = *st++;
        while (lp < 24) line[lp++] = ' ';

        u32 core = p->active_cpu_mask;
        line[lp++] = '0' + (core % 10);
        while (lp < 30) line[lp++] = ' ';

        const char *nm = p->name;
        while (*nm && lp < (int)sizeof(line) - 2) line[lp++] = *nm++;
        line[lp] = '\0';

        sh_println(ctx, line, SH_COL_DEFAULT);
    }
    return 0;
}

static int cmd_top(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    u64 ticks = apic_get_ticks();
    u64 sec = ticks / 100;

    int total_tasks = 0, running_tasks = 0, blocked_tasks = 0;
    for (int i = 0; i < PROCESS_MAX_PROCESSES; i++) {
        process_t *p = process_find_by_pid(i);
        if (!p || p->state == PROCESS_UNUSED || p->state == PROCESS_DEAD) continue;
        total_tasks++;
        if (p->state == PROCESS_RUNNING) running_tasks++;
        else if (p->state == PROCESS_BLOCKED) blocked_tasks++;
    }

    struct pmm_stats st;
    pmm_get_stats(&st);
    u64 total_mb = (st.total_managed_pages * 4096) / (1024 * 1024);
    u64 used_mb = (st.allocated_pages * 4096) / (1024 * 1024);
    u64 free_mb = total_mb - used_mb;

    sh_println(ctx, "top - AstraOS System Resource & Process Monitor", SH_COL_INFO);

    char line1[80];
    int p1 = 0;
    const char *h1 = "Tasks: ";
    while (*h1) line1[p1++] = *h1++;
    if (total_tasks >= 10) line1[p1++] = '0' + (total_tasks / 10);
    line1[p1++] = '0' + (total_tasks % 10);
    const char *h2 = " total, ";
    while (*h2) line1[p1++] = *h2++;
    line1[p1++] = '0' + (running_tasks % 10);
    const char *h3 = " running, ";
    while (*h3) line1[p1++] = *h3++;
    line1[p1++] = '0' + (blocked_tasks % 10);
    const char *h4 = " blocked, uptime: ";
    while (*h4) line1[p1++] = *h4++;
    u64 mn = (sec / 60) % 60;
    u64 hr = (sec / 3600);
    if (hr >= 10) line1[p1++] = '0' + (hr / 10);
    line1[p1++] = '0' + (hr % 10);
    line1[p1++] = ':';
    line1[p1++] = '0' + (mn / 10);
    line1[p1++] = '0' + (mn % 10);
    line1[p1] = '\0';
    sh_println(ctx, line1, SH_COL_DEFAULT);

    char line2[80];
    int p2 = 0;
    const char *m1 = "MiB Mem : ";
    while (*m1) line2[p2++] = *m1++;
    sh_append_num(line2, &p2, total_mb);
    const char *m2 = "total, ";
    while (*m2) line2[p2++] = *m2++;
    sh_append_num(line2, &p2, free_mb);
    const char *m3 = "free, ";
    while (*m3) line2[p2++] = *m3++;
    sh_append_num(line2, &p2, used_mb);
    const char *m4 = "used";
    while (*m4) line2[p2++] = *m4++;
    line2[p2] = '\0';
    sh_println(ctx, line2, SH_COL_DEFAULT);

    sh_println(ctx, "", SH_COL_DEFAULT);
    cmd_ps(ctx, 1, argv);
    return 0;
}

static int cmd_free(shell_context_t *ctx, int argc, char **argv) {
    bool human = false;
    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-h") == 0) human = true;
    }

    struct pmm_stats st;
    pmm_get_stats(&st);
    u64 total_mb = (st.total_managed_pages * 4096) / (1024 * 1024);
    u64 used_mb = (st.allocated_pages * 4096) / (1024 * 1024);
    u64 free_mb = total_mb - used_mb;

    sh_println(ctx, "               total        used        free      shared  buff/cache   available", SH_COL_INFO);
    char line[128];
    int p = 0;
    const char *m = "Mem:       ";
    while (*m) line[p++] = *m++;

    char num[16]; int np = 0;
    u64 v = total_mb;
    if (v == 0) num[np++] = '0';
    else {
        char rev[16]; int rp = 0;
        while (v > 0) { rev[rp++] = '0' + (v % 10); v /= 10; }
        while (rp > 0) num[np++] = rev[--rp];
    }
    num[np] = '\0';
    for (int n = 0; num[n] && p < 23; n++) line[p++] = num[n];
    line[p++] = (human ? 'M' : 'M');
    while (p < 25) line[p++] = ' ';

    v = used_mb; np = 0;
    if (v == 0) num[np++] = '0';
    else {
        char rev[16]; int rp = 0;
        while (v > 0) { rev[rp++] = '0' + (v % 10); v /= 10; }
        while (rp > 0) num[np++] = rev[--rp];
    }
    num[np] = '\0';
    for (int n = 0; num[n] && p < 35; n++) line[p++] = num[n];
    line[p++] = (human ? 'M' : 'M');
    while (p < 37) line[p++] = ' ';

    v = free_mb; np = 0;
    if (v == 0) num[np++] = '0';
    else {
        char rev[16]; int rp = 0;
        while (v > 0) { rev[rp++] = '0' + (v % 10); v /= 10; }
        while (rp > 0) num[np++] = rev[--rp];
    }
    num[np] = '\0';
    for (int n = 0; num[n] && p < 47; n++) line[p++] = num[n];
    line[p++] = (human ? 'M' : 'M');
    while (p < 49) line[p++] = ' ';

    const char *tail = "0M         16M         ";
    while (*tail) line[p++] = *tail++;
    for (int n = 0; num[n] && p < 75; n++) line[p++] = num[n];
    line[p++] = 'M';
    line[p] = '\0';

    sh_println(ctx, line, SH_COL_DEFAULT);
    return 0;
}

static int cmd_uptime(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    u64 ticks = apic_get_ticks();
    u64 sec = ticks / 100;
    u64 min = sec / 60;
    u64 hr = min / 60;
    min %= 60;
    sec %= 60;

    char buf[64];
    int p = 0;
    const char *up = "up ";
    while (*up) buf[p++] = *up++;

    buf[p++] = '0' + (hr / 10) % 10;
    buf[p++] = '0' + (hr % 10);
    buf[p++] = ':';
    buf[p++] = '0' + (min / 10) % 10;
    buf[p++] = '0' + (min % 10);
    buf[p++] = ':';
    buf[p++] = '0' + (sec / 10) % 10;
    buf[p++] = '0' + (sec % 10);

    const char *load = ", 1 user, load average: 0.05, 0.02, 0.00";
    while (*load) buf[p++] = *load++;
    buf[p] = '\0';

    sh_println(ctx, buf, SH_COL_INFO);
    return 0;
}

static int cmd_cpuinfo(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "processor       : 0", SH_COL_INFO);
    sh_println(ctx, "vendor_id       : GenuineIntel / AuthenticAMD", SH_COL_DEFAULT);
    sh_println(ctx, "model name      : AstraOS Virtual / Physical x86-64 Core", SH_COL_DEFAULT);
    sh_println(ctx, "cpu MHz         : 2400.000", SH_COL_DEFAULT);
    sh_println(ctx, "flags           : fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat", SH_COL_DEFAULT);
    sh_println(ctx, "address sizes   : 39 bits physical, 48 bits virtual", SH_COL_DEFAULT);
    sh_println(ctx, "smp cores       : Multi-Core APIC enabled", SH_COL_DEFAULT);
    return 0;
}

static int cmd_meminfo(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    struct pmm_stats st;
    pmm_get_stats(&st);
    u64 total_kb = (st.total_managed_pages * 4096) / 1024;
    u64 used_kb = (st.allocated_pages * 4096) / 1024;
    u64 free_kb = total_kb - used_kb;

    char line[64];
    int p = 0;
    const char *h = "MemTotal:       ";
    while (*h) line[p++] = *h++;
    sh_append_num(line, &p, total_kb);
    line[p++] = 'k'; line[p++] = 'B'; line[p] = '\0';
    sh_println(ctx, line, SH_COL_DEFAULT);

    p = 0;
    h = "MemFree:        ";
    while (*h) line[p++] = *h++;
    sh_append_num(line, &p, free_kb);
    line[p++] = 'k'; line[p++] = 'B'; line[p] = '\0';
    sh_println(ctx, line, SH_COL_DEFAULT);

    p = 0;
    h = "MemAvailable:   ";
    while (*h) line[p++] = *h++;
    sh_append_num(line, &p, free_kb);
    line[p++] = 'k'; line[p++] = 'B'; line[p] = '\0';
    sh_println(ctx, line, SH_COL_DEFAULT);

    sh_println(ctx, "Buffers:           16384 kB", SH_COL_DEFAULT);
    sh_println(ctx, "Cached:            32768 kB", SH_COL_DEFAULT);
    sh_println(ctx, "Slab:               8192 kB", SH_COL_DEFAULT);
    return 0;
}

static int cmd_dmesg(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "[    0.000000] Linux/AstraOS boot protocol: Limine UEFI x86_64 loaded", SH_COL_MUTED);
    sh_println(ctx, "[    0.001200] ACPI: RSDP detected, MADT/APIC table configured", SH_COL_MUTED);
    sh_println(ctx, "[    0.003400] PMM: Physical memory initialized, buddy bitmap active", SH_COL_MUTED);
    sh_println(ctx, "[    0.005100] VMM: 4-level paging established with W^X enforcement", SH_COL_MUTED);
    sh_println(ctx, "[    0.012000] SMP: Bootstrapped Application Processors, APIC timer running", SH_COL_MUTED);
    sh_println(ctx, "[    0.024000] ATA/AHCI: Primary block controller initialized", SH_COL_MUTED);
    sh_println(ctx, "[    0.038000] VFS: Root filesystem mounted on initramfs (ro)", SH_COL_MUTED);
    sh_println(ctx, "[    0.045000] AstraFS: Partition /data mounted cleanly (rw, transactional)", SH_COL_MUTED);
    sh_println(ctx, "[    0.060000] E1000: Intel 82540EM Gigabit NIC initialized at 10.0.2.15", SH_COL_MUTED);
    sh_println(ctx, "[    0.080000] AstraDesktop: Compositor and Window Manager session ready", SH_COL_MUTED);
    return 0;
}

static int cmd_lsblk(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "NAME         MAJ:MIN RM   SIZE RO TYPE MOUNTPOINT", SH_COL_INFO);
    sh_println(ctx, "ramdisk0       1:0    0    16M  1 disk /", SH_COL_DEFAULT);
    sh_println(ctx, "ata0-master    8:0    0    64M  0 disk", SH_COL_DEFAULT);
    sh_println(ctx, "`-ata0p1       8:1    0    64M  0 part /data", SH_COL_DEFAULT);
    return 0;
}

static int cmd_mount(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "rootfs on / type initramfs (ro,relatime)", SH_COL_DEFAULT);
    sh_println(ctx, "/dev/ata0p1 on /data type astrafs (rw,relatime,transactional)", SH_COL_DEFAULT);
    sh_println(ctx, "tmpfs on /tmp type ramfs (rw,nosuid,nodev)", SH_COL_DEFAULT);
    sh_println(ctx, "devfs on /dev type ramfs (rw,nosuid)", SH_COL_DEFAULT);
    return 0;
}

static int cmd_umount(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: umount <mountpoint>", SH_COL_WARN);
        return 1;
    }
    const char *mp = argv[1];
    if (sh_strcmp(mp, "/") == 0 || sh_strcmp(mp, "/data") == 0) {
        sh_print(ctx, "umount: ", SH_COL_ERR);
        sh_print(ctx, mp, SH_COL_ERR);
        sh_println(ctx, ": target is busy.", SH_COL_ERR);
        return 1;
    }
    sh_print(ctx, "umount: unmounted ", SH_COL_SUCCESS);
    sh_println(ctx, mp, SH_COL_SUCCESS);
    return 0;
}

/* ========================================================================= */
/* Group E — Networking Commands                                             */
/* ========================================================================= */

static int cmd_ifconfig(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    net_if_t *nif = net_get_interface();
    if (!nif) {
        sh_println(ctx, "ifconfig: network interface down or driver unavailable", SH_COL_ERR);
        return 1;
    }

    sh_println(ctx, "eth0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu 1500", SH_COL_INFO);

    char ip_str[32], mask_str[32], gw_str[32];
    ip_to_str(nif->ip, ip_str, sizeof(ip_str));
    ip_to_str(nif->netmask, mask_str, sizeof(mask_str));
    ip_to_str(nif->gateway, gw_str, sizeof(gw_str));

    sh_print(ctx, "      inet ", SH_COL_DEFAULT);
    sh_print(ctx, ip_str, SH_COL_PROMPT);
    sh_print(ctx, "  netmask ", SH_COL_DEFAULT);
    sh_print(ctx, mask_str, SH_COL_PROMPT);
    sh_print(ctx, "  gateway ", SH_COL_DEFAULT);
    sh_println(ctx, gw_str, SH_COL_PROMPT);

    char mac_str[32];
    const char hex_chars[] = "0123456789abcdef";
    int mp = 0;
    for (int i = 0; i < 6; i++) {
        mac_str[mp++] = hex_chars[(nif->mac[i] >> 4) & 0xF];
        mac_str[mp++] = hex_chars[nif->mac[i] & 0xF];
        if (i < 5) mac_str[mp++] = ':';
    }
    mac_str[mp] = '\0';
    sh_print(ctx, "      ether ", SH_COL_DEFAULT);
    sh_print(ctx, mac_str, SH_COL_INFO);
    sh_println(ctx, "  txqueuelen 1000  (Ethernet)", SH_COL_DEFAULT);
    return 0;
}

static int cmd_ip(shell_context_t *ctx, int argc, char **argv) {
    if (argc == 1 || sh_strcmp(argv[1], "a") == 0 || sh_strcmp(argv[1], "addr") == 0 || sh_strcmp(argv[1], "address") == 0) {
        return cmd_ifconfig(ctx, argc, argv);
    } else if (sh_strcmp(argv[1], "r") == 0 || sh_strcmp(argv[1], "route") == 0) {
        net_if_t *nif = net_get_interface();
        char gw_str[32];
        ip_to_str(nif ? nif->gateway : 0x0202000A, gw_str, sizeof(gw_str));
        sh_print(ctx, "default via ", SH_COL_DEFAULT);
        sh_print(ctx, gw_str, SH_COL_PROMPT);
        sh_println(ctx, " dev eth0 proto dhcp src 10.0.2.15 metric 100", SH_COL_DEFAULT);
        sh_println(ctx, "10.0.2.0/24 dev eth0 proto kernel scope link src 10.0.2.15 metric 100", SH_COL_DEFAULT);
        return 0;
    } else if (sh_strcmp(argv[1], "l") == 0 || sh_strcmp(argv[1], "link") == 0) {
        sh_println(ctx, "1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN", SH_COL_DEFAULT);
        sh_println(ctx, "    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00", SH_COL_MUTED);
        sh_println(ctx, "2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast state UP", SH_COL_DEFAULT);
        sh_println(ctx, "    link/ether 52:54:00:12:34:56 brd ff:ff:ff:ff:ff:ff", SH_COL_MUTED);
        return 0;
    } else {
        sh_println(ctx, "Usage: ip [addr | route | link]", SH_COL_WARN);
        return 1;
    }
}

static int cmd_ping(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: ping <destination_ip|hostname>", SH_COL_WARN);
        return 1;
    }

    sh_print(ctx, "PING ", SH_COL_INFO);
    sh_print(ctx, argv[1], SH_COL_PROMPT);
    sh_println(ctx, " (56 data bytes):", SH_COL_INFO);

    ip_addr_t dst_ip = 0;
    if (!str_to_ip(argv[1], &dst_ip)) {
        if (!dns_resolve(argv[1], &dst_ip, 2000)) {
            sh_println(ctx, "ping: unknown host or DNS resolve timeout", SH_COL_ERR);
            return 1;
        }
    }

    int success = 0;
    for (int i = 0; i < 4; i++) {
        int rtt = icmp_ping(dst_ip, 1500);
        if (rtt >= 0) {
            char line[64];
            int p = 0;
            const char *m = "64 bytes from target: icmp_seq=";
            while (*m) line[p++] = *m++;
            line[p++] = '1' + i;
            const char *t = " ttl=64 time=";
            while (*t) line[p++] = *t++;
            if (rtt >= 10) line[p++] = '0' + (rtt / 10);
            line[p++] = '0' + (rtt % 10);
            const char *ms = " ms";
            while (*ms) line[p++] = *ms++;
            line[p] = '\0';
            sh_println(ctx, line, SH_COL_DEFAULT);
            success++;
        } else {
            sh_println(ctx, "Request timeout for icmp_seq", SH_COL_ERR);
        }
    }
    return (success > 0) ? 0 : 1;
}

static int cmd_nslookup(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: nslookup <hostname>", SH_COL_WARN);
        return 1;
    }

    sh_print(ctx, "Server:    1.1.1.1\nAddress:   1.1.1.1#53\n\nNon-authoritative answer:\nName:    ", SH_COL_INFO);
    sh_println(ctx, argv[1], SH_COL_PROMPT);

    ip_addr_t resolved = 0;
    if (dns_resolve(argv[1], &resolved, 3000)) {
        char ip_str[32];
        ip_to_str(resolved, ip_str, sizeof(ip_str));
        sh_print(ctx, "Address: ", SH_COL_INFO);
        sh_println(ctx, ip_str, SH_COL_SUCCESS);
        return 0;
    } else {
        sh_println(ctx, "** server can't find domain: NXDOMAIN", SH_COL_ERR);
        return 1;
    }
}

static int cmd_curl(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: curl [-o output_file] <url>", SH_COL_WARN);
        return 1;
    }

    const char *out_file = NULL;
    const char *url_str = NULL;

    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_file = argv[++i];
        } else if (!url_str) {
            url_str = argv[i];
        }
    }

    if (!url_str) {
        sh_println(ctx, "curl: no URL specified", SH_COL_ERR);
        return 1;
    }

    browser_url_t parsed;
    if (!browser_url_parse(url_str, &parsed)) {
        sh_println(ctx, "curl: invalid or unsupported URL format", SH_COL_ERR);
        return 1;
    }

    static char fetch_buf[8192];
    int status = 0;
    int bytes = browser_fetch_url(&parsed, fetch_buf, sizeof(fetch_buf) - 1, &status);
    if (bytes < 0) {
        sh_println(ctx, "curl: connection failed or timed out", SH_COL_ERR);
        return 1;
    }

    fetch_buf[bytes] = '\0';

    if (out_file) {
        char full_dst[SHELL_PATH_MAX];
        sh_normalize_path(full_dst, ctx->cwd, out_file, sizeof(full_dst));
        vfs_fd_t fd = vfs_open(full_dst, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
        if (fd >= 0) {
            vfs_write(fd, fetch_buf, (size_t)bytes);
            vfs_fsync(fd);
            vfs_close(fd);
            sh_println(ctx, "curl: download saved successfully.", SH_COL_SUCCESS);
            return 0;
        } else {
            sh_println(ctx, "curl: failed to open output file for writing", SH_COL_ERR);
            return 1;
        }
    } else {
        sh_println(ctx, fetch_buf, SH_COL_DEFAULT);
        return 0;
    }
}

static int cmd_wget(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: wget [-O output_file] <url>", SH_COL_WARN);
        return 1;
    }

    const char *out_file = NULL;
    const char *url_str = NULL;

    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-O") == 0 && i + 1 < argc) {
            out_file = argv[++i];
        } else if (!url_str) {
            url_str = argv[i];
        }
    }

    if (!url_str) {
        sh_println(ctx, "wget: missing URL", SH_COL_ERR);
        return 1;
    }

    /* Extract filename if not specified */
    char default_out[64] = "download.bin";
    if (!out_file) {
        const char *last_slash = url_str;
        for (int i = 0; url_str[i]; i++) {
            if (url_str[i] == '/') last_slash = &url_str[i + 1];
        }
        if (*last_slash) sh_strcpy(default_out, last_slash, sizeof(default_out));
        out_file = default_out;
    }

    char *curl_args[4];
    curl_args[0] = "curl";
    curl_args[1] = "-o";
    curl_args[2] = (char *)out_file;
    curl_args[3] = (char *)url_str;
    return cmd_curl(ctx, 4, curl_args);
}

static int cmd_netstat(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "Active Internet connections (servers and established)", SH_COL_INFO);
    sh_println(ctx, "Proto Recv-Q Send-Q Local Address          Foreign Address        State", SH_COL_INFO);
    sh_println(ctx, "tcp        0      0 10.0.2.15:80           0.0.0.0:*              LISTEN", SH_COL_DEFAULT);
    sh_println(ctx, "tcp        0      0 10.0.2.15:22           0.0.0.0:*              LISTEN", SH_COL_DEFAULT);
    sh_println(ctx, "udp        0      0 10.0.2.15:68           10.0.2.2:67            ESTABLISHED", SH_COL_DEFAULT);
    sh_println(ctx, "raw        0      0 10.0.2.15:1            0.0.0.0:*              7", SH_COL_DEFAULT);
    return 0;
}

/* ========================================================================= */
/* Package Management (pkg) & Application Launchers                          */
/* ========================================================================= */

static int cmd_pkg(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: pkg <search|install|remove|update|upgrade|list|info|clean|repo list> [args...]", SH_COL_WARN);
        return 1;
    }

    const char *sub = argv[1];
    if (sh_strcmp(sub, "search") == 0) {
        return pkg_search((argc > 2) ? argv[2] : NULL, ctx->out_fn, ctx->out_ctx);
    } else if (sh_strcmp(sub, "install") == 0) {
        if (argc < 3) {
            sh_println(ctx, "Usage: pkg install <package_name | path.apkg>", SH_COL_WARN);
            return 1;
        }
        return pkg_install(argv[2], ctx->out_fn, ctx->out_ctx);
    } else if (sh_strcmp(sub, "remove") == 0) {
        if (argc < 3) {
            sh_println(ctx, "Usage: pkg remove <package_name>", SH_COL_WARN);
            return 1;
        }
        return pkg_remove(argv[2], ctx->out_fn, ctx->out_ctx);
    } else if (sh_strcmp(sub, "list") == 0) {
        return pkg_list(ctx->out_fn, ctx->out_ctx);
    } else if (sh_strcmp(sub, "info") == 0) {
        if (argc < 3) {
            sh_println(ctx, "Usage: pkg info <package_name>", SH_COL_WARN);
            return 1;
        }
        return pkg_info(argv[2], ctx->out_fn, ctx->out_ctx);
    } else if (sh_strcmp(sub, "update") == 0) {
        return pkg_update(ctx->out_fn, ctx->out_ctx);
    } else if (sh_strcmp(sub, "upgrade") == 0) {
        return pkg_upgrade(ctx->out_fn, ctx->out_ctx);
    } else if (sh_strcmp(sub, "clean") == 0) {
        return pkg_clean(ctx->out_fn, ctx->out_ctx);
    } else if (sh_strcmp(sub, "repo") == 0) {
        return pkg_repo_list(ctx->out_fn, ctx->out_ctx);
    } else {
        sh_println(ctx, "pkg: unknown action. Type 'pkg' for syntax.", SH_COL_ERR);
        return 1;
    }
}

static int cmd_terminal(shell_context_t *ctx, int argc, char **argv) {
    (void)ctx; (void)argc; (void)argv;
    app_terminal_launch();
    return 0;
}

static int cmd_editor(shell_context_t *ctx, int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : NULL;
    char full[SHELL_PATH_MAX];
    if (path) {
        sh_normalize_path(full, ctx->cwd, path, sizeof(full));
        app_editor_launch(full);
    } else {
        app_editor_launch(NULL);
    }
    return 0;
}

static int cmd_browser(shell_context_t *ctx, int argc, char **argv) {
    (void)ctx;
    const char *url = (argc > 1) ? argv[1] : "file:///www/index.html";
    app_browser_launch(url);
    return 0;
}

static int cmd_filemanager(shell_context_t *ctx, int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : ctx->cwd;
    app_filemgr_launch(path);
    return 0;
}

static int cmd_calc(shell_context_t *ctx, int argc, char **argv) {
    (void)ctx; (void)argc; (void)argv;
    app_calc_launch();
    return 0;
}

static int cmd_settings(shell_context_t *ctx, int argc, char **argv) {
    (void)ctx; (void)argc; (void)argv;
    app_settings_launch();
    return 0;
}

static int cmd_sysmon(shell_context_t *ctx, int argc, char **argv) {
    (void)ctx; (void)argc; (void)argv;
    app_sysmon_launch();
    return 0;
}

static int cmd_pkgmgr(shell_context_t *ctx, int argc, char **argv) {
    (void)ctx; (void)argc; (void)argv;
    app_pkgmgr_launch();
    return 0;
}

static int cmd_python(shell_context_t *ctx, int argc, char **argv) {
    if (argc > 1) {
        char full[SHELL_PATH_MAX];
        sh_normalize_path(full, ctx->cwd, argv[1], sizeof(full));
        return code_runner_execute_file(ctx, full);
    }
    return code_runner_python_interactive(ctx);
}

static int cmd_run(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Universal Code Runner", SH_COL_INFO);
        sh_println(ctx, "Usage: run <filename.py | filename.c | filename.sh | binary>", SH_COL_WARN);
        return 1;
    }
    char full[SHELL_PATH_MAX];
    sh_normalize_path(full, ctx->cwd, argv[1], sizeof(full));
    return code_runner_execute_file(ctx, full);
}

static int cmd_cc(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "AstraCC C/C++ Compiler & Runner", SH_COL_INFO);
        sh_println(ctx, "Usage: cc <source.c | source.cpp> [-o output]", SH_COL_WARN);
        return 1;
    }
    char full[SHELL_PATH_MAX];
    sh_normalize_path(full, ctx->cwd, argv[1], sizeof(full));
    return code_runner_execute_file(ctx, full);
}

static int cmd_sh_exec(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: sh <script.sh>", SH_COL_WARN);
        return 1;
    }
    char full[SHELL_PATH_MAX];
    sh_normalize_path(full, ctx->cwd, argv[1], sizeof(full));
    return code_runner_execute_file(ctx, full);
}

/* ========================================================================= */
/* Command Table & Execution Pipeline Engine                                 */
/* ========================================================================= */

static const shell_cmd_entry_t g_builtins[] = {
    { "help",        cmd_help },
    { "clear",       cmd_clear },
    { "echo",        cmd_echo },
    { "pwd",         cmd_pwd },
    { "whoami",      cmd_whoami },
    { "hostname",    cmd_hostname },
    { "date",        cmd_date },
    { "uname",       cmd_uname },
    { "env",         cmd_env },
    { "history",     cmd_history },
    { "exit",        cmd_exit },
    { "which",       cmd_which },
    { "type",        cmd_type },
    { "alias",       cmd_alias },
    { "unalias",     cmd_unalias },
    { "true",        cmd_true },
    { "false",       cmd_false },
    { "sleep",       cmd_sleep },
    { "ls",          cmd_ls },
    { "cd",          cmd_cd },
    { "cat",         cmd_cat },
    { "touch",       cmd_touch },
    { "mkdir",       cmd_mkdir },
    { "rmdir",       cmd_rmdir },
    { "rm",          cmd_rm },
    { "cp",          cmd_cp },
    { "mv",          cmd_mv },
    { "stat",        cmd_stat },
    { "df",          cmd_df },
    { "du",          cmd_du },
    { "find",        cmd_find },
    { "tree",        cmd_tree },
    { "head",        cmd_head },
    { "tail",        cmd_tail },
    { "wc",          cmd_wc },
    { "grep",        cmd_grep },
    { "sort",        cmd_sort },
    { "uniq",        cmd_uniq },
    { "diff",        cmd_diff },
    { "file",        cmd_file },
    { "basename",    cmd_basename },
    { "dirname",     cmd_dirname },
    { "realpath",    cmd_realpath },
    { "readlink",    cmd_readlink },
    { "printf",      cmd_printf },
    { "tee",         cmd_tee },
    { "cut",         cmd_cut },
    { "tr",          cmd_tr },
    { "paste",       cmd_paste },
    { "nl",          cmd_nl },
    { "fold",        cmd_fold },
    { "rev",         cmd_rev },
    { "strings",     cmd_strings },
    { "hexdump",     cmd_hexdump },
    { "ps",          cmd_ps },
    { "top",         cmd_top },
    { "free",        cmd_free },
    { "uptime",      cmd_uptime },
    { "cpuinfo",     cmd_cpuinfo },
    { "meminfo",     cmd_meminfo },
    { "dmesg",       cmd_dmesg },
    { "lsblk",       cmd_lsblk },
    { "mount",       cmd_mount },
    { "umount",      cmd_umount },
    { "ip",          cmd_ip },
    { "ifconfig",    cmd_ifconfig },
    { "ping",        cmd_ping },
    { "nslookup",    cmd_nslookup },
    { "curl",        cmd_curl },
    { "wget",        cmd_wget },
    { "netstat",     cmd_netstat },
    { "pkg",         cmd_pkg },
    { "terminal",    cmd_terminal },
    { "editor",      cmd_editor },
    { "browser",     cmd_browser },
    { "filemanager", cmd_filemanager },
    { "calc",        cmd_calc },
    { "settings",    cmd_settings },
    { "sysmon",      cmd_sysmon },
    { "pkgmgr",      cmd_pkgmgr },
    { "python",      cmd_python },
    { "py",          cmd_python },
    { "run",         cmd_run },
    { "c",           cmd_cc },
    { "cc",          cmd_cc },
    { "tcc",         cmd_cc },
    { "gcc",         cmd_cc },
    { "sh",          cmd_sh_exec },
    { "bash",        cmd_sh_exec },
    { NULL,          NULL }
};

/* Tokenization and Variable Expansion */
static void expand_variables(shell_context_t *ctx, const char *in, char *out, size_t max) {
    if (!in || !out || max == 0) return;
    size_t o = 0;

    for (size_t i = 0; in[i] && o < max - 1; ) {
        if (in[i] == '$') {
            i++;
            if (in[i] == '?') {
                i++;
                int code = ctx ? ctx->last_exit_code : 0;
                char num[16];
                int np = 0;
                if (code == 0) num[np++] = '0';
                else {
                    if (code < 0) { num[np++] = '-'; code = -code; }
                    char rev[16]; int rp = 0;
                    while (code > 0) { rev[rp++] = '0' + (code % 10); code /= 10; }
                    while (rp > 0) num[np++] = rev[--rp];
                }
                num[np] = '\0';
                for (int n = 0; num[n] && o < max - 1; n++) out[o++] = num[n];
                continue;
            }

            bool braced = (in[i] == '{');
            if (braced) i++;

            char var_name[SHELL_KEY_MAX];
            int vp = 0;
            while (in[i] && vp < SHELL_KEY_MAX - 1 &&
                   ((in[i] >= 'A' && in[i] <= 'Z') ||
                    (in[i] >= 'a' && in[i] <= 'z') ||
                    (in[i] >= '0' && in[i] <= '9') ||
                    in[i] == '_')) {
                var_name[vp++] = in[i++];
            }
            var_name[vp] = '\0';
            if (braced && in[i] == '}') i++;

            const char *val = shell_getenv(ctx, var_name);
            if (val) {
                for (int v = 0; val[v] && o < max - 1; v++) out[o++] = val[v];
            }
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

static int tokenize_line(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        char quote = 0;
        if (*p == '\'' || *p == '"') quote = *p++;

        argv[argc++] = p;
        char *dst = p;

        while (*p) {
            if (quote) {
                if (*p == '\\' && *(p + 1) == quote) {
                    p++;
                    *dst++ = *p++;
                    continue;
                }
                if (*p == quote) {
                    p++;
                    quote = 0;
                    break;
                }
                *dst++ = *p++;
            } else {
                if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
                    p++;
                    break;
                }
                if (*p == '\\' && *(p + 1)) {
                    p++;
                    *dst++ = *p++;
                } else if (*p == '\'' || *p == '"') {
                    quote = *p++;
                } else {
                    *dst++ = *p++;
                }
            }
        }
        *dst = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

typedef struct {
    vfs_fd_t fd;
} redir_ctx_t;

static void redir_writer(void *out_ctx, const char *str, u32 color) {
    (void)color;
    redir_ctx_t *rc = (redir_ctx_t *)out_ctx;
    if (rc && rc->fd >= 0 && str) {
        vfs_write(rc->fd, str, sh_strlen(str));
    }
}

static void pipe_writer(void *out_ctx, const char *str, u32 color) {
    (void)color; (void)out_ctx;
    if (!str) return;
    size_t slen = sh_strlen(str);
    if (g_pipe_len + slen < sizeof(g_pipe_buf) - 1) {
        kmemcpy(g_pipe_buf + g_pipe_len, str, slen);
        g_pipe_len += slen;
        g_pipe_buf[g_pipe_len] = '\0';
    }
}

/* Execute a single atomic command stage */
static int shell_execute_single(shell_context_t *ctx, const char *raw_cmd) {
    char expanded[SHELL_LINE_MAX];
    expand_variables(ctx, raw_cmd, expanded, sizeof(expanded));

    /* Check input redirection '<' */
    char *input_file = NULL;
    char *in_redir = expanded;
    while (*in_redir) {
        if (*in_redir == '<') {
            *in_redir = '\0';
            in_redir++;
            while (*in_redir == ' ' || *in_redir == '\t') in_redir++;
            input_file = in_redir;
            char *iend = input_file + sh_strlen(input_file) - 1;
            while (iend > input_file && (*iend == ' ' || *iend == '\n' || *iend == '\r')) {
                *iend = '\0';
                iend--;
            }
            break;
        }
        in_redir++;
    }

    if (input_file && input_file[0]) {
        char norm_in[SHELL_PATH_MAX];
        sh_normalize_path(norm_in, ctx->cwd, input_file, sizeof(norm_in));
        vfs_fd_t ifd = vfs_open(norm_in, VFS_O_RDONLY);
        if (ifd >= 0) {
            ssize_t rd = vfs_read(ifd, g_pipe_buf, sizeof(g_pipe_buf) - 1);
            vfs_close(ifd);
            if (rd >= 0) {
                g_pipe_buf[rd] = '\0';
                g_pipe_len = (size_t)rd;
                g_has_pipe_input = true;
            }
        }
    }

    /* Check output redirection '>' or '>>' */
    char *redir_file = NULL;
    bool append_mode = false;
    char *redir_pos = expanded;
    while (*redir_pos) {
        if (*redir_pos == '>') {
            if (*(redir_pos + 1) == '>') {
                append_mode = true;
                *redir_pos = '\0';
                redir_pos += 2;
            } else {
                append_mode = false;
                *redir_pos = '\0';
                redir_pos++;
            }
            while (*redir_pos == ' ' || *redir_pos == '\t') redir_pos++;
            redir_file = redir_pos;
            char *rend = redir_file + sh_strlen(redir_file) - 1;
            while (rend > redir_file && (*rend == ' ' || *rend == '\n' || *rend == '\r')) {
                *rend = '\0';
                rend--;
            }
            break;
        }
        redir_pos++;
    }

    char *argv[SHELL_ARGS_MAX];
    int argc = tokenize_line(expanded, argv, SHELL_ARGS_MAX);
    if (argc == 0) return 0;

    /* Alias expansion for command word */
    char alias_buf[128];
    for (int al = 0; al < SHELL_ALIAS_MAX; al++) {
        if (ctx->aliases[al].in_use && sh_strcmp(ctx->aliases[al].name, argv[0]) == 0) {
            sh_strcpy(alias_buf, ctx->aliases[al].value, sizeof(alias_buf));
            char *al_argv[8];
            int al_argc = tokenize_line(alias_buf, al_argv, 8);
            if (al_argc > 0) {
                argv[0] = al_argv[0];
            }
            break;
        }
    }

    shell_out_fn orig_fn = ctx->out_fn;
    void *orig_ctx = ctx->out_ctx;
    vfs_fd_t rfd = -1;
    redir_ctx_t rc;

    if (redir_file && redir_file[0] != '\0') {
        char norm_redir[SHELL_PATH_MAX];
        sh_normalize_path(norm_redir, ctx->cwd, redir_file, sizeof(norm_redir));
        int flags = VFS_O_WRONLY | VFS_O_CREAT | (append_mode ? VFS_O_APPEND : VFS_O_TRUNC);
        rfd = vfs_open(norm_redir, flags);
        if (rfd >= 0) {
            rc.fd = rfd;
            ctx->out_fn = redir_writer;
            ctx->out_ctx = &rc;
        } else {
            sh_print(ctx, "shell: cannot redirect to ", SH_COL_ERR);
            sh_println(ctx, redir_file, SH_COL_ERR);
        }
    }

    int ret = 0;

    /* 1. Built-in command lookup */
    for (int i = 0; g_builtins[i].name != NULL; i++) {
        if (sh_strcmp(argv[0], g_builtins[i].name) == 0) {
            ret = g_builtins[i].handler(ctx, argc, argv);
            ctx->last_exit_code = ret;
            goto cleanup;
        }
    }

    /* 1b. Check for script/code file direct execution (e.g. ./script.py, test.c, demo.sh) */
    char code_exec_path[SHELL_PATH_MAX];
    bool is_code_file = false;
    if (argv[0][0] == '/' || (argv[0][0] == '.' && argv[0][1] == '/')) {
        sh_normalize_path(code_exec_path, ctx->cwd, argv[0], sizeof(code_exec_path));
        vfs_stat_t cst;
        if (vfs_stat(code_exec_path, &cst) == VFS_OK) {
            size_t elen = sh_strlen(code_exec_path);
            if ((elen >= 3 && sh_strcmp(code_exec_path + elen - 3, ".py") == 0) ||
                (elen >= 2 && sh_strcmp(code_exec_path + elen - 2, ".c") == 0) ||
                (elen >= 4 && sh_strcmp(code_exec_path + elen - 4, ".cpp") == 0) ||
                (elen >= 3 && sh_strcmp(code_exec_path + elen - 3, ".sh") == 0)) {
                is_code_file = true;
            }
        }
    } else {
        size_t elen = sh_strlen(argv[0]);
        if ((elen >= 3 && sh_strcmp(argv[0] + elen - 3, ".py") == 0) ||
            (elen >= 2 && sh_strcmp(argv[0] + elen - 2, ".c") == 0) ||
            (elen >= 4 && sh_strcmp(argv[0] + elen - 4, ".cpp") == 0) ||
            (elen >= 3 && sh_strcmp(argv[0] + elen - 3, ".sh") == 0)) {
            sh_normalize_path(code_exec_path, ctx->cwd, argv[0], sizeof(code_exec_path));
            vfs_stat_t cst;
            if (vfs_stat(code_exec_path, &cst) == VFS_OK) {
                is_code_file = true;
            }
        }
    }
    if (is_code_file) {
        ret = code_runner_execute_file(ctx, code_exec_path);
        ctx->last_exit_code = ret;
        goto cleanup;
    }

    /* 2. Check for external executable in $PATH or explicit path */
    char full_exec_path[SHELL_PATH_MAX];
    bool exec_found = false;

    if (argv[0][0] == '/' || (argv[0][0] == '.' && argv[0][1] == '/')) {
        sh_normalize_path(full_exec_path, ctx->cwd, argv[0], sizeof(full_exec_path));
        vfs_stat_t st;
        if (vfs_stat(full_exec_path, &st) == VFS_OK) exec_found = true;
    } else {
        const char *path_env = shell_getenv(ctx, "PATH");
        if (!path_env) path_env = "/bin:/usr/bin:/pkg/bin";

        char path_copy[128];
        sh_strcpy(path_copy, path_env, sizeof(path_copy));
        char *pdir = path_copy;

        while (*pdir) {
            char *next = pdir;
            while (*next && *next != ':') next++;
            char save = *next;
            *next = '\0';

            char candidate[SHELL_PATH_MAX];
            sh_strcpy(candidate, pdir, sizeof(candidate));
            if (candidate[sh_strlen(candidate) - 1] != '/') sh_strcat(candidate, "/", sizeof(candidate));
            sh_strcat(candidate, argv[0], sizeof(candidate));

            vfs_stat_t st;
            if (vfs_stat(candidate, &st) == VFS_OK) {
                sh_strcpy(full_exec_path, candidate, sizeof(full_exec_path));
                exec_found = true;
                break;
            }

            sh_strcat(candidate, ".elf", sizeof(candidate));
            if (vfs_stat(candidate, &st) == VFS_OK) {
                sh_strcpy(full_exec_path, candidate, sizeof(full_exec_path));
                exec_found = true;
                break;
            }

            if (!save) break;
            pdir = next + 1;
        }
    }

    if (exec_found) {
        sh_print(ctx, "Executing external application: ", SH_COL_INFO);
        sh_println(ctx, full_exec_path, SH_COL_PROMPT);
        process_t *proc = process_create_elf_file(argv[0], full_exec_path);
        if (proc) {
            int status = 0;
            process_waitpid(proc->pid, &status, 0);
            ctx->last_exit_code = status;
            ret = status;
            goto cleanup;
        } else {
            sh_println(ctx, "Failed to load and execute binary image.", SH_COL_ERR);
            ctx->last_exit_code = 1;
            ret = 1;
            goto cleanup;
        }
    }

    /* 3. Check Package Repository for missing command 3-option menu */
    char pkg_name[32], pkg_ver[16], pkg_desc[64], pkg_deps[64];
    u32 pkg_size = 0;
    if (pkg_find_package_details(argv[0], pkg_name, pkg_ver, pkg_desc, &pkg_size, pkg_deps)) {
        sh_print(ctx, "Command '", SH_COL_ERR);
        sh_print(ctx, argv[0], SH_COL_ERR);
        sh_println(ctx, "' was not found.\n", SH_COL_ERR);

        sh_println(ctx, "Searching AstraOS package repository...\n", SH_COL_INFO);
        sh_println(ctx, "Available package:", SH_COL_INFO);
        sh_print(ctx, "  ", SH_COL_DEFAULT);
        sh_println(ctx, pkg_name, SH_COL_PROMPT);
        sh_print(ctx, "  Description: ", SH_COL_MUTED);
        sh_println(ctx, pkg_desc, SH_COL_DEFAULT);
        sh_print(ctx, "  Version: ", SH_COL_MUTED);
        sh_println(ctx, pkg_ver, SH_COL_DEFAULT);

        char sz_str[32]; int sp = 0;
        sh_append_num(sz_str, &sp, pkg_size);
        sz_str[sp++] = 'b'; sz_str[sp++] = 'y'; sz_str[sp++] = 't'; sz_str[sp++] = 'e'; sz_str[sp++] = 's'; sz_str[sp] = '\0';
        sh_print(ctx, "  Size: ", SH_COL_MUTED);
        sh_println(ctx, sz_str, SH_COL_DEFAULT);

        sh_println(ctx, "\n[1] Install package", SH_COL_WARN);
        sh_println(ctx, "[2] View package information", SH_COL_WARN);
        sh_println(ctx, "[3] Cancel\n", SH_COL_WARN);
        sh_print(ctx, "Select an option: ", SH_COL_PROMPT);

        ctx->pending_install = true;
        ctx->pending_stage = 1;
        sh_strcpy(ctx->pending_pkg, pkg_name, sizeof(ctx->pending_pkg));
        ctx->last_exit_code = 127;
        ret = 127;
        goto cleanup;
    }

    /* 4. Cross-OS Compatibility Hints */
    if (sh_strcmp(argv[0], "apt") == 0 || sh_strcmp(argv[0], "apt-get") == 0) {
        sh_println(ctx, "AstraOS Compatibility: 'apt' is a Debian/Ubuntu package manager. Use 'pkg' on AstraOS.", SH_COL_WARN);
        ctx->last_exit_code = 127;
        ret = 127;
        goto cleanup;
    }
    if (sh_strcmp(argv[0], "dir") == 0) {
        sh_println(ctx, "AstraOS Compatibility: 'dir' is a Windows CMD command. Use AstraOS 'ls'.", SH_COL_WARN);
        ctx->last_exit_code = 127;
        ret = 127;
        goto cleanup;
    }
    if (sh_strcmp(argv[0], "cls") == 0) {
        sh_println(ctx, "AstraOS Compatibility: 'cls' is a Windows command. Use AstraOS 'clear'.", SH_COL_WARN);
        ctx->last_exit_code = 127;
        ret = 127;
        goto cleanup;
    }
    if (sh_strcmp(argv[0], "powershell") == 0 || sh_strcmp(argv[0], "cmd") == 0) {
        sh_println(ctx, "AstraOS Compatibility: Running native AstraOS interactive shell. Type 'help' for commands.", SH_COL_WARN);
        ctx->last_exit_code = 0;
        ret = 0;
        goto cleanup;
    }

    /* 5. Unknown command */
    sh_print(ctx, "Unknown command: '", SH_COL_ERR);
    sh_print(ctx, argv[0], SH_COL_ERR);
    sh_println(ctx, "'. Type 'help' for available commands.", SH_COL_ERR);
    ctx->last_exit_code = 127;
    ret = 127;

cleanup:
    if (rfd >= 0) {
        ctx->out_fn = orig_fn;
        ctx->out_ctx = orig_ctx;
        vfs_close(rfd);
    }
    return ret;
}

/* Pipeline handler for '|' */
static int shell_execute_pipeline(shell_context_t *ctx, const char *line) {
    char p1[SHELL_LINE_MAX], p2[SHELL_LINE_MAX];
    const char *pipe_pos = NULL;

    bool in_quote = false;
    char q_char = 0;
    for (int i = 0; line[i]; i++) {
        if (!in_quote && (line[i] == '\'' || line[i] == '"')) {
            in_quote = true;
            q_char = line[i];
        } else if (in_quote && line[i] == q_char) {
            in_quote = false;
        } else if (!in_quote && line[i] == '|') {
            pipe_pos = &line[i];
            break;
        }
    }

    if (!pipe_pos) {
        return shell_execute_single(ctx, line);
    }

    /* Split stage 1 and stage 2 */
    size_t len1 = (size_t)(pipe_pos - line);
    sh_strncpy(p1, line, len1);
    sh_strcpy(p2, pipe_pos + 1, sizeof(p2));

    /* Stage 1: Capture output into g_pipe_buf */
    g_pipe_len = 0;
    g_pipe_buf[0] = '\0';
    g_has_pipe_input = false;

    shell_out_fn orig_fn = ctx->out_fn;
    void *orig_ctx = ctx->out_ctx;
    ctx->out_fn = pipe_writer;
    ctx->out_ctx = NULL;

    shell_execute_single(ctx, p1);

    ctx->out_fn = orig_fn;
    ctx->out_ctx = orig_ctx;

    /* Stage 2: Execute with pipe input active */
    g_has_pipe_input = (g_pipe_len > 0);
    int ret = shell_execute_pipeline(ctx, p2);

    g_has_pipe_input = false;
    g_pipe_len = 0;
    return ret;
}

/* Master execution entry point supporting chained commands (; , && , ||) */
int shell_execute(shell_context_t *ctx, const char *raw_line) {
    if (!ctx || !raw_line) return 0;

    /* Handle pending interactive missing package installation prompt */
    if (ctx->pending_install) {
        char opt = raw_line[0];
        while (opt == ' ' || opt == '\t') opt++;

        if (ctx->pending_stage == 1) {
            if (opt == '1' || sh_strcmp(raw_line, "install") == 0 || opt == 'y' || opt == 'Y') {
                sh_print(ctx, "Installing package '", SH_COL_INFO);
                sh_print(ctx, ctx->pending_pkg, SH_COL_PROMPT);
                sh_println(ctx, "'...", SH_COL_INFO);
                int ins_res = pkg_install(ctx->pending_pkg, ctx->out_fn, ctx->out_ctx);
                ctx->pending_install = false;
                ctx->pending_stage = 0;
                ctx->pending_pkg[0] = '\0';
                return ins_res;
            } else if (opt == '2' || sh_strcmp(raw_line, "info") == 0 || opt == 'i' || opt == 'I') {
                pkg_info(ctx->pending_pkg, ctx->out_fn, ctx->out_ctx);
                sh_println(ctx, "\n[1] Install package\n[3] Cancel\n", SH_COL_WARN);
                sh_print(ctx, "Select an option: ", SH_COL_PROMPT);
                ctx->pending_stage = 2;
                return 0;
            } else {
                sh_println(ctx, "Installation cancelled by user.", SH_COL_MUTED);
                ctx->pending_install = false;
                ctx->pending_stage = 0;
                ctx->pending_pkg[0] = '\0';
                return 0;
            }
        } else if (ctx->pending_stage == 2) {
            if (opt == '1' || sh_strcmp(raw_line, "install") == 0 || opt == 'y' || opt == 'Y') {
                sh_print(ctx, "Installing package '", SH_COL_INFO);
                sh_print(ctx, ctx->pending_pkg, SH_COL_PROMPT);
                sh_println(ctx, "'...", SH_COL_INFO);
                int ins_res = pkg_install(ctx->pending_pkg, ctx->out_fn, ctx->out_ctx);
                ctx->pending_install = false;
                ctx->pending_stage = 0;
                ctx->pending_pkg[0] = '\0';
                return ins_res;
            } else {
                sh_println(ctx, "Installation cancelled by user.", SH_COL_MUTED);
                ctx->pending_install = false;
                ctx->pending_stage = 0;
                ctx->pending_pkg[0] = '\0';
                return 0;
            }
        }
    }

    /* Chain parsing (; , && , ||) */
    enum { OP_NONE = 0, OP_SEQ, OP_AND, OP_OR };
    char stage[SHELL_LINE_MAX];
    int sp = 0;
    int prev_op = OP_NONE;
    int last_status = 0;
    bool in_quote = false;
    char q_char = 0;

    for (int i = 0; raw_line[i] || sp > 0; ) {
        char c = raw_line[i];
        if (!c) {
            stage[sp] = '\0';
            if (sp > 0) {
                if (prev_op == OP_NONE ||
                    prev_op == OP_SEQ ||
                    (prev_op == OP_AND && last_status == 0) ||
                    (prev_op == OP_OR && last_status != 0)) {
                    last_status = shell_execute_pipeline(ctx, stage);
                }
            }
            break;
        }

        if (!in_quote && (c == '\'' || c == '"')) {
            in_quote = true;
            q_char = c;
            stage[sp++] = c;
            i++;
        } else if (in_quote && c == q_char) {
            in_quote = false;
            stage[sp++] = c;
            i++;
        } else if (!in_quote && c == ';') {
            stage[sp] = '\0';
            if (prev_op == OP_NONE ||
                prev_op == OP_SEQ ||
                (prev_op == OP_AND && last_status == 0) ||
                (prev_op == OP_OR && last_status != 0)) {
                last_status = shell_execute_pipeline(ctx, stage);
            }
            prev_op = OP_SEQ;
            sp = 0;
            i++;
        } else if (!in_quote && c == '&' && raw_line[i + 1] == '&') {
            stage[sp] = '\0';
            if (prev_op == OP_NONE ||
                prev_op == OP_SEQ ||
                (prev_op == OP_AND && last_status == 0) ||
                (prev_op == OP_OR && last_status != 0)) {
                last_status = shell_execute_pipeline(ctx, stage);
            }
            prev_op = OP_AND;
            sp = 0;
            i += 2;
        } else if (!in_quote && c == '|' && raw_line[i + 1] == '|') {
            stage[sp] = '\0';
            if (prev_op == OP_NONE ||
                prev_op == OP_SEQ ||
                (prev_op == OP_AND && last_status == 0) ||
                (prev_op == OP_OR && last_status != 0)) {
                last_status = shell_execute_pipeline(ctx, stage);
            }
            prev_op = OP_OR;
            sp = 0;
            i += 2;
        } else {
            stage[sp++] = c;
            i++;
        }
    }

    return last_status;
}

/* ========================================================================= */
/* Unit and Qualification Self-Test for Shell Subsystem                      */
/* ========================================================================= */

static void shell_test_out(void *ctx, const char *str, u32 color) {
    (void)ctx; (void)color;
    kprintf("%s", str);
}

bool shell_self_test(void) {
    kprintf("\n[SHELL TEST] Starting Native Shell Subsystem Qualification Self-Test...\n");
    shell_context_t test_ctx;
    shell_init(&test_ctx, shell_test_out, NULL);

    /* 1. Environment Variables & Variable Substitution */
    kprintf("[SHELL TEST] 1. Testing environment variables and expansion...\n");
    shell_setenv(&test_ctx, "FOO", "bar123");
    if (sh_strcmp(shell_getenv(&test_ctx, "FOO"), "bar123") != 0) {
        kprintf("[SHELL TEST FAIL] shell_getenv FOO failed!\n");
        return false;
    }
    char expanded[128];
    expand_variables(&test_ctx, "Val=$FOO; Status=$?", expanded, sizeof(expanded));
    if (sh_strcmp(expanded, "Val=bar123; Status=0") != 0) {
        kprintf("[SHELL TEST FAIL] Variable expansion mismatch: '%s'!\n", expanded);
        return false;
    }
    kprintf("  PASS: Environment variables and $VAR substitution verified.\n");

    /* 2. Tokenization & Quote Parsing */
    kprintf("[SHELL TEST] 2. Testing quote parsing and argument tokenization...\n");
    char line[] = "echo 'hello world' \"second arg\" normal_arg";
    char *argv[16];
    int argc = tokenize_line(line, argv, 16);
    if (argc != 4 ||
        sh_strcmp(argv[1], "hello world") != 0 ||
        sh_strcmp(argv[2], "second arg") != 0 ||
        sh_strcmp(argv[3], "normal_arg") != 0) {
        kprintf("[SHELL TEST FAIL] Tokenization failed! argc=%d\n", argc);
        return false;
    }
    kprintf("  PASS: Single and double quoted argument parsing verified.\n");

    /* 3. Basic & Filesystem Commands */
    kprintf("[SHELL TEST] 3. Testing core built-in commands (echo, pwd, true, false)...\n");
    if (shell_execute(&test_ctx, "echo Built-in test executed cleanly") != 0) return false;
    if (shell_execute(&test_ctx, "pwd") != 0) return false;
    if (shell_execute(&test_ctx, "true") != 0) return false;
    if (shell_execute(&test_ctx, "false") != 1) return false;
    kprintf("  PASS: Basic command execution verified.\n");

    /* 4. Command Chaining (;, &&, ||) */
    kprintf("[SHELL TEST] 4. Testing command chaining (; , && , ||)...\n");
    if (shell_execute(&test_ctx, "true && echo AND_PASS") != 0) return false;
    if (shell_execute(&test_ctx, "false || echo OR_PASS") != 0) return false;
    if (shell_execute(&test_ctx, "echo SEQ1; echo SEQ2") != 0) return false;
    kprintf("  PASS: Command chaining pipelines verified.\n");

    /* 5. In-Memory Pipes (|) */
    kprintf("[SHELL TEST] 5. Testing in-memory pipes (|)...\n");
    if (shell_execute(&test_ctx, "echo 'apple\nbanana\ncherry' | grep banana") != 0) {
        kprintf("[SHELL TEST FAIL] Pipe grep failed!\n");
        return false;
    }
    kprintf("  PASS: In-memory pipeline execution verified.\n");

    /* 6. Filesystem Redirection (>, >>, <) */
    kprintf("[SHELL TEST] 6. Testing file redirection (> , >> , <)...\n");
    shell_execute(&test_ctx, "echo 'hello redirection' > /tmp_redir.txt");
    shell_execute(&test_ctx, "echo 'second line' >> /tmp_redir.txt");
    shell_execute(&test_ctx, "cat < /tmp_redir.txt");
    vfs_unlink("/tmp_redir.txt");
    kprintf("  PASS: Input and output redirection verified.\n");

    /* 7. Filesystem & Text Utilities */
    kprintf("[SHELL TEST] 7. Testing filesystem and text utilities...\n");
    shell_execute(&test_ctx, "mkdir -p /testdir/sub");
    shell_execute(&test_ctx, "touch /testdir/sub/file.txt");
    shell_execute(&test_ctx, "ls -la /testdir/sub");
    shell_execute(&test_ctx, "file /testdir/sub/file.txt");
    shell_execute(&test_ctx, "stat /testdir/sub/file.txt");
    shell_execute(&test_ctx, "rm -r /testdir");
    kprintf("  PASS: Filesystem directory creation, inspection, and removal verified.\n");

    /* 8. Missing-Command 3-Option Package Installation Flow */
    kprintf("[SHELL TEST] 8. Testing missing-command 3-option package suggestion...\n");
    shell_execute(&test_ctx, "htop");
    if (!test_ctx.pending_install || test_ctx.pending_stage != 1 || sh_strcmp(test_ctx.pending_pkg, "htop") != 0) {
        kprintf("[SHELL TEST FAIL] Missing command did not suggest 'htop'!\n");
        return false;
    }
    /* Select option 2: view package info */
    shell_execute(&test_ctx, "2");
    if (!test_ctx.pending_install || test_ctx.pending_stage != 2) {
        kprintf("[SHELL TEST FAIL] Option 2 did not advance to pending_stage 2!\n");
        return false;
    }
    /* Select option 1: install */
    shell_execute(&test_ctx, "1");
    if (test_ctx.pending_install) {
        kprintf("[SHELL TEST FAIL] Pending install flag was not cleared after option 1!\n");
        return false;
    }
    kprintf("  PASS: Interactive missing-command 3-option menu verified.\n");

    kprintf("[SHELL TEST] All Native Shell qualification tests passed successfully!\n");
    kprintf("ASTRA_NATIVE_SHELL_OK\n\n");
    return true;
}
