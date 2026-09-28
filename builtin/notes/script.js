const ta = el.querySelector('.notes-text');
const status = el.querySelector('.notes-status');
let timer = null;
ta.addEventListener('input', () => {
  status.textContent = 'typing…';
  clearTimeout(timer);
  timer = setTimeout(async () => {
    // rerender:false keeps the textarea (and the cursor) in place.
    await api.ops([{ op: 'set', path: 'text', value: ta.value }], { rerender: false });
    status.textContent = 'saved';
  }, 400);
});
