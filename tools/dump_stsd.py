#!/usr/bin/env python3
"""Walk an ISO-BMFF file and print every stsd sample entry in full detail."""
import sys, struct

CONTAINERS = {b'moov',b'trak',b'mdia',b'minf',b'stbl',b'edts',b'udta',b'wave'}

def fourcc(b):
    return b.decode('latin-1')

def walk(data, start, end, depth, path, out):
    off = start
    while off + 8 <= end:
        size = struct.unpack('>I', data[off:off+4])[0]
        typ  = data[off+4:off+8]
        hdr  = 8
        if size == 1:
            size = struct.unpack('>Q', data[off+8:off+16])[0]
            hdr = 16
        elif size == 0:
            size = end - off
        if size < hdr or off + size > end:
            out.append("%s! malformed box %r size=%d at %d" % ('  '*depth, typ, size, off))
            return
        out.append("%s%s  off=%d size=%d" % ('  '*depth, fourcc(typ), off, size))
        if typ in CONTAINERS:
            walk(data, off+hdr, off+size, depth+1, path+[typ], out)
        elif typ == b'stsd':
            # full box: version/flags(4) entry_count(4)
            n = struct.unpack('>I', data[off+hdr+4:off+hdr+8])[0]
            out.append("%sentries=%d" % ('  '*(depth+1), n))
            walk_stsd(data, off+hdr+8, off+size, depth+1, out)
        off += size

def walk_stsd(data, start, end, depth, out):
    off = start
    while off + 8 <= end:
        size = struct.unpack('>I', data[off:off+4])[0]
        typ  = data[off+4:off+8]
        if size < 8 or off+size > end: break
        out.append("%s[entry] %s off=%d size=%d" % ('  '*depth, fourcc(typ), off, size))
        body = data[off:off+size]
        out.append("%shex: %s" % ('  '*(depth+1), body[:64].hex()))
        # common 8 + 6 reserved + 2 dref = 16
        dref = struct.unpack('>H', body[14:16])[0]
        out.append("%sdata_reference_index=%d" % ('  '*(depth+1), dref))
        if typ in (b'mp4a', b'sowt', b'twos', b'raw ', b'.mp3', b'ac-3'):
            ver, rev = struct.unpack('>HH', body[16:20])
            vendor   = body[20:24]
            ch, bps  = struct.unpack('>HH', body[24:28])
            cid, psz = struct.unpack('>HH', body[28:32])
            sr       = struct.unpack('>I', body[32:36])[0]
            out.append("%sSOUND DESCRIPTION VERSION = %d  revision=%d vendor=%r" % ('  '*(depth+1), ver, rev, vendor))
            out.append("%schannels=%d sampleSize=%d compressionID=0x%04x packetSize=%d" % ('  '*(depth+1), ch, bps, cid, psz))
            out.append("%ssampleRate=%.3f (0x%08x, 16.16 fixed)" % ('  '*(depth+1), sr/65536.0, sr))
            extra_at = 36
            if ver == 1:
                spp, bpp, bpf, bpsamp = struct.unpack('>IIII', body[36:52])
                out.append("%sv1: samplesPerPacket=%d bytesPerPacket=%d bytesPerFrame=%d bytesPerSample=%d"
                           % ('  '*(depth+1), spp, bpp, bpf, bpsamp))
                extra_at = 52
            elif ver == 2:
                out.append("%sv2 layout (QT7) - not decoded here" % ('  '*(depth+1)))
                extra_at = 36 + 36
            out.append("%s--- extensions following the sound description ---" % ('  '*(depth+1)))
            walk(data, off+extra_at, off+size, depth+2, [], out)
        elif typ in (b'avc1', b'mp4v', b'jpeg'):
            ver, rev = struct.unpack('>HH', body[16:20])
            w, h = struct.unpack('>HH', body[40:44])
            depthfield = struct.unpack('>H', body[82:84])[0]
            out.append("%sVIDEO version=%d width=%d height=%d depth=%d" % ('  '*(depth+1), ver, w, h, depthfield))
            out.append("%s--- extensions ---" % ('  '*(depth+1)))
            walk(data, off+86, off+size, depth+2, [], out)
        off += size

data = open(sys.argv[1],'rb').read()
out = []
walk(data, 0, len(data), 0, [], out)
print("file: %s  (%d bytes)" % (sys.argv[1], len(data)))
print('\n'.join(out))
