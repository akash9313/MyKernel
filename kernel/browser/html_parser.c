#include <kernel/browser.h>
#include <kernel/css.h>
#include <kernel/mem.h>

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

/* Parse HTML entities: returns number of chars consumed from src */
static int decode_entity(const char *src, char *out_char) {
    if (src[0] != '&') return 0;
    if (str_case_starts_with(src, "&amp;"))  { *out_char = '&'; return 5; }
    if (str_case_starts_with(src, "&lt;"))   { *out_char = '<'; return 4; }
    if (str_case_starts_with(src, "&gt;"))   { *out_char = '>'; return 4; }
    if (str_case_starts_with(src, "&quot;")) { *out_char = '"'; return 6; }
    if (str_case_starts_with(src, "&nbsp;")) { *out_char = ' '; return 6; }
    return 0;
}

static void compute_node_style(html_node_t *n, const css_stylesheet_t *sheet) {
    css_init_style(&n->computed_style);

    /* 1. Default user-agent styles based on tag */
    if (n->tag == HTML_TAG_H1) {
        n->computed_style.font_scale = 2;
        n->computed_style.bold = true;
        n->computed_style.color = GFX_COLOR_ACCENT_CYAN;
        n->computed_style.has_color = true;
        n->computed_style.display = CSS_DISPLAY_BLOCK;
        n->computed_style.margin_top = 8;
        n->computed_style.margin_bottom = 6;
    } else if (n->tag == HTML_TAG_H2) {
        n->computed_style.font_scale = 1;
        n->computed_style.bold = true;
        n->computed_style.color = GFX_COLOR_ACCENT_PURPLE;
        n->computed_style.has_color = true;
        n->computed_style.display = CSS_DISPLAY_BLOCK;
        n->computed_style.margin_top = 6;
        n->computed_style.margin_bottom = 4;
    } else if (n->tag == HTML_TAG_H3) {
        n->computed_style.font_scale = 1;
        n->computed_style.bold = true;
        n->computed_style.color = GFX_COLOR_ACCENT_BLUE;
        n->computed_style.has_color = true;
        n->computed_style.display = CSS_DISPLAY_BLOCK;
        n->computed_style.margin_top = 4;
        n->computed_style.margin_bottom = 2;
    } else if (n->tag == HTML_TAG_P || n->tag == HTML_TAG_DIV || n->tag == HTML_TAG_LI) {
        n->computed_style.color = GFX_COLOR_TEXT_PRIMARY;
        n->computed_style.has_color = true;
        n->computed_style.display = CSS_DISPLAY_BLOCK;
        n->computed_style.margin_bottom = 4;
    } else if (n->tag == HTML_TAG_A) {
        n->computed_style.color = GFX_COLOR_ACCENT_BLUE;
        n->computed_style.has_color = true;
        n->computed_style.display = CSS_DISPLAY_INLINE;
    } else {
        n->computed_style.color = GFX_COLOR_TEXT_PRIMARY;
        n->computed_style.has_color = true;
        n->computed_style.display = n->is_block ? CSS_DISPLAY_BLOCK : CSS_DISPLAY_INLINE;
    }

    if (n->bold) n->computed_style.bold = true;
    if (n->italic) n->computed_style.italic = true;

    if (!sheet) {
        css_apply_style(&n->computed_style, &n->inline_style);
        return;
    }

    /* 2. Tag name string */
    const char *tag_str = "";
    if (n->tag == HTML_TAG_H1) tag_str = "h1";
    else if (n->tag == HTML_TAG_H2) tag_str = "h2";
    else if (n->tag == HTML_TAG_H3) tag_str = "h3";
    else if (n->tag == HTML_TAG_P) tag_str = "p";
    else if (n->tag == HTML_TAG_A) tag_str = "a";
    else if (n->tag == HTML_TAG_DIV) tag_str = "div";
    else if (n->tag == HTML_TAG_SPAN) tag_str = "span";
    else if (n->tag == HTML_TAG_LI) tag_str = "li";
    else if (n->tag == HTML_TAG_HR) tag_str = "hr";

    /* 3. Universal rules (*) */
    for (int i = 0; i < sheet->rule_count; i++) {
        if (sheet->rules[i].type == CSS_SELECTOR_UNIVERSAL) {
            css_apply_style(&n->computed_style, &sheet->rules[i].style);
        }
    }

    /* 4. Tag rules */
    if (tag_str[0] != '\0') {
        for (int i = 0; i < sheet->rule_count; i++) {
            if (sheet->rules[i].type == CSS_SELECTOR_TAG && str_case_eq(sheet->rules[i].name, tag_str)) {
                css_apply_style(&n->computed_style, &sheet->rules[i].style);
            }
        }
    }

    /* 5. Class rules (.card, .alert, etc.) */
    if (n->class_name[0] != '\0') {
        for (int i = 0; i < sheet->rule_count; i++) {
            if (sheet->rules[i].type == CSS_SELECTOR_CLASS && str_case_eq(sheet->rules[i].name, n->class_name)) {
                css_apply_style(&n->computed_style, &sheet->rules[i].style);
            }
        }
    }

    /* 6. ID rules (#main, #status, etc.) */
    if (n->id[0] != '\0') {
        for (int i = 0; i < sheet->rule_count; i++) {
            if (sheet->rules[i].type == CSS_SELECTOR_ID && str_case_eq(sheet->rules[i].name, n->id)) {
                css_apply_style(&n->computed_style, &sheet->rules[i].style);
            }
        }
    }

    /* 7. Inline styles (specificity 1000) */
    css_apply_style(&n->computed_style, &n->inline_style);

    if (n->computed_style.bold) n->bold = true;
    if (n->computed_style.italic) n->italic = true;
    if (n->computed_style.display == CSS_DISPLAY_BLOCK) n->is_block = true;
    else if (n->computed_style.display == CSS_DISPLAY_INLINE) n->is_block = false;
}

int html_parse(const char *html, size_t len, html_node_t *nodes, int max_nodes, char *out_title, size_t title_max, css_stylesheet_t *sheet) {
    if (!html || !nodes || max_nodes <= 0) return 0;
    kmemset(nodes, 0, sizeof(html_node_t) * (size_t)max_nodes);
    if (sheet) {
        kmemset(sheet, 0, sizeof(css_stylesheet_t));
    }
    if (out_title && title_max > 0) {
        out_title[0] = '\0';
    }

    size_t pos = 0;
    int node_count = 0;

    bool cur_bold = false;
    bool cur_italic = false;
    bool in_link = false;
    bool in_title = false;
    bool in_script = false;
    bool in_style = false;

    char cur_href[128] = {0};
    char cur_class[CSS_MAX_CLASS_LEN] = {0};
    char cur_id[CSS_MAX_ID_LEN] = {0};
    char cur_inline_style[CSS_MAX_INLINE_STYLE] = {0};
    html_tag_type_t cur_block = HTML_TAG_NONE;

    /* Temporary buffer to collect embedded CSS stylesheet */
    static char css_buffer[4096];
    size_t css_len = 0;

    while (pos < len && node_count < max_nodes) {
        /* 1. Skip comments: <!-- ... --> */
        if (pos + 3 < len && html[pos] == '<' && html[pos+1] == '!' &&
            html[pos+2] == '-' && html[pos+3] == '-') {
            pos += 4;
            while (pos + 2 < len && !(html[pos] == '-' && html[pos+1] == '-' && html[pos+2] == '>')) {
                pos++;
            }
            if (pos + 2 < len) pos += 3;
            continue;
        }

        /* 2. Skip DOCTYPE: <!DOCTYPE ... > */
        if (pos + 8 < len && html[pos] == '<' && html[pos+1] == '!' &&
            to_lower(html[pos+2]) == 'd' && to_lower(html[pos+3]) == 'o' &&
            to_lower(html[pos+4]) == 'c') {
            while (pos < len && html[pos] != '>') pos++;
            if (pos < len) pos++;
            continue;
        }

        /* 3. HTML Tags: <...> */
        if (html[pos] == '<') {
            pos++;
            bool is_closing = false;
            if (pos < len && html[pos] == '/') {
                is_closing = true;
                pos++;
            }

            /* Extract Tag Name */
            char tag_name[16] = {0};
            size_t ti = 0;
            while (pos < len && !is_whitespace(html[pos]) && html[pos] != '>' && html[pos] != '/' && ti < sizeof(tag_name) - 1) {
                tag_name[ti++] = to_lower(html[pos++]);
            }
            tag_name[ti] = '\0';

            /* Parse attributes on opening tags: href, class, id, style */
            char attr_href[128] = {0};
            char attr_class[CSS_MAX_CLASS_LEN] = {0};
            char attr_id[CSS_MAX_ID_LEN] = {0};
            char attr_style[CSS_MAX_INLINE_STYLE] = {0};

            if (!is_closing) {
                while (pos < len && html[pos] != '>') {
                    while (pos < len && is_whitespace(html[pos])) pos++;
                    if (pos >= len || html[pos] == '>' || html[pos] == '/') break;

                    /* Read attribute name */
                    char aname[32] = {0};
                    size_t ai = 0;
                    while (pos < len && !is_whitespace(html[pos]) && html[pos] != '=' && html[pos] != '>' && html[pos] != '/' && ai < sizeof(aname) - 1) {
                        aname[ai++] = to_lower(html[pos++]);
                    }
                    aname[ai] = '\0';

                    while (pos < len && is_whitespace(html[pos])) pos++;
                    if (pos < len && html[pos] == '=') {
                        pos++;
                        while (pos < len && is_whitespace(html[pos])) pos++;

                        char quote = 0;
                        if (pos < len && (html[pos] == '"' || html[pos] == '\'')) {
                            quote = html[pos++];
                        }

                        char aval[256] = {0};
                        size_t avi = 0;
                        while (pos < len && avi < sizeof(aval) - 1) {
                            if (quote && html[pos] == quote) { pos++; break; }
                            if (!quote && (is_whitespace(html[pos]) || html[pos] == '>')) break;
                            aval[avi++] = html[pos++];
                        }
                        aval[avi] = '\0';

                        if (str_case_eq(aname, "href")) {
                            for (size_t k = 0; aval[k] && k < sizeof(attr_href) - 1; k++) attr_href[k] = aval[k];
                        } else if (str_case_eq(aname, "class")) {
                            for (size_t k = 0; aval[k] && k < sizeof(attr_class) - 1; k++) attr_class[k] = aval[k];
                        } else if (str_case_eq(aname, "id")) {
                            for (size_t k = 0; aval[k] && k < sizeof(attr_id) - 1; k++) attr_id[k] = aval[k];
                        } else if (str_case_eq(aname, "style")) {
                            for (size_t k = 0; aval[k] && k < sizeof(attr_style) - 1; k++) attr_style[k] = aval[k];
                        }
                    }
                }
            }

            /* Advance past closing '>' */
            while (pos < len && html[pos] != '>') pos++;
            if (pos < len) pos++;

            /* Process Tag Semantics */
            if (str_case_eq(tag_name, "script")) {
                in_script = !is_closing;
            } else if (in_script) {
                continue;
            } else if (str_case_eq(tag_name, "style")) {
                if (!is_closing) {
                    in_style = true;
                    css_len = 0;
                } else {
                    in_style = false;
                    if (sheet && css_len > 0) {
                        css_parse_stylesheet(css_buffer, css_len, sheet);
                    }
                }
            } else if (str_case_eq(tag_name, "title")) {
                in_title = !is_closing;
            } else if (str_case_eq(tag_name, "b") || str_case_eq(tag_name, "strong")) {
                cur_bold = !is_closing;
            } else if (str_case_eq(tag_name, "i") || str_case_eq(tag_name, "em")) {
                cur_italic = !is_closing;
            } else if (str_case_eq(tag_name, "a")) {
                if (!is_closing) {
                    in_link = true;
                    for (size_t k = 0; attr_href[k] && k < sizeof(cur_href) - 1; k++) cur_href[k] = attr_href[k];
                    for (size_t k = 0; attr_class[k] && k < sizeof(cur_class) - 1; k++) cur_class[k] = attr_class[k];
                    for (size_t k = 0; attr_id[k] && k < sizeof(cur_id) - 1; k++) cur_id[k] = attr_id[k];
                    for (size_t k = 0; attr_style[k] && k < sizeof(cur_inline_style) - 1; k++) cur_inline_style[k] = attr_style[k];
                } else {
                    in_link = false;
                    cur_href[0] = '\0';
                    cur_class[0] = '\0';
                    cur_id[0] = '\0';
                    cur_inline_style[0] = '\0';
                }
            } else if (str_case_eq(tag_name, "h1") || str_case_eq(tag_name, "h2") ||
                       str_case_eq(tag_name, "h3") || str_case_eq(tag_name, "p") ||
                       str_case_eq(tag_name, "div") || str_case_eq(tag_name, "li") ||
                       str_case_eq(tag_name, "span")) {
                if (!is_closing) {
                    if (str_case_eq(tag_name, "h1")) cur_block = HTML_TAG_H1;
                    else if (str_case_eq(tag_name, "h2")) cur_block = HTML_TAG_H2;
                    else if (str_case_eq(tag_name, "h3")) cur_block = HTML_TAG_H3;
                    else if (str_case_eq(tag_name, "p"))  cur_block = HTML_TAG_P;
                    else if (str_case_eq(tag_name, "div")) cur_block = HTML_TAG_DIV;
                    else if (str_case_eq(tag_name, "li"))  cur_block = HTML_TAG_LI;
                    else if (str_case_eq(tag_name, "span")) cur_block = HTML_TAG_SPAN;

                    for (size_t k = 0; attr_class[k] && k < sizeof(cur_class) - 1; k++) cur_class[k] = attr_class[k];
                    for (size_t k = 0; attr_id[k] && k < sizeof(cur_id) - 1; k++) cur_id[k] = attr_id[k];
                    for (size_t k = 0; attr_style[k] && k < sizeof(cur_inline_style) - 1; k++) cur_inline_style[k] = attr_style[k];

                    /* If opening block (div/p/h1) has background/border or fixed dimensions, generate block container node */
                    if ((str_case_eq(tag_name, "div") || str_case_eq(tag_name, "p")) &&
                        (attr_class[0] != '\0' || attr_id[0] != '\0' || attr_style[0] != '\0')) {
                        if (node_count < max_nodes) {
                            html_node_t *bn = &nodes[node_count++];
                            bn->tag = cur_block;
                            bn->is_block = true;
                            for (size_t k = 0; attr_class[k] && k < sizeof(bn->class_name) - 1; k++) bn->class_name[k] = attr_class[k];
                            for (size_t k = 0; attr_id[k] && k < sizeof(bn->id) - 1; k++) bn->id[k] = attr_id[k];
                            if (attr_style[0] != '\0') {
                                css_parse_inline_style(attr_style, &bn->inline_style);
                            }
                            compute_node_style(bn, sheet);
                        }
                    }
                } else {
                    cur_block = HTML_TAG_NONE;
                    cur_class[0] = '\0';
                    cur_id[0] = '\0';
                    cur_inline_style[0] = '\0';
                }
            } else if (str_case_eq(tag_name, "hr")) {
                if (node_count < max_nodes) {
                    html_node_t *n = &nodes[node_count++];
                    n->tag = HTML_TAG_HR;
                    n->is_block = true;
                    compute_node_style(n, sheet);
                }
            } else if (str_case_eq(tag_name, "br")) {
                if (node_count < max_nodes) {
                    html_node_t *n = &nodes[node_count++];
                    n->tag = HTML_TAG_BR;
                    n->is_block = true;
                    compute_node_style(n, sheet);
                }
            }
            continue;
        }

        /* 4. Text Content Processing */
        if (in_script) {
            pos++;
            continue;
        }

        if (in_style) {
            /* Collect CSS stylesheet text */
            if (css_len < sizeof(css_buffer) - 1) {
                css_buffer[css_len++] = html[pos++];
                css_buffer[css_len] = '\0';
            } else {
                pos++;
            }
            continue;
        }

        /* Collect text run up to next '<' */
        char text_buf[128] = {0};
        size_t ti = 0;
        bool last_space = false;

        while (pos < len && html[pos] != '<' && ti < sizeof(text_buf) - 1) {
            if (is_whitespace(html[pos])) {
                if (!last_space && ti > 0) {
                    text_buf[ti++] = ' ';
                    last_space = true;
                }
                pos++;
            } else if (html[pos] == '&') {
                char decoded = 0;
                int consumed = decode_entity(&html[pos], &decoded);
                if (consumed > 0) {
                    text_buf[ti++] = decoded;
                    pos += (size_t)consumed;
                    last_space = (decoded == ' ');
                } else {
                    text_buf[ti++] = html[pos++];
                    last_space = false;
                }
            } else {
                text_buf[ti++] = html[pos++];
                last_space = false;
            }
        }
        text_buf[ti] = '\0';

        /* If collecting title */
        if (in_title && out_title && title_max > 0) {
            size_t oi = 0;
            while (out_title[oi] && oi < title_max - 1) oi++;
            size_t ki = 0;
            while (text_buf[ki] && oi < title_max - 1) {
                out_title[oi++] = text_buf[ki++];
            }
            out_title[oi] = '\0';
            continue;
        }

        /* Discard empty or whitespace-only text runs if no meaningful content */
        if (ti == 0 || (ti == 1 && text_buf[0] == ' ')) {
            continue;
        }

        /* Create a DOM node */
        if (node_count < max_nodes) {
            html_node_t *n = &nodes[node_count++];
            n->tag = (cur_block != HTML_TAG_NONE) ? cur_block : (in_link ? HTML_TAG_A : HTML_TAG_TEXT);
            n->bold = cur_bold;
            n->italic = cur_italic;
            n->is_block = (cur_block == HTML_TAG_H1 || cur_block == HTML_TAG_H2 ||
                           cur_block == HTML_TAG_H3 || cur_block == HTML_TAG_P ||
                           cur_block == HTML_TAG_DIV || cur_block == HTML_TAG_LI);

            for (size_t k = 0; text_buf[k] && k < sizeof(n->text) - 1; k++) {
                n->text[k] = text_buf[k];
            }
            if (in_link) {
                for (size_t k = 0; cur_href[k] && k < sizeof(n->href) - 1; k++) {
                    n->href[k] = cur_href[k];
                }
            }
            for (size_t k = 0; cur_class[k] && k < sizeof(n->class_name) - 1; k++) {
                n->class_name[k] = cur_class[k];
            }
            for (size_t k = 0; cur_id[k] && k < sizeof(n->id) - 1; k++) {
                n->id[k] = cur_id[k];
            }
            if (cur_inline_style[0] != '\0') {
                css_parse_inline_style(cur_inline_style, &n->inline_style);
            }

            compute_node_style(n, sheet);
        }
    }

    return node_count;
}
