/* json.h - minimal JSON DOM: parse, serialize, clone, path access. */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include "buf.h"

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct Json Json;
struct Json {
    JType   type;
    int     b;       /* J_BOOL */
    double  n;       /* J_NUM */
    char   *s;       /* J_STR */
    size_t  len;     /* J_ARR / J_OBJ element count */
    size_t  cap;
    Json  **items;   /* J_ARR elements / J_OBJ values */
    char  **keys;    /* J_OBJ keys (parallel to items) */
};

Json *json_parse(const char *src, size_t len, char *err, size_t errlen);
void  json_free(Json *v);
Json *json_clone(const Json *v);
void  json_dump(const Json *v, Buf *out, int indent); /* indent 0 = compact */
char *json_to_string(const Json *v, int indent);

Json *json_null(void);
Json *json_bool(int b);
Json *json_num(double n);
Json *json_str(const char *s);
Json *json_arr(void);
Json *json_obj(void);

void  json_arr_push(Json *arr, Json *v);             /* takes ownership */
void  json_arr_insert(Json *arr, size_t i, Json *v); /* i clamped to len */
Json *json_arr_take(Json *arr, size_t i);            /* detach, caller owns */

Json       *json_get(const Json *obj, const char *key);
const char *json_get_str(const Json *obj, const char *key); /* NULL if not str */
void        json_set(Json *obj, const char *key, Json *v);  /* takes ownership */
int         json_del(Json *obj, const char *key);           /* 1 if removed */

/* Dotted paths: "columns.0.cards". Empty path == root. Numeric segments index
 * arrays. json_path_get returns a borrowed pointer or NULL. */
Json *json_path_get(Json *root, const char *path);
/* Splits path into parent container + last segment. Returns parent or NULL.
 * *last points into a heap copy stored in *scratch (caller frees). */
Json *json_path_parent(Json *root, const char *path, char **last, char **scratch);

int  json_truthy(const Json *v);
int  json_is_index(const char *seg, size_t *out);
void json_write_scalar(const Json *v, Buf *out); /* text form for templates */

#endif
