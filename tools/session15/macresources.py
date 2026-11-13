# Minimal stand-in for the 'macresources' package used by macpack.py:
# parse DeRez 'data' blocks and build a resource fork.
import re, struct
class Res:
    def __init__(s, t, i, n, d): s.type, s.id, s.name, s.data = t, i, n, d
def parse_rez_code(text):
    if isinstance(text, bytes): text = text.decode('mac_roman')
    out = []
    for m in re.finditer(r"data\s+'(.{4})'\s*\(\s*(-?\d+)\s*(?:,\s*\"([^\"]*)\")?[^)]*\)\s*\{(.*?)\};", text, re.S):
        hexs = ''.join(re.findall(r'\$"([0-9A-Fa-f ]*)"', m.group(4))).replace(' ', '')
        out.append(Res(m.group(1).encode('mac_roman'), int(m.group(2)), m.group(3), bytes.fromhex(hexs)))
    return out
def make_file(resources):
    resources = list(resources)
    data = b''; offs = []
    for r in resources:
        offs.append(len(data)); data += struct.pack('>I', len(r.data)) + r.data
    types = []
    for r in resources:
        if r.type not in types: types.append(r.type)
    names = b''; refl = b''; tl = struct.pack('>H', len(types) - 1)
    reflist_base = 2 + 8 * len(types)
    for t in types:
        rs = [(r, o) for r, o in zip(resources, offs) if r.type == t]
        tl += t + struct.pack('>HH', len(rs) - 1, reflist_base + len(refl))
        for r, o in rs:
            if r.name is not None:
                no = len(names); nb = r.name.encode('mac_roman'); names += bytes([len(nb)]) + nb
            else: no = 0xFFFF
            refl += struct.pack('>hHI', r.id, no, o & 0xFFFFFF) + b'\0\0\0\0'
    tlist = tl + refl
    maplen = 28 + len(tlist) + len(names)
    hdr = struct.pack('>IIII', 256, 256 + len(data), len(data), maplen)
    m = hdr + b'\0' * 4 + b'\0\0' + b'\0\0' + struct.pack('>HH', 28, 28 + len(tlist)) + tlist + names
    return hdr + b'\0' * 240 + data + m
