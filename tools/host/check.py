"""
check.py -- compare what the codec produced on the host with ffmpeg.

  check.py pics OUT.yuv FFMPEG.yuv CODED_W CODED_H DISP_W DISP_H [INDEX]
      Every captured picture against ffmpeg's decode of the same frame,
      luma and chroma, after cropping to the display size. Without INDEX
      picture k is frame k. With INDEX (decode_run --index) each picture
      is compared with the frame of the sample it came from, and pictures
      decoded while deblocking was off (or after one, before the next key
      frame) are allowed to differ and reported as PSNR instead.

  check.py dst DST FFMPEG.yuv FMT DISP_W DISP_H CLIP_W CLIP_H MODE
      The destination after every call against the frame that call should
      show, converted from ffmpeg's decode with an INDEPENDENT numpy
      implementation of each format:
        2vuy   Cb Y0 Cr Y1, chroma rows repeated
        rgb32  0 R G B, BT.601 limited range, the integer formula in
               H264Codec.c's header comment
        rgb16  big-endian x555, top five bits of each rgb32 component
        y420   the three planes as they are
      MODE is seq, poster or twice (which frame each call shows).
"""
import sys, numpy as np

def frame(ref, k, W, H):
    fs = W * H * 3 // 2
    b = ref[k*fs:(k+1)*fs]
    return (b[:W*H].reshape(H, W), b[W*H:W*H+W*H//4].reshape(H//2, W//2),
            b[W*H+W*H//4:].reshape(H//2, W//2))

def rgb(y, u, v):
    H, W = y.shape
    U = np.repeat(np.repeat(u, 2, 0), 2, 1)[:H, :W].astype(np.int64) - 128
    V = np.repeat(np.repeat(v, 2, 0), 2, 1)[:H, :W].astype(np.int64) - 128
    C = 298 * (y.astype(np.int64) - 16) + 128
    r = np.clip((C + 409 * V) >> 8, 0, 255)
    g = np.clip((C - 100 * U - 208 * V) >> 8, 0, 255)
    b = np.clip((C + 516 * U) >> 8, 0, 255)
    return r, g, b

def expected(fmt, y, u, v, cw, ch):
    if fmt == '2vuy':
        W = y.shape[1]
        out = np.empty((y.shape[0], W * 2), np.uint8)
        out[:, 0::4] = np.repeat(u, 2, 0); out[:, 1::4] = y[:, 0::2]
        out[:, 2::4] = np.repeat(v, 2, 0); out[:, 3::4] = y[:, 1::2]
        out = out[:ch, :cw * 2].copy()
        if cw & 1:       # odd width: the last luma sample is doubled
            out[:, -1] = out[:, -3]
            out = np.concatenate([out, np.zeros((ch, 0), np.uint8)], 1)
        return out
    if fmt == 'rgb32':
        r, g, b = rgb(y, u, v)
        out = np.zeros((y.shape[0], y.shape[1], 4), np.uint8)
        out[..., 1] = r; out[..., 2] = g; out[..., 3] = b
        return out[:ch, :cw].reshape(ch, cw * 4)
    if fmt == 'rgb16':
        r, g, b = rgb(y, u, v)
        px = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
        out = np.zeros((y.shape[0], y.shape[1], 2), np.uint8)
        out[..., 0] = px >> 8; out[..., 1] = px & 255
        return out[:ch, :cw].reshape(ch, cw * 2)
    if fmt == 'y420':
        return np.concatenate([y[:ch, :cw].ravel(), u[:ch//2, :cw//2].ravel(), v[:ch//2, :cw//2].ravel()])
    raise SystemExit('unknown format ' + fmt)

def main():
    what = sys.argv[1]
    if what == 'pics':
        ours = np.fromfile(sys.argv[2], np.uint8); ref = np.fromfile(sys.argv[3], np.uint8)
        W, H, DW, DH = map(int, sys.argv[4:8])
        fs = W * H * 3 // 2
        idx = None
        if len(sys.argv) > 8:
            idx = [tuple(map(int, l.split())) for l in open(sys.argv[8])]
        n = len(ours) // fs
        nref = len(ref) // (DW * DH * 3 // 2)
        exact = 0; bad_clean = []; dirty = []; 
        for k in range(n):
            b = ours[k*fs:(k+1)*fs]
            y = b[:W*H].reshape(H, W)[:DH, :DW]
            u = b[W*H:W*H+W*H//4].reshape(H//2, W//2)[:DH//2, :DW//2]
            v = b[W*H+W*H//4:].reshape(H//2, W//2)[:DH//2, :DW//2]
            s = idx[k][1] if idx else k
            if s >= nref: break
            ry, ru, rv = frame(ref, s, DW, DH)
            same = np.array_equal(y, ry) and np.array_equal(u, ru) and np.array_equal(v, rv)
            if same: exact += 1
            else:
                mse = ((y.astype(float) - ry) ** 2).mean()
                psnr = 10 * np.log10(255 * 255 / mse) if mse > 0 else 99.0
                (dirty if (idx and idx[k][3]) else bad_clean).append((k, s, psnr))
        msg = "pictures %d, bit-exact %d" % (n, exact)
        if dirty:
            msg += ", %d expected-inexact (deblocking skipped since the last key frame), luma PSNR min %.1f dB mean %.1f dB" % (
                len(dirty), min(d[2] for d in dirty), sum(d[2] for d in dirty) / len(dirty))
        if bad_clean:
            msg += ", UNEXPECTED MISMATCHES %d, first at picture %d (sample %d, %.1f dB)" % (
                len(bad_clean), bad_clean[0][0], bad_clean[0][1], bad_clean[0][2])
        print(msg)
        sys.exit(1 if bad_clean else 0)
    else:
        fmt = sys.argv[4]
        DW, DH, CW, CH = map(int, sys.argv[5:9]); mode = sys.argv[9]
        ref = np.fromfile(sys.argv[3], np.uint8)
        per = {'2vuy': (CW + (CW & 1)) * 2 * CH if False else CW * 2 * CH, 'rgb32': CW * 4 * CH, 'rgb16': CW * 2 * CH,
               'y420': CW * CH + 2 * (CW // 2) * (CH // 2)}[fmt]
        dst = np.fromfile(sys.argv[2], np.uint8).reshape(-1, per)
        exact = unpainted = 0; first = None
        # v17: MODE map:FILE = one frame index per call, -1 = black (Y 16, Cb/Cr 128)
        cmap = [int(l.split()[0]) for l in open(mode[4:])] if mode.startswith('map:') else None
        fs = DW * DH * 3 // 2
        if cmap is not None:
            blk = np.concatenate([np.full(DW * DH, 16, np.uint8), np.full(fs - DW * DH, 128, np.uint8)])
            ref = np.concatenate([ref[:(len(ref) // fs) * fs], blk])
            nblack = len(ref) // fs - 1
        for i in range(len(dst)):
            k = 0 if mode == 'poster' else (max(i - 1, 0) if mode == 'twice' else i)
            if cmap is not None: k = cmap[i] if cmap[i] >= 0 else nblack
            d = dst[i]
            if np.all(d == 0xAB):
                unpainted += 1; first = first or (i, 'unpainted'); continue
            e = expected(fmt, *frame(ref, k, DW, DH), CW, CH).ravel()
            if fmt == '2vuy' and (CW & 1):
                # the writer stores 4 bytes for the odd last column; the dump holds CW*2 of them
                e = expected(fmt, *frame(ref, k, DW, DH), CW + 1, CH)
                e[:, -1] = e[:, -3]
                e = e[:, :CW * 2].ravel()
            if np.array_equal(d, e): exact += 1
            elif first is None:
                first = (i, 'differs from frame %d in %d bytes' % (k, int((d != e).sum())))
        print("%s %s %dx%d: calls %d, painted bit-exact %d, unpainted %d%s" % (
            fmt, mode, CW, CH, len(dst), exact, unpainted,
            "" if first is None else ", FIRST PROBLEM at call %d (%s)" % first))
        sys.exit(0 if exact == len(dst) else 1)
main()
