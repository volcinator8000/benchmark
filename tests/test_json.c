#include "../src/json.h"
#include "test_util.h"

static Json *parse(const char *s) {
    char err[128];
    Json *j = json_parse(s, strlen(s), err, sizeof err);
    if (!j) fprintf(stderr, "  parse error: %s\n", err);
    return j;
}

int main(void) {
    /* round trip */
    Json *j = parse("{\"a\":1,\"b\":[true,false,null,\"x\\n\\u00e9\\ud83d\\ude00\"],\"c\":{\"d\":-2.5e3}}");
    CHECK(j != NULL);
    char *s = json_stringify(j, 0);
    CHECK_STR(s, "{\"a\":1,\"b\":[true,false,null,\"x\\n\xc3\xa9\xf0\x9f\x98\x80\"],\"c\":{\"d\":-2500}}");
    free(s);
    CHECK(json_num(json_get(j, "a"), 0) == 1);
    CHECK(json_len(json_get(j, "b")) == 4);
    CHECK(json_path_get(j, "c.d")->num == -2500);
    CHECK(json_path_get(j, "b[0]")->b == 1);
    CHECK(json_path_get(j, "b.3")->type == JSON_STRING);
    CHECK(json_path_get(j, "b[9]") == NULL);
    CHECK(json_path_get(j, "nope.x") == NULL);
    CHECK(json_path_get(j, "") == j);

    /* path set / insert / detach */
    CHECK(json_path_set(j, "c.e", json_string("new")) == 0);
    CHECK_STR(json_str(json_path_get(j, "c.e"), NULL), "new");
    CHECK(json_path_set(j, "b[4]", json_number(4)) == 0);       /* append at len */
    CHECK(json_path_set(j, "b[-]", json_number(5)) == 0);       /* append with - */
    CHECK(json_len(json_get(j, "b")) == 6);
    CHECK(json_path_set(j, "b[10]", json_number(9)) == -1);
    CHECK(json_path_set(j, "missing.k", json_number(9)) == -1);
    CHECK(json_path_insert(j, "b[0]", json_string("first")) == 0);
    CHECK_STR(json_str(json_path_get(j, "b[0]"), NULL), "first");
    Json *d = json_path_detach(j, "b[0]");
    CHECK(d && d->type == JSON_STRING);
    json_free(d);
    CHECK(json_path_get(j, "b[0]")->type == JSON_BOOL);
    d = json_path_detach(j, "c.d");
    CHECK(d && d->num == -2500);
    json_free(d);
    CHECK(json_path_get(j, "c.d") == NULL);
    CHECK(json_path_detach(j, "zzz") == NULL);

    /* clone is deep */
    Json *k = json_clone(j);
    json_path_set(k, "a", json_number(42));
    CHECK(json_num(json_get(j, "a"), 0) == 1);
    json_free(k);
    json_free(j);

    /* errors */
    char err[128];
    CHECK(json_parse("{\"a\":}", 6, err, sizeof err) == NULL);
    CHECK(json_parse("[1,2", 4, err, sizeof err) == NULL);
    CHECK(json_parse("\"abc", 4, err, sizeof err) == NULL);
    CHECK(json_parse("01", 2, err, sizeof err) == NULL);
    CHECK(json_parse("{} x", 4, err, sizeof err) == NULL);
    CHECK(json_parse("\"\\q\"", 4, err, sizeof err) == NULL);
    CHECK(json_parse("", 0, err, sizeof err) == NULL);
    CHECK(strlen(err) > 0);

    /* deep nesting is rejected, not a crash */
    char deep[1000];
    memset(deep, '[', sizeof deep - 1);
    deep[sizeof deep - 1] = '\0';
    CHECK(json_parse(deep, strlen(deep), err, sizeof err) == NULL);

    /* string escaping */
    Json *str = json_string("a\"b\\c\n\x01");
    s = json_stringify(str, 0);
    CHECK_STR(s, "\"a\\\"b\\\\c\\n\\u0001\"");
    free(s);
    json_free(str);

    /* numbers */
    Json *n = parse("[1.5, 100000000000000000000, 3, -0]");
    s = json_stringify(n, 0);
    CHECK_STR(s, "[1.5,1e+20,3,-0]");
    free(s);
    json_free(n);

    /* pretty */
    Json *pj = parse("{\"a\":[1,{\"b\":2}]}");
    s = json_stringify(pj, 1);
    CHECK_STR(s, "{\n  \"a\": [\n    1,\n    {\n      \"b\": 2\n    }\n  ]\n}\n");
    free(s);
    json_free(pj);

    /* truthiness */
    CHECK(!json_truthy(NULL));
    Json *e = parse("[]"); CHECK(!json_truthy(e)); json_free(e);
    e = parse("\"\""); CHECK(!json_truthy(e)); json_free(e);
    e = parse("0"); CHECK(!json_truthy(e)); json_free(e);
    e = parse("{}"); CHECK(json_truthy(e)); json_free(e);

    /* object key replace and detach */
    Json *o = json_object();
    json_set(o, "k", json_number(1));
    json_set(o, "k", json_number(2));
    CHECK(json_len(o) == 1 && json_num(json_get(o, "k"), 0) == 2);
    Json *dk = json_detach_key(o, "k");
    CHECK(dk && json_len(o) == 0);
    json_free(dk);
    json_free(o);

    TEST_MAIN_END();
}
