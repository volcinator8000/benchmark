// Widget Board client.
//
// There is no per-widget JavaScript. Widgets describe interactions with data
// attributes, and this file turns them into ops against the widget's JSON:
//
//   <button data-op="inc" data-path="value" data-by="1">       click  -> op
//   <input|textarea|select data-op="set|toggle" data-path="x"> change -> op
//   <form data-op="push" data-path="items">                   submit -> op (form fields = value)
//   <el draggable data-drag="items.2"> inside <el data-drop="items">  drag -> move op
//
// The server applies the op, re-renders the widget with the C template engine,
// and returns the new HTML, which replaces the card in place.
(() => {
  "use strict";

  // ---------- helpers ----------

  async function api(method, url, body) {
    const res = await fetch(url, {
      method,
      headers: body !== undefined ? { "Content-Type": "application/json" } : {},
      body: body !== undefined ? JSON.stringify(body) : undefined,
    });
    const text = await res.text();
    let data = null;
    try { data = text ? JSON.parse(text) : null; } catch { data = { error: text }; }
    if (!res.ok) throw new Error((data && data.error) || `${res.status} ${res.statusText}`);
    return data;
  }

  let toastTimer;
  function toast(msg, isError = false) {
    const el = document.getElementById("toast");
    if (!el) return;
    el.textContent = msg;
    el.classList.toggle("error", isError);
    el.classList.add("show");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => el.classList.remove("show"), isError ? 4000 : 2000);
  }

  function parseValue(raw) {
    if (raw === undefined) return undefined;
    try { return JSON.parse(raw); } catch { return raw; }
  }

  const cardOf = (el) => el.closest("[data-widget]");

  function htmlToElement(html) {
    const t = document.createElement("template");
    t.innerHTML = html.trim();
    return t.content.firstElementChild;
  }

  // Remember which control had focus so re-rendering doesn't steal it
  // (e.g. adding several kanban cards in a row).
  function focusKey(el) {
    if (!el || !el.matches("input, textarea, select")) return null;
    const holder = el.closest("[data-op][data-path]");
    return holder ? `${holder.dataset.op}|${holder.dataset.path}|${el.name || ""}` : null;
  }

  function replaceCard(html) {
    const next = htmlToElement(html);
    if (!next) return;
    const prev = document.getElementById(next.id);
    if (!prev) return next;
    const key = prev.contains(document.activeElement) ? focusKey(document.activeElement) : null;
    prev.replaceWith(next);
    if (key) {
      for (const el of next.querySelectorAll("input, textarea, select")) {
        if (focusKey(el) === key) { el.focus(); break; }
      }
    }
    return next;
  }

  async function sendOp(card, op) {
    card.classList.add("is-busy");
    try {
      const res = await api("POST", `/api/widgets/${card.dataset.widget}/op`, op);
      replaceCard(res.html);
    } catch (err) {
      toast(err.message, true);
      card.classList.remove("is-busy");
    }
  }

  function opFrom(el, value) {
    const op = { op: el.dataset.op, path: el.dataset.path || "" };
    if (value !== undefined) op.value = value;
    if (el.dataset.by !== undefined) op.by = Number(el.dataset.by);
    if (el.dataset.to !== undefined) op.to = el.dataset.to;
    if (el.dataset.index !== undefined) op.index = Number(el.dataset.index);
    return op;
  }

  // ---------- widget ops ----------

  document.addEventListener("click", (e) => {
    const cmd = e.target.closest("[data-widget-cmd]");
    if (cmd && cmd.tagName === "BUTTON") return widgetCommand(cmd);

    const el = e.target.closest("[data-op]");
    if (!el || el.matches("input, textarea, select, form")) return;
    const card = cardOf(el);
    if (!card) return;
    e.preventDefault();
    sendOp(card, opFrom(el, parseValue(el.dataset.value)));
  });

  document.addEventListener("change", (e) => {
    const el = e.target;
    if (el.matches("[data-widget-cmd=title]")) return renameWidget(el);
    if (!el.matches("input[data-op], textarea[data-op], select[data-op]")) return;
    const card = cardOf(el);
    if (!card) return;
    let value;
    if (el.type === "checkbox") value = el.checked;
    else if (el.type === "number" || el.type === "range") value = el.valueAsNumber;
    else value = el.value;
    sendOp(card, opFrom(el, el.dataset.value !== undefined ? parseValue(el.dataset.value) : value));
  });

  document.addEventListener("submit", (e) => {
    const form = e.target;
    if (form.id === "add-widget") { e.preventDefault(); return addWidget(form); }
    if (!form.matches("form[data-op]")) return;
    e.preventDefault();
    const card = cardOf(form);
    if (!card) return;
    const fields = {};
    for (const [k, v] of new FormData(form)) fields[k] = v;
    const keys = Object.keys(fields);
    const value = form.dataset.value !== undefined
      ? parseValue(form.dataset.value)
      : keys.length === 1 && keys[0] === "value" ? fields.value : fields;
    sendOp(card, opFrom(form, value));
  });

  document.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && e.target.matches("[data-widget-cmd=title]")) e.target.blur();
  });

  // ---------- drag & drop ----------

  let drag = null; // { card, path, el }
  let marker = null;

  function clearDropUi() {
    marker?.remove();
    marker = null;
    document.querySelectorAll(".drop-target").forEach((z) => z.classList.remove("drop-target"));
  }

  function dropIndex(zone, y) {
    const items = [...zone.querySelectorAll(":scope > [data-drag]")];
    for (let i = 0; i < items.length; i++) {
      const r = items[i].getBoundingClientRect();
      if (y < r.top + r.height / 2) return { index: i, before: items[i] };
    }
    return { index: items.length, before: null };
  }

  document.addEventListener("dragstart", (e) => {
    const el = e.target.closest?.("[data-drag]");
    if (!el) return;
    const card = cardOf(el);
    if (!card) return;
    drag = { card, path: el.dataset.drag, el };
    el.classList.add("is-dragging");
    e.dataTransfer.effectAllowed = "move";
    e.dataTransfer.setData("text/plain", el.dataset.drag);
  });

  document.addEventListener("dragover", (e) => {
    if (!drag) return;
    const zone = e.target.closest("[data-drop]");
    if (!zone || cardOf(zone) !== drag.card) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = "move";
    if (!zone.classList.contains("drop-target")) {
      clearDropUi();
      zone.classList.add("drop-target");
    }
    const { before } = dropIndex(zone, e.clientY);
    marker ??= Object.assign(document.createElement("div"), { className: "drop-marker" });
    if (before) zone.insertBefore(marker, before);
    else zone.appendChild(marker);
  });

  document.addEventListener("drop", (e) => {
    if (!drag) return;
    const zone = e.target.closest("[data-drop]");
    if (!zone || cardOf(zone) !== drag.card) return;
    e.preventDefault();
    marker?.remove(); marker = null; // must not count as an item
    const { index } = dropIndex(zone, e.clientY);
    const { card, path } = drag;
    clearDropUi();
    sendOp(card, { op: "move", path, to: zone.dataset.drop, index });
  });

  document.addEventListener("dragend", () => {
    drag?.el.classList.remove("is-dragging");
    drag = null;
    clearDropUi();
  });

  // ---------- widget management ----------

  async function refreshTypeCss() {
    const link = document.getElementById("types-css");
    if (link) link.href = `/types.css?t=${Date.now()}`;
  }

  async function addWidget(form) {
    const fd = new FormData(form);
    try {
      const res = await api("POST", "/api/widgets", { type: fd.get("type"), title: fd.get("title") || undefined });
      const grid = document.getElementById("grid");
      grid.querySelector(".empty-board")?.remove();
      const el = htmlToElement(res.html);
      grid.appendChild(el);
      el.scrollIntoView({ behavior: "smooth", block: "nearest" });
      form.reset();
    } catch (err) {
      toast(err.message, true);
    }
  }

  async function widgetCommand(btn) {
    const card = cardOf(btn);
    const id = card.dataset.widget;
    try {
      if (btn.dataset.widgetCmd === "delete") {
        await api("DELETE", `/api/widgets/${id}`);
        card.remove();
        toast("Widget removed");
      } else if (btn.dataset.widgetCmd === "size") {
        const order = ["normal", "wide", "full"];
        const cur = order.find((s) => card.classList.contains(`size-${s}`)) || "normal";
        const next = order[(order.indexOf(cur) + 1) % order.length];
        const res = await api("PATCH", `/api/widgets/${id}`, { size: next });
        replaceCard(res.html);
      }
    } catch (err) {
      toast(err.message, true);
    }
  }

  async function renameWidget(input) {
    const card = cardOf(input);
    try {
      await api("PATCH", `/api/widgets/${card.dataset.widget}`, { title: input.value });
      toast("Renamed");
    } catch (err) {
      toast(err.message, true);
    }
  }

  // ---------- studio ----------

  const studio = document.getElementById("studio-form");
  if (studio) setupStudio(studio);

  function setupStudio(form) {
    const preview = document.getElementById("studio-preview");
    const cssTag = document.getElementById("studio-css");
    const status = document.getElementById("studio-status");
    const f = (name) => form.elements[name];

    const EXAMPLE = {
      name: "poll",
      label: "Poll",
      template: `<p class="poll-q">{{question}}</p>
{{#each options}}
<button type="button" class="poll-opt" data-op="inc" data-path="{{@path}}.votes">
  <span>{{label}}</span>
  <b>{{#if votes}}{{votes}}{{else}}0{{/if}}</b>
</button>
{{/each}}
<form data-op="push" data-path="options">
  <input name="label" placeholder="+ Add option" required>
</form>`,
      defaults: { question: "Where should we eat?", options: [{ label: "Tacos", votes: 2 }, { label: "Ramen", votes: 3 }] },
      css: `.w-poll .poll-q { font-weight: 600; margin: 0 0 10px; }
.w-poll .poll-opt { display: flex; justify-content: space-between; width: 100%; margin-bottom: 6px;
  padding: 8px 12px; border: 1px solid var(--line); border-radius: 8px;
  background: var(--surface); color: var(--text); font: inherit; cursor: pointer; }
.w-poll .poll-opt:hover { border-color: var(--accent); }
.w-poll input { width: 100%; }`,
    };

    function fill(def) {
      f("name").value = def.name || "";
      f("label").value = def.label || "";
      f("template").value = def.template || "";
      f("css").value = def.css || "";
      f("defaults").value = JSON.stringify(def.defaults ?? {}, null, 2);
      schedulePreview();
    }

    function setStatus(msg, isError = false) {
      status.textContent = msg;
      status.style.color = isError ? "var(--danger)" : "";
    }

    function readDefaults() {
      const raw = f("defaults").value.trim() || "{}";
      const v = JSON.parse(raw);
      if (typeof v !== "object" || v === null || Array.isArray(v)) throw new Error("Default data must be a JSON object");
      return v;
    }

    let timer, seq = 0;
    function schedulePreview() {
      clearTimeout(timer);
      timer = setTimeout(runPreview, 200);
    }

    async function runPreview() {
      const mine = ++seq;
      preview.className = `card-body w-${f("name").value || "preview"}`;
      cssTag.textContent = f("css").value;
      let data;
      try { data = readDefaults(); } catch (err) { setStatus(err.message, true); return; }
      try {
        const res = await api("POST", "/api/preview", { template: f("template").value, data });
        if (mine !== seq) return;
        preview.innerHTML = res.html;
        setStatus("");
      } catch (err) {
        if (mine !== seq) return;
        preview.innerHTML = `<div class="tmpl-error"></div>`;
        preview.firstChild.textContent = err.message;
      }
    }

    form.addEventListener("input", schedulePreview);

    form.addEventListener("submit", async (e) => {
      e.preventDefault();
      let defaults;
      try { defaults = readDefaults(); } catch (err) { setStatus(err.message, true); return; }
      try {
        await api("POST", "/api/types", {
          name: f("name").value.trim(),
          label: f("label").value.trim(),
          template: f("template").value,
          css: f("css").value,
          defaults,
        });
        setStatus("Saved ✓ — add it from the Board");
        refreshTypeCss();
        setTimeout(() => location.reload(), 700);
      } catch (err) {
        setStatus(err.message, true);
      }
    });

    document.getElementById("studio-example").addEventListener("click", () => fill(EXAMPLE));

    document.getElementById("type-list").addEventListener("click", async (e) => {
      const load = e.target.closest("[data-studio-load]");
      const del = e.target.closest("[data-studio-delete]");
      try {
        if (load) {
          const def = await api("GET", `/api/types/${encodeURIComponent(load.dataset.studioLoad)}`);
          if (def.builtin) { def.name += "-copy"; def.label += " (copy)"; }
          fill(def);
          window.scrollTo({ top: 0, behavior: "smooth" });
        } else if (del) {
          await api("DELETE", `/api/types/${encodeURIComponent(del.dataset.studioDelete)}`);
          del.closest("li").remove();
          toast("Type deleted");
        }
      } catch (err) {
        toast(err.message, true);
      }
    });

    fill(EXAMPLE);
  }
})();
