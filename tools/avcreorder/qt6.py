# "QuickTime 6" model + codec models (A-plain, A, B) over the real AVCDecLib (x86 build).
import ctypes as C, hashlib, time, random, sys, json, bisect
import mp4, rewrite
L = C.CDLL('/home/claude/s5/sim/avc/libsim.so'); L.g_init()
class Pic(C.Structure):
    _fields_ = [('plane', C.c_void_p*3), ('stride', C.c_long*3), ('width', C.c_long), ('height', C.c_long), ('key', C.c_long), ('count', C.c_long)]
L.avcdec_open.restype = C.c_void_p; L.avcdec_open.argtypes = [C.c_char_p, C.c_long]
for f in ('decode_sample', 'flush', 'take_lowest', 'decode_run', 'set_delay', 'get_delay', 'held', 'reset', 'close'):
    getattr(L, 'avcdec_' + f).argtypes = None
VP = C.c_void_p

def md5(p):
    h = hashlib.md5(); w, ht = p.width, p.height
    for k, (pw, ph) in enumerate(((w, ht), (w//2, ht//2), (w//2, ht//2))):
        base = p.plane[k]; s = p.stride[k]
        for y in range(ph): h.update(C.string_at(base + y*s, pw))
    return h.hexdigest()

class Codec:
    """mode: 'Aplain' (FFmpeg's own delay), 'A' (delay N forced, size==1 -> flush one), 'B' (delay 16, decode run, show lowest)"""
    def __init__(s, mode, avcC, N=0):
        s.mode, s.N = mode, N
        s.ctx = VP(L.avcdec_open(avcC, len(avcC))); s.p = Pic(); s.setup()
        s.ff_delay = L.avcdec_get_delay(s.ctx)
    def setup(s):
        if s.mode == 'A': L.avcdec_set_delay(s.ctx, C.c_long(s.N))
        if s.mode == 'B': L.avcdec_set_delay(s.ctx, C.c_long(16))
    def reset(s): L.avcdec_reset(s.ctx); s.setup()
    def sample(s, data):
        """returns (md5 or None, frames decoded)"""
        n = len(data); buf = C.create_string_buffer(data, n)
        if s.mode == 'B':
            nf = 0
            if n > 1:
                outs = C.c_long(0); nf = L.avcdec_decode_run(s.ctx, buf, C.c_long(n), C.byref(outs)); assert outs.value == 0
            poc = C.c_long(0)
            return (md5(s.p) if L.avcdec_take_lowest(s.ctx, C.byref(s.p), C.byref(poc)) == 1 else None), nf
        if s.mode == 'A' and n == 1:
            return (md5(s.p) if L.avcdec_flush(s.ctx, C.byref(s.p)) == 1 else None), 0
        r = L.avcdec_decode_sample(s.ctx, buf, C.c_long(n), C.byref(s.p))
        return (md5(s.p) if r == 1 else None), 1
    def close(s): L.avcdec_close(s.ctx)

def reference(orig, refmd5):
    buf, top, moov, T = mp4.load(orig); t = rewrite.video(T); pts, disp, pos = rewrite.order(t)
    md = [l.rsplit(',', 1)[1].strip() for l in open(refmd5) if not l.startswith('#')]
    assert len(md) == t.n, (len(md), t.n)
    times = [(pts[disp[j]] - t.elst_mt) / t.ts for j in range(t.n)]   # intended movie time of display frame j (QT7 semantics)
    end = times[-1] + t.durs[-1] / t.ts
    return times, md, end

def table(path):
    buf, top, moov, T = mp4.load(path); t = rewrite.video(T)
    # movie time of each sample from (single non-empty) edit: empty edit handled via elst re-read
    ed = t.trak.get(b'edts'); shift = 0.0; mt = t.elst_mt
    if ed:
        import struct
        el = ed.get(b'elst').data; n = struct.unpack('>I', el[4:8])[0]; p = 8
        for k in range(n):
            d, m = struct.unpack('>Ii', el[p:p+8]); p += 12
            if m == -1: shift += d / t.movie_ts
            else: mt = m; break
    st = [(d - mt) / t.ts + shift for d in t.dts]
    sync = t.sync if t.sync is not None else set(range(t.n))
    return buf, t, st, sorted(sync)

def expected(times, md, tt):
    j = bisect.bisect_right(times, tt + 1e-3) - 1   # 1 ms: edit lists only have movie-timescale precision
    return j  # -1 = nothing yet

def play(path, mode, N, times, md, end, timing=False):
    buf, t, st, sync = table(path)
    c = Codec(mode, t.avcC, N); shown = None; rec = []; tm = []
    for n in range(t.n):
        data = buf[t.offs[n]:t.offs[n] + t.sizes[n]]
        t0 = time.perf_counter(); m, nf = c.sample(data); dt = time.perf_counter() - t0
        if m: shown = m
        tm.append((nf, dt))
        tt = st[n]
        if tt + t.durs[n] / t.ts > 1e-6 and tt < end - 1e-6:
            rec.append((n, max(tt, 0.0), shown, m is not None))
    ffd = c.ff_delay; c.close()
    return rec, tm, ffd, t, st, sync, buf

def score(rec, times, md, end):
    nvis = len(rec); wrong = []; offs = []
    idx = {}
    for j, h in enumerate(md): idx.setdefault(h, []).append(j)
    painted = set()
    for k, (n, tt, shown, new) in enumerate(rec):
        j = expected(times, md, tt)
        ok = (shown == md[j]) if j >= 0 else (shown is None)
        if not ok: wrong.append(k)
        if shown in idx:
            jj = min(idx[shown], key=lambda x: abs(x - j)); painted.add(jj)
            offs.append((tt - times[jj]) * 1000)
    seg = lambda a, b: sum(1 for k in wrong if a <= k < b)
    lost = [j for j in range(len(md)) if j not in painted]
    lost_end = sum(1 for j in lost if j >= len(md) - 5)
    return dict(vis=nvis, wrong=len(wrong), w_start=seg(0, 5), w_mid=seg(5, nvis-5), w_end=seg(nvis-5, nvis),
                lost=len(lost), lost_end=lost_end, off_mean=sum(offs)/max(1, len(offs)), off_max=max((abs(o) for o in offs), default=0))

def seeks(path, mode, N, times, md, end, ntarg=20, K=40, seed=1, targets=None):
    buf, t, st, sync = table(path)
    c = Codec(mode, t.avcC, N); rnd = random.Random(seed); res = []
    tl = targets if targets is not None else [rnd.uniform(0, max(0.1, end - 2.0)) for q in range(ntarg)]
    for tgt in tl:
        s = max([i for i in range(t.n) if st[i] <= tgt + 1e-6] or [0])
        k = max([x for x in sync if x <= s] or [0])
        c.reset(); shown = None; marks = []
        for n in range(k, min(t.n, s + K)):
            m, nf = c.sample(buf[t.offs[n]:t.offs[n] + t.sizes[n]])
            if m: shown = m
            if n >= s:
                j = expected(times, md, max(st[n], 0)); marks.append((shown == md[j]) if j >= 0 else (shown is None))
        last_bad = max([i for i, ok in enumerate(marks) if not ok], default=-1)
        bad_after = sum(1 for ok in marks[min(len(marks), 12):] if not ok)
        res.append(dict(first_ok=marks[0], settle=last_bad + 1, perm=bad_after > 0, preroll=s - k))
    c.close()
    return dict(first_ok=sum(r['first_ok'] for r in res), n=len(res), settle_max=max(r['settle'] for r in res),
                settle_mean=sum(r['settle'] for r in res)/len(res), perm=sum(r['perm'] for r in res),
                preroll_max=max(r['preroll'] for r in res))
