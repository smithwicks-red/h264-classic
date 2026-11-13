/*
    blit_hosttest.c -- exercise the REAL '2vuy' and 'y420' blit functions
    from H264Codec.c on a host, against a synthetic picture, and dump the
    results for comparison with an independent Python reference.

    The functions under test are not copied here. They are extracted
    verbatim from H264Codec.c into blit_extract.inc by the build step, so
    what runs is the exact source CodeWarrior will compile. A blit is
    precisely the kind of code where an off-by-one produces a picture
    that looks almost right -- a one-pixel shift, a swapped chroma pair,
    a colour fringe on the last column -- and "almost right" is much
    harder to notice on a CRT across the room than a crash would be.

    Build:  gcc -std=c89 -Wall -o blit_hosttest blit_hosttest.c
    Run:    ./blit_hosttest <outdir>
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- Classic Mac OS / h264bsd type stubs ---------------- */

/*
    These MUST be 32-bit, not `long`. On 32-bit PowerPC `long` is four
    bytes; on an x86-64 host it is eight. Using `long` here silently made
    PlanarComponentInfo 16 bytes instead of 8, and so
    PlanarPixmapInfoYUV420 48 bytes instead of 24 -- which shifted every
    plane offset and made the y420 comparison fail for a reason that had
    nothing to do with the code under test.

    That is worth a comment rather than a quiet fix: the whole point of
    this harness is that it models the target, and a stub that is wider
    than the real type also hides 32-bit overflow behaviour that the Mac
    would actually exhibit.
*/
typedef unsigned char   u8;
typedef unsigned int    u32;
typedef unsigned char   Boolean;
typedef char           *Ptr;
typedef long            Size;
typedef unsigned int    OSType;
typedef int             SInt32;
typedef unsigned int    UInt32;

#define false 0
#define true  1

#define FOUR_CHAR_CODE(x)  ((OSType) \
    (((unsigned long)(unsigned char)(x)[0] << 24) | \
     ((unsigned long)(unsigned char)(x)[1] << 16) | \
     ((unsigned long)(unsigned char)(x)[2] <<  8) | \
     ((unsigned long)(unsigned char)(x)[3])))

#define kYUV420CodecType       FOUR_CHAR_CODE("y420")
#define k422YpCbCr8CodecType   FOUR_CHAR_CODE("2vuy")

typedef struct { short top, left, bottom, right; } Rect;

typedef struct {
    Ptr    baseAddr;
    short  rowBytes;
    Rect   bounds;
    short  pixelSize;
    OSType pixelFormat;
} PixMap;

typedef struct {
    PixMap dstPixMap;
    Rect   dstRect;
} CodecDecompressParams;

typedef struct { SInt32 offset; UInt32 rowBytes; } PlanarComponentInfo;
typedef struct {
    PlanarComponentInfo componentInfoY;
    PlanarComponentInfo componentInfoCb;
    PlanarComponentInfo componentInfoCr;
} PlanarPixmapInfoYUV420;

/* A stand-in for the decoder handle: the blits only ever ask it for
   geometry, so the stub carries exactly that. */
typedef struct {
    u32 picWidthMB, picHeightMB;
    u32 cropFlag, cropL, cropW, cropT, cropH;
} FakeDecoder;

typedef struct { FakeDecoder *h264; } H264Globals;
typedef H264Globals *H264GlobalsPtr;

static FakeDecoder *gDec;

static u32 h264bsdPicWidth(FakeDecoder *d)  { return d->picWidthMB;  }
static u32 h264bsdPicHeight(FakeDecoder *d) { return d->picHeightMB; }
static void h264bsdCroppingParams(FakeDecoder *d, u32 *f, u32 *l, u32 *w,
                                   u32 *t, u32 *h)
{ *f = d->cropFlag; *l = d->cropL; *w = d->cropW; *t = d->cropT; *h = d->cropH; }

static void BlockMoveData(const void *s, void *d, Size n)
{ memmove(d, s, (size_t)n); }

#define kOfferYUVOutput 1
#define H264_PIXMAP_FORMAT(pm)  ((pm).pixelFormat)

/* The code under test, verbatim. */
#include "blit_extract.inc"

/* ---------------- the synthetic picture ---------------- */

#define MBW   23          /* 23 macroblocks -> 368 px, i.e. 360 rounded up */
#define MBH   15          /* 15 macroblocks -> 240 px                      */
#define SRCW  (MBW * 16)
#define SRCH  (MBH * 16)
#define VISW  360
#define VISH  240

static u8 *MakePicture(void)
{
    long ySize = (long)SRCW * SRCH;
    long cSize = ySize / 4;
    u8  *pic = (u8 *)malloc(ySize + 2 * cSize);
    u8  *Y  = pic;
    u8  *Cb = pic + ySize;
    u8  *Cr = Cb + cSize;
    u32  x, y;

    /* Values chosen so a transposition, a one-row shift or a Cb/Cr swap
       all change the output. Nothing is constant along either axis. */
    for (y = 0; y < SRCH; y++)
        for (x = 0; x < SRCW; x++)
            Y[(long)y * SRCW + x] = (u8)((x * 3 + y * 7) & 0xFF);

    for (y = 0; y < SRCH / 2; y++)
        for (x = 0; x < SRCW / 2; x++)
        {
            Cb[(long)y * (SRCW / 2) + x] = (u8)((x * 5 + y * 11) & 0xFF);
            Cr[(long)y * (SRCW / 2) + x] = (u8)((x * 13 + y * 2 + 77) & 0xFF);
        }

    return pic;
}

static void WriteFile(const char *dir, const char *name,
                      const void *data, long len)
{
    char path[1024];
    FILE *f;
    sprintf(path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
    printf("  wrote %-22s %ld bytes\n", name, len);
}

/* ---------------- the cases ---------------- */

static void Run2vuy(const char *dir, u8 *pic, const char *tag,
                    Rect bounds, Rect dstRect)
{
    H264Globals glob;
    CodecDecompressParams p;
    long rowBytes = (long)(bounds.right - bounds.left) * 2;
    long bufBytes = rowBytes * (bounds.bottom - bounds.top);
    u8  *buf = (u8 *)malloc(bufBytes);
    char name[128];

    memset(buf, 0, bufBytes);
    glob.h264 = gDec;

    memset(&p, 0, sizeof(p));
    p.dstPixMap.baseAddr    = (Ptr)buf;
    p.dstPixMap.rowBytes    = (short)rowBytes;
    p.dstPixMap.bounds      = bounds;
    p.dstPixMap.pixelSize   = 16;
    p.dstPixMap.pixelFormat = k422YpCbCr8CodecType;
    p.dstRect               = dstRect;

    CopyPlanarYUVTo2vuy(&glob, pic, &p);

    sprintf(name, "out_2vuy_%s.raw", tag);
    WriteFile(dir, name, buf, bufBytes);
    free(buf);
}

static void RunY420(const char *dir, u8 *pic, const char *tag,
                    Rect bounds, Rect dstRect)
{
    H264Globals glob;
    CodecDecompressParams p;
    long bw = bounds.right - bounds.left;
    long bh = bounds.bottom - bounds.top;
    long yRB = bw, cRB = bw / 2;
    long hdr = (long)sizeof(PlanarPixmapInfoYUV420);
    long yBytes = yRB * bh, cBytes = cRB * (bh / 2);
    long bufBytes = hdr + yBytes + 2 * cBytes;
    u8  *buf = (u8 *)malloc(bufBytes);
    PlanarPixmapInfoYUV420 *ppi = (PlanarPixmapInfoYUV420 *)buf;
    char name[128];

    memset(buf, 0, bufBytes);
    glob.h264 = gDec;

    ppi->componentInfoY.offset   = (SInt32)hdr;
    ppi->componentInfoY.rowBytes = (UInt32)yRB;
    ppi->componentInfoCb.offset  = (SInt32)(hdr + yBytes);
    ppi->componentInfoCb.rowBytes= (UInt32)cRB;
    ppi->componentInfoCr.offset  = (SInt32)(hdr + yBytes + cBytes);
    ppi->componentInfoCr.rowBytes= (UInt32)cRB;

    memset(&p, 0, sizeof(p));
    p.dstPixMap.baseAddr    = (Ptr)buf;
    p.dstPixMap.rowBytes    = (short)yRB;
    p.dstPixMap.bounds      = bounds;
    p.dstPixMap.pixelSize   = 12;
    p.dstPixMap.pixelFormat = kYUV420CodecType;
    p.dstRect               = dstRect;

    CopyPlanarYUVToY420(&glob, pic, &p);

    sprintf(name, "out_y420_%s.raw", tag);
    WriteFile(dir, name, buf, bufBytes);
    free(buf);
}

int main(int argc, char **argv)
{
    u8 *pic;
    FakeDecoder dec;
    const char *dir = (argc > 1) ? argv[1] : ".";

    dec.picWidthMB = MBW;  dec.picHeightMB = MBH;
    dec.cropFlag = 1; dec.cropL = 0; dec.cropW = VISW;
    dec.cropT = 0;    dec.cropH = VISH;
    gDec = &dec;

    pic = MakePicture();
    WriteFile(dir, "in_pic.yuv", pic,
              (long)SRCW * SRCH + 2 * ((long)SRCW * SRCH / 4));

    printf("\ncase A: origin at 0,0 (buffer is exactly the frame)\n");
    {
        Rect b, r;
        b.top = 0; b.left = 0; b.bottom = VISH; b.right = VISW;
        r = b;
        Run2vuy(dir, pic, "A", b, r);
        RunY420(dir, pic, "A", b, r);
    }

    /* The awkward case the real BandDecompress log actually showed: a
       pixmap much larger than the frame, with NEGATIVE bounds, and the
       frame parked somewhere in the middle of it. */
    printf("\ncase B: frame inset into a larger pixmap with negative bounds\n");
    {
        Rect b, r;
        b.top = -50; b.left = -24; b.bottom = 718; b.right = 1256;
        r.top = 24;  r.left = 88;  r.bottom = 24 + VISH; r.right = 88 + VISW;
        Run2vuy(dir, pic, "B", b, r);
        RunY420(dir, pic, "B", b, r);
    }

    printf("\ndone\n");
    free(pic);
    return 0;
}
