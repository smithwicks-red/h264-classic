import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:12] == b'Joy!peffpwpc'
nsec = struct.unpack('>H', d[32:34])[0]
secs = []
for i in range(nsec):
    o = 40 + 28 * i
    nameOff, defAddr, execSz, unpSz, rawSz, cOff, kind = struct.unpack('>iIIIIIB', d[o:o+25])
    secs.append((kind, cOff, rawSz, unpSz))
ld = [s for s in secs if s[0] == 4][0]; L = ld[1]
(mainSec, mainOff, initSec, initOff, termSec, termOff, nLibs, nImp, nRelSec, relInstrOff,
 strOff, hashOff, hashPow, nExp) = struct.unpack('>iIiIiIIIIIIIII', d[L:L+56])
def cstr(off):
    s = L + strOff + off; e = d.index(b'\0', s); return d[s:e].decode('mac_roman')
libs = []
for i in range(nLibs):
    o = L + 56 + 24 * i
    nameO, oldV, curV, nI, firstI = struct.unpack('>IIIII', d[o:o+20])
    libs.append((cstr(nameO), nI, firstI))
symbase = L + 56 + 24 * nLibs
syms = []
for i in range(nImp):
    w = struct.unpack('>I', d[symbase + 4*i: symbase + 4*i + 4])[0]
    syms.append(cstr(w & 0xFFFFFF))
print('sections:', [(s[0], s[2], s[3]) for s in secs])
print('main section %d offset 0x%X, exports %d' % (mainSec, mainOff, nExp))
for name, n, f in libs:
    print('%s (%d): %s' % (name, n, ' '.join(sorted(syms[f:f+n]))))
# exports
hashTab = L + hashOff; keyTab = hashTab + 4 * (1 << hashPow); symTab = keyTab + 4 * nExp
for i in range(nExp):
    k = struct.unpack('>I', d[keyTab + 4*i: keyTab + 4*i + 4])[0]; ln = k >> 16
    cn, sec = struct.unpack('>Ii', d[symTab + 10*i: symTab + 10*i + 8])[0], 0
    cls_name = struct.unpack('>I', d[symTab + 10*i: symTab + 10*i + 4])[0]
    val = struct.unpack('>I', d[symTab + 10*i + 4: symTab + 10*i + 8])[0]
    secn = struct.unpack('>h', d[symTab + 10*i + 8: symTab + 10*i + 10])[0]
    nm = d[L + strOff + (cls_name & 0xFFFFFF): L + strOff + (cls_name & 0xFFFFFF) + ln].decode()
    print('export %-34s class %d section %d value 0x%X' % (nm, cls_name >> 24, secn, val))
