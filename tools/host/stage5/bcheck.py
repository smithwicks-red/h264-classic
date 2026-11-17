"""bcheck.py DATADIR WORKDIR -- stage 5 (v17) proofs on one movie prepared by bprep.py.

  --dr "CMD"        harness command (default ./dr), e.g. "qemu-ppc -cpu 750 ./dr_ppc"
  --rdly N|auto|atom  pass 'rdly' (auto = the decode/display distance of the file; atom =
                    the movie's own 'rdly' atom if any) [atom]
  --scen LIST       linear,twice,loop,seek,keyseek,posterseek  [all]
  --fmts LIST       destination formats for the linear run [y420]; y420 is checked
                    per call by md5, the others through check.py dst map:FILE
  --K N             calls checked after each seek target [20]
Model of QuickTime 6 (as sim/qt6.py): one sample per call in table order; a seek goes to
the sync sample at or before the target sample and feeds forward. Expected picture per
call, with N = the component's delay: sample s shows display picture s-N (black if < 0);
after a jump onto a key sample k nothing before display(k) is expected (the key is shown
at once and held until s-N passes it); a key sample repeated back to back shows what the
call before it showed. Prints one summary line per scenario and 'RESULT PASS|FAIL'.
"""
import sys, os, json, subprocess, hashlib, random, argparse
HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument('data'); ap.add_argument('work')
ap.add_argument('--dr', default=None); ap.add_argument('--rdly', default='atom')
ap.add_argument('--scen', default='linear,twice,loop,seek,keyseek,posterseek')
ap.add_argument('--fmts', default='y420'); ap.add_argument('--K', type=int, default=20)
ap.add_argument('--ntarg', type=int, default=20); ap.add_argument('--seed', type=int, default=1)
ap.add_argument('--extra', default='')
ap.add_argument('--timebase', default='media', help='media: syncFrameTime in the media time scale (times.txt); '
                'movie: as QuickTime 6 passes it: movie time scale 1000, edit shift applied (negative first start '
                'as hi=0/lo<0), durations rounded (33/34)')
A = ap.parse_args()
D = os.path.abspath(A.data); W = os.path.abspath(A.work); os.makedirs(W, exist_ok=True)
info = json.load(open(D + '/info.json'))
n, disp, md, sync, st = info['n'], info['disp'], info['md5'], set(info['sync']), info['st']
DW, DH = info['width'], info['height']
dr = A.dr or os.path.join(W, 'dr')
rd = None
if A.rdly == 'auto': rd = info['Nmap']
elif A.rdly == 'atom': rd = info['rdly']
elif A.rdly != 'none': rd = int(A.rdly)
TIMES = D + '/times.txt'
if A.timebase == 'movie':
    # movie time in ms, rounded per sample; duration = next start - start (the 33/34 pattern)
    med = [l.split() for l in open(D + '/times.txt')]
    ms = [int(round(t * 1000.0)) for t in st]
    last = int(round(int(med[-1][1]) * 1000.0 / int(med[-1][2])))
    TIMES = os.path.join(W, 'times_movie.txt')
    open(TIMES, 'w').write(''.join('%d %d 1000\n' % (ms[i], (ms[i + 1] - ms[i]) if i + 1 < n else last) for i in range(n)))
fs = DW * DH * 3 // 2
black = hashlib.md5(bytes([16]) * (DW * DH) + bytes([128]) * (fs - DW * DH)).hexdigest()
mdset = {}
for j, h in enumerate(md): mdset.setdefault(h, []).append(j)
FAIL = []

def run(name, calls, fmt='y420'):
    qf = os.path.join(W, name + '.qt'); open(qf, 'w').write(''.join('%d %d\n' % c for c in calls))
    dst = os.path.join(W, 'd.bin')
    cmd = dr.split() + [D + '/movie.samples', os.path.join(W, 'o.yuv'), '--size', str(DW), str(DH), '--mode', 'qt:' + qf,
                        '--times', TIMES, '--dst', fmt, '--dst-out', dst, '--log'] + A.extra.split()
    if rd is not None: cmd += ['--rdly', str(rd)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    open(os.path.join(W, name + '.' + fmt + '.err'), 'w').write(p.stderr)
    out = p.stdout
    v17 = [l for l in out.splitlines() if l.startswith('v17 ')]
    last = out.strip().splitlines()[-1] if out.strip() else ''
    ok = p.returncode == 0 and ' live after Close 0 ' in last and 'rc-errors 0 ' in last and v17
    N = int(v17[0].split()[2]) if v17 else -1
    return ok, N, v17[0] if v17 else 'no v17 line: ' + last + p.stderr[-300:], dst, last

def shown(dst, ncalls):
    res = []
    with open(dst, 'rb') as f:
        for i in range(ncalls):
            b = f.read(fs)
            if len(b) < fs: res.append('short'); continue
            if b.count(0xAB) == len(b): res.append('unpainted'); continue
            res.append(hashlib.md5(b).hexdigest())
    return res

def model(calls, N):
    E = []; prev = None; floor = -10**9; ep = None
    for (s, fl) in calls:
        key = s in sync and disp[s] >= 0
        if prev is not None and s == prev and key: e = ep
        else:
            if (prev is None or s != prev + 1) and key: floor = disp[s]   # v17b: start of instance = after a reset
            e = max(s - N, floor)
        E.append(e); prev = s; ep = e
    return E

def want(e): return black if e < 0 else (md[e] if e < len(md) else 'beyond-end')

def classify(h):
    if h == black: return 'black'
    if h in mdset: return 'pic'
    return 'unpainted' if h == 'unpainted' else 'BROKEN'

def report(name, ok, v17, txt, good):
    st_ = 'ok ' if (ok and good) else 'BAD'
    if not (ok and good): FAIL.append(name)
    print('%s %-11s %s | %s' % (st_, name, txt, v17))

scen = A.scen.split(',')
lin = [(s, 0) for s in range(n)]
if 'linear' in scen:
    for fmt in A.fmts.split(','):
        ok, N, v17, dst, last = run('linear', lin, fmt)
        E = model(lin, N)
        if fmt == 'y420':
            sh = shown(dst, len(lin))
            good_n = sum(1 for i in range(len(lin)) if sh[i] == want(E[i]))
            broken = sum(1 for h in sh if classify(h) == 'BROKEN')
            painted = set(j for h in sh if h in mdset for j in mdset[h] if True)
            exact_disp = set(E[i] for i in range(len(lin)) if E[i] >= 0 and sh[i] == want(E[i]))
            nblack = sum(1 for i in range(min(N, len(lin))) if sh[i] == want(E[0]))
            txt = 'y420 calls %d correct %d broken %d | first %d calls show picture 0: %d | pictures shown %d of %d (lost %d) N %d' % (
                len(lin), good_n, broken, N, nblack, len(exact_disp), len(md), len(md) - len(exact_disp), N)
            qend = int(v17.split('queue-at-end ')[1].split()[0]) if 'queue-at-end' in v17 else -1
            txt += ' | queue at end %d' % qend
            good = good_n == len(lin)
            if rd is not None and N != rd: good = False; txt += ' | N %d != rdly %d' % (N, rd)
            jl = [int(v17.split(w + ' ')[1].split()[0]) for w in ('resets', 'nokey-jumps') if w + ' ' in v17]
            if any(jl): good = False; txt += ' | FALSE JUMPS in linear play (resets, nokey-jumps) = %s' % jl
            if info['tails'] > 0 and (len(md) - len(exact_disp) != 0 or qend != 0): good = False; txt += ' | TAIL: pictures lost or left queued'
            report('linear', ok, v17, txt, good)
        else:
            mf = os.path.join(W, 'map.txt'); open(mf, 'w').write(''.join('%d\n' % e for e in E))
            yuv = os.path.join(W, 'ref.yuv')
            if not os.path.exists(yuv):
                subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'h264', '-i', D + '/movie.264', '-fps_mode', 'passthrough',
                                '-f', 'rawvideo', '-pix_fmt', 'yuv420p', yuv], check=True)
            p = subprocess.run(['python3', os.path.join(HERE, '..', 'check.py'), 'dst', dst, yuv, fmt, str(DW), str(DH), str(DW), str(DH), 'map:' + mf],
                               capture_output=True, text=True)
            report('linear', ok, v17, p.stdout.strip(), p.returncode == 0)

def segrun(name, calls, segs, keyreq):
    """segs: list of (first call index evaluated, end, target sample s)"""
    ok, N, v17, dst, last = run(name, calls)
    E = model(calls, N); sh = shown(dst, len(calls))
    first_ok = exact = tot = broken = perm = 0; settle_max = 0; fo_time = 0; bad_first = []
    held = 0
    for (a, b, s) in segs:
        if a >= len(calls): continue
        kk = calls[a][0]
        k = max([x for x in sync if x <= kk] or [0]) if name != 'loop' else 0
        dk = disp[k]
        ff = sh[a] == want(E[a]); first_ok += ff
        if not ff: bad_first.append(s)
        last_bad = -1
        for i in range(a, b):
            tot += 1; exact += sh[i] == want(E[i])
            if classify(sh[i]) == 'BROKEN': broken += 1
            st_s = calls[i][0]
            if st_s - N < dk: held += 1; continue     # time-model picture is before the key: unreachable, key held
            if sh[i] != want(st_s - N): last_bad = i - a
        fo_time += sh[a] == want(calls[a][0] - N)
        settle_max = max(settle_max, last_bad + 1)
        if last_bad >= 0 and b - a > N + 1 and last_bad >= (b - a) - 1: perm += 1
    txt = ('targets %d first-ok %d (time model %d) calls %d exact %d broken %d key-held %d settle<=%d (N %d) stuck %d%s' %
           (len(segs), first_ok, fo_time, tot, exact, broken, held, settle_max, N, perm,
            '' if not bad_first else ' badfirst ' + ','.join(map(str, bad_first[:8]))))
    good = exact == tot and broken == 0 and first_ok == len(segs) and settle_max <= N + 1 and perm == 0
    report(name, ok, v17, txt, good)

def build_seeks(targets, warm=10):
    calls = [(s, 0) for s in range(min(warm, n))]; segs = []
    for s in targets:
        k = max([x for x in sync if x <= s] or [0])
        calls += [(x, 0) for x in range(k, s)]
        a = len(calls); e = min(n, s + A.K)
        calls += [(x, 0) for x in range(s, e)]
        segs.append((a, len(calls), s))
    return calls, segs

def samp_at(t):
    c = [i for i in range(n) if st[i] <= t + 1e-6 and disp[i] >= 0]
    return max(c) if c else 0

if 'twice' in scen:
    calls = [(0, 0)] + lin
    segrun('twice', calls, [(0, len(calls), 0)], False)
if 'loop' in scen:
    calls = lin + [(s, 0) for s in range(min(n, 40))]
    segrun('loop', calls, [(n, len(calls), 0)], True)
if 'seek' in scen:
    rnd = random.Random(A.seed)
    tg = [samp_at(rnd.uniform(0, max(0.1, info['end'] - 2.0))) for q in range(A.ntarg)]
    calls, segs = build_seeks(tg); segrun('seek', calls, segs, False)
if 'keyseek' in scen:
    tg = [k for k in sorted(sync) if disp[k] >= 0]
    tg = tg[1:] + tg[:1]          # end with a jump back to the first key frame
    calls, segs = build_seeks(tg); segrun('keyseek', calls, segs, True)
if 'posterseek' in scen:
    calls = [(s, 0) for s in range(min(10, n))]; segs = []
    for k in [k for k in sorted(sync) if disp[k] >= 0][1:4] + [0]:
        a = len(calls); calls += [(k, 0), (k, 0)] + [(x, 0) for x in range(k + 1, min(n, k + A.K))]
        segs.append((a, len(calls), k))
    segrun('posterseek', calls, segs, True)
if 'drop' in scen:
    # v17c: QuickTime skips k samples mid-GOP (late, rate != 1): no reset; never a picture
    # older than one already shown; exact again from N samples after the next key sample.
    keys = sorted(k for k in sync if disp[k] >= 0)
    for kk in (1, 2, 5):
        if len(keys) < 3: break
        s0 = keys[1] + 7; K = min([k for k in keys if k > s0 + kk] or [n])
        end = min(n, K + N0 + 30) if 'N0' in dir() else min(n, K + 40)
        calls = [(x, 0) for x in range(0, s0)] + [(x, 0) for x in range(s0 + kk, end)]
        ok, N, v17, dst, last = run('drop%d' % kk, calls)
        sh = shown(dst, len(calls))
        back = broken = 0; lastj = -1; first_exact = None; bad_after = 0
        for i, (x, fl) in enumerate(calls):
            h = sh[i]
            if h in mdset:
                j = min(mdset[h], key=lambda q: abs(q - (x - N)))
                if i > s0 and j < lastj: back += 1
                lastj = max(lastj, j)
            elif h != black: broken += 1
            if x >= K + N:
                if h != want(x - N): bad_after += 1
        txt = 'dropped %d at sample %d, next key %d: stale/backwards %d, broken %d (between drop and key), wrong after key+N %d' % (
            kk, s0, K, back, broken, bad_after)
        report('drop%d' % kk, ok, v17, txt, back == 0 and bad_after == 0)
print('RESULT %s %s%s' % ('PASS' if not FAIL else 'FAIL', os.path.basename(info['src']), '' if not FAIL else ' (' + ','.join(FAIL) + ')'))
sys.exit(1 if FAIL else 0)
