/* Unit tests for the stack, JSON and template engine. Run with `make test`. */
#include <stdio.h>
#include <string.h>

#include "json.h"
#include "stack.h"
#include "tmpl.h"

static int failures, total;

static void check_render(const char *tpl, const char *data, const char *want) {
    char err[256];
    total++;
    Json *d = json_parse(data, strlen(data), err, sizeof err);
    Tmpl *t = tmpl_compile(tpl, err, sizeof err);
    if (!d || !t) {
        printf("FAIL %s\n  setup error: %s\n", tpl, err);
        failures++;
    } else {
        Buf out;
        buf_init(&out);
        if (tmpl_render(t, d, NULL, NULL, NULL, &out, err, sizeof err) < 0 ||
            strcmp(out.data ? out.data : "", want) != 0) {
            printf("FAIL %s\n  want: %s\n  got:  %s %s\n", tpl, want, out.data ? out.data : "", err);
            failures++;
        }
        buf_free(&out);
    }
    json_free(d);
    tmpl_free(t);
}

static void check_compile_error(const char *tpl, const char *needle) {
    char err[256];
    total++;
    Tmpl *t = tmpl_compile(tpl, err, sizeof err);
    if (t || !strstr(err, needle)) {
        printf("FAIL compile error for %s\n  want substring: %s\n  got: %s\n", tpl, needle, t ? "(compiled)" : err);
        failures++;
    }
    tmpl_free(t);
}

int main(void) {
    /* stack */
    Stack s;
    stack_init(&s);
    int a = 1, b = 2, c = 3;
    stack_push(&s, &a); stack_push(&s, &b); stack_push(&s, &c);
    total++;
    if (stack_len(&s) != 3 || stack_peek(&s) != &c || stack_at(&s, 2) != &a ||
        stack_pop(&s) != &c || stack_pop(&s) != &b || stack_pop(&s) != &a ||
        stack_pop(&s) != NULL || !stack_empty(&s)) {
        printf("FAIL stack\n");
        failures++;
    }
    stack_free(&s);

    /* templates */
    check_render("Hi {{name}}!", "{\"name\":\"<b>Ann</b>\"}", "Hi &lt;b&gt;Ann&lt;/b&gt;!");
    check_render("{{{name}}}", "{\"name\":\"<b>x</b>\"}", "<b>x</b>");
    check_render("{{a.b.1}}", "{\"a\":{\"b\":[10,20]}}", "20");
    check_render("{{#each xs}}{{.}}{{#unless @last}},{{/unless}}{{/each}}", "{\"xs\":[1,2,3]}", "1,2,3");
    check_render("{{#each xs}}x{{else}}empty{{/each}}", "{\"xs\":[]}", "empty");
    check_render("{{#each m}}{{@key}}={{.}} {{/each}}", "{\"m\":{\"a\":1,\"b\":\"z\"}}", "a=1 b=z ");
    check_render("{{#if on}}yes{{else}}no{{/if}}", "{\"on\":false}", "no");
    check_render("{{#each cols}}{{#each cards}}[{{@path}}]{{/each}}{{/each}}",
                 "{\"cols\":[{\"cards\":[1]},{\"cards\":[2,3]}]}",
                 "[cols.0.cards.0][cols.1.cards.0][cols.1.cards.1]");
    check_render("{{#each xs}}{{name}}/{{title}} {{/each}}",
                 "{\"title\":\"T\",\"xs\":[{\"name\":\"a\"},{\"name\":\"b\"}]}", "a/T b/T ");
    check_render("{{#each xs}}{{../title}}{{@root.title}}{{/each}}", "{\"title\":\"T\",\"xs\":[1]}", "TT");
    check_render("{{#each xs}}{{#each ys}}{{@index}}{{/each}}{{@index}};{{/each}}",
                 "{\"xs\":[{\"ys\":[0,0]},{\"ys\":[0]}]}", "010;01;");
    check_render("{{xs.length}} {{s.length}}", "{\"xs\":[1,2],\"s\":\"abc\"}", "2 3");
    check_render("{{! hidden }}ok{{missing.path}}", "{}", "ok");
    check_render("{{n}} {{f}} {{b}}", "{\"n\":42,\"f\":1.5,\"b\":true}", "42 1.5 true");

    check_compile_error("{{#each xs}}", "never closed");
    check_compile_error("{{#if x}}{{/each}}", "closes");
    check_compile_error("line1\n{{/if}}", "line 2");
    check_compile_error("{{#bogus x}}{{/bogus}}", "unknown block");
    check_compile_error("{{name", "unclosed tag");

    printf("%d/%d tests passed\n", total - failures, total);
    return failures ? 1 : 0;
}
