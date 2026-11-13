/*
    host_mac.h -- the small part of the Mac Toolbox that H264Codec.c uses,
    re-made on a Unix host so that the WHOLE of H264Codec.c can be compiled
    and RUN there (tools/host/decode_run.c).

    Up to v13 the harness cut fragments out of H264Codec.c by searching for
    marker lines. Since v14 H264Codec.c is included complete, with
    H264_HOST_BUILD defined; that switch only leaves out the Mac #includes
    at the top and the Component Manager dispatch glue at the bottom.

    Field ORDER and NAMES follow Universal Interfaces 3.4.1 for everything
    the codec touches. Field WIDTHS follow the Mac where the codec's
    arithmetic could notice (32-bit wide halves, 16-bit shorts). Layout
    compatibility with a real Mac is not needed and not claimed: nothing
    here crosses a real QuickTime boundary.

    The stand-ins also instrument what a real Mac cannot show us:
      - every Memory Manager block is counted, so a leak is a test failure
      - NewPtr can be told to refuse big blocks, to drive the
        temporary-memory fallback
      - the log file "exists" or not on command
      - the clock and the movie time base are under the harness's control
*/
#ifndef HOST_MAC_H
#define HOST_MAC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define pascal
typedef unsigned char   Boolean;
#ifndef false
#define false 0
#define true  1
#endif
typedef unsigned int    OSType;
typedef short           OSErr;
typedef int             ComponentResult;
typedef void           *ComponentInstance;
typedef char           *Ptr;
typedef Ptr            *Handle;
typedef long            Size;
typedef int             SInt32;
typedef unsigned int    UInt32;
typedef signed char     SInt8;
typedef unsigned short  UInt16;
typedef int             Fixed;
typedef unsigned char   Str255[256];
typedef const unsigned char *ConstStr255Param;

#define noErr                   0
#define paramErr              (-50)
#define fnfErr                (-43)
#define memFullErr           (-108)
#define badFormat           (-2002)
#define internalComponentErr (-2070)

/* ---------------- File Manager / Folder Manager ---------------- */
typedef struct { short vRefNum; long parID; Str255 name; } FSSpec;
#define fsWrPerm     2
#define fsFromStart  1
#define fsFromLEOF   2
#define kOnSystemDisk      ((short)0x8000)
#define kDesktopFolderType 0x6465736bu
#define kDontCreateFolder  0

static int   gHostLogExists = 0;     /* does "H264 Decoder Log" exist? */
static long  gHostFileCalls = 0;     /* every File Manager call the codec makes */
static FILE *gHostLogOut    = NULL;  /* where log text goes (stderr by default) */
static FILE *gRef[8];

static OSErr FSMakeFSSpec(short v, long d, ConstStr255Param name, FSSpec *spec)
{
    int isLog;
    (void)v; (void)d;
    gHostFileCalls++;
    memset(spec, 0, sizeof(*spec));
    memcpy(spec->name, name, (size_t)name[0] + 1);
    isLog = (name[0] >= 13 && memcmp(name + 1, "share:h264log", 13) == 0) ||
            (name[0] == 16 && memcmp(name + 1, "H264 Decoder Log", 16) == 0);
    if (isLog) return (OSErr)(gHostLogExists ? noErr : fnfErr);
    return fnfErr;      /* dump files: "not there yet" */
}
static OSErr FSpCreate(const FSSpec *s, OSType c, OSType t, short script)
{ (void)s; (void)c; (void)t; (void)script; gHostFileCalls++; return noErr; }
static OSErr FSpOpenDF(const FSSpec *s, char perm, short *refNum)
{
    char host[300];
    int  n = s->name[0], i, slot;
    (void)perm;
    gHostFileCalls++;
    for (slot = 1; slot < 8 && gRef[slot] != NULL; slot++) ;
    if (slot == 8) return -42;
    if ((n >= 13 && memcmp(s->name + 1, "share:h264log", 13) == 0) ||
        (n == 16 && memcmp(s->name + 1, "H264 Decoder Log", 16) == 0))
    {
        if (!gHostLogExists) return fnfErr;
        gRef[slot] = gHostLogOut ? gHostLogOut : stderr;
    }
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
    FILE *logf = gHostLogOut ? gHostLogOut : stderr;
    if (gRef[r] == logf)
    {
        long i; const char *p = (const char *)buf;
        for (i = 0; i < *count; i++) fputc(p[i] == '\r' ? '\n' : p[i], logf);
    }
    else fwrite(buf, 1, (size_t)*count, gRef[r]);
    return noErr;
}
static OSErr FSClose(short r)
{
    FILE *logf = gHostLogOut ? gHostLogOut : stderr;
    if (gRef[r] != logf && gRef[r] != NULL) fclose(gRef[r]);
    gRef[r] = NULL;
    return noErr;
}
static OSErr FlushVol(ConstStr255Param vol, short v) { (void)vol; (void)v; return noErr; }
static OSErr FindFolder(short v, OSType t, Boolean c, short *fv, long *fd)
{ (void)v; (void)t; (void)c; gHostFileCalls++; *fv = 0; *fd = 0; return noErr; }

/* ---------------- Timer ---------------- */
typedef struct { UInt32 hi; UInt32 lo; } UnsignedWide;
typedef struct { SInt32 hi; UInt32 lo; } wide;
/* A fake microsecond clock. Outside "sim" mode it advances 1 ms per
   reading; in sim mode the harness sets it. */
static UInt32 gFakeClock;
static int    gFakeClockAuto = 1;
static long   gHostClockReads = 0;
static void Microseconds(UnsignedWide *t)
{
    gHostClockReads++;
    if (gFakeClockAuto) gFakeClock += 1000;
    t->hi = 0; t->lo = gFakeClock;
}

/* ---------------- Movie time base ---------------- */
typedef void *TimeBase;
typedef int   TimeScale;
typedef int   TimeValue;
typedef struct { wide value; TimeScale scale; TimeBase base; } TimeRecord;
static double gHostMovieSeconds;     /* the time base's current time */
static TimeValue GetTimeBaseTime(TimeBase tb, TimeScale s, TimeRecord *tr)
{
    double v = gHostMovieSeconds * (double)s;
    (void)tb;
    tr->value.hi = 0;
    tr->value.lo = (UInt32)(v + 0.5);
    tr->scale = s;
    tr->base = tb;
    return (TimeValue)tr->value.lo;
}

/* ---------------- QuickDraw / ICM ---------------- */
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
typedef struct { int idSize; OSType cType; short width, height; } ImageDescription;
typedef ImageDescription *ImageDescriptionPtr;
typedef ImageDescriptionPtr *ImageDescriptionHandle;
typedef struct {
    wide    value;
    int     scale;
    void   *base;
    int     duration;
    Fixed   rate;
    int     recordSize;
    int     frameNumber;
    int     flags;
    wide    virtualStartTime;
    int     virtualDuration;
} ICMFrameTimeRecord;
typedef ICMFrameTimeRecord *ICMFrameTimePtr;
typedef struct { wide startTime; int scale; int duration; } ICMFrameTimeInfo;
typedef ICMFrameTimeInfo *ICMFrameTimeInfoPtr;
typedef struct {
    ImageDescriptionHandle  imageDescription;
    Ptr                     data;
    int                     bufferSize;
    int                     frameNumber;
    int                     conditionFlags;
    int                     callerFlags;
    CodecCapabilities      *capabilities;
    PixMap                  dstPixMap;
    ICMFrameTimePtr         frameTime;
    ICMFrameTimeInfoPtr     syncFrameTime;
    Rect                    dstRect;
    OSType                **wantedDestinationPixelTypes;
} CodecDecompressParams;
#define codecConditionCatchUpDiff (1L << 14)
typedef struct { SInt32 offset; UInt32 rowBytes; } PlanarComponentInfo;
typedef struct {
    PlanarComponentInfo componentInfoY, componentInfoCb, componentInfoCr;
} PlanarPixmapInfoYUV420;
#define kYUV420CodecType      0x79343230u
#define k422YpCbCr8CodecType  0x32767579u
#define k32ARGBPixelFormat    0x00000020u

typedef struct {
    Str255 typeName;
    short  version, revisionLevel;
    int    vendor;
    int    decompressFlags, compressFlags, formatFlags;
    unsigned char compressionAccuracy, decompressionAccuracy;
    unsigned short compressionSpeed, decompressionSpeed;
    unsigned char compressionLevel, resvd;
    short  minimumHeight, minimumWidth;
    short  decompressPipelineLatency, compressPipelineLatency;
    int    privateData;
} CodecInfo;
#define codecInfoDoes32        (1L << 4)
#define codecInfoDoesTemporal  (1L << 9)
#define codecInfoDepth24       (1L << 4)
#define codecInfoDepth32       (1L << 5)

/* ---------------- Memory Manager, counted ---------------- */
typedef struct { Ptr data; long size; int temp; } HostMaster;   /* a Handle points at .data */

static long gHostLiveBlocks, gHostLiveBytes, gHostPeakBytes;
static long gHostPtrAllocs, gHostTempAllocs, gHostPtrFails;
static long gHostPtrFailAbove = -1;    /* NewPtr refuses requests larger than this (-1 = never) */
static long gHostTempFail     = 0;     /* 1 = temporary memory refuses too */

static void HostCount(long bytes)
{
    gHostLiveBlocks++; gHostLiveBytes += bytes;
    if (gHostLiveBytes > gHostPeakBytes) gHostPeakBytes = gHostLiveBytes;
}
static Ptr NewPtr(Size n)
{
    long *p;
    if (gHostPtrFailAbove >= 0 && (long)n > gHostPtrFailAbove) { gHostPtrFails++; return NULL; }
    p = (long *)malloc((size_t)n + 16);
    if (p == NULL) return NULL;
    p[0] = (long)n;
    gHostPtrAllocs++; HostCount((long)n);
    return (Ptr)p + 16;
}
static Ptr NewPtrClear(Size n) { Ptr p = NewPtr(n); if (p) memset(p, 0, (size_t)n); return p; }
static void DisposePtr(Ptr p)
{
    long *q;
    if (p == NULL) return;
    q = (long *)(p - 16);
    gHostLiveBlocks--; gHostLiveBytes -= q[0];
    free(q);
}
static Handle HostNewHandle(Size n, int temp, int clear)
{
    HostMaster *m = (HostMaster *)malloc(sizeof(HostMaster));
    m->data = (Ptr)(clear ? calloc(1, (size_t)n + 1) : malloc((size_t)n + 1));
    m->size = (long)n; m->temp = temp;
    HostCount((long)n);
    return (Handle)m;
}
static Handle NewHandleClear(Size n) { return HostNewHandle(n, 0, 1); }
static Handle TempNewHandle(Size n, OSErr *err)
{
    if (gHostTempFail) { *err = memFullErr; return NULL; }
    *err = noErr; gHostTempAllocs++;
    return HostNewHandle(n, 1, 0);
}
static void DisposeHandle(Handle h)
{
    HostMaster *m = (HostMaster *)h;
    if (h == NULL) return;
    gHostLiveBlocks--; gHostLiveBytes -= m->size;
    free(m->data); free(m);
}
static Size GetHandleSize(Handle h) { return (Size)((HostMaster *)h)->size; }
static void HLock(Handle h) { (void)h; }
static void HUnlock(Handle h) { (void)h; }
static OSErr MemError(void) { return noErr; }
static void BlockMoveData(const void *s, void *d, Size n) { memmove(d, s, (size_t)n); }
static void BlockMove(const void *s, void *d, Size n) { memmove(d, s, (size_t)n); }

/* ---------------- Component Manager / ICM calls ---------------- */
static void *gHostInstanceStorage;
static void SetComponentInstanceStorage(ComponentInstance ci, Handle storage)
{ (void)ci; gHostInstanceStorage = (void *)storage; }

static const unsigned char *gHostAvcC; static long gHostAvcCSize;
static OSErr GetImageDescriptionExtension(ImageDescriptionHandle d, Handle *ext, OSType type, long index)
{
    Handle h;
    (void)d; (void)type; (void)index;
    if (gHostAvcC == NULL) return -1;
    h = HostNewHandle((Size)gHostAvcCSize, 0, 0);
    memcpy(*h, gHostAvcC, (size_t)gHostAvcCSize);
    *ext = h;
    return noErr;
}

#endif /* HOST_MAC_H */
