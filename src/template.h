#ifndef TEMPLATE_H
#define TEMPLATE_H

#include "json.h"

/*
 * Mustache-style template engine.
 *
 *   {{name}}              HTML-escaped value (dotted paths: a.b[0].c)
 *   {{{name}}} {{& name}} raw value
 *   {{#list}}...{{/list}} iterate an array (or enter an object / truthy value)
 *   {{^list}}...{{/list}} rendered when the value is falsy or empty
 *   {{#if x}}...{{else}}...{{/if}}   conditional without changing context
 *   {{#unless x}}...{{/unless}}
 *   {{else}}              also valid inside {{#list}} sections (empty list)
 *   {{.}} {{this}}        current context value
 *   {{@index}} {{@first}} {{@last}}   iteration metadata; {{@../index}} for the outer loop
 *   {{! comment }}
 *
 * Name lookup walks the context stack from innermost to outermost, like
 * Mustache. Both the open-block stack and the context stack are the
 * generic Stack from stack.h.
 */

/* Renders with `nctx` context objects; ctxs[nctx-1] is the innermost scope.
 * Returns a malloc'd string or NULL on error (err is filled). */
char *template_render_stack(const char *tpl, const Json *const *ctxs, size_t nctx,
                            char *err, size_t errlen);
char *template_render(const char *tpl, const Json *ctx, char *err, size_t errlen);
/* Checks tag syntax and block balance. Returns 0 if valid. */
int template_validate(const char *tpl, char *err, size_t errlen);

#endif
