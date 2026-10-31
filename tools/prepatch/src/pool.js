// The workers, each with its own pmucore instance (pixels.js serve() on the worker side): textures (convert), a
// bundle's LZ4 chunks (lz4hc), and the chunks of a big texture that its worker shares ({help}), which idle workers
// take before any new texture so the textures in flight finish sooner. Same code for Web Workers and worker_threads:
// spawn() -> {post(message, transfers), listen(onMessage, onError), end()}.
'use strict';
const POOL = (() => {
function create(n, spawn, init) {
  const ws = [], idle = [], queue = [], help = [], waiting = new Map(), readyQ = [];
  let id = 0, inflight = 0, failed = null;
  const send = (w, msg, tr, cb) => { const k = id++; waiting.set(k, cb); w.post({ id: k, ...msg }, tr); };
  const pump = () => {
    while (idle.length && (help.length || queue.length)) {
      const w = idle.shift();
      if (help.length) {
        const h = help.shift();
        send(w, { astc: h.job }, [h.job.sub.buffer], [out => h.owner.post({ helped: h.tag, k: h.k, out }, [out.buffer]),
          err => h.owner.post({ helped: h.tag, k: h.k, err: err.message })]);
        continue;
      }
      const [task, cb] = queue.shift();
      if (task.lz4) { const copies = task.lz4.map(x => x.slice()); send(w, { lz4: copies }, copies.map(x => x.buffer), cb); continue; }
      const raw = task.raw.slice();                       // (a copy the worker owns; the original may be part of a file)
      inflight++;
      send(w, { task: { ...task, raw } }, [raw.buffer], [r => { inflight--; cb[0](r); }, e => { inflight--; cb[1](e); }]);
    }
    while (readyQ.length && inflight + queue.length < 2 * n) readyQ.shift()();
  };
  for (let i = 0; i < n; i++) {
    const w = spawn();
    w.listen(m => {
      if (m.ready) { idle.push(w); pump(); return; }
      if (m.help !== undefined) { m.jobs.forEach((job, k) => help.push({ owner: w, tag: m.help, k, job })); pump(); return; }
      if (m.claim !== undefined) {                        // the owner takes one of its own chunks back
        const i = help.findIndex(h => h.owner === w && h.tag === m.claim);
        if (i < 0) { w.post({ mine: m.claim, k: -1 }); return; }
        const [h] = help.splice(i, 1);
        w.post({ mine: h.tag, k: h.k, job: h.job }, [h.job.sub.buffer]);
        return;
      }
      const cb = waiting.get(m.id); waiting.delete(m.id); idle.push(w);
      m.err ? cb[1](new Error(m.err)) : cb[0](m.r);
      pump();
    }, msg => { failed = msg || 'a worker failed'; for (const [, cb] of waiting) cb[1](new Error(failed)); waiting.clear(); });
    w.post(init);
    ws.push(w);
  }
  const run = task => failed ? Promise.reject(new Error(failed)) : new Promise((a, b) => { queue.push([task, [a, b]]); pump(); });
  return {
    convert: run,
    ready: () => inflight + queue.length < 2 * n ? Promise.resolve() : new Promise(a => readyQ.push(a)),
    // a bundle's chunks compressed on every worker (64 a batch), in their order
    lz4hc: async chunks => { const batches = []; for (let i = 0; i < chunks.length; i += 64) batches.push(chunks.slice(i, i + 64));
      return (await Promise.all(batches.map(b => run({ lz4: b })))).flat(); },
    close: () => ws.forEach(w => w.end()),
  };
}
// worker_threads (Node tests): each worker runs pixels.js serve()
function node(n, wasm, guide, opts = {}) {
  const { Worker } = require('worker_threads'), px = require.resolve('./pixels.js').replace(/\\/g, '/');
  const src = `const { parentPort } = require('worker_threads');
    const h = require('${px}').serve((m, t) => parentPort.postMessage(m, t)); parentPort.on('message', h);`;
  return create(n, () => { const w = new Worker(src, { eval: true });
    return { post: (m, t) => w.postMessage(m, t), listen: (f, e) => { w.on('message', f); w.on('error', x => e(x.message)); }, end: () => w.terminate() }; },
  { init: 1, wasm, guide, split: opts.split });
}
return { create, node };
})();
if (typeof module !== 'undefined') module.exports = POOL;
