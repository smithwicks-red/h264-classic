"""
make_kit.py SHAREDIR MOVIE OUTDIR

Build the hardware test kit from the binaries CodeWarrior produced in the
VM. SHAREDIR holds them as the ClassicVirtio share stores them (NAME,
NAME.rdump, NAME.idump); MOVIE is the MP4Fix-converted test clip.

Writes into OUTDIR:
    H264 Decoder.hqx, H264 Auto-Register.hqx, mp4fixer.hqx
    H264 test movie.mov
    H264 Kit.dsk            HFS image with every file already decoded
    READ ME FIRST.txt       CR line endings, for SimpleText
    SHA256SUMS.txt
and prints what went in, so the build log records it.
"""

import hashlib
import os
import sys
import time

import machfs

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import macpack  # noqa: E402

BINARIES = ['H264 Decoder', 'H264 Auto-Register', 'mp4fixer', 'H264 Stream']
# A binary not in SHAREDIR may be taken from an earlier kit's HFS image:
# set KIT_FROM_DSK=/path/to/old/H264 Kit.dsk (v15 kit: mp4fixer from v14).
FROM_DSK = os.environ.get('KIT_FROM_DSK')


def load_from_dsk(dsk, name):
    vol = machfs.Volume()
    with open(dsk, 'rb') as f:
        vol.read(f.read())
    if name not in vol:
        return None
    hf = vol[name]
    return macpack.MacFile(name, hf.data, hf.rsrc, hf.type, hf.creator, hf.flags)
MOVIE_NAME = 'H264 test movie.mov'
README_NAME = 'READ ME FIRST.txt'


def main(sharedir, movie, outdir):
    os.makedirs(outdir, exist_ok=True)
    here = os.path.dirname(os.path.abspath(__file__))
    files = []

    for name in BINARIES:
        if os.path.exists(os.path.join(sharedir, name)):
            mf = macpack.load_from_share(os.path.join(sharedir, name))
        else:
            mf = load_from_dsk(FROM_DSK, name) if FROM_DSK else None
            if mf is None:
                # v14: the one-file decoder needs no H264 Auto-Register.
                print('%-20s (not in %s, left out)' % (name, sharedir))
                continue
            print('%-20s (taken from %s)' % (name, os.path.basename(FROM_DSK)))
        if not mf.rsrc:
            raise SystemExit('%s has no resource fork -- it would not run' % name)
        files.append(mf)
        with open(os.path.join(outdir, name + '.hqx'), 'wb') as f:
            f.write(macpack.binhex(mf))
        print('%-20s %-4s/%-4s data %7d  rsrc %4d  -> %s.hqx' % (
            name, mf.type.decode('mac_roman'), mf.creator.decode('mac_roman'),
            len(mf.data), len(mf.rsrc), name))

    with open(movie, 'rb') as f:
        movie_data = f.read()
    with open(os.path.join(outdir, MOVIE_NAME), 'wb') as f:
        f.write(movie_data)
    files.append(macpack.MacFile(MOVIE_NAME, movie_data, b'', b'MooV',
                                 b'TVOD', 0))
    print('%-20s MooV/TVOD data %7d' % (MOVIE_NAME, len(movie_data)))

    with open(os.path.join(here, 'READ_ME_FIRST.txt'), encoding='utf-8') as f:
        readme = f.read()
    readme_mf = macpack.simpletext(README_NAME, readme)
    with open(os.path.join(outdir, README_NAME), 'wb') as f:
        f.write(readme_mf.data)
    files.append(readme_mf)

    # One HFS volume with everything already decoded. 8 MB is roomy for
    # the 2-3 MB clip and small enough to write to any card.
    vol = machfs.Volume()
    vol.name = 'H264 Kit'
    now = int(time.time()) + macpack.MAC_EPOCH_OFFSET
    vol.crdate = vol.mddate = now
    for mf in files:
        hf = machfs.File()
        hf.data, hf.rsrc = mf.data, mf.rsrc
        hf.type, hf.creator = mf.type, mf.creator
        hf.flags = mf.flags & ~macpack.FLAGS_TO_CLEAR
        hf.crdate = hf.mddate = now
        vol[mf.name.decode('mac_roman')] = hf
    image = vol.write(size=8 * 1024 * 1024, desktopdb=True, bootable=False)
    with open(os.path.join(outdir, 'H264 Kit.dsk'), 'wb') as f:
        f.write(image)
    print('H264 Kit.dsk         %d bytes, HFS volume "H264 Kit"' % len(image))

    with open(os.path.join(outdir, 'SHA256SUMS.txt'), 'w') as f:
        for n in sorted(os.listdir(outdir)):
            if n == 'SHA256SUMS.txt':
                continue
            with open(os.path.join(outdir, n), 'rb') as g:
                f.write('%s  %s\n' % (hashlib.sha256(g.read()).hexdigest(), n))


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
