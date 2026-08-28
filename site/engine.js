export function createEngine(onState) {
  const worker = new Worker(new URL('./worker.js', import.meta.url), { type: 'module' });
  const pending = new Map();
  let seq = 0;
  worker.addEventListener('message', (e) => {
    const m = e.data;
    if (m.ready !== undefined) { onState(m); return; }
    const done = pending.get(m.id);
    if (done) { pending.delete(m.id); done(m); }
  });
  worker.addEventListener('error', (e) => onState({ ready: false, error: e.message || 'see the browser console' }));
  return {
    verifyPassword(bytes, password) {
      return new Promise((resolve) => {
        const id = ++seq;
        pending.set(id, (m) => resolve(!!m.valid));
        worker.postMessage({ id, verify: true, bytes, password }, [bytes]);
      });
    },
    run(bytes, level, opts) {
      return new Promise((resolve) => {
        const id = ++seq;
        pending.set(id, resolve);
        worker.postMessage({ id, bytes, level, opts }, [bytes]);
      });
    },
  };
}
