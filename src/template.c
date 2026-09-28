#include "template.h"
#include "stack.h"
#include "util.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BLK_SECTION, BLK_INVERTED, BLK_IF, BLK_UNLESS };

/* Render budget: nested loops multiply, so a hostile or careless template
 * could otherwise run forever. */
#define MAX_STEPS  2000000   /* tags evaluated */
#define MAX_OUTPUT (8u * 1024u * 1024u)

typedef struct {
    int kind;
    char name[128];
    const char *body;     /* text position right after the opening tag */
    const Json *arr;      /* array being iterated, or NULL */
    size_t idx;
    int cond;             /* truthiness of the section value */
    int render;           /* whether this block's current branch is visible */
    int pushed_ctx;       /* whether the block pushed a context scope */
    int in_else;
} Block;

typedef struct {
    Stack *blocks;
    Stack *ctx;
    StrBuf out;
    const char *tpl;
    char *err;
    size_t errlen;
    int failed;
    size_t steps;
} Renderer;

static void fail(Renderer *r, const char *where, const char *msg) {
    if (!r->failed && r->err && r->errlen) {
        long line = 1;
        for (const char *p = r->tpl; p < where; p++) if (*p == '\n') line++;
        snprintf(r->err, r->errlen, "template error on line %ld: %s", line, msg);
    }
    r->failed = 1;
}

static int visible(const Renderer *r) {
    size_t n = stack_size(r->blocks);
    for (size_t i = 0; i < n; i++) {
        const Block *b = stack_at(r->blocks, i);
        if (!b->render) return 0;
    }
    return 1;
}

/* Visibility ignoring the top block (used when flipping {{else}}). */
static int parent_visible(const Renderer *r) {
    size_t n = stack_size(r->blocks);
    for (size_t i = 1; i < n; i++) {
        const Block *b = stack_at(r->blocks, i);
        if (!b->render) return 0;
    }
    return 1;
}

/* Looks up a name. `*tmp` receives a temporary Json the caller must free. */
static const Json *lookup(Renderer *r, const char *name, Json **tmp) {
    *tmp = NULL;
    if (strcmp(name, ".") == 0 || strcmp(name, "this") == 0)
        return stack_peek(r->ctx);

    if (name[0] == '@') {
        const char *p = name + 1;
        size_t skip = 0;
        while (str_starts_with(p, "../")) { skip++; p += 3; }
        size_t n = stack_size(r->blocks);
        const Block *found = NULL;
        for (size_t i = 0; i < n; i++) {
            const Block *b = stack_at(r->blocks, i);
            if (b->arr) {
                if (skip == 0) { found = b; break; }
                skip--;
            }
        }
        if (!found) return NULL;
        if (strcmp(p, "index") == 0) { *tmp = json_number((double)found->idx); return *tmp; }
        if (strcmp(p, "number") == 0) { *tmp = json_number((double)found->idx + 1); return *tmp; }
        if (strcmp(p, "first") == 0) { *tmp = json_bool(found->idx == 0); return *tmp; }
        if (strcmp(p, "last") == 0) { *tmp = json_bool(found->idx + 1 == json_len(found->arr)); return *tmp; }
        if (strcmp(p, "length") == 0) { *tmp = json_number((double)json_len(found->arr)); return *tmp; }
        return NULL;
    }

    /* First segment is resolved against the context stack, the rest as a path. */
    const char *rest = name;
    while (*rest && *rest != '.' && *rest != '[') rest++;
    size_t keylen = (size_t)(rest - name);
    if (keylen == 0) return NULL;

    const Json *base = NULL;
    size_t n = stack_size(r->ctx);
    for (size_t i = 0; i < n; i++) {
        const Json *c = stack_at(r->ctx, i);
        if (c && c->type == JSON_OBJECT) {
            Json *v = json_get_n(c, name, keylen);
            if (v) { base = v; break; }
        }
    }
    if (!base) return NULL;
    if (!*rest) return base;
    return json_path_get(base, rest);
}

static void emit_value(Renderer *r, const Json *v, int raw) {
    if (!v) return;
    char buf[64];
    char *owned = NULL;
    const char *s = NULL;
    switch (v->type) {
    case JSON_NULL: return;
    case JSON_BOOL: s = v->b ? "true" : "false"; break;
    case JSON_NUMBER:
        if (v->num == floor(v->num) && fabs(v->num) < 1e15) snprintf(buf, sizeof buf, "%.0f", v->num);
        else snprintf(buf, sizeof buf, "%.15g", v->num);
        s = buf;
        break;
    case JSON_STRING: s = v->str; break;
    case JSON_ARRAY:
    case JSON_OBJECT:
        owned = json_stringify(v, 0);
        s = owned;
        break;
    }
    if (raw) sb_append(&r->out, s);
    else sb_append_html_escaped(&r->out, s);
    free(owned);
}

static Block *push_block(Renderer *r, int kind, const char *name, const char *body) {
    Block *b = xcalloc(1, sizeof *b);
    b->kind = kind;
    snprintf(b->name, sizeof b->name, "%s", name);
    b->body = body;
    stack_push(r->blocks, b);
    return b;
}

static void open_section(Renderer *r, int kind, const char *name, const char *body) {
    int vis = visible(r);
    Block *b = push_block(r, kind, name, body);
    if (!vis) { b->render = 0; return; }

    Json *tmp = NULL;
    const Json *v = lookup(r, name, &tmp);
    int truthy = json_truthy(v);

    switch (kind) {
    case BLK_SECTION:
        b->cond = truthy;
        if (truthy && v->type == JSON_ARRAY) {
            b->arr = v;
            b->idx = 0;
            stack_push(r->ctx, (void *)json_at(v, 0));
            b->pushed_ctx = 1;
        } else if (truthy) {
            stack_push(r->ctx, (void *)v);
            b->pushed_ctx = 1;
        }
        b->render = truthy;
        break;
    case BLK_INVERTED:
    case BLK_UNLESS:
        b->cond = !truthy;
        b->render = b->cond;
        break;
    case BLK_IF:
        b->cond = truthy;
        b->render = truthy;
        break;
    }
    /* A temporary (e.g. {{#@first}}) must not be kept as a context. */
    if (tmp) {
        if (b->pushed_ctx) { stack_pop(r->ctx); b->pushed_ctx = 0; }
        json_free(tmp);
    }
}

/* Returns the new scan position. */
static const char *close_section(Renderer *r, const char *name, const char *after_tag, const char *tagpos) {
    Block *b = stack_peek(r->blocks);
    if (!b) { fail(r, tagpos, "closing tag without an open section"); return after_tag; }
    const char *expect = b->name;
    if (b->kind == BLK_IF) expect = "if";
    else if (b->kind == BLK_UNLESS) expect = "unless";
    if (strcmp(expect, name) != 0 && !(b->kind == BLK_SECTION && strcmp(name, "") == 0)) {
        char msg[256];
        snprintf(msg, sizeof msg, "{{/%s}} does not match open section {{#%s}}", name, expect);
        fail(r, tagpos, msg);
        return after_tag;
    }
    if (b->arr && b->idx + 1 < json_len(b->arr)) {
        b->idx++;
        b->in_else = 0;
        stack_pop(r->ctx);
        stack_push(r->ctx, (void *)json_at(b->arr, b->idx));
        b->render = 1;
        return b->body;
    }
    stack_pop(r->blocks);
    if (b->pushed_ctx) stack_pop(r->ctx);
    free(b);
    return after_tag;
}

static void handle_else(Renderer *r, const char *tagpos) {
    Block *b = stack_peek(r->blocks);
    if (!b) { fail(r, tagpos, "{{else}} outside of a section"); return; }
    if (b->in_else) { fail(r, tagpos, "duplicate {{else}}"); return; }
    b->in_else = 1;
    if (!parent_visible(r)) { b->render = 0; return; }
    if (b->arr) {
        /* Iterating a non-empty list: the else branch is skipped. The loop
         * only continues when the closing tag is reached, so leave the
         * iteration state alone. */
        b->render = 0;
        return;
    }
    b->render = !b->cond;
}

static void trim(const char **s, size_t *n) {
    while (*n && isspace((unsigned char)**s)) { (*s)++; (*n)--; }
    while (*n && isspace((unsigned char)(*s)[*n - 1])) (*n)--;
}

char *template_render_stack(const char *tpl, const Json *const *ctxs, size_t nctx,
                            char *err, size_t errlen) {
    Renderer r;
    memset(&r, 0, sizeof r);
    r.blocks = stack_new();
    r.ctx = stack_new();
    r.tpl = tpl;
    r.err = err;
    r.errlen = errlen;
    if (err && errlen) err[0] = '\0';
    sb_init(&r.out);
    for (size_t i = 0; i < nctx; i++) stack_push(r.ctx, (void *)ctxs[i]);

    const char *pos = tpl;
    while (*pos && !r.failed) {
        const char *open = strstr(pos, "{{");
        if (!open) {
            if (visible(&r)) sb_append(&r.out, pos);
            break;
        }
        if (visible(&r)) sb_appendn(&r.out, pos, (size_t)(open - pos));
        if (++r.steps > MAX_STEPS) { fail(&r, open, "render budget exceeded (too many iterations)"); break; }
        if (r.out.len > MAX_OUTPUT) { fail(&r, open, "render output too large"); break; }

        int triple = open[2] == '{';
        const char *inner = open + 2 + triple;
        const char *close = strstr(inner, triple ? "}}}" : "}}");
        if (!close) { fail(&r, open, "unclosed tag"); break; }
        size_t innerlen = (size_t)(close - inner);
        const char *after = close + (triple ? 3 : 2);
        trim(&inner, &innerlen);

        char tag[256];
        if (innerlen >= sizeof tag) { fail(&r, open, "tag too long"); break; }
        memcpy(tag, inner, innerlen);
        tag[innerlen] = '\0';

        if (triple) {
            if (visible(&r)) {
                Json *tmp = NULL;
                emit_value(&r, lookup(&r, tag, &tmp), 1);
                json_free(tmp);
            }
            pos = after;
            continue;
        }

        char sigil = tag[0];
        const char *arg = tag + 1;
        while (*arg == ' ') arg++;

        if (sigil == '!') {
            /* comment */
        } else if (sigil == '#') {
            if (str_starts_with(arg, "if ")) {
                const char *n = arg + 3; while (*n == ' ') n++;
                open_section(&r, BLK_IF, n, after);
            } else if (str_starts_with(arg, "unless ")) {
                const char *n = arg + 7; while (*n == ' ') n++;
                open_section(&r, BLK_UNLESS, n, after);
            } else if (str_starts_with(arg, "each ")) {
                const char *n = arg + 5; while (*n == ' ') n++;
                open_section(&r, BLK_SECTION, n, after);
            } else {
                open_section(&r, BLK_SECTION, arg, after);
            }
        } else if (sigil == '^') {
            open_section(&r, BLK_INVERTED, arg, after);
        } else if (sigil == '/') {
            if (strcmp(arg, "each") == 0) {
                Block *b = stack_peek(r.blocks);
                pos = close_section(&r, b ? b->name : "", after, open);
            } else {
                pos = close_section(&r, arg, after, open);
            }
            continue;
        } else if (strcmp(tag, "else") == 0) {
            handle_else(&r, open);
        } else if (sigil == '&') {
            if (visible(&r)) {
                Json *tmp = NULL;
                emit_value(&r, lookup(&r, arg, &tmp), 1);
                json_free(tmp);
            }
        } else if (sigil == '>') {
            fail(&r, open, "partials are not supported");
        } else {
            if (visible(&r)) {
                Json *tmp = NULL;
                emit_value(&r, lookup(&r, tag, &tmp), 0);
                json_free(tmp);
            }
        }
        pos = after;
    }

    if (!r.failed && !stack_is_empty(r.blocks)) {
        Block *b = stack_peek(r.blocks);
        char msg[256];
        snprintf(msg, sizeof msg, "unclosed section {{#%s}}",
                 b->kind == BLK_IF ? "if" : b->kind == BLK_UNLESS ? "unless" : b->name);
        fail(&r, b->body, msg);
    }

    stack_free_all(r.blocks, free);
    stack_free(r.ctx);
    if (r.failed) { sb_free(&r.out); return NULL; }
    return sb_detach(&r.out);
}

char *template_render(const char *tpl, const Json *ctx, char *err, size_t errlen) {
    const Json *ctxs[1] = { ctx };
    return template_render_stack(tpl, ctxs, ctx ? 1 : 0, err, errlen);
}

int template_validate(const char *tpl, char *err, size_t errlen) {
    Json *empty = json_object();
    char *out = template_render(tpl, empty, err, errlen);
    json_free(empty);
    if (!out) return -1;
    free(out);
    return 0;
}
