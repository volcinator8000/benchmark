/* main.c - routes for the widget board.
 *
 * HTML:  GET /                 dashboard
 *        GET /studio           widget type editor
 *        GET /widgets/:id      one rendered widget card (HTML fragment)
 *        GET /types.css        concatenated CSS of all widget types
 *        GET /static/...       static assets
 * API:   see README.md
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "http.h"
#include "json.h"
#include "store.h"
#include "tmpl.h"

#define TEMPLATE_DIR "templates"
#define BUILTIN_DIR  "widgets"
#define STATIC_DIR   "static"

static const char *PAGE_NAMES[] = { "layout", "dashboard", "widget", "studio" };
#define NPAGES (sizeof PAGE_NAMES / sizeof *PAGE_NAMES)

typedef struct {
    Store store;
    Tmpl *pages[NPAGES];
} App;

/* ---------- helpers ---------- */

static const Tmpl *page_lookup(const char *name, void *ud) {
    App *app = ud;
    for (size_t i = 0; i < NPAGES; i++)
        if (strcmp(PAGE_NAMES[i], name) == 0) return app->pages[i];
    return NULL;
}

/* Widget templates may include other widget types: {{> counter}} */
static const Tmpl *type_lookup(const char *name, void *ud) {
    App *app = ud;
    const WType *t = store_type(&app->store, name);
    return t ? t->tmpl : NULL;
}

static void reply_json(HttpRes *res, int status, Json *v) {
    res->status = status;
    res->ctype = "application/json";
    json_dump(v, &res->body, 0);
    json_free(v);
}

static void reply_error(HttpRes *res, int status, const char *msg) {
    Json *o = json_obj();
    json_set(o, "error", json_str(msg));
    reply_json(res, status, o);
}

static Json *parse_body(HttpReq *req, HttpRes *res) {
    char err[256];
    Json *v = json_parse(req->body, req->body_len, err, sizeof err);
    if (!v) {
        char msg[300];
        snprintf(msg, sizeof msg, "invalid JSON body: %s", err);
        reply_error(res, 400, msg);
        return NULL;
    }
    if (v->type != J_OBJ) {
        json_free(v);
        reply_error(res, 400, "JSON body must be an object");
        return NULL;
    }
    return v;
}

/* Matches "/prefix/<number><suffix>" exactly. */
static int match_id(const char *path, const char *prefix, const char *suffix, long *id) {
    size_t pl = strlen(prefix);
    if (strncmp(path, prefix, pl) != 0) return 0;
    char *end;
    long v = strtol(path + pl, &end, 10);
    if (end == path + pl || v <= 0) return 0;
    if (strcmp(end, suffix) != 0) return 0;
    *id = v;
    return 1;
}

static int is(const HttpReq *req, const char *method) { return strcmp(req->method, method) == 0; }

/* Render the widget's own template, then wrap it in the card frame. */
static void render_card(App *app, Json *w, Buf *out) {
    const char *type = json_get_str(w, "type");
    const WType *t = store_type(&app->store, type ? type : "");
    Buf body;
    buf_init(&body);
    char err[256];

    if (!t) {
        buf_printf(&body, "<div class=\"tmpl-error\">Unknown widget type</div>");
    } else {
        Json *vars = json_obj();
        json_set(vars, "widget", json_clone(w));
        json_del(json_get(vars, "widget"), "data");
        if (tmpl_render(t->tmpl, json_get(w, "data"), vars, type_lookup, app, &body, err, sizeof err) < 0) {
            buf_reset(&body);
            buf_puts(&body, "<div class=\"tmpl-error\">Template error: ");
            buf_html_escape(&body, err, strlen(err));
            buf_puts(&body, "</div>");
        }
        json_free(vars);
    }

    Json *ctx = json_clone(w);
    json_del(ctx, "data");
    json_set(ctx, "body", json_str(body.data ? body.data : ""));
    if (t) json_set(ctx, "type_label", json_str(json_get_str(t->def, "label")));
    if (tmpl_render(app->pages[2], ctx, NULL, page_lookup, app, out, err, sizeof err) < 0)
        buf_printf(out, "<!-- card error: %s -->", err);
    json_free(ctx);
    buf_free(&body);
}

static void render_page(App *app, HttpRes *res, int page, const char *title, Json *ctx) {
    Buf inner;
    buf_init(&inner);
    char err[256];
    if (tmpl_render(app->pages[page], ctx, NULL, page_lookup, app, &inner, err, sizeof err) < 0) {
        res->status = 500;
        res->ctype = "text/plain";
        buf_printf(&res->body, "template error: %s", err);
        buf_free(&inner);
        json_free(ctx);
        return;
    }
    Json *lctx = json_obj();
    json_set(lctx, "title", json_str(title));
    json_set(lctx, "body", json_str(inner.data ? inner.data : ""));
    json_set(lctx, "page", json_str(PAGE_NAMES[page]));
    if (tmpl_render(app->pages[0], lctx, NULL, page_lookup, app, &res->body, err, sizeof err) < 0) {
        res->status = 500;
        res->ctype = "text/plain";
        buf_reset(&res->body);
        buf_printf(&res->body, "layout error: %s", err);
    }
    json_free(lctx);
    json_free(ctx);
    buf_free(&inner);
}

static Json *types_summary(App *app, int full) {
    Json *arr = json_arr();
    for (size_t i = 0; i < app->store.ntypes; i++) {
        Json *d = json_clone(app->store.types[i].def);
        if (!full) { json_del(d, "template"); json_del(d, "css"); json_del(d, "defaults"); }
        json_arr_push(arr, d);
    }
    return arr;
}

static Json *widget_response(App *app, Json *w) {
    Json *o = json_obj();
    json_set(o, "widget", json_clone(w));
    Buf html;
    buf_init(&html);
    render_card(app, w, &html);
    json_set(o, "html", json_str(html.data ? html.data : ""));
    buf_free(&html);
    return o;
}

static void save(App *app) {
    if (store_save(&app->store) < 0) fprintf(stderr, "warning: failed to save database\n");
}

/* ---------- static files ---------- */

static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".css")) return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js")) return "text/javascript; charset=utf-8";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".svg")) return "image/svg+xml";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".ico")) return "image/x-icon";
    if (!strcmp(dot, ".json")) return "application/json";
    return "application/octet-stream";
}

static void serve_static(HttpReq *req, HttpRes *res) {
    const char *rel = req->path + strlen("/static/");
    if (!*rel || strstr(rel, "..") || strchr(rel, '\\')) {
        res->status = 404;
        buf_puts(&res->body, "not found");
        return;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s", STATIC_DIR, rel);
    size_t len;
    char *data = read_file(path, &len);
    if (!data) {
        res->status = 404;
        res->ctype = "text/plain";
        buf_puts(&res->body, "not found");
        return;
    }
    res->ctype = mime_for(path);
    buf_append(&res->body, data, len);
    free(data);
}

/* ---------- API: widgets ---------- */

static void api_widgets(App *app, HttpReq *req, HttpRes *res) {
    long id;
    char err[256];

    if (strcmp(req->path, "/api/widgets") == 0) {
        if (is(req, "GET")) { reply_json(res, 200, json_clone(app->store.widgets)); return; }
        if (is(req, "POST")) {
            Json *body = parse_body(req, res);
            if (!body) return;
            Json *w = store_add_widget(&app->store, json_get_str(body, "type"),
                                       json_get_str(body, "title"), json_get(body, "data"),
                                       err, sizeof err);
            json_free(body);
            if (!w) { reply_error(res, 422, err); return; }
            save(app);
            reply_json(res, 201, widget_response(app, w));
            return;
        }
        reply_error(res, 405, "use GET or POST");
        return;
    }

    if (match_id(req->path, "/api/widgets/", "/op", &id)) {
        if (!is(req, "POST")) { reply_error(res, 405, "use POST"); return; }
        Json *w = store_widget(&app->store, id, NULL);
        if (!w) { reply_error(res, 404, "no such widget"); return; }
        Json *op = parse_body(req, res);
        if (!op) return;
        int rc = store_apply_op(w, op, err, sizeof err);
        json_free(op);
        if (rc < 0) { reply_error(res, 422, err); return; }
        save(app);
        reply_json(res, 200, widget_response(app, w));
        return;
    }

    if (match_id(req->path, "/api/widgets/", "", &id)) {
        Json *w = store_widget(&app->store, id, NULL);
        if (!w) { reply_error(res, 404, "no such widget"); return; }
        if (is(req, "GET")) { reply_json(res, 200, widget_response(app, w)); return; }
        if (is(req, "DELETE")) {
            store_delete_widget(&app->store, id);
            save(app);
            reply_json(res, 200, json_obj());
            return;
        }
        if (is(req, "PATCH") || is(req, "PUT")) {
            Json *body = parse_body(req, res);
            if (!body) return;
            Json *data = json_get(body, "data");
            if (data && data->type != J_OBJ) {
                json_free(body);
                reply_error(res, 422, "'data' must be an object");
                return;
            }
            const char *title = json_get_str(body, "title");
            const char *size = json_get_str(body, "size");
            if (size && strcmp(size, "normal") && strcmp(size, "wide") && strcmp(size, "full")) {
                json_free(body);
                reply_error(res, 422, "'size' must be normal, wide or full");
                return;
            }
            if (title) json_set(w, "title", json_str(title));
            if (size) json_set(w, "size", json_str(size));
            if (data) json_set(w, "data", json_clone(data));
            json_free(body);
            save(app);
            reply_json(res, 200, widget_response(app, w));
            return;
        }
        reply_error(res, 405, "use GET, PATCH or DELETE");
        return;
    }

    reply_error(res, 404, "unknown endpoint");
}

/* ---------- API: types ---------- */

static void api_types(App *app, HttpReq *req, HttpRes *res) {
    char err[256];

    if (strcmp(req->path, "/api/types") == 0) {
        if (is(req, "GET")) { reply_json(res, 200, types_summary(app, 1)); return; }
        if (is(req, "POST")) {
            Json *body = parse_body(req, res);
            if (!body) return;
            int rc = store_put_type(&app->store, body, err, sizeof err);
            if (rc < 0) { json_free(body); reply_error(res, 422, err); return; }
            save(app);
            const WType *t = store_type(&app->store, json_get_str(body, "name"));
            json_free(body);
            reply_json(res, 201, json_clone(t->def));
            return;
        }
        reply_error(res, 405, "use GET or POST");
        return;
    }

    const char *name = req->path + strlen("/api/types/");
    const WType *t = store_type(&app->store, name);
    if (is(req, "GET")) {
        if (!t) { reply_error(res, 404, "no such type"); return; }
        reply_json(res, 200, json_clone(t->def));
        return;
    }
    if (is(req, "DELETE")) {
        int rc = store_delete_type(&app->store, name, err, sizeof err);
        if (rc == -1) { reply_error(res, 404, err); return; }
        if (rc < 0) { reply_error(res, 409, err); return; }
        save(app);
        reply_json(res, 200, json_obj());
        return;
    }
    reply_error(res, 405, "use GET or DELETE (create/update with POST /api/types)");
}

/* POST /api/preview {template, data?, css?} -> {html} | {error} */
static void api_preview(App *app, HttpReq *req, HttpRes *res) {
    if (!is(req, "POST")) { reply_error(res, 405, "use POST"); return; }
    Json *body = parse_body(req, res);
    if (!body) return;
    const char *src = json_get_str(body, "template");
    if (!src) { json_free(body); reply_error(res, 422, "'template' is required"); return; }

    char err[256];
    Tmpl *t = tmpl_compile(src, err, sizeof err);
    if (!t) { json_free(body); reply_error(res, 422, err); return; }

    Json *data = json_get(body, "data");
    Json *owned = NULL;
    if (!data || data->type != J_OBJ) data = owned = json_obj();
    Json *vars = json_obj();
    Json *wv = json_obj();
    json_set(wv, "id", json_num(0));
    json_set(wv, "title", json_str("Preview"));
    json_set(vars, "widget", wv);

    Buf out;
    buf_init(&out);
    int rc = tmpl_render(t, data, vars, type_lookup, app, &out, err, sizeof err);
    if (rc < 0) reply_error(res, 422, err);
    else {
        Json *o = json_obj();
        json_set(o, "html", json_str(out.data ? out.data : ""));
        reply_json(res, 200, o);
    }
    buf_free(&out);
    json_free(vars);
    json_free(owned);
    json_free(body);
    tmpl_free(t);
}

/* ---------- router ---------- */

static void handler(HttpReq *req, HttpRes *res, void *ud) {
    App *app = ud;
    const char *p = req->path;
    long id;

    if (strncmp(p, "/api/widgets", 12) == 0 && (p[12] == '\0' || p[12] == '/')) { api_widgets(app, req, res); return; }
    if (strncmp(p, "/api/types", 10) == 0 && (p[10] == '\0' || p[10] == '/')) { api_types(app, req, res); return; }
    if (strcmp(p, "/api/preview") == 0) { api_preview(app, req, res); return; }
    if (strncmp(p, "/api/", 5) == 0) { reply_error(res, 404, "unknown endpoint"); return; }

    if (!is(req, "GET") && !is(req, "HEAD")) {
        res->status = 405;
        res->ctype = "text/plain";
        buf_puts(&res->body, "method not allowed");
        return;
    }

    if (strncmp(p, "/static/", 8) == 0) { serve_static(req, res); return; }

    if (strcmp(p, "/types.css") == 0) {
        res->ctype = "text/css; charset=utf-8";
        for (size_t i = 0; i < app->store.ntypes; i++) {
            const char *css = json_get_str(app->store.types[i].def, "css");
            if (css && *css)
                buf_printf(&res->body, "/* %s */\n%s\n", json_get_str(app->store.types[i].def, "name"), css);
        }
        return;
    }

    if (match_id(p, "/widgets/", "", &id)) {
        Json *w = store_widget(&app->store, id, NULL);
        if (!w) { res->status = 404; buf_puts(&res->body, "no such widget"); return; }
        render_card(app, w, &res->body);
        return;
    }

    if (strcmp(p, "/") == 0) {
        Json *ctx = json_obj();
        Json *cards = json_arr();
        for (size_t i = 0; i < app->store.widgets->len; i++) {
            Buf html;
            buf_init(&html);
            render_card(app, app->store.widgets->items[i], &html);
            json_arr_push(cards, json_str(html.data ? html.data : ""));
            buf_free(&html);
        }
        json_set(ctx, "cards", cards);
        json_set(ctx, "types", types_summary(app, 0));
        render_page(app, res, 1, "Board", ctx);
        return;
    }

    if (strcmp(p, "/studio") == 0) {
        Json *ctx = json_obj();
        json_set(ctx, "types", types_summary(app, 0));
        render_page(app, res, 3, "Widget Studio", ctx);
        return;
    }

    res->status = 404;
    res->ctype = "text/plain";
    buf_puts(&res->body, "not found");
}

/* ---------- startup ---------- */

static void seed(App *app) {
    char err[256];
    Json *w;
    if (store_type(&app->store, "kanban")) {
        w = store_add_widget(&app->store, "kanban", "Sprint board", NULL, err, sizeof err);
        if (w) json_set(w, "size", json_str("full"));
    }
    if (store_type(&app->store, "checklist"))
        store_add_widget(&app->store, "checklist", "Today", NULL, err, sizeof err);
    if (store_type(&app->store, "counter"))
        store_add_widget(&app->store, "counter", "Coffees", NULL, err, sizeof err);
    if (store_type(&app->store, "note"))
        store_add_widget(&app->store, "note", "Scratchpad", NULL, err, sizeof err);
    save(app);
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    const char *host = argc > 2 ? argv[2] : "127.0.0.1";
    const char *db = getenv("BOARD_DB") ? getenv("BOARD_DB") : "board.json";

    static App app;
    for (size_t i = 0; i < NPAGES; i++) {
        char path[256], err[256];
        snprintf(path, sizeof path, "%s/%s.html", TEMPLATE_DIR, PAGE_NAMES[i]);
        char *src = read_file(path, NULL);
        if (!src) { fprintf(stderr, "error: can't read %s (run from the project root)\n", path); return 1; }
        app.pages[i] = tmpl_compile(src, err, sizeof err);
        free(src);
        if (!app.pages[i]) { fprintf(stderr, "error: %s: %s\n", path, err); return 1; }
    }

    int rc = store_open(&app.store, db, BUILTIN_DIR);
    if (rc < 0) return 1;
    if (rc == 1) seed(&app);
    fprintf(stderr, "%zu widget types, %zu widgets (db: %s)\n",
            app.store.ntypes, app.store.widgets->len, db);

    return http_serve(host, port, handler, &app) < 0 ? 1 : 0;
}
