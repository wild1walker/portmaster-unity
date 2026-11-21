#!/usr/bin/env python3
"""hotlist.py <nm -nS of ua_host.debug> <prof-N.txt>...: runtime/host/hot-arm64.txt, the functions of ua_host (its own
objects and the loader, shims and fake JNI archives linked into it) that UA_PROF recordings sampled on UnityMain, Unity's
render thread and ua_host's GL thread, heaviest first (each recording's samples scaled to the same total per thread),
as many as cover 99.5 % of the weight. runtime/host/build.sh (ARCH=arm64) compiles with one section a function and
renames these .text.hot.<name>: the linker places them together before the rest of the code.
    nm -nS --defined-only runtime/host/out/arm64/ua_host.debug > ua_host.syms   (the build the recordings ran)
    python3 tools/analysis/hotlist.py ua_host.syms pA/prof-1.txt eA/prof-1.txt ... > runtime/host/hot-arm64.txt"""
import sys, bisect, collections
syms=[]
for l in open(sys.argv[1]):
    q=l.split()
    if len(q)==4: syms.append((int(q[0],16),int(q[1],16),q[3]))
syms.sort(); st=[s[0] for s in syms]
W=collections.Counter()
for p in sys.argv[2:]:
    mods=[];names={}; per=collections.defaultdict(collections.Counter)
    for l in open(p):
        if l.startswith('module '):
            q=l.split(); mods.append((int(q[1],16),int(q[2],16),int(q[3],16),q[4]))
        elif l.startswith('thread '):
            q=l.split(); names[q[1]]=q[3]
    ua=[m for m in mods if m[3]=='ua_host'][0]
    for l in open(p):
        if not l.startswith('stack '): continue
        q=l.split(); t=names.get(q[3],'')
        if not (t=='UnityMain' or t.startswith('UnityGfxDevice') or t=='ua_glthread') or q[4]!='R': continue
        pc=int(q[6],16)
        if not ua[0]<=pc<ua[1]: continue
        v=pc-ua[2]; i=bisect.bisect_right(st,v)-1
        if i>=0 and v<syms[i][0]+syms[i][1]: per[t][syms[i][2]]+=int(q[2])
    for t,c in per.items():
        s=sum(c.values())
        for n,w in c.items(): W[n]+=w/s
tot=sum(W.values()); acc=0; out=[]
for n,w in W.most_common():
    out.append(n); acc+=w
    if acc>=0.995*tot: break
sz={s[2]:s[1] for s in syms}
print('# runtime/host/hot-arm64.txt: made by tools/analysis/hotlist.py from %d recordings; %d functions, %d bytes' % (len(sys.argv)-2, len(out), sum(sz.get(n,0) for n in out)))
for n in out: print(n)
