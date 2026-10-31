// A port's patcher questions (PATCHER_QUESTIONS, a Lua file such as setup/questions.lua) run as PortMaster's
// patcher runs them on the handheld, but here, for a handheld the player describes: the file is a Lua chunk
// that returns { {id=, question=, options={{value, label}, ...}}, ... } and may compute them first from the
// environment (DISPLAY_WIDTH, DISPLAY_HEIGHT, ...) and files (/proc/meminfo). It runs in fengari (Lua 5.3 in
// JavaScript, src/vendor/fengari-web.js, MIT) with only those: os.getenv and io.open answer from the handheld
// given, everything else that reaches outside is removed.
'use strict';
const QUESTIONS = (() => {
const F = (() => {
  if (typeof fengari !== 'undefined') return fengari;
  if (typeof window === 'undefined') globalThis.window = globalThis;   // (its wrapper names window)
  return require('./vendor/fengari-web.js');
})();
const { lua, lauxlib, lualib, to_luastring: ls } = F;

const SANDBOX = `
local E, FILES = __ENV, __FILES
__ENV, __FILES = nil, nil
local real_os = os
os = { getenv = function(k) return E[k] end, time = real_os.time, date = real_os.date, clock = real_os.clock,
       difftime = real_os.difftime }
local function file(s)
  local pos, f = 1, {}
  function f:read(...)
    local fmts = { ... }
    if #fmts == 0 then fmts = { "l" } end
    local out = {}
    for i, fmt in ipairs(fmts) do
      if type(fmt) == "number" then
        if pos <= #s then out[i] = s:sub(pos, pos + fmt - 1); pos = pos + fmt end
      else
        local c = fmt:gsub("^%*", ""):sub(1, 1)
        if c == "a" then out[i] = s:sub(pos); pos = #s + 1
        elseif c == "n" then
          local a, b = s:find("^%s*[-+]?%d+%.?%d*", pos)
          if a then out[i] = tonumber(s:sub(a, b)); pos = b + 1 end
        elseif (c == "l" or c == "L") and pos <= #s then
          local e = s:find("\\n", pos, true)
          out[i] = s:sub(pos, e and (c == "L" and e or e - 1) or #s); pos = e and e + 1 or #s + 1
        end
      end
      if out[i] == nil then break end
    end
    return table.unpack(out, 1, #fmts)
  end
  function f:lines(...) local a = table.pack(...); return function() return self:read(table.unpack(a, 1, a.n)) end end
  function f:close() return true end
  function f:seek() return "set", pos - 1 end
  return f
end
io = {
  open = function(p, m)
    if m and m:find("[wa+]") then return nil, p .. ": Permission denied", 13 end
    local s = FILES[p]
    if s == nil then return nil, p .. ": No such file or directory", 2 end
    return file(s)
  end,
  popen = function() return nil, "not available", 1 end,
}
io.lines = function(p, ...) local f, e = io.open(p); if not f then error(e, 2) end; return f:lines(...) end
require, dofile, loadfile, package, debug, collectgarbage = nil, nil, nil, nil, nil, nil
unpack = table.unpack                      -- (the patcher runs LuaJIT: Lua 5.1 names)
loadstring = function(s, n) return load(s, n, "t") end
local function json(v)
  local t = type(v)
  if t == "string" then
    return '"' .. v:gsub('[%c"\\\\]', function(c) return string.format("\\\\u%04x", c:byte()) end) .. '"'
  elseif t == "number" or t == "boolean" then return tostring(v)
  elseif t == "table" then
    local parts = {}
    if #v > 0 or next(v) == nil then
      for i = 1, #v do parts[i] = json(v[i]) end
      return "[" .. table.concat(parts, ",") .. "]"
    end
    for k, x in pairs(v) do
      if type(k) == "string" or type(k) == "number" then parts[#parts + 1] = json(tostring(k)) .. ":" .. json(x) end
    end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  return "null"
end
__JSON = json
`;

function check(L, rc, what) {
  if (rc === lua.LUA_OK) return;
  const m = lua.lua_tojsstring(L, -1);
  throw new Error(`${what}: ${m}`);
}
function pushTable(L, obj) {
  lua.lua_createtable(L, 0, 0);
  for (const [k, v] of Object.entries(obj)) {
    if (v === undefined || v === null || v === '') continue;
    lua.lua_pushstring(L, ls(String(v)));
    lua.lua_setfield(L, -2, ls(k));
  }
}

// run(source, {env, files}, name) -> [{id, question, options: [[value, label]]}]: the questions the file
// returns on that handheld (questions without an id or options are left out, as the patcher skips them)
function run(source, handheld = {}, name = 'questions.lua') {
  const L = lauxlib.luaL_newstate();
  lualib.luaL_openlibs(L);
  // a runaway file stops after ~20 million instructions
  let steps = 0;
  lua.lua_sethook(L, () => { if (++steps > 2000) lauxlib.luaL_error(L, ls('the questions file runs too long')); }, lua.LUA_MASKCOUNT, 10000);
  pushTable(L, handheld.env || {}); lua.lua_setglobal(L, ls('__ENV'));
  pushTable(L, handheld.files || {}); lua.lua_setglobal(L, ls('__FILES'));
  check(L, lauxlib.luaL_dostring(L, ls(SANDBOX)), 'sandbox');
  lua.lua_getglobal(L, ls('__JSON'));
  check(L, lauxlib.luaL_loadbuffer(L, ls(source), null, ls(`=${name}`)), name);
  check(L, lua.lua_pcall(L, 0, 1, 0), name);
  check(L, lua.lua_pcall(L, 1, 1, 0), `${name} (its result)`);
  const out = JSON.parse(lua.lua_tojsstring(L, -1));
  if (!Array.isArray(out)) throw new Error(`${name} does not return a list of questions`);
  return out.filter(q => q && typeof q.id === 'string' && Array.isArray(q.options) && q.options.length)
    .map(q => ({ id: q.id, question: typeof q.question === 'string' ? q.question : q.id,
      options: q.options.filter(Array.isArray).map(o => [String(o[0]), String(o[1] ?? o[0])]) }));
}

// the handheld as the questions see it: its screen (PortMaster's DISPLAY_WIDTH / DISPLAY_HEIGHT) and memory
// (/proc/meminfo's MemTotal, DEVICE_RAM in GB); what the player does not know is left out, as on a handheld
// where the file cannot read it
function handheld({ width, height, ramMb, portdir } = {}) {
  const env = { DEVICE_ARCH: 'aarch64' };
  if (width && height) Object.assign(env, { DISPLAY_WIDTH: width, DISPLAY_HEIGHT: height });
  if (ramMb) env.DEVICE_RAM = Math.max(1, Math.round(ramMb / 1024));
  if (portdir) env.GAMEDIR = `/roms/ports/${portdir}`;
  const files = {};
  if (ramMb) {
    const kb = Math.round(ramMb * 1024 * 0.94);   // (the kernel's own share is not in MemTotal)
    files['/proc/meminfo'] = `MemTotal:       ${String(kb).padStart(8)} kB\nMemFree:        ${String(Math.round(kb * 0.6)).padStart(8)} kB\n` +
      `MemAvailable:   ${String(Math.round(kb * 0.7)).padStart(8)} kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n`;
  }
  return { env, files };
}

// where the setup puts each answer: the patchscript's `echo "$ID" > "$GAMEDIR/<file>"` lines
function answerFiles(script) {
  const out = {};
  for (const m of (script || '').matchAll(/echo\s+"\$\{?(\w+)\}?"\s*>\s*"\$\{?GAMEDIR\}?\/([^"$]+)"/g)) out[m[1]] = m[2];
  return out;
}
return { run, handheld, answerFiles };
})();
if (typeof module !== 'undefined') module.exports = QUESTIONS;
