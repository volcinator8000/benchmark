el.querySelector('form.stack-push').addEventListener('submit', (e) => {
  e.preventDefault();
  const value = e.target.value.value.trim();
  if (!value) return;
  if (widget.state.items.length >= widget.state.max) { api.notify('Stack is full'); return; }
  api.ops([{ op: 'push', path: 'items', value }]);
});
el.querySelector('.stack-pop').addEventListener('click', () => api.ops([{ op: 'pop', path: 'items' }]));
