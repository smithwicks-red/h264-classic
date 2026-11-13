"""
extract_samples.py -- pull the avcC box and every video sample (AVCC,
length-prefixed, exactly as QuickTime hands them to BandDecompress) out of
an MP4/MOV, into one flat file the C harness can read without an MP4
parser:

    u32 avcCSize, avcC bytes,
    u32 sampleCount, then per sample: u32 size, bytes

Sample offsets/sizes come from ffprobe's packet list, which for an
MP4/MOV is the sample table (stco/stsz/stsc) resolved to file offsets.
Big-endian throughout, like the file format itself.
"""
import json, struct, subprocess, sys

src, out = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
i = data.find(b'avcC')
size = struct.unpack('>I', data[i-4:i])[0]
avcc = data[i+4:i-4+size]

pk = json.loads(subprocess.check_output(
    ['ffprobe', '-v', 'error', '-select_streams', 'v:0',
     '-show_entries', 'packet=pos,size,flags', '-of', 'json', src]))['packets']
with open(out, 'wb') as f:
    f.write(struct.pack('>I', len(avcc))); f.write(avcc)
    f.write(struct.pack('>I', len(pk)))
    for p in pk:
        pos, n = int(p['pos']), int(p['size'])
        f.write(struct.pack('>I', n)); f.write(data[pos:pos+n])
keys = [k for k, p in enumerate(pk) if 'K' in p.get('flags', '')]
print("avcC %d bytes, %d samples, key samples at %s" % (len(avcc), len(pk), keys[:8]))
