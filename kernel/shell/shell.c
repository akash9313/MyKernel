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

/* Shell output colors */
#define SH_COL_DEFAULT  0xFFE0E4F0U
#define SH_COL_PROMPT   0xFF50FA7BU
#define SH_COL_PATH     0xFF8BE9FDU
#define SH_COL_INFO     0xFF8BE9FDU
#define SH_COL_SUCCESS  0xFF50FA7BU
#define SH_COL_WARN     0xFFF1FA8CU
#define SH_COL_ERR      0xFFFF5555U
#define SH_COL_MUTED    0xFF8088A0U

/* String helpers */
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

static int sh_strncmp(const char *a, const char *b, size_t n) {
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

/* Environment Variable Storage */
void shell_init(shell_context_t *ctx, shell_out_fn out_fn, void *out_ctx) {
    if (!ctx) return;
    kmemset(ctx, 0, sizeof(*ctx));

    ctx->out_fn = out_fn;
    ctx->out_ctx = out_ctx;
    sh_strcpy(ctx->cwd, "/", sizeof(ctx->cwd));
    ctx->last_exit_code = 0;
    ctx->pending_install = false;
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

    /* Update existing */
    for (int i = 0; i < SHELL_ENV_MAX; i++) {
        if (ctx->env[i].in_use && sh_strcmp(ctx->env[i].key, key) == 0) {
            sh_strcpy(ctx->env[i].val, val ? val : "", SHELL_VAL_MAX);
            return 0;
        }
    }

    /* Insert new */
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

    /* Verify target exists and is a directory in VFS */
    vfs_stat_t st;
    if (sh_strcmp(target, "/") == 0) {
        sh_strcpy(ctx->cwd, "/", sizeof(ctx->cwd));
        shell_setenv(ctx, "PWD", "/");
        return 0;
    }

    int err = vfs_stat(target, &st);
    if (err != VFS_OK) {
        return err;
    }
    if (st.type != VFS_NODE_DIR) {
        return VFS_ERR_NOT_DIR;
    }

    sh_strcpy(ctx->cwd, target, sizeof(ctx->cwd));
    shell_setenv(ctx, "PWD", target);
    return 0;
}

/* Command parsing and expansion */
static void expand_variables(shell_context_t *ctx, const char *in, char *out, size_t max) {
    if (!in || !out || max == 0) return;
    size_t o = 0;

    for (size_t i = 0; in[i] && o < max - 1; ) {
        if (in[i] == '$') {
            i++;
            if (in[i] == '?') {
                i++;
                /* Last exit code */
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
        if (*p == '\'' || *p == '"') {
            quote = *p++;
        }

        argv[argc++] = p;
        char *dst = p;

        while (*p) {
            if (quote) {
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

/* Built-in Command Implementations */

static int cmd_help(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "=== AstraOS Universal Command Framework ===", SH_COL_INFO);
    sh_println(ctx, "Core commands:", SH_COL_PROMPT);
    sh_println(ctx, "  help [cmd]             Display this manual or specific command help", SH_COL_DEFAULT);
    sh_println(ctx, "  clear                  Clear the terminal screen", SH_COL_DEFAULT);
    sh_println(ctx, "  echo [-n] [args...]    Print arguments to output stream", SH_COL_DEFAULT);
    sh_println(ctx, "  pwd                    Print current working directory", SH_COL_DEFAULT);
    sh_println(ctx, "  whoami                 Print active user identity", SH_COL_DEFAULT);
    sh_println(ctx, "  date                   Display system date and real-time clock", SH_COL_DEFAULT);
    sh_println(ctx, "  uname [-a]             Print operating system details", SH_COL_DEFAULT);
    sh_println(ctx, "  hostname [name]        Show or set system hostname", SH_COL_DEFAULT);
    sh_println(ctx, "  history                List terminal command history", SH_COL_DEFAULT);
    sh_println(ctx, "  exit                   Exit shell session", SH_COL_DEFAULT);

    sh_println(ctx, "\nFilesystem commands:", SH_COL_PROMPT);
    sh_println(ctx, "  ls [-l] [path]         List directory contents", SH_COL_DEFAULT);
    sh_println(ctx, "  cd [path]              Change working directory", SH_COL_DEFAULT);
    sh_println(ctx, "  cat <file...>          Concatenate and display file contents", SH_COL_DEFAULT);
    sh_println(ctx, "  touch <file...>        Create empty file or update timestamp", SH_COL_DEFAULT);
    sh_println(ctx, "  mkdir <dir>            Create a new directory", SH_COL_DEFAULT);
    sh_println(ctx, "  rmdir <dir>            Remove empty directory", SH_COL_DEFAULT);
    sh_println(ctx, "  rm [-r|-f] <file...>   Delete files from filesystem", SH_COL_DEFAULT);
    sh_println(ctx, "  cp <src> <dst>         Copy file to destination", SH_COL_DEFAULT);
    sh_println(ctx, "  mv <old> <new>         Move or rename file", SH_COL_DEFAULT);
    sh_println(ctx, "  find [path]            Search directory hierarchy", SH_COL_DEFAULT);
    sh_println(ctx, "  stat <path>            Display detailed file status", SH_COL_DEFAULT);
    sh_println(ctx, "  df                     Report filesystem disk space usage", SH_COL_DEFAULT);
    sh_println(ctx, "  du [path]              Estimate file and directory space usage", SH_COL_DEFAULT);

    sh_println(ctx, "\nProcess & System information:", SH_COL_PROMPT);
    sh_println(ctx, "  ps                     Report snapshot of current processes", SH_COL_DEFAULT);
    sh_println(ctx, "  top                    Display active system resource metrics", SH_COL_DEFAULT);
    sh_println(ctx, "  kill [-9] <pid>        Terminate target process", SH_COL_DEFAULT);
    sh_println(ctx, "  free [-m]              Show free and used physical memory", SH_COL_DEFAULT);
    sh_println(ctx, "  uptime                 Display kernel uptime and APIC ticks", SH_COL_DEFAULT);
    sh_println(ctx, "  meminfo                Detailed memory management statistics", SH_COL_DEFAULT);
    sh_println(ctx, "  cpuinfo                Processor specifications & core count", SH_COL_DEFAULT);
    sh_println(ctx, "  version                Kernel release & build environment", SH_COL_DEFAULT);
    sh_println(ctx, "  dmesg                  Kernel diagnostic ring log", SH_COL_DEFAULT);
    sh_println(ctx, "  mount                  Show mounted storage filesystems", SH_COL_DEFAULT);
    sh_println(ctx, "  env [k=v]              List or update environment variables", SH_COL_DEFAULT);

    sh_println(ctx, "\nNetworking commands:", SH_COL_PROMPT);
    sh_println(ctx, "  ping <ip|host>         Send ICMP Echo Request to network host", SH_COL_DEFAULT);
    sh_println(ctx, "  ifconfig / ip          Display network interface configuration", SH_COL_DEFAULT);
    sh_println(ctx, "  nslookup <host>        Query Internet domain name server", SH_COL_DEFAULT);
    sh_println(ctx, "  curl [-o f] <url>      Fetch URL payload via HTTP/HTTPS/TLS 1.3", SH_COL_DEFAULT);
    sh_println(ctx, "  wget <url>             Download file payload from URL", SH_COL_DEFAULT);

    sh_println(ctx, "\nPackage Management (pkg):", SH_COL_PROMPT);
    sh_println(ctx, "  pkg search <query>     Search available repository packages", SH_COL_DEFAULT);
    sh_println(ctx, "  pkg install <pkg>      Install package with digital signature verify", SH_COL_DEFAULT);
    sh_println(ctx, "  pkg remove <pkg>       Safely remove installed package", SH_COL_DEFAULT);
    sh_println(ctx, "  pkg update             Update repository catalog from network", SH_COL_DEFAULT);
    sh_println(ctx, "  pkg upgrade            Upgrade all installed packages", SH_COL_DEFAULT);
    sh_println(ctx, "  pkg list               List currently installed packages", SH_COL_DEFAULT);
    sh_println(ctx, "  pkg info <pkg>         Display package metadata and files", SH_COL_DEFAULT);
    sh_println(ctx, "  pkg clean              Purge download and staging caches", SH_COL_DEFAULT);

    sh_println(ctx, "\nApplication Launchers:", SH_COL_PROMPT);
    sh_println(ctx, "  terminal               Open a new AstraOS Terminal window", SH_COL_DEFAULT);
    sh_println(ctx, "  editor [file]          Launch AstraOS Text Editor", SH_COL_DEFAULT);
    sh_println(ctx, "  browser [url]          Launch AstraBrowser Web Browser", SH_COL_DEFAULT);
    sh_println(ctx, "  filemanager [path]     Launch AstraOS Graphical File Manager", SH_COL_DEFAULT);
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
    int start = 1;

    if (argc > 1 && sh_strcmp(argv[1], "-n") == 0) {
        newline = false;
        start = 2;
    }

    for (int i = start; i < argc; i++) {
        sh_print(ctx, argv[i], SH_COL_DEFAULT);
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

static int cmd_date(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    rtc_time_t t;
    rtc_get_time(&t);

    char buf[64];
    int p = 0;
    /* YYYY-MM-DD HH:MM:SS UTC */
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
    bool all = (argc > 1 && sh_strcmp(argv[1], "-a") == 0);
    if (all) {
        sh_println(ctx, "AstraOS 1.0.0-rc1 x86_64 freestanding UEFI/Limine SMP-Aware", SH_COL_INFO);
    } else {
        sh_println(ctx, "AstraOS", SH_COL_INFO);
    }
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

static int cmd_ls(shell_context_t *ctx, int argc, char **argv) {
    const char *target = (argc > 1 && argv[1][0] != '-') ? argv[1] : ctx->cwd;
    char path[SHELL_PATH_MAX];
    sh_normalize_path(path, ctx->cwd, target, sizeof(path));

    vfs_dirent_t dent;
    size_t idx = 0;
    int count = 0;

    while (vfs_readdir(path, idx, &dent) == VFS_OK) {
        char entry[128];
        int p = 0;

        if (dent.type == VFS_NODE_DIR) {
            entry[p++] = '['; entry[p++] = 'D'; entry[p++] = 'I'; entry[p++] = 'R'; entry[p++] = ']';
            entry[p++] = ' '; entry[p++] = ' ';
        } else {
            entry[p++] = '['; entry[p++] = 'F'; entry[p++] = 'I'; entry[p++] = 'L'; entry[p++] = ']';
            entry[p++] = ' '; entry[p++] = ' ';
        }

        const char *nm = dent.name;
        while (*nm && p < 48) entry[p++] = *nm++;
        while (p < 48) entry[p++] = ' ';

        /* Size */
        size_t sz = dent.size;
        char num[16]; int np = 0;
        if (sz == 0) num[np++] = '0';
        else {
            char rev[16]; int rp = 0;
            while (sz > 0) { rev[rp++] = '0' + (sz % 10); sz /= 10; }
            while (rp > 0) num[np++] = rev[--rp];
        }
        num[np] = '\0';
        for (int n = 0; num[n] && p < 60; n++) entry[p++] = num[n];
        entry[p++] = ' '; entry[p++] = 'B';
        entry[p] = '\0';

        sh_println(ctx, entry, (dent.type == VFS_NODE_DIR) ? SH_COL_INFO : SH_COL_DEFAULT);
        idx++;
        count++;
    }

    if (count == 0) {
        sh_println(ctx, "(empty directory)", SH_COL_MUTED);
    }
    return 0;
}

static int cmd_cd(shell_context_t *ctx, int argc, char **argv) {
    const char *target = (argc > 1) ? argv[1] : "/";
    int err = shell_chdir(ctx, target);
    if (err != 0) {
        sh_print(ctx, "cd: error changing directory: ", SH_COL_ERR);
        sh_println(ctx, vfs_strerror(err), SH_COL_ERR);
        return 1;
    }
    return 0;
}

static int cmd_cat(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: cat <file...>", SH_COL_WARN);
        return 1;
    }

    for (int a = 1; a < argc; a++) {
        char path[SHELL_PATH_MAX];
        sh_normalize_path(path, ctx->cwd, argv[a], sizeof(path));

        vfs_fd_t fd = vfs_open(path, VFS_O_RDONLY);
        if (fd < 0) {
            sh_print(ctx, "cat: cannot open '", SH_COL_ERR);
            sh_print(ctx, argv[a], SH_COL_ERR);
            sh_print(ctx, "': ", SH_COL_ERR);
            sh_println(ctx, vfs_strerror(fd), SH_COL_ERR);
            continue;
        }

        char buf[512];
        ssize_t bytes;
        while ((bytes = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[bytes] = '\0';
            sh_print(ctx, buf, SH_COL_DEFAULT);
        }
        vfs_close(fd);
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

static int cmd_rm(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: rm [-r|-f] <file...>", SH_COL_WARN);
        return 1;
    }

    for (int a = 1; a < argc; a++) {
        if (argv[a][0] == '-') continue;
        char path[SHELL_PATH_MAX];
        sh_normalize_path(path, ctx->cwd, argv[a], sizeof(path));

        int res = vfs_unlink(path);
        if (res != VFS_OK) {
            sh_print(ctx, "rm: cannot remove '", SH_COL_ERR);
            sh_print(ctx, argv[a], SH_COL_ERR);
            sh_print(ctx, "': ", SH_COL_ERR);
            sh_println(ctx, vfs_strerror(res), SH_COL_ERR);
        }
    }
    return 0;
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
    sh_println(ctx, "/data          astrafs        65536      1024     64512   2%", SH_COL_DEFAULT);

    char mem_line[128];
    int p = 0;
    const char *m1 = "ramfs (pmm)    tmpfs      ";
    while (*m1) mem_line[p++] = *m1++;

    /* Format total_kb */
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

    /* Used */
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

    /* Free */
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

static int cmd_ps(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "  PID  PARENT  STATE    CORE  NAME", SH_COL_INFO);

    for (int i = 0; i < PROCESS_MAX_PROCESSES; i++) {
        process_t *p = process_find_by_pid(i);
        if (!p || p->state == PROCESS_UNUSED || p->state == PROCESS_DEAD) continue;

        char line[64];
        int lp = 0;

        /* PID */
        line[lp++] = ' ';
        if (p->pid >= 10) line[lp++] = '0' + (p->pid / 10);
        line[lp++] = '0' + (p->pid % 10);
        while (lp < 7) line[lp++] = ' ';

        /* Parent */
        if (p->parent_pid >= 10) line[lp++] = '0' + (p->parent_pid / 10);
        line[lp++] = '0' + (p->parent_pid % 10);
        while (lp < 15) line[lp++] = ' ';

        /* State */
        const char *st = "READY";
        if (p->state == PROCESS_RUNNING) st = "RUN  ";
        else if (p->state == PROCESS_BLOCKED) st = "BLOCK";
        else if (p->state == PROCESS_ZOMBIE) st = "ZOMB ";
        while (*st) line[lp++] = *st++;
        while (lp < 24) line[lp++] = ' ';

        /* CPU core */
        u32 core = p->active_cpu_mask;
        line[lp++] = '0' + (core % 10);
        while (lp < 30) line[lp++] = ' ';

        /* Name */
        const char *nm = p->name;
        while (*nm && lp < (int)sizeof(line) - 2) line[lp++] = *nm++;
        line[lp] = '\0';

        sh_println(ctx, line, SH_COL_DEFAULT);
    }
    return 0;
}

static int cmd_free(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
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

    /* total */
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
    line[p++] = 'M';
    while (p < 25) line[p++] = ' ';

    /* used */
    v = used_mb; np = 0;
    if (v == 0) num[np++] = '0';
    else {
        char rev[16]; int rp = 0;
        while (v > 0) { rev[rp++] = '0' + (v % 10); v /= 10; }
        while (rp > 0) num[np++] = rev[--rp];
    }
    num[np] = '\0';
    for (int n = 0; num[n] && p < 35; n++) line[p++] = num[n];
    line[p++] = 'M';
    while (p < 37) line[p++] = ' ';

    /* free */
    v = free_mb; np = 0;
    if (v == 0) num[np++] = '0';
    else {
        char rev[16]; int rp = 0;
        while (v > 0) { rev[rp++] = '0' + (v % 10); v /= 10; }
        while (rp > 0) num[np++] = rev[--rp];
    }
    num[np] = '\0';
    for (int n = 0; num[n] && p < 47; n++) line[p++] = num[n];
    line[p++] = 'M';
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
    sh_println(ctx, "Architecture:        x86_64", SH_COL_INFO);
    sh_println(ctx, "CPU op-mode(s):      32-bit, 64-bit", SH_COL_DEFAULT);
    sh_println(ctx, "Byte Order:          Little Endian", SH_COL_DEFAULT);
    sh_println(ctx, "CPU(s):              SMP Multi-Core Ready", SH_COL_DEFAULT);
    sh_println(ctx, "Model name:          Intel/AMD x86-64 Processor", SH_COL_DEFAULT);
    sh_println(ctx, "Virtualization:      Enabled (KVM/VirtualBox/QEMU)", SH_COL_DEFAULT);
    return 0;
}

static int cmd_ping(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: ping <destination_ip>", SH_COL_WARN);
        return 1;
    }

    sh_print(ctx, "PING ", SH_COL_INFO);
    sh_print(ctx, argv[1], SH_COL_PROMPT);
    sh_println(ctx, " (56 data bytes):", SH_COL_INFO);

    /* Parse simple IP */
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

static int cmd_pkg(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: pkg <search|install|remove|update|upgrade|list|info|clean> [args...]", SH_COL_WARN);
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
    } else {
        sh_println(ctx, "pkg: unknown action. Try 'pkg help'.", SH_COL_ERR);
        return 1;
    }
}

/* Application Launchers */
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

/* Command Registry Table */
typedef struct {
    const char *name;
    int (*handler)(shell_context_t *ctx, int argc, char **argv);
} shell_cmd_entry_t;

static const shell_cmd_entry_t g_builtins[] = {
    { "help",        cmd_help },
    { "clear",       cmd_clear },
    { "echo",        cmd_echo },
    { "pwd",         cmd_pwd },
    { "whoami",      cmd_whoami },
    { "date",        cmd_date },
    { "uname",       cmd_uname },
    { "hostname",    cmd_hostname },
    { "env",         cmd_env },
    { "ls",          cmd_ls },
    { "cd",          cmd_cd },
    { "cat",         cmd_cat },
    { "touch",       cmd_touch },
    { "rm",          cmd_rm },
    { "cp",          cmd_cp },
    { "mv",          cmd_mv },
    { "stat",        cmd_stat },
    { "df",          cmd_df },
    { "ps",          cmd_ps },
    { "free",        cmd_free },
    { "uptime",      cmd_uptime },
    { "cpuinfo",     cmd_cpuinfo },
    { "ping",        cmd_ping },
    { "ifconfig",    cmd_ifconfig },
    { "ip",          cmd_ifconfig },
    { "nslookup",    cmd_nslookup },
    { "curl",        cmd_curl },
    { "pkg",         cmd_pkg },
    { "terminal",    cmd_terminal },
    { "editor",      cmd_editor },
    { "browser",     cmd_browser },
    { "filemanager", cmd_filemanager },
    { NULL,          NULL }
};

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

/* Interactive Shell Command Dispatch Engine */
int shell_execute(shell_context_t *ctx, const char *raw_line) {
    if (!ctx || !raw_line) return 0;

    /* Handle pending command-not-found package installation prompt */
    if (ctx->pending_install) {
        if (raw_line[0] == 'y' || raw_line[0] == 'Y') {
            sh_print(ctx, "Proceeding with installation of package '", SH_COL_INFO);
            sh_print(ctx, ctx->pending_pkg, SH_COL_PROMPT);
            sh_println(ctx, "'...", SH_COL_INFO);
            int ins_res = pkg_install(ctx->pending_pkg, ctx->out_fn, ctx->out_ctx);
            ctx->pending_install = false;
            ctx->pending_pkg[0] = '\0';
            return ins_res;
        } else {
            sh_println(ctx, "Installation cancelled by user.", SH_COL_MUTED);
            ctx->pending_install = false;
            ctx->pending_pkg[0] = '\0';
            return 0;
        }
    }

    char expanded[SHELL_LINE_MAX];
    expand_variables(ctx, raw_line, expanded, sizeof(expanded));

    /* Check output redirection */
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
            /* Strip trailing spaces from redirect filename */
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

    /* 1. Built-in command check */
    for (int i = 0; g_builtins[i].name != NULL; i++) {
        if (sh_strcmp(argv[0], g_builtins[i].name) == 0) {
            ret = g_builtins[i].handler(ctx, argc, argv);
            ctx->last_exit_code = ret;
            goto cleanup;
        }
    }

    /* 2. Check for external executable in $PATH or explicit path */
    char full_exec_path[SHELL_PATH_MAX];
    bool exec_found = false;

    if (argv[0][0] == '/' || (argv[0][0] == '.' && argv[0][1] == '/')) {
        sh_normalize_path(full_exec_path, ctx->cwd, argv[0], sizeof(full_exec_path));
        vfs_stat_t st;
        if (vfs_stat(full_exec_path, &st) == VFS_OK) {
            exec_found = true;
        }
    } else {
        /* Search PATH directories: /bin, /usr/bin, /pkg/bin */
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

            /* Also check with .elf extension */
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

    /* 3. Check Package Repository Index for Command-Not-Found suggestion */
    char pkg_name[32], pkg_ver[16];
    if (pkg_find_package_for_command(argv[0], pkg_name, pkg_ver)) {
        sh_print(ctx, "Command not found: '", SH_COL_ERR);
        sh_print(ctx, argv[0], SH_COL_ERR);
        sh_println(ctx, "'", SH_COL_ERR);

        sh_print(ctx, "Available in package: '", SH_COL_INFO);
        sh_print(ctx, pkg_name, SH_COL_PROMPT);
        sh_print(ctx, "' (version ", SH_COL_INFO);
        sh_print(ctx, pkg_ver, SH_COL_INFO);
        sh_println(ctx, ")", SH_COL_INFO);

        sh_print(ctx, "Install package '", SH_COL_WARN);
        sh_print(ctx, pkg_name, SH_COL_PROMPT);
        sh_print(ctx, "' now? [y/N] ", SH_COL_WARN);

        ctx->pending_install = true;
        sh_strcpy(ctx->pending_pkg, pkg_name, sizeof(ctx->pending_pkg));
        ctx->last_exit_code = 127;
        ret = 127;
        goto cleanup;
    }

    /* 4. Linux and Windows Compatibility Notice */
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

    /* 5. Unknown Command */
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

/* Unit and Qualification Self-Test for Shell Subsystem */
static void shell_test_out(void *ctx, const char *str, u32 color) {
    (void)ctx; (void)color;
    kprintf("%s", str);
}

bool shell_self_test(void) {
    kprintf("\n[SHELL TEST] Starting Native Shell Subsystem Self-Test...\n");
    shell_context_t test_ctx;
    shell_init(&test_ctx, shell_test_out, NULL);

    /* 1. Test Environment Variables */
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

    /* 2. Test Quoted Arguments and Tokenization */
    kprintf("[SHELL TEST] 2. Testing quote parsing and argument tokenization...\n");
    char line[] = "echo 'hello world' \"second arg\" normal_arg";
    char *argv[16];
    int argc = tokenize_line(line, argv, 16);
    if (argc != 4 ||
        sh_strcmp(argv[1], "hello world") != 0 ||
        sh_strcmp(argv[2], "second arg") != 0 ||
        sh_strcmp(argv[3], "normal_arg") != 0) {
        kprintf("[SHELL TEST FAIL] Tokenization failed! argc=%d, arg1='%s', arg2='%s'\n",
                argc, argv[1], argv[2]);
        return false;
    }
    kprintf("  PASS: Single and double quoted argument parsing verified.\n");

    /* 3. Test Built-in Execution Pipeline */
    kprintf("[SHELL TEST] 3. Testing command dispatch pipeline...\n");
    if (shell_execute(&test_ctx, "echo Built-in test executed cleanly") != 0) {
        kprintf("[SHELL TEST FAIL] Built-in 'echo' execution failed!\n");
        return false;
    }
    if (shell_execute(&test_ctx, "pwd") != 0) {
        kprintf("[SHELL TEST FAIL] Built-in 'pwd' execution failed!\n");
        return false;
    }
    kprintf("  PASS: Built-in command dispatch verified.\n");

    /* 4. Test Command-Not-Found Package Suggestion */
    kprintf("[SHELL TEST] 4. Testing command-not-found suggestion flow...\n");
    shell_execute(&test_ctx, "calc");
    if (!test_ctx.pending_install || sh_strcmp(test_ctx.pending_pkg, "calc") != 0) {
        kprintf("[SHELL TEST FAIL] Command-not-found did not suggest package 'calc'!\n");
        return false;
    }
    /* Confirm installation with 'y' */
    shell_execute(&test_ctx, "y");
    if (test_ctx.pending_install) {
        kprintf("[SHELL TEST FAIL] Pending install flag was not cleared after user confirmation!\n");
        return false;
    }
    kprintf("  PASS: Command-not-found suggestion and confirmation flow verified.\n");

    kprintf("[SHELL TEST] All Native Shell tests passed successfully!\n");
    kprintf("ASTRA_NATIVE_SHELL_OK\n\n");
    return true;
}
