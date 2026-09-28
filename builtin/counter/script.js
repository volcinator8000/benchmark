el.querySelectorAll('[data-delta]').forEach((b) =>
  b.addEventListener('click', () =>
    api.ops([{ op: 'increment', path: 'count', value: +b.dataset.delta * widget.state.step }])));
el.querySelector('[data-reset]').addEventListener('click', () => api.ops([{ op: 'set', path: 'count', value: 0 }]));
el.querySelector('input[name=step]').addEventListener('change', (e) => {
  const step = Math.max(1, Math.floor(+e.target.value || 1));
  api.ops([{ op: 'set', path: 'step', value: step }]);
});
