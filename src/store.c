#define _POSIX_C_SOURCE 200809L
#include "store.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    Buf b;
    buf_init(&b);
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_append(&b, chunk, n);
    fclose(f);
    if (len_out) *len_out = b.len;
    return buf_steal(&b);
}

int valid_type_name(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 32) return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) return 0;
    return 1;
}

/* ---------- types ---------- */

const WType *store_type(const Store *st, const char *name) {
    for (size_t i = 0; i < st->ntypes; i++)
        if (strcmp(json_get_str(st->types[i].def, "name"), name) == 0) return &st->types[i];
    return NULL;
}

/* Validates + normalises a type definition, compiles it, and installs it. */
static int install_type(Store *st, const Json *in, int builtin, char *err, size_t errlen) {
    if (!in || in->type != J_OBJ) { snprintf(err, errlen, "type must be an object"); return -1; }
    const char *name = json_get_str(in, "name");
    const char *src = json_get_str(in, "template");
    if (!name || !valid_type_name(name)) {
        snprintf(err, errlen, "'name' must be 1-32 chars of [a-z0-9-]");
        return -1;
    }
    if (!src) { snprintf(err, errlen, "'template' (string) is required"); return -1; }

    const WType *existing = store_type(st, name);
    if (existing && !builtin && json_truthy(json_get(existing->def, "builtin"))) {
        snprintf(err, errlen, "'%s' is a built-in type and can't be replaced", name);
        return -1;
    }

    char terr[256];
    Tmpl *t = tmpl_compile(src, terr, sizeof terr);
    if (!t) { snprintf(err, errlen, "template error: %s", terr); return -1; }

    Json *def = json_obj();
    json_set(def, "name", json_str(name));
    const char *label = json_get_str(in, "label");
    json_set(def, "label", json_str(label && *label ? label : name));
    const char *desc = json_get_str(in, "description");
    json_set(def, "description", json_str(desc ? desc : ""));
    json_set(def, "template", json_str(src));
    const char *css = json_get_str(in, "css");
    json_set(def, "css", json_str(css ? css : ""));
    Json *defaults = json_get(in, "defaults");
    json_set(def, "defaults", defaults ? json_clone(defaults) : json_obj());
    json_set(def, "builtin", json_bool(builtin));

    if (existing) {
        WType *w = (WType *)existing;
        json_free(w->def);
        tmpl_free(w->tmpl);
        w->def = def;
        w->tmpl = t;
    } else {
        st->types = xrealloc(st->types, (st->ntypes + 1) * sizeof *st->types);
        st->types[st->ntypes].def = def;
        st->types[st->ntypes].tmpl = t;
        st->ntypes++;
    }
    return 0;
}

int store_put_type(Store *st, const Json *def, char *err, size_t errlen) {
    return install_type(st, def, 0, err, errlen);
}

int store_delete_type(Store *st, const char *name, char *err, size_t errlen) {
    for (size_t i = 0; i < st->ntypes; i++) {
        WType *w = &st->types[i];
        if (strcmp(json_get_str(w->def, "name"), name) != 0) continue;
        if (json_truthy(json_get(w->def, "builtin"))) {
            snprintf(err, errlen, "built-in types can't be deleted");
            return -2;
        }
        for (size_t j = 0; j < st->widgets->len; j++) {
            const char *t = json_get_str(st->widgets->items[j], "type");
            if (t && strcmp(t, name) == 0) {
                snprintf(err, errlen, "type is still used by widgets; delete them first");
                return -3;
            }
        }
        json_free(w->def);
        tmpl_free(w->tmpl);
        memmove(st->types + i, st->types + i + 1, (st->ntypes - i - 1) * sizeof *st->types);
        st->ntypes--;
        return 0;
    }
    snprintf(err, errlen, "no such type");
    return -1;
}

/* Built-ins live in <dir>/<name>.html (+ optional .json meta and .css) and
 * are registered exactly like user types posted to the API. */
static void load_builtins(Store *st, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "warning: can't open %s\n", dir); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".html") != 0) continue;
        char name[64];
        if (n - 5 >= sizeof name) continue;
        memcpy(name, e->d_name, n - 5);
        name[n - 5] = '\0';

        char path[512];
        snprintf(path, sizeof path, "%s/%s.html", dir, name);
        char *tpl = read_file(path, NULL);
        snprintf(path, sizeof path, "%s/%s.css", dir, name);
        char *css = read_file(path, NULL);
        snprintf(path, sizeof path, "%s/%s.json", dir, name);
        size_t mlen = 0;
        char *meta_src = read_file(path, &mlen);

        char err[256];
        Json *meta = meta_src ? json_parse(meta_src, mlen, err, sizeof err) : json_obj();
        if (!meta) {
            fprintf(stderr, "warning: %s: %s\n", path, err);
            meta = json_obj();
        }
        json_set(meta, "name", json_str(name));
        json_set(meta, "template", json_str(tpl ? tpl : ""));
        if (css) json_set(meta, "css", json_str(css));
        if (install_type(st, meta, 1, err, sizeof err) < 0)
            fprintf(stderr, "warning: builtin '%s': %s\n", name, err);
        json_free(meta);
        free(tpl);
        free(css);
        free(meta_src);
    }
    closedir(d);
}

/* ---------- persistence ---------- */

int store_save(const Store *st) {
    Json *root = json_obj();
    json_set(root, "next_id", json_num((double)st->next_id));
    Json *types = json_arr();
    for (size_t i = 0; i < st->ntypes; i++)
        if (!json_truthy(json_get(st->types[i].def, "builtin")))
            json_arr_push(types, json_clone(st->types[i].def));
    json_set(root, "types", types);
    json_set(root, "widgets", json_clone(st->widgets));

    char *text = json_to_string(root, 2);
    json_free(root);

    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", st->db_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { perror(tmp); free(text); return -1; }
    size_t len = strlen(text);
    int ok = fwrite(text, 1, len, f) == len;
    ok &= fclose(f) == 0;
    free(text);
    if (!ok || rename(tmp, st->db_path) != 0) { perror("save db"); return -1; }
    return 0;
}

int store_open(Store *st, const char *db_path, const char *builtin_dir) {
    memset(st, 0, sizeof *st);
    st->db_path = xstrdup(db_path);
    st->widgets = json_arr();
    st->next_id = 1;
    load_builtins(st, builtin_dir);

    size_t len;
    char *src = read_file(db_path, &len);
    if (!src) return 1; /* fresh database */

    char err[256];
    Json *root = json_parse(src, len, err, sizeof err);
    free(src);
    if (!root) { fprintf(stderr, "error: %s: %s\n", db_path, err); return -1; }

    Json *nid = json_get(root, "next_id");
    if (nid && nid->type == J_NUM) st->next_id = (long)nid->n;

    Json *types = json_get(root, "types");
    for (size_t i = 0; types && types->type == J_ARR && i < types->len; i++)
        if (install_type(st, types->items[i], 0, err, sizeof err) < 0)
            fprintf(stderr, "warning: stored type skipped: %s\n", err);

    Json *widgets = json_get(root, "widgets");
    if (widgets && widgets->type == J_ARR) {
        json_free(st->widgets);
        st->widgets = json_clone(widgets);
    }
    json_free(root);
    return 0;
}

/* ---------- widgets ---------- */

Json *store_widget(const Store *st, long id, size_t *index_out) {
    for (size_t i = 0; i < st->widgets->len; i++) {
        Json *idv = json_get(st->widgets->items[i], "id");
        if (idv && idv->type == J_NUM && (long)idv->n == id) {
            if (index_out) *index_out = i;
            return st->widgets->items[i];
        }
    }
    return NULL;
}

Json *store_add_widget(Store *st, const char *type, const char *title,
                       const Json *data, char *err, size_t errlen) {
    const WType *t = store_type(st, type ? type : "");
    if (!t) { snprintf(err, errlen, "unknown widget type '%s'", type ? type : ""); return NULL; }
    if (data && data->type != J_OBJ) { snprintf(err, errlen, "'data' must be an object"); return NULL; }

    Json *w = json_obj();
    json_set(w, "id", json_num((double)st->next_id++));
    json_set(w, "type", json_str(type));
    json_set(w, "title", json_str(title && *title ? title : json_get_str(t->def, "label")));
    json_set(w, "size", json_str("normal"));
    Json *defaults = json_get(t->def, "defaults");
    json_set(w, "data", json_clone(data ? data : (defaults && defaults->type == J_OBJ ? defaults : NULL)));
    if (!json_get(w, "data")) json_set(w, "data", json_obj());
    json_arr_push(st->widgets, w);
    return w;
}

int store_delete_widget(Store *st, long id) {
    size_t i;
    if (!store_widget(st, id, &i)) return -1;
    json_free(json_arr_take(st->widgets, i));
    return 0;
}

/* ---------- data ops ---------- */

/* Put v at path (taking ownership). Root path replaces the whole data. */
static int set_at(Json *widget, const char *path, Json *v, char *err, size_t errlen) {
    if (!*path) {
        if (v->type != J_OBJ) { json_free(v); snprintf(err, errlen, "root data must be an object"); return -1; }
        json_set(widget, "data", v);
        return 0;
    }
    char *last, *scratch;
    Json *parent = json_path_parent(json_get(widget, "data"), path, &last, &scratch);
    int rc = -1;
    size_t idx;
    if (parent && parent->type == J_OBJ) {
        json_set(parent, last, v);
        rc = 0;
    } else if (parent && parent->type == J_ARR && json_is_index(last, &idx) && idx <= parent->len) {
        if (idx == parent->len) json_arr_push(parent, v);
        else { json_free(parent->items[idx]); parent->items[idx] = v; }
        rc = 0;
    }
    if (rc < 0) { json_free(v); snprintf(err, errlen, "can't set '%s'", path); }
    free(scratch);
    return rc;
}

static long num_field(const Json *o, const char *k, long dflt) {
    Json *v = json_get(o, k);
    return (v && v->type == J_NUM) ? (long)v->n : dflt;
}

int store_apply_op(Json *widget, const Json *op, char *err, size_t errlen) {
    if (!op || op->type != J_OBJ) { snprintf(err, errlen, "op must be an object"); return -1; }
    const char *name = json_get_str(op, "op");
    const char *path = json_get_str(op, "path");
    Json *value = json_get(op, "value");
    Json *data = json_get(widget, "data");
    if (!name) { snprintf(err, errlen, "'op' is required"); return -1; }
    if (!path) path = "";

    if (strcmp(name, "set") == 0) {
        return set_at(widget, path, value ? json_clone(value) : json_null(), err, errlen);
    }
    if (strcmp(name, "toggle") == 0) {
        return set_at(widget, path, json_bool(!json_truthy(json_path_get(data, path))), err, errlen);
    }
    if (strcmp(name, "inc") == 0) {
        Json *cur = json_path_get(data, path);
        Json *by = json_get(op, "by");
        if (cur && cur->type != J_NUM && cur->type != J_NULL) {
            snprintf(err, errlen, "'%s' is not a number", path);
            return -1;
        }
        double n = (cur && cur->type == J_NUM) ? cur->n : 0;
        n += (by && by->type == J_NUM) ? by->n : 1;
        return set_at(widget, path, json_num(n), err, errlen);
    }
    if (strcmp(name, "push") == 0) {
        Json *arr = json_path_get(data, path);
        if (!arr) {
            if (set_at(widget, path, json_arr(), err, errlen) < 0) return -1;
            arr = json_path_get(json_get(widget, "data"), path);
        }
        if (!arr || arr->type != J_ARR) { snprintf(err, errlen, "'%s' is not a list", path); return -1; }
        long idx = num_field(op, "index", -1);
        Json *v = value ? json_clone(value) : json_null();
        if (idx < 0) json_arr_push(arr, v);
        else json_arr_insert(arr, (size_t)idx, v);
        return 0;
    }
    if (strcmp(name, "remove") == 0) {
        char *last, *scratch;
        Json *parent = json_path_parent(data, path, &last, &scratch);
        size_t idx;
        int rc = -1;
        if (parent && parent->type == J_ARR && json_is_index(last, &idx) && idx < parent->len) {
            json_free(json_arr_take(parent, idx));
            rc = 0;
        } else if (parent && parent->type == J_OBJ && json_del(parent, last)) {
            rc = 0;
        }
        free(scratch);
        if (rc < 0) snprintf(err, errlen, "nothing to remove at '%s'", path);
        return rc;
    }
    if (strcmp(name, "move") == 0) {
        const char *to = json_get_str(op, "to");
        if (!to) { snprintf(err, errlen, "'move' needs 'to'"); return -1; }
        size_t plen = strlen(path);
        if (strcmp(to, path) == 0 || (strncmp(to, path, plen) == 0 && to[plen] == '.')) {
            snprintf(err, errlen, "can't move an item into itself");
            return -1;
        }
        char *last, *scratch;
        Json *src = json_path_parent(data, path, &last, &scratch);
        size_t from;
        Json *dest = json_path_get(data, to);
        int ok = src && src->type == J_ARR && json_is_index(last, &from) && from < src->len &&
                 dest && dest->type == J_ARR;
        free(scratch);
        if (!ok) { snprintf(err, errlen, "bad move from '%s' to '%s'", path, to); return -1; }
        long idx = num_field(op, "index", -1);
        Json *item = json_arr_take(src, from);
        if (idx < 0) {
            json_arr_push(dest, item);
        } else {
            if (dest == src && (size_t)idx > from) idx--;
            json_arr_insert(dest, (size_t)idx, item);
        }
        return 0;
    }
    snprintf(err, errlen, "unknown op '%s' (use set, toggle, inc, push, remove, move)", name);
    return -1;
}
