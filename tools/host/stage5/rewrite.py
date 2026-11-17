# Moov rewriters for design A and B (sim). Old moov -> 'free' (offsets kept), new moov appended.
import struct, sys, json
import mp4

def video(T): return [t for t in T if t.handler == b'vide'][0]

def order(t):
    pts = [d + c for d, c in zip(t.dts, t.ctts)]
    disp = sorted(range(t.n), key=lambda i: (pts[i], i))     # display idx -> decode idx
    pos = [0]*t.n
    for p, d in enumerate(disp): pos[d] = p
    return pts, disp, pos

def elst_box(segs):  # segs: list of (movie_dur, media_time)
    p = struct.pack('>I', len(segs)) + b''.join(struct.pack('>IiI', d, m, 0x10000) for d, m in segs)
    return mp4.Box(b'edts', kids=[mp4.mk_full(b'elst', p)])

def replace_stbl(t, new):  # new: dict typ->Box or None (drop)
    kids = []
    for k in t.stbl.kids:
        if k.typ in (b'ctts', b'sdtp', b'cslg') : continue
        if k.typ == b'co64': k = new[b'stco']
        if k.typ in new:
            if new[k.typ] is not None: kids.append(new[k.typ])
        else: kids.append(k)
    if b'stss' in new and new[b'stss'] is not None and not t.stbl.get(b'stss'): kids.append(new[b'stss'])
    t.stbl.kids = kids

def set_durations(moov, t, media_dur, segs):
    md = t.trak.get(b'mdia').get(b'mdhd'); d = bytearray(md.data)
    assert d[0] == 0; d[16:20] = struct.pack('>I', media_dur); md.data = bytes(d)
    movie_dur = sum(s[0] for s in segs)
    tk = t.trak.get(b'tkhd'); d = bytearray(tk.data)
    if d[0] == 0: d[20:24] = struct.pack('>I', movie_dur); tk.data = bytes(d)
    kids = [k for k in t.trak.kids if k.typ != b'edts']
    i = [j for j, k in enumerate(kids) if k.typ == b'tkhd'][0]
    kids.insert(i+1, elst_box(segs)); t.trak.kids = kids
    mv = moov.get(b'mvhd'); d = bytearray(mv.data)
    if d[0] == 0:
        cur = struct.unpack('>I', d[16:20])[0]; d[16:20] = struct.pack('>I', max(cur, movie_dur)); mv.data = bytes(d)

def write(src_buf, top, moov, out):
    nm = moov.ser()
    with open(out, 'wb') as f:
        f.write(src_buf)
        f.seek(moov.off); f.write(struct.pack('>I4s', moov.size, b'free'))   # old moov becomes free
        f.seek(0, 2); f.write(nm)
    return len(nm)

def segs_for(t, media_start, media_dur):  # map media_start..end to movie time 0 (empty edit if negative)
    sc = t.movie_ts / t.ts; segs = []
    if media_start < 0: segs.append((round(-media_start*sc), -1)); media_start = 0
    segs.append((round((media_dur - media_start)*sc), media_start)); return segs

def table_bytes(t):
    return sum(len(t.stbl.get(x).ser()) for x in (b'stts', b'ctts', b'stss', b'stsz', b'stsc', b'stco', b'co64') if t.stbl.get(x))

def rewrite_A(src, out):
    buf, top, moov, T = mp4.load(src); t = video(T); pts, disp, pos = order(t)
    N = max(d - pos[d] for d in range(t.n)); N = max(N, 0)
    orig = table_bytes(t)
    sp = sorted(pts); dd = [sp[i+1] - sp[i] for i in range(t.n-1)] + [t.durs[-1]]
    durs = t.durs[:N] + dd            # retimed: sample n>=N lasts as long as display picture n-N
    last = t.offs[-1] + t.sizes[-1] - 1
    offs = t.offs + [last]*N; sizes = t.sizes + [1]*N
    stsc, stco, nch = mp4.b_chunks(offs, sizes)
    new = {b'stts': mp4.b_stts(durs), b'stsz': mp4.b_stsz(sizes), b'stsc': stsc, b'stco': stco}
    replace_stbl(t, new)
    media_dur = sum(durs); start = t.dts[N] - min(pts) + t.elst_mt
    set_durations(moov, t, media_dur, segs_for(t, start, media_dur))
    write(buf, top, moov, out)
    return dict(N=N, tail=N, orig_tab=orig, new_tab=table_bytes(t), chunks=nch)

def rewrite_B(src, out):
    buf, top, moov, T = mp4.load(src); t = video(T); pts, disp, pos = order(t)
    sync = t.sync if t.sync is not None else set(range(t.n))
    orig = table_bytes(t)
    n = t.n
    contig = [t.offs[f] + t.sizes[f] == t.offs[f+1] for f in range(n-1)] + [False]
    def block_end(f):
        while f < n-1 and contig[f]: f += 1
        return f
    need = []; m = -1
    for i in range(n): m = max(m, disp[i]); need.append(m)
    cur = 0; offs = []; sizes = []; runs = []; st = dict(straddle=0, straddle_fixed=0, late=0, one_byte=0, sync_lost=0, extended=0)
    lastbyte = None; is_sync = []
    for i in range(n):
        a = cur; b = cur - 1
        E = block_end(cur) if cur < n else n - 1
        if need[i] >= cur:
            if need[i] > E:                      # run crosses a gap: cannot be one sample
                st['straddle'] += 1; st['late'] += 1; b = E
            else:
                b = need[i]
        # 1-step lookahead: if the next sample would straddle, finish this block now (stop before a sync frame)
        if i + 1 < n and cur < n and need[i+1] > E and b < E:
            g = b
            while g < E and ((g+1) not in sync or g+1 == a): g += 1
            if g > b:
                st['extended'] += 1; b = g
                if need[i+1] <= block_end(g+1) if g+1 < n else True: st['straddle_fixed'] += 1
        for f in range(a+1, b+1):
            if f in sync: st['sync_lost'] += 1
        if b >= a:
            offs.append(t.offs[a]); sizes.append(t.offs[b] + t.sizes[b] - t.offs[a])
            lastbyte = t.offs[b] + t.sizes[b] - 1; cur = b + 1
            is_sync.append(a in sync); runs.append(b - a + 1)
        else:
            offs.append(lastbyte); sizes.append(1); st['one_byte'] += 1; is_sync.append(False); runs.append(0)
    leftover = n - cur
    sp = sorted(pts); durs = [sp[i+1] - sp[i] for i in range(n-1)] + [t.durs[-1]]
    stsc, stco, nch = mp4.b_chunks(offs, sizes)
    new = {b'stts': mp4.b_stts(durs), b'stsz': mp4.b_stsz(sizes), b'stsc': stsc, b'stco': stco,
           b'stss': mp4.b_stss([i for i in range(n) if is_sync[i]])}
    replace_stbl(t, new)
    media_dur = sum(durs); start = t.elst_mt - sp[0]
    set_durations(moov, t, media_dur, segs_for(t, start, media_dur))
    write(buf, top, moov, out)
    st.update(orig_tab=orig, new_tab=table_bytes(t), chunks=nch, leftover=leftover,
              runs_hist={k: runs.count(k) for k in sorted(set(runs))}, nsync=sum(is_sync))
    return st

if __name__ == '__main__':
    d = rewrite_A(sys.argv[1], sys.argv[2]) if sys.argv[3] == 'A' else rewrite_B(sys.argv[1], sys.argv[2])
    print(json.dumps(d))
