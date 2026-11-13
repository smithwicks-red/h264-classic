"""
drop_sim.py -- what does the picture look like when QuickTime drops
frames?

The Pismo's Millions run (2026-09-24) showed QuickTime calling
BandDecompress only 895 times in a 41.5-second playback of a 1229-frame
clip whose key frames are 186-250 frames apart. So QuickTime skipped
individual P-frames rather than skipping ahead to key frames, and
h264bsd -- which ignores frame_num gaps when the SPS says gaps are not
allowed -- decoded each following P-frame against a stale reference.

This script measures the damage with the codec's real decode path:

  1. build a QuickTime-like schedule: frame n is due at n / fps; the
     decoder is busy COST ms per picture; each time it is free it
     decodes whichever frame is due now (the frames in between are
     never delivered), or waits if it is ahead
  2. run decode_run in list: mode on that schedule
  3. compare every picture with ffmpeg's decode of the SAME frame, in
     PSNR (luma), and report how bad it gets and for how long

    usage: drop_sim.py WORKDIR COST_MS [naive|sync] [FPS]

WORKDIR must hold movie.samples, ffmpeg.yuv and decode_run_new, as
run_ab.sh leaves them. 'sync' never lets a key frame be skipped: if one
falls in the skipped range the decoder jumps to it instead.
"""
import os, subprocess, sys
import numpy as np

W, H, CW = 368, 240, 360
fs, cfs = W * H * 3 // 2, CW * H * 3 // 2


def keyframes(samples_path):
    raw = open(samples_path, 'rb').read()
    n_avcc = int.from_bytes(raw[0:4], 'big')
    pos = 4 + n_avcc
    count = int.from_bytes(raw[pos:pos + 4], 'big'); pos += 4
    keys = []
    for i in range(count):
        size = int.from_bytes(raw[pos:pos + 4], 'big'); pos += 4
        # first NAL of the sample: 4-byte length then header; type 5 = IDR
        k, end = pos, pos + size
        while k + 4 < end:
            nal_len = int.from_bytes(raw[k:k + 4], 'big')
            if raw[k + 4] & 0x1F == 5:
                keys.append(i)
                break
            k += 4 + nal_len
        pos += size
    return count, keys


def schedule(count, keys, cost_ms, policy, fps):
    period = 1000.0 / fps
    keyset = set(keys)
    t, last, out = 0.0, -1, []
    while last < count - 1:
        due = int(t / period)
        if due <= last:                 # ahead of schedule: wait
            due = last + 1
            t = due * period
        due = min(due, count - 1)
        if policy == 'sync':
            skipped_keys = [k for k in keys if last < k <= due]
            if skipped_keys:
                due = skipped_keys[-1]
        out.append(due)
        last = due
        t += cost_ms
    return out


def psnr(a, b):
    m = ((a.astype(np.float64) - b.astype(np.float64)) ** 2).mean()
    return 99.0 if m == 0 else 10 * np.log10(255.0 * 255.0 / m)


def main():
    work = sys.argv[1]
    cost = float(sys.argv[2])
    policy = sys.argv[3] if len(sys.argv) > 3 else 'naive'
    fps = float(sys.argv[4]) if len(sys.argv) > 4 else 29.97
    count, keys = keyframes(os.path.join(work, 'movie.samples'))
    sched = schedule(count, keys, cost, policy, fps)
    tag = '%s_%g' % (policy, cost)
    lst = os.path.join(work, 'sched_%s.txt' % tag)
    open(lst, 'w').write('\n'.join(map(str, sched)) + '\n')
    out = os.path.join(work, 'out_%s.yuv' % tag)
    subprocess.run([os.path.join(work, 'decode_run_new'),
                    os.path.join(work, 'movie.samples'), out, 'list:' + lst],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    ours = np.fromfile(out, dtype=np.uint8)
    ref = np.fromfile(os.path.join(work, 'ffmpeg.yuv'), dtype=np.uint8)
    n = len(ours) // fs
    vals = []
    for i in range(n):
        k = sched[i]
        y = ours[i * fs:i * fs + W * H].reshape(H, W)[:, :CW]
        r = ref[k * cfs:k * cfs + CW * H].reshape(H, CW)
        vals.append(psnr(y, r))
    vals = np.array(vals)
    exact = int((vals >= 99).sum())
    dropped = count - len(set(sched))
    print('%-5s cost %5.1f ms: decoded %4d of %d (%.1f fps), dropped %3d | '
          'bit-exact %4d, PSNR mean %.1f dB, worst %.1f dB, <35 dB %4.1f%%, <30 dB %4.1f%%'
          % (policy, cost, n, count, n / (count / fps), dropped, exact,
             vals[vals < 99].mean() if (vals < 99).any() else 99.0,
             vals.min(), 100.0 * (vals < 35).mean(), 100.0 * (vals < 30).mean()))
    # worst stretch: the GOP whose last pictures are furthest off
    for g, k0 in enumerate(keys):
        k1 = keys[g + 1] if g + 1 < len(keys) else count
        sel = [v for s, v in zip(sched, vals) if k0 <= s < k1]
        if sel:
            print('   GOP %d (frames %4d-%4d): pictures %3d, PSNR first %5.1f  median %5.1f  last %5.1f  min %5.1f'
                  % (g, k0, k1 - 1, len(sel), sel[0], float(np.median(sel)), sel[-1], min(sel)))


if __name__ == '__main__':
    main()
