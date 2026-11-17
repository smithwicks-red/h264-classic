# Proof a: compare C-rewritten vs Python-rewritten video tables; audio bytes vs original.
import sys, struct, hashlib, json
sys.path.insert(0, '/home/claude/s5/rw/sim')
import mp4, rewrite

def rdly(t):
    sd = t.stbl.get(b'stsd').data; i = sd.find(b'rdly')
    return struct.unpack('>I', sd[i+4:i+8])[0] if i > 0 else None

def info(path):
    buf, top, moov, T = mp4.load(path)
    rw = [x for x in T if x.handler == b'vide' and rdly(x) is not None]
    t = rw[0] if rw else rewrite.video(T)
    g = lambda x: (t.stbl.get(x).data if t.stbl.get(x) else None)
    ed = t.trak.get(b'edts')
    d = dict(stts=g(b'stts'), stsz=g(b'stsz'), stsc=g(b'stsc'), stss=g(b'stss'), ctts=g(b'ctts'),
             co64=g(b'co64'), elst=ed.get(b'elst').data if ed else None, n=t.n,
             mdhd=t.trak.get(b'mdia').get(b'mdhd').data, tkhd=t.trak.get(b'tkhd').data,
             mvhd=moov.get(b'mvhd').data, rdly=rdly(t), stsd_wo_rdly=None)
    sd = t.stbl.get(b'stsd').data; i = sd.find(b'rdly')
    d['stsd'] = sd
    d['stbl_types'] = [k.typ for k in t.stbl.kids]
    d['index'] = T.index(t)
    return buf, moov, T, t, d

def samples_hash(buf, offs, sizes):
    return [hashlib.md5(buf[o:o+s]).hexdigest() for o, s in zip(offs, sizes)]

def compare(orig, py, c, cmoov_off, cmoov_new, growth):
    ob, om, oT, ot, od = info(orig); pb, pm, pT, pt, pd = info(py); cb, cm, cT, ct, cd = info(c)
    diffs = []
    for k in ('stts', 'stsz', 'stsc', 'stss', 'ctts', 'co64', 'elst', 'n', 'mdhd', 'tkhd', 'mvhd', 'rdly', 'stsd', 'stbl_types'):
        if pd[k] != cd[k]: diffs.append(k)
    # chunk offsets: C file coords -> original coords
    split = cmoov_off + cmoov_new
    norm = lambda o: o - growth if o >= split else o
    if [norm(o) for o in ct.chunks] != pt.chunks: diffs.append('chunk_offsets')
    if [norm(o) for o in ct.offs] != pt.offs: diffs.append('sample_offsets')
    if samples_hash(cb, ct.offs, ct.sizes) != samples_hash(pb, pt.offs, pt.sizes): diffs.append('video_sample_bytes')
    if samples_hash(cb, ct.offs[:ot.n], ct.sizes[:ot.n]) != samples_hash(ob, ot.offs, ot.sizes): diffs.append('video_vs_orig_bytes')
    # every non-video track: sample bytes equal to original
    if od['index'] != cd['index'] or pd['index'] != cd['index']: diffs.append('track_choice')
    for i, (a, b) in enumerate(zip(oT, cT)):
        if i == od['index']: continue
        if samples_hash(ob, a.offs, a.sizes) != samples_hash(cb, b.offs, b.sizes): diffs.append('track_%s_bytes' % a.handler.decode())
    # start delay: movie time of each track's first presented picture/sample
    def first(t, rewritten):
        if rewritten and t.handler == b'vide':
            N = rdly(t) or 0; p0 = t.dts[N]          # sample N shows display picture 0
        else:
            p0 = min(d + c for d, c in zip(t.dts, t.ctts))
        return t.elst_lead / t.movie_ts + (max(p0, t.elst_mt) - t.elst_mt) / t.ts
    def av(T, rewritten):
        v = {id(oT): ot, id(pT): pt, id(cT): ct}[id(T)]; a = [x for x in T if x.handler == b'soun']
        return first(v, rewritten) - first(a[0], rewritten) if a else None
    o_av, p_av, c_av = av(oT, False), av(pT, True), av(cT, True)
    tick = 1.0 / ot.movie_ts + 1e-9
    if o_av is not None:
        if abs(c_av - o_av) > tick: diffs.append('av_start_offset_C')
        if abs(p_av - o_av) > tick: diffs.append('av_start_offset_py')
    other = len(oT) - 1
    return diffs, dict(N=cd['rdly'], n=ct.n, av_orig_ms=None if o_av is None else round(o_av*1000, 3),
                       av_c_ms=None if c_av is None else round(c_av*1000, 3), chunks=len(ct.chunks), other_tracks=other,
                       other_samples=sum(a.n for i, a in enumerate(oT) if i != od['index']))

if __name__ == '__main__':
    # args: orig py c json-of-tool
    tj = json.loads(sys.argv[4])
    diffs, st = compare(sys.argv[1], sys.argv[2], sys.argv[3], tj['moov_off'], tj['new_moov'], tj['growth'])
    print(json.dumps(dict(file=sys.argv[1].split('/')[-1], ok=not diffs, diffs=diffs, **st)))
