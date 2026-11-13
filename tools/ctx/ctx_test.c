/*
    ctx_test.c -- type-check every region of H264Codec.c touched since the
    Sep 10 build, IN CONTEXT and IN THE REAL FILE'S ORDER, on the host.

    Each frag_*.inc is extracted verbatim from H264Codec.c by extract.py,
    which also asserts that the fragments still appear in the same order
    in the real file as they are included below. That order matters: the
    loggers are included AFTER the YUV code that calls them, exactly as
    in H264Codec.c, so a missing forward declaration fails here as an
    implicit-declaration error -- the defect the first blit-only test
    could not see, because it never compiled BlitPicture.

    The stubs model only what the fragments reference, using 32-bit types
    to match PowerPC. What this CANNOT check is anything whose truth lives
    in headers not in the repo (Quickdraw.h's PixMap, Timer.h,
    Folders.h): those are stubbed with the spellings the code uses, so
    CodeWarrior against the real Universal Interfaces remains the final
    word on them.

    Build twice, once per PixMap layout:
      gcc <flags> ctx_test.c
      gcc <flags> -DOLDPIXMAPSTRUCT=1 ctx_test.c
*/

#include <string.h>

/* ---- Mac basics (32-bit PowerPC widths) ---- */
#define pascal
typedef unsigned char   u8;
typedef unsigned int    u32;
typedef int             i32;
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

/* ---- File Manager / Folder Manager / Timer ---- */
typedef struct { short vRefNum; long parID; Str255 name; } FSSpec;
#define fsWrPerm     2
#define fsFromStart  1
#define fsFromLEOF   2
OSErr FSMakeFSSpec(short vRefNum, long dirID, ConstStr255Param name, FSSpec *spec);
OSErr FSpCreate(const FSSpec *spec, OSType creator, OSType type, short script);
OSErr FSpOpenDF(const FSSpec *spec, char perm, short *refNum);
OSErr SetFPos(short refNum, short mode, long offset);
OSErr SetEOF(short refNum, long eof);
OSErr FSWrite(short refNum, long *count, const void *buf);
OSErr FSClose(short refNum);
OSErr FlushVol(ConstStr255Param vol, short vRefNum);

#define kOnSystemDisk      ((short)0x8000)
#define kDesktopFolderType 0x6465736bu      /* 'desk' */
#define kDontCreateFolder  0
OSErr FindFolder(short vRefNum, OSType folderType, Boolean createFolder,
                 short *foundVRefNum, long *foundDirID);

typedef struct { UInt32 hi; UInt32 lo; } UnsignedWide;
void Microseconds(UnsignedWide *microTickCount);

/* ---- QuickDraw / ICM ---- */
typedef struct { short top, left, bottom, right; } Rect;

typedef struct {
    Ptr     baseAddr;
    short   rowBytes;
    Rect    bounds;
    short   pmVersion, packType;
    int     packSize;
    int     hRes, vRes;
    short   pixelType, pixelSize, cmpCount, cmpSize;
#if defined(OLDPIXMAPSTRUCT) && OLDPIXMAPSTRUCT
    int     planeBytes;           /* classic-build name for the same slot */
    void   *pmTable;
    int     pmReserved;
#else
    OSType  pixelFormat;          /* the UI 3.4.1 !OLDPIXMAPSTRUCT spelling */
    void   *pmTable;
    void   *pmExt;
#endif
} PixMap;

/* real field order from ImageCodec.h (UI 3.4.1) */
typedef struct {
    int             flags;
    short           wantedPixelSize;
    short           extendWidth;
    short           extendHeight;
    short           bandMin;
    short           bandInc;
    short           pad;
    unsigned int    time;
    int             flags2;
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
    PlanarComponentInfo componentInfoY;
    PlanarComponentInfo componentInfoCb;
    PlanarComponentInfo componentInfoCr;
} PlanarPixmapInfoYUV420;

#define kYUV420CodecType      0x79343230u   /* 'y420' */
#define k422YpCbCr8CodecType  0x32767579u   /* '2vuy' */
#define k32ARGBPixelFormat    0x00000020u

/* ---- Toolbox calls the fragments make ---- */
OSErr  GetImageDescriptionExtension(ImageDescriptionHandle d, Handle *h, OSType t, int i);
void   HLock(Handle h);
void   HUnlock(Handle h);
void   DisposeHandle(Handle h);
Size   GetHandleSize(Handle h);
Ptr    NewPtr(Size n);
Ptr    NewPtrClear(Size n);
void   DisposePtr(Ptr p);
Handle NewHandleClear(Size n);
void   BlockMoveData(const void *s, void *d, Size n);

/* ---- h264bsd + avcC ---- */
typedef struct storage_s storage_t;
#define H264BSD_RDY            0
#define H264BSD_PIC_RDY        1
#define H264BSD_HDRS_RDY       2
#define H264BSD_ERROR          3
#define H264BSD_PARAM_SET_ERROR 4
#define H264BSD_MEMALLOC_ERROR 5
u32  h264bsdPicWidth(storage_t *s);
u32  h264bsdPicHeight(storage_t *s);
void h264bsdCroppingParams(storage_t *s, u32 *f, u32 *l, u32 *w, u32 *t, u32 *h);
u8  *h264bsdNextOutputPicture(storage_t *s, u32 *id, u32 *idr, u32 *err);
u32  h264bsdDecode(storage_t *s, u8 *byteStrm, u32 len, u32 picId, u32 *readBytes);
void h264bsdShutdown(storage_t *s);
void h264bsdFree(storage_t *s);
u32  AVCC_WorstCaseAnnexBSize(u32 n);
u32  AVCC_ConvertSampleToAnnexB(const u8 *src, u32 srcLen, u32 lengthSize,
                                u8 *dst, u32 dstCap);

/* ---- the code under test, in the real file's order ---- */
#include "frag_config.inc"
#include "frag_globals.inc"

/* things defined elsewhere in H264Codec.c, before these fragments */
static void  CopyPlanarYUVToPixMap(H264GlobalsPtr g, const u8 *yuv,
                                   CodecDecompressParams *p)
{ (void)g; (void)yuv; (void)p; }
static OSErr EnsureDecoder(H264GlobalsPtr g) { (void)g; return noErr; }
static int   FeedParameterSets(H264GlobalsPtr g, const u8 *d, u32 n)
{ (void)g; (void)d; (void)n; return 1; }

#include "frag_yuv.inc"
#include "frag_loggers.inc"
#include "frag_close.inc"
#include "frag_predecompress.inc"
#include "frag_drain.inc"
#include "frag_band.inc"
