"""
compare_ff.py OURS.yuv FFMPEG.yuv [N] -- compare the harness's captured
pictures (368x240 coded size, planar 4:2:0) with ffmpeg's decode (360x240
display size) frame by frame, luma AND chroma, after cropping ours to the
display width. Prints how many frames are bit-exact and the first mismatch.
"""
import sys, numpy as np
W, H, CW = 368, 240, 360
fs, cfs = W * H * 3 // 2, CW * H * 3 // 2
ours = np.fromfile(sys.argv[1], dtype=np.uint8)
ref = np.fromfile(sys.argv[2], dtype=np.uint8)
n = min(len(ours) // fs, len(ref) // cfs, int(sys.argv[3]) if len(sys.argv) > 3 else 10**9)
def crop(b):
    y = b[:W*H].reshape(H, W)[:, :CW]
    u = b[W*H:W*H+W*H//4].reshape(H//2, W//2)[:, :CW//2]
    v = b[W*H+W*H//4:fs].reshape(H//2, W//2)[:, :CW//2]
    return np.concatenate([y.ravel(), u.ravel(), v.ravel()])
exact, first_bad = 0, None
for i in range(n):
    a = crop(ours[i*fs:(i+1)*fs]); b = ref[i*cfs:(i+1)*cfs]
    if np.array_equal(a, b): exact += 1
    elif first_bad is None:
        m = ((a.astype(float) - b.astype(float))**2).mean()
        first_bad = (i, 10*np.log10(255*255/m))
print("frames compared %d, bit-exact %d%s" % (n, exact,
      "" if first_bad is None else ", first mismatch at %d (%.1f dB)" % first_bad))
