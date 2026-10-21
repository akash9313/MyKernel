#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define ASTRA_TYPES_DEFINED
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;

#include "../user/js_engine.h"

s64 sys_write(int fd, const void *buf, size_t count) {
    (void)fd;
    (void)buf;
    return (s64)count;
}

static int mock_get_elem(void *ctx, const char *id, u32 *out_doc_gen) {
    (void)ctx;
    if (out_doc_gen) *out_doc_gen = 1;
    if (strcmp(id, "test_elem") == 0) return 0;
    return -1;
}

static char s_elem_text[128] = "initial";
static const char *mock_get_text(void *ctx, u32 doc_gen, int node_idx) {
    (void)ctx;
    if (doc_gen != 1 || node_idx != 0) return NULL;
    return s_elem_text;
}

static bool mock_set_text(void *ctx, u32 doc_gen, int node_idx, const char *text) {
    (void)ctx;
    if (doc_gen != 1 || node_idx != 0 || !text) return false;
    strncpy(s_elem_text, text, sizeof(s_elem_text) - 1);
    s_elem_text[sizeof(s_elem_text) - 1] = '\0';
    return true;
}

static char s_onclick_buf[128] = "";
static bool mock_set_onclick(void *ctx, u32 doc_gen, int node_idx, const char *code) {
    (void)ctx;
    if (doc_gen != 1 || node_idx != 0 || !code) return false;
    strncpy(s_onclick_buf, code, sizeof(s_onclick_buf) - 1);
    s_onclick_buf[sizeof(s_onclick_buf) - 1] = '\0';
    return true;
}

__declspec(dllexport) int fuzz_eval_js(const char *input_code, int *out_recovery_code) {
    if (!input_code) return -1;

    js_vm_t vm;
    js_init(&vm);
    js_set_dom_bridge(&vm, NULL, mock_get_elem, mock_get_text, mock_set_text);
    js_set_dom_event_bridge(&vm, mock_set_onclick);

    js_value_t res;
    js_error_t err = js_eval(&vm, input_code, &res);

    if (out_recovery_code) {
        js_value_t res2;
        js_error_t err2 = js_eval(&vm, "var recovery_check = 10 + 20;", &res2);
        *out_recovery_code = (int)err2;
    }

    return (int)err;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 0;
}
