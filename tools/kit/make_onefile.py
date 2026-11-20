"""
make_onefile.py PEF OUTDIR [NAME] [INFO]

Turn CodeWarrior's shared-library output (the data fork of "H264 Decoder")
into a ONE-FILE QuickTime component, layout B1 (Session 14, 11 Nov 2026,
tested at boot in the VM):

  'thng' 256  extended template, registration flags 9
              (componentHasMultiplePlatforms | componentDoAutoVersion),
              version 0x00010000 (env H264_THNG_VERSION overrides, e.g. 0x00010010 for 16t), one platform entry:
              flags 0, code 'cdec' 256, platform 2 (PowerPC)
  'STR ' 256  name, 'STR ' 257 info
  'cdec' 256  the PEF container with its loader header's main symbol set
              to H264CodecComponentDispatchRD (a RoutineDescriptor, found
              in the export table, so any build works)
  empty data fork, type 'thng'

Writes NAME, NAME.rdump, NAME.idump (the share's format, which
macpack.load_from_share reads) and NAME.hqx into OUTDIR.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import macpack

def find_export(pef, want):
    nsec = struct.unpack('>H', pef[32:34])[0]
    L = None
    for i in range(nsec):
        o = 40 + 28 * i
        if pef[o + 24] == 4:
            L = struct.unpack('>I', pef[o + 20:o + 24])[0]
    (mainSec, mainOff, initSec, initOff, termSec, termOff, nLibs, nImp, nRelSec,
     relInstrOff, strOff, hashOff, hashPow, nExp) = struct.unpack('>iIiIiIIIIIIIII', pef[L:L + 56])
    keyTab = L + hashOff + 4 * (1 << hashPow)
    symTab = keyTab + 4 * nExp
    for i in range(nExp):
        ln = struct.unpack('>I', pef[keyTab + 4 * i:keyTab + 4 * i + 4])[0] >> 16
        cn, val, sec = struct.unpack('>IIh', pef[symTab + 10 * i:symTab + 10 * i + 10])
        s = L + strOff + (cn & 0xFFFFFF)
        if pef[s:s + ln] == want:
            return L, sec, val, cn >> 24
    raise SystemExit('export %r not found' % want)

def hexblock(b):
    out = []
    for i in range(0, len(b), 16):
        h = b[i:i + 16].hex().upper()
        out.append('\t$"%s"' % ' '.join(h[j:j + 4] for j in range(0, len(h), 4)))
    return '\n'.join(out)

def res(t, i, b):
    return "data '%s' (%d) {\n%s\n};\n\n" % (t, i, hexblock(b))

def pstr(s):
    b = s.encode('mac_roman')
    return bytes([len(b)]) + b

def main(pefpath, outdir, name='H264 Decoder',
         info='H.264/AVC Baseline decoder for QuickTime, powered by h264bsd. '
              'Homebrew port for Mac OS 9.2.2 / PowerPC.'):
    pef = bytearray(open(pefpath, 'rb').read())
    assert pef[:12] == b'Joy!peffpwpc', 'not a PowerPC PEF'
    L, sec, val, cls = find_export(pef, b'H264CodecComponentDispatchRD')
    assert cls == 1, 'the RoutineDescriptor should be a data symbol'
    struct.pack_into('>iI', pef, L, sec, val)
    thng = (b'imdc' + b'avc1' + b'wY9H' + struct.pack('>II', 0, 0) +
            b'cdec' + struct.pack('>h', 256) +
            b'STR ' + struct.pack('>h', 256) + b'STR ' + struct.pack('>h', 257) +
            b'\0\0\0\0' + struct.pack('>h', 0) +
            struct.pack('>IihI', int(os.environ.get('H264_THNG_VERSION', '0x00010000'), 16), 9, 0, 1) +
            struct.pack('>I', 0) + b'cdec' + struct.pack('>hh', 256, 2))
    assert len(thng) == 70
    os.makedirs(outdir, exist_ok=True)
    base = os.path.join(outdir, name)
    open(base, 'wb').close()
    with open(base + '.rdump', 'w') as f:
        f.write(res('thng', 256, thng) + res('STR ', 256, pstr('H.264 (Software)')) +
                res('STR ', 257, pstr(info)) + res('cdec', 256, bytes(pef)))
    with open(base + '.idump', 'w') as f:
        f.write('thng????\n')
    mf = macpack.load_from_share(base)
    with open(base + '.hqx', 'wb') as f:
        f.write(macpack.binhex(mf))
    print('%s: main = section %d offset 0x%X; resource fork %d bytes' % (name, sec, val, len(mf.rsrc)))

if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
