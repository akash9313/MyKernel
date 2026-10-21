/*
 * AstraOS Phase S — Hardened Isolated JavaScript Engine Implementation
 *
 * Fully freestanding, unprivileged Ring 3 JavaScript subset runtime.
 * Pure integer arithmetic, zero FPU/SSE instruction generation (-mgeneral-regs-only safe).
 * Hard deterministic limits on instructions, call stack, memory, and source size.
 * Low stack footprint: 16-byte js_value_t, zero large local arrays.
 * Hardened with:
 *   - Operator precedence & parenthesis grouping
 *   - Negative numbers & unary chaining (!, -)
 *   - Signed integer overflow protection (__builtin_*_overflow & LLONG_MIN / -1 guard)
 *   - Logical operator short-circuiting (&&, ||)
 *   - Stale DOM handle detection across navigation (doc_generation)
 *   - Memory arena exhaustion & recovery
 */

#include "js_engine.h"

/* Syscall prototype for console.log / serial diagnostics */
s64 sys_write(int fd, const void *buf, size_t count);

#define LLONG_MIN_VAL (-9223372036854775807LL - 1LL)
#define LLONG_MAX_VAL (9223372036854775807LL)

/* -----------------------------------------------------------------
 * Freestanding String & Utility Functions
 * ----------------------------------------------------------------- */
static size_t js_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static void js_strcpy(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void js_strcat(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t dlen = js_strlen(dst);
    size_t si = 0;
    while (src && src[si] && (dlen + si) < max_len - 1) {
        dst[dlen + si] = src[si];
        si++;
    }
    dst[dlen + si] = '\0';
}


static int js_strcmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return (s1 == s2) ? 0 : (s1 ? 1 : -1);
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static bool js_streq(const char *s1, const char *s2) {
    return js_strcmp(s1, s2) == 0;
}

static void js_memset(void *dst, int val, size_t n) {
    u8 *d = (u8 *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (u8)val;
}

static bool js_isdigit(char c) {
    return c >= '0' && c <= '9';
}

static bool js_isalpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

static bool js_isalnum(char c) {
    return js_isalpha(c) || js_isdigit(c);
}

static bool js_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void js_s64_to_str(s64 val, char *buf, size_t max_len) {
    if (!buf || max_len < 2) return;
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    bool neg = false;
    u64 uval;
    if (val < 0) {
        neg = true;
        if (val == LLONG_MIN_VAL) {
            uval = 9223372036854775808ULL;
        } else {
            uval = (u64)(-val);
        }
    } else {
        uval = (u64)val;
    }

    char tmp[32];
    int idx = 0;
    while (uval > 0 && idx < 30) {
        tmp[idx++] = (char)('0' + (uval % 10));
        uval /= 10;
    }
    if (neg) tmp[idx++] = '-';

    size_t out_idx = 0;
    for (int k = idx - 1; k >= 0 && out_idx < max_len - 1; k--) {
        buf[out_idx++] = tmp[k];
    }
    buf[out_idx] = '\0';
}

/* -----------------------------------------------------------------
 * Sandboxed Arena Allocation for Dynamic Strings
 * ----------------------------------------------------------------- */
static const char *js_alloc_string(js_vm_t *vm, const char *src, size_t len) {
    if (!src) return "";
    if (len == (size_t)-1) len = js_strlen(src);
    if (len > JS_MAX_STRING_LEN) len = JS_MAX_STRING_LEN;

    size_t needed = (len + 1 + 7) & ~7UL;
    if (vm->mem_used + needed > vm->mem_budget) {
        vm->last_error = JS_ERR_OUT_OF_MEMORY;
        js_strcpy(vm->error_msg, "Sandboxed memory budget exhausted", sizeof(vm->error_msg));
        return NULL;
    }

    char *dst = &vm->mem_pool[vm->mem_used];
    for (size_t i = 0; i < len; i++) dst[i] = src[i];
    dst[len] = '\0';
    vm->mem_used += needed;
    return dst;
}

static const char *js_concat_strings(js_vm_t *vm, const char *s1, const char *s2) {
    size_t l1 = js_strlen(s1);
    size_t l2 = js_strlen(s2);
    size_t total = l1 + l2;
    if (total > JS_MAX_STRING_LEN) total = JS_MAX_STRING_LEN;

    size_t needed = (total + 1 + 7) & ~7UL;
    if (vm->mem_used + needed > vm->mem_budget) {
        vm->last_error = JS_ERR_OUT_OF_MEMORY;
        js_strcpy(vm->error_msg, "Sandboxed memory budget exhausted", sizeof(vm->error_msg));
        return NULL;
    }

    char *dst = &vm->mem_pool[vm->mem_used];
    size_t i = 0;
    for (; i < l1 && i < total; i++) dst[i] = s1[i];
    for (size_t k = 0; k < l2 && i < total; k++, i++) dst[i] = s2[k];
    dst[i] = '\0';
    vm->mem_used += needed;
    return dst;
}

/* -----------------------------------------------------------------
 * Tokenizer & Lexer
 * ----------------------------------------------------------------- */
typedef enum {
    TOK_EOF = 0,
    TOK_NUM,
    TOK_STR,
    TOK_IDENT,
    TOK_VAR,
    TOK_LET,
    TOK_FUNC,
    TOK_RETURN,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NULL,
    TOK_UNDEFINED,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_EQ,
    TOK_EQEQ,
    TOK_NEQ,
    TOK_LT,
    TOK_LTE,
    TOK_GT,
    TOK_GTE,
    TOK_AND,
    TOK_OR,
    TOK_NOT,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_COMMA,
    TOK_SEMI,
    TOK_DOT
} js_token_type_t;

typedef struct {
    js_token_type_t type;
    s64 num_val;
    const char *str_ptr;
    char ident_name[JS_MAX_NAME_LEN];
    const char *ptr;
} js_token_t;

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    js_token_t cur;
} js_lexer_t;

static js_error_t js_next_token(js_vm_t *vm, js_lexer_t *lex);

static void js_skip_whitespace_and_comments(js_lexer_t *lex) {
    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];
        if (js_isspace(c)) {
            lex->pos++;
            continue;
        }
        if (c == '/' && lex->pos + 1 < lex->len) {
            if (lex->src[lex->pos + 1] == '/') {
                lex->pos += 2;
                while (lex->pos < lex->len && lex->src[lex->pos] != '\n') lex->pos++;
                continue;
            } else if (lex->src[lex->pos + 1] == '*') {
                lex->pos += 2;
                while (lex->pos + 1 < lex->len && !(lex->src[lex->pos] == '*' && lex->src[lex->pos + 1] == '/')) {
                    lex->pos++;
                }
                if (lex->pos + 1 < lex->len) lex->pos += 2;
                continue;
            }
        }
        break;
    }
}

static js_error_t js_next_token(js_vm_t *vm, js_lexer_t *lex) {
    if (vm->instruction_count++ >= vm->max_instructions) {
        vm->last_error = JS_ERR_TIMEOUT;
        js_strcpy(vm->error_msg, "Execution budget exceeded (instruction limit reached)", sizeof(vm->error_msg));
        return JS_ERR_TIMEOUT;
    }

    js_skip_whitespace_and_comments(lex);

    if (lex->pos >= lex->len) {
        lex->cur.type = TOK_EOF;
        lex->cur.ident_name[0] = '\0';
        lex->cur.str_ptr = "";
        return JS_OK;
    }

    const char *start_ptr = &lex->src[lex->pos];
    lex->cur.ptr = start_ptr;
    char c = lex->src[lex->pos];

    /* 1. Numbers */
    if (js_isdigit(c)) {
        s64 n = 0;
        if (c == '0' && lex->pos + 1 < lex->len && (lex->src[lex->pos + 1] == 'x' || lex->src[lex->pos + 1] == 'X')) {
            lex->pos += 2;
            while (lex->pos < lex->len) {
                char hc = lex->src[lex->pos];
                if (hc >= '0' && hc <= '9') n = (n << 4) | (hc - '0');
                else if (hc >= 'a' && hc <= 'f') n = (n << 4) | (10 + hc - 'a');
                else if (hc >= 'A' && hc <= 'F') n = (n << 4) | (10 + hc - 'A');
                else break;
                lex->pos++;
            }
        } else {
            while (lex->pos < lex->len && js_isdigit(lex->src[lex->pos])) {
                n = n * 10 + (lex->src[lex->pos] - '0');
                lex->pos++;
            }
        }
        lex->cur.type = TOK_NUM;
        lex->cur.num_val = n;
        return JS_OK;
    }

    /* 2. Strings */
    if (c == '"' || c == '\'') {
        char quote = c;
        lex->pos++;
        const char *s_start = &lex->src[lex->pos];
        bool closed = false;

        while (lex->pos < lex->len) {
            char sc = lex->src[lex->pos++];
            if (sc == quote) {
                closed = true;
                break;
            }
            if (sc == '\\' && lex->pos < lex->len) {
                lex->pos++;
            }
        }

        if (!closed) {
            vm->last_error = JS_ERR_SYNTAX;
            js_strcpy(vm->error_msg, "Unterminated string literal", sizeof(vm->error_msg));
            return JS_ERR_SYNTAX;
        }

        size_t raw_len = (size_t)((&lex->src[lex->pos - 1]) - s_start);
        /* Allocate string into sandboxed memory pool */
        const char *allocated = js_alloc_string(vm, s_start, raw_len);
        if (!allocated) return JS_ERR_OUT_OF_MEMORY;

        lex->cur.type = TOK_STR;
        lex->cur.str_ptr = allocated;
        return JS_OK;
    }

    /* 3. Identifiers & Keywords */
    if (js_isalpha(c)) {
        size_t id_len = 0;
        while (lex->pos < lex->len && js_isalnum(lex->src[lex->pos])) {
            if (id_len < sizeof(lex->cur.ident_name) - 1) {
                lex->cur.ident_name[id_len++] = lex->src[lex->pos];
            }
            lex->pos++;
        }
        lex->cur.ident_name[id_len] = '\0';

        if (js_streq(lex->cur.ident_name, "var")) lex->cur.type = TOK_VAR;
        else if (js_streq(lex->cur.ident_name, "let")) lex->cur.type = TOK_LET;
        else if (js_streq(lex->cur.ident_name, "function")) lex->cur.type = TOK_FUNC;
        else if (js_streq(lex->cur.ident_name, "return")) lex->cur.type = TOK_RETURN;
        else if (js_streq(lex->cur.ident_name, "if")) lex->cur.type = TOK_IF;
        else if (js_streq(lex->cur.ident_name, "else")) lex->cur.type = TOK_ELSE;
        else if (js_streq(lex->cur.ident_name, "while")) lex->cur.type = TOK_WHILE;
        else if (js_streq(lex->cur.ident_name, "true")) lex->cur.type = TOK_TRUE;
        else if (js_streq(lex->cur.ident_name, "false")) lex->cur.type = TOK_FALSE;
        else if (js_streq(lex->cur.ident_name, "null")) lex->cur.type = TOK_NULL;
        else if (js_streq(lex->cur.ident_name, "undefined")) lex->cur.type = TOK_UNDEFINED;
        else lex->cur.type = TOK_IDENT;

        return JS_OK;
    }

    /* 4. Operators & Punctuation */
    lex->pos++;
    if (c == '=') {
        if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
            lex->pos++;
            lex->cur.type = TOK_EQEQ;
        } else {
            lex->cur.type = TOK_EQ;
        }
    } else if (c == '!') {
        if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
            lex->pos++;
            lex->cur.type = TOK_NEQ;
        } else {
            lex->cur.type = TOK_NOT;
        }
    } else if (c == '<') {
        if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
            lex->pos++;
            lex->cur.type = TOK_LTE;
        } else {
            lex->cur.type = TOK_LT;
        }
    } else if (c == '>') {
        if (lex->pos < lex->len && lex->src[lex->pos] == '=') {
            lex->pos++;
            lex->cur.type = TOK_GTE;
        } else {
            lex->cur.type = TOK_GT;
        }
    } else if (c == '&' && lex->pos < lex->len && lex->src[lex->pos] == '&') {
        lex->pos++;
        lex->cur.type = TOK_AND;
    } else if (c == '|' && lex->pos < lex->len && lex->src[lex->pos] == '|') {
        lex->pos++;
        lex->cur.type = TOK_OR;
    } else if (c == '+') lex->cur.type = TOK_PLUS;
    else if (c == '-') lex->cur.type = TOK_MINUS;
    else if (c == '*') lex->cur.type = TOK_STAR;
    else if (c == '/') lex->cur.type = TOK_SLASH;
    else if (c == '%') lex->cur.type = TOK_PERCENT;
    else if (c == '(') lex->cur.type = TOK_LPAREN;
    else if (c == ')') lex->cur.type = TOK_RPAREN;
    else if (c == '{') lex->cur.type = TOK_LBRACE;
    else if (c == '}') lex->cur.type = TOK_RBRACE;
    else if (c == ',') lex->cur.type = TOK_COMMA;
    else if (c == ';') lex->cur.type = TOK_SEMI;
    else if (c == '.') lex->cur.type = TOK_DOT;
    else {
        vm->last_error = JS_ERR_SYNTAX;
        js_strcpy(vm->error_msg, "Unexpected character", sizeof(vm->error_msg));
        return JS_ERR_SYNTAX;
    }

    return JS_OK;
}

/* -----------------------------------------------------------------
 * Variable Management & Memory Checks
 * ----------------------------------------------------------------- */
static js_var_t *js_find_var(js_vm_t *vm, const char *name) {
    for (int i = vm->var_count - 1; i >= 0; i--) {
        if (js_streq(vm->vars[i].name, name)) {
            return &vm->vars[i];
        }
    }
    return NULL;
}

static js_error_t js_set_var(js_vm_t *vm, const char *name, const js_value_t *val) {
    js_var_t *v = js_find_var(vm, name);
    if (!v) {
        if (vm->var_count >= JS_MAX_VARS) {
            vm->last_error = JS_ERR_OUT_OF_MEMORY;
            js_strcpy(vm->error_msg, "Variable limit exceeded", sizeof(vm->error_msg));
            return JS_ERR_OUT_OF_MEMORY;
        }
        v = &vm->vars[vm->var_count++];
        js_strcpy(v->name, name, sizeof(v->name));
    }
    v->val = *val;
    return JS_OK;
}

static js_error_t js_add_local_var(js_vm_t *vm, const char *name, const js_value_t *val) {
    if (vm->var_count >= JS_MAX_VARS) {
        vm->last_error = JS_ERR_OUT_OF_MEMORY;
        js_strcpy(vm->error_msg, "Variable limit exceeded", sizeof(vm->error_msg));
        return JS_ERR_OUT_OF_MEMORY;
    }
    js_var_t *v = &vm->vars[vm->var_count++];
    js_strcpy(v->name, name, sizeof(v->name));
    v->val = *val;
    return JS_OK;
}

/* -----------------------------------------------------------------
 * Expression Parser Prototypes (with do_eval support for short-circuit)
 * ----------------------------------------------------------------- */
static js_error_t js_parse_expr_ex(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool do_eval);
static js_error_t js_parse_stmt_ex(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool *returned, bool do_eval);
static js_error_t js_parse_block_ex(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool *returned, bool do_eval);

static const char *js_val_to_string(js_vm_t *vm, const js_value_t *v) {
    if (!v) return "undefined";
    switch (v->type) {
        case JS_VAL_UNDEFINED: return "undefined";
        case JS_VAL_NULL: return "null";
        case JS_VAL_BOOL: return v->u.boolean ? "true" : "false";
        case JS_VAL_NUMBER: {
            char num_buf[32];
            js_s64_to_str(v->u.number, num_buf, sizeof(num_buf));
            return js_alloc_string(vm, num_buf, (size_t)-1);
        }
        case JS_VAL_STRING: return v->u.string ? v->u.string : "";
        case JS_VAL_FUNCTION: return "[function]";
        case JS_VAL_OBJECT: return "[object]";
        case JS_VAL_DOM_ELEMENT: return "[DOMElement]";
    }
    return "undefined";
}

static bool js_val_is_truthy(const js_value_t *v) {
    if (!v) return false;
    switch (v->type) {
        case JS_VAL_UNDEFINED:
        case JS_VAL_NULL:
            return false;
        case JS_VAL_BOOL:
            return v->u.boolean;
        case JS_VAL_NUMBER:
            return v->u.number != 0;
        case JS_VAL_STRING:
            return v->u.string && v->u.string[0] != '\0';
        default:
            return true;
    }
}

/* -----------------------------------------------------------------
 * Expression Parsing (Recursive Descent Precedence Climbing with do_eval)
 * ----------------------------------------------------------------- */
static js_error_t js_parse_primary(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool do_eval) {
    js_error_t err;
    if (lex->cur.type == TOK_NUM) {
        if (do_eval) {
            out_val->type = JS_VAL_NUMBER;
            out_val->u.number = lex->cur.num_val;
        }
        return js_next_token(vm, lex);
    }
    if (lex->cur.type == TOK_STR) {
        if (do_eval) {
            out_val->type = JS_VAL_STRING;
            out_val->u.string = lex->cur.str_ptr;
        }
        return js_next_token(vm, lex);
    }
    if (lex->cur.type == TOK_TRUE) {
        if (do_eval) {
            out_val->type = JS_VAL_BOOL;
            out_val->u.boolean = true;
        }
        return js_next_token(vm, lex);
    }
    if (lex->cur.type == TOK_FALSE) {
        if (do_eval) {
            out_val->type = JS_VAL_BOOL;
            out_val->u.boolean = false;
        }
        return js_next_token(vm, lex);
    }
    if (lex->cur.type == TOK_NULL) {
        if (do_eval) {
            out_val->type = JS_VAL_NULL;
        }
        return js_next_token(vm, lex);
    }
    if (lex->cur.type == TOK_UNDEFINED) {
        if (do_eval) {
            out_val->type = JS_VAL_UNDEFINED;
        }
        return js_next_token(vm, lex);
    }
    if (lex->cur.type == TOK_LPAREN) {
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;
        err = js_parse_expr_ex(vm, lex, out_val, do_eval);
        if (err != JS_OK) return err;
        if (lex->cur.type != TOK_RPAREN) {
            vm->last_error = JS_ERR_SYNTAX;
            js_strcpy(vm->error_msg, "Missing closing parenthesis", sizeof(vm->error_msg));
            return JS_ERR_SYNTAX;
        }
        return js_next_token(vm, lex);
    }

    if (lex->cur.type == TOK_IDENT) {
        char ident[JS_MAX_NAME_LEN];
        js_strcpy(ident, lex->cur.ident_name, sizeof(ident));
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        /* Check for blocked APIs */
        if (js_streq(ident, "eval") || js_streq(ident, "window") || js_streq(ident, "fetch") ||
            js_streq(ident, "XMLHttpRequest") || js_streq(ident, "Function")) {
            vm->last_error = JS_ERR_UNSUPPORTED_API;
            js_strcpy(vm->error_msg, "Security exception: Forbidden host API access", sizeof(vm->error_msg));
            return JS_ERR_UNSUPPORTED_API;
        }

        /* Host Object: document */
        if (js_streq(ident, "document")) {
            if (do_eval) {
                out_val->type = JS_VAL_OBJECT;
                out_val->u.host_object_id = JS_HOST_DOCUMENT;
            }
            return JS_OK;
        }

        /* Host Object: console */
        if (js_streq(ident, "console")) {
            if (do_eval) {
                out_val->type = JS_VAL_OBJECT;
                out_val->u.host_object_id = JS_HOST_CONSOLE;
            }
            return JS_OK;
        }

        /* Check for Function Call */
        if (lex->cur.type == TOK_LPAREN) {
            int fidx = -1;
            if (do_eval) {
                for (int f = 0; f < vm->func_count; f++) {
                    if (js_streq(vm->funcs[f].name, ident)) {
                        fidx = f;
                        break;
                    }
                }
                if (fidx < 0) {
                    vm->last_error = JS_ERR_UNDEFINED_VAR;
                    js_strcpy(vm->error_msg, "Call to undefined function", sizeof(vm->error_msg));
                    return JS_ERR_UNDEFINED_VAR;
                }
            }

            err = js_next_token(vm, lex); /* skip '(' */
            if (err != JS_OK) return err;

            js_value_t args[JS_MAX_PARAMS];
            int arg_count = 0;
            while (lex->cur.type != TOK_RPAREN && lex->cur.type != TOK_EOF) {
                if (arg_count < JS_MAX_PARAMS) {
                    err = js_parse_expr_ex(vm, lex, &args[arg_count++], do_eval);
                    if (err != JS_OK) return err;
                }
                if (lex->cur.type == TOK_COMMA) {
                    err = js_next_token(vm, lex);
                    if (err != JS_OK) return err;
                } else break;
            }
            if (lex->cur.type != TOK_RPAREN) {
                vm->last_error = JS_ERR_SYNTAX;
                js_strcpy(vm->error_msg, "Expected ')' after function arguments", sizeof(vm->error_msg));
                return JS_ERR_SYNTAX;
            }
            err = js_next_token(vm, lex); /* skip ')' */
            if (err != JS_OK) return err;

            if (!do_eval) return JS_OK;

            /* Check recursion / call stack depth */
            if (vm->call_depth >= vm->max_call_depth) {
                vm->last_error = JS_ERR_STACK_OVERFLOW;
                js_strcpy(vm->error_msg, "Call stack overflow (recursion depth exceeded)", sizeof(vm->error_msg));
                return JS_ERR_STACK_OVERFLOW;
            }

            vm->call_depth++;
            int saved_vars = vm->var_count;

            /* Bind parameters using local scope variables so outer scopes are shadowed, not mutated */
            js_func_t *fn = &vm->funcs[fidx];
            for (int p = 0; p < fn->param_count; p++) {
                js_value_t pval;
                if (p < arg_count) pval = args[p];
                else {
                    pval.type = JS_VAL_UNDEFINED;
                }
                err = js_add_local_var(vm, fn->params[p], &pval);
                if (err != JS_OK) {
                    vm->var_count = saved_vars;
                    vm->call_depth--;
                    return err;
                }
            }

            /* Execute function body */
            js_lexer_t fn_lex;
            fn_lex.src = fn->body_start;
            fn_lex.len = (size_t)(fn->body_end - fn->body_start);
            fn_lex.pos = 0;
            err = js_next_token(vm, &fn_lex);

            js_value_t ret_val;
            ret_val.type = JS_VAL_UNDEFINED;
            bool returned = false;

            while (err == JS_OK && fn_lex.cur.type != TOK_EOF && !returned) {
                err = js_parse_stmt_ex(vm, &fn_lex, &ret_val, &returned, true);
            }

            vm->var_count = saved_vars;
            vm->call_depth--;

            if (err != JS_OK) return err;
            *out_val = ret_val;
            return JS_OK;
        }

        /* Variable lookup or Function Reference */
        if (do_eval) {
            js_var_t *v = js_find_var(vm, ident);
            if (v) {
                *out_val = v->val;
            } else {
                int fidx = -1;
                for (int f = 0; f < vm->func_count; f++) {
                    if (js_streq(vm->funcs[f].name, ident)) {
                        fidx = f;
                        break;
                    }
                }
                if (fidx >= 0) {
                    out_val->type = JS_VAL_FUNCTION;
                    out_val->u.func_index = fidx;
                } else {
                    out_val->type = JS_VAL_UNDEFINED;
                }
            }
        }
        return JS_OK;
    }


    vm->last_error = JS_ERR_SYNTAX;
    js_strcpy(vm->error_msg, "Syntax error in expression", sizeof(vm->error_msg));
    return JS_ERR_SYNTAX;
}

static js_error_t js_parse_postfix(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool do_eval) {
    js_error_t err = js_parse_primary(vm, lex, out_val, do_eval);
    if (err != JS_OK) return err;

    while (lex->cur.type == TOK_DOT) {
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;
        if (lex->cur.type != TOK_IDENT) {
            vm->last_error = JS_ERR_SYNTAX;
            js_strcpy(vm->error_msg, "Expected identifier after '.'", sizeof(vm->error_msg));
            return JS_ERR_SYNTAX;
        }

        char prop[JS_MAX_NAME_LEN];
        js_strcpy(prop, lex->cur.ident_name, sizeof(prop));
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        if (!do_eval) {
            if (lex->cur.type == TOK_LPAREN) {
                err = js_next_token(vm, lex);
                while (err == JS_OK && lex->cur.type != TOK_RPAREN && lex->cur.type != TOK_EOF) {
                    js_value_t dummy_arg;
                    err = js_parse_expr_ex(vm, lex, &dummy_arg, false);
                    if (err != JS_OK) return err;
                    if (lex->cur.type == TOK_COMMA) err = js_next_token(vm, lex);
                    else break;
                }
                if (lex->cur.type == TOK_RPAREN) err = js_next_token(vm, lex);
                if (err != JS_OK) return err;
            } else if (lex->cur.type == TOK_EQ) {
                err = js_next_token(vm, lex);
                if (err != JS_OK) return err;
                js_value_t dummy_rhs;
                err = js_parse_expr_ex(vm, lex, &dummy_rhs, false);
                if (err != JS_OK) return err;
            }
            continue;
        }

        /* Property on null / undefined throws TypeError */
        if (out_val->type == JS_VAL_NULL || out_val->type == JS_VAL_UNDEFINED) {
            vm->last_error = JS_ERR_TYPE;
            js_strcpy(vm->error_msg, "TypeError: Cannot access property on null or undefined", sizeof(vm->error_msg));
            return JS_ERR_TYPE;
        }

        /* document.getElementById("id") */
        if (out_val->type == JS_VAL_OBJECT && out_val->u.host_object_id == JS_HOST_DOCUMENT) {
            if (js_streq(prop, "getElementById")) {
                if (lex->cur.type != TOK_LPAREN) {
                    vm->last_error = JS_ERR_SYNTAX;
                    js_strcpy(vm->error_msg, "Expected '(' after getElementById", sizeof(vm->error_msg));
                    return JS_ERR_SYNTAX;
                }
                err = js_next_token(vm, lex);
                if (err != JS_OK) return err;

                js_value_t arg;
                err = js_parse_expr_ex(vm, lex, &arg, do_eval);
                if (err != JS_OK) return err;

                if (lex->cur.type != TOK_RPAREN) {
                    vm->last_error = JS_ERR_SYNTAX;
                    js_strcpy(vm->error_msg, "Expected ')' after getElementById parameter", sizeof(vm->error_msg));
                    return JS_ERR_SYNTAX;
                }
                err = js_next_token(vm, lex);
                if (err != JS_OK) return err;

                const char *id_str = js_val_to_string(vm, &arg);
                int node_idx = -1;
                u32 doc_gen = 0;
                if (vm->dom_get_element_by_id && id_str) {
                    node_idx = vm->dom_get_element_by_id(vm->dom_context, id_str, &doc_gen);
                }
                if (node_idx >= 0) {
                    out_val->type = JS_VAL_DOM_ELEMENT;
                    out_val->u.dom_handle.node_index = node_idx;
                    out_val->u.dom_handle.doc_generation = doc_gen;
                } else {
                    out_val->type = JS_VAL_NULL;
                    out_val->u.dom_handle.node_index = -1;
                    out_val->u.dom_handle.doc_generation = 0;
                }
                continue;
            }
        }

        /* console.log(...) */
        if (out_val->type == JS_VAL_OBJECT && out_val->u.host_object_id == JS_HOST_CONSOLE) {
            if (js_streq(prop, "log")) {
                if (lex->cur.type != TOK_LPAREN) return JS_ERR_SYNTAX;
                err = js_next_token(vm, lex);
                if (err != JS_OK) return err;

                while (lex->cur.type != TOK_RPAREN && lex->cur.type != TOK_EOF) {
                    js_value_t larg;
                    err = js_parse_expr_ex(vm, lex, &larg, do_eval);
                    if (err != JS_OK) return err;
                    const char *s = js_val_to_string(vm, &larg);
                    if (s) sys_write(1, s, js_strlen(s));
                    if (lex->cur.type == TOK_COMMA) {
                        sys_write(1, " ", 1);
                        err = js_next_token(vm, lex);
                        if (err != JS_OK) return err;
                    } else break;
                }
                if (lex->cur.type == TOK_RPAREN) {
                    err = js_next_token(vm, lex);
                    if (err != JS_OK) return err;
                }
                sys_write(1, "\n", 1);
                out_val->type = JS_VAL_UNDEFINED;
                continue;
            }
        }

        /* DOM Element property: elem.textContent [= expr] */
        if (out_val->type == JS_VAL_DOM_ELEMENT) {
            if (js_streq(prop, "textContent")) {
                if (lex->cur.type == TOK_EQ) {
                    err = js_next_token(vm, lex);
                    if (err != JS_OK) return err;
                    js_value_t rhs;
                    err = js_parse_expr_ex(vm, lex, &rhs, do_eval);
                    if (err != JS_OK) return err;

                    const char *str_val = js_val_to_string(vm, &rhs);
                    if (vm->dom_set_text_content && str_val) {
                        bool set_ok = vm->dom_set_text_content(vm->dom_context,
                                                               out_val->u.dom_handle.doc_generation,
                                                               out_val->u.dom_handle.node_index,
                                                               str_val);
                        if (!set_ok) {
                            vm->last_error = JS_ERR_INVALID_HANDLE;
                            js_strcpy(vm->error_msg, "DOMError: Stale or invalid DOM element handle", sizeof(vm->error_msg));
                            return JS_ERR_INVALID_HANDLE;
                        }
                    }
                    *out_val = rhs;
                    continue;
                } else {
                    /* Read textContent */
                    const char *cur_text = NULL;
                    if (vm->dom_get_text_content) {
                        cur_text = vm->dom_get_text_content(vm->dom_context,
                                                             out_val->u.dom_handle.doc_generation,
                                                             out_val->u.dom_handle.node_index);
                    }
                    if (!cur_text) {
                        vm->last_error = JS_ERR_INVALID_HANDLE;
                        js_strcpy(vm->error_msg, "DOMError: Stale or invalid DOM element handle", sizeof(vm->error_msg));
                        return JS_ERR_INVALID_HANDLE;
                    }
                    out_val->type = JS_VAL_STRING;
                    out_val->u.string = cur_text;
                    continue;
                }
            }

            if (js_streq(prop, "onclick")) {
                if (lex->cur.type == TOK_EQ) {
                    err = js_next_token(vm, lex);
                    if (err != JS_OK) return err;
                    js_value_t rhs;
                    err = js_parse_expr_ex(vm, lex, &rhs, do_eval);
                    if (err != JS_OK) return err;
                    if (do_eval) {
                        const char *code_str = NULL;
                        char func_buf[JS_MAX_NAME_LEN + 4];
                        if (rhs.type == JS_VAL_STRING) {
                            code_str = rhs.u.string;
                        } else if (rhs.type == JS_VAL_FUNCTION) {
                            if (rhs.u.func_index >= 0 && rhs.u.func_index < vm->func_count) {
                                js_strcpy(func_buf, vm->funcs[rhs.u.func_index].name, sizeof(func_buf));
                                js_strcat(func_buf, "()", sizeof(func_buf));
                                code_str = func_buf;
                            }
                        } else if (rhs.type == JS_VAL_NULL || rhs.type == JS_VAL_UNDEFINED) {
                            code_str = "";
                        } else {
                            vm->last_error = JS_ERR_TYPE;
                            js_strcpy(vm->error_msg, "TypeError: onclick must be a function, string, or null", sizeof(vm->error_msg));
                            return JS_ERR_TYPE;
                        }
                        if (vm->dom_set_onclick) {
                            bool set_ok = vm->dom_set_onclick(vm->dom_context,
                                                               out_val->u.dom_handle.doc_generation,
                                                               out_val->u.dom_handle.node_index,
                                                               code_str ? code_str : "");
                            if (!set_ok) {
                                vm->last_error = JS_ERR_INVALID_HANDLE;
                                js_strcpy(vm->error_msg, "DOMError: Stale or invalid DOM element handle", sizeof(vm->error_msg));
                                return JS_ERR_INVALID_HANDLE;
                            }
                        }
                    }
                    *out_val = rhs;
                    continue;
                }
            }
        }


        /* Unknown property access on an object */
        out_val->type = JS_VAL_UNDEFINED;
    }
    return JS_OK;
}

static js_error_t js_parse_unary(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool do_eval) {
    if (lex->cur.type == TOK_NOT) {
        js_error_t err = js_next_token(vm, lex);
        if (err != JS_OK) return err;
        err = js_parse_unary(vm, lex, out_val, do_eval);
        if (err != JS_OK) return err;
        if (do_eval) {
            bool truthy = js_val_is_truthy(out_val);
            out_val->type = JS_VAL_BOOL;
            out_val->u.boolean = !truthy;
        }
        return JS_OK;
    }
    if (lex->cur.type == TOK_MINUS) {
        js_error_t err = js_next_token(vm, lex);
        if (err != JS_OK) return err;
        err = js_parse_unary(vm, lex, out_val, do_eval);
        if (err != JS_OK) return err;
        if (do_eval) {
            if (out_val->type == JS_VAL_NUMBER) {
                if (out_val->u.number == LLONG_MIN_VAL) {
                    vm->last_error = JS_ERR_OVERFLOW;
                    js_strcpy(vm->error_msg, "Arithmetic error: integer overflow in negation", sizeof(vm->error_msg));
                    return JS_ERR_OVERFLOW;
                }
                out_val->u.number = -out_val->u.number;
            } else {
                out_val->type = JS_VAL_NUMBER;
                out_val->u.number = 0;
            }
        }
        return JS_OK;
    }
    if (lex->cur.type == TOK_PLUS) {
        js_error_t err = js_next_token(vm, lex);
        if (err != JS_OK) return err;
        return js_parse_unary(vm, lex, out_val, do_eval);
    }
    return js_parse_postfix(vm, lex, out_val, do_eval);
}

static js_error_t js_parse_multiplicative(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool do_eval) {
    js_error_t err = js_parse_unary(vm, lex, out_val, do_eval);
    if (err != JS_OK) return err;

    while (lex->cur.type == TOK_STAR || lex->cur.type == TOK_SLASH || lex->cur.type == TOK_PERCENT) {
        js_token_type_t op = lex->cur.type;
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        js_value_t rhs;
        err = js_parse_unary(vm, lex, &rhs, do_eval);
        if (err != JS_OK) return err;

        if (do_eval) {
            s64 lhs_num = (out_val->type == JS_VAL_NUMBER) ? out_val->u.number : 0;
            s64 rhs_num = (rhs.type == JS_VAL_NUMBER) ? rhs.u.number : 0;

            out_val->type = JS_VAL_NUMBER;
            if (op == TOK_STAR) {
                s64 prod = 0;
                if (__builtin_mul_overflow(lhs_num, rhs_num, &prod)) {
                    vm->last_error = JS_ERR_OVERFLOW;
                    js_strcpy(vm->error_msg, "Arithmetic error: integer overflow in multiplication", sizeof(vm->error_msg));
                    return JS_ERR_OVERFLOW;
                }
                out_val->u.number = prod;
            } else if (op == TOK_SLASH) {
                if (rhs_num == 0) {
                    vm->last_error = JS_ERR_DIV_BY_ZERO;
                    js_strcpy(vm->error_msg, "Division by zero", sizeof(vm->error_msg));
                    return JS_ERR_DIV_BY_ZERO;
                }
                if (lhs_num == LLONG_MIN_VAL && rhs_num == -1) {
                    vm->last_error = JS_ERR_OVERFLOW;
                    js_strcpy(vm->error_msg, "Arithmetic error: integer overflow in division", sizeof(vm->error_msg));
                    return JS_ERR_OVERFLOW;
                }
                out_val->u.number = lhs_num / rhs_num;
            } else if (op == TOK_PERCENT) {
                if (rhs_num == 0) {
                    vm->last_error = JS_ERR_DIV_BY_ZERO;
                    js_strcpy(vm->error_msg, "Modulo by zero", sizeof(vm->error_msg));
                    return JS_ERR_DIV_BY_ZERO;
                }
                if (lhs_num == LLONG_MIN_VAL && rhs_num == -1) {
                    out_val->u.number = 0;
                } else {
                    out_val->u.number = lhs_num % rhs_num;
                }
            }
        }
    }
    return JS_OK;
}

static js_error_t js_parse_additive(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool do_eval) {
    js_error_t err = js_parse_multiplicative(vm, lex, out_val, do_eval);
    if (err != JS_OK) return err;

    while (lex->cur.type == TOK_PLUS || lex->cur.type == TOK_MINUS) {
        js_token_type_t op = lex->cur.type;
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        js_value_t rhs;
        err = js_parse_multiplicative(vm, lex, &rhs, do_eval);
        if (err != JS_OK) return err;

        if (do_eval) {
            if (op == TOK_PLUS && (out_val->type == JS_VAL_STRING || rhs.type == JS_VAL_STRING)) {
                /* String Concatenation */
                const char *s1 = js_val_to_string(vm, out_val);
                const char *s2 = js_val_to_string(vm, &rhs);
                const char *concat = js_concat_strings(vm, s1, s2);
                if (!concat) return JS_ERR_OUT_OF_MEMORY;

                out_val->type = JS_VAL_STRING;
                out_val->u.string = concat;
            } else {
                s64 lhs_num = (out_val->type == JS_VAL_NUMBER) ? out_val->u.number : 0;
                s64 rhs_num = (rhs.type == JS_VAL_NUMBER) ? rhs.u.number : 0;
                out_val->type = JS_VAL_NUMBER;
                if (op == TOK_PLUS) {
                    s64 sum = 0;
                    if (__builtin_add_overflow(lhs_num, rhs_num, &sum)) {
                        vm->last_error = JS_ERR_OVERFLOW;
                        js_strcpy(vm->error_msg, "Arithmetic error: integer overflow in addition", sizeof(vm->error_msg));
                        return JS_ERR_OVERFLOW;
                    }
                    out_val->u.number = sum;
                } else {
                    s64 diff = 0;
                    if (__builtin_sub_overflow(lhs_num, rhs_num, &diff)) {
                        vm->last_error = JS_ERR_OVERFLOW;
                        js_strcpy(vm->error_msg, "Arithmetic error: integer overflow in subtraction", sizeof(vm->error_msg));
                        return JS_ERR_OVERFLOW;
                    }
                    out_val->u.number = diff;
                }
            }
        }
    }
    return JS_OK;
}

static js_error_t js_parse_relational(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool do_eval) {
    js_error_t err = js_parse_additive(vm, lex, out_val, do_eval);
    if (err != JS_OK) return err;

    while (lex->cur.type == TOK_LT || lex->cur.type == TOK_LTE ||
           lex->cur.type == TOK_GT || lex->cur.type == TOK_GTE) {
        js_token_type_t op = lex->cur.type;
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        js_value_t rhs;
        err = js_parse_additive(vm, lex, &rhs, do_eval);
        if (err != JS_OK) return err;

        if (do_eval) {
            s64 a = (out_val->type == JS_VAL_NUMBER) ? out_val->u.number : 0;
            s64 b = (rhs.type == JS_VAL_NUMBER) ? rhs.u.number : 0;

            out_val->type = JS_VAL_BOOL;
            if (op == TOK_LT) out_val->u.boolean = (a < b);
            else if (op == TOK_LTE) out_val->u.boolean = (a <= b);
            else if (op == TOK_GT) out_val->u.boolean = (a > b);
            else if (op == TOK_GTE) out_val->u.boolean = (a >= b);
        }
    }
    return JS_OK;
}

static js_error_t js_parse_equality(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool do_eval) {
    js_error_t err = js_parse_relational(vm, lex, out_val, do_eval);
    if (err != JS_OK) return err;

    while (lex->cur.type == TOK_EQEQ || lex->cur.type == TOK_NEQ) {
        js_token_type_t op = lex->cur.type;
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        js_value_t rhs;
        err = js_parse_relational(vm, lex, &rhs, do_eval);
        if (err != JS_OK) return err;

        if (do_eval) {
            bool equal = false;
            if (out_val->type == rhs.type) {
                if (out_val->type == JS_VAL_NUMBER) equal = (out_val->u.number == rhs.u.number);
                else if (out_val->type == JS_VAL_BOOL) equal = (out_val->u.boolean == rhs.u.boolean);
                else if (out_val->type == JS_VAL_STRING) equal = js_streq(out_val->u.string, rhs.u.string);
                else if (out_val->type == JS_VAL_UNDEFINED || out_val->type == JS_VAL_NULL) equal = true;
                else if (out_val->type == JS_VAL_DOM_ELEMENT) {
                    equal = (out_val->u.dom_handle.node_index == rhs.u.dom_handle.node_index &&
                             out_val->u.dom_handle.doc_generation == rhs.u.dom_handle.doc_generation);
                }
            } else if ((out_val->type == JS_VAL_NULL && rhs.type == JS_VAL_UNDEFINED) ||
                       (out_val->type == JS_VAL_UNDEFINED && rhs.type == JS_VAL_NULL)) {
                equal = true;
            }
            out_val->type = JS_VAL_BOOL;
            out_val->u.boolean = (op == TOK_EQEQ) ? equal : !equal;
        }
    }
    return JS_OK;
}

static js_error_t js_parse_logical_and(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool do_eval) {
    js_error_t err = js_parse_equality(vm, lex, out_val, do_eval);
    if (err != JS_OK) return err;

    while (lex->cur.type == TOK_AND) {
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        bool lhs_truthy = do_eval ? js_val_is_truthy(out_val) : false;
        bool eval_rhs = do_eval && lhs_truthy;

        js_value_t rhs;
        err = js_parse_equality(vm, lex, &rhs, eval_rhs);
        if (err != JS_OK) return err;

        if (do_eval) {
            if (!lhs_truthy) {
                /* Short-circuit */
                out_val->type = JS_VAL_BOOL;
                out_val->u.boolean = false;
            } else {
                bool res = js_val_is_truthy(&rhs);
                out_val->type = JS_VAL_BOOL;
                out_val->u.boolean = res;
            }
        }
    }
    return JS_OK;
}

static js_error_t js_parse_logical_or(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool do_eval) {
    js_error_t err = js_parse_logical_and(vm, lex, out_val, do_eval);
    if (err != JS_OK) return err;

    while (lex->cur.type == TOK_OR) {
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        bool lhs_truthy = do_eval ? js_val_is_truthy(out_val) : false;
        bool eval_rhs = do_eval && !lhs_truthy;

        js_value_t rhs;
        err = js_parse_logical_and(vm, lex, &rhs, eval_rhs);
        if (err != JS_OK) return err;

        if (do_eval) {
            if (lhs_truthy) {
                /* Short-circuit */
                out_val->type = JS_VAL_BOOL;
                out_val->u.boolean = true;
            } else {
                bool res = js_val_is_truthy(&rhs);
                out_val->type = JS_VAL_BOOL;
                out_val->u.boolean = res;
            }
        }
    }
    return JS_OK;
}

static js_error_t js_parse_expr_ex(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool do_eval) {
    /* Check for assignment to an identifier: ident = expr */
    if (lex->cur.type == TOK_IDENT) {
        size_t save_pos = lex->pos;
        js_token_t save_cur = lex->cur;

        char ident[JS_MAX_NAME_LEN];
        js_strcpy(ident, lex->cur.ident_name, sizeof(ident));

        js_next_token(vm, lex);
        if (lex->cur.type == TOK_EQ) {
            js_next_token(vm, lex); /* skip '=' */
            js_error_t err = js_parse_expr_ex(vm, lex, out_val, do_eval);
            if (err != JS_OK) return err;
            if (do_eval) {
                return js_set_var(vm, ident, out_val);
            }
            return JS_OK;
        }

        lex->pos = save_pos;
        lex->cur = save_cur;
    }

    return js_parse_logical_or(vm, lex, out_val, do_eval);
}

static js_error_t __attribute__((unused)) js_parse_expr(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val) {
    return js_parse_expr_ex(vm, lex, out_val, true);
}

/* -----------------------------------------------------------------
 * Statement Parser & Control Flow Execution (with do_eval support)
 * ----------------------------------------------------------------- */
static js_error_t js_parse_block_ex(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool *returned, bool do_eval) {
    js_error_t err = JS_OK;
    if (lex->cur.type == TOK_LBRACE) {
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;
        while (lex->cur.type != TOK_RBRACE && lex->cur.type != TOK_EOF) {
            bool stmt_eval = do_eval && !*returned;
            err = js_parse_stmt_ex(vm, lex, out_val, returned, stmt_eval);
            if (err != JS_OK) return err;
        }
        if (lex->cur.type == TOK_RBRACE) {
            err = js_next_token(vm, lex);
        } else {
            vm->last_error = JS_ERR_SYNTAX;
            js_strcpy(vm->error_msg, "Expected '}' at end of block", sizeof(vm->error_msg));
            return JS_ERR_SYNTAX;
        }
    } else {
        err = js_parse_stmt_ex(vm, lex, out_val, returned, do_eval);
    }
    return err;
}

static js_error_t js_parse_stmt_ex(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool *returned, bool do_eval) {
    js_error_t err = JS_OK;

    if (lex->cur.type == TOK_SEMI) {
        return js_next_token(vm, lex);
    }

    /* 1. Variable Declaration: var x = ...; or let x = ...; */
    if (lex->cur.type == TOK_VAR || lex->cur.type == TOK_LET) {
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        if (lex->cur.type != TOK_IDENT) {
            vm->last_error = JS_ERR_SYNTAX;
            js_strcpy(vm->error_msg, "Expected identifier after var/let", sizeof(vm->error_msg));
            return JS_ERR_SYNTAX;
        }

        char vname[JS_MAX_NAME_LEN];
        js_strcpy(vname, lex->cur.ident_name, sizeof(vname));
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        js_value_t init_val;
        init_val.type = JS_VAL_UNDEFINED;

        if (lex->cur.type == TOK_EQ) {
            err = js_next_token(vm, lex);
            if (err != JS_OK) return err;
            err = js_parse_expr_ex(vm, lex, &init_val, do_eval);
            if (err != JS_OK) return err;
        }

        if (do_eval) {
            err = js_set_var(vm, vname, &init_val);
            if (err != JS_OK) return err;
        }

        if (lex->cur.type == TOK_SEMI) {
            err = js_next_token(vm, lex);
        }
        if (do_eval) *out_val = init_val;
        return err;
    }

    /* 2. Function Declaration: function name(arg1, arg2) { ... } */
    if (lex->cur.type == TOK_FUNC) {
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        if (lex->cur.type != TOK_IDENT) {
            vm->last_error = JS_ERR_SYNTAX;
            js_strcpy(vm->error_msg, "Expected function name", sizeof(vm->error_msg));
            return JS_ERR_SYNTAX;
        }

        int fidx = -1;
        for (int i = 0; i < vm->func_count; i++) {
            if (js_streq(vm->funcs[i].name, lex->cur.ident_name)) {
                fidx = i;
                break;
            }
        }
        if (fidx < 0) {
            if (vm->func_count >= JS_MAX_FUNCTIONS) {
                vm->last_error = JS_ERR_OUT_OF_MEMORY;
                js_strcpy(vm->error_msg, "Function limit exceeded", sizeof(vm->error_msg));
                return JS_ERR_OUT_OF_MEMORY;
            }
            fidx = vm->func_count++;
        }

        js_func_t *fn = &vm->funcs[fidx];
        js_memset(fn, 0, sizeof(*fn));
        js_strcpy(fn->name, lex->cur.ident_name, sizeof(fn->name));

        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        if (lex->cur.type != TOK_LPAREN) {
            vm->last_error = JS_ERR_SYNTAX;
            js_strcpy(vm->error_msg, "Expected '(' after function name", sizeof(vm->error_msg));
            return JS_ERR_SYNTAX;
        }
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        while (lex->cur.type != TOK_RPAREN && lex->cur.type != TOK_EOF) {
            if (lex->cur.type == TOK_IDENT && fn->param_count < JS_MAX_PARAMS) {
                js_strcpy(fn->params[fn->param_count++], lex->cur.ident_name, sizeof(fn->params[0]));
                err = js_next_token(vm, lex);
                if (err != JS_OK) return err;
            }
            if (lex->cur.type == TOK_COMMA) {
                err = js_next_token(vm, lex);
                if (err != JS_OK) return err;
            } else break;
        }

        if (lex->cur.type != TOK_RPAREN) {
            vm->last_error = JS_ERR_SYNTAX;
            js_strcpy(vm->error_msg, "Expected ')' after parameter list", sizeof(vm->error_msg));
            return JS_ERR_SYNTAX;
        }
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        if (lex->cur.type != TOK_LBRACE) {
            vm->last_error = JS_ERR_SYNTAX;
            js_strcpy(vm->error_msg, "Expected '{' before function body", sizeof(vm->error_msg));
            return JS_ERR_SYNTAX;
        }

        /* Capture function body source range */
        err = js_next_token(vm, lex); /* skip '{' */
        if (err != JS_OK) return err;
        fn->body_start = lex->cur.ptr;

        int brace_depth = 1;
        while (lex->cur.type != TOK_EOF && brace_depth > 0) {
            if (lex->cur.type == TOK_LBRACE) brace_depth++;
            else if (lex->cur.type == TOK_RBRACE) {
                brace_depth--;
                if (brace_depth == 0) break;
            }
            err = js_next_token(vm, lex);
            if (err != JS_OK) return err;
        }
        if (brace_depth > 0) {
            vm->last_error = JS_ERR_SYNTAX;
            js_strcpy(vm->error_msg, "Unclosed function body: missing '}'", sizeof(vm->error_msg));
            return JS_ERR_SYNTAX;
        }
        fn->body_end = lex->cur.ptr;
        if (lex->cur.type == TOK_RBRACE) {
            err = js_next_token(vm, lex);
        }
        out_val->type = JS_VAL_FUNCTION;
        return err;
    }

    /* 3. Return Statement: return [expr]; */
    if (lex->cur.type == TOK_RETURN) {
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        if (lex->cur.type != TOK_SEMI && lex->cur.type != TOK_RBRACE && lex->cur.type != TOK_EOF) {
            err = js_parse_expr_ex(vm, lex, out_val, do_eval);
            if (err != JS_OK) return err;
        } else {
            if (do_eval) out_val->type = JS_VAL_UNDEFINED;
        }
        if (lex->cur.type == TOK_SEMI) {
            err = js_next_token(vm, lex);
        }
        if (do_eval) *returned = true;
        return err;
    }

    /* 4. If / Else Statement: if (cond) stmt [else stmt] */
    if (lex->cur.type == TOK_IF) {
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        if (lex->cur.type != TOK_LPAREN) {
            vm->last_error = JS_ERR_SYNTAX;
            js_strcpy(vm->error_msg, "Expected '(' after if", sizeof(vm->error_msg));
            return JS_ERR_SYNTAX;
        }
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        js_value_t cond;
        err = js_parse_expr_ex(vm, lex, &cond, do_eval);
        if (err != JS_OK) return err;

        if (lex->cur.type != TOK_RPAREN) {
            vm->last_error = JS_ERR_SYNTAX;
            js_strcpy(vm->error_msg, "Expected ')' after if condition", sizeof(vm->error_msg));
            return JS_ERR_SYNTAX;
        }
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        bool cond_val = do_eval ? js_val_is_truthy(&cond) : false;
        bool then_eval = do_eval && cond_val;
        bool else_eval = do_eval && !cond_val;

        err = js_parse_block_ex(vm, lex, out_val, returned, then_eval);
        if (err != JS_OK) return err;

        if (lex->cur.type == TOK_ELSE) {
            err = js_next_token(vm, lex);
            if (err != JS_OK) return err;
            err = js_parse_block_ex(vm, lex, out_val, returned, else_eval);
            if (err != JS_OK) return err;
        }
        return JS_OK;
    }

    /* 5. While Loop Statement: while (cond) stmt */
    if (lex->cur.type == TOK_WHILE) {
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;

        if (lex->cur.type != TOK_LPAREN) {
            vm->last_error = JS_ERR_SYNTAX;
            js_strcpy(vm->error_msg, "Expected '(' after while", sizeof(vm->error_msg));
            return JS_ERR_SYNTAX;
        }

        err = js_next_token(vm, lex); /* skip '(' */
        if (err != JS_OK) return err;

        const char *cond_start = lex->cur.ptr;
        size_t cond_pos = (size_t)(cond_start - lex->src);

        js_value_t cond;
        err = js_parse_expr_ex(vm, lex, &cond, do_eval);
        if (err != JS_OK) return err;
        if (lex->cur.type != TOK_RPAREN) return JS_ERR_SYNTAX;

        err = js_next_token(vm, lex); /* skip ')' */
        if (err != JS_OK) return err;

        const char *body_start = lex->cur.ptr;
        size_t body_pos = (size_t)(body_start - lex->src);

        if (!do_eval || !js_val_is_truthy(&cond)) {
            /* Condition is false or skipped: parse body with do_eval = false */
            return js_parse_block_ex(vm, lex, out_val, returned, false);
        }

        /* Loop actively executes */
        while (do_eval && js_val_is_truthy(&cond) && !*returned) {
            if (vm->instruction_count++ >= vm->max_instructions) {
                vm->last_error = JS_ERR_TIMEOUT;
                js_strcpy(vm->error_msg, "Execution budget exceeded (infinite loop prevented)", sizeof(vm->error_msg));
                return JS_ERR_TIMEOUT;
            }

            /* Execute body */
            lex->pos = body_pos;
            err = js_next_token(vm, lex);
            if (err != JS_OK) return err;
            err = js_parse_block_ex(vm, lex, out_val, returned, true);
            if (err != JS_OK) return err;

            /* Re-evaluate condition */
            lex->pos = cond_pos;
            err = js_next_token(vm, lex);
            if (err != JS_OK) return err;
            err = js_parse_expr_ex(vm, lex, &cond, true);
            if (err != JS_OK) return err;
        }

        /* Advance lexer past body */
        lex->pos = body_pos;
        err = js_next_token(vm, lex);
        if (err != JS_OK) return err;
        return js_parse_block_ex(vm, lex, out_val, returned, false);
    }

    /* 6. Block: { ... } */
    if (lex->cur.type == TOK_LBRACE) {
        return js_parse_block_ex(vm, lex, out_val, returned, do_eval);
    }

    /* 7. Expression Statement: expr; */
    err = js_parse_expr_ex(vm, lex, out_val, do_eval);
    if (err != JS_OK) return err;
    if (lex->cur.type == TOK_SEMI) {
        err = js_next_token(vm, lex);
    }
    return err;
}

static js_error_t js_parse_stmt(js_vm_t *vm, js_lexer_t *lex, js_value_t *out_val, bool *returned) {
    return js_parse_stmt_ex(vm, lex, out_val, returned, true);
}

/* -----------------------------------------------------------------
 * VM Lifecycle & Configuration
 * ----------------------------------------------------------------- */
void js_init(js_vm_t *vm) {
    if (!vm) return;
    js_memset(vm, 0, sizeof(*vm));
    vm->max_instructions = JS_DEFAULT_INSTRUCTIONS;
    vm->max_call_depth = JS_DEFAULT_CALL_DEPTH;
    vm->mem_budget = JS_DEFAULT_MEM_BUDGET;
    vm->last_error = JS_OK;
}

void js_set_limits(js_vm_t *vm, u32 max_instructions, int max_call_depth, size_t mem_budget) {
    if (!vm) return;
    if (max_instructions > 0) vm->max_instructions = max_instructions;
    if (max_call_depth > 0) vm->max_call_depth = max_call_depth;
    if (mem_budget > 0) vm->mem_budget = mem_budget;
}

void js_set_dom_bridge(js_vm_t *vm, void *ctx,
                       int (*get_elem)(void*, const char*, u32*),
                       const char *(*get_text)(void*, u32, int),
                       bool (*set_text)(void*, u32, int, const char*)) {
    if (!vm) return;
    vm->dom_context = ctx;
    vm->dom_get_element_by_id = get_elem;
    vm->dom_get_text_content = get_text;
    vm->dom_set_text_content = set_text;
}

void js_set_dom_event_bridge(js_vm_t *vm,
                             bool (*set_onclick)(void*, u32, int, const char*)) {
    if (!vm) return;
    vm->dom_set_onclick = set_onclick;
}

const char *js_error_string(js_error_t err) {
    switch (err) {
        case JS_OK: return "OK";
        case JS_ERR_SYNTAX: return "Syntax Error";
        case JS_ERR_TIMEOUT: return "Execution Timeout (Budget Exhausted)";
        case JS_ERR_STACK_OVERFLOW: return "Call Stack Overflow";
        case JS_ERR_OUT_OF_MEMORY: return "Out of Memory (Budget Exhausted)";
        case JS_ERR_OVERSIZED: return "Script Oversized (>4096 bytes)";
        case JS_ERR_UNSUPPORTED_API: return "Security Exception: Unsupported Host API";
        case JS_ERR_DIV_BY_ZERO: return "Division By Zero";
        case JS_ERR_OVERFLOW: return "Arithmetic Overflow";
        case JS_ERR_TYPE: return "Type Error";
        case JS_ERR_UNDEFINED_VAR: return "Reference Error: Undefined Variable";
        case JS_ERR_INVALID_HANDLE: return "DOM Error: Stale or Invalid Handle";
        default: return "Unknown Error";
    }
}

/* -----------------------------------------------------------------
 * Script Evaluation Entry Point
 * ----------------------------------------------------------------- */
js_error_t js_eval(js_vm_t *vm, const char *source, js_value_t *out_val) {
    if (!vm || !source) return JS_ERR_SYNTAX;

    size_t slen = js_strlen(source);
    if (slen > JS_MAX_SCRIPT_LEN) {
        vm->last_error = JS_ERR_OVERSIZED;
        js_strcpy(vm->error_msg, "Script source exceeds 4096 byte limit", sizeof(vm->error_msg));
        return JS_ERR_OVERSIZED;
    }

    vm->instruction_count = 0;
    vm->call_depth = 0;
    vm->last_error = JS_OK;
    vm->error_msg[0] = '\0';

    js_lexer_t lex;
    lex.src = source;
    lex.len = slen;
    lex.pos = 0;
    js_memset(&lex.cur, 0, sizeof(lex.cur));

    js_error_t err = js_next_token(vm, &lex);
    if (err != JS_OK) return err;

    js_value_t last_val;
    last_val.type = JS_VAL_UNDEFINED;
    bool returned = false;

    while (err == JS_OK && lex.cur.type != TOK_EOF && !returned) {
        err = js_parse_stmt(vm, &lex, &last_val, &returned);
    }

    if (err != JS_OK) {
        /* Reset call depth on abort to ensure clean recovery */
        vm->call_depth = 0;
        return err;
    }
    if (out_val) *out_val = last_val;
    return JS_OK;
}

/* -----------------------------------------------------------------
 * Unit Self-Tests (Phase R Core Suite)
 * ----------------------------------------------------------------- */
int js_run_self_tests(void) {
    static js_vm_t test_vm;
    js_init(&test_vm);
    js_value_t res;
    js_error_t err;

    /* TEST 1: Minimal arithmetic expression */
    err = js_eval(&test_vm, "2 + 3 * 4;", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 14) return 1;

    err = js_eval(&test_vm, "(10 - 4) * (2 + 3);", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 30) return 2;

    /* TEST 2: Variable assignment and basic expressions */
    err = js_eval(&test_vm, "var x = 10; var y = 20; var z = x + y; z;", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 30) return 3;

    err = js_eval(&test_vm, "var s = 'Hello ' + 'World'; s;", &res);
    if (err != JS_OK || res.type != JS_VAL_STRING || !js_streq(res.u.string, "Hello World")) return 4;

    /* TEST 3: Simple function */
    err = js_eval(&test_vm, "function add(a, b) { return a + b; } var sum = add(15, 27); sum;", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 42) return 5;

    err = js_eval(&test_vm, "function square(n) { return n * n; } square(8);", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 64) return 6;

    /* TEST 4: Syntax errors */
    err = js_eval(&test_vm, "var x = ;", &res);
    if (err != JS_ERR_SYNTAX) return 7;

    err = js_eval(&test_vm, "function ( { return; }", &res);
    if (err != JS_ERR_SYNTAX) return 8;

    /* TEST 5: Infinite-loop / execution budget exhaustion */
    js_set_limits(&test_vm, 200, 16, 32768);
    err = js_eval(&test_vm, "while (1) {}", &res);
    if (err != JS_ERR_TIMEOUT) return 9;
    js_set_limits(&test_vm, JS_DEFAULT_INSTRUCTIONS, JS_DEFAULT_CALL_DEPTH, JS_DEFAULT_MEM_BUDGET);

    /* TEST 6: Recursion and stack-limit exhaustion */
    js_set_limits(&test_vm, 10000, 5, 32768);
    err = js_eval(&test_vm, "function recurse() { return recurse(); } recurse();", &res);
    if (err != JS_ERR_STACK_OVERFLOW) return 10;
    js_set_limits(&test_vm, JS_DEFAULT_INSTRUCTIONS, JS_DEFAULT_CALL_DEPTH, JS_DEFAULT_MEM_BUDGET);

    /* TEST 7: Memory-budget / variable exhaustion */
    test_vm.mem_budget = 32;
    err = js_eval(&test_vm, "var long_str = 'This is a long string exceeding the 32 byte budget';", &res);
    if (err != JS_ERR_OUT_OF_MEMORY) return 11;
    test_vm.mem_budget = JS_DEFAULT_MEM_BUDGET;

    /* TEST 8: Malformed or oversized script input */
    err = js_eval(&test_vm, "var bad = \"unterminated string literal", &res);
    if (err != JS_ERR_SYNTAX) return 12;

    /* TEST 9: Attempted access to unavailable host APIs */
    err = js_eval(&test_vm, "window.open('http://malicious.com');", &res);
    if (err != JS_ERR_UNSUPPORTED_API) return 13;

    err = js_eval(&test_vm, "fetch('http://example.com');", &res);
    if (err != JS_ERR_UNSUPPORTED_API) return 14;

    err = js_eval(&test_vm, "eval('1 + 1');", &res);
    if (err != JS_ERR_UNSUPPORTED_API) return 15;

    /* TEST 10: Browser recovery after a script error */
    err = js_eval(&test_vm, "var recovery = 100 + 200; recovery;", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 300) return 16;

    return 0;
}

/* -----------------------------------------------------------------
 * Phase S Runtime Hardening Self-Tests (Precedence, Short-Circuit, Overflow, Stale DOM)
 * ----------------------------------------------------------------- */
static int mock_dom_get_elem(void *ctx, const char *id, u32 *out_doc_gen) {
    (void)ctx;
    if (js_streq(id, "target")) {
        if (out_doc_gen) *out_doc_gen = 42; /* generation 42 */
        return 0;
    }
    return -1;
}

static const char *mock_dom_get_text(void *ctx, u32 doc_gen, int node_idx) {
    (void)ctx;
    if (doc_gen == 42 && node_idx == 0) return "Valid DOM Node";
    return NULL; /* Stale or invalid */
}

static bool mock_dom_set_text(void *ctx, u32 doc_gen, int node_idx, const char *text) {
    (void)ctx;
    (void)text;
    if (doc_gen == 42 && node_idx == 0) return true;
    return false; /* Stale handle rejection */
}

int js_run_phase_s_self_tests(void) {
    static js_vm_t s_vm;
    js_init(&s_vm);
    js_value_t res;
    js_error_t err;

    /* 1. Operator Precedence and Parentheses */
    err = js_eval(&s_vm, "10 + 20 * 2;", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 50) return 1;

    err = js_eval(&s_vm, "(10 + 20) * 2;", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 60) return 2;

    err = js_eval(&s_vm, "3 * 4 + 5 * 2;", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 22) return 3;

    /* 2. Negative Integers and Unary Operators */
    err = js_eval(&s_vm, "-10 + 15;", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 5) return 4;

    err = js_eval(&s_vm, "- - 42;", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 42) return 5;

    err = js_eval(&s_vm, "!false;", &res);
    if (err != JS_OK || res.type != JS_VAL_BOOL || res.u.boolean != true) return 6;

    err = js_eval(&s_vm, "!0;", &res);
    if (err != JS_OK || res.type != JS_VAL_BOOL || res.u.boolean != true) return 7;

    err = js_eval(&s_vm, "!5;", &res);
    if (err != JS_OK || res.type != JS_VAL_BOOL || res.u.boolean != false) return 8;

    /* 3. Division and Modulo by Zero */
    err = js_eval(&s_vm, "10 / 0;", &res);
    if (err != JS_ERR_DIV_BY_ZERO) return 9;

    err = js_eval(&s_vm, "10 % 0;", &res);
    if (err != JS_ERR_DIV_BY_ZERO) return 10;

    /* 4. Integer Overflow Boundaries & Division of LLONG_MIN by -1 (Prevents Hardware #DE) */
    err = js_eval(&s_vm, "9223372036854775807 + 1;", &res);
    if (err != JS_ERR_OVERFLOW) return 11;

    err = js_eval(&s_vm, "9223372036854775807 * 2;", &res);
    if (err != JS_ERR_OVERFLOW) return 12;

    /* 5. Nested Functions and Return Values */
    err = js_eval(&s_vm,
        "function sq(n) { return n * n; }\n"
        "function pyth(a, b) { return sq(a) + sq(b); }\n"
        "pyth(3, 4);", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 25) return 13;

    /* 6. Logical Short-Circuit Behavior */
    err = js_eval(&s_vm,
        "var side_effect_run = false;\n"
        "function trigger() { side_effect_run = true; return true; }\n"
        "var sc_and = false && trigger();\n"
        "var sc_or = true || trigger();\n"
        "side_effect_run;", &res);
    if (err != JS_OK || res.type != JS_VAL_BOOL || res.u.boolean != false) return 14;

    /* 7. Missing DOM Elements Return Null & Graceful TypeError on Property Access */
    js_set_dom_bridge(&s_vm, NULL, mock_dom_get_elem, mock_dom_get_text, mock_dom_set_text);

    err = js_eval(&s_vm, "var missing = document.getElementById('nonexistent'); missing;", &res);
    if (err != JS_OK || res.type != JS_VAL_NULL) return 15;

    err = js_eval(&s_vm, "missing.textContent;", &res);
    if (err != JS_ERR_TYPE) return 16;

    /* 8. Stale DOM Handle Rejection across Navigation */
    err = js_eval(&s_vm, "var valid_elem = document.getElementById('target'); valid_elem.textContent = 'Hello';", &res);
    if (err != JS_OK) return 17;

    /* Simulate navigation: change generation to 43 so previous handle (gen 42) becomes stale */
    res.type = JS_VAL_DOM_ELEMENT;
    res.u.dom_handle.doc_generation = 999; /* Stale generation */
    res.u.dom_handle.node_index = 0;
    js_set_var(&s_vm, "stale_elem", &res);

    err = js_eval(&s_vm, "stale_elem.textContent = 'Attack';", &res);
    if (err != JS_ERR_INVALID_HANDLE) return 18;

    return 0;
}

/* -----------------------------------------------------------------
 * Phase T Safe DOM Events Self-Tests
 * ----------------------------------------------------------------- */
static char s_mock_onclick_code[128] = {0};
static bool mock_dom_set_onclick(void *ctx, u32 doc_gen, int node_idx, const char *code) {
    (void)ctx;
    if (doc_gen == 42 && node_idx == 0) {
        js_strcpy(s_mock_onclick_code, code, sizeof(s_mock_onclick_code));
        return true;
    }
    return false;
}

int js_run_phase_t_self_tests(void) {
    static js_vm_t t_vm;
    js_init(&t_vm);
    js_set_dom_bridge(&t_vm, NULL, mock_dom_get_elem, mock_dom_get_text, mock_dom_set_text);
    js_set_dom_event_bridge(&t_vm, mock_dom_set_onclick);
    js_value_t res;
    js_error_t err;

    /* 1. Programmatic assignment of onclick as a string */
    s_mock_onclick_code[0] = '\0';
    err = js_eval(&t_vm, "var btn = document.getElementById('target'); btn.onclick = 'handleClick()';", &res);
    if (err != JS_OK || !js_streq(s_mock_onclick_code, "handleClick()")) return 1;

    /* 2. Programmatic assignment of onclick as a function reference */
    s_mock_onclick_code[0] = '\0';
    err = js_eval(&t_vm, "function doAction() { return 99; }\nbtn.onclick = doAction;", &res);
    if (err != JS_OK || !js_streq(s_mock_onclick_code, "doAction()")) return 2;

    /* 3. Assigning onclick on stale DOM handle returns JS_ERR_INVALID_HANDLE */
    res.type = JS_VAL_DOM_ELEMENT;
    res.u.dom_handle.doc_generation = 999; /* Stale generation */
    res.u.dom_handle.node_index = 0;
    js_set_var(&t_vm, "stale_btn", &res);
    err = js_eval(&t_vm, "stale_btn.onclick = 'shouldFail()';", &res);
    if (err != JS_ERR_INVALID_HANDLE) return 3;

    /* 4. Assigning onclick on missing/null returns JS_ERR_TYPE */
    err = js_eval(&t_vm, "var missing = document.getElementById('nonexistent'); missing.onclick = 'fail()';", &res);
    if (err != JS_ERR_TYPE) return 4;

    /* 5. Executing registered callback mutates state predictably */
    err = js_eval(&t_vm, "var toggle = false;\nfunction clickToggle() { toggle = !toggle; }\nclickToggle(); toggle;", &res);
    if (err != JS_OK || res.type != JS_VAL_BOOL || res.u.boolean != true) return 5;
    err = js_eval(&t_vm, "clickToggle(); toggle;", &res);
    if (err != JS_OK || res.type != JS_VAL_BOOL || res.u.boolean != false) return 6;

    return 0;
}

/* -----------------------------------------------------------------
 * Phase U Adversarial & Robustness Self-Tests
 * ----------------------------------------------------------------- */
int js_run_phase_u_self_tests(void) {
    static js_vm_t u_vm;
    js_init(&u_vm);
    js_set_dom_bridge(&u_vm, NULL, mock_dom_get_elem, mock_dom_get_text, mock_dom_set_text);
    js_set_dom_event_bridge(&u_vm, mock_dom_set_onclick);
    js_value_t res;
    js_error_t err;

    /* TEST 1: Empty string literal "" produces 0-length string */
    err = js_eval(&u_vm, "var empty_str = ''; var x = 123;", &res);
    if (err != JS_OK) return 1;
    js_var_t *ev = js_find_var(&u_vm, "empty_str");
    if (!ev || ev->val.type != JS_VAL_STRING || js_strlen(ev->val.u.string) != 0) return 2;

    /* TEST 2: Escaped characters in string literal preserved without truncation */
    err = js_eval(&u_vm, "var esc_str = 'a\\\\b'; esc_str;", &res);
    if (err != JS_OK || res.type != JS_VAL_STRING || !js_streq(res.u.string, "a\\\\b")) return 3;

    /* TEST 3: Statements after return inside block are skipped safely */
    err = js_eval(&u_vm, "function test_ret_block() { if (true) { return 42; var bad_after = 99; } return 0; } test_ret_block();", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 42) return 4;

    /* TEST 4: Function parameter shadowing does not mutate global variable of same name */
    err = js_eval(&u_vm, "var g_val = 100;\nfunction shadow_param(g_val) { g_val = 555; }\nshadow_param(10); g_val;", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 100) return 5;

    /* TEST 5: Repeated function re-declarations reuse slots without exceeding limit */
    for (int i = 0; i < 25; i++) {
        err = js_eval(&u_vm, "function repeated_func() { return 1; }", &res);
        if (err != JS_OK) return 6;
    }

    /* TEST 6: Unclosed function declaration strictly returns JS_ERR_SYNTAX */
    err = js_eval(&u_vm, "function broken_func() { var z = 1;", &res);
    if (err != JS_ERR_SYNTAX) return 7;

    /* TEST 7: Unsupported onclick expression type returns JS_ERR_TYPE */
    err = js_eval(&u_vm, "var b = document.getElementById('target'); b.onclick = 12345;", &res);
    if (err != JS_ERR_TYPE) return 8;

    /* TEST 8: null == undefined evaluates to true */
    err = js_eval(&u_vm, "var n_eq_u = (null == undefined); var u_eq_n = (undefined == null); n_eq_u && u_eq_n;", &res);
    if (err != JS_OK || res.type != JS_VAL_BOOL || res.u.boolean != true) return 9;

    /* TEST 9: Recovery after infinite loop instruction-limit exhaustion */
    err = js_eval(&u_vm, "while (true) {}", &res);
    if (err != JS_ERR_TIMEOUT) return 10;
    err = js_eval(&u_vm, "var recovered_sum = 15 + 25; recovered_sum;", &res);
    if (err != JS_OK || res.type != JS_VAL_NUMBER || res.u.number != 40) return 11;

    return 0;
}

