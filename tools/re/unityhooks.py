#!/usr/bin/env python3
"""The rows of runtime/host/unityhooks.c for one Unity player: each hooked function's offset, size,
instruction set and the FNV-1a 64 hash of its bytes, read from the player's libunity.so with Unity's
symbol file for it (`pmunity player fetch <version> --symbols`: Variations/<backend>/Release/Symbols/
<abi>/libunity.sym.so).

    python3 tools/re/unityhooks.py <libunity.so> <libunity.sym.so> [--unity <version abi>]

It checks what the runtime's hook relies on, and prints no row where a check fails:
  - the symbol file belongs to this library (the same GNU build id, when the library has one; else
    the function's bytes must at least be in the library's text);
  - the bytes the jump replaces (8 in ARM state, 16 in A64, 8 or 10 in Thumb: 10 at an entry that is
    not word-aligned) are instructions that can run anywhere else: in ARM state no pc-relative
    operand, no literal load, no branch but a b<cond>, which the runtime rewrites; in Thumb the same, whole instructions, and an IT block whole
    among them (`bx lr` and a `pop` of the pc may be moved: they return); in A64 no literal load, no
    indirect branch, no write of x16 or x17 (the trampoline's jumps use x16), and a direct branch only
    to outside those bytes (the runtime rewrites direct branches, adr and adrp for their new place);
  - nothing in the function branches into those bytes after the first instruction.
A 'replace' row (a function the runtime replaces whole: only its entry is overwritten, nothing of it
runs again) checks only the last point and that the function is longer than the jump.
The function's meaning (its arguments, the enum values the hook compares) is read by hand for each
version before a row is added; the hash then ties the row to exactly that code.
Pure Python, no Unity code is printed."""
import argparse
import struct
import sys

HOOKS = [
    # (mangled-name prefix, short name[, 'call': a function the runtime only calls (any instruction set,
    # nothing moved), whose hash ties the row to the code read])
    ('_Z25AdjustThreadingModeForAPI', 'AdjustThreadingModeForAPI'),
    # unitypool.c (UA_GLES_POOL): Unity's GLES buffer pool
    ('_ZN17BufferManagerGLES13AcquireBufferE', 'BufferManagerGLES::AcquireBuffer'),
    ('_ZN17BufferManagerGLES13ReleaseBufferE', 'BufferManagerGLES::ReleaseBuffer'),
    ('_ZN17BufferManagerGLES24UpdateLiveSetFromPendingE', 'BufferManagerGLES::UpdateLiveSetFromPending'),
    ('_ZN17BufferManagerGLES16PruneFreeBuffersE', 'BufferManagerGLES::PruneFreeBuffers'),
    ('_ZN17BufferManagerGLES5ClearE', 'BufferManagerGLES::Clear'),
    ('_ZN17BufferManagerGLES13InvalidateAllE', 'BufferManagerGLES::InvalidateAll'),
    ('_ZN17BufferManagerGLESD', 'BufferManagerGLES::~BufferManagerGLES'),
    ('_Z15GetGraphicsCapsv', 'GetGraphicsCaps', 'call'),
    # unitymath.c (UA_INV_MEMO): the 4x4 inverse, its last results kept
    ('_Z20InvertMatrix4x4_FullPKfPf', 'InvertMatrix4x4_Full'),
    ('_Z25InvertMatrix4x4_General3DPKfPf', 'InvertMatrix4x4_General3D'),
    # unityverts.c (UA_FAST_VERTS): dynamic batching's vertex transform
    ('_Z26TransformVerticesInnerLoopILb0ELb0ELb1EE', 'TransformVerticesInnerLoop<false, false, true>'),
    # unityhooks.c (UA_PRESENT_SYNC): where the main thread waits for the last frame's present
    ('_Z61GraphicsDeviceSettings_Set_Custom_PropWaitForPresentSyncPoint', 'GraphicsDeviceSettings.waitForPresentSyncPoint (set)', 'call'),
    ('_ZN15GfxDeviceClient10BeginFrameEv', 'GfxDeviceClient::BeginFrame', 'call'),
    ('_ZN15GfxDeviceClient8EndFrameEv', 'GfxDeviceClient::EndFrame', 'call'),
    # unitybeh.c (UA_BEH_PREFETCH, UA_BEH_EMPTY): the behaviour loops' iterator and the script call, and the code
    # their offsets were read from
    ('_ZN12SafeIteratorI4ListI8ListNodeI9BehaviourEEE4NextEv', 'SafeIterator<List<ListNode<Behaviour> > >::Next'),
    ('_ZN13MonoBehaviour16CallUpdateMethodEi', 'MonoBehaviour::CallUpdateMethod'),
    ('_ZNK10GameObject8IsActiveEv', 'GameObject::IsActive', 'call'),
    # unityscripts.c (UA_SCRIPT_FIND): the script lookup AddComponent and serialization make, a class's change and a
    # script's end (which empty its cache), and the PPtr dereference it calls
    ('_ZN17MonoScriptManager17FindRuntimeScriptE17ScriptingClassPtr', 'MonoScriptManager::FindRuntimeScript(ScriptingClassPtr)'),
    ('_ZN10MonoScript5RenewE17ScriptingClassPtr', 'MonoScript::Renew'),
    ('_ZN10MonoScriptD2Ev', 'MonoScript::~MonoScript (D2)'),
    ('_ZN10MonoScriptD0Ev', 'MonoScript::~MonoScript (D0)', 'call'),
    ('_ZNK4PPtrI10MonoScriptEcvPS0_Ev', 'PPtr<MonoScript>::operator MonoScript*', 'call'),
    ('_ZNK4core8hash_mapIiP6ObjectNS_4hashIiEESt8equal_toIiEE6lookupERKi', 'core::hash_map<int, Object*>::lookup', 'call'),
    ('_ZN4core8hash_mapIiP6ObjectNS_4hashIiEESt8equal_toIiEE5eraseERKi', 'core::hash_map<int, Object*>::erase'),
    ('_ZN20BaseBehaviourManager12CommonUpdateI16BehaviourManagerEEvv', 'BaseBehaviourManager::CommonUpdate<BehaviourManager>', 'call'),
    # fmodasync.c (UA_FMOD_ASYNC, UA_FMOD_COUNT): FMOD's update replaced (it runs on a worker), every entry of FMOD's
    # API gated (its handle's validate, the first call of each), the update's caller (where its result is dropped)
    ('_ZN4FMOD6System6updateEv', 'FMOD::System::update', 'replace'),
    ('_ZN4FMOD7SystemI6updateEv', 'FMOD::SystemI::update', 'call'),
    ('_ZN12AudioManager6UpdateEv', 'AudioManager::Update', 'call'),
    ('_ZN4FMOD7SystemI8validate', 'FMOD::SystemI::validate'),
    ('_ZN4FMOD8ChannelI8validate', 'FMOD::ChannelI::validate'),
    ('_ZN4FMOD13ChannelGroupI8validate', 'FMOD::ChannelGroupI::validate'),
    ('_ZN4FMOD6SoundI8validate', 'FMOD::SoundI::validate'),
    ('_ZN4FMOD4DSPI8validate', 'FMOD::DSPI::validate'),
    ('_ZN4FMOD14DSPConnectionI8validate', 'FMOD::DSPConnectionI::validate'),
    ('_ZN4FMOD7ReverbI8validate', 'FMOD::ReverbI::validate'),
    # unityprefetch.c (UA_PREFETCH): the loops whose call sites get a prefetch, the functions they call there,
    # and the code the structure offsets were read from (all only checked: a call site's one word is changed)
    ('_ZN20BaseBehaviourManager12CommonUpdateI20LateBehaviourManagerEEvv', 'BaseBehaviourManager::CommonUpdate<LateBehaviourManager>', 'call'),
    ('_ZN20BaseBehaviourManager12CommonUpdateI21FixedBehaviourManagerEEvv', 'BaseBehaviourManager::CommonUpdate<FixedBehaviourManager>', 'call'),
    ('_ZN12SafeIteratorI4ListI8ListNodeI13MonoBehaviourEEE4NextEv', 'SafeIterator<List<ListNode<MonoBehaviour> > >::Next', 'call'),
    ('_ZN13MonoBehaviour6UpdateEv', 'MonoBehaviour::Update', 'call'),
    ('_ZNK13MonoBehaviour23IsStateMachineBehaviourEv', 'MonoBehaviour::IsStateMachineBehaviour', 'call'),
    ('_ZNK22SerializableManagedRef14GetScriptCacheEv', 'SerializableManagedRef::GetScriptCache', 'call'),
    ('_ZN9Scripting32GetCachedPtrFromScriptingWrapperE', 'Scripting::GetCachedPtrFromScriptingWrapper', 'call'),
    ('_ZL24PrepareSpriteRenderNodesILb0EE', 'PrepareSpriteRenderNodes<false>', 'call'),
    ('_ZL24PrepareSpriteRenderNodesILb1EE', 'PrepareSpriteRenderNodes<true>', 'call'),
    ('_ZN12BaseRenderer16FlattenBasicDataE', 'BaseRenderer::FlattenBasicData', 'call'),
    ('_Z40ProcessCameraIndexListIsNodeVisibleStep1', 'ProcessCameraIndexListIsNodeVisibleStep1', 'call'),
    ('_Z17IsNodeVisibleFast', 'IsNodeVisibleFast', 'call'),
    ('_ZN25PreOrderPlayableTraverser8TraverseE', 'PreOrderPlayableTraverser::Traverse', 'call'),
    ('_ZN25PreOrderPlayableTraverser14RecursiveVisitE', 'PreOrderPlayableTraverser::RecursiveVisit', 'call'),
]


class Elf:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        if d[:4] != b'\x7fELF':
            raise SystemExit(f'{path}: not an ELF file')
        self.is64 = d[4] == 2
        if self.is64:
            self.machine, = struct.unpack_from('<H', d, 0x12)
            phoff, = struct.unpack_from('<Q', d, 0x20)
            shoff, = struct.unpack_from('<Q', d, 0x28)
            phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', d, 0x36)
        else:
            self.machine, = struct.unpack_from('<H', d, 0x12)
            phoff, = struct.unpack_from('<I', d, 0x1c)
            shoff, = struct.unpack_from('<I', d, 0x20)
            phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from('<HHHHH', d, 0x2a)
        self.segs, self.notes = [], []
        for i in range(phnum):
            o = phoff + i * phentsize
            if self.is64:
                t, fl, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, o)
            else:
                t, off, va, pa, fs, ms, fl, al = struct.unpack_from('<IIIIIIII', d, o)
            if t == 1:
                self.segs.append((va, off, fs, fl))
            elif t == 4:
                self.notes.append((off, fs))
        self.secs = []
        for i in range(shnum):
            o = shoff + i * shentsize
            if self.is64:
                v = struct.unpack_from('<IIQQQQIIQQ', d, o)
            else:
                v = struct.unpack_from('<IIIIIIIIII', d, o)
            self.secs.append(v)       # name, type, flags, addr, off, size, link, info, align, entsize

    def build_id(self):
        d = self.d
        srcs = list(self.notes) + [(s[4], s[5]) for s in self.secs if s[1] == 7]
        for off, size in srcs:
            p = off
            while p + 12 <= off + size:
                nsz, dsz, typ = struct.unpack_from('<III', d, p)
                name = d[p + 12:p + 12 + nsz]
                desc_at = p + 12 + ((nsz + 3) & ~3)
                if typ == 3 and name[:3] == b'GNU':
                    return d[desc_at:desc_at + dsz].hex()
                p = desc_at + ((dsz + 3) & ~3)
        return None

    def file_off(self, va, n):
        for v, o, fs, fl in self.segs:
            if v <= va and va + n <= v + fs:
                return o + va - v, fl
        return None, 0

    def funcs(self):
        d = self.d
        for s in self.secs:
            if s[1] != 2:                       # SHT_SYMTAB
                continue
            stroff = self.secs[s[6]][4]
            ent = s[9]
            for i in range(s[5] // ent):
                o = s[4] + i * ent
                if self.is64:
                    st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', d, o)
                else:
                    st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from('<IIIBBH', d, o)
                if st_info & 15 != 2 or not st_value or st_shndx == 0:
                    continue
                e = d.index(b'\0', stroff + st_name)
                yield d[stroff + st_name:e].decode('utf-8', 'replace'), st_value, st_size


def fnv1a(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
    return h


def arm_movable(w):
    """an ARM-state instruction that computes the same wherever it runs (no pc operand), or a b<cond>, which the
    runtime rewrites for its new place; never a bl"""
    cond = w >> 28
    if cond == 0xf:
        return False                            # unconditional space (blx imm, pld, ...)
    if (w & 0x0f000000) == 0x0b000000:
        return False                            # bl
    if (w & 0x0f000000) == 0x0a000000:
        return True                             # b<cond>: the runtime loads its target into the pc instead
    if (w & 0x0ffffff0) == 0x012fff10 or (w & 0x0ffffff0) == 0x012fff30:
        return False                            # bx / blx reg
    if (w & 0x0e000000) == 0x08000000:          # ldm / stm: pc in the list or as base
        return not (w & 0x8000) and ((w >> 16) & 15) != 15
    regs = [(w >> 16) & 15, (w >> 12) & 15, w & 15]
    if (w & 0x0c000000) == 0x04000000:          # ldr / str immediate or register
        regs = [(w >> 16) & 15, (w >> 12) & 15] + ([w & 15] if w & 0x02000000 else [])
    return 15 not in regs


def a64_target(w, pc):
    """the target of an A64 direct branch at pc, else None"""
    if (w & 0x7c000000) == 0x14000000:          # b / bl
        imm = w & 0x3ffffff
        return pc + 4 * (imm - (1 << 26) if imm & 0x2000000 else imm)
    if (w & 0xff000010) == 0x54000000 or (w & 0x7e000000) == 0x34000000:    # b.cond, cbz / cbnz
        imm = (w >> 5) & 0x7ffff
        return pc + 4 * (imm - (1 << 19) if imm & 0x40000 else imm)
    if (w & 0x7e000000) == 0x36000000:          # tbz / tbnz
        imm = (w >> 5) & 0x3fff
        return pc + 4 * (imm - (1 << 14) if imm & 0x2000 else imm)
    return None


def a64_movable(w):
    """an A64 instruction the trampoline can run: one that computes the same wherever it runs, or a direct
    branch, adr or adrp, which the runtime rewrites for its new place. Never a literal load or an indirect
    branch, and none that may write x16 or x17 (any register field a write can be in counts as one)"""
    if (w & 0x3b000000) == 0x18000000:
        return False                            # ldr (literal), ldrsw, prfm literal
    if (w & 0xfe000000) == 0xd6000000:
        return False                            # br / blr / ret
    if (w & 0x7c000000) == 0x14000000:
        return True                             # b / bl (no register field)
    regs = [w & 31]
    if (w & 0x0a000000) == 0x08000000:          # loads and stores: the base (writeback), a pair's second register
        regs.append((w >> 5) & 31)
        if (w & 0x3a000000) == 0x28000000:
            regs.append((w >> 10) & 31)
    return not any(r in (16, 17) for r in regs)


def thumb_insns(code):
    """(offset, halfword 1, halfword 2 or None) for each Thumb instruction of code"""
    i = 0
    while i + 2 <= len(code):
        h, = struct.unpack_from('<H', code, i)
        if h >> 11 in (0x1d, 0x1e, 0x1f) and i + 4 <= len(code):
            yield i, h, struct.unpack_from('<H', code, i + 2)[0]
            i += 4
        else:
            yield i, h, None
            i += 2


def thumb_movable(h, h2):
    """a Thumb instruction that computes the same wherever it runs (no pc operand, no branch but a return)"""
    if h2 is None:
        if h == 0x4770 or (h & 0xff00) == 0xbd00:
            return True                         # bx lr, pop {..., pc}: return
        if (h & 0xf000) == 0xd000 or (h & 0xf800) == 0xe000 or (h & 0xf500) == 0xb100:
            return False                        # b<c>, b, cbz / cbnz
        if (h & 0xf800) == 0x4800 or (h & 0xf800) == 0xa000:
            return False                        # ldr literal, adr
        if (h & 0xfc00) == 0x4400:              # add / cmp / mov / bx / blx with high registers
            return (h & 0x0078) != 0x0078 and (h & 0x0087) != 0x0087 and (h & 0xff00) != 0x4700
        return True
    if (h & 0xf800) == 0xf000 and h2 & 0x8000:
        return False                            # b.w, b<c>.w, bl, blx, and the other branch-space forms
    return (h & 0xf) != 0xf                     # Rn = pc (literal loads, adr.w, tbb, ...): never moved


def thumb_entry_ok(code, moved):
    """the first `moved` bytes are whole Thumb instructions that can be moved, any IT block whole among them"""
    it = 0
    for off, h, h2 in thumb_insns(code[:moved + 4]):
        if off >= moved:
            return off == moved and it == 0
        if off + (4 if h2 is not None else 2) > moved or not thumb_movable(h, h2):
            return False
        if it:
            it -= 1
        if h2 is None and (h & 0xff00) == 0xbf00 and h & 0xf:     # IT: up to four instructions follow
            m = h & 0xf
            it = 4 - ((m & -m).bit_length() - 1)
    return it == 0


def thumb_targets(code, start):
    """(pc, target) of each direct Thumb branch in code at start"""
    for off, h, h2 in thumb_insns(code):
        pc = start + off
        if h2 is None:
            if (h & 0xf000) == 0xd000 and (h & 0x0e00) != 0x0e00:
                yield pc, pc + 4 + (((h & 0xff) ^ 0x80) - 0x80) * 2
            elif (h & 0xf800) == 0xe000:
                yield pc, pc + 4 + (((h & 0x7ff) ^ 0x400) - 0x400) * 2
            elif (h & 0xf500) == 0xb100:
                yield pc, pc + 4 + (((h >> 9) & 1) << 6 | ((h >> 3) & 0x1f) << 1)
        elif (h & 0xf800) == 0xf000 and h2 & 0x8000:
            s_, j1, j2 = (h >> 10) & 1, (h2 >> 13) & 1, (h2 >> 11) & 1
            if h2 & 0x1000:                      # b.w, bl, blx
                i1, i2 = 1 - (j1 ^ s_), 1 - (j2 ^ s_)
                imm = s_ << 24 | i1 << 23 | i2 << 22 | (h & 0x3ff) << 12 | (h2 & 0x7ff) << 1
                yield pc, pc + 4 + imm - (s_ << 25)
            elif (h & 0x0380) != 0x0380:         # b<c>.w
                imm = s_ << 20 | j2 << 19 | j1 << 18 | (h & 0x3f) << 12 | (h2 & 0x7ff) << 1
                yield pc, pc + 4 + imm - (s_ << 21)


def branches_into(code, start, isa, lo, hi):
    """offsets in the function whose direct branch target lies in [lo, hi)"""
    if isa == 't':
        return [pc for pc, t in thumb_targets(code, start) if lo <= t < hi]
    hits = []
    for i in range(0, len(code) - 3, 4):
        w, = struct.unpack_from('<I', code, i)
        pc = start + i
        t = None
        if isa == 'a':
            if (w & 0x0e000000) == 0x0a000000 and w >> 28 != 0xf:
                imm = w & 0xffffff
                imm = imm - (1 << 24) if imm & 0x800000 else imm
                t = pc + 8 + imm * 4
        else:
            t = a64_target(w, pc)
        if t is not None and lo <= t < hi:
            hits.append(pc)
    return hits


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('lib')
    ap.add_argument('sym')
    ap.add_argument('--unity', default='?', help='the player, for the row (e.g. "6000.0.61f1 armeabi-v7a")')
    a = ap.parse_args(argv)
    lib, sym = Elf(a.lib), Elf(a.sym)
    bl, bs = lib.build_id(), sym.build_id()
    if bl and bs and bl != bs:
        raise SystemExit(f'the symbol file is not this library\'s: build id {bs} against {bl}')
    funcs = {}
    for name, val, size in sym.funcs():
        for pre, short, *kind in HOOKS:
            if name.startswith(pre) and (val, size) not in [(g[0], g[1]) for g in funcs.get(short, [])]:
                funcs.setdefault(short, []).append((val, size, name))     # (D1/D2 and C1/C2: one function)
    ok = 0
    for pre, short, *kind in HOOKS:
        got = funcs.get(short, [])
        if len(got) != 1:
            print(f'// {short}: {len(got)} symbols ({", ".join(g[2] for g in got) or "none"}): no row', file=sys.stderr)
            continue
        val, size, name = got[0]
        if lib.is64:
            isa, addr = 'x', val
        else:
            isa, addr = ('t' if val & 1 else 'a'), val & ~1
        call, replace = kind == ['call'], kind == ['replace']
        off, fl = lib.file_off(addr, size)
        if off is None or not (fl & 1):
            print(f'// {short}: +{addr:#x} is not in the library\'s code: no row', file=sys.stderr)
            continue
        code = lib.d[off:off + size]
        moved = 8 if isa == 'a' else 16 if isa == 'x' else (10 if addr & 2 else 8)
        isa_name = {"a": "ARM", "t": "THUMB", "x": "A64"}[isa]
        if call:
            print(f'    {{ "{a.unity}", "{short}", {addr:#x}, {size:#x}, ISA_{isa_name}, {fnv1a(code):#018x}ull }},')
            ok += 1
            continue
        if replace:
            into = branches_into(code, addr, isa, addr + 1, addr + moved)
            if size <= moved or into:
                print(f'// {short}: {size} bytes, branches into the entry from {into}: no row', file=sys.stderr)
                continue
            print(f'    {{ "{a.unity}", "{short}", {addr:#x}, {size:#x}, ISA_{isa_name}, {fnv1a(code):#018x}ull }},')
            ok += 1
            continue
        if isa == 't':
            if not thumb_entry_ok(code, moved):
                print(f'// {short}: the entry\'s {moved} bytes cannot be moved: no row', file=sys.stderr)
                continue
        else:
            words = struct.unpack_from(f'<{moved // 4}I', code, 0)
            mov = arm_movable if isa == 'a' else a64_movable
            bad = [i for i, w in enumerate(words) if not mov(w)]
            if bad:
                print(f'// {short}: instruction {bad[0]} of the entry cannot be moved: no row', file=sys.stderr)
                continue
        # (a branch among the moved instructions to the first one would reach the jump to the hook)
        into = branches_into(code, addr, isa, addr + (2 if isa == 't' else 4), addr + moved) or \
            branches_into(code[:moved], addr, isa, addr, addr + 2)
        if into:
            print(f'// {short}: branches into the entry from {", ".join(hex(x) for x in into)}: no row', file=sys.stderr)
            continue
        print(f'    {{ "{a.unity}", "{short}", {addr:#x}, {size:#x}, ISA_{isa_name}, {fnv1a(code):#018x}ull }},')
        ok += 1
    # fmodasync.c: where AudioManager::Update's call of FMOD::System::update returns
    am, up = funcs.get('AudioManager::Update', []), funcs.get('FMOD::System::update', [])
    if len(am) == 1 and len(up) == 1:
        val, size, _ = am[0]
        isa = 'x' if lib.is64 else ('t' if val & 1 else 'a')
        addr, target = val & ~1 if isa != 'x' else val, up[0][0] & ~1
        off, _ = lib.file_off(addr, size)
        code = lib.d[off:off + size]
        sites = []
        if isa == 't':
            sites = [pc + 4 for pc, t in thumb_targets(code, addr) if t == target]
        else:
            for i in range(0, size - 3, 4):
                w, = struct.unpack_from('<I', code, i)
                pc = addr + i
                t = a64_target(w, pc) if isa == 'x' else (pc + 8 + ((((w & 0xffffff) ^ 0x800000) - 0x800000) * 4)
                                                         if (w & 0x0f000000) == 0x0b000000 else None)
                if t == target and ((w & 0xfc000000) == 0x94000000 or isa == 'a'):
                    sites.append(pc + 4)
        print(f'    // AudioManager::Update: FMOD::System::update returns to '
              f'{", ".join(hex(x | (isa == "t")) for x in sites) or "(no call found)"}')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
