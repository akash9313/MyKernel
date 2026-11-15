#ifndef ASTRA_KERNEL_SHELL_H
#define ASTRA_KERNEL_SHELL_H

#include <kernel/types.h>

#define SHELL_ENV_MAX      32
#define SHELL_KEY_MAX      32
#define SHELL_VAL_MAX      128
#define SHELL_ARGS_MAX     32
#define SHELL_PATH_MAX     128
#define SHELL_LINE_MAX     256
#define SHELL_ALIAS_MAX    16

typedef void (*shell_out_fn)(void *ctx, const char *str, u32 color);

typedef struct {
    char key[SHELL_KEY_MAX];
    char val[SHELL_VAL_MAX];
    bool in_use;
} shell_env_entry_t;

typedef struct {
    char name[32];
    char value[128];
    bool in_use;
} shell_alias_t;

typedef struct shell_context {
    char              cwd[SHELL_PATH_MAX];
    char              old_pwd[SHELL_PATH_MAX];
    shell_env_entry_t env[SHELL_ENV_MAX];
    shell_alias_t     aliases[SHELL_ALIAS_MAX];
    int               last_exit_code;
    shell_out_fn      out_fn;
    void             *out_ctx;
    
    /* Interactive command-not-found pending installation prompt state */
    bool              pending_install;
    int               pending_stage; /* 1: initial menu [1][2][3], 2: post-info menu [1][3] */
    char              pending_pkg[32];
} shell_context_t;

void shell_init(shell_context_t *ctx, shell_out_fn out_fn, void *out_ctx);
const char *shell_getenv(shell_context_t *ctx, const char *key);
int shell_setenv(shell_context_t *ctx, const char *key, const char *val);
int shell_unsetenv(shell_context_t *ctx, const char *key);
const char *shell_getcwd(shell_context_t *ctx);
int shell_chdir(shell_context_t *ctx, const char *path);
int shell_execute(shell_context_t *ctx, const char *cmd_line);
bool shell_self_test(void);

#endif /* ASTRA_KERNEL_SHELL_H */

