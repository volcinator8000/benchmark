#include "json.h"
#include "util.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 200

static Json *json_alloc(JsonType t) {
    Json *j = xcalloc(1, sizeof *j);
    j->type = t;
    return j;
}

Json *json_null(void) { return json_alloc(JSON_NULL); }
Json *json_bool(int b) { Json *j = json_alloc(JSON_BOOL); j->b = !!b; return j; }
Json *json_number(double n) { Json *j = json_alloc(JSON_NUMBER); j->num = n; return j; }
Json *json_string(const char *s) { Json *j = json_alloc(JSON_STRING); j->str = xstrdup(s); return j; }
Json *json_string_n(const char *s, size_t n) { Json *j = json_alloc(JSON_STRING); j->str = xstrndup(s, n); return j; }
Json *json_array(void) { return json_alloc(JSON_ARRAY); }
Json *json_object(void) { return json_alloc(JSON_OBJECT); }

void json_free(Json *j) {
    if (!j) return;
    switch (j->type) {
    case JSON_STRING:
        free(j->str);
        break;
    case JSON_ARRAY:
        for (size_t i = 0; i < j->arr.len; i++) json_free(j->arr.items[i]);
        free(j->arr.items);
        break;
    case JSON_OBJECT:
        for (size_t i = 0; i < j->obj.len; i++) {
            free(j->obj.keys[i]);
            json_free(j->obj.vals[i]);
        }
        free(j->obj.keys);
        free(j->obj.vals);
        break;
    default:
        break;
    }
    free(j);
}

Json *json_clone(const Json *j) {
    if (!j) return NULL;
    switch (j->type) {
    case JSON_NULL: return json_null();
    case JSON_BOOL: return json_bool(j->b);
    case JSON_NUMBER: return json_number(j->num);
    case JSON_STRING: return json_string(j->str);
    case JSON_ARRAY: {
        Json *a = json_array();
        for (size_t i = 0; i < j->arr.len; i++) json_push(a, json_clone(j->arr.items[i]));
        return a;
    }
    case JSON_OBJECT: {
        Json *o = json_object();
        for (size_t i = 0; i < j->obj.len; i++) json_set(o, j->obj.keys[i], json_clone(j->obj.vals[i]));
        return o;
    }
    }
    return NULL;
}

/* ---------- objects ---------- */

Json *json_get_n(const Json *obj, const char *key, size_t keylen) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->obj.len; i++)
        if (strlen(obj->obj.keys[i]) == keylen && memcmp(obj->obj.keys[i], key, keylen) == 0)
            return obj->obj.vals[i];
    return NULL;
}

Json *json_get(const Json *obj, const char *key) {
    return json_get_n(obj, key, strlen(key));
}

void json_set(Json *obj, const char *key, Json *val) {
    if (!obj || obj->type != JSON_OBJECT) { json_free(val); return; }
    if (!val) val = json_null();
    for (size_t i = 0; i < obj->obj.len; i++) {
        if (strcmp(obj->obj.keys[i], key) == 0) {
            json_free(obj->obj.vals[i]);
            obj->obj.vals[i] = val;
            return;
        }
    }
    if (obj->obj.len == obj->obj.cap) {
        obj->obj.cap = obj->obj.cap ? obj->obj.cap * 2 : 8;
        obj->obj.keys = xrealloc(obj->obj.keys, obj->obj.cap * sizeof *obj->obj.keys);
        obj->obj.vals = xrealloc(obj->obj.vals, obj->obj.cap * sizeof *obj->obj.vals);
    }
    obj->obj.keys[obj->obj.len] = xstrdup(key);
    obj->obj.vals[obj->obj.len] = val;
    obj->obj.len++;
}

Json *json_detach_key(Json *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->obj.len; i++) {
        if (strcmp(obj->obj.keys[i], key) == 0) {
            Json *v = obj->obj.vals[i];
            free(obj->obj.keys[i]);
            memmove(&obj->obj.keys[i], &obj->obj.keys[i + 1], (obj->obj.len - i - 1) * sizeof *obj->obj.keys);
            memmove(&obj->obj.vals[i], &obj->obj.vals[i + 1], (obj->obj.len - i - 1) * sizeof *obj->obj.vals);
            obj->obj.len--;
            return v;
        }
    }
    return NULL;
}

/* ---------- arrays ---------- */

void json_insert(Json *arr, size_t i, Json *val) {
    if (!arr || arr->type != JSON_ARRAY) { json_free(val); return; }
    if (!val) val = json_null();
    if (i > arr->arr.len) i = arr->arr.len;
    if (arr->arr.len == arr->arr.cap) {
        arr->arr.cap = arr->arr.cap ? arr->arr.cap * 2 : 8;
        arr->arr.items = xrealloc(arr->arr.items, arr->arr.cap * sizeof *arr->arr.items);
    }
    memmove(&arr->arr.items[i + 1], &arr->arr.items[i], (arr->arr.len - i) * sizeof *arr->arr.items);
    arr->arr.items[i] = val;
    arr->arr.len++;
}

void json_push(Json *arr, Json *val) {
    if (!arr || arr->type != JSON_ARRAY) { json_free(val); return; }
    json_insert(arr, arr->arr.len, val);
}

Json *json_at(const Json *arr, size_t i) {
    if (!arr || arr->type != JSON_ARRAY || i >= arr->arr.len) return NULL;
    return arr->arr.items[i];
}

Json *json_detach_index(Json *arr, size_t i) {
    if (!arr || arr->type != JSON_ARRAY || i >= arr->arr.len) return NULL;
    Json *v = arr->arr.items[i];
    memmove(&arr->arr.items[i], &arr->arr.items[i + 1], (arr->arr.len - i - 1) * sizeof *arr->arr.items);
    arr->arr.len--;
    return v;
}

size_t json_len(const Json *j) {
    if (!j) return 0;
    if (j->type == JSON_ARRAY) return j->arr.len;
    if (j->type == JSON_OBJECT) return j->obj.len;
    return 0;
}

/* ---------- parser ---------- */

typedef struct {
    const char *p;
    const char *end;
    const char *start;
    char *err;
    size_t errlen;
    int depth;
} Parser;

static void perr(Parser *ps, const char *msg) {
    if (ps->err && ps->errlen)
        snprintf(ps->err, ps->errlen, "%s at offset %ld", msg, (long)(ps->p - ps->start));
}

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static Json *parse_value(Parser *ps);

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex4(Parser *ps, unsigned *out) {
    if (ps->end - ps->p < 4) return -1;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(ps->p[i]);
        if (h < 0) return -1;
        v = v * 16 + (unsigned)h;
    }
    ps->p += 4;
    *out = v;
    return 0;
}

static void utf8_encode(StrBuf *sb, unsigned cp) {
    if (cp < 0x80) {
        sb_appendc(sb, (char)cp);
    } else if (cp < 0x800) {
        sb_appendc(sb, (char)(0xC0 | (cp >> 6)));
        sb_appendc(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_appendc(sb, (char)(0xE0 | (cp >> 12)));
        sb_appendc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_appendc(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_appendc(sb, (char)(0xF0 | (cp >> 18)));
        sb_appendc(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_appendc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_appendc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

/* Parses a JSON string literal starting at the opening quote. Returns a malloc'd C string. */
static char *parse_string_raw(Parser *ps) {
    if (ps->p >= ps->end || *ps->p != '"') { perr(ps, "expected string"); return NULL; }
    ps->p++;
    StrBuf sb;
    sb_init(&sb);
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p;
        if (c == '"') {
            ps->p++;
            return sb_detach(&sb);
        }
        if (c < 0x20) { perr(ps, "control character in string"); break; }
        if (c == '\\') {
            ps->p++;
            if (ps->p >= ps->end) { perr(ps, "unterminated escape"); break; }
            char e = *ps->p++;
            switch (e) {
            case '"': sb_appendc(&sb, '"'); break;
            case '\\': sb_appendc(&sb, '\\'); break;
            case '/': sb_appendc(&sb, '/'); break;
            case 'b': sb_appendc(&sb, '\b'); break;
            case 'f': sb_appendc(&sb, '\f'); break;
            case 'n': sb_appendc(&sb, '\n'); break;
            case 'r': sb_appendc(&sb, '\r'); break;
            case 't': sb_appendc(&sb, '\t'); break;
            case 'u': {
                unsigned cp;
                if (parse_hex4(ps, &cp) != 0) { perr(ps, "bad \\u escape"); goto fail; }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo;
                    if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                        ps->p += 2;
                        if (parse_hex4(ps, &lo) != 0 || lo < 0xDC00 || lo > 0xDFFF) { perr(ps, "bad surrogate pair"); goto fail; }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                utf8_encode(&sb, cp);
                break;
            }
            default:
                perr(ps, "bad escape");
                goto fail;
            }
            continue;
        }
        sb_appendc(&sb, (char)c);
        ps->p++;
    }
    if (ps->p >= ps->end) perr(ps, "unterminated string");
fail:
    sb_free(&sb);
    return NULL;
}

static Json *parse_number(Parser *ps) {
    const char *s = ps->p;
    if (ps->p < ps->end && *ps->p == '-') ps->p++;
    if (ps->p >= ps->end || !isdigit((unsigned char)*ps->p)) { perr(ps, "bad number"); return NULL; }
    if (*ps->p == '0') {
        ps->p++;
    } else {
        while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
    }
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        if (ps->p >= ps->end || !isdigit((unsigned char)*ps->p)) { perr(ps, "bad fraction"); return NULL; }
        while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
        if (ps->p >= ps->end || !isdigit((unsigned char)*ps->p)) { perr(ps, "bad exponent"); return NULL; }
        while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
    }
    char buf[64];
    size_t n = (size_t)(ps->p - s);
    if (n >= sizeof buf) { perr(ps, "number too long"); return NULL; }
    memcpy(buf, s, n);
    buf[n] = '\0';
    return json_number(strtod(buf, NULL));
}

static Json *parse_array(Parser *ps) {
    ps->p++; /* [ */
    Json *a = json_array();
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; return a; }
    for (;;) {
        skip_ws(ps);
        Json *v = parse_value(ps);
        if (!v) { json_free(a); return NULL; }
        json_push(a, v);
        skip_ws(ps);
        if (ps->p >= ps->end) { perr(ps, "unterminated array"); json_free(a); return NULL; }
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == ']') { ps->p++; return a; }
        perr(ps, "expected ',' or ']'");
        json_free(a);
        return NULL;
    }
}

static Json *parse_object(Parser *ps) {
    ps->p++; /* { */
    Json *o = json_object();
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return o; }
    for (;;) {
        skip_ws(ps);
        char *key = parse_string_raw(ps);
        if (!key) { json_free(o); return NULL; }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') { perr(ps, "expected ':'"); free(key); json_free(o); return NULL; }
        ps->p++;
        skip_ws(ps);
        Json *v = parse_value(ps);
        if (!v) { free(key); json_free(o); return NULL; }
        json_set(o, key, v);
        free(key);
        skip_ws(ps);
        if (ps->p >= ps->end) { perr(ps, "unterminated object"); json_free(o); return NULL; }
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == '}') { ps->p++; return o; }
        perr(ps, "expected ',' or '}'");
        json_free(o);
        return NULL;
    }
}

static int match_lit(Parser *ps, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) >= n && memcmp(ps->p, lit, n) == 0) { ps->p += n; return 1; }
    return 0;
}

static Json *parse_value(Parser *ps) {
    skip_ws(ps);
    if (ps->p >= ps->end) { perr(ps, "unexpected end of input"); return NULL; }
    if (ps->depth >= MAX_DEPTH) { perr(ps, "nesting too deep"); return NULL; }
    Json *r = NULL;
    switch (*ps->p) {
    case '{': ps->depth++; r = parse_object(ps); ps->depth--; break;
    case '[': ps->depth++; r = parse_array(ps); ps->depth--; break;
    case '"': {
        char *s = parse_string_raw(ps);
        if (!s) return NULL;
        r = json_alloc(JSON_STRING);
        r->str = s;
        break;
    }
    case 't': if (match_lit(ps, "true")) r = json_bool(1); else perr(ps, "bad literal"); break;
    case 'f': if (match_lit(ps, "false")) r = json_bool(0); else perr(ps, "bad literal"); break;
    case 'n': if (match_lit(ps, "null")) r = json_null(); else perr(ps, "bad literal"); break;
    default:
        if (*ps->p == '-' || isdigit((unsigned char)*ps->p)) r = parse_number(ps);
        else perr(ps, "unexpected character");
    }
    return r;
}

Json *json_parse(const char *text, size_t len, char *err, size_t errlen) {
    Parser ps = { text, text + len, text, err, errlen, 0 };
    if (err && errlen) err[0] = '\0';
    Json *v = parse_value(&ps);
    if (!v) return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) {
        perr(&ps, "trailing characters");
        json_free(v);
        return NULL;
    }
    return v;
}

/* ---------- serialiser ---------- */

static void write_string(StrBuf *sb, const char *s) {
    sb_appendc(sb, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': sb_append(sb, "\\\""); break;
        case '\\': sb_append(sb, "\\\\"); break;
        case '\n': sb_append(sb, "\\n"); break;
        case '\r': sb_append(sb, "\\r"); break;
        case '\t': sb_append(sb, "\\t"); break;
        case '\b': sb_append(sb, "\\b"); break;
        case '\f': sb_append(sb, "\\f"); break;
        default:
            if (*p < 0x20) sb_printf(sb, "\\u%04x", *p);
            else sb_appendc(sb, (char)*p);
        }
    }
    sb_appendc(sb, '"');
}

static void write_number(StrBuf *sb, double n) {
    if (isnan(n) || isinf(n)) { sb_append(sb, "null"); return; }
    if (n == floor(n) && fabs(n) < 1e15) sb_printf(sb, "%.0f", n);
    else sb_printf(sb, "%.17g", n);
}

static void indent(StrBuf *sb, int level) {
    sb_appendc(sb, '\n');
    for (int i = 0; i < level; i++) sb_append(sb, "  ");
}

static void write_value(StrBuf *sb, const Json *j, int pretty, int level) {
    if (!j) { sb_append(sb, "null"); return; }
    switch (j->type) {
    case JSON_NULL: sb_append(sb, "null"); break;
    case JSON_BOOL: sb_append(sb, j->b ? "true" : "false"); break;
    case JSON_NUMBER: write_number(sb, j->num); break;
    case JSON_STRING: write_string(sb, j->str); break;
    case JSON_ARRAY:
        sb_appendc(sb, '[');
        for (size_t i = 0; i < j->arr.len; i++) {
            if (i) sb_appendc(sb, ',');
            if (pretty) indent(sb, level + 1);
            write_value(sb, j->arr.items[i], pretty, level + 1);
        }
        if (pretty && j->arr.len) indent(sb, level);
        sb_appendc(sb, ']');
        break;
    case JSON_OBJECT:
        sb_appendc(sb, '{');
        for (size_t i = 0; i < j->obj.len; i++) {
            if (i) sb_appendc(sb, ',');
            if (pretty) indent(sb, level + 1);
            write_string(sb, j->obj.keys[i]);
            sb_append(sb, pretty ? ": " : ":");
            write_value(sb, j->obj.vals[i], pretty, level + 1);
        }
        if (pretty && j->obj.len) indent(sb, level);
        sb_appendc(sb, '}');
        break;
    }
}

char *json_stringify(const Json *j, int pretty) {
    StrBuf sb;
    sb_init(&sb);
    write_value(&sb, j, pretty, 0);
    if (pretty) sb_appendc(&sb, '\n');
    return sb_detach(&sb);
}

/* ---------- paths ---------- */

typedef struct {
    const char *key;
    size_t keylen;
    long idx;      /* -1 when not numeric, -2 for "-" (append) */
} Seg;

/* Reads the next path segment. Returns 0 at end of path. */
static int next_seg(const char **pp, Seg *s) {
    const char *p = *pp;
    while (*p == '.') p++;
    if (!*p) { *pp = p; return 0; }
    if (*p == '[') {
        p++;
        s->key = p;
        while (*p && *p != ']') p++;
        s->keylen = (size_t)(p - s->key);
        if (*p == ']') p++;
    } else {
        s->key = p;
        while (*p && *p != '.' && *p != '[') p++;
        s->keylen = (size_t)(p - s->key);
    }
    s->idx = -1;
    if (s->keylen == 1 && s->key[0] == '-') {
        s->idx = -2;
    } else if (s->keylen > 0 && s->keylen < 10) {
        long v = 0;
        int all_digits = 1;
        for (size_t i = 0; i < s->keylen; i++) {
            if (!isdigit((unsigned char)s->key[i])) { all_digits = 0; break; }
            v = v * 10 + (s->key[i] - '0');
        }
        if (all_digits) s->idx = v;
    }
    *pp = p;
    return 1;
}

static Json *step(const Json *cur, const Seg *s) {
    if (!cur) return NULL;
    if (cur->type == JSON_OBJECT) return json_get_n(cur, s->key, s->keylen);
    if (cur->type == JSON_ARRAY) {
        long i = s->idx;
        if (i < 0) return NULL;
        return json_at(cur, (size_t)i);
    }
    return NULL;
}

Json *json_path_get(const Json *root, const char *path) {
    const Json *cur = root;
    const char *p = path ? path : "";
    Seg s;
    while (cur && next_seg(&p, &s)) cur = step(cur, &s);
    return (Json *)cur;
}

/* Resolves everything but the last segment. Returns NULL if the path is empty. */
static Json *path_parent(Json *root, const char *path, Seg *last) {
    const char *p = path ? path : "";
    Seg s;
    if (!next_seg(&p, &s)) return NULL;
    Json *cur = root;
    for (;;) {
        Seg nxt;
        if (!next_seg(&p, &nxt)) { *last = s; return cur; }
        cur = step(cur, &s);
        if (!cur) return NULL;
        s = nxt;
    }
}

int json_path_set(Json *root, const char *path, Json *val) {
    Seg last;
    Json *parent = path_parent(root, path, &last);
    if (!parent) { json_free(val); return -1; }
    if (parent->type == JSON_OBJECT) {
        char *key = xstrndup(last.key, last.keylen);
        json_set(parent, key, val);
        free(key);
        return 0;
    }
    if (parent->type == JSON_ARRAY) {
        long i = last.idx;
        if (i == -2 || (size_t)i == parent->arr.len) { json_push(parent, val); return 0; }
        if (i < 0 || (size_t)i > parent->arr.len) { json_free(val); return -1; }
        json_free(parent->arr.items[i]);
        parent->arr.items[i] = val;
        return 0;
    }
    json_free(val);
    return -1;
}

int json_path_insert(Json *root, const char *path, Json *val) {
    Seg last;
    Json *parent = path_parent(root, path, &last);
    if (!parent) { json_free(val); return -1; }
    if (parent->type == JSON_OBJECT) {
        char *key = xstrndup(last.key, last.keylen);
        json_set(parent, key, val);
        free(key);
        return 0;
    }
    if (parent->type == JSON_ARRAY) {
        long i = last.idx;
        if (i == -2) { json_push(parent, val); return 0; }
        if (i < 0) { json_free(val); return -1; }
        json_insert(parent, (size_t)i, val);
        return 0;
    }
    json_free(val);
    return -1;
}

Json *json_path_detach(Json *root, const char *path) {
    Seg last;
    Json *parent = path_parent(root, path, &last);
    if (!parent) return NULL;
    if (parent->type == JSON_OBJECT) {
        char *key = xstrndup(last.key, last.keylen);
        Json *v = json_detach_key(parent, key);
        free(key);
        return v;
    }
    if (parent->type == JSON_ARRAY) {
        long i = last.idx;
        if (i == -2) i = (long)parent->arr.len - 1;
        if (i < 0) return NULL;
        return json_detach_index(parent, (size_t)i);
    }
    return NULL;
}

/* ---------- helpers ---------- */

int json_truthy(const Json *j) {
    if (!j) return 0;
    switch (j->type) {
    case JSON_NULL: return 0;
    case JSON_BOOL: return j->b;
    case JSON_NUMBER: return j->num != 0;
    case JSON_STRING: return j->str[0] != '\0';
    case JSON_ARRAY: return j->arr.len > 0;
    case JSON_OBJECT: return 1;
    }
    return 0;
}

const char *json_str(const Json *j, const char *def) {
    return (j && j->type == JSON_STRING) ? j->str : def;
}

double json_num(const Json *j, double def) {
    return (j && j->type == JSON_NUMBER) ? j->num : def;
}

const char *json_type_name(JsonType t) {
    switch (t) {
    case JSON_NULL: return "null";
    case JSON_BOOL: return "boolean";
    case JSON_NUMBER: return "number";
    case JSON_STRING: return "string";
    case JSON_ARRAY: return "array";
    case JSON_OBJECT: return "object";
    }
    return "?";
}
