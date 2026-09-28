#include "store.h"
#include "template.h"
#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNDO_LIMIT 50

static const char *TYPES_FILE = "types.json";
static const char *WIDGETS_FILE = "widgets.json";

static void path_join(char *out, size_t n, const char *a, const char *b) {
    snprintf(out, n, "%s/%s", a, b);
}

static int valid_type_name(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n > 64) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static size_t type_index(const Store *st, const char *name) {
    for (size_t i = 0; i < json_len(st->types); i++)
        if (strcmp(json_str(json_get(json_at(st->types, i), "name"), ""), name) == 0) return i;
    return (size_t)-1;
}

Json *store_get_type(const Store *st, const char *name) {
    size_t i = type_index(st, name);
    return i == (size_t)-1 ? NULL : json_at(st->types, i);
}

int store_put_type(Store *st, Json *def, int builtin, char *err, size_t errlen) {
    if (!def || def->type != JSON_OBJECT) { snprintf(err, errlen, "type definition must be an object"); return -1; }
    const char *name = json_str(json_get(def, "name"), NULL);
    if (!name || !valid_type_name(name)) {
        snprintf(err, errlen, "name is required: 1-64 chars of letters, digits, '-' or '_'");
        return -1;
    }
    const char *tpl = json_str(json_get(def, "template"), NULL);
    if (!tpl) { snprintf(err, errlen, "template (string) is required"); return -1; }
    if (template_validate(tpl, err, errlen) != 0) return -1;
    Json *ds = json_get(def, "default_state");
    if (!ds) json_set(def, "default_state", json_object());
    else if (ds->type != JSON_OBJECT) { snprintf(err, errlen, "default_state must be an object"); return -1; }
    const char *keys[] = { "description", "script", "style" };
    for (size_t i = 0; i < 3; i++) {
        Json *v = json_get(def, keys[i]);
        if (!v || v->type == JSON_NULL) json_set(def, keys[i], json_string(""));
        else if (v->type != JSON_STRING) { snprintf(err, errlen, "%s must be a string", keys[i]); return -1; }
    }
    json_set(def, "builtin", json_bool(builtin));

    size_t existing = type_index(st, name);
    if (existing != (size_t)-1) {
        Json *old = json_at(st->types, existing);
        if (json_truthy(json_get(old, "builtin")) && !builtin) {
            snprintf(err, errlen, "'%s' is a built-in type; clone it under a new name instead", name);
            return -1;
        }
        json_free(json_detach_index(st->types, existing));
        json_insert(st->types, existing, def);
    } else {
        json_push(st->types, def);
    }
    return 0;
}

int store_delete_type(Store *st, const char *name, char *err, size_t errlen) {
    size_t i = type_index(st, name);
    if (i == (size_t)-1) { snprintf(err, errlen, "no such type"); return -1; }
    if (json_truthy(json_get(json_at(st->types, i), "builtin"))) {
        snprintf(err, errlen, "built-in types cannot be deleted");
        return -1;
    }
    for (size_t w = 0; w < json_len(st->widgets); w++) {
        if (strcmp(json_str(json_get(json_at(st->widgets, w), "type"), ""), name) == 0) {
            snprintf(err, errlen, "type is in use by widget %d; delete those widgets first",
                     (int)json_num(json_get(json_at(st->widgets, w), "id"), 0));
            return -1;
        }
    }
    json_free(json_detach_index(st->types, i));
    return 0;
}

/* ---------- built-in loading ---------- */

static char *read_optional(const char *dir, const char *file) {
    char path[2048];
    path_join(path, sizeof path, dir, file);
    char *s = read_file(path, NULL);
    return s ? s : xstrdup("");
}

static int load_builtin(Store *st, const char *dir, const char *name) {
    char path[2048];
    path_join(path, sizeof path, dir, "type.json");
    size_t len;
    char *text = read_file(path, &len);
    if (!text) return -1;
    char err[256];
    Json *def = json_parse(text, len, err, sizeof err);
    free(text);
    if (!def) { fprintf(stderr, "builtin %s: %s\n", name, err); return -1; }
    if (!json_get(def, "name")) json_set(def, "name", json_string(name));
    char *tpl = read_optional(dir, "template.html");
    char *script = read_optional(dir, "script.js");
    char *style = read_optional(dir, "style.css");
    json_set(def, "template", json_string(tpl));
    json_set(def, "script", json_string(script));
    json_set(def, "style", json_string(style));
    free(tpl); free(script); free(style);
    if (store_put_type(st, def, 1, err, sizeof err) != 0) {
        fprintf(stderr, "builtin %s: %s\n", name, err);
        json_free(def);
        return -1;
    }
    return 0;
}

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void load_builtins(Store *st, const char *builtin_dir) {
    DIR *d = opendir(builtin_dir);
    if (!d) { fprintf(stderr, "warning: no builtin directory at %s\n", builtin_dir); return; }
    char *names[256];
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) {
        if (e->d_name[0] == '.') continue;
        names[n++] = xstrdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp_names);
    for (size_t i = 0; i < n; i++) {
        char dir[1024];
        path_join(dir, sizeof dir, builtin_dir, names[i]);
        if (load_builtin(st, dir, names[i]) == 0) printf("loaded built-in widget type '%s'\n", names[i]);
        free(names[i]);
    }
}

/* ---------- persistence ---------- */

static Json *load_json_file(const char *path) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) return NULL;
    char err[256];
    Json *j = json_parse(text, len, err, sizeof err);
    free(text);
    if (!j) fprintf(stderr, "warning: %s: %s (ignoring file)\n", path, err);
    return j;
}

int store_init(Store *st, const char *data_dir, const char *builtin_dir) {
    memset(st, 0, sizeof *st);
    st->types = json_array();
    st->widgets = json_array();
    st->next_id = 1;
    snprintf(st->data_dir, sizeof st->data_dir, "%s", data_dir);
    if (ensure_dir(data_dir) != 0) { fprintf(stderr, "cannot create data dir %s\n", data_dir); return -1; }

    load_builtins(st, builtin_dir);

    char path[1024];
    path_join(path, sizeof path, data_dir, TYPES_FILE);
    Json *types = load_json_file(path);
    if (types && types->type == JSON_ARRAY) {
        while (json_len(types)) {
            Json *def = json_detach_index(types, 0);
            char err[256];
            if (store_put_type(st, def, 0, err, sizeof err) != 0) {
                fprintf(stderr, "warning: skipping saved type: %s\n", err);
                json_free(def);
            }
        }
    }
    json_free(types);

    path_join(path, sizeof path, data_dir, WIDGETS_FILE);
    Json *doc = load_json_file(path);
    if (doc && doc->type == JSON_OBJECT) {
        st->next_id = (int)json_num(json_get(doc, "next_id"), 1);
        Json *ws = json_detach_key(doc, "widgets");
        if (ws && ws->type == JSON_ARRAY) {
            json_free(st->widgets);
            st->widgets = ws;
        } else {
            json_free(ws);
        }
    }
    json_free(doc);
    return 0;
}

void store_free(Store *st) {
    json_free(st->types);
    json_free(st->widgets);
    for (size_t i = 0; i < st->undo_len; i++) stack_free_all(st->undo[i].history, (void (*)(void *))json_free);
    free(st->undo);
    memset(st, 0, sizeof *st);
}

int store_save(Store *st) {
    Json *user_types = json_array();
    for (size_t i = 0; i < json_len(st->types); i++) {
        Json *t = json_at(st->types, i);
        if (!json_truthy(json_get(t, "builtin"))) json_push(user_types, json_clone(t));
    }
    char *text = json_stringify(user_types, 1);
    json_free(user_types);
    char path[1024];
    path_join(path, sizeof path, st->data_dir, TYPES_FILE);
    int rc = write_file_atomic(path, text, strlen(text));
    free(text);
    if (rc != 0) { perror("save types"); return -1; }

    Json *doc = json_object();
    json_set(doc, "next_id", json_number(st->next_id));
    json_set(doc, "widgets", json_clone(st->widgets));
    text = json_stringify(doc, 1);
    json_free(doc);
    path_join(path, sizeof path, st->data_dir, WIDGETS_FILE);
    rc = write_file_atomic(path, text, strlen(text));
    free(text);
    if (rc != 0) { perror("save widgets"); return -1; }
    return 0;
}

/* ---------- widgets ---------- */

Json *store_get_widget(const Store *st, int id) {
    for (size_t i = 0; i < json_len(st->widgets); i++) {
        Json *w = json_at(st->widgets, i);
        if ((int)json_num(json_get(w, "id"), -1) == id) return w;
    }
    return NULL;
}

Json *store_create_widget(Store *st, const char *type, const char *title, Json *state, char *err, size_t errlen) {
    Json *def = store_get_type(st, type ? type : "");
    if (!def) { snprintf(err, errlen, "unknown widget type '%s'", type ? type : ""); json_free(state); return NULL; }
    if (state && state->type != JSON_OBJECT) { snprintf(err, errlen, "state must be an object"); json_free(state); return NULL; }
    Json *w = json_object();
    json_set(w, "id", json_number(st->next_id++));
    json_set(w, "type", json_string(type));
    const char *desc = json_str(json_get(def, "description"), "");
    json_set(w, "title", json_string(title && *title ? title : (*desc ? desc : type)));
    json_set(w, "state", state ? state : json_clone(json_get(def, "default_state")));
    json_push(st->widgets, w);
    store_save(st);
    return w;
}

static Stack *undo_stack(Store *st, int id, int create) {
    for (size_t i = 0; i < st->undo_len; i++)
        if (st->undo[i].id == id) return st->undo[i].history;
    if (!create) return NULL;
    if (st->undo_len == st->undo_cap) {
        st->undo_cap = st->undo_cap ? st->undo_cap * 2 : 8;
        st->undo = xrealloc(st->undo, st->undo_cap * sizeof *st->undo);
    }
    st->undo[st->undo_len].id = id;
    st->undo[st->undo_len].history = stack_new();
    return st->undo[st->undo_len++].history;
}

static void push_undo(Store *st, int id, Json *snapshot) {
    Stack *h = undo_stack(st, id, 1);
    if (stack_size(h) >= UNDO_LIMIT) {
        /* Drop the oldest snapshot: rebuild the stack without its bottom element. */
        Stack *tmp = stack_new();
        while (stack_size(h) > 1) stack_push(tmp, stack_pop(h));
        json_free(stack_pop(h));
        while (!stack_is_empty(tmp)) stack_push(h, stack_pop(tmp));
        stack_free(tmp);
    }
    stack_push(h, snapshot);
}

int store_delete_widget(Store *st, int id) {
    for (size_t i = 0; i < json_len(st->widgets); i++) {
        Json *w = json_at(st->widgets, i);
        if ((int)json_num(json_get(w, "id"), -1) == id) {
            json_free(json_detach_index(st->widgets, i));
            for (size_t u = 0; u < st->undo_len; u++) {
                if (st->undo[u].id == id) {
                    stack_free_all(st->undo[u].history, (void (*)(void *))json_free);
                    st->undo[u] = st->undo[--st->undo_len];
                    break;
                }
            }
            store_save(st);
            return 0;
        }
    }
    return -1;
}

static int apply_one(Json *state, const Json *op, char *err, size_t errlen) {
    const char *kind = json_str(json_get(op, "op"), "");
    const char *path = json_str(json_get(op, "path"), "");
    const Json *value = json_get(op, "value");

    if (strcmp(kind, "set") == 0) {
        if (!value) { snprintf(err, errlen, "set: value is required"); return -1; }
        if (path[0] == '\0') {
            if (value->type != JSON_OBJECT) { snprintf(err, errlen, "set: root state must be an object"); return -1; }
            /* Replace the root by swapping struct contents with a clone. */
            Json *copy = json_clone(value);
            Json tmp = *state;
            *state = *copy;
            *copy = tmp;
            json_free(copy);
            return 0;
        }
        if (json_path_set(state, path, json_clone(value)) != 0) { snprintf(err, errlen, "set: path '%s' does not resolve", path); return -1; }
        return 0;
    }
    if (strcmp(kind, "push") == 0) {
        Json *arr = json_path_get(state, path);
        if (!arr || arr->type != JSON_ARRAY) { snprintf(err, errlen, "push: '%s' is not an array", path); return -1; }
        json_push(arr, value ? json_clone(value) : json_null());
        return 0;
    }
    if (strcmp(kind, "pop") == 0) {
        Json *arr = json_path_get(state, path);
        if (!arr || arr->type != JSON_ARRAY) { snprintf(err, errlen, "pop: '%s' is not an array", path); return -1; }
        if (json_len(arr) == 0) { snprintf(err, errlen, "pop: '%s' is empty", path); return -1; }
        json_free(json_detach_index(arr, json_len(arr) - 1));
        return 0;
    }
    if (strcmp(kind, "insert") == 0) {
        if (!value) { snprintf(err, errlen, "insert: value is required"); return -1; }
        if (json_path_insert(state, path, json_clone(value)) != 0) { snprintf(err, errlen, "insert: path '%s' does not resolve", path); return -1; }
        return 0;
    }
    if (strcmp(kind, "remove") == 0) {
        Json *v = json_path_detach(state, path);
        if (!v) { snprintf(err, errlen, "remove: path '%s' does not resolve", path); return -1; }
        json_free(v);
        return 0;
    }
    if (strcmp(kind, "move") == 0) {
        const char *from = json_str(json_get(op, "from"), "");
        const char *to = json_str(json_get(op, "to"), "");
        Json *v = json_path_detach(state, from);
        if (!v) { snprintf(err, errlen, "move: from '%s' does not resolve", from); return -1; }
        if (json_path_insert(state, to, v) != 0) { snprintf(err, errlen, "move: to '%s' does not resolve", to); return -1; }
        return 0;
    }
    if (strcmp(kind, "increment") == 0) {
        Json *n = json_path_get(state, path);
        if (!n || n->type != JSON_NUMBER) { snprintf(err, errlen, "increment: '%s' is not a number", path); return -1; }
        n->num += value ? json_num(value, 1) : 1;
        return 0;
    }
    snprintf(err, errlen, "unknown op '%s' (expected set, push, pop, insert, remove, move, increment)", kind);
    return -1;
}

int store_apply_ops(Store *st, Json *widget, const Json *ops, char *err, size_t errlen) {
    if (!ops || ops->type != JSON_ARRAY) { snprintf(err, errlen, "ops must be an array"); return -1; }
    Json *state = json_get(widget, "state");
    Json *work = json_clone(state);
    for (size_t i = 0; i < json_len(ops); i++) {
        const Json *op = json_at(ops, i);
        if (!op || op->type != JSON_OBJECT) { snprintf(err, errlen, "op %zu is not an object", i); json_free(work); return -1; }
        char inner[200];
        if (apply_one(work, op, inner, sizeof inner) != 0) {
            snprintf(err, errlen, "op %zu failed: %s", i, inner);
            json_free(work);
            return -1;
        }
    }
    return store_set_state(st, widget, work);
}

int store_set_state(Store *st, Json *widget, Json *state) {
    int id = (int)json_num(json_get(widget, "id"), -1);
    Json *old = json_detach_key(widget, "state");
    push_undo(st, id, old);
    json_set(widget, "state", state);
    store_save(st);
    return 0;
}

int store_undo(Store *st, Json *widget) {
    int id = (int)json_num(json_get(widget, "id"), -1);
    Stack *h = undo_stack(st, id, 0);
    if (!h || stack_is_empty(h)) return -1;
    Json *prev = stack_pop(h);
    json_set(widget, "state", prev);
    store_save(st);
    return 0;
}

/* ---------- rendering ---------- */

char *store_render_widget(const Store *st, const Json *widget, char *err, size_t errlen) {
    const Json *def = store_get_type(st, json_str(json_get(widget, "type"), ""));
    if (!def) { snprintf(err, errlen, "widget type no longer exists"); return NULL; }
    const Json *ctxs[2] = { widget, json_get(widget, "state") };
    return template_render_stack(json_str(json_get(def, "template"), ""), ctxs, 2, err, errlen);
}

char *store_render_preview(const char *tpl, const Json *state, char *err, size_t errlen) {
    Json *meta = json_object();
    json_set(meta, "id", json_number(0));
    json_set(meta, "type", json_string("preview"));
    json_set(meta, "title", json_string("Preview"));
    const Json *ctxs[2] = { meta, state };
    char *out = template_render_stack(tpl, ctxs, state ? 2 : 1, err, errlen);
    json_free(meta);
    return out;
}
