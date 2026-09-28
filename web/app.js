'use strict';

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
const board = $('#board');

let types = {};
let widgets = new Map();

/* ---------- API ---------- */

async function api(method, url, body) {
  const res = await fetch(url, {
    method,
    headers: body !== undefined ? { 'Content-Type': 'application/json' } : {},
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  const ct = res.headers.get('content-type') || '';
  const data = ct.includes('json') ? await res.json() : await res.text();
  if (!res.ok) throw new Error((data && data.error) || res.statusText);
  return data;
}

let toastTimer;
function toast(msg, isError = true) {
  const t = $('#toast');
  t.textContent = msg;
  t.classList.toggle('error', isError);
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.hidden = true; }, 3500);
}

/* ---------- types ---------- */

async function loadTypes() {
  const data = await api('GET', '/api/widget-types');
  types = {};
  for (const t of data.types) types[t.name] = t;
  injectStyles();
  buildAddMenu();
}

function injectStyles() {
  for (const t of Object.values(types)) {
    let s = document.head.querySelector(`style[data-type="${t.name}"]`);
    if (!s) {
      s = document.createElement('style');
      s.dataset.type = t.name;
      document.head.appendChild(s);
    }
    if (s.textContent !== (t.style || '')) s.textContent = t.style || '';
  }
}

function buildAddMenu() {
  const menu = $('#add-widget-menu');
  menu.innerHTML = '';
  for (const t of Object.values(types)) {
    const b = document.createElement('button');
    b.className = 'menu-item';
    b.dataset.type = t.name;
    b.innerHTML = `<b></b>${t.builtin ? '<span class="tag">built-in</span>' : ''}<small></small>`;
    $('b', b).textContent = t.name;
    $('small', b).textContent = t.description || '';
    menu.appendChild(b);
  }
}

/* ---------- widgets ---------- */

async function loadWidgets() {
  const data = await api('GET', '/api/widgets');
  widgets = new Map();
  board.innerHTML = '';
  for (const w of data.widgets) {
    widgets.set(w.id, w);
    board.appendChild(makeShell(w));
  }
  updateEmpty();
  await Promise.all(data.widgets.map((w) => refreshWidget(w.id)));
}

function updateEmpty() {
  let empty = $('#empty-board');
  if (widgets.size === 0) {
    if (!empty) {
      empty = document.createElement('div');
      empty.id = 'empty-board';
      empty.className = 'empty-board';
      empty.innerHTML = '<h2>No widgets yet</h2><p>Add one from the menu above, or define your own widget type.</p>';
      board.appendChild(empty);
    }
  } else if (empty) {
    empty.remove();
  }
  $('.brand .muted').textContent = `${widgets.size} widgets · ${Object.keys(types).length} types`;
}

function makeShell(w) {
  const el = $('#widget-shell').content.firstElementChild.cloneNode(true);
  el.dataset.id = w.id;
  $('.widget-title', el).textContent = w.title;
  $('.widget-type', el).textContent = w.type;
  $('.widget-title', el).addEventListener('click', () => renameWidget(w.id, el));
  $('[data-act=refresh]', el).addEventListener('click', () => refreshWidget(w.id));
  $('[data-act=undo]', el).addEventListener('click', async () => {
    try { mount(await api('POST', `/api/widgets/${w.id}/undo`)); }
    catch (e) { toast(e.message); }
  });
  const del = $('[data-act=delete]', el);
  del.addEventListener('click', async () => {
    if (!del.classList.contains('confirm')) {
      del.classList.add('confirm');
      del.textContent = 'Delete?';
      setTimeout(() => { del.classList.remove('confirm'); del.innerHTML = '&#10005;'; }, 2500);
      return;
    }
    try {
      await api('DELETE', `/api/widgets/${w.id}`);
      widgets.delete(w.id);
      el.remove();
      updateEmpty();
    } catch (e) { toast(e.message); }
  });
  return el;
}

async function renameWidget(id, el) {
  const span = $('.widget-title', el);
  const input = document.createElement('input');
  input.className = 'title-input';
  input.value = span.textContent;
  span.replaceWith(input);
  input.focus();
  input.select();
  let done = false;
  const finish = async (save) => {
    if (done) return;
    done = true;
    const title = input.value.trim();
    input.replaceWith(span);
    if (save && title && title !== span.textContent) {
      try {
        const p = await api('PATCH', `/api/widgets/${id}`, { title });
        widgets.set(id, p.widget);
        span.textContent = p.widget.title;
      } catch (e) { toast(e.message); }
    }
  };
  input.addEventListener('blur', () => finish(true));
  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') finish(true);
    if (e.key === 'Escape') finish(false);
  });
}

async function refreshWidget(id) {
  try { mount(await api('GET', `/api/widgets/${id}`)); }
  catch (e) { toast(e.message); }
}

function mount(payload) {
  const w = payload.widget;
  widgets.set(w.id, w);
  const shell = board.querySelector(`.widget[data-id="${w.id}"]`);
  if (!shell) return;
  $('.widget-title', shell).textContent = w.title;
  const body = $('.widget-body', shell);
  if (payload.render_error) {
    body.innerHTML = '';
    const pre = document.createElement('pre');
    pre.className = 'render-error';
    pre.textContent = payload.render_error;
    body.appendChild(pre);
    return;
  }
  body.innerHTML = payload.html;
  runScript(w, body);
}

function runScript(w, body) {
  const t = types[w.type];
  if (!t || !t.script || !t.script.trim()) return;
  const wapi = {
    id: w.id,
    state: w.state,
    // Apply state ops on the server. The widget is re-rendered unless {rerender:false}.
    async ops(ops, opts = {}) {
      try {
        const p = await api('POST', `/api/widgets/${w.id}/ops`, { ops });
        if (opts.rerender === false) {
          widgets.set(w.id, p.widget);
          w.state = p.widget.state;
          wapi.state = p.widget.state;
        } else {
          mount(p);
        }
        return p.widget.state;
      } catch (e) { toast(e.message); throw e; }
    },
    async setState(state, opts = {}) {
      return wapi.replace(state, opts);
    },
    async replace(state, opts = {}) {
      try {
        const p = await api('PATCH', `/api/widgets/${w.id}`, { state });
        if (opts.rerender === false) { widgets.set(w.id, p.widget); w.state = p.widget.state; wapi.state = p.widget.state; }
        else mount(p);
        return p.widget.state;
      } catch (e) { toast(e.message); throw e; }
    },
    refresh: () => refreshWidget(w.id),
    notify: (msg) => toast(msg, false),
  };
  try {
    new Function('el', 'widget', 'api', t.script)(body, w, wapi);
  } catch (e) {
    console.error(`widget ${w.id} (${w.type}) script error:`, e);
    const pre = document.createElement('pre');
    pre.className = 'render-error';
    pre.textContent = `script error: ${e.message}`;
    body.prepend(pre);
  }
}

async function addWidget(type) {
  try {
    const p = await api('POST', '/api/widgets', { type });
    widgets.set(p.widget.id, p.widget);
    board.appendChild(makeShell(p.widget));
    updateEmpty();
    mount(p);
  } catch (e) { toast(e.message); }
}

/* ---------- add-widget menu ---------- */

$('#add-widget-btn').addEventListener('click', (e) => {
  e.stopPropagation();
  $('#add-widget-menu').hidden = !$('#add-widget-menu').hidden;
});
$('#add-widget-menu').addEventListener('click', (e) => {
  const item = e.target.closest('.menu-item');
  if (!item) return;
  $('#add-widget-menu').hidden = true;
  addWidget(item.dataset.type);
});
document.addEventListener('click', () => { $('#add-widget-menu').hidden = true; });

/* ---------- types dialog ---------- */

function renderTypesList() {
  const list = $('#types-list');
  list.innerHTML = '';
  for (const t of Object.values(types)) {
    const row = document.createElement('div');
    row.className = 'type-row';
    row.innerHTML = `<div class="type-info"><b></b>${t.builtin ? '<span class="tag">built-in</span>' : ''}<small></small></div>
      <div class="type-actions">
        <button class="btn small" data-do="add">Add</button>
        <button class="btn small" data-do="clone">Clone</button>
        ${t.builtin ? '' : '<button class="btn small" data-do="edit">Edit</button><button class="btn small danger" data-do="delete">Delete</button>'}
      </div>`;
    $('b', row).textContent = t.name;
    $('small', row).textContent = t.description || '';
    row.addEventListener('click', async (e) => {
      const b = e.target.closest('[data-do]');
      if (!b) return;
      const action = b.dataset.do;
      if (action === 'add') { $('#types-dialog').close(); addWidget(t.name); }
      if (action === 'edit') { $('#types-dialog').close(); openEditor(t, 'edit'); }
      if (action === 'clone') { $('#types-dialog').close(); openEditor(t, 'clone'); }
      if (action === 'delete') {
        try {
          await api('DELETE', `/api/widget-types/${t.name}`);
          await loadTypes();
          renderTypesList();
          updateEmpty();
        } catch (err) { toast(err.message); }
      }
    });
    list.appendChild(row);
  }
}

$('#types-btn').addEventListener('click', () => { renderTypesList(); $('#types-dialog').showModal(); });
$$('[data-close]').forEach((b) => b.addEventListener('click', () => b.closest('dialog').close()));

/* ---------- type editor ---------- */

let editorMode = 'create';
const form = $('#type-form');

function openEditor(t, mode) {
  editorMode = mode;
  form.reset();
  $('#preview').hidden = true;
  $('#editor-error').textContent = '';
  const titles = { create: 'New widget type', edit: `Edit type "${t && t.name}"`, clone: `Clone "${t && t.name}" as a new type` };
  $('#editor-title').textContent = titles[mode];
  if (t) {
    form.name.value = mode === 'clone' ? `${t.name}-copy` : t.name;
    form.description.value = t.description || '';
    form.template.value = t.template || '';
    form.default_state.value = JSON.stringify(t.default_state || {}, null, 2);
    form.script.value = t.script || '';
    form.style.value = t.style || '';
  } else {
    form.template.value = '<div class="hello">\n  <p>Hello, {{who}}!</p>\n  <button class="hello-btn">Clicked {{clicks}} times</button>\n</div>';
    form.default_state.value = '{\n  "who": "world",\n  "clicks": 0\n}';
    form.script.value = "el.querySelector('.hello-btn').addEventListener('click', () =>\n  api.ops([{ op: 'increment', path: 'clicks' }]));";
    form.style.value = '.hello-btn { margin-top: 8px; }';
  }
  form.name.readOnly = mode === 'edit';
  $('#type-editor').showModal();
}

function readEditor() {
  let default_state;
  try {
    default_state = JSON.parse(form.default_state.value || '{}');
  } catch (e) {
    throw new Error(`default state is not valid JSON: ${e.message}`);
  }
  if (!default_state || typeof default_state !== 'object' || Array.isArray(default_state))
    throw new Error('default state must be a JSON object');
  return {
    name: form.name.value.trim(),
    description: form.description.value.trim(),
    template: form.template.value,
    default_state,
    script: form.script.value,
    style: form.style.value,
  };
}

$('#new-type-btn').addEventListener('click', () => openEditor(null, 'create'));

$('#preview-btn').addEventListener('click', async () => {
  $('#editor-error').textContent = '';
  try {
    const def = readEditor();
    const r = await api('POST', '/api/render', { template: def.template, state: def.default_state });
    let s = $('#preview-style');
    if (!s) { s = document.createElement('style'); s.id = 'preview-style'; document.head.appendChild(s); }
    s.textContent = def.style;
    const pv = $('#preview');
    pv.hidden = false;
    pv.innerHTML = '<div class="preview-label">Preview (rendered by the server)</div><div class="preview-body"></div>';
    $('.preview-body', pv).innerHTML = r.html;
  } catch (e) {
    $('#editor-error').textContent = e.message;
  }
});

form.addEventListener('submit', async (e) => {
  e.preventDefault();
  $('#editor-error').textContent = '';
  try {
    const def = readEditor();
    if (editorMode === 'edit') await api('PUT', `/api/widget-types/${def.name}`, def);
    else await api('POST', '/api/widget-types', def);
    await loadTypes();
    updateEmpty();
    $('#type-editor').close();
    toast(`Saved widget type "${def.name}"`, false);
    if (editorMode === 'edit') {
      for (const w of widgets.values()) if (w.type === def.name) refreshWidget(w.id);
    }
  } catch (err) {
    $('#editor-error').textContent = err.message;
  }
});

/* ---------- boot ---------- */

(async () => {
  try {
    await loadTypes();
    await loadWidgets();
  } catch (e) {
    toast(`Failed to load: ${e.message}`);
  }
})();
