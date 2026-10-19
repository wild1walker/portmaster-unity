#!/usr/bin/env python3
"""Import/relocation analysis of Android ARMv7 shared objects.

Run inside the ua-spec docker image (debian:bookworm + python3-pyelftools +
libc6-armhf-cross), e.g.
  docker run --rm -v "$PWD":/w ua-spec python3 -I /w/spec/tools/elfimports.py OUTDIR LIB...

For each LIB writes OUTDIR/imports_<name>.txt (one line per undefined dynamic
symbol: provider<TAB>name<TAB>type<TAB>bind<TAB>version<TAB>reloc-types<TAB>flags)
and prints a summary (json) to stdout.
"""
import sys, os, json, collections, struct
from elftools.elf.elffile import ELFFile
from elftools.elf.dynamic import DynamicSection
from elftools.elf.relocation import RelocationSection
from elftools.elf.gnuversions import GNUVerSymSection, GNUVerNeedSection, GNUVerDefSection

GLIBC_DIR = '/usr/arm-linux-gnueabihf/lib'

ARM_RELOC = {0: 'R_ARM_NONE', 2: 'R_ARM_ABS32', 3: 'R_ARM_REL32', 17: 'R_ARM_TLS_DTPMOD32',
             18: 'R_ARM_TLS_DTPOFF32', 19: 'R_ARM_TLS_TPOFF32', 20: 'R_ARM_COPY',
             21: 'R_ARM_GLOB_DAT', 22: 'R_ARM_JUMP_SLOT', 23: 'R_ARM_RELATIVE',
             160: 'R_ARM_IRELATIVE'}


def exports(path):
    s = set()
    with open(path, 'rb') as f:
        e = ELFFile(f)
        ds = e.get_section_by_name('.dynsym')
        for sym in ds.iter_symbols():
            if sym['st_shndx'] != 'SHN_UNDEF' and sym['st_info']['bind'] in ('STB_GLOBAL', 'STB_WEAK'):
                s.add(sym.name)
    return s


GLIBC = {}
for lib in ('libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0', 'librt.so.1',
            'libgcc_s.so.1', 'libstdc++.so.6', 'ld-linux-armhf.so.3'):
    p = os.path.join(GLIBC_DIR, lib)
    if os.path.exists(p):
        GLIBC[lib] = exports(p)
GLIBC_C = GLIBC['libc.so.6'] | GLIBC.get('libpthread.so.0', set()) | GLIBC.get('librt.so.1', set()) \
    | GLIBC.get('libdl.so.2', set()) | GLIBC.get('ld-linux-armhf.so.3', set())

DL = {'dlopen', 'dlsym', 'dlclose', 'dlerror', 'dladdr', 'dl_iterate_phdr', 'dl_unwind_find_exidx',
      'android_dlopen_ext', 'android_get_LD_LIBRARY_PATH', 'android_update_LD_LIBRARY_PATH'}
ZLIB_PREFIX = ('inflate', 'deflate', 'compress', 'uncompress', 'gz', 'zlib', 'zError', 'crc32',
               'adler32', 'get_crc_table')
SLES = ('slCreateEngine', 'slQuery', 'SL_IID_')


LIBMAIN_EXPORTS = set()


def provider(name, needed, ver=''):
    if 'libmain.so' in needed and name in LIBMAIN_EXPORTS:
        return 'libmain.so'
    if ver.endswith('@libm.so'):
        return 'libm'
    if ver.endswith('@libdl.so'):
        return 'libdl'
    if name.startswith('__android_log') or name in ('android_set_abort_message',):
        return 'liblog' if name.startswith('__android_log') else 'libc(bionic)'
    if name.startswith('egl'):
        return 'libEGL'
    if name.startswith('gl') and len(name) > 2 and name[2].isupper():
        return 'libGLESv2'
    if name.startswith(SLES):
        return 'libOpenSLES'
    if name.startswith(('ANativeWindow', 'AAsset', 'ALooper', 'AConfiguration', 'AInput', 'AKeyEvent',
                        'AMotionEvent', 'ASensor', 'AStorageManager', 'ANativeActivity', 'AObbInfo',
                        'AChoreographer', 'ATrace', 'AHardwareBuffer', 'ASharedMemory')):
        return 'libandroid'
    if name.startswith(ZLIB_PREFIX):
        return 'libz'
    if name in DL:
        return 'libdl'
    if name.startswith('_Z') or name.startswith('__cxa_') or name.startswith('__gxx_'):
        if name in ('__cxa_atexit', '__cxa_finalize'):
            return 'libc'
        return 'libstdc++'
    if name in GLIBC['libm.so.6'] and name not in GLIBC['libc.so.6']:
        return 'libm'
    if name.startswith('__aeabi_') or name.startswith('__gnu_') or name.startswith('_Unwind_') \
            or name in GLIBC.get('libgcc_s.so.1', set()) and name not in GLIBC_C:
        return 'libgcc(compiler-rt; exported by bionic libc on LP32)'
    return 'libc'


# imports whose prototype has float/double args or return value: the Android libs use the
# softfp (base AAPCS) convention, our host is armhf (AAPCS-VFP) -> need pcs("aapcs") thunks.
SOFTFP_FLOAT = set('''acos acosf asin asinf atan atan2 atan2f atanf ceil ceilf cos cosf cosh exp expf fabs fabsf
floor floorf fmax fmaxf fmin fminf fmod fmodf frexp ldexp log log10 log10f logf modf modff pow powf remainder
rint round roundf sin sinf sinh sqrt sqrtf tan tanf tanh trunc truncf strtod strtof atof difftime __isfinite
ASensor_getResolution'''.split())
# bionic vs glibc ABI differences (struct layout / constants / missing symbol)
ABI_NOTE = {
    '__sF': 'bionic FILE[3] array (84 bytes each); map &__sF[0..2] to glibc stdin/stdout/stderr',
    '__errno': 'bionic name of __errno_location',
    '_ctype_': 'bionic/OpenBSD ctype table char[257] (index c+1)', '_tolower_tab_': 'short[257] table',
    '_toupper_tab_': 'short[257] table', '__page_size': 'unsigned int variable = 4096',
    '__system_property_get': 'Android properties; return fake values', '__assert2': '(file,line,func,msg)',
    '__get_h_errno': 'glibc __h_errno_location', 'strlcpy': 'not in glibc<2.38', 'tkill': 'syscall(SYS_tkill)',
    '__pthread_cleanup_push': 'bionic cleanup list', '__pthread_cleanup_pop': 'bionic cleanup list',
    'sigsetjmp': 'bionic sigjmp_buf is 256 bytes; glibc writes 388 -> own implementation',
    'setjmp': 'bionic jmp_buf is 256 bytes (and setjmp saves sigmask); glibc writes 388 -> own implementation',
    'longjmp': 'pair with own setjmp', 'siglongjmp': 'pair with own sigsetjmp',
    '__data_start': 'WEAK; Boehm GC data-root start: give libmono RW segment start or 0, never host value',
    'data_start': 'WEAK; same as __data_start',
    '__google_potentially_blocking_region_begin': 'WEAK; must resolve to 0',
    '__google_potentially_blocking_region_end': 'WEAK; must resolve to 0',
    '__gnu_Unwind_Find_exidx': 'must search OUR modules PT_ARM_EXIDX (glibc version uses dl_iterate_phdr and will not see them)',
    'pthread_cond_timedwait_relative_np': 'bionic-only; relative timeout',
    'sysconf': 'bionic _SC_* numbering differs (0x28 PAGE_SIZE, 0x61 NPROCESSORS_ONLN, 0x62 PHYS_PAGES, 6 CLK_TCK)',
    'dlopen': 'bionic RTLD_* values (LAZY=1 NOW=0 GLOBAL=2 NOLOAD=4); route through loader',
    'dlsym': 'bionic RTLD_DEFAULT=0xffffffff RTLD_NEXT=0xfffffffe', 'dlclose': 'loader', 'dlerror': 'loader',
    'stat': 'bionic struct stat == kernel stat64 (104 B): call stat64', 'fstat': 'call fstat64',
    'lstat': 'call lstat64', 'statfs': 'bionic struct statfs == statfs64: call statfs64',
    'readdir': 'bionic struct dirent == dirent64: call readdir64',
    'pthread_mutex_init': 'bionic pthread_mutex_t is 4 bytes (glibc 24); static init 0 / 0x4000 recursive',
    'pthread_mutex_lock': 'mutex 4 bytes', 'pthread_mutex_unlock': 'mutex 4 bytes', 'pthread_mutex_trylock': 'mutex 4 bytes',
    'pthread_mutex_destroy': 'mutex 4 bytes',
    'pthread_cond_init': 'bionic pthread_cond_t is 4 bytes (glibc 48); static init 0',
    'pthread_cond_wait': 'cond 4 bytes', 'pthread_cond_timedwait': 'cond 4 bytes', 'pthread_cond_signal': 'cond 4 bytes',
    'pthread_cond_broadcast': 'cond 4 bytes', 'pthread_cond_destroy': 'cond 4 bytes',
    'pthread_attr_init': 'bionic pthread_attr_t is 24 bytes (glibc 36)', 'pthread_attr_destroy': 'attr 24 bytes',
    'pthread_attr_setdetachstate': 'attr 24 bytes', 'pthread_attr_getdetachstate': 'attr 24 bytes',
    'pthread_attr_setstacksize': 'attr 24 bytes', 'pthread_attr_setschedparam': 'attr 24 bytes',
    'pthread_attr_getstack': 'attr 24 bytes', 'pthread_getattr_np': 'fills bionic attr (24 bytes)',
    'pthread_create': 'takes bionic attr', 'pthread_exit': 'run bionic cleanup handlers',
    'sem_init': 'bionic sem_t is 4 bytes (glibc 16)', 'sem_destroy': 'sem 4 bytes', 'sem_post': 'sem 4 bytes',
    'sem_wait': 'sem 4 bytes', 'sem_trywait': 'sem 4 bytes', 'sem_timedwait': 'sem 4 bytes', 'sem_getvalue': 'sem 4 bytes',
    'sigaction': 'bionic struct sigaction 16 B (sigset 4 B) vs glibc 140 B',
    'sigprocmask': 'bionic sigset_t 4 bytes (glibc 128)', 'pthread_sigmask': 'sigset_t 4 bytes',
    'sigsuspend': 'sigset_t 4 bytes', 'bsd_signal': 'signal()',
    'getpwuid': 'bionic LP32 struct passwd has no pw_gecos', 'getpwnam': 'bionic LP32 struct passwd has no pw_gecos',
    'getaddrinfo': 'struct addrinfo: ai_canonname/ai_addr swapped; AI_* values differ',
    'freeaddrinfo': 'pair with getaddrinfo wrapper', 'getnameinfo': 'NI_* flag values differ',
    'mbrtowc': 'bionic mbstate_t 4 bytes vs glibc 8', 'wcrtomb': 'mbstate_t 4 vs 8 bytes',
    'setlocale': 'bionic only C locale; return "C"',
    'clearerr': 'FILE*', 'fclose': 'FILE*', 'fflush': 'FILE* (NULL ok)', 'fgets': 'FILE*', 'fprintf': 'FILE*',
    'fputc': 'FILE*', 'fputs': 'FILE*', 'fread': 'FILE*', 'fscanf': 'FILE*', 'fseek': 'FILE*; long offset (archives <2GiB)',
    'ftell': 'FILE*', 'fwrite': 'FILE*', 'getc': 'FILE*', 'putc': 'FILE*', 'ungetc': 'FILE*', 'getwc': 'FILE*',
    'putwc': 'FILE*', 'ungetwc': 'FILE*', 'setvbuf': 'FILE*', 'vfprintf': 'FILE*', 'fdopen': 'returns FILE*',
    'fopen': 'returns FILE*', 'popen': 'returns FILE*', 'pclose': 'FILE*',
    'environ': 'glibc environ', '__stack_chk_guard': 'provide variable',
    '__aeabi_atexit': '(obj, dtor, dso) -> __cxa_atexit(dtor, obj, dso)',
}


def analyse(path):
    out = {'path': path}
    with open(path, 'rb') as f:
        e = ELFFile(f)
        out['phdrs'] = [seg['p_type'] for seg in e.iter_segments()]
        dyn = e.get_section_by_name('.dynamic')
        needed, tags = [], {}
        for t in dyn.iter_tags():
            if t.entry.d_tag == 'DT_NEEDED':
                needed.append(t.needed)
            else:
                tags[t.entry.d_tag] = t.entry.d_val
        out['needed'] = needed
        out['flags'] = {k: hex(v) for k, v in tags.items() if k in ('DT_FLAGS', 'DT_FLAGS_1', 'DT_SYMBOLIC', 'DT_TEXTREL')}
        dynsym = e.get_section_by_name('.dynsym')
        syms = list(dynsym.iter_symbols())
        # versions
        versym = e.get_section_by_name('.gnu.version')
        vneed = e.get_section_by_name('.gnu.version_r')
        vdef = e.get_section_by_name('.gnu.version_d')
        vername = {}
        if vneed:
            for verneed, auxs in vneed.iter_versions():
                for a in auxs:
                    vername[a['vna_other']] = '%s@%s' % (a.name, verneed.name)
        if vdef:
            for verdef, auxs in vdef.iter_versions():
                for a in auxs:
                    vername[verdef['vd_ndx']] = a.name + '(def)'
                    break
        out['verneed'] = sorted(set(vername.values()))
        vers = []
        if versym:
            for i in range(versym.num_symbols()):
                vers.append(versym.get_symbol(i)['ndx'])
        # relocations
        relcount = collections.Counter()
        symrel = collections.defaultdict(set)
        relocs = []
        for sec in e.iter_sections():
            if isinstance(sec, RelocationSection):
                for r in sec.iter_relocations():
                    t = ARM_RELOC.get(r['r_info_type'], str(r['r_info_type']))
                    relcount[(sec.name, t)] += 1
                    if r['r_info_sym']:
                        symrel[r['r_info_sym']].add(t)
                    relocs.append((r['r_offset'], r['r_info_type'], r['r_info_sym']))
        out['relocs'] = {'%s %s' % k: v for k, v in sorted(relcount.items())}
        tls = [s.name for s in syms if s['st_info']['type'] == 'STT_TLS']
        out['tls_symbols'] = tls
        und = []
        for i, s in enumerate(syms):
            if i == 0 or s['st_shndx'] != 'SHN_UNDEF':
                continue
            v = vers[i] if i < len(vers) else 0
            vn = vername.get(v, '') if not isinstance(v, str) else ''
            prov = provider(s.name, needed, vn)
            flags = []
            if s.name in SOFTFP_FLOAT:
                flags.append('SOFTFP_FLOAT_ARGS')
            if s.name in ABI_NOTE:
                flags.append('ABI:' + ABI_NOTE[s.name])
            if s.name not in GLIBC_C and s.name not in GLIBC['libm.so.6'] and prov in ('libc', 'libm', 'libdl', 'libc(bionic)'):
                flags.append('NOT_IN_GLIBC')
            und.append({'name': s.name, 'type': s['st_info']['type'].replace('STT_', ''),
                        'bind': s['st_info']['bind'].replace('STB_', ''), 'ver': vn,
                        'rel': sorted(symrel.get(i, [])), 'prov': prov, 'flags': flags})
        out['undefined'] = und
        out['n_defined_exports'] = sum(1 for s in syms if s['st_shndx'] != 'SHN_UNDEF' and s.name)
        out['exports'] = sorted(s.name for s in syms if s['st_shndx'] != 'SHN_UNDEF' and s.name
                                and s['st_info']['bind'] in ('STB_GLOBAL', 'STB_WEAK'))
        # init/fini arrays: read words, apply R_ARM_RELATIVE (addend in place) / ABS32
        def read_array(tag_addr, tag_sz):
            if tag_addr not in tags:
                return []
            addr, sz = tags[tag_addr], tags[tag_sz]
            for seg in e.iter_segments():
                if seg['p_type'] == 'PT_LOAD' and seg['p_vaddr'] <= addr < seg['p_vaddr'] + seg['p_filesz']:
                    off = seg['p_offset'] + addr - seg['p_vaddr']
                    f.seek(off)
                    data = f.read(sz)
                    return ['0x%x' % w for w in struct.unpack('<%dI' % (sz // 4), data)]
            return []
        out['init_array'] = {'addr': hex(tags.get('DT_INIT_ARRAY', 0)), 'count': tags.get('DT_INIT_ARRAYSZ', 0) // 4,
                             'entries': read_array('DT_INIT_ARRAY', 'DT_INIT_ARRAYSZ')}
        out['fini_array'] = {'addr': hex(tags.get('DT_FINI_ARRAY', 0)), 'count': tags.get('DT_FINI_ARRAYSZ', 0) // 4,
                             'entries': read_array('DT_FINI_ARRAY', 'DT_FINI_ARRAYSZ')}
        out['DT_INIT'] = hex(tags['DT_INIT']) if 'DT_INIT' in tags else None
        out['DT_FINI'] = hex(tags['DT_FINI']) if 'DT_FINI' in tags else None
        # TPIDRURO reads (mrc p15,0,Rt,c13,c0,3): ARM encoding 0xEE1D0F70|Rt<<12 (cond AL)
        text = e.get_section_by_name('.text')
        data = text.data()
        base = text['sh_addr']
        arm_hits, thumb_hits = [], []
        for off in range(0, len(data) - 3, 2):
            w = struct.unpack_from('<I', data, off)[0]
            if off % 4 == 0 and (w & 0xFFFF0FFF) == 0xEE1D0F70:
                arm_hits.append(hex(base + off))
            hw1, hw2 = struct.unpack_from('<HH', data, off)
            if hw1 == 0xEE1D and (hw2 & 0x0FFF) == 0x0F70:
                thumb_hits.append(hex(base + off))
        out['tpidruro_reads'] = {'arm': arm_hits[:50], 'arm_count': len(arm_hits),
                                 'thumb': thumb_hits[:50], 'thumb_count': len(thumb_hits)}
    return out


def main():
    outdir = sys.argv[1]
    res = {}
    for p in sys.argv[2:]:
        if os.path.basename(p) == 'libmain.so':
            with open(p, 'rb') as f:
                e = ELFFile(f)
                for sym in e.get_section_by_name('.dynsym').iter_symbols():
                    if sym['st_shndx'] != 'SHN_UNDEF' and sym.name:
                        LIBMAIN_EXPORTS.add(sym.name)
    for p in sys.argv[2:]:
        name = os.path.basename(p).split('.so')[0]
        r = analyse(p)
        res[name] = r
        with open(os.path.join(outdir, 'imports_%s.txt' % name), 'w') as o:
            o.write('# Undefined dynamic symbols of %s\n' % os.path.basename(p))
            o.write('# DT_NEEDED: %s\n' % ' '.join(r['needed']))
            o.write('# columns: provider\tname\ttype\tbind\tversion\trelocs\tflags\n')
            for u in sorted(r['undefined'], key=lambda u: (u['prov'], u['name'])):
                o.write('%s\t%s\t%s\t%s\t%s\t%s\t%s\n' % (u['prov'], u['name'], u['type'], u['bind'], u['ver'] or '-',
                                                        ','.join(u['rel']) or '-', ','.join(u['flags']) or '-'))
        with open(os.path.join(outdir, 'exports_%s.txt' % name), 'w') as o:
            o.write('\n'.join(r['exports']) + '\n')
    for k, r in res.items():
        r = dict(r)
        r['undefined_count'] = len(r['undefined'])
        r['by_provider'] = collections.Counter(u['prov'] for u in r['undefined'])
        r['not_in_glibc'] = [u['name'] for u in r['undefined'] if 'NOT_IN_GLIBC' in u['flags']]
        r['object_imports'] = [u['name'] for u in r['undefined'] if u['type'] == 'OBJECT']
        r['weak_imports'] = [u['name'] for u in r['undefined'] if u['bind'] == 'WEAK']
        del r['undefined']
        del r['exports']
        print(json.dumps({k: r}, indent=1, default=str))


main()
