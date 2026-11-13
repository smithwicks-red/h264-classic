"""
extract_run.py SRC OUTDIR -- cut the pieces of H264Codec.c that the
runtime harness (decode_run.c) executes, VERBATIM, into OUTDIR.

Same idea as tools/ctx/extract.py, which only type-checks. This one also
pulls out FeedParameterSets and EnsureDecoder, because decode_run.c RUNS
the code against the real h264bsd and needs the real set-up path, not
stubs. SRC is a parameter so an older copy of H264Codec.c can be cut
into a second directory and the two builds compared side by side.
"""
import os, sys

src_path, outdir = sys.argv[1], sys.argv[2]
src = open(src_path, encoding='latin-1').read().split('\n')
os.makedirs(outdir, exist_ok=True)

def find(pred, start=0, what="?"):
    for i in range(start, len(src)):
        if pred(src[i]):
            return i
    raise SystemExit("marker not found: " + what)

def func_from(i):
    depth, seen = 0, False
    for j in range(i, len(src)):
        depth += src[j].count('{') - src[j].count('}')
        if '{' in src[j]:
            seen = True
        if seen and depth == 0:
            return src[i:j + 1]
    raise SystemExit("unbalanced from line %d" % (i + 1))

out = {}
a = find(lambda l: l.startswith('#define kOfferYUVOutput'), what="kOfferYUVOutput")
# older sources end the config block at the LogPlaybackStats prototype;
# newer ones add kMaxDecodeStalls after it
try:
    b = find(lambda l: l.startswith('#define kMaxDecodeStalls'), a, "kMaxDecodeStalls")
except SystemExit:
    b = find(lambda l: l.startswith('static void LogPlaybackStats(void);'), a, "LogPlaybackStats fwd")
out['frag_config.inc'] = src[a:b + 1]

e = find(lambda l: l.startswith('} H264Globals, *H264GlobalsPtr;'), what="globals end")
s = max(i for i in range(e) if src[i].startswith('typedef struct {'))
# the struct's array bound lives just above it; take it along verbatim
ks = [i for i in range(b, s) if src[i].startswith('#define kMaxParamSets')]
out['frag_globals.inc'] = ([src[ks[-1]]] if ks else []) + src[s:e + 1]

fp = find(lambda l: l.startswith('static int FeedParameterSets('), what="FeedParameterSets")
out['frag_feed.inc'] = func_from(fp)

y0 = find(lambda l: l == '#if kOfferYUVOutput', what="yuv block")
bp = find(lambda l: l.startswith('static void BlitPicture('), y0, "BlitPicture")
out['frag_yuv.inc'] = src[y0:bp] + func_from(bp)

lp = find(lambda l: l.startswith('#define kLogPathC'), what="kLogPathC")
ln = find(lambda l: l.startswith('static void LogNum(const char *label, long v)')
          and not l.rstrip().endswith(';'), lp, "LogNum def")
out['frag_loggers.inc'] = src[lp:ln] + func_from(ln)

ed = find(lambda l: l.startswith('static OSErr EnsureDecoder('), what="EnsureDecoder")
out['frag_ensure.inc'] = func_from(ed)

dr = find(lambda l: l.startswith('static void DrainOutputPictures('), what="Drain")
out['frag_drain.inc'] = func_from(dr)

bw = find(lambda l: l.startswith('static ComponentResult BandDecompressWork('), what="BandDecompressWork")
st = find(lambda l: l.startswith('static void LogPlaybackStats(void)')
          and not l.rstrip().endswith(';'), bw, "LogPlaybackStats def")
out['frag_band.inc'] = src[bw:st] + func_from(st)

order = [('config', a), ('globals', s), ('feed', fp), ('yuv', y0), ('loggers', lp),
         ('ensure', ed), ('drain', dr), ('band', bw)]
assert [x[1] for x in order] == sorted(x[1] for x in order), "file order changed: " + str(order)

for k, v in out.items():
    open(os.path.join(outdir, k), 'w').write('\n'.join(v) + '\n')
print("cut %d fragments from %s into %s" % (len(out), src_path, outdir))
