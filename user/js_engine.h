/*
 * AstraOS Phase R — Isolated JavaScript Engine Header
 *
 * Lightweight, strictly sandboxed, unprivileged Ring 3 JavaScript subset runtime.
 * Implements an integer-only, deterministic execution environment with hard resource
 * limits (memory budget, instruction budget, call depth) and safe DOM bridging.
 */

#ifndef ASTRA_USER_JS_ENGINE_H
#define ASTRA_USER_JS_ENGINE_H

#ifndef ASTRA_TYPES_DEFINED
#define ASTRA_TYPES_DEFINED
typedef unsigned long size_t;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void*)0)
#endif

/* Resource & Execution Limits */
#define JS_MAX_SCRIPT_LEN       4096
#define JS_DEFAULT_INSTRUCTIONS 10000
#define JS_DEFAULT_CALL_DEPTH   16
#define JS_MAX_VARS             64
#define JS_MAX_FUNCTIONS        16
#define JS_MAX_PARAMS           8
#define JS_MAX_NAME_LEN         32
#define JS_MAX_STRING_LEN       256
#define JS_DEFAULT_MEM_BUDGET   32768

/* Value Types */
typedef enum {
    JS_VAL_UNDEFINED = 0,
    JS_VAL_NULL,
    JS_VAL_BOOL,
    JS_VAL_NUMBER,      /* Signed 64-bit integer */
    JS_VAL_STRING,      /* Bounded string buffer */
    JS_VAL_FUNCTION,    /* User function index */
    JS_VAL_OBJECT,      /* Host object (document, console, etc.) */
    JS_VAL_DOM_ELEMENT  /* Safe handle to user-space DOM node */
} js_val_type_t;

/* DOM Element Handle with Document Generation Guard */
typedef struct {
    u32 doc_generation;
    int node_index;
} js_dom_handle_t;

/* Value Representation */
typedef struct {
    js_val_type_t type;
    union {
        bool boolean;
        s64 number;
        const char *string;  /* Pointer into sandboxed mem_pool or static literal */
        int func_index;
        int host_object_id;
        js_dom_handle_t dom_handle;
    } u;
} js_value_t;

/* Error Codes */
typedef enum {
    JS_OK = 0,
    JS_ERR_SYNTAX,
    JS_ERR_TIMEOUT,
    JS_ERR_STACK_OVERFLOW,
    JS_ERR_OUT_OF_MEMORY,
    JS_ERR_OVERSIZED,
    JS_ERR_UNSUPPORTED_API,
    JS_ERR_DIV_BY_ZERO,
    JS_ERR_OVERFLOW,
    JS_ERR_TYPE,
    JS_ERR_UNDEFINED_VAR,
    JS_ERR_INVALID_HANDLE
} js_error_t;

/* Host Object IDs */
#define JS_HOST_DOCUMENT 1
#define JS_HOST_CONSOLE  2

/* Variable Entry */
typedef struct {
    char name[JS_MAX_NAME_LEN];
    js_value_t val;
} js_var_t;

/* Function Definition */
typedef struct {
    char name[JS_MAX_NAME_LEN];
    char params[JS_MAX_PARAMS][JS_MAX_NAME_LEN];
    int param_count;
    const char *body_start;
    const char *body_end;
} js_func_t;

/* Call Frame for Recursion Depth Tracking */
typedef struct {
    char func_name[JS_MAX_NAME_LEN];
} js_frame_t;

/* Virtual Machine Context */
typedef struct {
    /* Memory sandbox */
    char mem_pool[JS_DEFAULT_MEM_BUDGET];
    size_t mem_used;
    size_t mem_budget;

    /* Execution counters */
    u32 instruction_count;
    u32 max_instructions;
    int call_depth;
    int max_call_depth;
    js_frame_t call_stack[JS_DEFAULT_CALL_DEPTH];

    /* Variables & Functions */
    js_var_t vars[JS_MAX_VARS];
    int var_count;
    js_func_t funcs[JS_MAX_FUNCTIONS];
    int func_count;

    /* DOM Bridge Callbacks */
    void *dom_context;
    int (*dom_get_element_by_id)(void *ctx, const char *id, u32 *out_doc_gen);
    const char *(*dom_get_text_content)(void *ctx, u32 doc_gen, int node_idx);
    bool (*dom_set_text_content)(void *ctx, u32 doc_gen, int node_idx, const char *text);
    bool (*dom_set_onclick)(void *ctx, u32 doc_gen, int node_idx, const char *code);

    /* Status & Diagnostic Message */
    js_error_t last_error;
    char error_msg[128];
} js_vm_t;

/* VM Lifecycle & Configuration */
void js_init(js_vm_t *vm);
void js_set_limits(js_vm_t *vm, u32 max_instructions, int max_call_depth, size_t mem_budget);
void js_set_dom_bridge(js_vm_t *vm, void *ctx,
                       int (*get_elem)(void*, const char*, u32*),
                       const char *(*get_text)(void*, u32, int),
                       bool (*set_text)(void*, u32, int, const char*));
void js_set_dom_event_bridge(js_vm_t *vm,
                             bool (*set_onclick)(void*, u32, int, const char*));

/* Evaluation & Diagnostics */
js_error_t js_eval(js_vm_t *vm, const char *source, js_value_t *out_val);
const char *js_error_string(js_error_t err);

/* Unit Self-Tests */
int js_run_self_tests(void);
int js_run_phase_s_self_tests(void);
int js_run_phase_t_self_tests(void);
int js_run_phase_u_self_tests(void);

#endif /* ASTRA_USER_JS_ENGINE_H */

