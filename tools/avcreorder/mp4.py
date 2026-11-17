# Minimal MP4/MOV box parser + writer for sample tables (sim only).
import struct
CONT = {b'moov', b'trak', b'mdia', b'minf', b'stbl', b'edts', b'dinf'}

class Box:
    def __init__(s, typ, data=None, kids=None): s.typ, s.data, s.kids = typ, data, kids
    def find(s, t): return [k for k in (s.kids or []) if k.typ == t]
    def get(s, t): r = s.find(t); return r[0] if r else None
    def ser(s):
        body = b''.join(k.ser() for k in s.kids) if s.kids is not None else s.data
        return struct.pack('>I4s', 8 + len(body), s.typ) + body

def parse(buf, a=0, b=None, top=False):
    b = len(buf) if b is None else b; out = []
    while a + 8 <= b:
        sz, typ = struct.unpack('>I4s', buf[a:a+8]); h = 8
        if sz == 1: sz = struct.unpack('>Q', buf[a+8:a+16])[0]; h = 16
        elif sz == 0: sz = b - a
        if typ in CONT: bx = Box(typ, kids=parse(buf, a+h, a+sz))
        elif top and typ != b'moov': bx = Box(typ, data=None)
        else: bx = Box(typ, data=bytes(buf[a+h:a+sz]))
        bx.off, bx.size = a, sz; out.append(bx); a += sz
    return out

def full(d): return d[0], d[4:]
def u32s(d, n, at=0): return list(struct.unpack('>%dI' % n, d[at:at+4*n]))

class Track: pass

def tracks(moov):
    mvhd = moov.get(b'mvhd').data
    mts = struct.unpack('>I', mvhd[12:16] if mvhd[0] == 0 else mvhd[20:24])[0]
    res = []
    for trak in moov.find(b'trak'):
        t = Track(); t.trak = trak; t.movie_ts = mts
        mdia = trak.get(b'mdia'); md = mdia.get(b'mdhd').data
        t.ts = struct.unpack('>I', md[12:16] if md[0] == 0 else md[20:24])[0]
        t.handler = mdia.get(b'hdlr').data[8:12]
        st = mdia.get(b'minf').get(b'stbl'); t.stbl = st
        d = st.get(b'stts').data; n = struct.unpack('>I', d[4:8])[0]; v = u32s(d, 2*n, 8)
        durs = []
        for i in range(n): durs += [v[2*i+1]] * v[2*i]
        t.durs = durs; t.dts = [0]*len(durs); acc = 0
        for i, x in enumerate(durs): t.dts[i] = acc; acc += x
        t.n = len(durs)
        c = st.get(b'ctts'); t.ctts = [0]*t.n
        if c:
            ver = c.data[0]; n = struct.unpack('>I', c.data[4:8])[0]; i = 0
            for e in range(n):
                cnt, off = struct.unpack('>I' + ('i' if ver else 'I'), c.data[8+8*e:16+8*e])
                for _ in range(cnt): t.ctts[i] = off; i += 1
        s = st.get(b'stss')
        t.sync = None if s is None else set(x-1 for x in u32s(s.data, struct.unpack('>I', s.data[4:8])[0], 8))
        z = st.get(b'stsz').data; fs, n = struct.unpack('>II', z[4:12])
        t.sizes = [fs]*n if fs else u32s(z, n, 12)
        co = st.get(b'stco'); big = False
        if co is None: co = st.get(b'co64'); big = True
        n = struct.unpack('>I', co.data[4:8])[0]
        t.chunks = list(struct.unpack('>%d%s' % (n, 'Q' if big else 'I'), co.data[8:8+(8 if big else 4)*n]))
        sc = st.get(b'stsc').data; n = struct.unpack('>I', sc[4:8])[0]; e = u32s(sc, 3*n, 8)
        t.offs = []; t.chunk_of = []; si = 0
        for ci in range(len(t.chunks)):
            spc = None
            for k in range(n):
                if e[3*k] - 1 <= ci: spc = e[3*k+1]
            o = t.chunks[ci]
            for _ in range(spc):
                if si >= t.n: break
                t.offs.append(o); t.chunk_of.append(ci); o += t.sizes[si]; si += 1
        t.elst_mt = 0; t.elst_lead = 0; t.elst_nmedia = 0; t.elst_dur = 0; ed = trak.get(b'edts')
        if ed:
            el = ed.get(b'elst').data; ver = el[0]; n = struct.unpack('>I', el[4:8])[0]; p = 8
            for k in range(n):
                if ver: dur, mt = struct.unpack('>Qq', el[p:p+16]); p += 20
                else: dur, mt = struct.unpack('>Ii', el[p:p+8]); p += 12
                if mt >= 0:
                    if t.elst_nmedia == 0: t.elst_mt = mt; t.elst_dur = dur   # 0 = untrimmed
                    t.elst_nmedia += 1
                elif t.elst_nmedia == 0: t.elst_lead += dur   # leading empty edit(s), movie timescale
        sd = st.get(b'stsd').data; t.avcC = None
        t.entry = sd[12:16] if len(sd) >= 16 else None
        i = sd.find(b'avcC')
        if i > 0:
            L = struct.unpack('>I', sd[i-4:i])[0]; t.avcC = sd[i+4:i-4+L]
        res.append(t)
    return res

def load(path):
    buf = open(path, 'rb').read()
    top = parse(buf, top=True)
    moov = [b for b in top if b.typ == b'moov'][0]
    return buf, top, moov, tracks(moov)

# ---- table builders ----
def mk_full(typ, payload, ver=0): return Box(typ, data=bytes([ver, 0, 0, 0]) + payload)
def b_stts(durs):
    ent = []
    for d in durs:
        if ent and ent[-1][1] == d: ent[-1][0] += 1
        else: ent.append([1, d])
    return mk_full(b'stts', struct.pack('>I', len(ent)) + b''.join(struct.pack('>II', c, d) for c, d in ent))
def b_stsz(sizes): return mk_full(b'stsz', struct.pack('>II', 0, len(sizes)) + struct.pack('>%dI' % len(sizes), *sizes))
def b_stss(idx): return mk_full(b'stss', struct.pack('>I', len(idx)) + b''.join(struct.pack('>I', i+1) for i in idx))
def b_chunks(offs, sizes):
    chunks = []; spc = []
    for o, s in zip(offs, sizes):
        if chunks and offs_end[0] == o: spc[-1] += 1
        else: chunks.append(o); spc.append(1)
        offs_end = [o + s] if True else None
        offs_end = [o + s]
    ent = []
    for i, c in enumerate(spc):
        if not ent or ent[-1][1] != c: ent.append([i+1, c, 1])
    stsc = mk_full(b'stsc', struct.pack('>I', len(ent)) + b''.join(struct.pack('>III', *e) for e in ent))
    stco = mk_full(b'stco', struct.pack('>I', len(chunks)) + struct.pack('>%dI' % len(chunks), *chunks))
    return stsc, stco, len(chunks)
