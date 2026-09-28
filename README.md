# Widgets

A small web app for arranging interactive widgets on a board, written in C
with no dependencies beyond libc and POSIX sockets. It ships with a kanban
board, a stack, a counter and a notes widget, and users can define their own
widget types through the HTTP API (or the in-page editor).

```
make            # builds ./widgets
./widgets       # http://127.0.0.1:8080/
make test       # unit tests for the stack, JSON and template modules (ASan/UBSan)
```

Options: `--port 8080 --host 127.0.0.1 --data data --web web --builtin builtin`.
The server binds to localhost by default because widget scripts run in the
browser of whoever opens the page.

## What is inside

| Module | Purpose |
| --- | --- |
| `src/stack.[ch]` | Generic pointer stack. Backs the template engine's block and context stacks and the per-widget undo history. |
| `src/json.[ch]` | JSON parser, serializer and path helpers (`columns[1].cards[0].title`). |
| `src/template.[ch]` | Mustache-style template engine. Every widget is HTML produced by it. |
| `src/http.[ch]` | Minimal HTTP/1.1 server. |
| `src/store.[ch]` | Widget types, widget instances, state ops, undo, persistence to `data/`. |
| `src/main.c` | Routing. |
| `builtin/<type>/` | Built-in widget types: `type.json`, `template.html`, `script.js`, `style.css`. |
| `web/` | Frontend. `index.html` is itself rendered by the template engine. |

Built-in types go through exactly the same mechanism as user-defined ones:
a template, a default state, an optional script and stylesheet. Anything the
kanban board does can be done by a custom type.

## Template language

```
{{name}}                  HTML-escaped value; dotted paths work: {{a.b[0].c}}
{{{name}}}  {{& name}}    raw (unescaped) value
{{#list}}...{{/list}}     repeat for each element; {{.}} is the element
{{#obj}}...{{/obj}}       enter an object (or render once if truthy)
{{^list}}...{{/list}}     render when empty / falsy
{{#list}}...{{else}}...{{/list}}   else branch for an empty list
{{#if x}}...{{else}}...{{/if}}     conditional, context unchanged
{{#unless x}}...{{/unless}}
{{#each list}}...{{/each}}  alias for {{#list}}...{{/list}}
{{@index}} {{@number}} {{@first}} {{@last}} {{@length}}   loop metadata
{{@../index}}             index of the enclosing loop
{{! comment }}
```

Name lookup walks the context stack from innermost to outermost, like
Mustache. A widget template sees its state keys directly (`{{columns}}`) and
the widget metadata behind them (`{{id}}`, `{{title}}`, `{{type}}`).

## HTTP API

Widget types:

```
GET    /api/widget-types          list types (template, script, style included)
POST   /api/widget-types          create  {name, template, default_state?, description?, script?, style?}
GET    /api/widget-types/:name
PUT    /api/widget-types/:name    replace a custom type (full definition, template required; built-ins are read-only)
DELETE /api/widget-types/:name    fails with 409 while widgets of that type exist
POST   /api/render                {template, state} -> {html}   preview without saving
```

Widgets:

```
GET    /api/widgets               list instances
POST   /api/widgets               {type, title?, state?} -> {widget, html}
GET    /api/widgets/:id           -> {widget, html}
PATCH  /api/widgets/:id           {title?, state?}
DELETE /api/widgets/:id
GET    /api/widgets/:id/render    rendered HTML only
POST   /api/widgets/:id/ops       {ops: [...]} -> {widget, html}
POST   /api/widgets/:id/undo      restore the state before the last change
```

State ops are applied atomically (all or nothing) on a copy of the state and
paths are relative to the widget's state:

| op | fields | effect |
| --- | --- | --- |
| `set` | `path`, `value` | assign (empty path replaces the whole state) |
| `push` | `path`, `value` | append to an array |
| `pop` | `path` | remove the last element |
| `insert` | `path` (ending in an index), `value` | insert into an array |
| `remove` | `path` | delete a key or element |
| `move` | `from`, `to` | detach then insert; `to` indexes the array after the detach |
| `increment` | `path`, `value?` | add to a number (default 1) |

### Example: define and use a custom widget with curl

```sh
# 1. register a type
curl -s -X POST localhost:8080/api/widget-types -H 'Content-Type: application/json' -d '{
  "name": "todo",
  "description": "Checklist",
  "template": "<ul class=\"todo\">{{#items}}<li data-idx=\"{{@index}}\"><input type=\"checkbox\" {{#if done}}checked{{/if}}> {{text}}</li>{{else}}<li>Nothing to do</li>{{/items}}</ul><form><input name=\"text\" placeholder=\"New item\"><button>Add</button></form>",
  "default_state": { "items": [] },
  "script": "el.querySelectorAll(\"input[type=checkbox]\").forEach(cb => cb.addEventListener(\"change\", () => api.ops([{op:\"set\", path:`items[${cb.closest(\"li\").dataset.idx}].done`, value: cb.checked}]))); el.querySelector(\"form\").addEventListener(\"submit\", e => { e.preventDefault(); const text = e.target.text.value.trim(); if (text) api.ops([{op:\"push\", path:\"items\", value:{text, done:false}}]); });",
  "style": ".todo { list-style: none; padding: 0 }"
}'

# 2. create a widget of that type
curl -s -X POST localhost:8080/api/widgets -d '{"type":"todo","title":"Groceries"}'

# 3. change its state (id from the previous response)
curl -s -X POST localhost:8080/api/widgets/1/ops \
  -d '{"ops":[{"op":"push","path":"items","value":{"text":"Milk","done":false}}]}'

# 4. get the rendered HTML
curl -s localhost:8080/api/widgets/1/render
```

## Widget scripts

A type's `script` runs in the browser after every render as
`function (el, widget, api)`:

- `el` is the widget's body element containing the rendered template.
- `widget` is `{id, type, title, state}`.
- `api.ops(ops, {rerender})` posts state ops; the widget re-renders with the
  server's HTML unless `rerender: false`.
- `api.replace(state)` replaces the whole state, `api.refresh()` re-renders,
  `api.notify(message)` shows a toast.

Scripts are plain JavaScript supplied by whoever defines the type, so treat
the app as a personal, local tool.

## Limits

Rendering stops with an error after two million tag evaluations or eight
megabytes of output, so a template with deeply nested loops fails instead of
hanging the server. Requests are capped at 64 KB of headers and 8 MB of body.
The server handles one connection at a time; a client that opens a connection
and sends nothing holds it for up to ten seconds.

## Persistence

Custom types are stored in `data/types.json` and widgets in
`data/widgets.json`, written atomically after every change. Undo history is
kept in memory (50 snapshots per widget) and cleared on restart.
