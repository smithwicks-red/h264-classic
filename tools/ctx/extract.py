import sys
src = open(__import__('os').path.join(__import__('os').path.dirname(__file__), '..', '..', 'codec-src', 'H264Codec.c'), encoding='latin-1').read().split('\n')
def find(pred, start=0, what="?"):
    for i in range(start, len(src)):
        if pred(src[i]): return i
    raise SystemExit("marker not found: " + what)
def func_from(i):
    depth, seen = 0, False
    for j in range(i, len(src)):
        depth += src[j].count('{') - src[j].count('}')
        if '{' in src[j]: seen = True
        if seen and depth == 0: return src[i:j+1]
    raise SystemExit("unbalanced from line %d" % (i+1))
out = {}
a = find(lambda l: l.startswith('#define kOfferYUVOutput'), what="kOfferYUVOutput")
b = find(lambda l: l.startswith('#define kMaxDecodeStalls'), a, "kMaxDecodeStalls")
out['frag_config.inc'] = src[a:b+1]
e = find(lambda l: l.startswith('} H264Globals, *H264GlobalsPtr;'), what="globals end")
s = max(i for i in range(e) if src[i].startswith('typedef struct {'))
out['frag_globals.inc'] = src[s:e+1]
y0 = find(lambda l: l == '#if kOfferYUVOutput', what="yuv block")
bp = find(lambda l: l.startswith('static void BlitPicture('), y0, "BlitPicture")
out['frag_yuv.inc'] = src[y0:bp] + func_from(bp)
lp = find(lambda l: l.startswith('#define kLogPathC'), what="kLogPathC")
ln = find(lambda l: l.startswith('static void LogNum(const char *label, long v)') and not l.rstrip().endswith(';'), lp, "LogNum def")
out['frag_loggers.inc'] = src[lp:ln] + func_from(ln)
cl = find(lambda l: l.startswith('pascal ComponentResult H264CodecClose(H264GlobalsPtr glob, ComponentInstance self)') and not l.rstrip().endswith(';'), what="Close")
out['frag_close.inc'] = func_from(cl)
pd = find(lambda l: l.startswith('pascal ComponentResult H264CodecPreDecompress(H264GlobalsPtr glob, CodecDecompressParams *p)'), what="PreDecompress")
out['frag_predecompress.inc'] = func_from(pd)
dr = find(lambda l: l.startswith('static void DrainOutputPictures('), what="Drain")
out['frag_drain.inc'] = func_from(dr)
bw = find(lambda l: l.startswith('static ComponentResult BandDecompressWork('), what="BandDecompressWork")
st = find(lambda l: l.startswith('static void LogPlaybackStats(void)') and not l.rstrip().endswith(';'), bw, "LogPlaybackStats def")
out['frag_band.inc'] = src[bw:st] + func_from(st)
# sanity: ordering in the real file must match the harness include order
order = [('config',a),('globals',s),('yuv',y0),('loggers',lp),('close',cl),('predecompress',pd),('drain',dr),('band',bw)]
assert [x[1] for x in order] == sorted(x[1] for x in order), "file order changed: " + str(order)
for k, v in out.items():
    open(k, 'w').write('\n'.join(v) + '\n')
    print("%-24s %4d lines" % (k, len(v)))
