#ifndef JSON_H
#define JSON_H

#include <stddef.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct Json Json;

struct Json {
    JsonType type;
    union {
        int b;
        double num;
        char *str;
        struct { Json **items; size_t len, cap; } arr;
        struct { char **keys; Json **vals; size_t len, cap; } obj;
    };
};

/* Constructors. Every Json* is heap allocated and owned by its holder. */
Json *json_null(void);
Json *json_bool(int b);
Json *json_number(double n);
Json *json_string(const char *s);
Json *json_string_n(const char *s, size_t n);
Json *json_array(void);
Json *json_object(void);
Json *json_clone(const Json *j);
void json_free(Json *j);

/* Parsing and serialising. On error json_parse returns NULL and fills err. */
Json *json_parse(const char *text, size_t len, char *err, size_t errlen);
char *json_stringify(const Json *j, int pretty);

/* Object access. Setting takes ownership of val and replaces an existing key. */
Json *json_get(const Json *obj, const char *key);
Json *json_get_n(const Json *obj, const char *key, size_t keylen);
void json_set(Json *obj, const char *key, Json *val);
Json *json_detach_key(Json *obj, const char *key);

/* Array access. */
void json_push(Json *arr, Json *val);
Json *json_at(const Json *arr, size_t i);
void json_insert(Json *arr, size_t i, Json *val);
Json *json_detach_index(Json *arr, size_t i);
size_t json_len(const Json *j);

/*
 * JSON paths: "columns[1].cards[0].title" or "columns.1.cards.0.title".
 * An empty path means the root. json_path_set creates the key or
 * replaces the value at the path; for arrays an index equal to the
 * length (or "-") appends. Functions return 0 on success and -1 if the
 * path does not resolve.
 */
Json *json_path_get(const Json *root, const char *path);
int json_path_set(Json *root, const char *path, Json *val);
int json_path_insert(Json *root, const char *path, Json *val);
Json *json_path_detach(Json *root, const char *path);

/* Helpers. */
int json_truthy(const Json *j);
const char *json_str(const Json *j, const char *def);
double json_num(const Json *j, double def);
const char *json_type_name(JsonType t);

#endif
