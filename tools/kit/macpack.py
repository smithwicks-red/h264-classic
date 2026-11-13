"""
macpack.py -- carry Classic Mac OS files (data fork + resource fork +
Finder info) across systems that only know about one fork.

Used to build the "take it to the real Mac" kit for the H.264 decoder:
the binaries come out of the Mac OS 9 VM onto the UTM shared folder,
where the ClassicVirtio 9P driver stores

    NAME          the data fork, as-is
    NAME.rdump    the resource fork, as Rez source text (DeRez style)
    NAME.idump    type + creator (8 bytes), then Finder flags, one
                  keyword per line (kHasBeenInited, kHasNoINITs, ...)

and this module turns that back into real Mac files in three portable
containers:

  * BinHex 4.0 (.hqx)   7-bit text. Survives FAT USB sticks, email, web
                        downloads, FTP in ASCII mode, anything. StuffIt
                        Expander, on every Mac OS 9 install, decodes it.
  * MacBinary II (.bin) 8-bit, no expansion. Also decoded by StuffIt
                        Expander. Must travel as binary.
  * an HFS disk image   (via machfs) holding everything, for emulators
                        and for writing straight to a card or stick.

Why write the BinHex encoder here rather than use Python's: the binhex
module and binascii.b2a_hqx/rlecode_hqx were removed in Python 3.11.
Only binascii.crc_hqx -- the CRC BinHex uses -- survives, and it is
used below. The encoder is checked two independent ways: by `lsar`/
`unar` (The Unarchiver) on the host, and by StuffIt Expander in the VM
decoding the files back onto the share, where the resulting .rdump
must match the original byte for byte.
"""

import binascii
import struct
import time

# ------------------------------------------------------------------
# Finder flags (Inside Macintosh: Macintosh Toolbox Essentials 7-47)
# ------------------------------------------------------------------
FINDER_FLAGS = {
    'kIsOnDesk':       0x0001,
    'kIsShared':       0x0040,
    'kHasNoINITs':     0x0080,
    'kHasBeenInited':  0x0100,
    'kHasCustomIcon':  0x0400,
    'kIsStationery':   0x0800,
    'kNameLocked':     0x1000,
    'kHasBundle':      0x2000,
    'kIsInvisible':    0x4000,
    'kIsAlias':        0x8000,
}

# Flags a receiving Finder should recompute rather than inherit, as
# every MacBinary/BinHex writer of the era did: "inited" means "this
# Finder has already placed the icon", which is false on the new Mac,
# and "on desk" refers to the old Mac's desktop.
FLAGS_TO_CLEAR = FINDER_FLAGS['kHasBeenInited'] | FINDER_FLAGS['kIsOnDesk']


class MacFile:
    def __init__(self, name, data=b'', rsrc=b'', type=b'????',
                 creator=b'????', flags=0, mtime=None):
        if isinstance(name, str):
            name = name.encode('mac_roman')
        if not (1 <= len(name) <= 31):
            raise ValueError('HFS names are 1..31 bytes: %r' % name)
        if b':' in name:
            raise ValueError('colon in a Mac file name: %r' % name)
        self.name = name
        self.data = bytes(data)
        self.rsrc = bytes(rsrc)
        self.type = bytes(type)
        self.creator = bytes(creator)
        assert len(self.type) == 4 and len(self.creator) == 4
        self.flags = flags & 0xFFFF
        self.mtime = mtime if mtime is not None else time.time()


def parse_idump(raw):
    """-> (type, creator, finder flags) from a ClassicVirtio .idump."""
    ftype, creator = raw[0:4], raw[4:8]
    flags = 0
    for word in raw[8:].decode('ascii', 'replace').split():
        if word not in FINDER_FLAGS:
            raise ValueError('unknown Finder flag keyword %r' % word)
        flags |= FINDER_FLAGS[word]
    return ftype, creator, flags


def rsrc_from_rdump(text):
    """Compile ClassicVirtio/DeRez-style Rez text to a binary fork."""
    import macresources
    resources = list(macresources.parse_rez_code(text))
    if not resources:
        return b''
    return bytes(macresources.make_file(resources))


def load_from_share(path_without_suffix, display_name=None):
    import os
    base = path_without_suffix
    with open(base, 'rb') as f:
        data = f.read()
    rsrc = b''
    if os.path.exists(base + '.rdump'):
        with open(base + '.rdump', 'rb') as f:
            rsrc = rsrc_from_rdump(f.read())
    ftype, creator, flags = b'????', b'????', 0
    if os.path.exists(base + '.idump'):
        with open(base + '.idump', 'rb') as f:
            ftype, creator, flags = parse_idump(f.read())
    return MacFile(display_name or os.path.basename(base), data, rsrc,
                   ftype, creator, flags, os.path.getmtime(base))


# ------------------------------------------------------------------
# BinHex 4.0
# ------------------------------------------------------------------
HQX_ALPHABET = (b'!"#$%&\'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`'
                b'abcdefhijklmpqr')
assert len(HQX_ALPHABET) == 64
HQX_BANNER = b'(This file must be converted with BinHex 4.0)'


def _crc(data):
    # BinHex's CRC is CRC-16/XMODEM (poly 0x1021, init 0) -- the value
    # the spec describes as "computed as if two zero bytes followed".
    return binascii.crc_hqx(data, 0)


def _rle90(data):
    """BinHex run-length encoding. 0x90 is the marker: X 0x90 n means
    "X, n times in total"; a literal 0x90 is written 0x90 0x00. Runs are
    capped at 255, and a run of the marker byte itself is never
    compressed -- the case CPython's old comment worried about ("xx 90
    90") simply never gets produced."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        b = data[i]
        run = 1
        while i + run < n and data[i + run] == b and run < 255:
            run += 1
        if b == 0x90:
            out += b'\x90\x00'
            i += 1
            continue
        if run > 3:                   # the threshold CPython's coder used
            out.append(b)
            out += bytes((0x90, run))
            i += run
        else:
            out.append(b)
            i += 1
    return bytes(out)


def _six_bit(data):
    out = bytearray()
    for i in range(0, len(data), 3):
        chunk = data[i:i + 3]
        pad = 3 - len(chunk)
        v = int.from_bytes(chunk + b'\0' * pad, 'big')
        chars = [(v >> s) & 0x3F for s in (18, 12, 6, 0)]
        # A final partial group emits only the characters it needs.
        for c in chars[:4 - pad] if pad else chars:
            out.append(HQX_ALPHABET[c])
    return bytes(out)


def binhex(mf, eol=b'\n', linelen=64):
    header = (bytes((len(mf.name),)) + mf.name + b'\0' + mf.type +
              mf.creator + struct.pack('>HII',
                                       mf.flags & ~FLAGS_TO_CLEAR & 0xFFFF,
                                       len(mf.data), len(mf.rsrc)))
    stream = (header + struct.pack('>H', _crc(header)) +
              mf.data + struct.pack('>H', _crc(mf.data)) +
              mf.rsrc + struct.pack('>H', _crc(mf.rsrc)))
    body = b':' + _six_bit(_rle90(stream)) + b':'
    lines = [body[i:i + linelen] for i in range(0, len(body), linelen)]
    return HQX_BANNER + eol + eol + eol.join(lines) + eol


# ------------------------------------------------------------------
# MacBinary II
# ------------------------------------------------------------------
MAC_EPOCH_OFFSET = 2082844800   # seconds from 1904-01-01 to 1970-01-01


def macbinary(mf):
    def pad128(b):
        return b + b'\0' * (-len(b) % 128)
    flags = mf.flags & ~FLAGS_TO_CLEAR
    macdate = int(mf.mtime) + MAC_EPOCH_OFFSET
    h = bytearray(128)
    h[1] = len(mf.name)
    h[2:2 + len(mf.name)] = mf.name
    h[65:69] = mf.type
    h[69:73] = mf.creator
    h[73] = (flags >> 8) & 0xFF
    struct.pack_into('>II', h, 83, len(mf.data), len(mf.rsrc))
    struct.pack_into('>II', h, 91, macdate, macdate)
    h[101] = flags & 0xFF
    h[122] = 129            # MacBinary II
    h[123] = 129
    struct.pack_into('>H', h, 124, _crc(bytes(h[:124])))
    return bytes(h) + pad128(mf.data) + pad128(mf.rsrc)


# ------------------------------------------------------------------
# Plain text for SimpleText
# ------------------------------------------------------------------
def simpletext(name, text):
    """A TEXT/ttxt file: Mac Roman, CR line endings, ready for SimpleText."""
    body = text.replace('\r\n', '\n').replace('\n', '\r')
    return MacFile(name, body.encode('mac_roman'), b'', b'TEXT', b'ttxt',
                   0)
