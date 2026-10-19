"""Conversion step `audio`: long sound clips stay compressed in memory (cfg['audio']).

Unity decompresses a clip marked DecompressOnLoad to PCM when its scene loads. Clips of at least
`compressed_min_s` seconds (music loops, voices, ambience) are switched to CompressedInMemory: they
are decoded while they play instead, which saves their PCM (for Cuphead's Devil fight 61 MB) for a
little CPU. Short effects, which play often and many at once, keep their PCM. The sound itself is
the same: the same Vorbis data is decoded either way.
"""
from .unity_io import Loaded

DEFAULTS = {'compressed_min_s': 2.0}
VORBIS = 1


def run(job):
    c = dict(DEFAULTS, **job.cfg.get('audio', {}))
    files = job.list('', lambda n: n.endswith('.assets'))
    changed = 0
    for rel in files:
        if job.is_done('audio', rel):
            continue
        lf = Loaded(job.path_in(rel))
        n = 0
        for _, o in lf.objects('AudioClip'):
            t = o.read_typetree()
            if t['m_LoadType'] == 0 and t['m_CompressionFormat'] == VORBIS and t['m_Length'] >= c['compressed_min_s']:
                t['m_LoadType'] = 1
                o.save_typetree(t)
                n += 1
        if n:
            job.write(rel, lf.save())
            changed += n
        lf.close()
        job.mark_done('audio', rel)
    job.log(f'audio: {changed} clips of {c["compressed_min_s"]:g} s or longer kept compressed in memory')
