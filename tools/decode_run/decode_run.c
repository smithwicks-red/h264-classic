/*
    decode_run.c -- RUN the codec's real decode path on the host.

    tools/ctx/ctx_test.c only type-checks the fragments of H264Codec.c.
    This harness executes them: the verbatim BandDecompressWork loop,
    DrainOutputPictures, BlitPicture (into a '2vuy' buffer, the format
    QuickTime 6.0.3 actually chose), FeedParameterSets and EnsureDecoder,
    all linked against the real h264bsd and fed the real sample bytes of
    a movie, one QuickTime sample per BandDecompress call, exactly as the
    ICM delivers them.

    Every picture h264bsd hands to DrainOutputPictures is captured (by
    wrapping h264bsdNextOutputPicture) and written to a raw .yuv file so
    that a script can compare it with ffmpeg bit for bit.

    Why this exists: the Mac showed a blank poster frame, and reading the
    loop suggested the first call on a fresh decoder never decodes its
    IDR. That claim deserves a measurement on the real code, not a
    re-reading of it -- so this builds the SAME fragments from the old
    and the new H264Codec.c and runs both.

    Build (see run_ab.sh):
        python3 extract_run.py ../../codec-src/H264Codec.c frags_new
        gcc -std=gnu89 -O1 -Ifrags_new -I<decoder-src> -I../../codec-src \
            decode_run.c <decoder-src>/h264bsd_*.c ../../codec-src/avcC_parse.c

    Usage:
        decode_run SAMPLES OUT.yuv [seq|poster|twice] [DST.2vuy]

      DST.2vuy, if given, receives the visible 360x240 '2vuy' destination
      AFTER EVERY CALL (the buffer is filled with 0xAB before each call),
      so a script can check that each call painted the right frame --
      including calls where h264bsd produced no new picture.

      seq     every sample once, in order, on one decoder (normal play)
      poster  ONLY the first sample, on a fresh decoder: what QuickTime
              does to draw the poster frame when a movie opens
      twice   the first sample twice, then the rest: the pattern the
              Mac log suggests QuickTime used, which is what made the old
              loop look correct during playback
      list:F  the samples named in text file F (one index per line), in
              that order: a playback in which QuickTime SKIPPED samples,
              as the Pismo's Millions run showed it does when it falls
              behind (see drop_sim.py)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basetype.h"
#include "h264bsd_decoder.h"
#include "avcC_parse.h"

/* ---- Mac basics (32-bit PowerPC widths where it matters) ---- */
#define pascal
typedef unsigned char   Boolean;
#define false 0
#define true  1
typedef unsigned int    OSType;
typedef short           OSErr;
typedef int             ComponentResult;
typedef void           *ComponentInstance;
typedef char           *Ptr;
typedef Ptr            *Handle;
typedef int             Size;
typedef int             SInt32;
typedef unsigned int    UInt32;
typedef unsigned char   Str255[256];
typedef const unsigned char *ConstStr255Param;

#define noErr                   0
#define paramErr              (-50)
#define fnfErr                (-43)
#define memFullErr           (-108)
#define badFormat           (-2002)
#define internalComponentErr (-2070)

/* ---- File Manager / Folder Manager / Timer: host-backed stubs ---- */
typedef struct { short vRefNum; long parID; Str255 name; } FSSpec;
#define fsWrPerm     2
#define fsFromStart  1
#define fsFromLEOF   2
#define kOnSystemDisk      ((short)0x8000)
#define kDesktopFolderType 0x6465736bu
#define kDontCreateFolder  0
typedef struct { UInt32 hi; UInt32 lo; } UnsignedWide;

static FILE *gRef[8];

static OSErr FSMakeFSSpec(short v, long d, ConstStr255Param name, FSSpec *spec)
{
    (void)v; (void)d;
    memset(spec, 0, sizeof(*spec));
    memcpy(spec->name, name, (size_t)name[0] + 1);
    return noErr;
}
static OSErr FSpCreate(const FSSpec *s, OSType c, OSType t, short script)
{ (void)s; (void)c; (void)t; (void)script; return noErr; }
static OSErr FSpOpenDF(const FSSpec *s, char perm, short *refNum)
{
    char host[300];
    int  n = s->name[0], i, slot;
    (void)perm;
    for (slot = 1; slot < 8 && gRef[slot] != NULL; slot++) ;
    if (slot == 8) return -42;
    if (n > 5 && memcmp(s->name + 1, "share:h264log", 13) == 0)
        gRef[slot] = stderr;                 /* the codec's log -> console */
    else
    {
        strcpy(host, "dump_");
        for (i = 0; i < n; i++)
            host[5 + i] = (char)(s->name[1 + i] == ':' ? '_' : s->name[1 + i]);
        host[5 + n] = '\0';
        gRef[slot] = fopen(host, "wb");
        if (gRef[slot] == NULL) return -36;
    }
    *refNum = (short)slot;
    return noErr;
}
static OSErr SetFPos(short r, short mode, long off) { (void)r; (void)mode; (void)off; return noErr; }
static OSErr SetEOF(short r, long eof) { (void)r; (void)eof; return noErr; }
static OSErr FSWrite(short r, long *count, const void *buf)
{
    if (gRef[r] == stderr)
    {
        long i; const char *p = (const char *)buf;
        for (i = 0; i < *count; i++) fputc(p[i] == '\r' ? '\n' : p[i], stderr);
    }
    else fwrite(buf, 1, (size_t)*count, gRef[r]);
    return noErr;
}
static OSErr FSClose(short r)
{
    if (gRef[r] != stderr && gRef[r] != NULL) fclose(gRef[r]);
    gRef[r] = NULL;
    return noErr;
}
static OSErr FlushVol(ConstStr255Param vol, short v) { (void)vol; (void)v; return noErr; }
static OSErr FindFolder(short v, OSType t, Boolean c, short *fv, long *fd)
{ (void)v; (void)t; (void)c; *fv = 0; *fd = 0; return noErr; }
/* A fake clock: 1 ms per reading, plus whatever the harness adds to
   simulate a pause (see "twice" mode, which pauses 20 s after the poster
   frame the way a person does before pressing Play). */
static UInt32 gFakeClock;
static void Microseconds(UnsignedWide *t) { t->hi = 0; t->lo = (gFakeClock += 1000); }

/* ---- QuickDraw / ICM (field order from UI 3.4.1, as in ctx_test.c) ---- */
typedef struct { short top, left, bottom, right; } Rect;
typedef struct {
    Ptr     baseAddr;
    short   rowBytes;
    Rect    bounds;
    short   pmVersion, packType;
    int     packSize;
    int     hRes, vRes;
    short   pixelType, pixelSize, cmpCount, cmpSize;
    OSType  pixelFormat;
    void   *pmTable;
    void   *pmExt;
} PixMap;
typedef struct {
    int flags; short wantedPixelSize, extendWidth, extendHeight, bandMin, bandInc, pad;
    unsigned int time; int flags2;
} CodecCapabilities;
typedef struct { short width, height; } ImageDescription;
typedef ImageDescription *ImageDescriptionPtr;
typedef ImageDescriptionPtr *ImageDescriptionHandle;
typedef struct {
    ImageDescriptionHandle  imageDescription;
    Ptr                     data;
    int                     bufferSize;
    CodecCapabilities      *capabilities;
    PixMap                  dstPixMap;
    Rect                    dstRect;
    OSType                **wantedDestinationPixelTypes;
} CodecDecompressParams;
typedef struct { SInt32 offset; UInt32 rowBytes; } PlanarComponentInfo;
typedef struct {
    PlanarComponentInfo componentInfoY, componentInfoCb, componentInfoCr;
} PlanarPixmapInfoYUV420;
#define kYUV420CodecType      0x79343230u
#define k422YpCbCr8CodecType  0x32767579u
#define k32ARGBPixelFormat    0x00000020u

/* ---- Memory Manager on malloc ---- */
static void   HLock(Handle h) { (void)h; }
static void   HUnlock(Handle h) { (void)h; }
static Ptr    NewPtr(Size n) { return (Ptr)malloc((size_t)n); }
static Ptr    NewPtrClear(Size n) { return (Ptr)calloc(1, (size_t)n); }
static void   DisposePtr(Ptr p) { free(p); }
static Handle NewHandleClear(Size n) { Handle h = (Handle)malloc(sizeof(Ptr)); *h = NewPtrClear(n); return h; }
static void   DisposeHandle(Handle h) { if (h) { free(*h); free(h); } }
static void   BlockMoveData(const void *s, void *d, Size n) { memmove(d, s, (size_t)n); }

/* ---- capture every picture the codec receives from h264bsd ---- */
static FILE *gOut;
static long  gPicsThisCall, gPicsTotal;

static u8 *Harness_NextOutputPicture(storage_t *s, u32 *id, u32 *idr, u32 *err)
{
    u8 *pic = h264bsdNextOutputPicture(s, id, idr, err);
    if (pic != NULL)
    {
        size_t n = (size_t)h264bsdPicWidth(s) * 16 * h264bsdPicHeight(s) * 16 * 3 / 2;
        fwrite(pic, 1, n, gOut);
        gPicsThisCall++;
        gPicsTotal++;
    }
    return pic;
}

/* ---- the code under test, verbatim, in the real file's order ---- */
#include "frag_config.inc"
#include "frag_globals.inc"
#include "frag_feed.inc"
static void CopyPlanarYUVToPixMap(H264GlobalsPtr g, const u8 *yuv, CodecDecompressParams *p)
{ (void)g; (void)yuv; (void)p; }   /* 32-bit path not exercised: dst is '2vuy' */
#include "frag_yuv.inc"
#include "frag_loggers.inc"
#include "frag_ensure.inc"
#define h264bsdNextOutputPicture Harness_NextOutputPicture
#include "frag_drain.inc"
#undef h264bsdNextOutputPicture
#include "frag_band.inc"

/* ---- sample file (see extract_samples.py) ---- */
static u32 BE32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

int main(int argc, char **argv)
{
    FILE *f;
    long  fileSize, pos;
    u8   *file, *avcC;
    u32   avcCSize, count, i, maxSample = 0;
    u8  **sample;
    u32  *sampleSize;
    const char *mode = argc > 3 ? argv[3] : "seq";
    H264GlobalsPtr glob;
    CodecDecompressParams p;
    ImageDescription desc, *descPtr = &desc;
    static u8 dst2vuy[728 * 240];   /* rowBytes 728, as QuickTime allocated it -- NOT 360*2 */
    ComponentResult rc;
    long firstPicCall = -1, noPicCalls = 0;
    u32 calls;
    FILE *dstOut = NULL;
    u32 *schedule = NULL, nSchedule = 0;

    if (argc < 3) { fprintf(stderr, "usage: %s SAMPLES OUT.yuv [seq|poster|twice|list:FILE]\n", argv[0]); return 2; }
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
    for (i = 0; i < count; i++)
    {
        sampleSize[i] = BE32(file + pos); pos += 4;
        sample[i] = file + pos; pos += (long)sampleSize[i];
        if (sampleSize[i] > maxSample) maxSample = sampleSize[i];
    }

    gOut = fopen(argv[2], "wb");
    if (!gOut) { perror(argv[2]); return 2; }
    if (argc > 4)
    {
        dstOut = fopen(argv[4], "wb");
        if (!dstOut) { perror(argv[4]); return 2; }
    }

    /* What PreDecompress does, minus the ICM: build the decoder, feed
       the avcC parameter sets, size the Annex-B scratch buffer. */
    glob = (H264GlobalsPtr)calloc(1, sizeof(H264Globals));
    if (EnsureDecoder(glob) != noErr) { fprintf(stderr, "EnsureDecoder failed\n"); return 1; }
    if (!FeedParameterSets(glob, avcC, avcCSize)) { fprintf(stderr, "FeedParameterSets failed\n"); return 1; }
    glob->annexBScratchSize = AVCC_WorstCaseAnnexBSize(maxSample);
    glob->annexBScratch = (u8 *)malloc(glob->annexBScratchSize);

    /* The destination QuickTime 6.0.3 chose for playback on the Mac:
       an offscreen '2vuy' buffer, 360x240, rowBytes 728 (logged). */
    memset(&p, 0, sizeof(p));
    desc.width = 360; desc.height = 240;
    p.imageDescription = &descPtr;
    p.dstPixMap.baseAddr = (Ptr)dst2vuy;
    p.dstPixMap.rowBytes = (short)(0x8000 | 728);
    p.dstPixMap.bounds.right = 360; p.dstPixMap.bounds.bottom = 240;
    p.dstPixMap.pixelSize = 16;
    p.dstPixMap.pixelFormat = k422YpCbCr8CodecType;
    p.dstRect.right = 360; p.dstRect.bottom = 240;

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
    calls = schedule != NULL ? nSchedule :
            (strcmp(mode, "poster") == 0) ? 1 : count + (strcmp(mode, "twice") == 0 ? 1 : 0);
    for (i = 0; i < calls; i++)
    {
        u32 s = schedule != NULL ? schedule[i] :
                (strcmp(mode, "twice") == 0) ? (i == 0 ? 0 : i - 1) : i;
        p.data = (Ptr)sample[s];
        p.bufferSize = (int)sampleSize[s];
        gPicsThisCall = 0;
        memset(dst2vuy, 0xAB, sizeof(dst2vuy));     /* "not painted" sentinel */
        rc = H264CodecBandDecompress(glob, &p);
        if (dstOut != NULL)
        {
            int row;
            for (row = 0; row < 240; row++) fwrite(dst2vuy + row * 728, 1, 720, dstOut);
        }
        if (i < 4 || rc != noErr)
            fprintf(stderr, "[harness] call %lu (sample %lu, %lu bytes): rc %d, pictures out %ld\n",
                    (unsigned long)i, (unsigned long)s, (unsigned long)sampleSize[s], (int)rc, gPicsThisCall);
        if (i == 0 && strcmp(mode, "twice") == 0) gFakeClock += 20000000UL;  /* 20 s pause */
        if (gPicsThisCall > 0 && firstPicCall < 0) firstPicCall = (long)i;
        if (gPicsThisCall == 0) noPicCalls++;
    }
    fclose(gOut);
    if (dstOut != NULL) fclose(dstOut);
    LogPlaybackStats();          /* the codec's own report, to stderr */
    fprintf(stderr, "[harness] mode %s: %lu calls, %ld pictures, first picture on call %ld, %ld calls produced none\n",
            mode, (unsigned long)calls, gPicsTotal, firstPicCall, noPicCalls);
    printf("%s %lu %ld %ld %ld\n", mode, (unsigned long)calls, gPicsTotal, firstPicCall, noPicCalls);
    return 0;
}
