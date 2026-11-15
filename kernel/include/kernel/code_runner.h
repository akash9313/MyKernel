#ifndef ASTRA_KERNEL_CODE_RUNNER_H
#define ASTRA_KERNEL_CODE_RUNNER_H

#include <kernel/types.h>
#include <kernel/shell.h>

/* Universal Code Runner API */
int code_runner_execute_file(shell_context_t *ctx, const char *filepath);
int code_runner_python(shell_context_t *ctx, const char *script, const char *filepath);
int code_runner_python_interactive(shell_context_t *ctx);
int code_runner_c(shell_context_t *ctx, const char *source, const char *filepath);
int code_runner_sh(shell_context_t *ctx, const char *script, const char *filepath);

#endif /* ASTRA_KERNEL_CODE_RUNNER_H */

