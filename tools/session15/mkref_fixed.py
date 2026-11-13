#!/usr/bin/env python3
"""mkref.py IN.mp4 URL OUT.mov -- write a moov-only QuickTime reference movie whose
media data lives at URL (the original, untouched .mp4). Audio entry QT-ified via qtify."""
import sys, struct
sys.path.insert(0, sys.argv[4] if len(sys.argv) > 4 else '.')
import qtify as Q
CONT = {b'moov', b'trak', b'mdia', b'minf', b'dinf', b'stbl', b'edts', b'udta'}
def boxes(d, s, e):
    while s + 8 <= e:
        n, t = struct.unpack('>I4s', d[s:s+8]); h = 8
        if n == 1: n = struct.unpack('>Q', d[s+8:s+16])[0]; h = 16
        if n == 0: n = e - s
        yield t, s, h, n
        s += n
def rebuild(d, s, e, url, delta):
    out = b''
    for t, o, h, n in boxes(d, s, e):
        body = d[o+h:o+n]
        if t in CONT:
            body = rebuild(d, o+h, o+n, url, delta)
        elif t == b'dref':
            ent = struct.pack('>I4sI', 12 + len(url) + 1, b'url ', 0) + url + b'\0'
            body = struct.pack('>II', 0, 1) + ent
        elif t == b'stco':
            c = struct.unpack('>I', body[4:8])[0]
            v = struct.unpack('>%dI' % c, body[8:8+4*c])
            body = body[:8] + struct.pack('>%dI' % c, *[x - delta for x in v])
        elif t == b'co64':
            c = struct.unpack('>I', body[4:8])[0]
            v = struct.unpack('>%dQ' % c, body[8:8+8*c])
            body = body[:8] + struct.pack('>%dQ' % c, *[x - delta for x in v])
        elif t in (b'iods',):
            continue
        out += struct.pack('>I4s', 8 + len(body), t) + body
    return out
src = open(sys.argv[1], 'rb').read()
new, _d = Q.qtify(src, verbose=False)
import os
delta = 0 if os.environ.get("MDATFIRST") else len(new) - len(src)
for t, o, h, n in boxes(new, 0, len(new)):
    if t == b'moov':
        m = rebuild(new, o+h, o+n, sys.argv[2].encode(), delta)
        open(sys.argv[3], 'wb').write(struct.pack('>I4s', 8+len(m), b'moov') + m)
        print('moov', 8+len(m), 'delta', delta); break
