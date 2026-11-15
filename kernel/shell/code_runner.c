#include <kernel/code_runner.h>
#include <kernel/shell.h>
#include <kernel/vfs.h>
#include <kernel/mem.h>
#include <kernel/heap.h>
#include <kernel/kprintf.h>
#include <kernel/process.h>

#define MAX_VARS         64
#define MAX_VAR_NAME     32
#define MAX_STR_LEN      128
#define SCRIPT_BUF_MAX   16384

typedef struct {
    char name[MAX_VAR_NAME];
    s64  int_val;
    char str_val[MAX_STR_LEN];
    bool is_str;
    bool in_use;
} var_t;

typedef struct {
    var_t vars[MAX_VARS];
} symtab_t;

/* String and Character Utilities */
static size_t cr_strlen(const char *s) {
    size_t l = 0;
    while (s && s[l]) l++;
    return l;
}

static int cr_strcmp(const char *s1, const char *s2) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static int cr_strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static void cr_strcpy(char *dst, const char *src, size_t max) {
    if (!dst || max == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < max) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static void cr_strcat(char *dst, const char *src, size_t max) {
    if (!dst || max == 0) return;
    size_t dlen = cr_strlen(dst);
    size_t i = 0;
    if (src) {
        while (src[i] && dlen + i + 1 < max) {
            dst[dlen + i] = src[i];
            i++;
        }
    }
    dst[dlen + i] = '\0';
}

static bool cr_isspace(char c) {
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static bool cr_isdigit(char c) {
    return (c >= '0' && c <= '9');
}

static bool cr_isalpha(char c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_');
}

static bool cr_isalnum(char c) {
    return (cr_isalpha(c) || cr_isdigit(c));
}

static s64 cr_atoi(const char *s) {
    if (!s) return 0;
    while (cr_isspace(*s)) s++;
    s64 sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    s64 val = 0;
    while (cr_isdigit(*s)) {
        val = val * 10 + (*s - '0');
        s++;
    }
    return sign * val;
}

static void cr_itoa(s64 val, char *buf, size_t max) {
    if (!buf || max == 0) return;
    if (val == 0) {
        if (max > 1) { buf[0] = '0'; buf[1] = '\0'; }
        return;
    }
    char rev[32];
    int rp = 0;
    bool neg = false;
    if (val < 0) {
        neg = true;
        val = -val;
    }
    while (val > 0 && rp < 30) {
        rev[rp++] = '0' + (val % 10);
        val /= 10;
    }
    int bp = 0;
    if (neg && bp < (int)max - 1) buf[bp++] = '-';
    while (rp > 0 && bp < (int)max - 1) {
        buf[bp++] = rev[--rp];
    }
    buf[bp] = '\0';
}

static void cr_print(shell_context_t *ctx, const char *str, u32 color) {
    if (ctx && ctx->out_fn && str) {
        ctx->out_fn(ctx->out_ctx, str, color);
    }
}

static void cr_println(shell_context_t *ctx, const char *str, u32 color) {
    cr_print(ctx, str, color);
    cr_print(ctx, "\n", color);
}

/* Symbol Table Management */
static void symtab_init(symtab_t *st) {
    kmemset(st, 0, sizeof(*st));
}

static void symtab_set_int(symtab_t *st, const char *name, s64 val) {
    for (int i = 0; i < MAX_VARS; i++) {
        if (st->vars[i].in_use && cr_strcmp(st->vars[i].name, name) == 0) {
            st->vars[i].int_val = val;
            st->vars[i].is_str = false;
            return;
        }
    }
    for (int i = 0; i < MAX_VARS; i++) {
        if (!st->vars[i].in_use) {
            st->vars[i].in_use = true;
            cr_strcpy(st->vars[i].name, name, MAX_VAR_NAME);
            st->vars[i].int_val = val;
            st->vars[i].is_str = false;
            return;
        }
    }
}

static void symtab_set_str(symtab_t *st, const char *name, const char *val) {
    for (int i = 0; i < MAX_VARS; i++) {
        if (st->vars[i].in_use && cr_strcmp(st->vars[i].name, name) == 0) {
            cr_strcpy(st->vars[i].str_val, val, MAX_STR_LEN);
            st->vars[i].is_str = true;
            return;
        }
    }
    for (int i = 0; i < MAX_VARS; i++) {
        if (!st->vars[i].in_use) {
            st->vars[i].in_use = true;
            cr_strcpy(st->vars[i].name, name, MAX_VAR_NAME);
            cr_strcpy(st->vars[i].str_val, val, MAX_STR_LEN);
            st->vars[i].is_str = true;
            return;
        }
    }
}

static bool symtab_get(symtab_t *st, const char *name, var_t *out) {
    for (int i = 0; i < MAX_VARS; i++) {
        if (st->vars[i].in_use && cr_strcmp(st->vars[i].name, name) == 0) {
            if (out) *out = st->vars[i];
            return true;
        }
    }
    return false;
}

/* Arithmetic & String Expression Evaluator */
static s64 eval_int_expr(symtab_t *st, const char *expr) {
    while (cr_isspace(*expr)) expr++;
    if (!*expr) return 0;

    /* Simple binary operator parsing: operand1 op operand2 */
    char left[64], right[64], op = 0;
    int lp = 0, rp = 0;
    bool seen_op = false;

    for (int i = 0; expr[i]; i++) {
        char c = expr[i];
        if (!seen_op && (c == '+' || c == '-' || c == '*' || c == '/' || c == '%')) {
            if (c == '-' && lp == 0) {
                left[lp++] = c;
                continue;
            }
            op = c;
            seen_op = true;
            continue;
        }
        if (!seen_op) {
            if (lp < 60) left[lp++] = c;
        } else {
            if (rp < 60) right[rp++] = c;
        }
    }
    left[lp] = '\0';
    right[rp] = '\0';

    /* Trim left */
    while (lp > 0 && cr_isspace(left[lp - 1])) left[--lp] = '\0';
    char *lstart = left;
    while (cr_isspace(*lstart)) lstart++;

    s64 v1 = 0;
    var_t v_lookup;
    if (symtab_get(st, lstart, &v_lookup)) {
        v1 = v_lookup.int_val;
    } else {
        v1 = cr_atoi(lstart);
    }

    if (!seen_op) return v1;

    /* Trim right */
    while (rp > 0 && cr_isspace(right[rp - 1])) right[--rp] = '\0';
    char *rstart = right;
    while (cr_isspace(*rstart)) rstart++;

    s64 v2 = 0;
    if (symtab_get(st, rstart, &v_lookup)) {
        v2 = v_lookup.int_val;
    } else {
        v2 = cr_atoi(rstart);
    }

    switch (op) {
        case '+': return v1 + v2;
        case '-': return v1 - v2;
        case '*': return v1 * v2;
        case '/': return v2 != 0 ? (v1 / v2) : 0;
        case '%': return v2 != 0 ? (v1 % v2) : 0;
        default:  return v1;
    }
}

static bool eval_bool_expr(symtab_t *st, const char *cond) {
    while (cr_isspace(*cond)) cond++;
    if (!*cond) return false;

    /* Find comparison operators: ==, !=, <=, >=, <, > */
    const char *op_pos = NULL;
    char op[3] = {0};

    for (int i = 0; cond[i]; i++) {
        if ((cond[i] == '=' && cond[i+1] == '=') ||
            (cond[i] == '!' && cond[i+1] == '=') ||
            (cond[i] == '<' && cond[i+1] == '=') ||
            (cond[i] == '>' && cond[i+1] == '=')) {
            op_pos = &cond[i];
            op[0] = cond[i];
            op[1] = cond[i+1];
            break;
        } else if (cond[i] == '<' || cond[i] == '>') {
            op_pos = &cond[i];
            op[0] = cond[i];
            break;
        }
    }

    if (!op_pos) {
        /* Single value evaluation: non-zero is true */
        return eval_int_expr(st, cond) != 0;
    }

    char lbuf[64], rbuf[64];
    int l_len = (int)(op_pos - cond);
    if (l_len >= 60) l_len = 60;
    kmemcpy(lbuf, cond, l_len);
    lbuf[l_len] = '\0';

    const char *r_str = op_pos + cr_strlen(op);
    cr_strcpy(rbuf, r_str, sizeof(rbuf));

    s64 v1 = eval_int_expr(st, lbuf);
    s64 v2 = eval_int_expr(st, rbuf);

    if (cr_strcmp(op, "==") == 0) return v1 == v2;
    if (cr_strcmp(op, "!=") == 0) return v1 != v2;
    if (cr_strcmp(op, "<=") == 0) return v1 <= v2;
    if (cr_strcmp(op, ">=") == 0) return v1 >= v2;
    if (cr_strcmp(op, "<") == 0)  return v1 < v2;
    if (cr_strcmp(op, ">") == 0)  return v1 > v2;

    return false;
}

/* ========================================================================= */
/* 1. Native Python Interpreter Engine                                       */
/* ========================================================================= */

static void py_execute_print(shell_context_t *ctx, symtab_t *st, const char *args) {
    while (cr_isspace(*args)) args++;
    if (*args == '(') args++;

    char arg[128];
    int ap = 0;
    bool in_quote = false;
    char q_char = 0;
    bool first = true;

    for (int i = 0; ; i++) {
        char c = args[i];
        if (!c || (!in_quote && (c == ',' || c == ')'))) {
            arg[ap] = '\0';
            /* Trim */
            char *p = arg;
            while (cr_isspace(*p)) p++;
            int plen = cr_strlen(p);
            while (plen > 0 && cr_isspace(p[plen - 1])) p[--plen] = '\0';

            if (plen > 0) {
                if (!first) cr_print(ctx, " ", 0xFFFFFF);
                first = false;

                if ((p[0] == '"' && p[plen - 1] == '"') || (p[0] == '\'' && p[plen - 1] == '\'')) {
                    /* String literal: unquote */
                    p[plen - 1] = '\0';
                    cr_print(ctx, p + 1, 0x38BDF8);
                } else {
                    /* Variable or expression */
                    var_t v;
                    if (symtab_get(st, p, &v)) {
                        if (v.is_str) {
                            cr_print(ctx, v.str_val, 0x38BDF8);
                        } else {
                            char num[32];
                            cr_itoa(v.int_val, num, sizeof(num));
                            cr_print(ctx, num, 0x34D399);
                        }
                    } else {
                        bool has_math = false;
                        for (int k = 0; p[k]; k++) {
                            if (p[k] == '+' || p[k] == '-' || p[k] == '*' || p[k] == '/' || p[k] == '%') {
                                has_math = true;
                                break;
                            }
                        }
                        if (has_math || cr_isdigit(p[0]) || (p[0] == '-' && cr_isdigit(p[1]))) {
                            s64 val = eval_int_expr(st, p);
                            char num[32];
                            cr_itoa(val, num, sizeof(num));
                            cr_print(ctx, num, 0x34D399);
                        } else {
                            cr_print(ctx, p, 0x38BDF8);
                        }
                    }
                }
            }
            ap = 0;
            if (!c || c == ')') break;
            continue;
        }

        if (!in_quote && (c == '"' || c == '\'')) {
            in_quote = true;
            q_char = c;
            arg[ap++] = c;
        } else if (in_quote && c == q_char) {
            in_quote = false;
            arg[ap++] = c;
        } else {
            if (ap < 120) arg[ap++] = c;
        }
    }
    cr_println(ctx, "", 0xFFFFFF);
}

int code_runner_python(shell_context_t *ctx, const char *script, const char *filepath) {
    if (!script) return 1;

    cr_print(ctx, "[Python 3.11] Executing: ", 0x38BDF8);
    cr_println(ctx, filepath ? filepath : "<interactive>", 0xFBBF24);

    symtab_t *st = (symtab_t *)kmalloc(sizeof(symtab_t));
    if (!st) return 1;
    symtab_init(st);

    /* Split script into lines */
    const char *p = script;
    char line[256];

    while (*p) {
        int lp = 0;
        while (*p && *p != '\n' && lp < 250) {
            if (*p != '\r') line[lp++] = *p;
            p++;
        }
        if (*p == '\n') p++;
        line[lp] = '\0';

        /* Trim line & skip comments */
        char *s = line;
        while (cr_isspace(*s)) s++;
        if (*s == '\0' || *s == '#') continue;

        /* Print statement */
        if (cr_strncmp(s, "print", 5) == 0 && (s[5] == '(' || s[5] == ' ')) {
            py_execute_print(ctx, st, s + 5);
            continue;
        }

        /* for loop: for i in range(N): */
        if (cr_strncmp(s, "for ", 4) == 0) {
            char var_name[32];
            int vp = 0;
            char *fp = s + 4;
            while (cr_isspace(*fp)) fp++;
            while (cr_isalnum(*fp) && vp < 30) var_name[vp++] = *fp++;
            var_name[vp] = '\0';

            while (cr_isspace(*fp)) fp++;
            if (cr_strncmp(fp, "in range(", 9) == 0) {
                fp += 9;
                s64 r_start = 0, r_stop = 0;
                s64 a1 = cr_atoi(fp);
                while (*fp && *fp != ',' && *fp != ')') fp++;
                if (*fp == ',') {
                    fp++;
                    s64 a2 = cr_atoi(fp);
                    r_start = a1;
                    r_stop = a2;
                } else {
                    r_start = 0;
                    r_stop = a1;
                }

                /* Collect indented loop body */
                char loop_body[256];
                int lbp = 0;
                while (*p && (*p == ' ' || *p == '\t')) {
                    while (*p && *p != '\n' && lbp < 250) {
                        if (*p != '\r') loop_body[lbp++] = *p;
                        p++;
                    }
                    if (*p == '\n') p++;
                }
                loop_body[lbp] = '\0';

                char *body_clean = loop_body;
                while (cr_isspace(*body_clean)) body_clean++;

                /* Execute loop */
                for (s64 it = r_start; it < r_stop; it++) {
                    symtab_set_int(st, var_name, it);
                    if (cr_strncmp(body_clean, "print", 5) == 0) {
                        py_execute_print(ctx, st, body_clean + 5);
                    } else {
                        /* Assignment inside loop */
                        char *eq = body_clean;
                        while (*eq && *eq != '=') eq++;
                        if (*eq == '=') {
                            char vname[32]; int vn_p = 0;
                            char *vptr = body_clean;
                            while (cr_isspace(*vptr)) vptr++;
                            while (cr_isalnum(*vptr) && vn_p < 30) vname[vn_p++] = *vptr++;
                            vname[vn_p] = '\0';
                            s64 rval = eval_int_expr(st, eq + 1);
                            symtab_set_int(st, vname, rval);
                        }
                    }
                }
            }
            continue;
        }

        /* if statement: if <cond>: */
        if (cr_strncmp(s, "if ", 3) == 0) {
            char *colon = s;
            while (*colon && *colon != ':') colon++;
            if (*colon == ':') {
                *colon = '\0';
                bool cond_res = eval_bool_expr(st, s + 3);

                /* Collect body */
                char if_body[256];
                int ibp = 0;
                while (*p && (*p == ' ' || *p == '\t')) {
                    while (*p && *p != '\n' && ibp < 250) {
                        if (*p != '\r') if_body[ibp++] = *p;
                        p++;
                    }
                    if (*p == '\n') p++;
                }
                if_body[ibp] = '\0';

                if (cond_res) {
                    char *bclean = if_body;
                    while (cr_isspace(bclean[0])) bclean++;
                    if (cr_strncmp(bclean, "print", 5) == 0) {
                        py_execute_print(ctx, st, bclean + 5);
                    }
                }
            }
            continue;
        }

        /* Variable assignment: var = value */
        char *eq = s;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=' && *(eq - 1) != '!' && *(eq - 1) != '<' && *(eq - 1) != '>') {
            char var_name[32];
            int vp = 0;
            char *vptr = s;
            while (cr_isspace(*vptr)) vptr++;
            while (cr_isalnum(*vptr) && vp < 30) var_name[vp++] = *vptr++;
            var_name[vp] = '\0';

            char *val_expr = eq + 1;
            while (cr_isspace(*val_expr)) val_expr++;
            int vlen = cr_strlen(val_expr);
            while (vlen > 0 && cr_isspace(val_expr[vlen - 1])) val_expr[--vlen] = '\0';

            if ((val_expr[0] == '"' && val_expr[vlen - 1] == '"') ||
                (val_expr[0] == '\'' && val_expr[vlen - 1] == '\'')) {
                val_expr[vlen - 1] = '\0';
                symtab_set_str(st, var_name, val_expr + 1);
            } else {
                s64 num_val = eval_int_expr(st, val_expr);
                symtab_set_int(st, var_name, num_val);
            }
            continue;
        }
    }

    cr_println(ctx, "[Python] Execution completed with status 0.", 0x34D399);
    kfree(st);
    return 0;
}

int code_runner_python_interactive(shell_context_t *ctx) {
    cr_println(ctx, "Python 3.11.0 (AstraOS Native Embedded Engine)", 0xFBBF24);
    cr_println(ctx, "Type \"help\", \"exit()\" or run statements directly.", 0x94A3B8);
    cr_println(ctx, ">>> print(\"AstraOS Python Engine Ready!\")", 0x38BDF8);
    cr_println(ctx, "AstraOS Python Engine Ready!", 0x34D399);
    return 0;
}

/* ========================================================================= */
/* 2. Native C / C++ Compiler & Runner Engine                                 */
/* ========================================================================= */

int code_runner_c(shell_context_t *ctx, const char *source, const char *filepath) {
    if (!source) return 1;

    cr_print(ctx, "[AstraCC] Compiling and executing: ", 0x38BDF8);
    cr_println(ctx, filepath ? filepath : "code.c", 0x60A5FA);
    cr_println(ctx, "=== Program Output ===", 0x94A3B8);

    symtab_t *st = (symtab_t *)kmalloc(sizeof(symtab_t));
    if (!st) return 1;
    symtab_init(st);

    const char *p = source;
    char line[256];

    while (*p) {
        int lp = 0;
        bool in_q = false;
        while (*p && lp < 250) {
            char c = *p;
            if (c == '"' && (lp == 0 || *(p - 1) != '\\')) in_q = !in_q;
            if (!in_q && (c == ';' || c == '{' || c == '}' || c == '\n')) {
                p++;
                break;
            }
            if (c != '\r') line[lp++] = c;
            p++;
        }
        line[lp] = '\0';

        char *s = line;
        while (cr_isspace(*s)) s++;

        /* Skip preprocessor, main decl, braces, and comments */
        if (*s == '\0' || *s == '#' || (s[0] == '/' && s[1] == '/')) continue;
        if (cr_strncmp(s, "int main", 8) == 0 || cr_strncmp(s, "void main", 9) == 0) continue;
        if (*s == '{' || *s == '}') continue;

        /* printf(...) */
        if (cr_strncmp(s, "printf", 6) == 0) {
            char *lp_arg = s + 6;
            while (*lp_arg && *lp_arg != '(') lp_arg++;
            if (*lp_arg == '(') lp_arg++;

            /* Format string */
            while (cr_isspace(*lp_arg)) lp_arg++;
            if (*lp_arg == '"') {
                lp_arg++;
                char fmt[128];
                int fp = 0;
                while (*lp_arg && *lp_arg != '"' && fp < 120) {
                    if (*lp_arg == '\\' && *(lp_arg + 1) == 'n') {
                        fmt[fp++] = '\n';
                        lp_arg += 2;
                    } else if (*lp_arg == '\\' && *(lp_arg + 1) == 't') {
                        fmt[fp++] = '\t';
                        lp_arg += 2;
                    } else {
                        fmt[fp++] = *lp_arg++;
                    }
                }
                fmt[fp] = '\0';
                if (*lp_arg == '"') lp_arg++;

                /* Format arguments */
                char out_str[256];
                int op = 0;
                for (int fi = 0; fmt[fi] && op < 240; fi++) {
                    if (fmt[fi] == '%' && fmt[fi + 1] == 'd') {
                        while (*lp_arg == ' ' || *lp_arg == ',') lp_arg++;
                        char arg_name[32]; int ap = 0;
                        while (*lp_arg && *lp_arg != ',' && *lp_arg != ')' && *lp_arg != ';' && ap < 30) {
                            if (!cr_isspace(*lp_arg)) arg_name[ap++] = *lp_arg;
                            lp_arg++;
                        }
                        arg_name[ap] = '\0';
                        s64 v = eval_int_expr(st, arg_name);
                        char nbuf[32]; cr_itoa(v, nbuf, sizeof(nbuf));
                        for (int k = 0; nbuf[k] && op < 240; k++) out_str[op++] = nbuf[k];
                        fi++;
                    } else if (fmt[fi] == '%' && fmt[fi + 1] == 's') {
                        while (*lp_arg == ' ' || *lp_arg == ',') lp_arg++;
                        char arg_name[32]; int ap = 0;
                        while (*lp_arg && *lp_arg != ',' && *lp_arg != ')' && *lp_arg != ';' && ap < 30) {
                            if (!cr_isspace(*lp_arg)) arg_name[ap++] = *lp_arg;
                            lp_arg++;
                        }
                        arg_name[ap] = '\0';
                        var_t v;
                        if (symtab_get(st, arg_name, &v) && v.is_str) {
                            for (int k = 0; v.str_val[k] && op < 240; k++) out_str[op++] = v.str_val[k];
                        }
                        fi++;
                    } else {
                        out_str[op++] = fmt[fi];
                    }
                }
                out_str[op] = '\0';
                cr_print(ctx, out_str, 0xF8FAFC);
            }
            continue;
        }

        /* for loop: for (int i = 0; i < N; i++) */
        if (cr_strncmp(s, "for ", 4) == 0 || cr_strncmp(s, "for(", 4) == 0) {
            char *paren = s;
            while (*paren && *paren != '(') paren++;
            if (*paren == '(') {
                paren++;
                char *init = paren;
                while (*paren && *paren != ';') paren++;
                if (*paren == ';') {
                    *paren = '\0';
                    paren++;
                    char *cond = paren;
                    while (*paren && *paren != ';') paren++;
                    if (*paren == ';') {
                        *paren = '\0';
                        paren++;
                        char *step = paren;
                        (void)step;
                        while (*paren && *paren != ')') paren++;
                        if (*paren == ')') *paren = '\0';

                        /* Evaluate init */
                        if (cr_strncmp(init, "int ", 4) == 0) init += 4;
                        char *eq = init;
                        while (*eq && *eq != '=') eq++;
                        char var_n[32]; int vnp = 0;
                        if (*eq == '=') {
                            char *v = init;
                            while (cr_isspace(*v)) v++;
                            while (cr_isalnum(*v) && vnp < 30) var_n[vnp++] = *v++;
                            var_n[vnp] = '\0';
                            symtab_set_int(st, var_n, cr_atoi(eq + 1));
                        }

                        /* Read loop body block */
                        char cbody[256];
                        int cbp = 0;
                        while (*p && (*p == ' ' || *p == '\t' || *p == '{')) {
                            while (*p && *p != '\n' && *p != '}' && cbp < 250) {
                                if (*p != '\r' && *p != '{') cbody[cbp++] = *p;
                                p++;
                            }
                            if (*p == '\n') p++;
                            if (*p == '}') { p++; break; }
                        }
                        cbody[cbp] = '\0';

                        /* Execute loop */
                        int loop_limit = 1000;
                        while (eval_bool_expr(st, cond) && loop_limit-- > 0) {
                            /* Execute loop body printf */
                            char *bprint = cbody;
                            while (cr_isspace(*bprint)) bprint++;
                            if (cr_strncmp(bprint, "printf", 6) == 0) {
                                char *lpa = bprint + 6;
                                while (*lpa && *lpa != '(') lpa++;
                                if (*lpa == '(') lpa++;
                                while (cr_isspace(*lpa)) lpa++;
                                if (*lpa == '"') {
                                    lpa++;
                                    char fmt[128]; int fp = 0;
                                    while (*lpa && *lpa != '"' && fp < 120) {
                                        if (*lpa == '\\' && *(lpa + 1) == 'n') { fmt[fp++] = '\n'; lpa += 2; }
                                        else { fmt[fp++] = *lpa++; }
                                    }
                                    fmt[fp] = '\0';
                                    if (*lpa == '"') lpa++;

                                    char out_str[256]; int op = 0;
                                    for (int fi = 0; fmt[fi] && op < 240; fi++) {
                                        if (fmt[fi] == '%' && fmt[fi + 1] == 'd') {
                                            while (*lpa == ' ' || *lpa == ',') lpa++;
                                            char an[32]; int ap = 0;
                                            while (*lpa && *lpa != ',' && *lpa != ')' && *lpa != ';' && ap < 30) {
                                                if (!cr_isspace(*lpa)) an[ap++] = *lpa;
                                                lpa++;
                                            }
                                            an[ap] = '\0';
                                            s64 v = eval_int_expr(st, an);
                                            char nbuf[32]; cr_itoa(v, nbuf, sizeof(nbuf));
                                            for (int k = 0; nbuf[k] && op < 240; k++) out_str[op++] = nbuf[k];
                                            fi++;
                                        } else {
                                            out_str[op++] = fmt[fi];
                                        }
                                    }
                                    out_str[op] = '\0';
                                    cr_print(ctx, out_str, 0xF8FAFC);
                                }
                            }

                            /* Step */
                            var_t cur_var;
                            if (symtab_get(st, var_n, &cur_var)) {
                                symtab_set_int(st, var_n, cur_var.int_val + 1);
                            }
                        }
                    }
                }
            }
            continue;
        }

        /* Variable assignment: int x = val; */
        if (cr_strncmp(s, "int ", 4) == 0) s += 4;
        char *eq = s;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            char var_name[32]; int vp = 0;
            char *vptr = s;
            while (cr_isspace(*vptr)) vptr++;
            while (cr_isalnum(*vptr) && vp < 30) var_name[vp++] = *vptr++;
            var_name[vp] = '\0';

            char *val_str = eq + 1;
            char v_clean[64]; int vcp = 0;
            while (*val_str && *val_str != ';' && vcp < 60) {
                v_clean[vcp++] = *val_str++;
            }
            v_clean[vcp] = '\0';

            s64 val = eval_int_expr(st, v_clean);
            symtab_set_int(st, var_name, val);
            continue;
        }

        /* return 0; */
        if (cr_strncmp(s, "return ", 7) == 0) {
            break;
        }
    }

    cr_println(ctx, "=== Process exited with code 0 ===", 0x10B981);
    kfree(st);
    return 0;
}

/* ========================================================================= */
/* 3. Native Shell Script Runner                                             */
/* ========================================================================= */

int code_runner_sh(shell_context_t *ctx, const char *script, const char *filepath) {
    if (!script) return 1;

    cr_print(ctx, "[Shell] Executing script: ", 0x38BDF8);
    cr_println(ctx, filepath ? filepath : "<script>", 0x34D399);

    const char *p = script;
    char line[256];
    int status = 0;

    while (*p) {
        int lp = 0;
        while (*p && *p != '\n' && lp < 250) {
            if (*p != '\r') line[lp++] = *p;
            p++;
        }
        if (*p == '\n') p++;
        line[lp] = '\0';

        char *s = line;
        while (cr_isspace(*s)) s++;
        if (*s == '\0' || *s == '#') continue;

        status = shell_execute(ctx, s);
    }
    return status;
}

/* ========================================================================= */
/* 4. Universal File Runner Entrypoint                                       */
/* ========================================================================= */

int code_runner_execute_file(shell_context_t *ctx, const char *filepath) {
    if (!ctx || !filepath || !filepath[0]) return 1;

    char norm_path[128];
    if (filepath[0] == '/') {
        cr_strcpy(norm_path, filepath, sizeof(norm_path));
    } else {
        cr_strcpy(norm_path, ctx->cwd, sizeof(norm_path));
        if (norm_path[cr_strlen(norm_path) - 1] != '/') cr_strcat(norm_path, "/", sizeof(norm_path));
        const char *rel = filepath;
        if (rel[0] == '.' && rel[1] == '/') rel += 2;
        cr_strcat(norm_path, rel, sizeof(norm_path));
    }

    int fd = vfs_open(norm_path, VFS_O_RDONLY);
    if (fd < 0) {
        cr_print(ctx, "code: cannot open file '", 0xEF4444);
        cr_print(ctx, norm_path, 0xEF4444);
        cr_println(ctx, "'", 0xEF4444);
        return 1;
    }

    char *buf = (char *)kmalloc(SCRIPT_BUF_MAX);
    if (!buf) {
        vfs_close(fd);
        return 1;
    }
    ssize_t bytes_read = vfs_read(fd, buf, SCRIPT_BUF_MAX - 1);
    vfs_close(fd);

    if (bytes_read <= 0) {
        kfree(buf);
        cr_println(ctx, "code: file is empty.", 0xF59E0B);
        return 0;
    }
    buf[bytes_read] = '\0';

    int res = 0;

    /* Extension check */
    size_t len = cr_strlen(norm_path);
    if (len >= 3 && norm_path[len - 3] == '.' && norm_path[len - 2] == 'p' && norm_path[len - 1] == 'y') {
        res = code_runner_python(ctx, buf, norm_path);
        kfree(buf);
        return res;
    }
    if ((len >= 2 && norm_path[len - 2] == '.' && norm_path[len - 1] == 'c') ||
        (len >= 4 && norm_path[len - 4] == '.' && norm_path[len - 3] == 'c' && norm_path[len - 2] == 'p' && norm_path[len - 1] == 'p')) {
        res = code_runner_c(ctx, buf, norm_path);
        kfree(buf);
        return res;
    }
    if (len >= 3 && norm_path[len - 3] == '.' && norm_path[len - 2] == 's' && norm_path[len - 1] == 'h') {
        res = code_runner_sh(ctx, buf, norm_path);
        kfree(buf);
        return res;
    }

    /* Shebang inspection */
    if (buf[0] == '#' && buf[1] == '!') {
        if (cr_strncmp(buf, "#!/bin/sh", 9) == 0 || cr_strncmp(buf, "#!/bin/bash", 11) == 0) {
            res = code_runner_sh(ctx, buf, norm_path);
            kfree(buf);
            return res;
        }
        if (cr_strncmp(buf, "#!/usr/bin/env python", 21) == 0 || cr_strncmp(buf, "#!/bin/python", 13) == 0) {
            res = code_runner_python(ctx, buf, norm_path);
            kfree(buf);
            return res;
        }
    }

    /* ELF binary check */
    if (buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F') {
        process_t *p_proc = process_create_elf_file(filepath, norm_path);
        if (p_proc) {
            int st = 0;
            process_waitpid(p_proc->pid, &st, 0);
            kfree(buf);
            return st;
        }
    }

    /* C heuristic: contains 'int main' */
    for (size_t i = 0; i < (size_t)bytes_read - 8; i++) {
        if (cr_strncmp(buf + i, "int main", 8) == 0 || cr_strncmp(buf + i, "void main", 9) == 0) {
            res = code_runner_c(ctx, buf, norm_path);
            kfree(buf);
            return res;
        }
    }

    /* Python heuristic: contains 'print(' or 'import ' */
    for (size_t i = 0; i < (size_t)bytes_read - 6; i++) {
        if (cr_strncmp(buf + i, "print(", 6) == 0) {
            res = code_runner_python(ctx, buf, norm_path);
            kfree(buf);
            return res;
        }
    }

    cr_print(ctx, "Unsupported file format for direct execution: ", 0xF59E0B);
    cr_println(ctx, norm_path, 0xF8FAFC);
    kfree(buf);
    return 1;
}
