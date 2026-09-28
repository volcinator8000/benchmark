#ifndef STORE_H
#define STORE_H

#include "json.h"
#include "stack.h"

/*
 * Widget types describe how a widget renders and behaves:
 *   { name, description, template, default_state, script, style, builtin }
 * Widgets are instances:
 *   { id, type, title, state }
 * Built-in types come from builtin/<name>/{type.json,template.html,script.js,style.css}.
 * User-defined types persist to <data>/types.json, widgets to <data>/widgets.json.
 */
typedef struct {
    Json *types;      /* array of type objects, builtins first */
    Json *widgets;    /* array of widget objects */
    int next_id;
    char data_dir[512];
    /* per-widget undo history: previous state snapshots */
    struct { int id; Stack *history; } *undo;
    size_t undo_len, undo_cap;
} Store;

int store_init(Store *st, const char *data_dir, const char *builtin_dir);
void store_free(Store *st);
int store_save(Store *st);

Json *store_get_type(const Store *st, const char *name);
/* Validates and adds/replaces a type. Takes ownership of def on success (returns 0). */
int store_put_type(Store *st, Json *def, int builtin, char *err, size_t errlen);
int store_delete_type(Store *st, const char *name, char *err, size_t errlen);

Json *store_get_widget(const Store *st, int id);
/* Creates a widget from a type. state may be NULL to use the type's default. Returns the widget or NULL. */
Json *store_create_widget(Store *st, const char *type, const char *title, Json *state, char *err, size_t errlen);
int store_delete_widget(Store *st, int id);

/* Applies a list of ops to a widget's state atomically. On success the old state is
 * pushed onto the undo stack and the store is saved. Returns 0, or -1 with err filled. */
int store_apply_ops(Store *st, Json *widget, const Json *ops, char *err, size_t errlen);
/* Replaces the state wholesale (also undoable). Takes ownership of state. */
int store_set_state(Store *st, Json *widget, Json *state);
/* Restores the previous state. Returns -1 if there is nothing to undo. */
int store_undo(Store *st, Json *widget);

/* Renders a widget with its type's template. Returns malloc'd HTML or NULL. */
char *store_render_widget(const Store *st, const Json *widget, char *err, size_t errlen);
/* Renders an arbitrary template against a state (used for previews). */
char *store_render_preview(const char *tpl, const Json *state, char *err, size_t errlen);

#endif
