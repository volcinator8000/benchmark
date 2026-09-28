#include "tmpl.h"
#include "stack.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PARTIAL_DEPTH 16
#define MAX_OUTPUT (4u * 1024u * 1024u)

typedef enum { N_TEXT, N_VAR, N_RAW, N_EACH, N_IF, N_UNLESS, N_PARTIAL } NType;

typedef struct Node Node;
typedef struct {
    Node  **items;
    size_t  len, cap;
} NodeList;

struct Node {
    NType    type;
    char    *text;   /* literal text, expression, or partial name */
    size_t   len;
    int      line;
    int      in_else; /* compile-time: currently filling alt branch */
    NodeList body;
    NodeList alt;
};

struct Tmpl {
    NodeList root;
};

/* ---------- node tree ---------- */

static void list_push(NodeList *l, Node *n) {
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = xrealloc(l->items, l->cap * sizeof *l->items);
    }
    l->items[l->len++] = n;
}

static void node_free(Node *n);

static void list_free(NodeList *l) {
    for (size_t i = 0; i < l->len; i++) node_free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->len = l->cap = 0;
}

static void node_free(Node *n) {
    if (!n) return;
    free(n->text);
    list_free(&n->body);
    list_free(&n->alt);
    free(n);
}

void tmpl_free(Tmpl *t) {
    if (!t) return;
    list_free(&t->root);
    free(t);
}

static Node *node_new(NType type, const char *text, size_t len, int line) {
    Node *n = xcalloc(1, sizeof *n);
    n->type = type;
    n->text = xstrndup(text, len);
    n->len = len;
    n->line = line;
    return n;
}

/* ---------- compiler ---------- */

static void trim(const char **s, size_t *n) {
    while (*n && isspace((unsigned char)**s)) { (*s)++; (*n)--; }
    while (*n && isspace((unsigned char)(*s)[*n - 1])) (*n)--;
}

static int starts_word(const char *s, size_t n, const char *w) {
    size_t wl = strlen(w);
    if (n < wl || memcmp(s, w, wl) != 0) return 0;
    return n == wl || isspace((unsigned char)s[wl]);
}

static const char *block_name(NType t) {
    switch (t) {
    case N_EACH: return "each";
    case N_IF: return "if";
    case N_UNLESS: return "unless";
    default: return "?";
    }
}

Tmpl *tmpl_compile(const char *src, char *err, size_t errlen) {
    Tmpl *t = xcalloc(1, sizeof *t);
    Stack open; /* stack of Node* for currently open blocks */
    stack_init(&open);
    if (err && errlen) err[0] = '\0';

    const char *p = src;
    int line = 1;

#define CUR_LIST() (stack_empty(&open) ? &t->root : \
    (((Node *)stack_peek(&open))->in_else ? &((Node *)stack_peek(&open))->alt \
                                          : &((Node *)stack_peek(&open))->body))
#define FAIL(...) do { if (err) snprintf(err, errlen, __VA_ARGS__); goto fail; } while (0)

    while (*p) {
        const char *tag = strstr(p, "{{");
        size_t textlen = tag ? (size_t)(tag - p) : strlen(p);
        if (textlen) {
            list_push(CUR_LIST(), node_new(N_TEXT, p, textlen, line));
            for (size_t i = 0; i < textlen; i++) if (p[i] == '\n') line++;
        }
        if (!tag) break;

        int raw = tag[2] == '{';
        const char *inner = tag + (raw ? 3 : 2);
        const char *close = strstr(inner, raw ? "}}}" : "}}");
        if (!close) FAIL("line %d: unclosed tag", line);
        const char *s = inner;
        size_t n = (size_t)(close - inner);
        int tag_line = line;
        for (size_t i = 0; i < n; i++) if (s[i] == '\n') line++;
        p = close + (raw ? 3 : 2);
        trim(&s, &n);

        if (raw) {
            if (!n) FAIL("line %d: empty {{{ }}}", tag_line);
            list_push(CUR_LIST(), node_new(N_RAW, s, n, tag_line));
            continue;
        }
        if (!n) FAIL("line %d: empty {{ }}", tag_line);

        char c = s[0];
        if (c == '!') continue; /* comment */

        if (c == '#') {
            s++; n--;
            NType bt;
            size_t kw;
            if (starts_word(s, n, "each")) { bt = N_EACH; kw = 4; }
            else if (starts_word(s, n, "if")) { bt = N_IF; kw = 2; }
            else if (starts_word(s, n, "unless")) { bt = N_UNLESS; kw = 6; }
            else FAIL("line %d: unknown block '{{#%.*s}}'", tag_line, (int)n, s);
            s += kw; n -= kw;
            trim(&s, &n);
            if (!n) FAIL("line %d: {{#%s}} needs an expression", tag_line, block_name(bt));
            Node *blk = node_new(bt, s, n, tag_line);
            list_push(CUR_LIST(), blk);
            stack_push(&open, blk);
            continue;
        }
        if (c == '/') {
            s++; n--;
            trim(&s, &n);
            Node *top = stack_pop(&open);
            if (!top) FAIL("line %d: '{{/%.*s}}' without open block", tag_line, (int)n, s);
            const char *want = block_name(top->type);
            if (strlen(want) != n || memcmp(want, s, n) != 0)
                FAIL("line %d: '{{/%.*s}}' closes '{{#%s}}' opened on line %d",
                     tag_line, (int)n, s, want, top->line);
            continue;
        }
        if (n == 4 && memcmp(s, "else", 4) == 0) {
            Node *top = stack_peek(&open);
            if (!top) FAIL("line %d: {{else}} outside a block", tag_line);
            if (top->in_else) FAIL("line %d: duplicate {{else}}", tag_line);
            top->in_else = 1;
            continue;
        }
        if (c == '>') {
            s++; n--;
            trim(&s, &n);
            if (!n) FAIL("line %d: partial needs a name", tag_line);
            list_push(CUR_LIST(), node_new(N_PARTIAL, s, n, tag_line));
            continue;
        }
        list_push(CUR_LIST(), node_new(N_VAR, s, n, tag_line));
    }

    if (!stack_empty(&open)) {
        Node *top = stack_peek(&open);
        FAIL("line %d: '{{#%s}}' is never closed", top->line, block_name(top->type));
    }
    stack_free(&open);
    return t;

fail:
    stack_free(&open);
    tmpl_free(t);
    return NULL;
#undef CUR_LIST
#undef FAIL
}

/* ---------- renderer ---------- */

typedef struct {
    Json       *val;
    char       *path;    /* owned */
    int         is_loop;
    size_t      index, count;
    const char *key;     /* borrowed, object iteration only */
} Frame;

typedef struct {
    Stack         scopes; /* Frame* ; bottom = root data */
    Json         *vars;
    TmplPartialFn partials;
    void         *ud;
    Buf          *out;
    int           depth;
    char         *err;
    size_t        errlen;
    int           failed;
} Ctx;

typedef struct {
    Json *v;
    int   owned;
    char *path; /* owned, may be NULL */
} Resolved;

static char *path_join(const char *base, const char *rest) {
    Buf b;
    buf_init(&b);
    if (base && *base) {
        buf_puts(&b, base);
        if (rest && *rest) buf_putc(&b, '.');
    }
    if (rest) buf_puts(&b, rest);
    return buf_steal(&b);
}

static Frame *loop_frame(Ctx *c, size_t skip) {
    for (size_t d = skip; d < stack_len(&c->scopes); d++) {
        Frame *f = stack_at(&c->scopes, d);
        if (f->is_loop) return f;
    }
    return NULL;
}

static Resolved resolve(Ctx *c, const char *expr) {
    Resolved r = { NULL, 0, NULL };
    size_t skip = 0;
    while (strncmp(expr, "../", 3) == 0) { skip++; expr += 3; }
    if (skip >= stack_len(&c->scopes)) return r;
    Frame *top = stack_at(&c->scopes, skip);

    if (strcmp(expr, ".") == 0 || strcmp(expr, "this") == 0 || *expr == '\0') {
        r.v = top->val;
        r.path = xstrdup(top->path);
        return r;
    }
    if (strncmp(expr, "this.", 5) == 0) {
        r.v = json_path_get(top->val, expr + 5);
        r.path = path_join(top->path, expr + 5);
        return r;
    }

    if (expr[0] == '@') {
        const char *name = expr + 1;
        Frame *lf = loop_frame(c, skip);
        if (strcmp(name, "path") == 0) {
            r.v = json_str(top->path); r.owned = 1; return r;
        }
        if (strcmp(name, "index") == 0) {
            if (lf) { r.v = json_num((double)lf->index); r.owned = 1; }
            return r;
        }
        if (strcmp(name, "count") == 0) {
            if (lf) { r.v = json_num((double)lf->count); r.owned = 1; }
            return r;
        }
        if (strcmp(name, "first") == 0) {
            if (lf) { r.v = json_bool(lf->index == 0); r.owned = 1; }
            return r;
        }
        if (strcmp(name, "last") == 0) {
            if (lf) { r.v = json_bool(lf->index + 1 == lf->count); r.owned = 1; }
            return r;
        }
        if (strcmp(name, "key") == 0) {
            if (lf && lf->key) { r.v = json_str(lf->key); r.owned = 1; }
            else if (lf) { r.v = json_num((double)lf->index); r.owned = 1; }
            return r;
        }
        if (strcmp(name, "root") == 0 || strncmp(name, "root.", 5) == 0) {
            Frame *root = stack_at(&c->scopes, stack_len(&c->scopes) - 1);
            const char *rest = name[4] ? name + 5 : "";
            r.v = json_path_get(root->val, rest);
            r.path = xstrdup(rest);
            return r;
        }
        r.v = c->vars ? json_path_get(c->vars, name) : NULL;
        return r;
    }

    /* Plain name: search current scope, then walk outwards. */
    size_t seglen = strcspn(expr, ".");
    for (size_t d = skip; d < stack_len(&c->scopes); d++) {
        Frame *f = stack_at(&c->scopes, d);
        if (!f->val || f->val->type != J_OBJ) continue;
        char *first = xstrndup(expr, seglen);
        Json *hit = json_get(f->val, first);
        free(first);
        if (hit) {
            r.v = json_path_get(f->val, expr);
            r.path = path_join(f->path, expr);
            return r;
        }
    }
    return r;
}

static void resolved_free(Resolved *r) {
    if (r->owned) json_free(r->v);
    free(r->path);
}

/* Adds a virtual `.length` suffix for lists, objects and strings. */
static Resolved resolve_expr(Ctx *c, const char *expr) {
    size_t n = strlen(expr);
    if (n > 7 && strcmp(expr + n - 7, ".length") == 0) {
        char *prefix = xstrndup(expr, n - 7);
        Resolved base = resolve(c, prefix);
        free(prefix);
        if (base.v && (base.v->type == J_ARR || base.v->type == J_OBJ || base.v->type == J_STR)) {
            size_t len = base.v->type == J_STR ? strlen(base.v->s) : base.v->len;
            resolved_free(&base);
            Resolved r = { json_num((double)len), 1, NULL };
            return r;
        }
        resolved_free(&base);
    }
    return resolve(c, expr);
}

static void render_list(Ctx *c, const NodeList *l);

static void rfail(Ctx *c, const char *msg, const Node *n) {
    if (c->failed) return;
    c->failed = 1;
    if (c->err) snprintf(c->err, c->errlen, "line %d: %s", n->line, msg);
}

static void push_frame(Ctx *c, Json *val, char *path, int is_loop,
                       size_t index, size_t count, const char *key) {
    Frame *f = xcalloc(1, sizeof *f);
    f->val = val;
    f->path = path;
    f->is_loop = is_loop;
    f->index = index;
    f->count = count;
    f->key = key;
    stack_push(&c->scopes, f);
}

static void pop_frame(Ctx *c) {
    Frame *f = stack_pop(&c->scopes);
    if (f) { free(f->path); free(f); }
}

static void render_node(Ctx *c, const Node *n) {
    if (c->failed) return;
    if (c->out->len > MAX_OUTPUT) { rfail(c, "output too large", n); return; }

    switch (n->type) {
    case N_TEXT:
        buf_append(c->out, n->text, n->len);
        break;

    case N_VAR:
    case N_RAW: {
        Resolved r = resolve_expr(c, n->text);
        if (r.v) {
            if (n->type == N_RAW) {
                json_write_scalar(r.v, c->out);
            } else {
                Buf tmp;
                buf_init(&tmp);
                json_write_scalar(r.v, &tmp);
                buf_html_escape(c->out, tmp.data ? tmp.data : "", tmp.len);
                buf_free(&tmp);
            }
        }
        resolved_free(&r);
        break;
    }

    case N_IF:
    case N_UNLESS: {
        Resolved r = resolve_expr(c, n->text);
        int t = json_truthy(r.v);
        resolved_free(&r);
        if (n->type == N_UNLESS) t = !t;
        render_list(c, t ? &n->body : &n->alt);
        break;
    }

    case N_EACH: {
        Resolved r = resolve_expr(c, n->text);
        Json *coll = r.v;
        if (!coll || (coll->type != J_ARR && coll->type != J_OBJ) || coll->len == 0) {
            resolved_free(&r);
            render_list(c, &n->alt);
            break;
        }
        /* Iterate over a clone-free snapshot of the count; the tree is not
         * mutated during rendering so borrowing is safe. */
        size_t count = coll->len;
        for (size_t i = 0; i < count && !c->failed; i++) {
            char seg[32];
            const char *key = NULL;
            char *path;
            if (coll->type == J_OBJ) {
                key = coll->keys[i];
                path = path_join(r.path, key);
            } else {
                snprintf(seg, sizeof seg, "%zu", i);
                path = path_join(r.path, seg);
            }
            push_frame(c, coll->items[i], path, 1, i, count, key);
            render_list(c, &n->body);
            pop_frame(c);
        }
        resolved_free(&r);
        break;
    }

    case N_PARTIAL: {
        if (c->depth >= MAX_PARTIAL_DEPTH) { rfail(c, "partials nested too deeply", n); break; }
        const Tmpl *pt = c->partials ? c->partials(n->text, c->ud) : NULL;
        if (!pt) {
            char msg[160];
            snprintf(msg, sizeof msg, "unknown partial '%s'", n->text);
            rfail(c, msg, n);
            break;
        }
        c->depth++;
        render_list(c, &pt->root);
        c->depth--;
        break;
    }
    }
}

static void render_list(Ctx *c, const NodeList *l) {
    for (size_t i = 0; i < l->len && !c->failed; i++) render_node(c, l->items[i]);
}

int tmpl_render(const Tmpl *t, Json *data, Json *vars,
                TmplPartialFn partials, void *ud,
                Buf *out, char *err, size_t errlen) {
    Ctx c;
    memset(&c, 0, sizeof c);
    stack_init(&c.scopes);
    c.vars = vars;
    c.partials = partials;
    c.ud = ud;
    c.out = out;
    c.err = err;
    c.errlen = errlen;
    if (err && errlen) err[0] = '\0';

    push_frame(&c, data, xstrdup(""), 0, 0, 0, NULL);
    render_list(&c, &t->root);
    while (!stack_empty(&c.scopes)) pop_frame(&c);
    stack_free(&c.scopes);
    return c.failed ? -1 : 0;
}
