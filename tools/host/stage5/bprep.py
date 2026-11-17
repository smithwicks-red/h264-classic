"""bprep.py MOVIE OUTDIR [REF.md5] -- stage 5 (v17) host test data for one movie.

Writes into OUTDIR:
  movie.samples   avcC + every video sample as QuickTime hands it over (decode_run format)
  times.txt       per sample: media start time, duration, time scale (syncFrameTime)
  info.json       n, frames, sync samples, display index of every sample (-1 = 1-byte
                  tail sample), md5 of every display picture, movie time of every
                  sample (edit list applied, as qt6.py), rdly atom if present, Nmap
The display mapping comes from ffprobe on the samples as a raw Annex B stream
(pkt_pos -> sample); the md5s from ffmpeg framemd5 of the same stream (yuv420p),
cross-checked with REF.md5 when given.
"""
import sys, os, struct, json, subprocess, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mp4, rewrite

def main():
    src, out = sys.argv[1], sys.argv[2]
    ref = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(out, exist_ok=True)
    buf, top, moov, T = mp4.load(src); t = rewrite.video(T)
    avcC = t.avcC
    # 'rdly' in the sample entry
    sd = t.stbl.get(b'stsd').data; i = sd.find(b'rdly'); rdly = None
    if i > 0 and struct.unpack('>I', sd[i-4:i])[0] == 12: rdly = struct.unpack('>I', sd[i+4:i+8])[0]
    samples = [buf[t.offs[n]:t.offs[n] + t.sizes[n]] for n in range(t.n)]
    with open(out + '/movie.samples', 'wb') as f:
        f.write(struct.pack('>I', len(avcC)) + avcC + struct.pack('>I', t.n))
        for s in samples: f.write(struct.pack('>I', len(s)) + s)
    with open(out + '/times.txt', 'w') as f:
        for n in range(t.n): f.write('%d %d %d\n' % (t.dts[n], t.durs[n], t.ts))
    # movie time of each sample (qt6.table)
    ed = t.trak.get(b'edts'); shift = 0.0; mt = t.elst_mt
    if ed:
        el = ed.get(b'elst').data; ne = struct.unpack('>I', el[4:8])[0]; p = 8
        for k in range(ne):
            d, m = struct.unpack('>Ii', el[p:p+8]); p += 12
            if m == -1: shift += d / t.movie_ts
            else: mt = m; break
    st = [(d - mt) / t.ts + shift for d in t.dts]
    sync = sorted(t.sync) if t.sync is not None else list(range(t.n))
    # raw Annex B
    ls = (avcC[4] & 3) + 1
    nsps = avcC[5] & 31; p = 6; ps = b''
    for _ in range(nsps):
        L = struct.unpack('>H', avcC[p:p+2])[0]; ps += b'\0\0\0\1' + avcC[p+2:p+2+L]; p += 2 + L
    npps = avcC[p]; p += 1
    for _ in range(npps):
        L = struct.unpack('>H', avcC[p:p+2])[0]; ps += b'\0\0\0\1' + avcC[p+2:p+2+L]; p += 2 + L
    raw = bytearray(); pos_of = {}; real = []
    for n, s in enumerate(samples):
        if len(s) <= 1: continue
        pos_of[len(raw)] = n; real.append(n)
        if not raw: raw += ps
        k = 0
        while k + ls <= len(s):
            L = int.from_bytes(s[k:k+ls], 'big'); k += ls
            raw += b'\0\0\0\1' + s[k:k+L]; k += L
    rawp = out + '/movie.264'; open(rawp, 'wb').write(raw)
    pp = subprocess.run(['ffprobe', '-v', 'error', '-f', 'h264', '-i', rawp, '-select_streams', 'v',
                         '-show_entries', 'frame=pkt_pos', '-of', 'csv=p=0'], capture_output=True, text=True).stdout.split()
    disp = [-1] * t.n
    for j, x in enumerate(pp):
        disp[pos_of[int(x.strip(','))]] = j
    fm = subprocess.run(['ffmpeg', '-v', 'error', '-f', 'h264', '-i', rawp, '-fps_mode', 'passthrough', '-pix_fmt', 'yuv420p',
                         '-f', 'framemd5', '-'], capture_output=True, text=True).stdout
    md = [l.rsplit(',', 1)[1].strip() for l in fm.splitlines() if l and not l.startswith('#')]
    refok = None
    if ref:
        rm = [l.rsplit(',', 1)[1].strip() for l in open(ref) if not l.startswith('#')]
        refok = (rm == md)

    nmap = max([n - disp[n] for n in range(t.n) if disp[n] >= 0] or [0])
    info = dict(src=os.path.abspath(src), n=t.n, frames=len(md), sync=sync, disp=disp, md5=md, st=st,
                end=st[-1] + t.durs[-1] / t.ts, rdly=rdly, Nmap=nmap, ref_match=refok,
                width=None, tails=sum(1 for s in samples if len(s) == 1), mapped=sum(1 for d in disp if d >= 0))
    w, h = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=width,height',
                           '-of', 'csv=p=0', src], capture_output=True, text=True).stdout.strip().split(',')[:2]
    info['width'], info['height'] = int(w), int(h)
    json.dump(info, open(out + '/info.json', 'w'))
    print('%s: samples %d frames %d mapped %d tails %d sync %d Nmap %d rdly %s ref %s %dx%d' % (
        os.path.basename(src), t.n, len(md), info['mapped'], info['tails'], len(sync), nmap, rdly, refok, info['width'], info['height']))
main()
