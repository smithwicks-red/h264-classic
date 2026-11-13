#!/usr/bin/env python3
"""
qtify.py -- turn an ISO-BMFF (.mp4) file into something QuickTime 6 on
            Mac OS 9 will parse with its *MOV* parser, video AND audio.

------------------------------------------------------------------------
Why this exists
------------------------------------------------------------------------
QuickTime has two completely separate parsers for what is essentially the
same box structure, and they disagree in opposite directions:

  * The MPEG-4 importer understands 'esds' natively (it was written for
    MPEG-4 audio) but PREDATES H.264, so it throws away the 'avcC'
    sample-description extension it does not recognise.  Our decoder then
    gets a sample description with no SPS/PPS and QuickTime reports
    codecExtensionNotFoundErr (-8971).

  * The MOV parser copies every sample-description extension VERBATIM, so
    'avcC' survives and our decoder works -- but it expects audio to be
    described the QuickTime way, and an ISO 'mp4a' entry is not.

Flipping the ftyp major brand to 'qt  ' (four bytes, offset 8) forces the
MOV parser.  That fixes video.  This tool additionally fixes the audio
sample description so the same file plays sound.

------------------------------------------------------------------------
What is actually wrong with an ISO 'mp4a' entry, from QuickTime's view
------------------------------------------------------------------------
ISO writes a *version 0* AudioSampleEntry:

    ... channels=2 sampleSize=16 compressionID=0 packetSize=0
        sampleRate=48000.0
        esds                              <- bare, at the top of the entry

Version 0 means "one sample per packet, fixed frame size" -- true for raw
PCM, nonsense for AAC, where one entry in the stsz table is a 1024-sample
compressed frame.  QuickTime therefore cannot work out the packet framing
and produces silence.

QuickTime writes a *version 1* SoundDescription:

    ... channels=2 sampleSize=16 compressionID=0xFFFE packetSize=0
        sampleRate=48000.0
        samplesPerPacket=1024 bytesPerPacket=0 bytesPerFrame=0
        bytesPerSample=2
        wave                              <- the decoder config, wrapped
          frma = 'mp4a'
          mp4a = 00 00 00 00
          esds = ...
          <8-byte terminator>

compressionID 0xFFFE is -2, kVariableCompression: "the frame size varies,
consult samplesPerPacket".  The 'wave' atom is QuickTime's
siDecompressionParam extension; the Sound Manager looks for the codec
config *there*, not loose in the entry.

Both changes were verified against a reference file produced by
`ffmpeg -c copy -f mov`, whose MOV muxer has interoperated with real
QuickTime for two decades.  We copy its layout byte for byte, including
its 8-byte terminator atom (size=8, type=0) rather than the bare 4 zero
bytes some documentation describes.

------------------------------------------------------------------------
The offset problem, and why we cannot just splice bytes in
------------------------------------------------------------------------
Growing the sample description grows 'moov'.  In a faststart file
(ftyp / moov / mdat) that pushes 'mdat' further down the file -- but
'stco' holds ABSOLUTE file offsets to every chunk.  Splice without
fixing stco and every chunk offset is short by the delta; QuickTime then
reads the middle of some other chunk and you get the "Invalid NAL unit
size" class of corruption we already walked into once when we tried
deleting the ftyp box outright.

So: splice, then add the delta to every stco/co64 entry that points at
or past the insertion point, then repair the size field of every box
that encloses it.

------------------------------------------------------------------------
Usage
------------------------------------------------------------------------
    python3 qtify.py in.mp4 out.mov [--no-brand] [--audio-only]

    --no-brand    leave the ftyp brand alone (audio fix only)
    --audio-only  do not touch the brand and do not rename; for testing
"""

import sys
import struct

CONTAINERS = {b'moov', b'trak', b'mdia', b'minf', b'stbl',
              b'edts', b'udta', b'wave', b'dinf'}

AAC_SAMPLES_PER_PACKET = 1024      # one AAC-LC frame
kVariableCompression   = 0xFFFE    # -2 as an unsigned 16-bit field


# ----------------------------------------------------------------------
# a minimal box walker that remembers who contains what
# ----------------------------------------------------------------------

class Box(object):
    __slots__ = ('typ', 'off', 'size', 'hdr', 'parent', 'kids')

    def __init__(self, typ, off, size, hdr, parent):
        self.typ, self.off, self.size, self.hdr = typ, off, size, hdr
        self.parent = parent
        self.kids = []

    def __repr__(self):
        return "<%s off=%d size=%d>" % (self.typ.decode('latin-1'),
                                        self.off, self.size)


def parse(data, start, end, parent, flat):
    off = start
    while off + 8 <= end:
        size = struct.unpack('>I', data[off:off + 4])[0]
        typ = data[off + 4:off + 8]
        hdr = 8
        if size == 1:
            size = struct.unpack('>Q', data[off + 8:off + 16])[0]
            hdr = 16
        elif size == 0:
            size = end - off
        if size < hdr or off + size > end:
            break
        b = Box(typ, off, size, hdr, parent)
        flat.append(b)
        if parent is not None:
            parent.kids.append(b)
        if typ in CONTAINERS:
            parse(data, off + hdr, off + size, b, flat)
        elif typ == b'stsd':
            # full box header (4) + entry_count (4), then the entries,
            # each of which is itself a box
            parse(data, off + hdr + 8, off + size, b, flat)
        off += size


def ancestors(b):
    out = []
    p = b.parent
    while p is not None:
        out.append(p)
        p = p.parent
    return out


# ----------------------------------------------------------------------
# the audio sample description rewrite
# ----------------------------------------------------------------------

def build_qt_audio_entry(old):
    """Take an ISO version-0 'mp4a' AudioSampleEntry and return the
    QuickTime version-1 equivalent with the esds wrapped in a wave atom."""

    if old[4:8] != b'mp4a':
        raise ValueError("not an mp4a entry")

    version = struct.unpack('>H', old[16:18])[0]
    if version != 0:
        return None, "sound description is already version %d" % version

    channels, sample_size = struct.unpack('>HH', old[24:28])
    sample_rate = old[32:36]           # keep the 16.16 fixed value verbatim

    # everything after the 36-byte version-0 record is extensions
    extensions = old[36:]

    # find the bare esds among them
    esds = None
    off = 0
    while off + 8 <= len(extensions):
        sz = struct.unpack('>I', extensions[off:off + 4])[0]
        ty = extensions[off + 4:off + 8]
        if sz < 8 or off + sz > len(extensions):
            break
        if ty == b'esds':
            esds = extensions[off:off + sz]
        off += sz

    if esds is None:
        return None, "no esds extension found - is this really AAC?"

    # --- the wave atom, laid out exactly as ffmpeg's mov muxer writes it
    frma = struct.pack('>I', 12) + b'frma' + b'mp4a'
    inner = struct.pack('>I', 12) + b'mp4a' + b'\x00\x00\x00\x00'
    term = struct.pack('>I', 8) + b'\x00\x00\x00\x00'
    wave_body = frma + inner + esds + term
    wave = struct.pack('>I', 8 + len(wave_body)) + b'wave' + wave_body

    # --- the version-1 sound description
    body = (
        b'\x00\x00\x00\x00\x00\x00'                 # reserved[6]
        + old[14:16]                                # data_reference_index
        + struct.pack('>H', 1)                      # version = 1
        + b'\x00\x00'                               # revision
        + b'\x00\x00\x00\x00'                       # vendor
        + struct.pack('>HH', channels, sample_size)
        + struct.pack('>HH', kVariableCompression, 0)
        + sample_rate
        + struct.pack('>IIII',
                      AAC_SAMPLES_PER_PACKET,       # samplesPerPacket
                      0,                            # bytesPerPacket
                      0,                            # bytesPerFrame
                      2)                            # bytesPerSample
        + wave
    )
    new = struct.pack('>I', 8 + len(body)) + b'mp4a' + body
    return new, None


# ----------------------------------------------------------------------
# main
# ----------------------------------------------------------------------

def qtify(data, patch_brand=True, verbose=True):
    flat = []
    parse(data, 0, len(data), None, flat)

    def say(*a):
        if verbose:
            print(*a)

    entries = [b for b in flat
               if b.typ == b'mp4a'
               and b.parent is not None and b.parent.typ == b'stsd']
    if not entries:
        say("  no mp4a sample entry found; audio untouched")
        new_data = bytearray(data)
        delta = 0
    elif len(entries) > 1:
        raise SystemExit("more than one mp4a entry; this tool handles one")
    else:
        e = entries[0]
        old_entry = data[e.off:e.off + e.size]
        new_entry, err = build_qt_audio_entry(old_entry)
        if new_entry is None:
            say("  audio: %s -- leaving it alone" % err)
            new_data = bytearray(data)
            delta = 0
        else:
            delta = len(new_entry) - len(old_entry)
            say("  audio: mp4a entry %d -> %d bytes (delta %+d)"
                % (len(old_entry), len(new_entry), delta))
            say("         version 0 -> 1, compressionID 0x0000 -> 0xFFFE,")
            say("         samplesPerPacket=%d, esds wrapped in a wave atom"
                % AAC_SAMPLES_PER_PACKET)

            new_data = bytearray(data[:e.off]) \
                + bytearray(new_entry) \
                + bytearray(data[e.off + e.size:])

            # 1. grow every box that encloses the entry
            for anc in ancestors(e):
                sz = struct.unpack('>I', new_data[anc.off:anc.off + 4])[0]
                if sz == 1:
                    old = struct.unpack('>Q', new_data[anc.off + 8:anc.off + 16])[0]
                    struct.pack_into('>Q', new_data, anc.off + 8, old + delta)
                else:
                    struct.pack_into('>I', new_data, anc.off, sz + delta)
                say("         grew %-4s %d -> %d"
                    % (anc.typ.decode('latin-1'), anc.size, anc.size + delta))

            # 2. shift every chunk offset that points at or past the splice
            insertion = e.off
            for b in flat:
                if b.typ not in (b'stco', b'co64'):
                    continue
                base = b.off + (delta if b.off > insertion else 0)
                n = struct.unpack('>I', new_data[base + 12:base + 16])[0]
                wide = (b.typ == b'co64')
                step = 8 if wide else 4
                moved = 0
                for i in range(n):
                    p = base + 16 + i * step
                    if wide:
                        v = struct.unpack('>Q', new_data[p:p + 8])[0]
                    else:
                        v = struct.unpack('>I', new_data[p:p + 4])[0]
                    if v >= insertion:
                        v += delta
                        moved += 1
                        if wide:
                            struct.pack_into('>Q', new_data, p, v)
                        else:
                            struct.pack_into('>I', new_data, p, v)
                say("         %s: shifted %d of %d chunk offsets by %+d"
                    % (b.typ.decode('latin-1'), moved, n, delta))

    # 3. the four-byte brand patch that forces the MOV parser
    if patch_brand:
        if bytes(new_data[4:8]) != b'ftyp':
            say("  !! file does not start with an ftyp box; brand NOT patched")
        else:
            old_brand = bytes(new_data[8:12])
            new_data[8:12] = b'qt  '
            say("  brand: %r -> 'qt  '  (forces the MOV parser, saving avcC)"
                % old_brand.decode('latin-1'))

    return bytes(new_data), delta


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = set(a for a in sys.argv[1:] if a.startswith('--'))
    if len(args) != 2:
        raise SystemExit(__doc__)
    src, dst = args
    data = open(src, 'rb').read()
    print("qtify: %s (%d bytes)" % (src, len(data)))
    out, delta = qtify(data, patch_brand=('--no-brand' not in opts
                                          and '--audio-only' not in opts))
    open(dst, 'wb').write(out)
    print("  wrote %s (%d bytes)" % (dst, len(out)))
