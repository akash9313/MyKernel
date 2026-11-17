# AstraOS Terminal 2.0 — Shell Runtime Architecture & Execution Report

## 1. Executive Summary

This report documents the architectural design, pipeline engine, tokenization rules, and process execution mechanics of the **AstraOS Native Shell Runtime**.

The AstraOS shell bridges interactive user input with kernel subsystems and user-space binaries. It provides full support for POSIX-compliant script chaining, variable expansion, in-memory pipelines, file descriptor redirection, interactive command history, and user-space ELF64 process execution.

---

## 2. Shell Context & Data Structures

Each active shell instance is encapsulated in a dedicated `shell_context_t` structure (`kernel/include/kernel/shell.h`):

```c
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
```

### Context Isolation
1. **Re-entrant Design**: Shell output is routed through a function pointer callback `out_fn(ctx, text, color)`. The same shell engine seamlessly drives graphical terminals (`app_terminal`), serial COM1 monitor console, and automated self-test harnesses without code modification.
2. **Current Working Directory (CWD)**: Maintained per context. Paths are automatically normalized relative to `cwd` with full traversal resolution (`.`, `..`, `/`).
3. **Environment Table**: Supports up to 32 active environment key-value pairs (e.g. `PATH`, `USER`, `HOME`, `SHELL`, `TERM`, `OS`).

---

## 3. Lexical Analysis, Tokenization & Variable Expansion

The shell command processor employs a deterministic tokenizer that handles quotes, escapes, and variable substitutions before dispatching commands:

### 1. Variable Substitution (`expand_variables`)
- References such as `$VAR` or `${VAR}` are looked up in the session environment table and substituted inline.
- `$?` expands to the numeric return value of the previous command (`ctx->last_exit_code`).
- Variables are expanded outside single quotes; within double quotes, variables expand normally.

### 2. Argument Tokenization (`parse_args`)
- **Single Quotes (`'...'`)**: Preserves the literal value of each character within the quotes; no variable expansion.
- **Double Quotes (`"..."`)**: Groups whitespace-separated words into a single argument while allowing variable expansion.
- **Backslash Escaping**: Recognizes `\n`, `\t`, `\\`, `\"`, `\'`, and octal/hex escape codes.

---

## 4. Pipeline Engine & Control Flow Chaining

The execution engine evaluates compound command lines in hierarchical stages:

```
[Command Line Input]
         |
         v
 [1. Sequential Semicolons (;) ]
         |
         v
 [2. Conditional Chaining (&& / ||) ]
         |
         v
 [3. In-Memory Pipes (|) ]
         |
         v
 [4. Stream Redirection (<, >, >>) ]
         |
         v
 [5. Command Dispatch (Builtin vs User ELF) ]
```

### 1. Sequential Chaining (`;`)
Separates commands executed in sequence regardless of success or failure:
`echo "Step 1" ; echo "Step 2"`

### 2. Logical Short-Circuit Chaining (`&&` and `||`)
- `cmd1 && cmd2`: `cmd2` executes only if `cmd1` returns `0` (success).
- `cmd1 || cmd2`: `cmd2` executes only if `cmd1` returns non-zero (failure).

### 3. In-Memory Streaming Pipeline (`|`)
- Implemented with an 8 KiB in-memory circular pipe buffer (`g_pipe_buf`).
- When a pipeline is encountered (`cmd1 | cmd2`):
  1. `cmd1` executes with output redirected into `g_pipe_buf`.
  2. `g_has_pipe_input` is flagged, and `g_pipe_len` is recorded.
  3. `cmd2` executes with `s_stream_buf` loaded from `g_pipe_buf` as its standard input stream.
  4. Stream-aware utilities (`cat`, `wc`, `grep`, `sort`, `uniq`, `head`, `tail`, `tr`, `cut`) consume the stream directly.

### 4. File Stream Redirection (`>`, `>>`, `<`)
- `> file`: Truncates or creates target file and redirects output.
- `>> file`: Appends command output to target file.
- `< file`: Feeds target file contents as standard input to the command.

---

## 5. Process Execution Model & Ring 3 Containment

When a command name is not found in the built-in table (`g_builtins[]`), the shell searches executable paths (`/bin`, `/usr/bin`, `/pkg/bin`):

```
+-------------------------------------------------------------------+
|               AstraOS Dual Execution Architecture                 |
+-------------------------------------------------------------------+
| Ring 0 (Kernel Builtins)   | ls, grep, pkg, netstat, ps, etc.     |
| Ring 3 (User-Space ELFs)   | /bin/user_app.elf, /bin/browser.elf  |
+-------------------------------------------------------------------+
```

### User Process Creation & Isolation
1. **ELF64 Validation**: `elf_load()` parses ELF magic (`0x7F 'E' 'L' 'F'`), verifies 64-bit AMD x86-64 executable architecture, and validates program headers.
2. **Virtual Address Space**: `vmm_create_user_address_space()` creates an isolated PML4 table with user-accessible pages (`PTE_USER`) mapped between `0x0000000000400000` and `0x0000700000000000`.
3. **Supervisor Boundary Protection**: Supervisor memory (`0xffffffff80000000` .. `0xffffffff80400000`) is mapped with `User=0`.
4. **Fault Containment**: If a user application faults (e.g. invalid pointer, division by zero, privileged instruction), the CPU triggers a Page Fault (`#PF` Vector 14) or General Protection Fault (`#GP` Vector 13). The kernel exception handler safely terminates the offending process with an exit code, reclaims all allocated pages, and returns to the interactive shell prompt without halting the kernel.

---

## 6. Qualification Results

The Native Shell Subsystem passed 100% of the rigorous validation tests in the kernel qualification test suite:

- Environment variable manipulation and `$VAR` expansion: **PASS**
- Quoted argument parsing and tokenization: **PASS**
- Builtin command execution: **PASS**
- Sequential (`;`) and short-circuit (`&&`, `||`) chaining: **PASS**
- In-memory pipe streams (`cmd1 | cmd2`): **PASS**
- File redirection (`>`, `>>`, `<`): **PASS**
- Directory creation, tree inspection, and removal: **PASS**
- Interactive missing-command detection and 3-stage package prompt: **PASS**
- Marker emitted: **`ASTRA_NATIVE_SHELL_OK`**
