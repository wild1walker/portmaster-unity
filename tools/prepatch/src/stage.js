// The game's files between unpacking and the zip: an ordered map of gamedata/ paths to files. Files of a
// folder the user gave are read where they are; unpacked ones (installers, archives) and converted ones
// are written to a store: the browser's private storage (OPFS), or a folder the user picks.
'use strict';
const STAGE = (() => {

class Stage {
  constructor(store) { this.store = store; this.entries = new Map(); this.n = 0; }
  has(rel) { return this.entries.has(rel); }
  // a file from chunks (an installer's member): the first one of a path wins (later ones are drained)
  async put(rel, size, mode, chunks) {
    if (this.entries.has(rel)) { for await (const _ of chunks) { /* an earlier source had it */ } return false; }
    const handle = await this.store.write(`f${this.n++}`, chunks);
    this.entries.set(rel, { mode, size, handle });
    return true;
  }
  putFile(rel, file, mode) {                    // a File read where it is
    if (this.entries.has(rel)) return false;
    this.entries.set(rel, { mode, size: file.size, file });
    return true;
  }
  async putParts(rel, parts, mode = 0o644) {    // new contents for a path (a converted file): replaces it in its place
    const handle = await this.store.write(`f${this.n++}`, (async function* () { for (const p of parts) yield p; })());
    const old = this.entries.get(rel);
    this.entries.set(rel, { mode: old ? old.mode : mode, size: parts.reduce((a, p) => a + p.length, 0), handle });
  }
  // a file written only when the zip is (an archive built from other files): chunks() gives its bytes each time
  putLazy(rel, size, chunks, mode = 0o644) { this.entries.set(rel, { mode, size, chunks, stored: true }); }
  delete(rel) { this.entries.delete(rel); }
  rename(fn) {                                   // fn(rel) -> new rel, or null to drop it; the order is kept
    const next = new Map();
    for (const [rel, e] of this.entries) { const r = fn(rel); if (r !== null && !next.has(r)) next.set(r, e); }
    this.entries = next;
  }
  async file(rel) { const e = this.entries.get(rel); if (!e) throw new Error(`${rel}: not staged`); if (e.chunks) throw new Error(`${rel}: written with the zip`); return e.file || this.store.file(e.handle); }
  dirs() {                                       // every folder that holds files
    const out = new Set();
    for (const rel of this.entries.keys()) { let i = rel.lastIndexOf('/'); while (i > 0) { out.add(rel.slice(0, i)); i = rel.lastIndexOf('/', i - 1); } }
    return out;
  }
  // the textures step's view of one folder: list(dir), read(rel), slice(rel, off, n)
  view(prefix) {
    const P = prefix ? `${prefix}/` : '';
    return {
      list: async dir => {
        const D = P + (dir ? `${dir}/` : ''), seen = new Map();
        for (const rel of this.entries.keys()) {
          if (!rel.startsWith(D)) continue;
          const rest = rel.slice(D.length), i = rest.indexOf('/');
          if (i < 0) seen.set(rest, false); else if (!seen.has(rest.slice(0, i))) seen.set(rest.slice(0, i), true);
        }
        return [...seen].map(([name, dir]) => ({ name, dir }));
      },
      read: async rel => new Uint8Array(await (await this.file(P + rel)).arrayBuffer()),
      file: rel => this.file(P + rel),
      size: async rel => { const e = this.entries.get(P + rel); return e ? e.size : -1; },     // (-1: no such file)
      slice: async (rel, off, n) => new Uint8Array(await (await this.file(P + rel)).slice(off, off + n).arrayBuffer()),
    };
  }
  async clear() { this.entries.clear(); if (this.store.clear) await this.store.clear(); }
}

// chunks written in pieces of about 8 MB (each write of a FileSystemWritableFileStream costs)
async function writeChunks(w, chunks) {
  let buf = [], size = 0;
  for await (const c of chunks) {
    buf.push(c); size += c.length;
    if (size >= 8 << 20) { await w.write(new Blob(buf)); buf = []; size = 0; }
  }
  if (size) await w.write(new Blob(buf));
}
function handleStore(dir, owner) {             // a directory handle (OPFS, or a folder the user picked)
  return {
    where: owner ? `the folder ${dir.name}` : "the browser's storage",
    async write(name, chunks) {
      const fh = await dir.getFileHandle(name, { create: true });
      const w = await fh.createWritable();
      try { await writeChunks(w, chunks); await w.close(); } catch (e) { try { await w.abort(); } catch { /* gone */ } throw e; }
      return fh;
    },
    file: fh => fh.getFile(),
    async clear() { try { await owner.removeEntry(dir.name, { recursive: true }); } catch { /* left */ } },
  };
}
async function opfs() {                         // a fresh folder in the browser's private storage (old ones removed)
  const root = await navigator.storage.getDirectory();
  for await (const [name] of root.entries()) if (name.startsWith('pmuprepatch-')) { try { await root.removeEntry(name, { recursive: true }); } catch { /* in use */ } }
  const dir = await root.getDirectoryHandle(`pmuprepatch-${Date.now()}`, { create: true });
  return handleStore(dir, root);
}
async function pickedFolder(parent) {           // a work folder the user picks (no OPFS, or too little room in it)
  parent = parent || await showDirectoryPicker({ mode: 'readwrite', id: 'pmuprepatch-work' });
  const dir = await parent.getDirectoryHandle(`pmuprepatch-work-${Date.now()}`, { create: true });
  return handleStore(dir, parent);
}

return { Stage, opfs, pickedFolder, handleStore };
})();
if (typeof module !== 'undefined') module.exports = STAGE;
