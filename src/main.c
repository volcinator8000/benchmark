/*
 * widgets - a small widget dashboard server written in C.
 *
 *   GET    /                         dashboard page (rendered with the template engine)
 *   GET    /static/<file>            frontend assets from ./web
 *   GET    /api/widget-types         list widget types (built-in + user defined)
 *   POST   /api/widget-types         create a custom widget type
 *   GET    /api/widget-types/:name
 *   PUT    /api/widget-types/:name   update a custom widget type
 *   DELETE /api/widget-types/:name
 *   GET    /api/widgets              list widget instances
 *   POST   /api/widgets              {type, title?, state?} -> create an instance
 *   GET    /api/widgets/:id
 *   PATCH  /api/widgets/:id          {title?, state?}
 *   DELETE /api/widgets/:id
 *   GET    /api/widgets/:id/render   rendered HTML for the widget
 *   POST   /api/widgets/:id/ops      {ops:[{op,path,value|from,to}...]} -> mutate state
 *   POST   /api/widgets/:id/undo     revert the last mutation
 *   POST   /api/render               {template, state} -> {html} (preview)
 */
#include "http.h"
#include "json.h"
#include "store.h"
#include "template.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Store store;
    char web_dir[512];
} App;

/* ---------- response helpers ---------- */

static void reply_json(HttpResponse *res, int status, Json *j) {
    char *text = json_stringify(j, 0);
    http_reply(res, status, "application/json; charset=utf-8", text, strlen(text));
    free(text);
    json_free(j);
}

static void reply_error(HttpResponse *res, int status, const char *msg) {
    Json *j = json_object();
    json_set(j, "error", json_string(msg));
    reply_json(res, status, j);
}

static void reply_html(HttpResponse *res, int status, char *html_owned) {
    http_reply(res, status, "text/html; charset=utf-8", html_owned, strlen(html_owned));
    free(html_owned);
}

/* Parses a JSON object body; replies 400 and returns NULL on failure. */
static Json *parse_body(const HttpRequest *req, HttpResponse *res) {
    char err[200];
    if (req->body_len == 0) { reply_error(res, 400, "request body must be a JSON object"); return NULL; }
    Json *j = json_parse(req->body, req->body_len, err, sizeof err);
    if (!j) {
        char msg[256];
        snprintf(msg, sizeof msg, "invalid JSON: %s", err);
        reply_error(res, 400, msg);
        return NULL;
    }
    if (j->type != JSON_OBJECT) { json_free(j); reply_error(res, 400, "request body must be a JSON object"); return NULL; }
    return j;
}

static Json *widget_payload(App *app, Json *w) {
    Json *out = json_object();
    json_set(out, "widget", json_clone(w));
    char err[256];
    char *html = store_render_widget(&app->store, w, err, sizeof err);
    if (html) { json_set(out, "html", json_string(html)); free(html); }
    else { json_set(out, "html", json_null()); json_set(out, "render_error", json_string(err)); }
    return out;
}

/* Matches "/prefix/<segment>[/rest]". Copies the segment; sets *rest to what follows (or ""). */
static int match_segment(const char *path, const char *prefix, char *seg, size_t seglen, const char **rest) {
    size_t n = strlen(prefix);
    if (strncmp(path, prefix, n) != 0 || path[n] == '\0') return 0;
    const char *s = path + n;
    const char *e = strchr(s, '/');
    size_t len = e ? (size_t)(e - s) : strlen(s);
    if (len == 0 || len >= seglen) return 0;
    memcpy(seg, s, len);
    seg[len] = '\0';
    *rest = e ? e : "";
    return 1;
}

static int is_method(const HttpRequest *req, const char *m) { return strcmp(req->method, m) == 0; }

/* ---------- routes ---------- */

static void route_index(App *app, HttpResponse *res) {
    char path[1024];
    snprintf(path, sizeof path, "%s/index.html", app->web_dir);
    char *tpl = read_file(path, NULL);
    if (!tpl) { reply_error(res, 500, "web/index.html is missing"); return; }

    Json *ctx = json_object();
    Json *types = json_array();
    for (size_t i = 0; i < json_len(app->store.types); i++) {
        Json *t = json_at(app->store.types, i);
        Json *brief = json_object();
        json_set(brief, "name", json_clone(json_get(t, "name")));
        json_set(brief, "description", json_clone(json_get(t, "description")));
        json_set(brief, "builtin", json_clone(json_get(t, "builtin")));
        json_push(types, brief);
    }
    json_set(ctx, "types", types);
    json_set(ctx, "widget_count", json_number((double)json_len(app->store.widgets)));
    json_set(ctx, "type_count", json_number((double)json_len(app->store.types)));

    char err[256];
    char *html = template_render(tpl, ctx, err, sizeof err);
    free(tpl);
    json_free(ctx);
    if (!html) { reply_error(res, 500, err); return; }
    reply_html(res, 200, html);
}

static void route_static(App *app, const HttpRequest *req, HttpResponse *res) {
    const char *rel = req->path + strlen("/static/");
    if (strstr(rel, "..") || rel[0] == '/' || rel[0] == '\0') { reply_error(res, 404, "not found"); return; }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", app->web_dir, rel);
    size_t len;
    char *data = read_file(path, &len);
    if (!data) { reply_error(res, 404, "not found"); return; }
    http_reply(res, 200, http_mime_for_path(path), data, len);
    free(data);
}

static void route_types(App *app, const HttpRequest *req, HttpResponse *res) {
    char err[256];
    if (is_method(req, "GET")) {
        Json *out = json_object();
        json_set(out, "types", json_clone(app->store.types));
        reply_json(res, 200, out);
        return;
    }
    if (is_method(req, "POST")) {
        Json *def = parse_body(req, res);
        if (!def) return;
        const char *name = json_str(json_get(def, "name"), "");
        if (store_get_type(&app->store, name)) {
            json_free(def);
            reply_error(res, 409, "a widget type with that name already exists (use PUT to update it)");
            return;
        }
        if (store_put_type(&app->store, def, 0, err, sizeof err) != 0) { json_free(def); reply_error(res, 422, err); return; }
        store_save(&app->store);
        Json *out = json_object();
        json_set(out, "type", json_clone(store_get_type(&app->store, name)));
        reply_json(res, 201, out);
        return;
    }
    reply_error(res, 405, "method not allowed");
}

static void route_type(App *app, const HttpRequest *req, HttpResponse *res, const char *name) {
    char err[256];
    Json *existing = store_get_type(&app->store, name);
    if (is_method(req, "GET")) {
        if (!existing) { reply_error(res, 404, "no such widget type"); return; }
        Json *out = json_object();
        json_set(out, "type", json_clone(existing));
        reply_json(res, 200, out);
        return;
    }
    if (is_method(req, "PUT")) {
        Json *def = parse_body(req, res);
        if (!def) return;
        json_set(def, "name", json_string(name));
        if (store_put_type(&app->store, def, 0, err, sizeof err) != 0) { json_free(def); reply_error(res, 422, err); return; }
        store_save(&app->store);
        Json *out = json_object();
        json_set(out, "type", json_clone(store_get_type(&app->store, name)));
        reply_json(res, existing ? 200 : 201, out);
        return;
    }
    if (is_method(req, "DELETE")) {
        if (store_delete_type(&app->store, name, err, sizeof err) != 0) { reply_error(res, existing ? 409 : 404, err); return; }
        store_save(&app->store);
        Json *out = json_object();
        json_set(out, "deleted", json_string(name));
        reply_json(res, 200, out);
        return;
    }
    reply_error(res, 405, "method not allowed");
}

static void route_widgets(App *app, const HttpRequest *req, HttpResponse *res) {
    if (is_method(req, "GET")) {
        Json *out = json_object();
        json_set(out, "widgets", json_clone(app->store.widgets));
        reply_json(res, 200, out);
        return;
    }
    if (is_method(req, "POST")) {
        Json *body = parse_body(req, res);
        if (!body) return;
        char err[256];
        Json *state = json_detach_key(body, "state");
        if (state && state->type == JSON_NULL) { json_free(state); state = NULL; }
        Json *w = store_create_widget(&app->store, json_str(json_get(body, "type"), NULL),
                                      json_str(json_get(body, "title"), ""), state, err, sizeof err);
        json_free(body);
        if (!w) { reply_error(res, 422, err); return; }
        reply_json(res, 201, widget_payload(app, w));
        return;
    }
    reply_error(res, 405, "method not allowed");
}

static void route_widget(App *app, const HttpRequest *req, HttpResponse *res, int id, const char *rest) {
    Json *w = store_get_widget(&app->store, id);
    if (!w) { reply_error(res, 404, "no such widget"); return; }
    char err[256];

    if (strcmp(rest, "") == 0 || strcmp(rest, "/") == 0) {
        if (is_method(req, "GET")) { reply_json(res, 200, widget_payload(app, w)); return; }
        if (is_method(req, "DELETE")) {
            store_delete_widget(&app->store, id);
            Json *out = json_object();
            json_set(out, "deleted", json_number(id));
            reply_json(res, 200, out);
            return;
        }
        if (is_method(req, "PATCH") || is_method(req, "PUT")) {
            Json *body = parse_body(req, res);
            if (!body) return;
            Json *title = json_get(body, "title");
            if (title && title->type == JSON_STRING) json_set(w, "title", json_clone(title));
            Json *state = json_detach_key(body, "state");
            if (state && state->type != JSON_OBJECT) {
                json_free(state);
                json_free(body);
                reply_error(res, 400, "state must be a JSON object");
                return;
            }
            if (state) store_set_state(&app->store, w, state);
            else store_save(&app->store);
            json_free(body);
            reply_json(res, 200, widget_payload(app, w));
            return;
        }
        reply_error(res, 405, "method not allowed");
        return;
    }
    if (strcmp(rest, "/render") == 0) {
        if (!is_method(req, "GET")) { reply_error(res, 405, "method not allowed"); return; }
        char *html = store_render_widget(&app->store, w, err, sizeof err);
        if (!html) { reply_error(res, 500, err); return; }
        reply_html(res, 200, html);
        return;
    }
    if (strcmp(rest, "/ops") == 0) {
        if (!is_method(req, "POST")) { reply_error(res, 405, "method not allowed"); return; }
        Json *body = parse_body(req, res);
        if (!body) return;
        Json *ops = json_get(body, "ops");
        if (store_apply_ops(&app->store, w, ops, err, sizeof err) != 0) { json_free(body); reply_error(res, 422, err); return; }
        json_free(body);
        reply_json(res, 200, widget_payload(app, w));
        return;
    }
    if (strcmp(rest, "/undo") == 0) {
        if (!is_method(req, "POST")) { reply_error(res, 405, "method not allowed"); return; }
        if (store_undo(&app->store, w) != 0) { reply_error(res, 409, "nothing to undo"); return; }
        reply_json(res, 200, widget_payload(app, w));
        return;
    }
    reply_error(res, 404, "not found");
}

static void route_render(const HttpRequest *req, HttpResponse *res) {
    if (!is_method(req, "POST")) { reply_error(res, 405, "method not allowed"); return; }
    Json *body = parse_body(req, res);
    if (!body) return;
    const char *tpl = json_str(json_get(body, "template"), NULL);
    if (!tpl) { json_free(body); reply_error(res, 400, "template (string) is required"); return; }
    Json *state = json_get(body, "state");
    char err[256];
    char *html = store_render_preview(tpl, state, err, sizeof err);
    Json *out = json_object();
    if (html) { json_set(out, "html", json_string(html)); free(html); }
    else json_set(out, "error", json_string(err));
    json_free(body);
    reply_json(res, html ? 200 : 422, out);
}

static void handler(const HttpRequest *req, HttpResponse *res, void *ud) {
    App *app = ud;
    const char *path = req->path;
    char seg[128];
    const char *rest;

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) { route_index(app, res); return; }
    if (str_starts_with(path, "/static/")) { route_static(app, req, res); return; }
    if (strcmp(path, "/api/widget-types") == 0) { route_types(app, req, res); return; }
    if (match_segment(path, "/api/widget-types/", seg, sizeof seg, &rest) && rest[0] == '\0') {
        route_type(app, req, res, seg);
        return;
    }
    if (strcmp(path, "/api/widgets") == 0) { route_widgets(app, req, res); return; }
    if (match_segment(path, "/api/widgets/", seg, sizeof seg, &rest)) {
        char *end;
        long id = strtol(seg, &end, 10);
        if (*end != '\0') { reply_error(res, 404, "widget id must be a number"); return; }
        route_widget(app, req, res, (int)id, rest);
        return;
    }
    if (strcmp(path, "/api/render") == 0) { route_render(req, res); return; }
    reply_error(res, 404, "not found");
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--host 127.0.0.1] [--port 8080] [--data data] [--web web] [--builtin builtin]\n",
            argv0);
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = 8080;
    const char *data_dir = "data";
    const char *web_dir = "web";
    const char *builtin_dir = "builtin";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (strcmp(argv[i], "--web") == 0 && i + 1 < argc) web_dir = argv[++i];
        else if (strcmp(argv[i], "--builtin") == 0 && i + 1 < argc) builtin_dir = argv[++i];
        else { usage(argv[0]); return 2; }
    }

    static App app;
    snprintf(app.web_dir, sizeof app.web_dir, "%s", web_dir);
    if (store_init(&app.store, data_dir, builtin_dir) != 0) return 1;
    printf("%zu widget types, %zu widgets loaded from %s/\n",
           json_len(app.store.types), json_len(app.store.widgets), data_dir);
    printf("listening on http://%s:%d/\n", host, port);
    fflush(stdout);
    int rc = http_serve(host, port, handler, &app);
    store_free(&app.store);
    return rc == 0 ? 0 : 1;
}
