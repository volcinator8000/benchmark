#include "../src/template.h"
#include "test_util.h"

static Json *parse(const char *s) {
    char err[128];
    Json *j = json_parse(s, strlen(s), err, sizeof err);
    if (!j) fprintf(stderr, "  parse error: %s\n", err);
    return j;
}

static void expect(const char *tpl, const char *ctx_json, const char *want) {
    Json *ctx = parse(ctx_json);
    char err[256];
    char *out = template_render(tpl, ctx, err, sizeof err);
    if (!out) fprintf(stderr, "  render error: %s\n", err);
    CHECK_STR(out, want);
    free(out);
    json_free(ctx);
}

static void expect_error(const char *tpl) {
    char err[256];
    Json *ctx = json_object();
    char *out = template_render(tpl, ctx, err, sizeof err);
    CHECK(out == NULL && err[0] != '\0');
    free(out);
    json_free(ctx);
}

int main(void) {
    expect("Hello {{name}}!", "{\"name\":\"World\"}", "Hello World!");
    expect("{{a}}|{{{a}}}|{{& a}}", "{\"a\":\"<b>&\\\"'\"}", "&lt;b&gt;&amp;&quot;&#39;|<b>&\"'|<b>&\"'");
    expect("{{missing}}-{{n}}-{{f}}-{{t}}-{{z}}", "{\"n\":3,\"f\":1.5,\"t\":true,\"z\":null}", "-3-1.5-true-");
    expect("{{a.b.c}} {{a.list[1]}} {{a.list.0}}", "{\"a\":{\"b\":{\"c\":\"deep\"},\"list\":[10,20]}}", "deep 20 10");
    expect("{{obj}}", "{\"obj\":{\"x\":1}}", "{&quot;x&quot;:1}");

    /* sections over arrays */
    expect("{{#items}}[{{.}}]{{/items}}", "{\"items\":[1,2,3]}", "[1][2][3]");
    expect("{{#items}}{{@index}}:{{name}}{{#if @last}}.{{else}},{{/if}}{{/items}}",
           "{\"items\":[{\"name\":\"a\"},{\"name\":\"b\"}]}", "0:a,1:b.");
    expect("{{#items}}x{{/items}}", "{\"items\":[]}", "");
    expect("{{#items}}x{{else}}empty{{/items}}", "{\"items\":[]}", "empty");
    expect("{{#items}}{{.}}{{else}}empty{{/items}}", "{\"items\":[1,2]}", "12");
    expect("{{^items}}none{{/items}}{{#items}}some{{/items}}", "{\"items\":[]}", "none");
    expect("{{^items}}none{{/items}}", "{\"items\":[1]}", "");

    /* nested loops and outer index */
    expect("{{#cols}}{{title}}:{{#cards}}({{@../index}},{{@index}}){{text}}{{/cards}};{{/cols}}",
           "{\"cols\":[{\"title\":\"A\",\"cards\":[{\"text\":\"a1\"},{\"text\":\"a2\"}]},{\"title\":\"B\",\"cards\":[]}]}",
           "A:(0,0)a1(0,1)a2;B:;");
    expect("{{#each cols}}{{#each cards}}{{@number}}/{{@length}} {{/each}}{{/each}}",
           "{\"cols\":[{\"cards\":[1,2]}]}", "1/2 2/2 ");

    /* context stack fall-through */
    expect("{{#inner}}{{outer}}/{{x}}{{/inner}}", "{\"outer\":\"O\",\"inner\":{\"x\":\"X\"}}", "O/X");
    expect("{{#items}}{{title}}-{{.}} {{/items}}", "{\"title\":\"T\",\"items\":[\"p\",\"q\"]}", "T-p T-q ");

    /* object section */
    expect("{{#person}}{{name}}{{/person}}", "{\"person\":{\"name\":\"Ann\"}}", "Ann");
    expect("{{#flag}}yes{{/flag}}{{#off}}no{{/off}}", "{\"flag\":true,\"off\":false}", "yes");

    /* if / unless / else */
    expect("{{#if a}}A{{else}}notA{{/if}}{{#unless a}}U{{/unless}}", "{\"a\":0}", "notAU");
    expect("{{#if a}}A{{else}}notA{{/if}}{{#unless a}}U{{/unless}}", "{\"a\":\"x\"}", "A");
    expect("{{#if a}}{{#if b}}ab{{else}}a{{/if}}{{else}}{{#if b}}b{{else}}none{{/if}}{{/if}}",
           "{\"a\":false,\"b\":true}", "b");
    expect("{{#if a}}{{#if b}}ab{{else}}a{{/if}}{{else}}{{#if b}}b{{else}}none{{/if}}{{/if}}",
           "{\"a\":true,\"b\":false}", "a");
    /* if does not change context */
    expect("{{#if person}}{{person.name}}{{/if}}", "{\"person\":{\"name\":\"Ann\"}}", "Ann");

    /* suppressed sections don't evaluate or emit, but still nest properly */
    expect("{{#if no}}{{#items}}{{.}}{{/items}}{{/if}}done", "{\"items\":[1,2,3]}", "done");
    expect("{{#items}}{{#if @first}}first{{/if}}{{/items}}", "{\"items\":[1,2]}", "first");

    /* comments and whitespace inside tags */
    expect("a{{! comment }}b{{  name  }}c", "{\"name\":\"N\"}", "abNc");

    /* errors */
    expect_error("{{#a}}unclosed");
    expect_error("{{/a}}");
    expect_error("{{#a}}{{/b}}");
    expect_error("{{unclosed");
    expect_error("{{else}}");
    expect_error("{{> partial}}");
    expect_error("{{#if a}}{{/a}}");

    /* exponential templates hit the render budget instead of hanging */
    {
        char big[4096] = "";
        for (int i = 0; i < 60; i++) strcat(big, "{{#a}}");
        strcat(big, "x");
        for (int i = 0; i < 60; i++) strcat(big, "{{/a}}");
        Json *ctx = parse("{\"a\":[1,2]}");
        char e[256];
        char *out = template_render(big, ctx, e, sizeof e);
        CHECK(out == NULL && strstr(e, "budget") != NULL);
        free(out);
        json_free(ctx);
    }

    /* validate */
    char err[128];
    CHECK(template_validate("{{#x}}{{y}}{{/x}}", err, sizeof err) == 0);
    CHECK(template_validate("{{#x}}{{y}}", err, sizeof err) != 0);

    /* multi-context render */
    Json *meta = parse("{\"id\":7,\"title\":\"W\"}");
    Json *state = parse("{\"title\":\"S\",\"n\":1}");
    const Json *ctxs[2] = { meta, state };
    char *out = template_render_stack("{{id}}:{{title}}:{{n}}", ctxs, 2, err, sizeof err);
    CHECK_STR(out, "7:S:1");
    free(out);
    json_free(meta);
    json_free(state);

    TEST_MAIN_END();
}
