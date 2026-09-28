/* tmpl.h - a small Mustache/Handlebars-flavoured template engine.
 *
 *   {{ expr }}                 HTML-escaped output
 *   {{{ expr }}}               raw output
 *   {{#each expr}} .. {{else}} .. {{/each}}
 *   {{#if expr}} .. {{else}} .. {{/if}}
 *   {{#unless expr}} .. {{/unless}}
 *   {{> partial}}              include another template (by name)
 *   {{! comment }}
 *
 * Expressions:
 *   name.sub.0     looked up in the current scope, then outer scopes
 *   . / this       the current scope value
 *   ../name        skip one scope outwards (repeatable)
 *   @index @key @first @last @count   loop metadata of the innermost #each
 *   @path          dotted JSON path of the current scope value, relative to
 *                  the root data ("columns.1.cards.0"); lets templates tell
 *                  the client exactly which piece of data an element maps to
 *   @root.x        lookup from the root data
 *   @name.x        lookup in the "vars" object passed to tmpl_render
 */
#ifndef TMPL_H
#define TMPL_H

#include "buf.h"
#include "json.h"

typedef struct Tmpl Tmpl;

typedef const Tmpl *(*TmplPartialFn)(const char *name, void *ud);

Tmpl *tmpl_compile(const char *src, char *err, size_t errlen);
void  tmpl_free(Tmpl *t);

/* Returns 0 on success, -1 on error (message in err). */
int tmpl_render(const Tmpl *t, Json *data, Json *vars,
                TmplPartialFn partials, void *ud,
                Buf *out, char *err, size_t errlen);

#endif
