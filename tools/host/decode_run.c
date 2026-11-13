/*
    decode_run.c (v14) -- RUN the codec on the host.

    Compiles the WHOLE of codec-src/H264Codec.c against tools/host/host_mac.h
    and drives it the way QuickTime does: Open, PreDecompress, one
    BandDecompress per sample, Close. It links the real h264bsd and feeds
    the real sample bytes of a movie.

    Every picture the codec takes from h264bsd is captured to OUT.yuv (by
    wrapping h264bsdNextOutputPicture), and after every call the visible
    part of the destination is appended to the --dst-out file, so scripts
    can compare both with ffmpeg bit for bit.

    usage: decode_run SAMPLES OUT.yuv [options]

      --mode seq|poster|twice|list:FILE
            seq     every sample once, in order (normal play)       [default]
            poster  only the first sample, on a fresh decoder
            twice   the first sample twice, then the rest (what QuickTime
                    does: poster frame, then playback)
            list:F  the samples named in text file F, in that order
      --dst 2vuy|rgb32|rgb16|y420     destination pixel format      [2vuy]
      --dst-out FILE   write the visible destination after every call
      --size W H       the movie's display size (default 360 240, the dv512 clip)
      --clip W H       make the destination rectangle W x H (to exercise
                       odd sizes and clipping); default is the display size
      --index FILE     one line per captured picture: call, sample, deblocking off?
      --log            the log file "exists": the codec's report goes to stderr
      --ptrfail N      NewPtr refuses blocks larger than N bytes (forces
                       the temporary-memory fallback)
      --tempfail       ... and temporary memory refuses as well
      --corrupt SEED PERMILLE   damage that share of the samples (flip
                       bytes, truncate) to exercise error survival
      --sim DECODE_MS BLIT_MS   play against a simulated clock: frame n is
                       due at n/29.97 s; a call that decodes costs
                       DECODE_MS (two thirds of it with deblocking off),
                       one that paints costs BLIT_MS more; QuickTime hands
                       over every frame in order, as soon as it is due and
                       the previous call has returned (the model the
                       Pismo logs fit)
      --qt-frametime   with --sim: pass frameTime (time base + rate) as the
                       documentation describes. Default is what QuickTime
                       6.0.3 was measured to pass: only syncFrameTime.
      --keep-going     call BandDecompress even if PreDecompress failed
                       (as QuickTime may, for an unsupported stream)
      --catchup PERMILLE   flag that share of calls as QuickTime catch-up
                       frames (codecConditionCatchUpDiff)
      --seek-at N K    with --sim: after call N, jump back K samples to the
                       key frame at or before that point and replay from it
                       without waiting -- QuickTime catching up after a jump
      --prebuf-first   give PreDecompress bufferSize = the FIRST sample's
                       size, as QuickTime 6 does (v15), not the largest
*/

#define H264_HOST_BUILD 1

#include "host_mac.h"

#include "basetype.h"
#include "h264bsd_decoder.h"

/* ---- capture every picture the codec receives from h264bsd ---- */
static FILE *gOut, *gIndex;
static long  gPicsThisCall, gPicsTotal;
static long  gCurCall, gCurSample;
static int   gDeblockOffNow;
static u8   *gIsKey;          /* per sample: contains an IDR slice */
static int   gTainted;        /* a picture since the last key frame was not deblocked */
/* --sim: the simulated clock. Each picture charges its decode time and
   each blit its paint time to the codec's own Microseconds(), so the
   codec sees time pass INSIDE the call, as on a real Mac. */
static int    gSim;
static double gSimDecodeUs, gSimBlitUs;
static void HostSimBlit(void) { if (gSim) gFakeClock += (UInt32)gSimBlitUs; }
#define H264_HOST_BLIT_HOOK() HostSimBlit()

static u8 *Harness_NextOutputPicture(storage_t *s, u32 *id, u32 *idr, u32 *err)
{
    u8 *pic = h264bsdNextOutputPicture(s, id, idr, err);
    if (pic != NULL)
    {
        size_t n = (size_t)h264bsdPicWidth(s) * 16 * h264bsdPicHeight(s) * 16 * 3 / 2;
        fwrite(pic, 1, n, gOut);
        /* Deblocking state of THIS picture: h264bsd filters (or not) just
           before handing it over, so the flag now is the flag it used.
           "Tainted" = this picture, or a reference before it since the
           last key frame, was not filtered: then it cannot be bit-exact. */
        if (gIsKey != NULL && gIsKey[gCurSample]) gTainted = 0;
        if (s->skipDeblocking) gTainted = 1;
        if (gSim) gFakeClock += (UInt32)(gSimDecodeUs * (s->skipDeblocking ? 2.0 / 3.0 : 1.0));
        if (gIndex) fprintf(gIndex, "%ld %ld %d %d\n", gCurCall, gCurSample, (int)s->skipDeblocking, gTainted);
        gPicsThisCall++;
        gPicsTotal++;
    }
    return pic;
}
#define h264bsdNextOutputPicture Harness_NextOutputPicture

/* gcc has no "\p" Pascal strings: spell the length byte (16) out. */
#define kLogFallbackName ((ConstStr255Param)"\020H264 Decoder Log")

/* ---- the code under test: the whole file ---- */
#include "H264Codec.c"

#undef h264bsdNextOutputPicture

/* ---- sample file (see extract_samples.py) ---- */
static u32 BE32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static u32 gRand = 1;
static u32 Rand(void) { gRand = gRand * 1664525u + 1013904223u; return gRand >> 8; }

static int DISP_W = 360, DISP_H = 240;   /* --size W H: the movie's display size */
/* A destination pixmap bigger than the picture, with a negative origin and
   the picture placed inside it, like the screen pixmap the Mac logged:
   bounds (-24,-50), picture at (88,24). */
static int PM_W = 512, PM_H = 320;       /* sized from the display size in main() */
#define PM_L   (-24)
#define PM_T   (-50)
#define RECT_L 88
#define RECT_T 24

int main(int argc, char **argv)
{
    FILE *f;
    long  fileSize, pos;
    u8   *file, *avcC;
    u32   avcCSize, count, i, maxSample = 0;
    u8  **sample;
    u32  *sampleSize;
    u8   *isKey;
    const char *mode = "seq", *dstName = "2vuy", *dstOutName = NULL, *indexName = NULL;
    H264GlobalsPtr glob;
    CodecDecompressParams p;
    CodecCapabilities caps;
    ImageDescription desc, *descPtr = &desc;
    ICMFrameTimeRecord ft;
    ComponentResult rc;
    long firstPicCall = -1, noPicCalls = 0, errCalls = 0, strayBytes = 0;
    u32 calls;
    FILE *dstOut = NULL;
    u32 *schedule = NULL, nSchedule = 0;
    int a, clipW = -1, clipH = -1;
    int corruptSeed = 0, corruptPermille = 0;
    int sim = 0, seekAt = -1, seekBack = 0, qtFrameTime = 0, catchUpPermille = 0, keepGoing = 0, preBufFirst = 0;
    ICMFrameTimeInfo sft;
    double simDecodeMs = 0, simBlitMs = 0;
    u8 *dstBuf; long dstRowBytes, dstBpp; int dstFmt;   /* 0 2vuy 1 rgb32 2 rgb16 3 y420 */
    PlanarPixmapInfoYUV420 *ppi = NULL;
    u8 *scratchSample;

    if (argc < 3) { fprintf(stderr, "usage: %s SAMPLES OUT.yuv [options] (see the top of decode_run.c)\n", argv[0]); return 2; }
    for (a = 3; a < argc; a++)
    {
        if      (!strcmp(argv[a], "--mode") && a + 1 < argc) mode = argv[++a];
        else if (!strcmp(argv[a], "--dst") && a + 1 < argc) dstName = argv[++a];
        else if (!strcmp(argv[a], "--dst-out") && a + 1 < argc) dstOutName = argv[++a];
        else if (!strcmp(argv[a], "--index") && a + 1 < argc) indexName = argv[++a];
        else if (!strcmp(argv[a], "--clip") && a + 2 < argc) { clipW = atoi(argv[++a]); clipH = atoi(argv[++a]); }
        else if (!strcmp(argv[a], "--size") && a + 2 < argc) { DISP_W = atoi(argv[++a]); DISP_H = atoi(argv[++a]); }
        else if (!strcmp(argv[a], "--log")) gHostLogExists = 1;
        else if (!strcmp(argv[a], "--ptrfail") && a + 1 < argc) gHostPtrFailAbove = atol(argv[++a]);
        else if (!strcmp(argv[a], "--tempfail")) gHostTempFail = 1;
        else if (!strcmp(argv[a], "--corrupt") && a + 2 < argc) { corruptSeed = atoi(argv[++a]); corruptPermille = atoi(argv[++a]); }
        else if (!strcmp(argv[a], "--sim") && a + 2 < argc) { sim = 1; simDecodeMs = atof(argv[++a]); simBlitMs = atof(argv[++a]); }
        else if (!strcmp(argv[a], "--qt-frametime")) qtFrameTime = 1;
        else if (!strcmp(argv[a], "--keep-going")) keepGoing = 1;
        else if (!strcmp(argv[a], "--prebuf-first")) preBufFirst = 1;
        else if (!strcmp(argv[a], "--catchup") && a + 1 < argc) catchUpPermille = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--seek-at") && a + 2 < argc) { seekAt = atoi(argv[++a]); seekBack = atoi(argv[++a]); }
        else { fprintf(stderr, "unknown option %s\n", argv[a]); return 2; }
    }

    if (clipW < 0) { clipW = DISP_W; clipH = DISP_H; }
    PM_W = ((DISP_W + 152 + 15) / 16) * 16;
    PM_H = ((DISP_H + 80 + 15) / 16) * 16;

    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); fileSize = ftell(f); fseek(f, 0, SEEK_SET);
    file = (u8 *)malloc((size_t)fileSize);
    if (fread(file, 1, (size_t)fileSize, f) != (size_t)fileSize) return 2;
    fclose(f);

    avcCSize = BE32(file); avcC = file + 4; pos = 4 + (long)avcCSize;
    count = BE32(file + pos); pos += 4;
    sample = (u8 **)malloc(count * sizeof(u8 *));
    sampleSize = (u32 *)malloc(count * sizeof(u32));
    isKey = (u8 *)calloc(count, 1);
    for (i = 0; i < count; i++)
    {
        u32 k;
        sampleSize[i] = BE32(file + pos); pos += 4;
        sample[i] = file + pos; pos += (long)sampleSize[i];
        if (sampleSize[i] > maxSample) maxSample = sampleSize[i];
        for (k = 0; k + 4 < sampleSize[i]; )
        {
            u32 n = BE32(sample[i] + k);
            if ((sample[i][k + 4] & 0x1F) == 5) isKey[i] = 1;
            k += 4 + n;
        }
    }
    scratchSample = (u8 *)malloc(maxSample + 16);
    gIsKey = isKey;

    gOut = fopen(argv[2], "wb");
    if (!gOut) { perror(argv[2]); return 2; }
    if (dstOutName) { dstOut = fopen(dstOutName, "wb"); if (!dstOut) { perror(dstOutName); return 2; } }
    if (indexName)  { gIndex = fopen(indexName, "w");   if (!gIndex) { perror(indexName); return 2; } }

    /* ---- the destination ---- */
    memset(&p, 0, sizeof(p));
    memset(&caps, 0, sizeof(caps));
    memset(&desc, 0, sizeof(desc));
    desc.width = DISP_W; desc.height = DISP_H;
    p.imageDescription = &descPtr;
    p.capabilities = &caps;
    p.bufferSize = (int)(preBufFirst ? sampleSize[0] : maxSample);
    p.dstPixMap.bounds.left = PM_L; p.dstPixMap.bounds.top = PM_T;
    p.dstPixMap.bounds.right = PM_L + PM_W; p.dstPixMap.bounds.bottom = PM_T + PM_H;
    p.dstRect.left = RECT_L; p.dstRect.top = RECT_T;
    p.dstRect.right = (short)(RECT_L + clipW); p.dstRect.bottom = (short)(RECT_T + clipH);

    if (!strcmp(dstName, "2vuy"))
    {
        /* The buffer QuickTime 6.0.3 allocated on the Mac: exactly the
           picture, rowBytes 728 (not 720). */
        dstFmt = 0; dstBpp = 2; dstRowBytes = (DISP_W == 360) ? 728 : ((DISP_W * 2 + 15) / 16) * 16;
        p.dstPixMap.bounds.left = 0; p.dstPixMap.bounds.top = 0;
        p.dstPixMap.bounds.right = DISP_W; p.dstPixMap.bounds.bottom = DISP_H;
        p.dstRect.left = 0; p.dstRect.top = 0;
        p.dstRect.right = (short)clipW; p.dstRect.bottom = (short)clipH;
        p.dstPixMap.pixelSize = 16; p.dstPixMap.pixelFormat = k422YpCbCr8CodecType;
    }
    else if (!strcmp(dstName, "rgb32"))
    { dstFmt = 1; dstBpp = 4; dstRowBytes = PM_W * 4; p.dstPixMap.pixelSize = 32; p.dstPixMap.pixelFormat = 0; }
    else if (!strcmp(dstName, "rgb16"))
    { dstFmt = 2; dstBpp = 2; dstRowBytes = PM_W * 2; p.dstPixMap.pixelSize = 16; p.dstPixMap.pixelFormat = 16; }
    else if (!strcmp(dstName, "y420"))
    { dstFmt = 3; dstBpp = 1; dstRowBytes = PM_W; p.dstPixMap.pixelSize = 12; p.dstPixMap.pixelFormat = kYUV420CodecType; }
    else { fprintf(stderr, "unknown --dst %s\n", dstName); return 2; }

    dstBuf = (u8 *)malloc((size_t)PM_W * 4 * PM_H + 64);
    if (dstFmt == 3)
    {
        /* planar: a header, then Y, Cb, Cr planes with their own strides */
        ppi = (PlanarPixmapInfoYUV420 *)dstBuf;
        ppi->componentInfoY.offset  = 64;                         ppi->componentInfoY.rowBytes  = PM_W;
        ppi->componentInfoCb.offset = 64 + PM_W * PM_H;           ppi->componentInfoCb.rowBytes = PM_W / 2;
        ppi->componentInfoCr.offset = 64 + PM_W * PM_H * 5 / 4;   ppi->componentInfoCr.rowBytes = PM_W / 2;
    }
    p.dstPixMap.baseAddr = (Ptr)dstBuf;
    p.dstPixMap.rowBytes = (short)(0x8000 | dstRowBytes);

    /* ---- Open and PreDecompress, as QuickTime calls them ---- */
    gHostAvcC = avcC; gHostAvcCSize = (long)avcCSize;
    if (H264CodecOpen(NULL, NULL) != noErr) { fprintf(stderr, "Open failed\n"); return 1; }
    glob = (H264GlobalsPtr)gHostInstanceStorage;
    rc = H264CodecPreDecompress(glob, &p);
    if (rc != noErr && keepGoing)
        fprintf(stderr, "[harness] PreDecompress returned %d; calling BandDecompress anyway (--keep-going)\n", (int)rc);
    else if (rc != noErr)
    {
        fprintf(stderr, "[harness] PreDecompress failed: %d\n", (int)rc);
        H264CodecClose(glob, NULL);
        printf("predecompress-failed %d live %ld\n", (int)rc, gHostLiveBlocks);
        return gHostLiveBlocks == 0 ? 0 : 1;
    }

    if (strncmp(mode, "list:", 5) == 0)
    {
        FILE *lf = fopen(mode + 5, "r");
        unsigned long v;
        if (!lf) { perror(mode + 5); return 2; }
        schedule = (u32 *)malloc(count * 4 * sizeof(u32));
        while (nSchedule < count * 4 && fscanf(lf, "%lu", &v) == 1)
        {
            if (v >= count) { fprintf(stderr, "sample %lu out of range\n", v); return 2; }
            schedule[nSchedule++] = (u32)v;
        }
        fclose(lf);
    }
    if (sim && seekAt >= 0)
    {
        /* build: 0..seekAt, then from the key frame at or before
           (seekAt - seekBack) ... no: jump FORWARD is what catches up.
           Play 0..seekAt, then jump to target = seekAt + seekBack: replay
           from the key frame at or before target up to target, then go on. */
        u32 target = (u32)(seekAt + seekBack), key, k;
        if (target >= count) target = count - 1;
        for (key = target; key > 0 && !isKey[key]; key--) ;
        schedule = (u32 *)malloc(count * 2 * sizeof(u32));
        for (k = 0; k <= (u32)seekAt; k++) schedule[nSchedule++] = k;
        for (k = key; k < count; k++) schedule[nSchedule++] = k;
    }
    calls = schedule != NULL ? nSchedule :
            (strcmp(mode, "poster") == 0) ? 1 : count + (strcmp(mode, "twice") == 0 ? 1 : 0);

    gRand = (u32)corruptSeed * 2654435761u + 12345u;
    if (sim) { gFakeClockAuto = 0; memset(&ft, 0, sizeof(ft)); gSim = 1; gSimDecodeUs = simDecodeMs * 1000.0; gSimBlitUs = simBlitMs * 1000.0; }

    {
        double t = 0.0, maxLag = 0.0, lag = 0.0, timeShift = 0.0;
        long   painted = 0, decoded = 0, dropped = 0;
        const double fps = 30000.0 / 1001.0;
        u32    jumpTarget = 0; int jumping = 0;

        for (i = 0; i < calls; i++)
        {
            u32 s = schedule != NULL ? schedule[i] :
                    (strcmp(mode, "twice") == 0) ? (i == 0 ? 0 : i - 1) : i;
            unsigned long blitsBefore, picsBefore;
            const u8 *data = sample[s];
            u32 size = sampleSize[s];

            if (corruptPermille > 0 && (int)(Rand() % 1000) < corruptPermille)
            {
                u32 kind = Rand() % 4, k, n;
                memcpy(scratchSample, sample[s], size);
                data = scratchSample;
                if (kind == 0) { n = 1 + Rand() % 8; for (k = 0; k < n; k++) scratchSample[Rand() % size] ^= (u8)(1u << (Rand() % 8)); }
                else if (kind == 1) { size = 1 + Rand() % size; }                       /* truncated */
                else if (kind == 2) { n = Rand() % size; for (k = n; k < size && k < n + 64; k++) scratchSample[k] = (u8)Rand(); }
                else { scratchSample[Rand() % 4] ^= 0x40; }                             /* broken length prefix */
            }

            p.conditionFlags = (catchUpPermille > 0 && (int)(Rand() % 1000) < catchUpPermille) ?
                               (int)codecConditionCatchUpDiff : 0;
            p.data = (Ptr)data;
            p.bufferSize = (int)size;
            p.frameNumber = (int)s;
            gPicsThisCall = 0; gCurCall = (long)i; gCurSample = (long)s;

            if (sim)
            {
                /* Movie time of sample s. After a jump the movie clock is
                   shifted so that the TARGET sample is due "now". */
                double due = (double)s / fps + timeShift, start;
                if (seekAt >= 0 && (long)i == (long)seekAt + 1)
                {
                    jumpTarget = (u32)(seekAt + seekBack); if (jumpTarget >= count) jumpTarget = count - 1;
                    timeShift = t - (double)jumpTarget / fps;   /* target is due right now */
                    due = (double)s / fps + timeShift;
                    jumping = 1;
                }
                start = (t > due) ? t : due;      /* QuickTime waits until the frame is due */
                lag = start - due;
                if (!jumping && lag > maxLag) maxLag = lag;
                if (jumping && s >= jumpTarget) jumping = 0;
                gHostMovieSeconds = start; gFakeClock = (UInt32)(start * 1e6);
                ft.value.hi = 0; ft.value.lo = (UInt32)(due * 30000.0 + 0.5);
                ft.scale = 30000; ft.base = (void *)&ft; ft.duration = 1001; ft.rate = 0x10000;
                ft.recordSize = (int)sizeof(ft); ft.frameNumber = (int)s;
                sft.startTime.hi = 0; sft.startTime.lo = (UInt32)((double)s * 1001.0 + 0.5);
                sft.scale = 30000; sft.duration = 1001;
                p.frameTime = qtFrameTime ? &ft : NULL;
                p.syncFrameTime = &sft;
                t = start;
            }

            if (dstOut != NULL) memset(dstBuf + (dstFmt == 3 ? 64 : 0), 0xAB, (size_t)PM_W * 4 * PM_H);  /* "not painted" */
            blitsBefore = glob->statBlitY420 + glob->statBlit2vuy + glob->statBlitRGB32 + glob->statBlitRGB16;
            picsBefore  = glob->picsOut;
            gDeblockOffNow = glob->noDeblock;

            rc = H264CodecBandDecompress(glob, &p);

            if (sim)
            {
                /* The codec's own clock advanced by what it did (see
                   HostSimBlit and the capture wrapper); QuickTime's next
                   call can come no earlier than that. */
                int didDecode = (glob->picsOut != picsBefore);
                int didBlit   = (glob->statBlitY420 + glob->statBlit2vuy + glob->statBlitRGB32 + glob->statBlitRGB16) != blitsBefore;
                if (didDecode) decoded++; else dropped++;
                if (didBlit) painted++;
                t = (double)gFakeClock / 1e6 + 0.00005;
            }

            if (dstOut != NULL)
            {
                int row;
                if (dstFmt == 3)
                {
                    u8 *y = dstBuf + ppi->componentInfoY.offset, *cb = dstBuf + ppi->componentInfoCb.offset, *cr = dstBuf + ppi->componentInfoCr.offset;
                    int ox = RECT_L - PM_L, oy = RECT_T - PM_T;
                    for (row = 0; row < clipH; row++) fwrite(y + (oy + row) * PM_W + ox, 1, (size_t)clipW, dstOut);
                    for (row = 0; row < clipH / 2; row++) fwrite(cb + (oy / 2 + row) * (PM_W / 2) + ox / 2, 1, (size_t)(clipW / 2), dstOut);
                    for (row = 0; row < clipH / 2; row++) fwrite(cr + (oy / 2 + row) * (PM_W / 2) + ox / 2, 1, (size_t)(clipW / 2), dstOut);
                }
                else
                {
                    long ox = p.dstRect.left - p.dstPixMap.bounds.left, oy = p.dstRect.top - p.dstPixMap.bounds.top;
                    for (row = 0; row < clipH; row++)
                        fwrite(dstBuf + (oy + row) * dstRowBytes + ox * dstBpp, 1, (size_t)(clipW * dstBpp), dstOut);
                }
            }
            if (dstOut != NULL && dstFmt != 3)
            {
                /* Nothing outside the destination rectangle may be touched.
                   (The '2vuy' writer finishes an odd last column as a whole
                   pixel pair, two bytes past the rectangle, as v13 did.) */
                long ox = (p.dstRect.left - p.dstPixMap.bounds.left) * dstBpp, oy = p.dstRect.top - p.dstPixMap.bounds.top;
                long x1 = ox + clipW * dstBpp + ((dstFmt == 0 && (clipW & 1)) ? 2 : 0), r, x;
                long rows = (dstFmt == 0) ? DISP_H : PM_H;
                for (r = 0; r < rows; r++)
                    for (x = 0; x < dstRowBytes; x++)
                        if (!(r >= oy && r < oy + clipH && x >= ox && x < x1) && dstBuf[r * dstRowBytes + x] != 0xAB)
                            strayBytes++;
            }
            if (rc != noErr) errCalls++;
            if ((i < 2 && gHostLogExists) || rc != noErr)
                fprintf(stderr, "[harness] call %lu (sample %lu, %lu bytes): rc %d, pictures out %ld\n",
                        (unsigned long)i, (unsigned long)s, (unsigned long)size, (int)rc, gPicsThisCall);
            if (i == 0 && strcmp(mode, "twice") == 0) gFakeClock += 20000000UL;  /* 20 s pause */
            if (gPicsThisCall > 0 && firstPicCall < 0) firstPicCall = (long)i;
            if (gPicsThisCall == 0) noPicCalls++;
        }

        if (sim)
            printf("sim decode %.1f ms blit %.1f ms: calls %lu decoded %ld painted %ld not-decoded %ld | "
                   "final lag %.0f ms, worst lag %.0f ms | late %lu, blit skips %lu, deblock offs %lu, "
                   "pictures without deblocking %lu, key-frame waits %lu, dropped %lu, nonref skips %lu\n",
                   simDecodeMs, simBlitMs, (unsigned long)calls, decoded, painted, dropped,
                   lag * 1000.0, maxLag * 1000.0,
                   glob->statLateCalls, glob->statBlitSkips, glob->statDeblockOffs,
                   glob->statNoDeblockPics, glob->statIDRWaits, glob->statDroppedCalls, glob->statNonRefSkips);
    }

    {
        unsigned long errs = glob->statDecodeErrors;
        long ptrA = gHostPtrAllocs, tempA = gHostTempAllocs, peak = gHostPeakBytes, fileCalls, clockReads;
        fclose(gOut);
        if (dstOut != NULL) fclose(dstOut);
        if (gIndex) fclose(gIndex);
        fileCalls = gHostFileCalls; clockReads = gHostClockReads;
        H264CodecClose(glob, NULL);
        printf("%s calls %lu pictures %ld firstpic %ld nopic %ld rc-errors %ld decode-errors %lu | "
               "mem: ptr %ld temp %ld peak %ld bytes, live after Close %ld | file-calls %ld clock-reads %ld | stray-bytes %ld\n",
               mode, (unsigned long)calls, gPicsTotal, firstPicCall, noPicCalls, errCalls, errs,
               ptrA, tempA, peak, gHostLiveBlocks, fileCalls, clockReads, strayBytes);
    }
    return (gHostLiveBlocks == 0 && strayBytes == 0) ? 0 : 1;
}

/* The decoder's memory hooks are implemented in H264Codec.c (H264MemAlloc,
   H264MemFree) on top of the counted NewPtr / TempNewHandle above. */
