// Runs after every render with (el, widget, api). Every interaction is expressed
// as generic state ops, so this same mechanism is available to custom widgets.
const col = (n) => `columns[${n}]`;

// Drag and drop between (and within) columns.
let dragging = null;
el.querySelectorAll('.kanban-card').forEach((card) => {
  card.addEventListener('dragstart', (e) => {
    dragging = { col: +card.dataset.col, idx: +card.dataset.idx };
    card.classList.add('dragging');
    e.dataTransfer.effectAllowed = 'move';
    e.dataTransfer.setData('text/plain', JSON.stringify(dragging));
  });
  card.addEventListener('dragend', () => {
    card.classList.remove('dragging');
    el.querySelectorAll('.kanban-cards.over').forEach((z) => z.classList.remove('over'));
  });
});

el.querySelectorAll('.kanban-cards').forEach((zone) => {
  zone.addEventListener('dragover', (e) => { e.preventDefault(); zone.classList.add('over'); });
  zone.addEventListener('dragleave', () => zone.classList.remove('over'));
  zone.addEventListener('drop', (e) => {
    e.preventDefault();
    zone.classList.remove('over');
    let src = dragging;
    try { src = JSON.parse(e.dataTransfer.getData('text/plain')) || src; } catch (_) {}
    if (!src) return;
    const dst = +zone.dataset.col;
    // Cards excluding the one being dragged: the index we compute is valid
    // after the server detaches the source card.
    const cards = [...zone.querySelectorAll('.kanban-card:not(.dragging)')];
    let to = cards.length;
    for (let i = 0; i < cards.length; i++) {
      const r = cards[i].getBoundingClientRect();
      if (e.clientY < r.top + r.height / 2) { to = i; break; }
    }
    if (src.col === dst && to === src.idx) return;
    api.ops([{ op: 'move', from: `${col(src.col)}.cards[${src.idx}]`, to: `${col(dst)}.cards[${to}]` }]);
  });
});

// Add card / add column forms.
el.querySelectorAll('form.kanban-add').forEach((form) => {
  form.addEventListener('submit', (e) => {
    e.preventDefault();
    const text = form.text.value.trim();
    if (!text) return;
    api.ops([{ op: 'push', path: `${col(form.dataset.col)}.cards`, value: { text } }]);
  });
});
el.querySelector('form.kanban-add-col').addEventListener('submit', (e) => {
  e.preventDefault();
  const title = e.target.title.value.trim();
  if (!title) return;
  api.ops([{ op: 'push', path: 'columns', value: { title, cards: [] } }]);
});

// Delete buttons.
el.querySelectorAll('[data-action="del-card"]').forEach((b) =>
  b.addEventListener('click', () =>
    api.ops([{ op: 'remove', path: `${col(b.dataset.col)}.cards[${b.dataset.idx}]` }])));
el.querySelectorAll('[data-action="del-col"]').forEach((b) =>
  b.addEventListener('click', () => api.ops([{ op: 'remove', path: col(b.dataset.col) }])));

// Rename a column inline.
el.querySelectorAll('.kanban-col-title').forEach((t) => {
  const commit = () => {
    const title = t.textContent.trim();
    if (title && title !== widget.state.columns[+t.dataset.col].title)
      api.ops([{ op: 'set', path: `${col(t.dataset.col)}.title`, value: title }]);
  };
  t.addEventListener('blur', commit);
  t.addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); t.blur(); } });
});
