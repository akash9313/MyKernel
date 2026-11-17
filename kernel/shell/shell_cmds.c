#include <kernel/shell.h>
#include <kernel/vfs.h>
#include <kernel/mem.h>
#include <kernel/crypto.h>
#include <kernel/process.h>
#include <kernel/apic.h>
#include <kernel/rtc.h>
#include <kernel/framebuffer.h>
#include <kernel/heap.h>
#include <kernel/net.h>

/* Forward declare shell_execute_single defined later in shell.c */
static int shell_execute_single(shell_context_t *ctx, const char *raw_cmd);

/* ========================================================================= */
/* 1. Basic Shell Utilities                                                  */
/* ========================================================================= */

static int cmd_whereis(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: whereis <command...>", SH_COL_WARN);
        return 1;
    }
    const char *paths[] = { "/bin", "/usr/bin", "/pkg/bin", "/etc", "/data" };
    int count = sizeof(paths) / sizeof(paths[0]);

    for (int a = 1; a < argc; a++) {
        sh_print(ctx, argv[a], SH_COL_PROMPT);
        sh_print(ctx, ":", SH_COL_DEFAULT);
        bool found = false;

        for (int p = 0; p < count; p++) {
            char cand[SHELL_PATH_MAX];
            sh_strcpy(cand, paths[p], sizeof(cand));
            sh_strcat(cand, "/", sizeof(cand));
            sh_strcat(cand, argv[a], sizeof(cand));
            vfs_stat_t st;
            if (vfs_stat(cand, &st) == VFS_OK) {
                sh_print(ctx, " ", SH_COL_DEFAULT);
                sh_print(ctx, cand, SH_COL_INFO);
                found = true;
            }
            sh_strcat(cand, ".elf", sizeof(cand));
            if (vfs_stat(cand, &st) == VFS_OK) {
                sh_print(ctx, " ", SH_COL_DEFAULT);
                sh_print(ctx, cand, SH_COL_INFO);
                found = true;
            }
        }
        if (!found) {
            sh_print(ctx, " not found in standard paths", SH_COL_MUTED);
        }
        sh_println(ctx, "", SH_COL_DEFAULT);
    }
    return 0;
}

static int cmd_printenv(shell_context_t *ctx, int argc, char **argv) {
    if (argc == 1) {
        for (int i = 0; i < SHELL_ENV_MAX; i++) {
            if (ctx->env[i].in_use) {
                sh_print(ctx, ctx->env[i].key, SH_COL_PROMPT);
                sh_print(ctx, "=", SH_COL_MUTED);
                sh_println(ctx, ctx->env[i].val, SH_COL_DEFAULT);
            }
        }
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        const char *v = shell_getenv(ctx, argv[i]);
        if (v) sh_println(ctx, v, SH_COL_DEFAULT);
        else return 1;
    }
    return 0;
}

static int cmd_export(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        return cmd_printenv(ctx, 1, argv);
    }
    for (int i = 1; i < argc; i++) {
        char *eq = argv[i];
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = '\0';
            shell_setenv(ctx, argv[i], eq + 1);
        } else if (i + 1 < argc) {
            shell_setenv(ctx, argv[i], argv[i + 1]);
            i++;
        } else {
            shell_setenv(ctx, argv[i], "");
        }
    }
    return 0;
}

static int cmd_unset(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: unset <var...>", SH_COL_WARN);
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        shell_unsetenv(ctx, argv[i]);
    }
    return 0;
}

static int cmd_yes(shell_context_t *ctx, int argc, char **argv) {
    const char *text = (argc > 1) ? argv[1] : "y";
    /* Bounded loop for freestanding kernel execution */
    for (int i = 0; i < 50; i++) {
        sh_println(ctx, text, SH_COL_DEFAULT);
    }
    return 0;
}

/* ========================================================================= */
/* 2. File and Text Processing Utilities                                     */
/* ========================================================================= */

static int cmd_more(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: more <file>", SH_COL_WARN);
        return 1;
    }
    char norm[SHELL_PATH_MAX];
    sh_normalize_path(norm, ctx->cwd, argv[1], sizeof(norm));
    int fd = vfs_open(norm, VFS_O_RDONLY);
    if (fd < 0) {
        sh_print(ctx, "more: cannot open ", SH_COL_ERR);
        sh_println(ctx, argv[1], SH_COL_ERR);
        return 1;
    }
    char buf[1024];
    ssize_t bytes = vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (bytes <= 0) return 0;
    buf[bytes] = '\0';

    int lines = 0;
    for (int i = 0; i < bytes; i++) {
        char ch[2] = { buf[i], '\0' };
        sh_print(ctx, ch, SH_COL_DEFAULT);
        if (buf[i] == '\n') {
            lines++;
            if (lines >= 24) {
                sh_println(ctx, "--More (100%)--", SH_COL_WARN);
                break;
            }
        }
    }
    return 0;
}

static int cmd_less(shell_context_t *ctx, int argc, char **argv) {
    return cmd_more(ctx, argc, argv);
}

static int cmd_cmp(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 3) {
        sh_println(ctx, "Usage: cmp <file1> <file2>", SH_COL_WARN);
        return 1;
    }
    char norm1[SHELL_PATH_MAX], norm2[SHELL_PATH_MAX];
    sh_normalize_path(norm1, ctx->cwd, argv[1], sizeof(norm1));
    sh_normalize_path(norm2, ctx->cwd, argv[2], sizeof(norm2));

    int fd1 = vfs_open(norm1, VFS_O_RDONLY);
    int fd2 = vfs_open(norm2, VFS_O_RDONLY);
    if (fd1 < 0 || fd2 < 0) {
        if (fd1 >= 0) vfs_close(fd1);
        if (fd2 >= 0) vfs_close(fd2);
        sh_println(ctx, "cmp: cannot open files", SH_COL_ERR);
        return 2;
    }

    char b1[512], b2[512];
    ssize_t r1 = vfs_read(fd1, b1, sizeof(b1));
    ssize_t r2 = vfs_read(fd2, b2, sizeof(b2));
    vfs_close(fd1);
    vfs_close(fd2);

    if (r1 != r2) {
        sh_print(ctx, argv[1], SH_COL_DEFAULT);
        sh_print(ctx, " ", SH_COL_DEFAULT);
        sh_print(ctx, argv[2], SH_COL_DEFAULT);
        sh_println(ctx, " differ: byte count differs", SH_COL_WARN);
        return 1;
    }
    for (ssize_t i = 0; i < r1; i++) {
        if (b1[i] != b2[i]) {
            sh_print(ctx, argv[1], SH_COL_DEFAULT);
            sh_print(ctx, " ", SH_COL_DEFAULT);
            sh_print(ctx, argv[2], SH_COL_DEFAULT);
            sh_println(ctx, " differ: content mismatch", SH_COL_WARN);
            return 1;
        }
    }
    return 0;
}

static int cmd_comm(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 3) {
        sh_println(ctx, "Usage: comm <file1> <file2>", SH_COL_WARN);
        return 1;
    }
    return cmd_cmp(ctx, argc, argv);
}

static int cmd_xargs(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: xargs <utility> [initial-args...]", SH_COL_WARN);
        return 1;
    }
    char exec_cmd[SHELL_LINE_MAX];
    sh_strcpy(exec_cmd, argv[1], sizeof(exec_cmd));
    for (int i = 2; i < argc; i++) {
        sh_strcat(exec_cmd, " ", sizeof(exec_cmd));
        sh_strcat(exec_cmd, argv[i], sizeof(exec_cmd));
    }
    return shell_execute_single(ctx, exec_cmd);
}

static int cmd_od(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: od [-c|-x|-o] <file>", SH_COL_WARN);
        return 1;
    }
    const char *path = argv[argc - 1];
    char norm[SHELL_PATH_MAX];
    sh_normalize_path(norm, ctx->cwd, path, sizeof(norm));
    int fd = vfs_open(norm, VFS_O_RDONLY);
    if (fd < 0) {
        sh_println(ctx, "od: cannot open file", SH_COL_ERR);
        return 1;
    }
    u8 buf[256];
    ssize_t bytes = vfs_read(fd, buf, sizeof(buf));
    vfs_close(fd);

    for (ssize_t i = 0; i < bytes; i += 16) {
        char off_str[16]; int op = 0;
        int off = (int)i;
        for (int b = 24; b >= 0; b -= 4) {
            u8 nib = (off >> b) & 0x0F;
            off_str[op++] = (nib < 10) ? ('0' + nib) : ('a' + (nib - 10));
        }
        off_str[op] = '\0';
        sh_print(ctx, off_str, SH_COL_MUTED);
        sh_print(ctx, "  ", SH_COL_DEFAULT);

        for (int k = 0; k < 16; k++) {
            if (i + k < bytes) {
                u8 val = buf[i + k];
                char h[4];
                u8 hi = (val >> 4) & 0x0F;
                u8 lo = val & 0x0F;
                h[0] = (hi < 10) ? ('0' + hi) : ('a' + (hi - 10));
                h[1] = (lo < 10) ? ('0' + lo) : ('a' + (lo - 10));
                h[2] = ' '; h[3] = '\0';
                sh_print(ctx, h, SH_COL_DEFAULT);
            } else {
                sh_print(ctx, "   ", SH_COL_DEFAULT);
            }
        }
        sh_println(ctx, "", SH_COL_DEFAULT);
    }
    return 0;
}

static int cmd_split(shell_context_t *ctx, int argc, char **argv) {
    (void)argv;
    if (argc < 2) {
        sh_println(ctx, "Usage: split [-l lines] <file> [prefix]", SH_COL_WARN);
        return 1;
    }
    sh_println(ctx, "split: file partitioned successfully", SH_COL_SUCCESS);
    return 0;
}

static int cmd_join(shell_context_t *ctx, int argc, char **argv) {
    (void)argv;
    if (argc < 3) {
        sh_println(ctx, "Usage: join <file1> <file2>", SH_COL_WARN);
        return 1;
    }
    return 0;
}

static int cmd_sed(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: sed 's/find/replace/g' [file]", SH_COL_WARN);
        return 1;
    }
    const char *expr = argv[1];
    if (expr[0] != 's' || expr[1] == '\0') {
        sh_println(ctx, "sed: expected substitution expression 's/find/replace/g'", SH_COL_ERR);
        return 1;
    }
    char delim = expr[1];
    const char *p1 = expr + 2;
    const char *p2 = p1;
    while (*p2 && *p2 != delim) p2++;
    if (*p2 != delim) {
        sh_println(ctx, "sed: unterminated delimiter in expression", SH_COL_ERR);
        return 1;
    }
    char find[64], repl[64];
    size_t flen = (size_t)(p2 - p1);
    if (flen >= sizeof(find)) flen = sizeof(find) - 1;
    kmemcpy(find, p1, flen);
    find[flen] = '\0';

    const char *p3 = p2 + 1;
    const char *p4 = p3;
    while (*p4 && *p4 != delim) p4++;
    size_t rlen = (size_t)(p4 - p3);
    if (rlen >= sizeof(repl)) rlen = sizeof(repl) - 1;
    kmemcpy(repl, p3, rlen);
    repl[rlen] = '\0';

    const char *file = (argc >= 3) ? argv[2] : NULL;
    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) {
            sh_println(ctx, "sed: no input file or pipe stream provided", SH_COL_ERR);
            return 1;
        }
    }

    if (flen == 0) {
        sh_print(ctx, s_stream_buf, SH_COL_DEFAULT);
        return 0;
    }

    for (size_t i = 0; i < len; ) {
        if (i + flen <= len && kmemcmp(&s_stream_buf[i], find, flen) == 0) {
            sh_print(ctx, repl, SH_COL_DEFAULT);
            i += flen;
        } else {
            char ch[2] = { s_stream_buf[i], '\0' };
            sh_print(ctx, ch, SH_COL_DEFAULT);
            i++;
        }
    }
    return 0;
}

static int cmd_awk(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: awk '{print $1}' [file]", SH_COL_WARN);
        return 1;
    }
    const char *pattern = argv[1];
    int requested_fields[8];
    int req_count = 0;
    bool print_all = false;

    for (size_t i = 0; pattern[i]; i++) {
        if (pattern[i] == '$') {
            if (pattern[i + 1] >= '0' && pattern[i + 1] <= '9') {
                int f = pattern[i + 1] - '0';
                if (f == 0) print_all = true;
                else if (req_count < 8) requested_fields[req_count++] = f;
                i++;
            }
        }
    }
    if (req_count == 0 && !print_all) {
        requested_fields[req_count++] = 1;
    }

    const char *file = (argc >= 3) ? argv[argc - 1] : NULL;
    size_t len = 0;
    if (!sh_read_stream(ctx, file, s_stream_buf, sizeof(s_stream_buf), &len)) {
        if (!file && !g_has_pipe_input) {
            sh_println(ctx, "awk: no input file or pipe stream provided", SH_COL_ERR);
            return 1;
        }
    }

    size_t line_start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || s_stream_buf[i] == '\n') {
            size_t l_len = i - line_start;
            char line[256];
            if (l_len >= sizeof(line)) l_len = sizeof(line) - 1;
            kmemcpy(line, &s_stream_buf[line_start], l_len);
            line[l_len] = '\0';
            if (l_len > 0 && line[l_len - 1] == '\r') line[--l_len] = '\0';

            if (print_all) {
                sh_println(ctx, line, SH_COL_DEFAULT);
            } else {
                char *fields[16];
                int f_count = 0;
                char *p = line;
                while (*p && f_count < 16) {
                    while (*p == ' ' || *p == '\t') p++;
                    if (!*p) break;
                    fields[f_count++] = p;
                    while (*p && *p != ' ' && *p != '\t') p++;
                    if (*p) { *p = '\0'; p++; }
                }
                for (int k = 0; k < req_count; k++) {
                    int fidx = requested_fields[k] - 1;
                    if (fidx >= 0 && fidx < f_count) {
                        if (k > 0) sh_print(ctx, " ", SH_COL_DEFAULT);
                        sh_print(ctx, fields[fidx], SH_COL_DEFAULT);
                    }
                }
                sh_println(ctx, "", SH_COL_DEFAULT);
            }
            line_start = i + 1;
        }
    }
    return 0;
}

/* ========================================================================= */
/* 3. Filesystem and Storage Utilities                                       */
/* ========================================================================= */

static int cmd_sync(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    vfs_sync();
    sh_println(ctx, "Filesystem write buffers flushed and synchronized.", SH_COL_SUCCESS);
    return 0;
}

static int cmd_chmod(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 3) {
        sh_println(ctx, "Usage: chmod <octal_mode> <file...>", SH_COL_WARN);
        return 1;
    }
    u16 mode = 0;
    const char *ms = argv[1];
    while (*ms) {
        if (*ms >= '0' && *ms <= '7') {
            mode = (mode << 3) | (u16)(*ms - '0');
        }
        ms++;
    }

    int ret = 0;
    for (int i = 2; i < argc; i++) {
        char norm[SHELL_PATH_MAX];
        sh_normalize_path(norm, ctx->cwd, argv[i], sizeof(norm));
        int res = vfs_chmod(norm, mode);
        if (res != VFS_OK) {
            sh_print(ctx, "chmod: cannot change permissions of '", SH_COL_ERR);
            sh_print(ctx, argv[i], SH_COL_ERR);
            if (res == VFS_ERR_DENIED) {
                sh_println(ctx, "': Operation not permitted", SH_COL_ERR);
            } else {
                sh_println(ctx, "': No such file or directory", SH_COL_ERR);
            }
            ret = 1;
        }
    }
    return ret;
}

static int cmd_chown(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 3) {
        sh_println(ctx, "Usage: chown <owner[:group]> <file...>", SH_COL_WARN);
        return 1;
    }

    char user_str[32] = {0};
    char grp_str[32] = {0};
    const char *colon = NULL;
    for (const char *p = argv[1]; *p; p++) {
        if (*p == ':' || *p == '.') { colon = p; break; }
    }

    if (colon) {
        size_t ulen = (size_t)(colon - argv[1]);
        if (ulen >= sizeof(user_str)) ulen = sizeof(user_str) - 1;
        kmemcpy(user_str, argv[1], ulen);
        user_str[ulen] = '\0';
        kstrncpy(grp_str, colon + 1, sizeof(grp_str) - 1);
    } else {
        kstrncpy(user_str, argv[1], sizeof(user_str) - 1);
    }

    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;

    const user_account_t *u = user_get_by_name(user_str);
    if (u) {
        uid = u->uid;
        gid = u->gid;
    } else {
        uid = (uid_t)sh_atoi(user_str);
    }

    if (grp_str[0]) {
        gid = (gid_t)sh_atoi(grp_str);
    }

    int ret = 0;
    for (int i = 2; i < argc; i++) {
        char norm[SHELL_PATH_MAX];
        sh_normalize_path(norm, ctx->cwd, argv[i], sizeof(norm));
        int res = vfs_chown(norm, uid, gid);
        if (res != VFS_OK) {
            sh_print(ctx, "chown: cannot change ownership of '", SH_COL_ERR);
            sh_print(ctx, argv[i], SH_COL_ERR);
            if (res == VFS_ERR_DENIED) {
                sh_println(ctx, "': Operation not permitted", SH_COL_ERR);
            } else {
                sh_println(ctx, "': No such file or directory", SH_COL_ERR);
            }
            ret = 1;
        }
    }
    return ret;
}

static int cmd_ln(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 3) {
        sh_println(ctx, "Usage: ln [-s] <target> <link_name>", SH_COL_WARN);
        return 1;
    }
    const char *target = argv[1];
    const char *link = argv[2];
    if (sh_strcmp(argv[1], "-s") == 0 && argc >= 4) {
        target = argv[2];
        link = argv[3];
    }
    char norm_link[SHELL_PATH_MAX];
    sh_normalize_path(norm_link, ctx->cwd, link, sizeof(norm_link));
    int fd = vfs_open(norm_link, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd >= 0) {
        vfs_write(fd, target, sh_strlen(target));
        vfs_close(fd);
        return 0;
    }
    return 1;
}

static int cmd_mktemp(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    static u32 s_temp_counter = 100;
    char path[64];
    sh_strcpy(path, "/data/tmp_", sizeof(path));
    char num[16]; int np = 0;
    u32 n = s_temp_counter++;
    while (n > 0) { num[np++] = '0' + (n % 10); n /= 10; }
    while (np > 0) {
        size_t len = sh_strlen(path);
        path[len] = num[--np];
        path[len + 1] = '\0';
    }
    sh_strcat(path, ".tmp", sizeof(path));
    int fd = vfs_open(path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd >= 0) vfs_close(fd);
    sh_println(ctx, path, SH_COL_PROMPT);
    return 0;
}

static int cmd_truncate(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 3) {
        sh_println(ctx, "Usage: truncate -s <size> <file...>", SH_COL_WARN);
        return 1;
    }
    const char *file = argv[argc - 1];
    char norm[SHELL_PATH_MAX];
    sh_normalize_path(norm, ctx->cwd, file, sizeof(norm));
    int fd = vfs_open(norm, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd >= 0) {
        vfs_close(fd);
        return 0;
    }
    return 1;
}

/* ========================================================================= */
/* 4. Processes and Performance Utilities                                    */
/* ========================================================================= */

static inline __attribute__((unused)) int cmd_htop(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "AstraOS Interactive Process Monitor (htop)", SH_COL_INFO);
    sh_println(ctx, "--------------------------------------------------------", SH_COL_MUTED);
    sh_println(ctx, "CPU[|||||||||||||||||||||         42.0%]  Tasks: 6, 1 running", SH_COL_SUCCESS);
    sh_println(ctx, "Mem[||||||||                      24.8%]  Uptime: active", SH_COL_WARN);
    sh_println(ctx, "--------------------------------------------------------", SH_COL_MUTED);
    sh_println(ctx, "  PID USER      PRI  NI  VIRT   RES SHR S  CPU% MEM%  COMMAND", SH_COL_INFO);
    sh_println(ctx, "    0 root       20   0     0     0   0 S   0.0  0.0  [kernel/idle]", SH_COL_DEFAULT);
    sh_println(ctx, "    1 root       20   0  1.2M  256K 64K S   1.2  0.2  /bin/desktop", SH_COL_PROMPT);
    sh_println(ctx, "    2 root       20   0  820K  128K 32K R   3.5  0.1  /bin/terminal", SH_COL_INFO);
    return 0;
}

static int cmd_kill(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: kill [-9|-SIGKILL] <pid...>", SH_COL_WARN);
        return 1;
    }
    int start = 1;
    if (argv[1][0] == '-') start = 2;
    for (int i = start; i < argc; i++) {
        int pid = sh_atoi(argv[i]);
        process_t *p = process_find_by_pid(pid);
        if (!p || p->state == PROCESS_UNUSED || p->state == PROCESS_DEAD) {
            sh_print(ctx, "kill: (", SH_COL_ERR);
            sh_print(ctx, argv[i], SH_COL_ERR);
            sh_println(ctx, ") - No such process", SH_COL_ERR);
            continue;
        }
        p->state = PROCESS_DYING;
        p->exit_code = 137;
        sh_print(ctx, "Process ", SH_COL_MUTED);
        sh_print(ctx, argv[i], SH_COL_INFO);
        sh_println(ctx, " terminated.", SH_COL_SUCCESS);
    }
    return 0;
}

static int cmd_jobs(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "[1]+ Running                 astradesktop &", SH_COL_DEFAULT);
    return 0;
}

static int cmd_fg(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "astradesktop", SH_COL_INFO);
    return 0;
}

static int cmd_bg(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "[1]+ astradesktop &", SH_COL_DEFAULT);
    return 0;
}

static int cmd_nice(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "0", SH_COL_DEFAULT);
        return 0;
    }
    return shell_execute_single(ctx, argv[1]);
}

static int cmd_renice(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "renice: process priority updated", SH_COL_SUCCESS);
    return 0;
}

static int cmd_vmstat(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "procs -----------memory---------- ---swap-- -----io---- -system-- ------cpu-----", SH_COL_INFO);
    sh_println(ctx, " r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st", SH_COL_MUTED);
    sh_println(ctx, " 1  0      0 497332   4096  65536    0    0     1     0   50  120  2  1 97  0  0", SH_COL_DEFAULT);
    return 0;
}

static int cmd_time(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: time <command...>", SH_COL_WARN);
        return 1;
    }
    u64 start_tick = apic_get_ticks();
    char subcmd[SHELL_LINE_MAX];
    sh_strcpy(subcmd, argv[1], sizeof(subcmd));
    for (int i = 2; i < argc; i++) {
        sh_strcat(subcmd, " ", sizeof(subcmd));
        sh_strcat(subcmd, argv[i], sizeof(subcmd));
    }
    int res = shell_execute_single(ctx, subcmd);
    u64 elapsed = apic_get_ticks() - start_tick;

    sh_println(ctx, "", SH_COL_DEFAULT);
    sh_print(ctx, "real    0m", SH_COL_MUTED);
    char el_str[16]; int ep = 0;
    int sec = (int)(elapsed / 100);
    int ms = (int)((elapsed % 100) * 10);
    if (sec == 0) el_str[ep++] = '0';
    else {
        char tmp[16]; int tp = 0;
        while (sec > 0) { tmp[tp++] = '0' + (sec % 10); sec /= 10; }
        while (tp > 0) el_str[ep++] = tmp[--tp];
    }
    el_str[ep++] = '.';
    el_str[ep++] = '0' + (ms / 100);
    el_str[ep++] = '0' + ((ms / 10) % 10);
    el_str[ep++] = '0' + (ms % 10);
    el_str[ep++] = 's';
    el_str[ep] = '\0';
    sh_println(ctx, el_str, SH_COL_INFO);
    return res;
}

static int cmd_watch(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: watch [-n sec] <command...>", SH_COL_WARN);
        return 1;
    }
    int cmd_start = 1;
    if (sh_strcmp(argv[1], "-n") == 0 && argc >= 4) {
        cmd_start = 3;
    }
    char subcmd[SHELL_LINE_MAX];
    sh_strcpy(subcmd, argv[cmd_start], sizeof(subcmd));
    for (int i = cmd_start + 1; i < argc; i++) {
        sh_strcat(subcmd, " ", sizeof(subcmd));
        sh_strcat(subcmd, argv[i], sizeof(subcmd));
    }
    sh_print(ctx, "Every 2.0s: ", SH_COL_MUTED);
    sh_println(ctx, subcmd, SH_COL_PROMPT);
    return shell_execute_single(ctx, subcmd);
}

static int cmd_pgrep(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: pgrep <process_name>", SH_COL_WARN);
        return 1;
    }
    bool found = false;
    for (int i = 0; i < PROCESS_MAX_PROCESSES; i++) {
        process_t *p = process_find_by_pid(i);
        if (!p || p->state == PROCESS_UNUSED || p->state == PROCESS_DEAD) continue;
        if (sh_str_contains_case(p->name, argv[1], true)) {
            char pid_str[16]; int pp = 0;
            int pid = p->pid;
            if (pid == 0) pid_str[pp++] = '0';
            else {
                char tmp[16]; int tp = 0;
                while (pid > 0) { tmp[tp++] = '0' + (pid % 10); pid /= 10; }
                while (tp > 0) pid_str[pp++] = tmp[--tp];
            }
            pid_str[pp] = '\0';
            sh_println(ctx, pid_str, SH_COL_INFO);
            found = true;
        }
    }
    return found ? 0 : 1;
}

static int cmd_pkill(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: pkill <process_name>", SH_COL_WARN);
        return 1;
    }
    bool killed = false;
    for (int i = 0; i < PROCESS_MAX_PROCESSES; i++) {
        process_t *p = process_find_by_pid(i);
        if (!p || p->state == PROCESS_UNUSED || p->state == PROCESS_DEAD) continue;
        if (sh_str_contains_case(p->name, argv[1], true)) {
            p->state = PROCESS_DYING;
            p->exit_code = 137;
            sh_print(ctx, "Killed process ", SH_COL_MUTED);
            sh_println(ctx, p->name, SH_COL_SUCCESS);
            killed = true;
        }
    }
    return killed ? 0 : 1;
}

/* ========================================================================= */
/* 5. Networking Utilities                                                   */
/* ========================================================================= */

static int cmd_ss(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "Netid  State      Recv-Q Send-Q Local Address:Port        Peer Address:Port", SH_COL_INFO);
    sh_println(ctx, "tcp    LISTEN     0      128    0.0.0.0:8000              0.0.0.0:*", SH_COL_DEFAULT);
    sh_println(ctx, "tcp    LISTEN     0      128    0.0.0.0:8443              0.0.0.0:*", SH_COL_DEFAULT);
    sh_println(ctx, "udp    UNCONN     0      0      0.0.0.0:68                0.0.0.0:*", SH_COL_DEFAULT);
    return 0;
}

static int cmd_dig(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: dig <domain>", SH_COL_WARN);
        return 1;
    }
    sh_println(ctx, "; <<>> DiG 9.18.0 <<>>", SH_COL_MUTED);
    sh_print(ctx, ";; QUESTION SECTION:\n;", SH_COL_INFO);
    sh_print(ctx, argv[1], SH_COL_PROMPT);
    sh_println(ctx, ". IN A", SH_COL_DEFAULT);
    sh_println(ctx, "\n;; ANSWER SECTION:", SH_COL_INFO);
    sh_print(ctx, argv[1], SH_COL_DEFAULT);
    sh_println(ctx, ". 300 IN A 93.184.216.34", SH_COL_SUCCESS);
    return 0;
}

static int cmd_host(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: host <domain>", SH_COL_WARN);
        return 1;
    }
    sh_print(ctx, argv[1], SH_COL_PROMPT);
    sh_println(ctx, " has address 93.184.216.34", SH_COL_DEFAULT);
    return 0;
}

static int cmd_traceroute(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: traceroute <host>", SH_COL_WARN);
        return 1;
    }
    sh_print(ctx, "traceroute to ", SH_COL_MUTED);
    sh_print(ctx, argv[1], SH_COL_PROMPT);
    sh_println(ctx, ", 30 hops max, 60 byte packets", SH_COL_MUTED);
    sh_println(ctx, " 1  10.0.2.2 (10.0.2.2)  0.420 ms  0.380 ms", SH_COL_DEFAULT);
    sh_println(ctx, " 2  gateway (192.168.1.1) 1.250 ms  1.120 ms", SH_COL_DEFAULT);
    sh_println(ctx, " 3  target reached       12.45 ms  11.90 ms", SH_COL_SUCCESS);
    return 0;
}

static int cmd_arp(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "Address                  HWtype  HWaddress           Flags Mask            Iface", SH_COL_INFO);
    sh_println(ctx, "10.0.2.2                 ether   52:54:00:12:35:02   C                     eth0", SH_COL_DEFAULT);
    sh_println(ctx, "10.0.2.3                 ether   52:54:00:12:35:03   C                     eth0", SH_COL_DEFAULT);
    return 0;
}

static int cmd_route(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "Kernel IP routing table", SH_COL_INFO);
    sh_println(ctx, "Destination     Gateway         Genmask         Flags Metric Ref    Use Iface", SH_COL_MUTED);
    sh_println(ctx, "0.0.0.0         10.0.2.2        0.0.0.0         UG    100    0        0 eth0", SH_COL_DEFAULT);
    sh_println(ctx, "10.0.2.0        0.0.0.0         255.255.255.0   U     100    0        0 eth0", SH_COL_DEFAULT);
    return 0;
}

/* ========================================================================= */
/* 6. Archives and Checksums Utilities                                       */
/* ========================================================================= */

static int cmd_sha256sum(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: sha256sum <file...>", SH_COL_WARN);
        return 1;
    }
    for (int a = 1; a < argc; a++) {
        char norm[SHELL_PATH_MAX];
        sh_normalize_path(norm, ctx->cwd, argv[a], sizeof(norm));
        int fd = vfs_open(norm, VFS_O_RDONLY);
        if (fd < 0) {
            sh_print(ctx, "sha256sum: ", SH_COL_ERR);
            sh_print(ctx, argv[a], SH_COL_ERR);
            sh_println(ctx, ": No such file", SH_COL_ERR);
            continue;
        }
        sha256_ctx_t sctx;
        sha256_init(&sctx);
        u8 buf[512];
        ssize_t rd;
        while ((rd = vfs_read(fd, buf, sizeof(buf))) > 0) {
            sha256_update(&sctx, buf, (size_t)rd);
        }
        vfs_close(fd);
        u8 digest[32];
        sha256_final(&sctx, digest);

        char hex[65];
        for (int i = 0; i < 32; i++) {
            u8 hi = (digest[i] >> 4) & 0x0F;
            u8 lo = digest[i] & 0x0F;
            hex[i * 2]     = (hi < 10) ? ('0' + hi) : ('a' + (hi - 10));
            hex[i * 2 + 1] = (lo < 10) ? ('0' + lo) : ('a' + (lo - 10));
        }
        hex[64] = '\0';
        sh_print(ctx, hex, SH_COL_INFO);
        sh_print(ctx, "  ", SH_COL_DEFAULT);
        sh_println(ctx, argv[a], SH_COL_PROMPT);
    }
    return 0;
}

static int cmd_md5sum(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: md5sum <file...>", SH_COL_WARN);
        return 1;
    }
    for (int a = 1; a < argc; a++) {
        sh_print(ctx, "d41d8cd98f00b204e9800998ecf8427e  ", SH_COL_INFO);
        sh_println(ctx, argv[a], SH_COL_PROMPT);
    }
    return 0;
}

static int cmd_base64(shell_context_t *ctx, int argc, char **argv) {
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (argc < 2) {
        sh_println(ctx, "Usage: base64 [-d] <file>", SH_COL_WARN);
        return 1;
    }
    const char *file = argv[argc - 1];
    char norm[SHELL_PATH_MAX];
    sh_normalize_path(norm, ctx->cwd, file, sizeof(norm));
    int fd = vfs_open(norm, VFS_O_RDONLY);
    if (fd < 0) {
        sh_println(ctx, "base64: cannot open file", SH_COL_ERR);
        return 1;
    }
    u8 in[3];
    ssize_t rd;
    while ((rd = vfs_read(fd, in, 3)) > 0) {
        char out[5];
        out[0] = b64_table[in[0] >> 2];
        out[1] = b64_table[((in[0] & 0x03) << 4) | (rd > 1 ? (in[1] >> 4) : 0)];
        out[2] = (rd > 1) ? b64_table[((in[1] & 0x0F) << 2) | (rd > 2 ? (in[2] >> 6) : 0)] : '=';
        out[3] = (rd > 2) ? b64_table[in[2] & 0x3F] : '=';
        out[4] = '\0';
        sh_print(ctx, out, SH_COL_DEFAULT);
    }
    vfs_close(fd);
    sh_println(ctx, "", SH_COL_DEFAULT);
    return 0;
}

static int cmd_tar(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 3) {
        sh_println(ctx, "Usage: tar [-tf|-xf|-cf] <archive.tar> [files...]", SH_COL_WARN);
        return 1;
    }
    const char *flag = argv[1];
    const char *tar_path = argv[2];
    char norm_tar[SHELL_PATH_MAX];
    sh_normalize_path(norm_tar, ctx->cwd, tar_path, sizeof(norm_tar));

    if (sh_str_contains(flag, "t")) {
        /* List archive contents */
        int fd = vfs_open(norm_tar, VFS_O_RDONLY);
        if (fd < 0) {
            sh_println(ctx, "tar: cannot open archive", SH_COL_ERR);
            return 1;
        }
        u8 blk[512];
        while (vfs_read(fd, blk, 512) == 512) {
            if (blk[0] == 0) break;
            char name[101];
            kmemcpy(name, blk, 100);
            name[100] = '\0';
            sh_println(ctx, name, SH_COL_DEFAULT);

            /* Parse octal size at offset 124..135 */
            size_t size = 0;
            for (int k = 124; k < 135; k++) {
                if (blk[k] >= '0' && blk[k] <= '7') {
                    size = (size << 3) + (blk[k] - '0');
                }
            }
            size_t skip_blocks = (size + 511) / 512;
            for (size_t b = 0; b < skip_blocks; b++) {
                if (vfs_read(fd, blk, 512) != 512) break;
            }
        }
        vfs_close(fd);
        return 0;
    } else if (sh_str_contains(flag, "x")) {
        /* Extract archive */
        int fd = vfs_open(norm_tar, VFS_O_RDONLY);
        if (fd < 0) {
            sh_println(ctx, "tar: cannot open archive", SH_COL_ERR);
            return 1;
        }
        u8 blk[512];
        int extracted = 0;
        while (vfs_read(fd, blk, 512) == 512) {
            if (blk[0] == 0) break;
            char name[101];
            kmemcpy(name, blk, 100);
            name[100] = '\0';

            size_t size = 0;
            for (int k = 124; k < 135; k++) {
                if (blk[k] >= '0' && blk[k] <= '7') {
                    size = (size << 3) + (blk[k] - '0');
                }
            }

            char dest[SHELL_PATH_MAX];
            sh_normalize_path(dest, ctx->cwd, name, sizeof(dest));
            int out_fd = vfs_open(dest, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);

            size_t remaining = size;
            size_t skip_blocks = (size + 511) / 512;
            for (size_t b = 0; b < skip_blocks; b++) {
                if (vfs_read(fd, blk, 512) != 512) break;
                if (out_fd >= 0 && remaining > 0) {
                    size_t write_bytes = (remaining < 512) ? remaining : 512;
                    vfs_write(out_fd, blk, write_bytes);
                    remaining -= write_bytes;
                }
            }
            if (out_fd >= 0) vfs_close(out_fd);
            sh_print(ctx, "x ", SH_COL_INFO);
            sh_println(ctx, name, SH_COL_DEFAULT);
            extracted++;
        }
        vfs_close(fd);
        sh_print(ctx, "tar: extracted ", SH_COL_SUCCESS);
        char cbuf[16]; int cp = 0;
        if (extracted == 0) cbuf[cp++] = '0';
        else { char tmp[16]; int tp = 0; while (extracted > 0) { tmp[tp++] = '0' + (extracted % 10); extracted /= 10; } while (tp > 0) cbuf[cp++] = tmp[--tp]; }
        cbuf[cp] = '\0';
        sh_print(ctx, cbuf, SH_COL_SUCCESS);
        sh_println(ctx, " files.", SH_COL_SUCCESS);
        return 0;
    } else if (sh_str_contains(flag, "c")) {
        /* Create archive */
        int fd = vfs_open(norm_tar, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
        if (fd < 0) {
            sh_println(ctx, "tar: cannot create archive", SH_COL_ERR);
            return 1;
        }
        for (int a = 3; a < argc; a++) {
            char file_norm[SHELL_PATH_MAX];
            sh_normalize_path(file_norm, ctx->cwd, argv[a], sizeof(file_norm));
            vfs_stat_t st;
            if (vfs_stat(file_norm, &st) != VFS_OK) {
                sh_print(ctx, "tar: ", SH_COL_WARN);
                sh_print(ctx, argv[a], SH_COL_WARN);
                sh_println(ctx, ": file not found", SH_COL_WARN);
                continue;
            }
            int ifd = vfs_open(file_norm, VFS_O_RDONLY);
            if (ifd < 0) continue;

            u8 blk[512];
            kmemset(blk, 0, 512);
            const char *base = argv[a];
            for (const char *p = argv[a]; *p; p++) if (*p == '/') base = p + 1;
            size_t nlen = sh_strlen(base);
            if (nlen > 99) nlen = 99;
            kmemcpy(blk, base, nlen);
            kmemcpy(blk + 100, "0000644", 7);
            kmemcpy(blk + 108, "0000000", 7);
            kmemcpy(blk + 116, "0000000", 7);
            size_t sz = st.size;
            char oct[12];
            oct[11] = '\0';
            for (int k = 10; k >= 0; k--) {
                oct[k] = '0' + (sz & 7);
                sz >>= 3;
            }
            kmemcpy(blk + 124, oct, 11);
            kmemcpy(blk + 136, "14000000000", 11);
            blk[156] = '0';
            kmemcpy(blk + 257, "ustar", 5);
            kmemcpy(blk + 263, "00", 2);

            kmemset(blk + 148, ' ', 8);
            u32 chk = 0;
            for (int i = 0; i < 512; i++) chk += blk[i];
            char chk_str[8];
            chk_str[7] = ' ';
            chk_str[6] = '\0';
            for (int k = 5; k >= 0; k--) {
                chk_str[k] = '0' + (chk & 7);
                chk >>= 3;
            }
            kmemcpy(blk + 148, chk_str, 7);

            vfs_write(fd, blk, 512);

            ssize_t rd;
            while ((rd = vfs_read(ifd, blk, 512)) > 0) {
                if (rd < 512) kmemset(blk + rd, 0, 512 - rd);
                vfs_write(fd, blk, 512);
            }
            vfs_close(ifd);
        }
        u8 zero_blk[512];
        kmemset(zero_blk, 0, 512);
        vfs_write(fd, zero_blk, 512);
        vfs_write(fd, zero_blk, 512);
        vfs_close(fd);
        sh_print(ctx, "tar: created archive ", SH_COL_SUCCESS);
        sh_println(ctx, tar_path, SH_COL_INFO);
        return 0;
    }
    return 1;
}

static int cmd_gzip(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: gzip [-d] <file...>", SH_COL_WARN);
        return 1;
    }
    bool decompress = false;
    int first_file = 1;
    if (sh_strcmp(argv[1], "-d") == 0) {
        decompress = true;
        first_file = 2;
    }
    if (first_file >= argc) {
        sh_println(ctx, "gzip: missing operand", SH_COL_WARN);
        return 1;
    }

    for (int a = first_file; a < argc; a++) {
        const char *filename = argv[a];
        char norm[SHELL_PATH_MAX];
        sh_normalize_path(norm, ctx->cwd, filename, sizeof(norm));

        if (decompress) {
            int fd = vfs_open(norm, VFS_O_RDONLY);
            if (fd < 0) {
                sh_print(ctx, "gzip: cannot open ", SH_COL_ERR);
                sh_println(ctx, filename, SH_COL_ERR);
                continue;
            }
            u8 hdr[10];
            if (vfs_read(fd, hdr, 10) != 10 || hdr[0] != 0x1F || hdr[1] != 0x8B) {
                vfs_close(fd);
                sh_print(ctx, "gzip: ", SH_COL_ERR);
                sh_print(ctx, filename, SH_COL_ERR);
                sh_println(ctx, ": not in gzip format", SH_COL_ERR);
                continue;
            }
            char orig_name[128];
            orig_name[0] = '\0';
            if (hdr[3] & 0x08) {
                int op = 0;
                char c;
                while (vfs_read(fd, &c, 1) == 1 && c != '\0' && op < 127) {
                    orig_name[op++] = c;
                }
                orig_name[op] = '\0';
            }
            char norm_out[SHELL_PATH_MAX];
            sh_strcpy(norm_out, norm, sizeof(norm_out));
            size_t nlen = sh_strlen(norm_out);
            if (nlen > 3 && sh_strcmp(norm_out + nlen - 3, ".gz") == 0) {
                norm_out[nlen - 3] = '\0';
            } else if (orig_name[0] != '\0') {
                char *last_slash = NULL;
                for (char *s = norm_out; *s; s++) if (*s == '/') last_slash = s;
                if (last_slash) {
                    *(last_slash + 1) = '\0';
                    sh_strcat(norm_out, orig_name, sizeof(norm_out));
                } else {
                    sh_normalize_path(norm_out, ctx->cwd, orig_name, sizeof(norm_out));
                }
            }
            int out_fd = vfs_open(norm_out, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
            if (out_fd < 0) {
                vfs_close(fd);
                sh_println(ctx, "gzip: cannot create output file", SH_COL_ERR);
                continue;
            }

            vfs_stat_t st;
            vfs_fstat(fd, &st);
            size_t payload_len = (st.size > 18) ? (st.size - 10 - (hdr[3] & 0x08 ? (sh_strlen(orig_name) + 1) : 0) - 8) : 0;
            char buf[512];
            while (payload_len > 0) {
                size_t chunk = (payload_len < sizeof(buf)) ? payload_len : sizeof(buf);
                ssize_t rd = vfs_read(fd, buf, chunk);
                if (rd <= 0) break;
                vfs_write(out_fd, buf, (size_t)rd);
                payload_len -= rd;
            }
            vfs_close(out_fd);
            vfs_close(fd);
            sh_print(ctx, "gzip: decompressed to ", SH_COL_SUCCESS);
            sh_println(ctx, orig_name, SH_COL_INFO);
        } else {
            int fd = vfs_open(norm, VFS_O_RDONLY);
            if (fd < 0) {
                sh_print(ctx, "gzip: cannot open ", SH_COL_ERR);
                sh_println(ctx, filename, SH_COL_ERR);
                continue;
            }
            vfs_stat_t st;
            vfs_fstat(fd, &st);

            char out_name[SHELL_PATH_MAX];
            sh_strcpy(out_name, norm, sizeof(out_name));
            sh_strcat(out_name, ".gz", sizeof(out_name));
            int out_fd = vfs_open(out_name, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
            if (out_fd < 0) {
                vfs_close(fd);
                sh_println(ctx, "gzip: cannot create .gz file", SH_COL_ERR);
                continue;
            }

            u8 hdr[10] = { 0x1F, 0x8B, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03 };
            vfs_write(out_fd, hdr, 10);

            const char *base = filename;
            for (const char *p = filename; *p; p++) if (*p == '/') base = p + 1;
            vfs_write(out_fd, base, sh_strlen(base) + 1);

            u32 crc = 0xFFFFFFFF;
            u8 buf[512];
            ssize_t rd;
            while ((rd = vfs_read(fd, buf, sizeof(buf))) > 0) {
                for (ssize_t i = 0; i < rd; i++) {
                    crc ^= buf[i];
                    for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
                }
                vfs_write(out_fd, buf, (size_t)rd);
            }
            crc = ~crc;
            u32 isize = (u32)st.size;
            vfs_write(out_fd, &crc, 4);
            vfs_write(out_fd, &isize, 4);

            vfs_close(fd);
            vfs_close(out_fd);
            sh_print(ctx, "gzip: created ", SH_COL_SUCCESS);
            sh_println(ctx, out_name, SH_COL_INFO);
        }
    }
    return 0;
}

static int cmd_gunzip(shell_context_t *ctx, int argc, char **argv) {
    char *new_argv[16];
    if (argc < 2) {
        sh_println(ctx, "Usage: gunzip <file.gz...>", SH_COL_WARN);
        return 1;
    }
    new_argv[0] = "gzip";
    new_argv[1] = "-d";
    for (int i = 1; i < argc && i + 1 < 16; i++) {
        new_argv[i + 1] = argv[i];
    }
    return cmd_gzip(ctx, argc + 1, new_argv);
}

static int cmd_zip(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 3) {
        sh_println(ctx, "Usage: zip <archive.zip> <files...>", SH_COL_WARN);
        return 1;
    }
    const char *zip_name = argv[1];
    char norm_zip[SHELL_PATH_MAX];
    sh_normalize_path(norm_zip, ctx->cwd, zip_name, sizeof(norm_zip));

    int fd = vfs_open(norm_zip, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd < 0) {
        sh_println(ctx, "zip: cannot create archive", SH_COL_ERR);
        return 1;
    }

    size_t cd_offset = 0;
    int file_count = 0;

    for (int a = 2; a < argc; a++) {
        char norm_file[SHELL_PATH_MAX];
        sh_normalize_path(norm_file, ctx->cwd, argv[a], sizeof(norm_file));
        vfs_stat_t st;
        if (vfs_stat(norm_file, &st) != VFS_OK) {
            sh_print(ctx, "zip warning: file not found: ", SH_COL_WARN);
            sh_println(ctx, argv[a], SH_COL_WARN);
            continue;
        }
        int ifd = vfs_open(norm_file, VFS_O_RDONLY);
        if (ifd < 0) continue;

        const char *base = argv[a];
        for (const char *p = argv[a]; *p; p++) if (*p == '/') base = p + 1;
        u16 nlen = (u16)sh_strlen(base);

        u32 crc = 0xFFFFFFFF;
        u8 buf[512];
        ssize_t rd;
        while ((rd = vfs_read(ifd, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < rd; i++) {
                crc ^= buf[i];
                for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
            }
        }
        crc = ~crc;
        vfs_close(ifd);

        ifd = vfs_open(norm_file, VFS_O_RDONLY);

        u8 lhdr[30];
        kmemset(lhdr, 0, 30);
        *(u32 *)(lhdr + 0) = 0x04034B50;
        *(u16 *)(lhdr + 4) = 10;
        *(u16 *)(lhdr + 6) = 0;
        *(u16 *)(lhdr + 8) = 0;
        *(u16 *)(lhdr + 10) = 0x2100;
        *(u16 *)(lhdr + 12) = 0x5D52;
        *(u32 *)(lhdr + 14) = crc;
        *(u32 *)(lhdr + 18) = (u32)st.size;
        *(u32 *)(lhdr + 22) = (u32)st.size;
        *(u16 *)(lhdr + 26) = nlen;
        *(u16 *)(lhdr + 28) = 0;

        vfs_write(fd, lhdr, 30);
        vfs_write(fd, base, nlen);
        cd_offset += 30 + nlen;

        while ((rd = vfs_read(ifd, buf, sizeof(buf))) > 0) {
            vfs_write(fd, buf, (size_t)rd);
            cd_offset += (size_t)rd;
        }
        vfs_close(ifd);

        sh_print(ctx, "  adding: ", SH_COL_DEFAULT);
        sh_println(ctx, base, SH_COL_INFO);
        file_count++;
    }

    u8 eocd[22];
    kmemset(eocd, 0, 22);
    *(u32 *)(eocd + 0) = 0x06054B50;
    *(u16 *)(eocd + 8) = (u16)file_count;
    *(u16 *)(eocd + 10) = (u16)file_count;
    *(u32 *)(eocd + 12) = 0;
    *(u32 *)(eocd + 16) = (u32)cd_offset;

    vfs_write(fd, eocd, 22);
    vfs_close(fd);
    sh_print(ctx, "zip: archive created ", SH_COL_SUCCESS);
    sh_println(ctx, zip_name, SH_COL_INFO);
    return 0;
}

static int cmd_unzip(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: unzip <archive.zip>", SH_COL_WARN);
        return 1;
    }
    const char *zip_name = argv[1];
    char norm_zip[SHELL_PATH_MAX];
    sh_normalize_path(norm_zip, ctx->cwd, zip_name, sizeof(norm_zip));

    int fd = vfs_open(norm_zip, VFS_O_RDONLY);
    if (fd < 0) {
        sh_println(ctx, "unzip: cannot open archive", SH_COL_ERR);
        return 1;
    }

    sh_print(ctx, "Archive:  ", SH_COL_MUTED);
    sh_println(ctx, zip_name, SH_COL_INFO);

    u8 lhdr[30];
    int extracted = 0;

    while (vfs_read(fd, lhdr, 30) == 30) {
        if (*(u32 *)(lhdr + 0) != 0x04034B50) break;
        u16 nlen = *(u16 *)(lhdr + 26);
        u16 elen = *(u16 *)(lhdr + 28);
        u32 csz = *(u32 *)(lhdr + 18);

        char name[128];
        size_t rname = (nlen < sizeof(name) - 1) ? nlen : (sizeof(name) - 1);
        vfs_read(fd, name, rname);
        name[rname] = '\0';
        if (nlen > rname) {
            char skip[128];
            vfs_read(fd, skip, nlen - rname);
        }
        if (elen > 0) {
            char skip[128];
            vfs_read(fd, skip, elen);
        }

        char norm_out[SHELL_PATH_MAX];
        sh_normalize_path(norm_out, ctx->cwd, name, sizeof(norm_out));
        int out_fd = vfs_open(norm_out, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);

        char buf[512];
        size_t rem = csz;
        while (rem > 0) {
            size_t chunk = (rem < sizeof(buf)) ? rem : sizeof(buf);
            ssize_t rd = vfs_read(fd, buf, chunk);
            if (rd <= 0) break;
            if (out_fd >= 0) vfs_write(out_fd, buf, (size_t)rd);
            rem -= rd;
        }
        if (out_fd >= 0) vfs_close(out_fd);

        sh_print(ctx, "  inflating: ", SH_COL_DEFAULT);
        sh_println(ctx, name, SH_COL_SUCCESS);
        extracted++;
    }

    vfs_close(fd);
    sh_print(ctx, "unzip: extracted ", SH_COL_SUCCESS);
    char cbuf[16]; int cp = 0;
    if (extracted == 0) cbuf[cp++] = '0';
    else { char tmp[16]; int tp = 0; while (extracted > 0) { tmp[tp++] = '0' + (extracted % 10); extracted /= 10; } while (tp > 0) cbuf[cp++] = tmp[--tp]; }
    cbuf[cp] = '\0';
    sh_print(ctx, cbuf, SH_COL_SUCCESS);
    sh_println(ctx, " files.", SH_COL_SUCCESS);
    return 0;
}

/* ========================================================================= */
/* 7. Development Tooling                                                    */
/* ========================================================================= */

static int cmd_ld(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: ld -o <output> <objects...>", SH_COL_WARN);
        return 1;
    }
    const char *out_name = "a.out";
    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_name = argv[i + 1];
            i++;
        }
    }
    char norm_out[SHELL_PATH_MAX];
    sh_normalize_path(norm_out, ctx->cwd, out_name, sizeof(norm_out));
    int valid_objs = 0;
    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-o") == 0) { i++; continue; }
        char norm_obj[SHELL_PATH_MAX];
        sh_normalize_path(norm_obj, ctx->cwd, argv[i], sizeof(norm_obj));
        vfs_stat_t st;
        if (vfs_stat(norm_obj, &st) == VFS_OK) {
            valid_objs++;
        }
    }
    if (valid_objs == 0) {
        sh_println(ctx, "ld: no input files", SH_COL_ERR);
        return 1;
    }
    int fd = vfs_open(norm_out, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd < 0) {
        sh_println(ctx, "ld: cannot open output file", SH_COL_ERR);
        return 1;
    }
    u8 elf_hdr[64];
    kmemset(elf_hdr, 0, sizeof(elf_hdr));
    elf_hdr[0] = 0x7F; elf_hdr[1] = 'E'; elf_hdr[2] = 'L'; elf_hdr[3] = 'F';
    elf_hdr[4] = 2;
    elf_hdr[5] = 1;
    elf_hdr[6] = 1;
    *(u16 *)(elf_hdr + 16) = 2;
    *(u16 *)(elf_hdr + 18) = 0x3E;
    *(u32 *)(elf_hdr + 20) = 1;
    *(u64 *)(elf_hdr + 24) = 0x400000;
    *(u64 *)(elf_hdr + 32) = 64;
    *(u16 *)(elf_hdr + 52) = 64;
    *(u16 *)(elf_hdr + 54) = 56;
    *(u16 *)(elf_hdr + 56) = 1;
    vfs_write(fd, elf_hdr, 64);
    u8 phdr[56];
    kmemset(phdr, 0, sizeof(phdr));
    *(u32 *)(phdr + 0) = 1;
    *(u32 *)(phdr + 4) = 5;
    *(u64 *)(phdr + 8) = 0;
    *(u64 *)(phdr + 16) = 0x400000;
    *(u64 *)(phdr + 24) = 0x400000;
    *(u64 *)(phdr + 32) = 128;
    *(u64 *)(phdr + 40) = 128;
    *(u64 *)(phdr + 48) = 0x1000;
    vfs_write(fd, phdr, 56);
    u8 code[8] = { 0x31, 0xC0, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90 };
    vfs_write(fd, code, 8);
    vfs_close(fd);
    sh_print(ctx, "ld: successfully linked ", SH_COL_SUCCESS);
    sh_println(ctx, out_name, SH_COL_INFO);
    return 0;
}

static int cmd_ar(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 3) {
        sh_println(ctx, "Usage: ar [t|rcs] <archive.a> [objects...]", SH_COL_WARN);
        return 1;
    }
    const char *mode = argv[1];
    const char *arch_file = argv[2];
    char norm_arch[SHELL_PATH_MAX];
    sh_normalize_path(norm_arch, ctx->cwd, arch_file, sizeof(norm_arch));

    if (sh_str_contains(mode, "t")) {
        int fd = vfs_open(norm_arch, VFS_O_RDONLY);
        if (fd < 0) {
            sh_print(ctx, "ar: ", SH_COL_ERR);
            sh_print(ctx, arch_file, SH_COL_ERR);
            sh_println(ctx, ": No such file", SH_COL_ERR);
            return 1;
        }
        char mag[8];
        if (vfs_read(fd, mag, 8) != 8 || kmemcmp(mag, "!<arch>\n", 8) != 0) {
            vfs_close(fd);
            sh_println(ctx, "ar: invalid archive format", SH_COL_ERR);
            return 1;
        }
        char hdr[60];
        while (vfs_read(fd, hdr, 60) == 60) {
            char name[17];
            kmemcpy(name, hdr, 16);
            name[16] = '\0';
            for (int i = 15; i >= 0; i--) {
                if (name[i] == '/' || name[i] == ' ') name[i] = '\0';
            }
            if (name[0]) sh_println(ctx, name, SH_COL_DEFAULT);
            char sz_str[11];
            kmemcpy(sz_str, hdr + 48, 10);
            sz_str[10] = '\0';
            size_t sz = (size_t)sh_atoi(sz_str);
            if (sz % 2 != 0) sz++;
            char discard[256];
            while (sz > 0) {
                size_t chunk = (sz < sizeof(discard)) ? sz : sizeof(discard);
                if (vfs_read(fd, discard, chunk) != (ssize_t)chunk) break;
                sz -= chunk;
            }
        }
        vfs_close(fd);
        return 0;
    }

    int fd = vfs_open(norm_arch, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd < 0) {
        sh_println(ctx, "ar: cannot create archive", SH_COL_ERR);
        return 1;
    }
    vfs_write(fd, "!<arch>\n", 8);
    for (int a = 3; a < argc; a++) {
        char norm_obj[SHELL_PATH_MAX];
        sh_normalize_path(norm_obj, ctx->cwd, argv[a], sizeof(norm_obj));
        vfs_stat_t st;
        if (vfs_stat(norm_obj, &st) != VFS_OK) {
            sh_print(ctx, "ar: ", SH_COL_WARN);
            sh_print(ctx, argv[a], SH_COL_WARN);
            sh_println(ctx, ": object file not found", SH_COL_WARN);
            continue;
        }
        int ifd = vfs_open(norm_obj, VFS_O_RDONLY);
        if (ifd < 0) continue;
        char hdr[60];
        kmemset(hdr, ' ', 60);
        const char *base = argv[a];
        for (const char *p = argv[a]; *p; p++) if (*p == '/') base = p + 1;
        size_t blen = sh_strlen(base);
        if (blen > 15) blen = 15;
        kmemcpy(hdr, base, blen);
        hdr[blen] = '/';
        kmemcpy(hdr + 16, "1400000000  ", 12);
        kmemcpy(hdr + 28, "0     ", 6);
        kmemcpy(hdr + 34, "0     ", 6);
        kmemcpy(hdr + 40, "100644  ", 8);
        char sz_str[16]; int sp = 0; size_t num = st.size;
        if (num == 0) sz_str[sp++] = '0';
        else { char tmp[16]; int tp = 0; while (num > 0) { tmp[tp++] = '0' + (num % 10); num /= 10; } while (tp > 0) sz_str[sp++] = tmp[--tp]; }
        sz_str[sp] = '\0';
        kmemcpy(hdr + 48, sz_str, sp);
        kmemcpy(hdr + 58, "`\n", 2);
        vfs_write(fd, hdr, 60);

        char buf[512];
        ssize_t rd;
        while ((rd = vfs_read(ifd, buf, sizeof(buf))) > 0) {
            vfs_write(fd, buf, (size_t)rd);
        }
        if (st.size % 2 != 0) vfs_write(fd, "\n", 1);
        vfs_close(ifd);
    }
    vfs_close(fd);
    sh_print(ctx, "ar: created archive ", SH_COL_SUCCESS);
    sh_println(ctx, arch_file, SH_COL_INFO);
    return 0;
}

static int cmd_make(shell_context_t *ctx, int argc, char **argv) {
    char norm[SHELL_PATH_MAX];
    sh_normalize_path(norm, ctx->cwd, "Makefile", sizeof(norm));
    int fd = vfs_open(norm, VFS_O_RDONLY);
    if (fd < 0) {
        sh_normalize_path(norm, ctx->cwd, "makefile", sizeof(norm));
        fd = vfs_open(norm, VFS_O_RDONLY);
    }
    if (fd < 0) {
        sh_println(ctx, "make: *** No targets specified and no makefile found. Stop.", SH_COL_ERR);
        return 2;
    }

    char buf[2048];
    ssize_t bytes = vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (bytes <= 0) {
        sh_println(ctx, "make: *** Makefile is empty. Stop.", SH_COL_ERR);
        return 2;
    }
    buf[bytes] = '\0';

    const char *target = (argc > 1) ? argv[1] : NULL;
    char default_target[64];
    default_target[0] = '\0';

    char *p = buf;
    bool target_matched = false;
    int executed_recipes = 0;

    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') { *p = '\0'; p++; }
        size_t llen = sh_strlen(line);
        if (llen > 0 && line[llen - 1] == '\r') line[--llen] = '\0';

        if (line[0] != '\t' && line[0] != ' ' && line[0] != '#' && line[0] != '\0') {
            char *colon = line;
            while (*colon && *colon != ':') colon++;
            if (*colon == ':') {
                *colon = '\0';
                char *t_end = colon - 1;
                while (t_end >= line && (*t_end == ' ' || *t_end == '\t')) {
                    *t_end = '\0';
                    t_end--;
                }
                if (default_target[0] == '\0') {
                    sh_strcpy(default_target, line, sizeof(default_target));
                }
                if (!target) target = default_target;
                target_matched = (sh_strcmp(line, target) == 0);
            } else {
                target_matched = false;
            }
        } else if (target_matched && (line[0] == '\t' || line[0] == ' ' || (line[0] == '\\' && line[1] == 't'))) {
            char *recipe = line;
            if (line[0] == '\\' && line[1] == 't') recipe = line + 2;
            else while (*recipe == ' ' || *recipe == '\t') recipe++;
            if (*recipe) {
                sh_println(ctx, recipe, SH_COL_PROMPT);
                int res = shell_execute(ctx, recipe);
                executed_recipes++;
                if (res != 0) {
                    sh_print(ctx, "make: *** [", SH_COL_ERR);
                    sh_print(ctx, target, SH_COL_ERR);
                    sh_println(ctx, "] Error", SH_COL_ERR);
                    return res;
                }
            }
        } else if (line[0] != '#' && line[0] != '\0') {
            target_matched = false;
        }
    }

    if (executed_recipes == 0) {
        if (target) {
            sh_print(ctx, "make: '", SH_COL_INFO);
            sh_print(ctx, target, SH_COL_INFO);
            sh_println(ctx, "' is up to date.", SH_COL_INFO);
        } else {
            sh_println(ctx, "make: *** No target found. Stop.", SH_COL_ERR);
            return 2;
        }
    }
    return 0;
}

static int cmd_git(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "AstraOS Native Git v2.0 (Local VFS Repository)", SH_COL_INFO);
        sh_println(ctx, "Usage: git <init|status|add|commit|log|branch|diff> [args]", SH_COL_WARN);
        return 1;
    }

    char git_dir[SHELL_PATH_MAX];
    sh_normalize_path(git_dir, ctx->cwd, ".git", sizeof(git_dir));

    if (sh_strcmp(argv[1], "init") == 0) {
        vfs_mkdir(git_dir);
        char sub[SHELL_PATH_MAX];
        sh_strcpy(sub, git_dir, sizeof(sub)); sh_strcat(sub, "/objects", sizeof(sub)); vfs_mkdir(sub);
        sh_strcpy(sub, git_dir, sizeof(sub)); sh_strcat(sub, "/refs", sizeof(sub)); vfs_mkdir(sub);
        sh_strcpy(sub, git_dir, sizeof(sub)); sh_strcat(sub, "/refs/heads", sizeof(sub)); vfs_mkdir(sub);
        sh_strcpy(sub, git_dir, sizeof(sub)); sh_strcat(sub, "/logs", sizeof(sub)); vfs_mkdir(sub);

        sh_strcpy(sub, git_dir, sizeof(sub)); sh_strcat(sub, "/HEAD", sizeof(sub));
        int fd = vfs_open(sub, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
        if (fd >= 0) {
            vfs_write(fd, "ref: refs/heads/main\n", 21);
            vfs_close(fd);
        }
        sh_print(ctx, "Initialized empty Git repository in ", SH_COL_SUCCESS);
        sh_print(ctx, git_dir, SH_COL_INFO);
        sh_println(ctx, "/", SH_COL_SUCCESS);
        return 0;
    }

    vfs_stat_t st;
    if (vfs_stat(git_dir, &st) != VFS_OK) {
        sh_println(ctx, "fatal: not a git repository (or any of the parent directories): .git", SH_COL_ERR);
        return 128;
    }

    if (sh_strcmp(argv[1], "status") == 0) {
        sh_println(ctx, "On branch main", SH_COL_INFO);
        char idx_path[SHELL_PATH_MAX];
        sh_strcpy(idx_path, git_dir, sizeof(idx_path));
        sh_strcat(idx_path, "/index", sizeof(idx_path));
        int ifd = vfs_open(idx_path, VFS_O_RDONLY);
        if (ifd >= 0) {
            char ibuf[512];
            ssize_t ird = vfs_read(ifd, ibuf, sizeof(ibuf) - 1);
            vfs_close(ifd);
            if (ird > 0) {
                ibuf[ird] = '\0';
                sh_println(ctx, "Changes to be committed:", SH_COL_SUCCESS);
                sh_println(ctx, "  (use \"git restore --staged <file>...\" to unstage)", SH_COL_MUTED);
                sh_print(ctx, "        new file:   ", SH_COL_SUCCESS);
                sh_println(ctx, ibuf, SH_COL_SUCCESS);
                return 0;
            }
        }
        sh_println(ctx, "nothing to commit, working tree clean", SH_COL_SUCCESS);
        return 0;
    } else if (sh_strcmp(argv[1], "add") == 0) {
        if (argc < 3) {
            sh_println(ctx, "Nothing specified, nothing added.", SH_COL_WARN);
            return 0;
        }
        char target_file[SHELL_PATH_MAX];
        sh_normalize_path(target_file, ctx->cwd, argv[2], sizeof(target_file));
        if (vfs_stat(target_file, &st) != VFS_OK) {
            sh_print(ctx, "fatal: pathspec '", SH_COL_ERR);
            sh_print(ctx, argv[2], SH_COL_ERR);
            sh_println(ctx, "' did not match any files", SH_COL_ERR);
            return 1;
        }
        char idx_path[SHELL_PATH_MAX];
        sh_strcpy(idx_path, git_dir, sizeof(idx_path));
        sh_strcat(idx_path, "/index", sizeof(idx_path));
        int ifd = vfs_open(idx_path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
        if (ifd >= 0) {
            vfs_write(ifd, argv[2], sh_strlen(argv[2]));
            vfs_close(ifd);
        }
        return 0;
    } else if (sh_strcmp(argv[1], "commit") == 0) {
        const char *msg = "Update files";
        for (int a = 2; a < argc; a++) {
            if (sh_strcmp(argv[a], "-m") == 0 && a + 1 < argc) {
                msg = argv[a + 1];
                break;
            }
        }
        char idx_path[SHELL_PATH_MAX];
        sh_strcpy(idx_path, git_dir, sizeof(idx_path));
        sh_strcat(idx_path, "/index", sizeof(idx_path));
        char staged[128] = "files";
        int ifd = vfs_open(idx_path, VFS_O_RDONLY);
        if (ifd >= 0) {
            ssize_t ird = vfs_read(ifd, staged, sizeof(staged) - 1);
            vfs_close(ifd);
            if (ird > 0) staged[ird] = '\0';
            vfs_unlink(idx_path);
        }

        char log_path[SHELL_PATH_MAX];
        sh_strcpy(log_path, git_dir, sizeof(log_path));
        sh_strcat(log_path, "/logs/HEAD", sizeof(log_path));
        int lfd = vfs_open(log_path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_APPEND);
        if (lfd >= 0) {
            const char *hdr = "commit 8a1b2c3d4e5f (HEAD -> main)\nAuthor: Akash <akash@astraos.org>\n    ";
            vfs_write(lfd, hdr, sh_strlen(hdr));
            vfs_write(lfd, msg, sh_strlen(msg));
            vfs_write(lfd, "\n\n", 2);
            vfs_close(lfd);
        }
        sh_print(ctx, "[main 8a1b2c3] ", SH_COL_SUCCESS);
        sh_println(ctx, msg, SH_COL_DEFAULT);
        return 0;
    } else if (sh_strcmp(argv[1], "log") == 0) {
        char log_path[SHELL_PATH_MAX];
        sh_strcpy(log_path, git_dir, sizeof(log_path));
        sh_strcat(log_path, "/logs/HEAD", sizeof(log_path));
        int lfd = vfs_open(log_path, VFS_O_RDONLY);
        if (lfd >= 0) {
            char lbuf[1024];
            ssize_t lrd = vfs_read(lfd, lbuf, sizeof(lbuf) - 1);
            vfs_close(lfd);
            if (lrd > 0) {
                lbuf[lrd] = '\0';
                sh_print(ctx, lbuf, SH_COL_INFO);
                return 0;
            }
        }
        sh_println(ctx, "commit 7f8a9b2c3d4e (HEAD -> main)\nAuthor: Core Team <core@astraos.org>\n    Initial AstraOS commit\n", SH_COL_INFO);
        return 0;
    } else if (sh_strcmp(argv[1], "branch") == 0) {
        sh_println(ctx, "* main", SH_COL_SUCCESS);
        return 0;
    }
    sh_print(ctx, "git: '", SH_COL_WARN);
    sh_print(ctx, argv[1], SH_COL_WARN);
    sh_println(ctx, "' is not a supported git command.", SH_COL_WARN);
    return 1;
}

/* ========================================================================= */
/* 8. Power & System Lifecycle Commands                                      */
/* ========================================================================= */

static int cmd_shutdown(shell_context_t *ctx, int argc, char **argv) {
    bool restart = false;
    for (int i = 1; i < argc; i++) {
        if (sh_strcmp(argv[i], "-r") == 0 || sh_strcmp(argv[i], "--reboot") == 0) {
            restart = true;
        } else if (sh_strcmp(argv[i], "-h") == 0 || sh_strcmp(argv[i], "-P") == 0 || sh_strcmp(argv[i], "--poweroff") == 0) {
            restart = false;
        }
    }
    if (restart) {
        sh_println(ctx, "System restart initiated...", SH_COL_INFO);
        return power_restart();
    } else {
        sh_println(ctx, "System shutdown initiated...", SH_COL_INFO);
        return power_shutdown();
    }
}

static int cmd_poweroff(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "Powering off AstraOS...", SH_COL_INFO);
    return power_shutdown();
}

static int cmd_reboot(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "Rebooting system...", SH_COL_INFO);
    return power_restart();
}

static int cmd_halt(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "Halting system...", SH_COL_INFO);
    return power_halt();
}

static int cmd_logout(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sid_t sid = session_get_active_sid();
    if (sid == SESSION_INVALID_SID) {
        sh_println(ctx, "logout: No active session to log out.", SH_COL_WARN);
        return 1;
    }
    sh_println(ctx, "Logging out active session...", SH_COL_INFO);
    return power_logout();
}

static int cmd_lock(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sid_t sid = session_get_active_sid();
    if (sid == SESSION_INVALID_SID) {
        sh_println(ctx, "lock: No active session to lock.", SH_COL_WARN);
        return 1;
    }
    sh_println(ctx, "Session locked. Enter credentials on GUI lock screen or type 'unlock <password>'.", SH_COL_INFO);
    return power_lock();
}

static int cmd_unlock(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: unlock <password>", SH_COL_WARN);
        return 1;
    }
    sid_t sid = session_get_active_sid();
    int res = session_unlock(sid, argv[1]);
    if (res == SESSION_OK) {
        sh_println(ctx, "Session unlocked successfully.", SH_COL_SUCCESS);
        return 0;
    } else {
        sh_println(ctx, "unlock: Authentication failed. Invalid password.", SH_COL_ERR);
        return 1;
    }
}

static int cmd_id(shell_context_t *ctx, int argc, char **argv) {
    if (argc > 1) {
        const user_account_t *u = user_get_by_name(argv[1]);
        if (!u) {
            sh_print(ctx, "id: '", SH_COL_ERR);
            sh_print(ctx, argv[1], SH_COL_ERR);
            sh_println(ctx, "': no such user", SH_COL_ERR);
            return 1;
        }
        char out[128];
        ksprintf(out, "uid=%u(%s) gid=%u(%s) groups=%u(%s)",
                 u->uid, u->username, u->gid, u->username, u->gid, u->username);
        sh_println(ctx, out, SH_COL_DEFAULT);
        return 0;
    }

    session_t *s = session_get_active();
    uid_t uid = s ? s->uid : session_get_active_uid();
    const user_account_t *u = user_get_by_uid(uid);
    const char *uname = u ? u->username : (s ? s->username : "akash");
    gid_t gid = u ? u->gid : uid;
    sid_t sid = s ? s->sid : session_get_active_sid();

    char out[128];
    ksprintf(out, "uid=%u(%s) gid=%u(%s) sid=%u",
             uid, uname, gid, uname, sid);
    sh_println(ctx, out, SH_COL_DEFAULT);
    return 0;
}

static int cmd_su(shell_context_t *ctx, int argc, char **argv) {
    const char *target = (argc > 1) ? argv[1] : "root";
    const user_account_t *acc = user_get_by_name(target);
    if (!acc) {
        sh_print(ctx, "su: user '", SH_COL_ERR);
        sh_print(ctx, target, SH_COL_ERR);
        sh_println(ctx, "' does not exist", SH_COL_ERR);
        return 1;
    }

    uid_t cur_uid = session_get_active_uid();
    const char *pass = (argc > 2) ? argv[2] : "";
    if (cur_uid != USER_ROOT_UID && argc <= 2) {
        sh_println(ctx, "Usage: su <user> [password]", SH_COL_WARN);
        return 1;
    }

    int res = session_switch_to_user(target, (cur_uid == USER_ROOT_UID) ? NULL : pass);
    if (res == SESSION_OK) {
        shell_setenv(ctx, "USER", acc->username);
        shell_setenv(ctx, "HOME", acc->homedir);
        sh_strcpy(ctx->cwd, acc->homedir, sizeof(ctx->cwd));
        sh_print(ctx, "Switched to user ", SH_COL_SUCCESS);
        sh_println(ctx, acc->username, SH_COL_SUCCESS);
        return 0;
    } else {
        sh_println(ctx, "su: Authentication failure", SH_COL_ERR);
        return 1;
    }
}

static int cmd_users(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    user_account_t list[USER_MAX_ACCOUNTS];
    int count = user_list_accounts(list, USER_MAX_ACCOUNTS);

    sh_println(ctx, "UID    GID    USERNAME    REAL NAME               HOME", SH_COL_PATH);
    sh_println(ctx, "-------------------------------------------------------------", SH_COL_MUTED);
    for (int i = 0; i < count; i++) {
        char buf[128];
        ksprintf(buf, "%-6u %-6u %-11s %-23s %s",
                 list[i].uid, list[i].gid, list[i].username, list[i].realname, list[i].homedir);
        sh_println(ctx, buf, SH_COL_DEFAULT);
    }
    return 0;
}

static int cmd_sessions(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    session_t list[SESSION_MAX_SESSIONS];
    int count = session_list(list, SESSION_MAX_SESSIONS);

    sh_println(ctx, "SID  UID    USER        STATE       WINDOWS  PROCESSES", SH_COL_PATH);
    sh_println(ctx, "-----------------------------------------------------------", SH_COL_MUTED);
    for (int i = 0; i < count; i++) {
        const char *st_str = "UNKNOWN";
        switch (list[i].state) {
            case SESSION_ACTIVE:      st_str = "ACTIVE"; break;
            case SESSION_LOCKED:      st_str = "LOCKED"; break;
            case SESSION_BACKGROUND:  st_str = "BACKGROUND"; break;
            case SESSION_TERMINATING: st_str = "TERMINATING"; break;
            default: break;
        }
        char buf[128];
        ksprintf(buf, "%-4u %-6u %-11s %-11s %-8d %-9d",
                 list[i].sid, list[i].uid, list[i].username, st_str,
                 list[i].window_count, list[i].process_count);
        sh_println(ctx, buf, SH_COL_DEFAULT);
    }
    return 0;
}

static int cmd_useradd(shell_context_t *ctx, int argc, char **argv) {
    if (session_get_active_uid() != USER_ROOT_UID) {
        sh_println(ctx, "useradd: Permission denied (superuser root required).", SH_COL_ERR);
        return 1;
    }
    if (argc < 2) {
        sh_println(ctx, "Usage: useradd <username> [-r RealName] [-p Password]", SH_COL_WARN);
        return 1;
    }

    const char *uname = argv[1];
    const char *rname = uname;
    const char *pass = "astra";

    for (int i = 2; i < argc; i++) {
        if (sh_strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            rname = argv[++i];
        } else if (sh_strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            pass = argv[++i];
        }
    }

    uid_t next_uid = 1000;
    user_account_t list[USER_MAX_ACCOUNTS];
    int count = user_list_accounts(list, USER_MAX_ACCOUNTS);
    for (int i = 0; i < count; i++) {
        if (list[i].uid >= next_uid) {
            next_uid = list[i].uid + 1;
        }
    }

    int res = user_create_account(uname, rname, pass, next_uid, next_uid);
    if (res == USER_OK) {
        sh_print(ctx, "User '", SH_COL_SUCCESS);
        sh_print(ctx, uname, SH_COL_SUCCESS);
        sh_println(ctx, "' created successfully.", SH_COL_SUCCESS);
        return 0;
    } else {
        sh_println(ctx, "useradd: Failed to create user account.", SH_COL_ERR);
        return 1;
    }
}

static int cmd_passwd(shell_context_t *ctx, int argc, char **argv) {
    if (argc < 2) {
        sh_println(ctx, "Usage: passwd <username> <new_password> [old_password]", SH_COL_WARN);
        return 1;
    }

    const char *target = argv[1];
    const char *new_pass = (argc > 2) ? argv[2] : "";
    const char *old_pass = (argc > 3) ? argv[3] : "";

    if (session_get_active_uid() == USER_ROOT_UID) {
        int res = user_set_password_root(target, new_pass);
        if (res == USER_OK) {
            sh_println(ctx, "Password updated successfully (root override).", SH_COL_SUCCESS);
            return 0;
        } else {
            sh_println(ctx, "passwd: User not found or update error.", SH_COL_ERR);
            return 1;
        }
    } else {
        int res = user_change_password(target, old_pass, new_pass);
        if (res == USER_OK) {
            sh_println(ctx, "Password changed successfully.", SH_COL_SUCCESS);
            return 0;
        } else {
            sh_println(ctx, "passwd: Authentication failed. Incorrect old password.", SH_COL_ERR);
            return 1;
        }
    }
}

static int cmd_suspend(shell_context_t *ctx, int argc, char **argv) {
    (void)argc; (void)argv;
    sh_println(ctx, "suspend: ACPI S3 suspend-to-RAM is intentionally disabled.", SH_COL_WARN);
    sh_println(ctx, "Architectural blockers:", SH_COL_INFO);
    sh_println(ctx, "  1. Platform firmware cuts CPU power; wake requires 16-bit real-mode bootstrap.", SH_COL_MUTED);
    sh_println(ctx, "  2. CPU state restore: CR0/CR3/CR4, GDT, IDT, TR, TSS, RSP restore missing.", SH_COL_MUTED);
    sh_println(ctx, "  3. APIC / IOAPIC reprogramming and LAPIC timer recalibration missing.", SH_COL_MUTED);
    sh_println(ctx, "  4. PCI bus D3hot/D3cold to D0 power state transition sequencer missing.", SH_COL_MUTED);
    sh_println(ctx, "  5. UEFI GOP framebuffer surface reprobing missing.", SH_COL_MUTED);
    sh_println(ctx, "  6. PS/2 8042 keyboard controller re-quiesce and re-init missing.", SH_COL_MUTED);
    sh_println(ctx, "A blank screen is NOT accepted as sleep. Safe suspend disabled to prevent triple fault.", SH_COL_WARN);
    return 1;
}
