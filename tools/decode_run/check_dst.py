"""
check_dst.py DST.2vuy FFMPEG.yuv MODE -- for every BandDecompress call,
check the 2vuy destination the codec left behind against the frame that
call SHOULD show, built from ffmpeg's decode with the codec's own 4:2:0 ->
4:2:2 rule (nearest chroma row, Cb Y0 Cr Y1). Reports bit-exact calls,
unpainted calls (sentinel 0xAB survives), and the first mismatch.
"""
import sys, numpy as np
CW, H = 360, 240
cfs = CW * H * 3 // 2
dst = np.fromfile(sys.argv[1], dtype=np.uint8).reshape(-1, H, CW * 2)
ref = np.fromfile(sys.argv[2], dtype=np.uint8)
mode = sys.argv[3]
def expected(k):
    b = ref[k*cfs:(k+1)*cfs]
    y = b[:CW*H].reshape(H, CW)
    u = b[CW*H:CW*H+CW*H//4].reshape(H//2, CW//2)
    v = b[CW*H+CW*H//4:].reshape(H//2, CW//2)
    out = np.empty((H, CW*2), np.uint8)
    out[:, 0::4] = np.repeat(u, 2, axis=0)
    out[:, 1::4] = y[:, 0::2]
    out[:, 2::4] = np.repeat(v, 2, axis=0)
    out[:, 3::4] = y[:, 1::2]
    return out
exact = unpainted = 0; first_bad = None
for i in range(len(dst)):
    k = 0 if mode == 'poster' else (max(i - 1, 0) if mode == 'twice' else i)
    d = dst[i]
    if np.all(d == 0xAB): unpainted += 1; first_bad = first_bad or (i, 'unpainted'); continue
    if np.array_equal(d, expected(k)): exact += 1
    elif first_bad is None: first_bad = (i, 'differs from frame %d' % k)
print("%s: calls %d, painted bit-exact %d, unpainted %d%s" % (mode, len(dst), exact, unpainted,
      "" if first_bad is None else ", first problem at call %d (%s)" % first_bad))
