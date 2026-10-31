// The port's packing step (pmunity android-data) as its patchscript runs it, read from the patchscript: the
// command line is found and its words expanded with the script's own variables (simple assignments, and
// `[ -f "<file>" ] && V=<value>` tests answered from what is staged); the options become android.js's build
// and pack. An option the page does not do, or an --assembly-patch command it has no build of, is Unsupported:
// the packing is then left to the handheld.
'use strict';
const PACK = (() => {
const A_ = () => (typeof ANDROID !== 'undefined' ? ANDROID : require('./android.js'));
const unsupported = m => new (A_().Unsupported)(m);

// shell words of one command: quotes, $VAR / ${VAR} in double quotes and bare, backslash escapes
function words(text, vars) {
  const out = []; let w = null, i = 0;
  const v = name => { if (!(name in vars)) throw unsupported(`the patchscript's $${name} (not a plain assignment)`); return vars[name]; };
  const dollar = () => {
    const m = /^\$(?:\{(\w+)\}|(\w+))/.exec(text.slice(i));
    if (!m) { i++; return '$'; }
    i += m[0].length; return v(m[1] || m[2]);
  };
  while (i < text.length) {
    const c = text[i];
    if (c === ' ' || c === '\t' || c === '\n') { if (w !== null) out.push(w); w = null; i++; continue; }
    w = w ?? '';
    if (c === "'") { const e = text.indexOf("'", i + 1); w += text.slice(i + 1, e); i = e + 1; }
    else if (c === '"') {
      i++;
      while (text[i] !== '"') {
        if (text[i] === '\\' && '"\\$`'.includes(text[i + 1])) { w += text[i + 1]; i += 2; }
        else if (text[i] === '$') w += dollar();
        else if (text[i] === '`') throw unsupported('a command substitution in the packing command');
        else w += text[i++];
      }
      i++;
    } else if (c === '\\') { w += text[i + 1]; i += 2; }
    else if (c === '$') w += dollar();
    else if (c === ';' || c === '|' || c === '&' || c === '`') break;
    else w += text[i++];
  }
  if (w !== null) out.push(w);
  return out;
}

// the script's variables up to line `upto`: X="..." / X=word lines, and [ -f "F" ] && X=v (has(F) answers it)
function variables(lines, upto, base, has) {
  const vars = { ...base };
  for (const line of lines.slice(0, upto)) {
    for (const st of line.replace(/#.*$/, '').split(';')) {
      const s = st.trim();
      let m = /^(\w+)=("[^"`]*"|[^\s"`$;]*|"?\$\{?\w+\}?[^\s"`;]*"?)$/.exec(s);
      if (m) { try { vars[m[1]] = words(m[2], vars).join(' '); } catch { delete vars[m[1]]; } continue; }
      m = /^\[ -f ("[^"]+"|\S+) \] && (\w+)=(\S*)$/.exec(s);
      if (m) { let f; try { f = words(m[1], vars)[0]; } catch { continue; } if (has(f)) vars[m[2]] = m[3]; }
    }
  }
  return vars;
}

// the `pm android-data ...` line(s) of the script: [line index, the command's words after android-data]
function command(script, base, has) {
  const lines = script.split('\n');
  for (let i = 0; i < lines.length; i++) {
    if (!/^\s*pm android-data\b/.test(lines[i])) continue;
    let text = lines[i], j = i;
    while (/\\\s*$/.test(text) && j + 1 < lines.length) text = text.replace(/\\\s*$/, ' ') + lines[++j];
    text = text.replace(/\s*\|\|.*$/s, '');
    const vars = variables(lines, i, base, has);
    return { line: i, words: words(text.replace(/^\s*pm android-data/, ''), vars), vars };
  }
  return null;
}

// android-data's options -> {flags}: only what android.js does (the other values left at their defaults)
const IGNORED = new Set(['--consume-src', '--no-verify', '--force']);
const SINGLE = new Set(['--src', '--pkg', '--out', '--name', '--package', '--version-code', '--game-managed', '--mono-etc',
  '--verify-bundles', '--pack', '--graphics-apis', '--blit', '--mt-rendering', '--async-upload-buffer', '--rewired-joysticks', '--plugin-map']);
const MULTI = new Set(['--root-file', '--drop-missing-assembly', '--managed-file', '--dllmap', '--beside', '--loose-assets']);
function options(ws) {
  const o = { multi: {}, patch: [], flags: new Set() };
  for (let i = 0; i < ws.length; i++) {
    const w = ws[i];
    if (IGNORED.has(w)) continue;
    if (w === '--no-video-shader') { o.flags.add(w); continue; }
    if (w === '--assembly-patch') { o.patch.push([ws[i + 1], ws[i + 2]]); i += 2; continue; }
    if (SINGLE.has(w)) { o[w] = ws[++i]; continue; }
    if (MULTI.has(w)) { (o.multi[w] = o.multi[w] || []).push(ws[++i]); continue; }
    throw unsupported(`android-data ${w}`);
  }
  if (o['--pack'] !== 'all') throw unsupported(`android-data --pack ${o['--pack'] || 'none'}`);
  if (o['--plugin-map']) throw unsupported('android-data --plugin-map');
  for (const k of ['--src', '--pkg', '--out']) if (!o[k]) throw unsupported(`android-data without ${k}`);
  return o;
}

// codepatch builds the page carries (CODEPATCH): an --assembly-patch command that runs the port's setup/codepatch
// (the binary whose CRC-32 the build lists) -> its arguments ({managed} {out} left out)
function codepatchArgs(cmd, port, known) {
  const ws = words(cmd, {}), k = ws.findIndex(x => /(^|\/)codepatch$/.test(x));
  if (k < 0) throw unsupported(`the assembly patch command ${cmd}`);
  const file = ws[k].replace(/^.*?\/?(setup\/)/, '$1'), e = port.entries.find(x => x.name === `${port.portdir}/${file}`);
  if (!e || !known.includes(e.crc >>> 0)) throw unsupported(`this port's ${file} (not the release the page's codepatch is built from)`);
  const rest = ws.slice(k + 1);
  if (rest.slice(-2).join(' ') !== '{managed} {out}') throw unsupported(`the assembly patch command ${cmd}`);
  return rest.slice(0, -2);
}

return { words, variables, command, options, codepatchArgs };
})();
if (typeof module !== 'undefined') module.exports = PACK;
