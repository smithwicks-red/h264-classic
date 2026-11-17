import sys, os, json
sys.path.insert(0, '/home/claude/s5/rw/sim')
import qt6, rewrite, compare, mp4
F = '/home/claude/s5/sim/files'; R = '/home/claude/s5/sim/ref'
for f in sorted(os.listdir(F)):
    times, md, end = qt6.reference(f'{F}/{f}', f'{R}/{f}.md5')
    o = mp4.load(f'{F}/{f}'); vt = rewrite.video(o[3]); pts_, disp_, pos_ = rewrite.order(vt)
    kt = []
    for k in sorted(vt.sync or []):
        for dj in range(pos_[k] - 3, pos_[k] + 2):
            if 0 <= dj < len(times) and times[dj] >= 0 and times[dj] < end - 1.5: kt.append(times[dj])
    res = {}
    for tag, path in (('py', f'/home/claude/s5/rw/pyout/A_{f}'), ('c', f'/home/claude/s5/rw/stage_rw/C_{f}')):
        N = compare.info(path)[4]['rdly']
        rec, tm, ffd, t, st, sync, buf = qt6.play(path, 'A', N, times, md, end)
        sc = qt6.score(rec, times, md, end)
        sk = qt6.seeks(path, 'A', N, times, md, end)
        kk = qt6.seeks(path, 'A', N, times, md, end, targets=kt) if kt else None
        res[tag] = dict(N=N, wrong=sc['wrong'], lost_end=sc['lost_end'], off_mean=round(sc['off_mean'], 3), off_max=round(sc['off_max'], 3),
                        seek=sk, keyseek=kk)
    print(json.dumps(dict(file=f, same=res['py'] == res['c'], **res)), flush=True)
