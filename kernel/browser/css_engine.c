#include <kernel/css.h>
#include <kernel/mem.h>
#include <kernel/kprintf.h>

static bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static bool str_case_eq(const char *s, const char *lit) {
    while (*s && *lit) {
        if (to_lower(*s) != to_lower(*lit)) return false;
        s++;
        lit++;
    }
    return (*s == '\0' && *lit == '\0');
}

static bool str_case_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (to_lower(*s) != to_lower(*prefix)) return false;
        s++;
        prefix++;
    }
    return true;
}

static size_t k_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

static int parse_int(const char *str, int *out_val) {
    while (*str && is_whitespace(*str)) str++;
    if (!*str) return 0;

    int sign = 1;
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    int val = 0;
    int digits = 0;
    while (*str >= '0' && *str <= '9') {
        val = val * 10 + (*str - '0');
        digits++;
        str++;
    }

    if (digits == 0) return 0;
    if (out_val) *out_val = val * sign;
    return digits;
}

static int parse_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

u32 css_parse_color(const char *val) {
    if (!val) return 0xFFFFFF;
    while (*val && is_whitespace(*val)) val++;

    /* 1. Hex format: #rgb or #rrggbb */
    if (val[0] == '#') {
        val++;
        int d[6];
        int count = 0;
        while (count < 6) {
            int hd = parse_hex_digit(val[count]);
            if (hd < 0) break;
            d[count] = hd;
            count++;
        }

        if (count == 3) {
            /* #rgb -> #rrggbb */
            u32 r = (u32)(d[0] * 16 + d[0]);
            u32 g = (u32)(d[1] * 16 + d[1]);
            u32 b = (u32)(d[2] * 16 + d[2]);
            return (r << 16) | (g << 8) | b;
        } else if (count >= 6) {
            u32 r = (u32)(d[0] * 16 + d[1]);
            u32 g = (u32)(d[2] * 16 + d[3]);
            u32 b = (u32)(d[4] * 16 + d[5]);
            return (r << 16) | (g << 8) | b;
        }
        return 0xFFFFFF;
    }

    /* 2. Standard Named Colors */
    if (str_case_eq(val, "black"))      return 0x000000;
    if (str_case_eq(val, "white"))      return 0xFFFFFF;
    if (str_case_eq(val, "red"))        return 0xEF4444;
    if (str_case_eq(val, "green"))      return 0x22C55E;
    if (str_case_eq(val, "blue"))       return 0x38BDF8;
    if (str_case_eq(val, "yellow"))     return 0xEAB308;
    if (str_case_eq(val, "cyan"))       return 0x2DD4BF;
    if (str_case_eq(val, "magenta") || str_case_eq(val, "fuchsia")) return 0xEC4899;
    if (str_case_eq(val, "gray") || str_case_eq(val, "grey")) return 0x94A3B8;
    if (str_case_eq(val, "darkgray") || str_case_eq(val, "darkgrey")) return 0x334155;
    if (str_case_eq(val, "lightgray") || str_case_eq(val, "lightgrey")) return 0xE2E8F0;
    if (str_case_eq(val, "navy"))       return 0x0B1120;
    if (str_case_eq(val, "purple"))     return 0x818CF8;
    if (str_case_eq(val, "orange"))     return 0xF97316;
    if (str_case_eq(val, "silver"))     return 0xCBD5E1;
    if (str_case_eq(val, "teal"))       return 0x0D9488;
    if (str_case_eq(val, "maroon"))     return 0x991B1B;
    if (str_case_eq(val, "olive"))      return 0x84CC16;

    return 0xFFFFFF;
}

void css_init_style(css_style_t *style) {
    if (!style) return;
    kmemset(style, 0, sizeof(css_style_t));
    style->font_scale = 1;
    style->display = CSS_DISPLAY_INLINE;
    style->text_align = CSS_ALIGN_LEFT;
}

static void parse_property(const char *prop, const char *val, css_style_t *style) {
    if (!prop || !val || !style) return;

    /* Trim leading whitespace */
    while (*prop && is_whitespace(*prop)) prop++;
    while (*val && is_whitespace(*val)) val++;

    if (str_case_eq(prop, "color")) {
        style->color = css_parse_color(val);
        style->has_color = true;
    } else if (str_case_eq(prop, "background-color") || str_case_eq(prop, "background")) {
        if (str_case_eq(val, "none") || str_case_eq(val, "transparent")) {
            style->has_bg_color = false;
        } else {
            style->bg_color = css_parse_color(val);
            style->has_bg_color = true;
        }
    } else if (str_case_eq(prop, "font-size")) {
        if (str_case_eq(val, "large") || str_case_eq(val, "x-large") || str_case_eq(val, "xx-large")) {
            style->font_scale = 2;
        } else {
            int px = 0;
            if (parse_int(val, &px) && px > 18) {
                style->font_scale = 2;
            } else {
                style->font_scale = 1;
            }
        }
    } else if (str_case_eq(prop, "font-weight")) {
        if (str_case_eq(val, "bold") || str_case_eq(val, "bolder") ||
            str_case_eq(val, "700") || str_case_eq(val, "800") || str_case_eq(val, "900")) {
            style->bold = true;
        } else if (str_case_eq(val, "normal") || str_case_eq(val, "400")) {
            style->bold = false;
        }
    } else if (str_case_eq(prop, "font-style")) {
        if (str_case_eq(val, "italic") || str_case_eq(val, "oblique")) {
            style->italic = true;
        } else if (str_case_eq(val, "normal")) {
            style->italic = false;
        }
    } else if (str_case_eq(prop, "text-align")) {
        if (str_case_eq(val, "center")) {
            style->text_align = CSS_ALIGN_CENTER;
        } else if (str_case_eq(val, "right")) {
            style->text_align = CSS_ALIGN_RIGHT;
        } else {
            style->text_align = CSS_ALIGN_LEFT;
        }
    } else if (str_case_eq(prop, "display")) {
        if (str_case_eq(val, "block")) {
            style->display = CSS_DISPLAY_BLOCK;
        } else if (str_case_eq(val, "none")) {
            style->display = CSS_DISPLAY_NONE;
        } else {
            style->display = CSS_DISPLAY_INLINE;
        }
    } else if (str_case_eq(prop, "margin")) {
        int v1 = 0, v2 = 0;
        int d1 = parse_int(val, &v1);
        if (d1 > 0) {
            const char *sp = val + d1;
            while (*sp && (*sp == 'p' || *sp == 'x' || is_whitespace(*sp))) sp++;
            int d2 = parse_int(sp, &v2);
            if (d2 > 0) {
                style->margin_top = v1;
                style->margin_bottom = v1;
                style->margin_left = v2;
                style->margin_right = v2;
            } else {
                style->margin_top = v1;
                style->margin_bottom = v1;
                style->margin_left = v1;
                style->margin_right = v1;
            }
        }
    } else if (str_case_eq(prop, "margin-top")) {
        parse_int(val, &style->margin_top);
    } else if (str_case_eq(prop, "margin-bottom")) {
        parse_int(val, &style->margin_bottom);
    } else if (str_case_eq(prop, "margin-left")) {
        parse_int(val, &style->margin_left);
    } else if (str_case_eq(prop, "margin-right")) {
        parse_int(val, &style->margin_right);
    } else if (str_case_eq(prop, "padding")) {
        int v1 = 0, v2 = 0;
        int d1 = parse_int(val, &v1);
        if (d1 > 0) {
            const char *sp = val + d1;
            while (*sp && (*sp == 'p' || *sp == 'x' || is_whitespace(*sp))) sp++;
            int d2 = parse_int(sp, &v2);
            if (d2 > 0) {
                style->padding_top = v1;
                style->padding_bottom = v1;
                style->padding_left = v2;
                style->padding_right = v2;
            } else {
                style->padding_top = v1;
                style->padding_bottom = v1;
                style->padding_left = v1;
                style->padding_right = v1;
            }
        }
    } else if (str_case_eq(prop, "padding-top")) {
        parse_int(val, &style->padding_top);
    } else if (str_case_eq(prop, "padding-bottom")) {
        parse_int(val, &style->padding_bottom);
    } else if (str_case_eq(prop, "padding-left")) {
        parse_int(val, &style->padding_left);
    } else if (str_case_eq(prop, "padding-right")) {
        parse_int(val, &style->padding_right);
    } else if (str_case_eq(prop, "border")) {
        /* e.g. "1px solid #38bdf8" or "2px solid red" or "none" */
        if (str_case_eq(val, "none") || str_case_eq(val, "0")) {
            style->has_border = false;
            style->border_width = 0;
        } else {
            int bw = 1;
            int d = parse_int(val, &bw);
            if (d > 0) {
                style->border_width = (bw > 0) ? bw : 1;
                val += d;
            } else {
                style->border_width = 1;
            }
            while (*val && (*val == 'p' || *val == 'x' || is_whitespace(*val))) val++;
            if (str_case_starts_with(val, "solid")) val += 5;
            else if (str_case_starts_with(val, "dashed")) val += 6;
            else if (str_case_starts_with(val, "dotted")) val += 6;
            while (*val && is_whitespace(*val)) val++;
            if (*val) {
                style->border_color = css_parse_color(val);
            } else {
                style->border_color = 0x334155;
            }
            style->has_border = true;
        }
    } else if (str_case_eq(prop, "border-width")) {
        int bw = 0;
        if (parse_int(val, &bw) && bw > 0) {
            style->border_width = bw;
            style->has_border = true;
        }
    } else if (str_case_eq(prop, "border-color")) {
        style->border_color = css_parse_color(val);
        style->has_border = true;
    } else if (str_case_eq(prop, "width")) {
        if (str_case_eq(val, "100%") || str_case_eq(val, "auto")) {
            style->width = 0;
        } else {
            parse_int(val, &style->width);
        }
    }
}

static void css_parse_declarations(const char *decl_str, size_t len, css_style_t *style) {
    if (!decl_str || len == 0 || !style) return;

    size_t pos = 0;
    while (pos < len) {
        /* Skip whitespace and semicolons */
        while (pos < len && (is_whitespace(decl_str[pos]) || decl_str[pos] == ';')) pos++;
        if (pos >= len) break;

        /* Skip CSS comments */
        if (pos + 1 < len && decl_str[pos] == '/' && decl_str[pos+1] == '*') {
            pos += 2;
            while (pos + 1 < len && !(decl_str[pos] == '*' && decl_str[pos+1] == '/')) pos++;
            if (pos + 1 < len) pos += 2;
            continue;
        }

        /* Read property name */
        char prop[32] = {0};
        size_t pi = 0;
        while (pos < len && decl_str[pos] != ':' && decl_str[pos] != ';' && decl_str[pos] != '}' && pi < sizeof(prop) - 1) {
            if (!is_whitespace(decl_str[pos])) {
                prop[pi++] = to_lower(decl_str[pos]);
            }
            pos++;
        }
        prop[pi] = '\0';

        if (pos >= len || decl_str[pos] != ':') {
            /* Skip malformed token until next ';' */
            while (pos < len && decl_str[pos] != ';') pos++;
            continue;
        }
        pos++; /* Skip ':' */

        /* Read value */
        char val[64] = {0};
        size_t vi = 0;
        while (pos < len && decl_str[pos] != ';' && decl_str[pos] != '}' && vi < sizeof(val) - 1) {
            val[vi++] = decl_str[pos++];
        }
        /* Trim trailing whitespace from value */
        while (vi > 0 && is_whitespace(val[vi - 1])) vi--;
        val[vi] = '\0';

        if (prop[0] != '\0' && val[0] != '\0') {
            parse_property(prop, val, style);
        }

        if (pos < len && decl_str[pos] == ';') pos++;
    }
}

void css_parse_inline_style(const char *style_str, css_style_t *out_style) {
    if (!style_str || !out_style) return;
    css_parse_declarations(style_str, k_strlen(style_str), out_style);
}

void css_parse_stylesheet(const char *css, size_t len, css_stylesheet_t *sheet) {
    if (!css || len == 0 || !sheet) return;

    size_t pos = 0;
    while (pos < len && sheet->rule_count < CSS_MAX_RULES) {
        /* Skip whitespace and semicolons */
        while (pos < len && (is_whitespace(css[pos]) || css[pos] == ';')) pos++;
        if (pos >= len) break;

        /* Skip CSS comments */
        if (pos + 1 < len && css[pos] == '/' && css[pos+1] == '*') {
            pos += 2;
            while (pos + 1 < len && !(css[pos] == '*' && css[pos+1] == '/')) pos++;
            if (pos + 1 < len) pos += 2;
            continue;
        }

        /* Read selector up to '{' */
        char sel[CSS_MAX_SELECTOR_LEN] = {0};
        size_t si = 0;
        while (pos < len && css[pos] != '{' && si < sizeof(sel) - 1) {
            if (!is_whitespace(css[pos])) {
                sel[si++] = to_lower(css[pos]);
            }
            pos++;
        }
        sel[si] = '\0';

        if (pos >= len || css[pos] != '{') {
            /* Malformed: skip until next '{' or end */
            while (pos < len && css[pos] != '{') pos++;
            if (pos < len) pos++;
            continue;
        }
        pos++; /* Skip '{' */

        /* Extract declaration block up to '}' */
        size_t decl_start = pos;
        while (pos < len && css[pos] != '}') pos++;
        size_t decl_len = pos - decl_start;
        if (pos < len && css[pos] == '}') pos++;

        if (sel[0] == '\0') continue;

        /* Determine selector type and specificity */
        css_rule_t *rule = &sheet->rules[sheet->rule_count];
        css_init_style(&rule->style);

        const char *s_name = sel;
        if (sel[0] == '#') {
            rule->type = CSS_SELECTOR_ID;
            rule->specificity = 100;
            s_name = &sel[1];
        } else if (sel[0] == '.') {
            rule->type = CSS_SELECTOR_CLASS;
            rule->specificity = 10;
            s_name = &sel[1];
        } else if (sel[0] == '*') {
            rule->type = CSS_SELECTOR_UNIVERSAL;
            rule->specificity = 0;
            s_name = "";
        } else {
            rule->type = CSS_SELECTOR_TAG;
            rule->specificity = 1;
        }

        size_t ni = 0;
        while (s_name[ni] && ni < sizeof(rule->name) - 1) {
            rule->name[ni] = s_name[ni];
            ni++;
        }
        rule->name[ni] = '\0';

        /* Parse declaration body */
        css_parse_declarations(&css[decl_start], decl_len, &rule->style);
        sheet->rule_count++;
    }
}

void css_apply_style(css_style_t *dst, const css_style_t *src) {
    if (!dst || !src) return;

    if (src->has_color) {
        dst->color = src->color;
        dst->has_color = true;
    }
    if (src->has_bg_color) {
        dst->bg_color = src->bg_color;
        dst->has_bg_color = true;
    }
    if (src->font_scale > 0) {
        dst->font_scale = src->font_scale;
    }
    if (src->bold) {
        dst->bold = true;
    }
    if (src->italic) {
        dst->italic = true;
    }
    if (src->text_align != CSS_ALIGN_LEFT || src->display != CSS_DISPLAY_INLINE) {
        dst->text_align = src->text_align;
    }
    if (src->display != CSS_DISPLAY_INLINE) {
        dst->display = src->display;
    }

    if (src->margin_top) dst->margin_top = src->margin_top;
    if (src->margin_bottom) dst->margin_bottom = src->margin_bottom;
    if (src->margin_left) dst->margin_left = src->margin_left;
    if (src->margin_right) dst->margin_right = src->margin_right;

    if (src->padding_top) dst->padding_top = src->padding_top;
    if (src->padding_bottom) dst->padding_bottom = src->padding_bottom;
    if (src->padding_left) dst->padding_left = src->padding_left;
    if (src->padding_right) dst->padding_right = src->padding_right;

    if (src->has_border) {
        dst->has_border = true;
        dst->border_width = src->border_width;
        dst->border_color = src->border_color;
    }
    if (src->width > 0) {
        dst->width = src->width;
    }
}

bool css_self_test(void) {
    kprintf("[CSS TEST] Running Bounded CSS Engine Self-Test Suite...\n");

    /* 1. Test Color Parsing */
    kprintf("[CSS TEST] 1. Testing Color Parsing (hex, short-hex, named)...\n");
    if (css_parse_color("#ff0000") != 0xFF0000) {
        kprintf("[FAIL] #ff0000 parsing failed!\n");
        return false;
    }
    if (css_parse_color("#0f0") != 0x00FF00) {
        kprintf("[FAIL] #0f0 short hex parsing failed!\n");
        return false;
    }
    if (css_parse_color("blue") != 0x38BDF8) {
        kprintf("[FAIL] Named color 'blue' failed!\n");
        return false;
    }
    if (css_parse_color("white") != 0xFFFFFF) {
        kprintf("[FAIL] Named color 'white' failed!\n");
        return false;
    }
    kprintf("  PASS: Hex and named colors parsed accurately.\n");

    /* 2. Test Stylesheet Parsing: Tag, Class, ID, Universal */
    kprintf("[CSS TEST] 2. Testing Stylesheet Parsing & Selectors...\n");
    static const char test_css[] =
        "/* Test stylesheet */\n"
        "* { color: #ffffff; }\n"
        "p { color: #94a3b8; font-size: 16px; margin: 8px; }\n"
        ".card { background-color: #1e293b; border: 1px solid #38bdf8; padding: 10px; display: block; }\n"
        "#main { text-align: center; font-weight: bold; font-size: large; }\n"
        "/* Malformed noise */\n"
        "invalid { unknown-prop: 123; ;; }\n";

    static css_stylesheet_t sheet;
    kmemset(&sheet, 0, sizeof(sheet));
    css_parse_stylesheet(test_css, sizeof(test_css) - 1, &sheet);

    if (sheet.rule_count < 4) {
        kprintf("[FAIL] Expected at least 4 rules parsed, got %d!\n", sheet.rule_count);
        return false;
    }
    kprintf("  PASS: Stylesheet rules parsed (%d rules detected).\n", sheet.rule_count);
    kprintf("ASTRA_CSS_PARSER_OK\n");

    /* 3. Test Cascade and Specificity */
    kprintf("[CSS TEST] 3. Testing Cascade, Specificity & Inline Styles...\n");
    css_style_t node_style;
    css_init_style(&node_style);

    /* Apply tag rule (p) */
    for (int i = 0; i < sheet.rule_count; i++) {
        if (sheet.rules[i].type == CSS_SELECTOR_TAG && str_case_eq(sheet.rules[i].name, "p")) {
            css_apply_style(&node_style, &sheet.rules[i].style);
        }
    }
    if (!node_style.has_color || node_style.color != 0x94A3B8 || node_style.margin_top != 8) {
        kprintf("[FAIL] Tag rule application failed!\n");
        return false;
    }

    /* Apply class rule (.card overrides/augments) */
    for (int i = 0; i < sheet.rule_count; i++) {
        if (sheet.rules[i].type == CSS_SELECTOR_CLASS && str_case_eq(sheet.rules[i].name, "card")) {
            css_apply_style(&node_style, &sheet.rules[i].style);
        }
    }
    if (!node_style.has_bg_color || node_style.bg_color != 0x1E293B || !node_style.has_border ||
        node_style.padding_top != 10 || node_style.display != CSS_DISPLAY_BLOCK) {
        kprintf("[FAIL] Class rule application failed!\n");
        return false;
    }

    /* Apply ID rule (#main overrides font-size, sets bold and center) */
    for (int i = 0; i < sheet.rule_count; i++) {
        if (sheet.rules[i].type == CSS_SELECTOR_ID && str_case_eq(sheet.rules[i].name, "main")) {
            css_apply_style(&node_style, &sheet.rules[i].style);
        }
    }
    if (node_style.font_scale != 2 || !node_style.bold || node_style.text_align != CSS_ALIGN_CENTER) {
        kprintf("[FAIL] ID rule application failed!\n");
        return false;
    }

    /* Apply inline style: style="color: red; border: 2px solid green" (highest specificity) */
    css_style_t inline_style;
    css_init_style(&inline_style);
    css_parse_inline_style("color: red; border: 2px solid green", &inline_style);
    css_apply_style(&node_style, &inline_style);

    if (node_style.color != 0xEF4444 || node_style.border_width != 2 || node_style.border_color != 0x22C55E) {
        kprintf("[FAIL] Inline style override failed! Color=0x%06x\n", (unsigned)node_style.color);
        return false;
    }
    kprintf("  PASS: Cascade & Specificity verified: Tag < Class < ID < Inline.\n");
    kprintf("ASTRA_CSS_CASCADE_OK\n");

    /* 4. Malformed and Boundary Defense */
    kprintf("[CSS TEST] 4. Testing Malformed CSS Resilience & Boundary Limits...\n");
    static css_stylesheet_t mal_sheet;
    kmemset(&mal_sheet, 0, sizeof(mal_sheet));
    const char malformed[] = "{{{{ ;;; color:: red;;; border: ; width: 99999999px; -weird-unknown: 42; }}}}}";
    css_parse_stylesheet(malformed, sizeof(malformed) - 1, &mal_sheet);
    kprintf("  PASS: Malformed CSS handled cleanly without crash or infinite loop.\n");

    return true;
}
