/*
    mp4fix_hosttest.c -- compile the REAL MP4Fix.c on a modern host and
    check that the bytes it produces are identical to the Python proof.

    This is not a rewrite of the algorithm. It stubs the Classic Mac OS
    APIs over stdio and then #includes MP4Fix.c unmodified, so what gets
    exercised is the exact source CodeWarrior will compile. A bug in the
    stco shift would silently corrupt a movie in a way that only shows up
    minutes into playback, which is precisely the kind of bug worth
    catching before it reaches the Mac.

    Build:
        gcc -std=c89 -Wall -o mp4fix_hosttest mp4fix_hosttest.c
    Run:
        ./mp4fix_hosttest in.mp4 out.mov
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- minimal Classic Mac OS type stubs ---------------- */

typedef short          OSErr;
typedef char          *Ptr;
typedef long           Size;
typedef unsigned char  Str255[256];
typedef const unsigned char *ConstStr255Param;
typedef unsigned long  OSType;

#define noErr        0
#define eofErr     (-39)
#define ioErr      (-36)
#define memFullErr (-108)
#define dupFNErr   (-48)
#define opWrErr    (-49)
#define permErr    (-54)
#define fnfErr     (-43)

#define fsFromStart  1
#define fsRdPerm     1
#define fsWrPerm     2
#define fsRdWrPerm   3
#define smSystemScript 0

typedef struct { short vRefNum; long parID; unsigned char name[64]; } FSSpec;
typedef struct { OSType fdType; OSType fdCreator; short fdFlags; long fdLocation; short fdFldr; } FInfo;
typedef struct { int sfGood; FSSpec sfFile; } StandardFileReply;
typedef long SFTypeList[4];

/* Each FSSpec carries a real path in .name (Pascal) for the harness. */
static void PathOf(const FSSpec *s, char *out)
{
    int n = s->name[0];
    memcpy(out, s->name + 1, n);
    out[n] = '\0';
}

#define kMaxForks 8
static FILE *gForks[kMaxForks];

static OSErr OpenFork(const FSSpec *s, const char *mode, short *ref)
{
    char path[512];
    int i;
    PathOf(s, path);
    for (i = 1; i < kMaxForks; i++) if (gForks[i] == NULL) break;
    if (i == kMaxForks) return ioErr;
    gForks[i] = fopen(path, mode);
    if (!gForks[i]) return fnfErr;
    *ref = (short)i;
    return noErr;
}

static OSErr FSpOpenDF(const FSSpec *s, char perm, short *ref)
{
    return OpenFork(s, (perm == fsRdPerm) ? "rb" : "r+b", ref);
}
static OSErr FSpCreate(const FSSpec *s, OSType c, OSType t, short script)
{
    char path[512]; FILE *f;
    (void)c; (void)t; (void)script;
    PathOf(s, path);
    f = fopen(path, "wb");
    if (!f) return ioErr;
    fclose(f);
    return noErr;
}
static OSErr FSpDelete(const FSSpec *s)
{ char path[512]; PathOf(s, path); return remove(path) == 0 ? noErr : fnfErr; }
static OSErr FSClose(short ref)
{ if (ref > 0 && gForks[ref]) { fclose(gForks[ref]); gForks[ref] = NULL; } return noErr; }
static OSErr FSRead(short ref, long *count, Ptr buf)
{
    size_t got = fread(buf, 1, (size_t)*count, gForks[ref]);
    *count = (long)got;
    return got == 0 ? eofErr : noErr;
}
static OSErr FSWrite(short ref, long *count, Ptr buf)
{
    size_t put = fwrite(buf, 1, (size_t)*count, gForks[ref]);
    *count = (long)put;
    return noErr;
}
static OSErr SetFPos(short ref, short mode, long pos)
{ (void)mode; return fseek(gForks[ref], pos, SEEK_SET) == 0 ? noErr : ioErr; }
static OSErr GetEOF(short ref, long *eof)
{
    long here = ftell(gForks[ref]);
    fseek(gForks[ref], 0, SEEK_END);
    *eof = ftell(gForks[ref]);
    fseek(gForks[ref], here, SEEK_SET);
    return noErr;
}
static OSErr SetEOF(short ref, long eof) { (void)ref; (void)eof; return noErr; }
static OSErr FlushVol(void *n, short v) { (void)n; (void)v; return noErr; }
static OSErr FSpGetFInfo(const FSSpec *s, FInfo *fi)
{ (void)s; memset(fi, 0, sizeof(*fi)); return noErr; }
static OSErr FSpSetFInfo(const FSSpec *s, FInfo *fi) { (void)s; (void)fi; return noErr; }
static OSErr FSpRename(const FSSpec *s, ConstStr255Param n) { (void)s; (void)n; return noErr; }

static Ptr NewPtr(Size n)      { return (Ptr)malloc((size_t)n); }
static Ptr NewPtrClear(Size n) { return (Ptr)calloc(1, (size_t)n); }
static void DisposePtr(Ptr p)  { free(p); }
static void BlockMoveData(const void *s, void *d, Size n) { memmove(d, s, (size_t)n); }

/* The harness drives the transform directly, so these never run. */
static void StandardGetFile(void *f, short n, SFTypeList t, StandardFileReply *r)
{ (void)f; (void)n; (void)t; r->sfGood = 0; }
static void StandardPutFile(ConstStr255Param p, ConstStr255Param d, StandardFileReply *r)
{ (void)p; (void)d; r->sfGood = 0; }

/* Keep MP4Fix.c's own main out of the way. */
#define main mp4fix_main_unused

/* Its Mac headers are all stubbed above. */
#define __TYPES__
#define __FILES__
#define __STANDARDFILE__
#define __ERRORS__
#define __MEMORY__

#include "../codec-src/MP4Fix.c"

#undef main

static void SetSpec(FSSpec *s, const char *path)
{
    size_t n = strlen(path);
    /* .name is a Str63 like the real FSSpec, so a host path longer than
       63 characters used to overflow it and die with "stack smashing
       detected" AFTER writing correct output -- confusing, not harmful.
       Refuse instead; use a short relative path (cd to the folder). */
    if (n > 63)
    {
        fprintf(stderr, "path too long for the Str63 stub (%lu > 63): %s\n"
                        "cd to its folder and use a short relative name.\n",
                (unsigned long)n, path);
        exit(2);
    }
    memset(s, 0, sizeof(*s));
    s->name[0] = (unsigned char)n;
    memcpy(s->name + 1, path, n);
}

int main(int argc, char **argv)
{
    FSSpec in, out;
    OSErr  err;

    if (argc != 3) { fprintf(stderr, "usage: %s in.mp4 out.mov\n", argv[0]); return 2; }

    SetSpec(&in,  argv[1]);
    SetSpec(&out, argv[2]);

    printf("--- running the real RewriteWithAudioFix() ---\n");
    err = RewriteWithAudioFix(&in, &out);
    printf("--- returned %d ---\n", (int)err);
    return err == noErr ? 0 : 1;
}
