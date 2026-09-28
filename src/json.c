#define _POSIX_C_SOURCE 200809L
#include "json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

/* ---------- constructors ---------- */

static Json *jnew(JType t) {
    Json *v = xcalloc(1, sizeof *v);
    v->type = t;
    return v;
}

Json *json_null(void) { return jnew(J_NULL); }
Json *json_bool(int b) { Json *v = jnew(J_BOOL); v->b = !!b; return v; }
Json *json_num(double n) { Json *v = jnew(J_NUM); v->n = n; return v; }
Json *json_str(const char *s) { Json *v = jnew(J_STR); v->s = xstrdup(s ? s : ""); return v; }
Json *json_arr(void) { return jnew(J_ARR); }
Json *json_obj(void) { return jnew(J_OBJ); }

void json_free(Json *v) {
    if (!v) return;
    for (size_t i = 0; i < v->len; i++) {
        json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->s);
    free(v);
}

static void grow(Json *v) {
    if (v->len < v->cap) return;
    v->cap = v->cap ? v->cap * 2 : 4;
    v->items = xrealloc(v->items, v->cap * sizeof *v->items);
    if (v->type == J_OBJ) v->keys = xrealloc(v->keys, v->cap * sizeof *v->keys);
}

void json_arr_push(Json *arr, Json *v) {
    grow(arr);
    arr->items[arr->len++] = v;
}

void json_arr_insert(Json *arr, size_t i, Json *v) {
    if (i > arr->len) i = arr->len;
    grow(arr);
    memmove(arr->items + i + 1, arr->items + i, (arr->len - i) * sizeof *arr->items);
    arr->items[i] = v;
    arr->len++;
}

Json *json_arr_take(Json *arr, size_t i) {
    if (arr->type != J_ARR || i >= arr->len) return NULL;
    Json *v = arr->items[i];
    memmove(arr->items + i, arr->items + i + 1, (arr->len - i - 1) * sizeof *arr->items);
    arr->len--;
    return v;
}

Json *json_get(const Json *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (size_t i = 0; i < obj->len; i++)
        if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    return NULL;
}

const char *json_get_str(const Json *obj, const char *key) {
    Json *v = json_get(obj, key);
    return (v && v->type == J_STR) ? v->s : NULL;
}

void json_set(Json *obj, const char *key, Json *v) {
    for (size_t i = 0; i < obj->len; i++) {
        if (strcmp(obj->keys[i], key) == 0) {
            json_free(obj->items[i]);
            obj->items[i] = v;
            return;
        }
    }
    grow(obj);
    obj->keys[obj->len] = xstrdup(key);
    obj->items[obj->len] = v;
    obj->len++;
}

int json_del(Json *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return 0;
    for (size_t i = 0; i < obj->len; i++) {
        if (strcmp(obj->keys[i], key) == 0) {
            json_free(obj->items[i]);
            free(obj->keys[i]);
            size_t rest = obj->len - i - 1;
            memmove(obj->items + i, obj->items + i + 1, rest * sizeof *obj->items);
            memmove(obj->keys + i, obj->keys + i + 1, rest * sizeof *obj->keys);
            obj->len--;
            return 1;
        }
    }
    return 0;
}

Json *json_clone(const Json *v) {
    if (!v) return NULL;
    Json *c = jnew(v->type);
    c->b = v->b;
    c->n = v->n;
    if (v->s) c->s = xstrdup(v->s);
    for (size_t i = 0; i < v->len; i++) {
        if (v->type == J_OBJ) json_set(c, v->keys[i], json_clone(v->items[i]));
        else json_arr_push(c, json_clone(v->items[i]));
    }
    return c;
}

/* ---------- parser ---------- */

typedef struct {
    const char *p, *end, *start;
    char *err;
    size_t errlen;
    int failed;
} Parser;

static void perr(Parser *ps, const char *msg) {
    if (ps->failed) return;
    ps->failed = 1;
    if (ps->err) snprintf(ps->err, ps->errlen, "%s at offset %ld", msg, (long)(ps->p - ps->start));
}

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int hex4(Parser *ps, unsigned *out) {
    if (ps->end - ps->p < 4) return -1;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = ps->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    ps->p += 4;
    *out = v;
    return 0;
}

static void put_utf8(Buf *b, unsigned cp) {
    char t[4];
    if (cp < 0x80) { t[0] = (char)cp; buf_append(b, t, 1); }
    else if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F));
        buf_append(b, t, 2);
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        buf_append(b, t, 3);
    } else {
        t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F));
        buf_append(b, t, 4);
    }
}

static char *parse_string_raw(Parser *ps) {
    /* assumes *p == '"' */
    ps->p++;
    Buf b;
    buf_init(&b);
    while (ps->p < ps->end) {
        char c = *ps->p++;
        if (c == '"') return buf_steal(&b);
        if ((unsigned char)c < 0x20) { perr(ps, "control character in string"); break; }
        if (c != '\\') { buf_putc(&b, c); continue; }
        if (ps->p >= ps->end) break;
        char e = *ps->p++;
        switch (e) {
        case '"': buf_putc(&b, '"'); break;
        case '\\': buf_putc(&b, '\\'); break;
        case '/': buf_putc(&b, '/'); break;
        case 'b': buf_putc(&b, '\b'); break;
        case 'f': buf_putc(&b, '\f'); break;
        case 'n': buf_putc(&b, '\n'); break;
        case 'r': buf_putc(&b, '\r'); break;
        case 't': buf_putc(&b, '\t'); break;
        case 'u': {
            unsigned cp;
            if (hex4(ps, &cp) < 0) { perr(ps, "bad \\u escape"); goto fail; }
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                unsigned lo;
                if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    ps->p += 2;
                    if (hex4(ps, &lo) < 0 || lo < 0xDC00 || lo > 0xDFFF) {
                        perr(ps, "bad surrogate pair"); goto fail;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            if (cp == 0) cp = 0xFFFD; /* keep strings NUL-free */
            put_utf8(&b, cp);
            break;
        }
        default: perr(ps, "bad escape"); goto fail;
        }
    }
    perr(ps, "unterminated string");
fail:
    buf_free(&b);
    return NULL;
}

static Json *parse_value(Parser *ps, int depth);

static Json *parse_value(Parser *ps, int depth) {
    if (depth > JSON_MAX_DEPTH) { perr(ps, "nesting too deep"); return NULL; }
    skip_ws(ps);
    if (ps->p >= ps->end) { perr(ps, "unexpected end"); return NULL; }
    char c = *ps->p;

    if (c == '{') {
        ps->p++;
        Json *o = json_obj();
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == '}') { ps->p++; return o; }
        for (;;) {
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != '"') { perr(ps, "expected key"); break; }
            char *k = parse_string_raw(ps);
            if (!k) break;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') { free(k); perr(ps, "expected ':'"); break; }
            ps->p++;
            Json *v = parse_value(ps, depth + 1);
            if (!v) { free(k); break; }
            json_set(o, k, v);
            free(k);
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == '}') { ps->p++; return o; }
            perr(ps, "expected ',' or '}'");
            break;
        }
        json_free(o);
        return NULL;
    }
    if (c == '[') {
        ps->p++;
        Json *a = json_arr();
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ']') { ps->p++; return a; }
        for (;;) {
            Json *v = parse_value(ps, depth + 1);
            if (!v) break;
            json_arr_push(a, v);
            skip_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == ']') { ps->p++; return a; }
            perr(ps, "expected ',' or ']'");
            break;
        }
        json_free(a);
        return NULL;
    }
    if (c == '"') {
        char *s = parse_string_raw(ps);
        if (!s) return NULL;
        Json *v = jnew(J_STR);
        v->s = s;
        return v;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char tmp[64];
        size_t n = 0;
        while (ps->p + n < ps->end && n < sizeof tmp - 1 &&
               strchr("+-0123456789.eE", ps->p[n]))
            n++;
        memcpy(tmp, ps->p, n);
        tmp[n] = '\0';
        char *endp;
        double d = strtod(tmp, &endp);
        if (endp == tmp || (size_t)(endp - tmp) != n || !isfinite(d)) {
            perr(ps, "bad number");
            return NULL;
        }
        ps->p += n;
        return json_num(d);
    }
    if (ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) { ps->p += 4; return json_bool(1); }
    if (ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) { ps->p += 5; return json_bool(0); }
    if (ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) { ps->p += 4; return json_null(); }
    perr(ps, "unexpected character");
    return NULL;
}

Json *json_parse(const char *src, size_t len, char *err, size_t errlen) {
    Parser ps = { src, src + len, src, err, errlen, 0 };
    if (err && errlen) err[0] = '\0';
    Json *v = parse_value(&ps, 0);
    if (!v) return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) {
        perr(&ps, "trailing characters");
        json_free(v);
        return NULL;
    }
    return v;
}

/* ---------- serializer ---------- */

static void dump_string(const char *s, Buf *out) {
    buf_putc(out, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': buf_puts(out, "\\\""); break;
        case '\\': buf_puts(out, "\\\\"); break;
        case '\n': buf_puts(out, "\\n"); break;
        case '\r': buf_puts(out, "\\r"); break;
        case '\t': buf_puts(out, "\\t"); break;
        case '<': buf_puts(out, "\\u003c"); break; /* safe inside <script> */
        default:
            if (*p < 0x20) buf_printf(out, "\\u%04x", *p);
            else buf_putc(out, (char)*p);
        }
    }
    buf_putc(out, '"');
}

static void dump_num(double n, Buf *out) {
    if (n == (double)(long long)n && fabs(n) < 1e15) buf_printf(out, "%lld", (long long)n);
    else buf_printf(out, "%.17g", n);
}

static void newline(Buf *out, int indent, int level) {
    if (!indent) return;
    buf_putc(out, '\n');
    for (int i = 0; i < indent * level; i++) buf_putc(out, ' ');
}

static void dump(const Json *v, Buf *out, int indent, int level) {
    switch (v->type) {
    case J_NULL: buf_puts(out, "null"); break;
    case J_BOOL: buf_puts(out, v->b ? "true" : "false"); break;
    case J_NUM: dump_num(v->n, out); break;
    case J_STR: dump_string(v->s, out); break;
    case J_ARR:
    case J_OBJ: {
        int obj = v->type == J_OBJ;
        buf_putc(out, obj ? '{' : '[');
        for (size_t i = 0; i < v->len; i++) {
            if (i) buf_putc(out, ',');
            newline(out, indent, level + 1);
            if (obj) {
                dump_string(v->keys[i], out);
                buf_puts(out, indent ? ": " : ":");
            }
            dump(v->items[i], out, indent, level + 1);
        }
        if (v->len) newline(out, indent, level);
        buf_putc(out, obj ? '}' : ']');
        break;
    }
    }
}

void json_dump(const Json *v, Buf *out, int indent) {
    if (!v) { buf_puts(out, "null"); return; }
    dump(v, out, indent, 0);
}

char *json_to_string(const Json *v, int indent) {
    Buf b;
    buf_init(&b);
    json_dump(v, &b, indent);
    return buf_steal(&b);
}

/* ---------- helpers ---------- */

int json_is_index(const char *seg, size_t *out) {
    if (!*seg) return 0;
    size_t n = 0;
    for (const char *p = seg; *p; p++) {
        if (!isdigit((unsigned char)*p)) return 0;
        n = n * 10 + (size_t)(*p - '0');
        if (n > 100000000) return 0;
    }
    *out = n;
    return 1;
}

static Json *child(Json *v, const char *seg) {
    if (!v) return NULL;
    if (v->type == J_OBJ) return json_get(v, seg);
    if (v->type == J_ARR) {
        size_t i;
        if (json_is_index(seg, &i) && i < v->len) return v->items[i];
    }
    return NULL;
}

Json *json_path_get(Json *root, const char *path) {
    if (!path || !*path) return root;
    char *copy = xstrdup(path);
    Json *cur = root;
    char *save = NULL;
    for (char *seg = strtok_r(copy, ".", &save); seg && cur; seg = strtok_r(NULL, ".", &save))
        cur = child(cur, seg);
    free(copy);
    return cur;
}

Json *json_path_parent(Json *root, const char *path, char **last, char **scratch) {
    *last = NULL;
    *scratch = NULL;
    if (!path || !*path) return NULL;
    char *copy = xstrdup(path);
    char *dot = strrchr(copy, '.');
    Json *parent;
    if (dot) {
        *dot = '\0';
        parent = json_path_get(root, copy);
        *last = dot + 1;
    } else {
        parent = root;
        *last = copy;
    }
    *scratch = copy;
    return parent;
}

int json_truthy(const Json *v) {
    if (!v) return 0;
    switch (v->type) {
    case J_NULL: return 0;
    case J_BOOL: return v->b;
    case J_NUM: return v->n != 0;
    case J_STR: return v->s[0] != '\0';
    case J_ARR:
    case J_OBJ: return v->len > 0;
    }
    return 0;
}

void json_write_scalar(const Json *v, Buf *out) {
    if (!v) return;
    switch (v->type) {
    case J_NULL: break;
    case J_BOOL: buf_puts(out, v->b ? "true" : "false"); break;
    case J_NUM: dump_num(v->n, out); break;
    case J_STR: buf_puts(out, v->s); break;
    case J_ARR:
    case J_OBJ: json_dump(v, out, 0); break;
    }
}
