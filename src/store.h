/* store.h - widget types + widget instances, persisted to a JSON file.
 *
 * A widget *type* is { name, label, template, css, defaults, builtin }.
 * A widget *instance* is { id, type, title, size, data }.
 * The instance's `data` is rendered through its type's template.
 */
#ifndef STORE_H
#define STORE_H

#include "json.h"
#include "tmpl.h"

typedef struct {
    Json *def;   /* the type definition object (owned) */
    Tmpl *tmpl;  /* compiled template (owned) */
} WType;

typedef struct {
    WType  *types;
    size_t  ntypes;
    Json   *widgets;   /* J_ARR of instance objects */
    long    next_id;
    char   *db_path;
} Store;

int  store_open(Store *st, const char *db_path, const char *builtin_dir);
int  store_save(const Store *st);

const WType *store_type(const Store *st, const char *name);
/* Creates or replaces a user type. def is borrowed (cloned). */
int  store_put_type(Store *st, const Json *def, char *err, size_t errlen);
int  store_delete_type(Store *st, const char *name, char *err, size_t errlen);

Json *store_widget(const Store *st, long id, size_t *index_out);
Json *store_add_widget(Store *st, const char *type, const char *title,
                       const Json *data, char *err, size_t errlen);
int   store_delete_widget(Store *st, long id);

/* Apply a data op ({op, path, value, to, index, by}) to widget["data"]. */
int store_apply_op(Json *widget, const Json *op, char *err, size_t errlen);

int   valid_type_name(const char *s);
char *read_file(const char *path, size_t *len_out); /* NULL on failure */

#endif
