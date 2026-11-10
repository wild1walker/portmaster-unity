#!/usr/bin/env python3
# gen-api.py <il2cpp-api-functions.h>... - prints il2cpp_api.inc: for each function of IL2CPP's C
# API, its name and the arguments narrower than 32 bits. A macOS arm64 callee relies on its
# caller having extended those (Apple's convention), an AAPCS64 caller (Unity's Android player)
# does not have to: the bridge's forwarders extend them. Read from the header of the editor
# whose player the route runs with (Unity.app/Contents/il2cpp/libil2cpp); the table is checked
# in, the header never is.
#   API(name)                     nothing to extend
#   API(name, U8(i) S8(i) U16(i) S16(i) ...)   argument i (0-based, all integer registers)
#   APIH(name, H(i) ... U8(i) ...)             as API; argument i is a class or a method, which
#                                              can be one the bridge stands in for (bridge.c)
# The script refuses what a forwarder cannot pass along: more than 8 arguments, a floating or
# by-value composite argument (none in 2017.4 - 6000.2).
import re, sys

NARROW = {'bool': 'U8', 'uint8_t': 'U8', 'unsigned char': 'U8', 'char': 'S8', 'int8_t': 'S8', 'signed char': 'S8',
          'uint16_t': 'U16', 'Il2CppChar': 'U16', 'unsigned short': 'U16', 'int16_t': 'S16', 'short': 'S16'}
WIDE = {'int', 'int32_t', 'uint32_t', 'unsigned int', 'int64_t', 'uint64_t', 'size_t', 'uintptr_t', 'intptr_t',
        'il2cpp_array_size_t', 'Il2CppGCHandle', 'long', 'unsigned long', 'long long', 'Il2CppProfileFlags',
        'Il2CppStat', 'Il2CppRuntimeUnhandledExceptionPolicy', 'Il2CppGCMode', 'Il2CppMethodPointer',
        'Il2CppMethodPointer_NoArgs', 'il2cpp_register_object_callback', 'il2cpp_WorldChangedCallback',
        'il2cpp_liveness_reallocate_callback', 'Il2CppFrameWalkFunc', 'Il2CppBacktraceFunc', 'Il2CppSetFindPlugInCallback',
        'Il2CppLogCallback', 'Il2CppDebuggerTransport', 'Il2CppAndroidUpStateFunc', 'ClassUserDataFunc', 'DataSegmentFunc',
        'GCHeapFunc', 'Il2CppProfileFunc', 'Il2CppProfileMethodFunc', 'Il2CppProfileAllocFunc', 'Il2CppProfileGCFunc',
        'Il2CppProfileGCResizeFunc', 'Il2CppProfileFileIOFunc', 'Il2CppProfileThreadFunc', 'il2cpp_liveness_callback',
        'Il2CppDomainFunc', 'il2cpp_hresult_t', 'Il2CppThreadFunc'}

def kind(param):
    p = re.sub(r'\[\s*\]$', '*', param.strip())
    if re.fullmatch(r'(const\s+)?(Il2CppClass|MethodInfo)\s*\*\s*\w*', p): return 'H'
    if '(' in p or '*' in p or '&' in p: return None                    # a pointer (to a function too)
    words = [w for w in p.replace('const', ' ').split()]
    if not words: return None
    t = ' '.join(words[:-1]) if len(words) > 1 else words[0]            # the last word is the name
    if t in NARROW: return NARROW[t]
    if t in WIDE: return None
    raise SystemExit('gen-api.py: an argument type this script does not know: %r in %r' % (t, param))

def split(args):
    out, depth, cur = [], 0, ''
    for c in args:
        if c == '(': depth += 1
        if c == ')': depth -= 1
        if c == ',' and depth == 0: out.append(cur); cur = ''
        else: cur += c
    if cur.strip(): out.append(cur)
    return [a for a in out if a.strip() and a.strip() != 'void']

rows = {}
for path in sys.argv[1:]:                   # several releases' headers: every name of any of them
  src = open(path).read()
  src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
  src = re.sub(r'//[^\n]*', '', src)
  for m in re.finditer(r'DO_API(?:_NO_RETURN)?\s*\(\s*([^,]+?)\s*,\s*(\w+)\s*,\s*\((.*?)\)\s*\)\s*;', src, flags=re.S):
    name, params = m.group(2), split(m.group(3))
    if len(params) > 8: raise SystemExit('gen-api.py: %s has more than 8 arguments' % name)
    ext = ['%s(%d)' % (k, i) for i, k in ((i, kind(p)) for i, p in enumerate(params)) if k]
    if rows.get(name, ext) != ext: raise SystemExit('gen-api.py: %s differs between the headers' % name)
    rows[name] = ext
for name in sorted(rows):
    marks = sorted(rows[name], key=lambda m: m[0] != 'H')
    print('%s(%s%s)' % ('APIH' if marks and marks[0][0] == 'H' else 'API', name, ', ' + ' '.join(marks) if marks else ''))
