/* AVCDecTest b21 (r2 API checks added) (CodeWarrior 8.3, PPC): loads AVCDecLib (Retro68 GCC shlb, FFmpeg 0.8.15 H.264),
   decodes 9 raw .264 clips from ":clips:" beside the app, MD5 over cropped Y,U,V of every output
   picture (same as the host harness), C pass then AltiVec pass. No console, no UI: writes
   "AVCDecTest Log" beside the app and quits by itself. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <MacTypes.h>
#include <Memory.h>
#include <Files.h>
#include <Events.h>
#include <Processes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Script.h>
#include <Gestalt.h>
#include <CodeFragments.h>

#ifndef gestaltPowerPCProcessorFeatures
#define gestaltPowerPCProcessorFeatures 'ppcf'
#endif
#ifndef gestaltPowerPCHasVectorInstructions
#define gestaltPowerPCHasVectorInstructions 4
#endif

/* ---- AVCDecLib API (mirror of avcdec.h) ---- */
typedef struct {
    void *(*alloc)(void *opaque, unsigned long size);
    void  (*release)(void *opaque, void *ptr);
    void  (*log)(void *opaque, int level, const char *fmt, va_list vl);
    void  *opaque;
} AVCDecCallbacks;
typedef struct {
    const unsigned char *plane[3];
    long stride[3];
    long width, height, key, count;
    long poc;               /* r2 (0x00080F02): appended, struct is 44 bytes */
} AVCDecPicture;
typedef unsigned long (*VersionFn)(void);
typedef long (*InitFn)(const AVCDecCallbacks *, unsigned long, unsigned long *);
typedef void *(*OpenFn)(const unsigned char *, long);
typedef long (*FeedFn)(void *, const unsigned char *, long, long *, AVCDecPicture *);
typedef long (*SampleFn)(void *, const unsigned char *, long, AVCDecPicture *);
typedef long (*FlushFn)(void *, AVCDecPicture *);
typedef void (*CtxFn)(void *);
typedef long (*SkipFn)(void *, long, long);
typedef long (*GetFn)(void *);
typedef long (*SetDelayFn)(void *, long);
typedef long (*TakeFn)(void *, AVCDecPicture *, long *);
typedef void *(*MPAllocAlignedFn)(ByteCount size, UInt32 align, UInt32 options);
typedef void (*MPFreeFn)(void *p);

static const char *kClips[9][2] = {
    {"base_320x240.264", "7d898a065a47e141bb71a9bdac9ea95a"},
    {"base_480x360.264", "26c1f1e5898cd7761bb29e88991ac76c"},
    {"base_640x480.264", "9281a31a4b6399a5cf1e23d21ee192c5"},
    {"main_320x240.264", "d2f5bc1c3d3c61c6f048a3ed47e3f9dd"},
    {"main_480x360.264", "a7ff7619802fcc6e63f2b8690896c4d3"},
    {"main_640x480.264", "e0d370cd339614fcb195aee130a459b3"},
    {"high_320x240.264", "b46f638ac272afc0ec0c337bc1153d45"},
    {"high_480x360.264", "1cb311416dcf71b0749348f293ea2731"},
    {"high_640x480.264", "f809d9dd80d3291e3951363322763ca0"}
};

static short gLogRef = 0;
static void Log(const char *fmt, ...)
{
    char buf[600];
    va_list ap;
    long n, i;
    ParamBlockRec pb;
    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    n = (long)strlen(buf);
    for (i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = '\r';
    if (gLogRef != 0 && n > 0) {
        (void)FSWrite(gLogRef, &n, buf);
        memset(&pb, 0, sizeof(pb));
        pb.ioParam.ioRefNum = gLogRef;
        (void)PBFlushFileSync(&pb);
    }
}
static OSErr AppSpec(FSSpec *app)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    Str255 pname;
    OSErr err = GetCurrentProcess(&psn);
    if (err != noErr) return err;
    memset(&info, 0, sizeof(info));
    info.processInfoLength = sizeof(info);
    info.processName = pname;
    info.processAppSpec = app;
    return GetProcessInformation(&psn, &info);
}
static short OpenLog(void)
{
    FSSpec app, spec;
    short ref;
    OSErr err;
    if (AppSpec(&app) != noErr) return 0;
    err = FSMakeFSSpec(app.vRefNum, app.parID, "\pAVCDecTest Log", &spec);
    if (err == fnfErr) err = FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript);
    if (err != noErr) return 0;
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return 0;
    (void)SetEOF(ref, 0);
    return ref;
}
static asm unsigned long CW_SP(void)
{
    nofralloc
    mr r3, r1
    blr
}
static void *Sym(CFragConnectionID conn, const char *name)
{
    Str255 pn;
    Ptr addr = NULL;
    CFragSymbolClass cls = 0;
    OSErr err;
    size_t n = strlen(name);
    pn[0] = (unsigned char)n;
    memcpy(pn + 1, name, n);
    err = FindSymbol(conn, pn, &addr, &cls);
    if (err != noErr) Log("FindSymbol %s err=%d\n", name, (int)err);
    return err == noErr ? (void *)addr : NULL;
}

/* ---- MD5 (RFC 1321), byte-oriented so endianness does not matter ---- */
typedef struct { unsigned long s[4], lo, hi; unsigned char buf[64]; } MD5;
#define F1(x,y,z) (z ^ (x & (y ^ z)))
#define F2(x,y,z) (y ^ (z & (x ^ y)))
#define F3(x,y,z) (x ^ y ^ z)
#define F4(x,y,z) (y ^ (x | ~z))
#define STEP(f,a,b,c,d,x,t,s) a += f(b,c,d) + x + t; a = ((a << s) | ((a & 0xffffffffUL) >> (32 - s))); a += b;
static void MD5Block(MD5 *m, const unsigned char *p)
{
    unsigned long a = m->s[0], b = m->s[1], c = m->s[2], d = m->s[3], x[16];
    int i;
    for (i = 0; i < 16; i++)
        x[i] = (unsigned long)p[i*4] | ((unsigned long)p[i*4+1] << 8) | ((unsigned long)p[i*4+2] << 16) | ((unsigned long)p[i*4+3] << 24);
    STEP(F1,a,b,c,d,x[0],0xd76aa478,7)  STEP(F1,d,a,b,c,x[1],0xe8c7b756,12) STEP(F1,c,d,a,b,x[2],0x242070db,17) STEP(F1,b,c,d,a,x[3],0xc1bdceee,22)
    STEP(F1,a,b,c,d,x[4],0xf57c0faf,7)  STEP(F1,d,a,b,c,x[5],0x4787c62a,12) STEP(F1,c,d,a,b,x[6],0xa8304613,17) STEP(F1,b,c,d,a,x[7],0xfd469501,22)
    STEP(F1,a,b,c,d,x[8],0x698098d8,7)  STEP(F1,d,a,b,c,x[9],0x8b44f7af,12) STEP(F1,c,d,a,b,x[10],0xffff5bb1,17) STEP(F1,b,c,d,a,x[11],0x895cd7be,22)
    STEP(F1,a,b,c,d,x[12],0x6b901122,7) STEP(F1,d,a,b,c,x[13],0xfd987193,12) STEP(F1,c,d,a,b,x[14],0xa679438e,17) STEP(F1,b,c,d,a,x[15],0x49b40821,22)
    STEP(F2,a,b,c,d,x[1],0xf61e2562,5)  STEP(F2,d,a,b,c,x[6],0xc040b340,9)  STEP(F2,c,d,a,b,x[11],0x265e5a51,14) STEP(F2,b,c,d,a,x[0],0xe9b6c7aa,20)
    STEP(F2,a,b,c,d,x[5],0xd62f105d,5)  STEP(F2,d,a,b,c,x[10],0x02441453,9) STEP(F2,c,d,a,b,x[15],0xd8a1e681,14) STEP(F2,b,c,d,a,x[4],0xe7d3fbc8,20)
    STEP(F2,a,b,c,d,x[9],0x21e1cde6,5)  STEP(F2,d,a,b,c,x[14],0xc33707d6,9) STEP(F2,c,d,a,b,x[3],0xf4d50d87,14) STEP(F2,b,c,d,a,x[8],0x455a14ed,20)
    STEP(F2,a,b,c,d,x[13],0xa9e3e905,5) STEP(F2,d,a,b,c,x[2],0xfcefa3f8,9)  STEP(F2,c,d,a,b,x[7],0x676f02d9,14) STEP(F2,b,c,d,a,x[12],0x8d2a4c8a,20)
    STEP(F3,a,b,c,d,x[5],0xfffa3942,4)  STEP(F3,d,a,b,c,x[8],0x8771f681,11) STEP(F3,c,d,a,b,x[11],0x6d9d6122,16) STEP(F3,b,c,d,a,x[14],0xfde5380c,23)
    STEP(F3,a,b,c,d,x[1],0xa4beea44,4)  STEP(F3,d,a,b,c,x[4],0x4bdecfa9,11) STEP(F3,c,d,a,b,x[7],0xf6bb4b60,16) STEP(F3,b,c,d,a,x[10],0xbebfbc70,23)
    STEP(F3,a,b,c,d,x[13],0x289b7ec6,4) STEP(F3,d,a,b,c,x[0],0xeaa127fa,11) STEP(F3,c,d,a,b,x[3],0xd4ef3085,16) STEP(F3,b,c,d,a,x[6],0x04881d05,23)
    STEP(F3,a,b,c,d,x[9],0xd9d4d039,4)  STEP(F3,d,a,b,c,x[12],0xe6db99e5,11) STEP(F3,c,d,a,b,x[15],0x1fa27cf8,16) STEP(F3,b,c,d,a,x[2],0xc4ac5665,23)
    STEP(F4,a,b,c,d,x[0],0xf4292244,6)  STEP(F4,d,a,b,c,x[7],0x432aff97,10) STEP(F4,c,d,a,b,x[14],0xab9423a7,15) STEP(F4,b,c,d,a,x[5],0xfc93a039,21)
    STEP(F4,a,b,c,d,x[12],0x655b59c3,6) STEP(F4,d,a,b,c,x[3],0x8f0ccc92,10) STEP(F4,c,d,a,b,x[10],0xffeff47d,15) STEP(F4,b,c,d,a,x[1],0x85845dd1,21)
    STEP(F4,a,b,c,d,x[8],0x6fa87e4f,6)  STEP(F4,d,a,b,c,x[15],0xfe2ce6e0,10) STEP(F4,c,d,a,b,x[6],0xa3014314,15) STEP(F4,b,c,d,a,x[13],0x4e0811a1,21)
    STEP(F4,a,b,c,d,x[4],0xf7537e82,6)  STEP(F4,d,a,b,c,x[11],0xbd3af235,10) STEP(F4,c,d,a,b,x[2],0x2ad7d2bb,15) STEP(F4,b,c,d,a,x[9],0xeb86d391,21)
    m->s[0] += a; m->s[1] += b; m->s[2] += c; m->s[3] += d;
}
static void MD5Init(MD5 *m)
{
    m->s[0] = 0x67452301UL; m->s[1] = 0xefcdab89UL; m->s[2] = 0x98badcfeUL; m->s[3] = 0x10325476UL;
    m->lo = m->hi = 0;
}
static void MD5Update(MD5 *m, const unsigned char *p, unsigned long n)
{
    unsigned long used = m->lo & 63, k;
    if ((m->lo += n) < n) m->hi++;
    if (used) {
        k = 64 - used;
        if (n < k) { memcpy(m->buf + used, p, n); return; }
        memcpy(m->buf + used, p, k); MD5Block(m, m->buf); p += k; n -= k;
    }
    while (n >= 64) { MD5Block(m, p); p += 64; n -= 64; }
    memcpy(m->buf, p, n);
}
static void MD5Final(MD5 *m, char *hex)
{
    static const unsigned char pad[64] = {0x80};
    unsigned char len[8], d[16];
    unsigned long lo = m->lo << 3, hi = (m->hi << 3) | (m->lo >> 29), used = m->lo & 63;
    int i;
    for (i = 0; i < 4; i++) { len[i] = (unsigned char)(lo >> (8*i)); len[i+4] = (unsigned char)(hi >> (8*i)); }
    MD5Update(m, pad, used < 56 ? 56 - used : 120 - used);
    MD5Update(m, len, 8);
    for (i = 0; i < 16; i++) d[i] = (unsigned char)(m->s[i/4] >> (8*(i%4)));
    for (i = 0; i < 16; i++) sprintf(hex + 2*i, "%02x", d[i]);
}

/* ---- allocator callbacks: MPAllocateAligned 16-byte, counted ---- */
static MPAllocAlignedFn gMPAlloc;
static MPFreeFn gMPFree;
static unsigned long gInUse, gPeak, gFails, gTempBlocks, gMPBlocks;
/* 16-byte header before every user block: [0]=size [1]=kind (1=MPAllocateAligned, 2=TempNewHandle)
   [2]=Handle. MPAllocateAligned first; the MP pool on this system is small (~2.5 MB seen in run 1),
   so fall back to locked temporary memory, aligned by hand to 16 bytes. */
static void *CBAlloc(void *opaque, unsigned long size)
{
    unsigned char *p;
    unsigned long *h;
    OSErr err;
    (void)opaque;
    p = (unsigned char *)gMPAlloc(size + 16, 4 /* kMPAllocate16ByteAligned */, 0);
    if (p) {
        h = (unsigned long *)p; h[0] = size; h[1] = 1; h[2] = 0; gMPBlocks++;
    } else {
        Handle th = TempNewHandle((Size)(size + 32), &err);
        if (!th) {
            if (gFails++ < 3) Log("  alloc %lu failed: MP null, TempNewHandle err=%d TempFreeMem=%ld FreeMem=%ld\n",
                                  size, (int)err, TempFreeMem(), FreeMem());
            return NULL;
        }
        HLock(th);
        p = (unsigned char *)((((unsigned long)*th) + 15UL) & ~15UL);
        h = (unsigned long *)p; h[0] = size; h[1] = 2; h[2] = (unsigned long)th; gTempBlocks++;
    }
    gInUse += size;
    if (gInUse > gPeak) gPeak = gInUse;
    return p + 16;
}
static void CBRelease(void *opaque, void *ptr)
{
    unsigned long *h;
    (void)opaque;
    if (!ptr) return;
    h = (unsigned long *)((unsigned char *)ptr - 16);
    gInUse -= h[0];
    if (h[1] == 2) DisposeHandle((Handle)h[2]); else gMPFree(h);
}
static int gLibLogs;
static void CBLog(void *opaque, int level, const char *fmt, va_list vl)
{
    (void)opaque; (void)vl;
    if (gLibLogs++ >= 12) return;
    Log("  [lib log %d] %s", level, fmt);   /* fmt only: va_list not interpreted across compilers */
    if (fmt[0] && fmt[strlen(fmt) - 1] != '\n') Log("\n");
}

static FeedFn fFeed; static FlushFn fFlush; static OpenFn fOpen; static CtxFn fClose;
static SkipFn fSkip; static GetFn fGetDelay, fHeld; static SetDelayFn fSetDelay; static TakeFn fTake, fPeek;
static unsigned long gLastYield;
static void Yield(void)
{
    EventRecord ev;
    if (TickCount() - gLastYield > 20) { (void)WaitNextEvent(0, &ev, 0, NULL); gLastYield = TickCount(); }
}
static void HashPic(MD5 *m, const AVCDecPicture *p)
{
    long y;
    for (y = 0; y < p->height; y++) MD5Update(m, p->plane[0] + y * p->stride[0], p->width);
    for (y = 0; y < p->height / 2; y++) MD5Update(m, p->plane[1] + y * p->stride[1], p->width / 2);
    for (y = 0; y < p->height / 2; y++) MD5Update(m, p->plane[2] + y * p->stride[2], p->width / 2);
}
static unsigned long QuickHash(const AVCDecPicture *p)
{
    unsigned long h = 5381;
    long x, y, k;
    for (k = 0; k < 3; k++) {
        long w = k ? p->width / 2 : p->width, hh = k ? p->height / 2 : p->height;
        for (y = 0; y < hh; y++) {
            const unsigned char *r = p->plane[k] + y * p->stride[k];
            for (x = 0; x < w; x++) h = h * 33UL + r[x];
        }
    }
    return h;
}

/* variants */
#define V_REF 0     /* reference: avcdec_flush drain */
#define V_TAKE 1    /* drain with avcdec_take_lowest */
#define V_PEEK 2    /* avcdec_peek_lowest after each decode that returned 0 */
#define V_SKIP00 3  /* avcdec_set_skip(c,0,0) */
#define V_SKIP10 4  /* avcdec_set_skip(c,1,0) */
static const char *kVarName[5] = {"ref ", "take", "peek", "s0,0", "s1,0"};

typedef struct {
    long frames, ticks, peak, inuse, delayFirst, delayMax, heldchk, heldbad, pocviol, pocreset,
         ntake, takepocbad, heldEnd, npk, pkmatch, pkbad, pkcountbad, skiprv;
    int ok;
    char hex[33];
} Result;

static Result *gR;
static void *gCtx;
static MD5 gMD5;
static long gPrevPoc, gDraining, gKeyOut, gPkPoc[64];
static unsigned long gPkHash[64];

static void Put(AVCDecPicture *p)
{
    long d, i;
    unsigned long hh;
    HashPic(&gMD5, p);
    if (p->key) gKeyOut++;
    if (gKeyOut <= 1 && gR->npk > 0) {
        hh = QuickHash(p);
        for (i = 0; i < gR->npk; i++) if (gPkPoc[i] == p->poc) { if (gPkHash[i] == hh) gR->pkmatch++; else gR->pkbad++; }
    }
    gR->frames++;
    d = fGetDelay(gCtx);
    if (gR->frames == 1) gR->delayFirst = d;
    if (d > gR->delayMax) gR->delayMax = d;
    if (gR->frames > 1 && p->poc <= gPrevPoc) { if (p->key) gR->pocreset++; else gR->pocviol++; }
    gPrevPoc = p->poc;
    if (!gDraining && gR->frames > 1) { gR->heldchk++; if (fHeld(gCtx) != 2) gR->heldbad++; }
}
static void DoPeek(void)
{
    AVCDecPicture q;
    long pc = -999, before, r;
    if (gR->npk >= 64 || gKeyOut > 1) return;
    before = fHeld(gCtx);
    r = fPeek(gCtx, &q, &pc);
    if (r > 0) {
        gPkPoc[gR->npk] = pc; gPkHash[gR->npk] = QuickHash(&q);
        if (q.count != gR->frames || q.poc != pc) gR->pkcountbad++;
        gR->npk++;
    }
    if (fHeld(gCtx) != before) gR->pkbad += 100;
}

#define CHUNK 65536L
static int RunClip(const FSSpec *app, int idx, const char *mode, int var, unsigned char *buf, Result *R)
{
    Str255 path;
    FSSpec spec;
    short ref = 0;
    OSErr err;
    AVCDecPicture pic;
    long n, used, r, pc;
    unsigned long t0;
    const char *name = kClips[idx][0];

    memset(R, 0, sizeof(*R));
    R->delayFirst = -9; R->delayMax = -9; R->heldEnd = -9;
    gR = R; gDraining = 0; gKeyOut = 0; gPrevPoc = 0;
    sprintf((char *)path + 1, ":clips:%s", name);
    path[0] = (unsigned char)strlen((char *)path + 1);
    err = FSMakeFSSpec(app->vRefNum, app->parID, path, &spec);
    if (err == noErr) err = FSpOpenDF(&spec, fsRdPerm, &ref);
    if (err != noErr) { Log("%s %-17s open err=%d\n", mode, name, (int)err); return 0; }
    gPeak = gInUse;
    MD5Init(&gMD5);
    t0 = TickCount();
    gCtx = fOpen(NULL, 0);
    if (!gCtx) { Log("%s %-17s avcdec_open failed (alloc fails=%lu)\n", mode, name, gFails); FSClose(ref); return 0; }
    if (var == V_SKIP00) R->skiprv = fSkip(gCtx, 0, 0);
    if (var == V_SKIP10) R->skiprv = fSkip(gCtx, 1, 0);
    for (;;) {
        const unsigned char *p = buf;
        n = CHUNK;
        err = FSRead(ref, &n, buf);
        if (n <= 0) break;
        while (n > 0) {
            r = fFeed(gCtx, p, n, &used, &pic);
            if (r > 0) Put(&pic);
            else if (r == 0 && var == V_PEEK) DoPeek();
            if (r < 0) { Log("%s %-17s feed error %ld\n", mode, name, r); break; }
            p += used; n -= used;
            Yield();
        }
        if (err != noErr) break;
    }
    if (fFeed(gCtx, NULL, 0, &used, &pic) > 0) Put(&pic);
    gDraining = 1;
    if (var == V_TAKE) {
        while ((r = fTake(gCtx, &pic, &pc)) > 0) { if (pc != pic.poc) R->takepocbad++; R->ntake++; Put(&pic); }
        R->heldEnd = fHeld(gCtx);
    } else {
        while (fFlush(gCtx, &pic) > 0) Put(&pic);
    }
    fClose(gCtx);
    gCtx = NULL;
    R->ticks = (long)(TickCount() - t0);
    FSClose(ref);
    MD5Final(&gMD5, R->hex);
    R->ok = strcmp(R->hex, kClips[idx][1]) == 0;
    R->peak = (long)gPeak; R->inuse = (long)gInUse;
    if (var == V_REF)
        Log("%s %-17s frames=%3ld md5 %s ticks=%6ld peak=%8ld inuse_after=%ld%s%s\n", mode, name, R->frames,
            R->ok ? "OK  " : "FAIL", R->ticks, R->peak, R->inuse, R->ok ? "" : " got=", R->ok ? "" : R->hex);
    else
        Log("%s %s %-17s frames=%3ld md5=%s %s ticks=%ld peak=%ld inuse_after=%ld\n", mode, kVarName[var], name,
            R->frames, R->hex, R->ok ? "(=ref)" : "(!=ref)", R->ticks, R->peak, R->inuse);
    Log("   delay first=%ld max=%ld held chk=%ld bad=%ld pocviol=%ld pocreset=%ld take=%ld takepocbad=%ld held_end=%ld peeks=%ld match=%ld bad=%ld countbad=%ld skiprv=%ld\n",
        R->delayFirst, R->delayMax, R->heldchk, R->heldbad, R->pocviol, R->pocreset, R->ntake, R->takepocbad,
        R->heldEnd, R->npk, R->pkmatch, R->pkbad, R->pkcountbad, R->skiprv);
    return R->ok;
}

static int gPass, gTotal;
static void Check(int ok, const char *fmt, ...)
{
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    gTotal++;
    if (ok) gPass++;
    Log("%s %s\n", ok ? "PASS" : "FAIL", buf);
}

static const char *kExports[14] = {
    "avcdec_version", "avcdec_init", "avcdec_open", "avcdec_feed_annexb", "avcdec_decode_sample",
    "avcdec_flush", "avcdec_reset", "avcdec_close", "avcdec_set_skip", "avcdec_get_delay",
    "avcdec_set_delay", "avcdec_take_lowest", "avcdec_peek_lowest", "avcdec_held"
};
/* host (qemu-ppc, same library) md5 for set_skip(1,0), C and AltiVec identical */
static const char *kSkip10Host[2] = {"f22c2774e61252d398d3626aac206851", "964bc98236e77010c4ae3780b52d04f6"};

int main(void)
{
    CFragConnectionID conn = 0, mpconn = 0;
    Ptr mainAddr = NULL;
    Str255 errName;
    OSErr err;
    long resp = 0;
    int altivec, pass, i, k, okc, nfound, badinuse;
    FSSpec app, lib;
    VersionFn fVersion; InitFn fInit;
    AVCDecCallbacks cb;
    unsigned long spm = 99, ver;
    unsigned char *buf;
    void *syms[14];
    static Result ref[9], R;
    static const int kB[2] = {3, 8};   /* main_320x240, high_640x480 */
    const char *mode;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    gLogRef = OpenLog();
    if (gLogRef == 0) return 0;
    Log("AVCDecTest b21 (CodeWarrior 8.3 app, Retro68 GCC AVCDecLib r2 shlb: FFmpeg 0.8.15 h264) sizeof(AVCDecPicture)=%d\n",
        (int)sizeof(AVCDecPicture));
    err = Gestalt(gestaltPowerPCProcessorFeatures, &resp);
    altivec = (err == noErr) && ((resp >> gestaltPowerPCHasVectorInstructions) & 1);
    Log("Gestalt ppcf err=%d resp=%08lX AltiVec=%d  main sp&15=%lu  FreeMem=%ld\n", (int)err, resp, altivec,
        CW_SP() & 15UL, FreeMem());
    Log("TempFreeMem=%ld MaxBlock=%ld\n", TempFreeMem(), MaxBlock());

    err = GetSharedLibrary("\pMPLibrary", kPowerPCCFragArch, kReferenceCFrag, &mpconn, &mainAddr, errName);
    if (err == noErr) { gMPAlloc = (MPAllocAlignedFn)Sym(mpconn, "MPAllocateAligned"); gMPFree = (MPFreeFn)Sym(mpconn, "MPFree"); }
    if (!gMPAlloc || !gMPFree) { Log("MPLibrary unavailable err=%d\n", (int)err); goto done; }

    errName[0] = 0;
    err = GetSharedLibrary("\pAVCDecLib", kPowerPCCFragArch, kPrivateCFragCopy, &conn, &mainAddr, errName);
    if (err != noErr && AppSpec(&app) == noErr && FSMakeFSSpec(app.vRefNum, app.parID, "\pAVCDecLib", &lib) == noErr) {
        Log("GetSharedLibrary err=%d, trying GetDiskFragment\n", (int)err);
        errName[0] = 0;
        err = GetDiskFragment(&lib, 0, kCFragGoesToEOF, "\pAVCDecLib", kPrivateCFragCopy, &conn, &mainAddr, errName);
    }
    Log("load AVCDecLib err=%d errName=%.*s\n", (int)err, (int)errName[0], (char *)errName + 1);
    if (err != noErr) goto done;
    nfound = 0;
    for (k = 0; k < 14; k++) {
        syms[k] = Sym(conn, kExports[k]);
        Log("  export %-21s %s\n", kExports[k], syms[k] ? "found" : "MISSING");
        if (syms[k]) nfound++;
    }
    Check(nfound == 14, "exports found %d/14", nfound);
    if (nfound != 14) goto done;
    fVersion = (VersionFn)syms[0]; fInit = (InitFn)syms[1]; fOpen = (OpenFn)syms[2]; fFeed = (FeedFn)syms[3];
    fFlush = (FlushFn)syms[5]; fClose = (CtxFn)syms[7]; fSkip = (SkipFn)syms[8]; fGetDelay = (GetFn)syms[9];
    fSetDelay = (SetDelayFn)syms[10]; fTake = (TakeFn)syms[11]; fPeek = (TakeFn)syms[12]; fHeld = (GetFn)syms[13];
    ver = fVersion();
    Check(ver == 0x00080F02UL, "avcdec_version=%08lX (expect 00080F02)", ver);

    buf = (unsigned char *)NewPtr(CHUNK);
    if (!buf || AppSpec(&app) != noErr) { Log("NewPtr/AppSpec failed\n"); goto done; }
    cb.alloc = CBAlloc; cb.release = CBRelease; cb.log = CBLog; cb.opaque = NULL;
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1 && !altivec) { Log("AltiVec pass skipped (Gestalt)\n"); break; }
        mode = pass ? "AV" : "C ";
        Log("avcdec_init(flags=%d) -> %ld  lib sp&15=%lu\n", pass, fInit(&cb, pass ? 1UL : 0UL, &spm), spm);
        okc = 0; badinuse = 0;
        for (i = 0; i < 9; i++) { okc += RunClip(&app, i, mode, V_REF, buf, &ref[i]); if (ref[i].inuse != 0) badinuse++; }
        Log("pass %s: %d/9 OK\n", pass ? "AltiVec" : "C", okc);
        Check(okc == 9, "%s 9-clip reference md5 %d/9", mode, okc);
        for (k = 0; k < 2; k++) {
            int ci = kB[k];
            const char *cn = kClips[ci][0];
            Result *rr = &ref[ci];
            Check(rr->delayFirst == 2 && rr->delayMax == 2, "%s %s get_delay first=%ld max=%ld (expect 2)", mode, cn, rr->delayFirst, rr->delayMax);
            Check(rr->heldchk > 200 && rr->heldbad == 0, "%s %s held==2 at steady state: %ld checks, %ld bad", mode, cn, rr->heldchk, rr->heldbad);
            RunClip(&app, ci, mode, V_TAKE, buf, &R); if (R.inuse != 0) badinuse++;
            Check(R.ok && R.frames == 240 && R.pocviol == 0 && R.takepocbad == 0 && R.heldEnd == 0 && R.ntake > 0,
                  "%s %s take_lowest drain: md5 %s frames=%ld take=%ld pocviol=%ld pocreset(key)=%ld takepocbad=%ld held_end=%ld",
                  mode, cn, R.ok ? "=ref" : R.hex, R.frames, R.ntake, R.pocviol, R.pocreset, R.takepocbad, R.heldEnd);
            RunClip(&app, ci, mode, V_PEEK, buf, &R); if (R.inuse != 0) badinuse++;
            Check(R.ok && R.frames == 240 && R.npk > 0 && R.pkbad == 0 && R.pkcountbad == 0,
                  "%s %s peek_lowest: md5 %s frames=%ld peeks=%ld match=%ld bad=%ld countbad=%ld",
                  mode, cn, R.ok ? "=ref" : R.hex, R.frames, R.npk, R.pkmatch, R.pkbad, R.pkcountbad);
            RunClip(&app, ci, mode, V_SKIP00, buf, &R); if (R.inuse != 0) badinuse++;
            Check(R.ok && R.frames == 240 && R.skiprv == 0, "%s %s set_skip(0,0): rv=%ld md5 %s frames=%ld",
                  mode, cn, R.skiprv, R.ok ? "=ref" : R.hex, R.frames);
            RunClip(&app, ci, mode, V_SKIP10, buf, &R); if (R.inuse != 0) badinuse++;
            Check(R.frames == 240 && R.skiprv == 0, "%s %s set_skip(1,0): rv=%ld frames=%ld md5=%s (host %s: %s)",
                  mode, cn, R.skiprv, R.frames, R.hex, kSkip10Host[k], strcmp(R.hex, kSkip10Host[k]) == 0 ? "same" : "DIFFERENT");
        }
        Check(badinuse == 0, "%s bytes in use after close == 0 on all 17 runs (%d nonzero)", mode, badinuse);
    }
    DisposePtr((Ptr)buf);
done:
    Log("TOTAL %d/%d\n", gPass, gTotal);
    Log("done. alloc fails=%lu inuse=%lu MP blocks=%lu Temp blocks=%lu\n", gFails, gInUse, gMPBlocks, gTempBlocks);
    if (gLogRef) { (void)FSClose(gLogRef); gLogRef = 0; }
    return 0;
}
