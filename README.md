# Widget Board

A widget dashboard (kanban, checklist, counter, note, plus any widgets you define) whose whole backend is C:

| Part | File | What it does |
|---|---|---|
| Stack | `src/stack.[ch]` | Generic LIFO stack. The template compiler uses it to match open and close tags, and the renderer uses it for scopes. |
| Template engine | `src/tmpl.[ch]` | A Mustache/Handlebars-style engine. It compiles a template into a node tree and renders it against JSON. |
| JSON | `src/json.[ch]` | Parser, serializer, and dotted-path access (`columns.0.cards`). |
| HTTP server | `src/http.[ch]` | Single-threaded HTTP/1.1 server. It closes the connection after each request. |
| Store | `src/store.[ch]` | Widget types and instances, data ops, and atomic saves to `board.json`. |
| Routes | `src/main.c` | Pages and the JSON API. |

There's no JavaScript per widget. Widgets declare their interactions with `data-*` attributes. The client (`static/app.js`) turns those into data ops. The server applies each op, re-renders the widget in C, and sends back the new HTML.

## Run

```sh
make            # builds ./board (needs only cc + libc)
make run        # http://127.0.0.1:8080
./board 9000 0.0.0.0     # custom port / bind address
BOARD_DB=/path/db.json ./board
make test       # template engine + stack unit tests
```

Run it from the project root, because it reads `templates/`, `widgets/` and `static/`. On the first start it seeds a demo board. To reset, delete `board.json`.

## Making a custom widget

You can use the **Studio** page (`/studio`), which has a live preview, or call the API directly:

```sh
curl -X POST localhost:8080/api/types -H 'Content-Type: application/json' -d '{
  "name": "poll",
  "label": "Poll",
  "template": "<p>{{question}}</p>{{#each options}}<button data-op=\"inc\" data-path=\"{{@path}}.votes\">{{label}}: {{votes}}</button>{{/each}}<form data-op=\"push\" data-path=\"options\"><input name=\"label\" placeholder=\"Add option\"></form>",
  "css": ".w-poll button { display:block; width:100%; margin:4px 0 }",
  "defaults": { "question": "Lunch?", "options": [ {"label":"Tacos","votes":0} ] }
}'

curl -X POST localhost:8080/api/widgets -d '{"type":"poll","title":"Team lunch"}'
```

If a template is invalid, the API rejects it and the error includes the line number, for example `line 3: '{{/if}}' closes '{{#each}}' opened on line 1`.

The built-in types in `widgets/*.html|.json|.css` are loaded through the same code path as user types.

### Template syntax

```
{{x}}  {{{raw}}}  {{a.b.0}}  {{list.length}}  {{! comment }}
{{#each xs}} … {{else}} … {{/each}}     {{@index}} {{@key}} {{@first}} {{@last}} {{@count}}
{{#if x}} … {{else}} … {{/if}}          {{#unless x}} … {{/unless}}
{{../x}}  {{@root.x}}  {{@widget.title}}  {{> other-widget-type}}
{{@path}}   the JSON path of the current item, e.g. "columns.1.cards.0"
```

Name lookups walk outwards through the enclosing scopes, as in Mustache. `{{x}}` is HTML-escaped.

### Interaction attributes → ops

| Markup | Op sent |
|---|---|
| `<button data-op="inc" data-path="n" data-by="-1">` | `{"op":"inc","path":"n","by":-1}` |
| `<button data-op="set" data-path="x" data-value='"hi"'>` | `set` (`data-value` is parsed as JSON) |
| `<input type=checkbox data-op="toggle" data-path="…done">` | `toggle` on change |
| `<textarea data-op="set" data-path="text">` | `set` on change, with the field's value |
| `<form data-op="push" data-path="items"><input name="text">` | `push` with `{"text": …}` |
| `<button data-op="remove" data-path="{{@path}}">` | `remove` |
| `draggable data-drag="{{@path}}"` inside `data-drop="list.path"` | `move` with `to` + `index` |

## API

| Method | Path | Body / notes |
|---|---|---|
| GET | `/api/types` | All types (built-in + custom) |
| POST | `/api/types` | `{name, label?, template, css?, defaults?, description?}`. Creates or replaces a type. |
| GET / DELETE | `/api/types/:name` | Built-in types can't be deleted. A type can't be deleted while widgets use it (409). |
| POST | `/api/preview` | `{template, data?}` → `{html}` or `{error}` |
| GET / POST | `/api/widgets` | List, or create with `{type, title?, data?}` |
| GET / PATCH / DELETE | `/api/widgets/:id` | PATCH `{title?, size?: normal\|wide\|full, data?}` |
| POST | `/api/widgets/:id/op` | `{op: set\|toggle\|inc\|push\|remove\|move, path, value?, by?, to?, index?}` |

Widget responses look like `{widget, html}`, where `html` is the rendered card.

## Security notes

- The server binds to `127.0.0.1` by default. **Custom templates and CSS are served as raw HTML**, so anyone who can call `POST /api/types` can inject script. Don't expose the server publicly without adding authentication.
- Values interpolated with `{{x}}` are always escaped. Request bodies are capped at 1 MB, JSON nesting at 64 levels, and rendered output at 4 MB. `/static` rejects `..`.
- There is one request at a time with a 5 s socket timeout, so a slow client stalls the server for up to 5 s.
