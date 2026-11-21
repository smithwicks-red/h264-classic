/*
    H264Codec.c

    A QuickTime Image Decompressor ("Base Codec") component that wraps the
    h264bsd Baseline/Main-profile H.264 decoder for playback under
    QuickTime 4-6 on Mac OS 9.2.2.

    ------------------------------------------------------------------
    READ THIS FIRST -- honest status of this file
    ------------------------------------------------------------------
    This is a first-draft port written without the actual QuickTime SDK
    headers open in front of me (I'm working from the documented, public
    Image Compression Manager API as published in Apple's "Image
    Compression Manager Reference" and reproduced in every third-party
    codec of that era). The *shape* of this file -- the Base Codec macro
    pattern, which callbacks exist and roughly what they receive -- is
    solid and standard. The parts most likely to need a correction pass
    once CodeWarrior's actual ImageCodec.h is open next to it are:
      - exact field names inside CodecDecompressParams /
        ImageSubCodecDecompressRecord (I've used the commonly-documented
        names; a couple of QuickTime versions renamed a few of these)
      - the exact ImageSubCodecDecompressCapabilities fields to fill in
        during Initialize
      - whether GetImageDescriptionExtension wants a 1-based or 0-based
        index, and its exact signature
    None of that changes the architecture below -- it's the kind of thing
    the compiler will flag immediately as an unknown field/undeclared
    identifier, at which point the fix is a one-line rename, not a
    redesign. Treat the first CodeWarrior build of this file as a
    "resolve the red squiggles" pass, not a "did I get the whole design
    wrong" pass.

    ------------------------------------------------------------------
    Component identity
    ------------------------------------------------------------------
    componentType     = 'imdc'  (image decompressor)
    componentSubType  = 'avc1'  (must match this exactly so QuickTime's
                                  Component Manager finds us when it opens
                                  a movie whose sample description is
                                  'avc1' -- this is not a cosmetic choice)
    componentManufacturer = 'wY9H'  (arbitrary but distinct from 'appl';
                                  see H264Codec.r)

    ------------------------------------------------------------------
    Pixel format we hand back to QuickTime
    ------------------------------------------------------------------
    We ask the Image Compression Manager for a plain 32-bit RGB
    destination (PreDecompress sets capabilities->wantedPixelSize = 32)
    and do the YCbCr->RGB conversion ourselves in CopyPlanarYUVToPixMap.

    The original plan was the opposite: hand QuickTime planar 4:2:0
    directly (k420YpCbCr8PlanarCodecType, 'y420') and let its own YUV
    blitter do the colour conversion, which is how Cinepak/MPEG-1/DV
    always worked on these machines and which would genuinely be faster.
    That was abandoned, at least for now, for a practical reason: a
    planar destination has to be negotiated through
    wantedDestinationPixelTypes and described with a
    PlanarPixmapInfoYUV420, and none of that can be debugged until the
    codec is loading and being called at all. 32-bit RGB needs no
    negotiation, works on every Mac OS 9 display path, and makes a wrong
    picture diagnosable by eye.

    Worth being explicit that these two halves must agree, because for
    several drafts they did not: the blit was still writing planar YUV
    into a buffer the ICM had formatted as RGB, which would have painted
    convincing-looking garbage rather than failing loudly. Switching to
    planar output later means changing BOTH PreDecompress and the blit,
    together.

    The conversion is BT.601 limited-range, table-driven, and was
    verified against a floating-point reference over ~5.6M input
    combinations before it ever ran on the Mac: no out-of-range table
    index, and a maximum deviation of 1 LSB (rounding).

    ------------------------------------------------------------------
    Threading / reentrancy
    ------------------------------------------------------------------
    Deliberately not attempted. Mac OS 9 QuickTime decompression happens
    on the caller's thread; h264bsd is not thread-safe across multiple
    simultaneous storage_t instances calling into shared globals (it
    isn't -- it's fully reentrant per storage_t instance, no global
    state), so this is actually fine for the single-movie-at-a-time case
    Base Codec gives us (one globals struct, hence one storage_t, per
    open component instance).
*/

/* H264_HOST_BUILD is defined only by the host test harness
   (tools/host/decode_run.c), which compiles THIS WHOLE FILE against its
   own small stand-ins for the Mac headers and then runs it. CodeWarrior
   never defines it, so on the Mac nothing here changes. Up to v13 the
   harness cut fragments out of this file by searching for marker lines,
   which broke every time the file was reorganised. */
#ifndef H264_HOST_BUILD
#include <Components.h>
#include <ImageCodec.h>
#include <QuickTimeComponents.h>
#include <Memory.h>
#include <Files.h>   /* FSMakeFSSpec/FSpOpenDF/FSWrite -- the logger below */
#include <Folders.h> /* FindFolder -- the logger looks for its file on the desktop */
#include <Timer.h>   /* Microseconds -- the playback statistics */
#include <Gestalt.h>
#include <string.h>   /* memset, used to clear the CodecInfo struct */

/* ComponentDispatchHelper.c (pulled in near the bottom of this file) builds
   a classic Mixed-Mode-Manager routine descriptor for our PowerPC component
   entry point -- that's what lets 68k/CFM-68k callers and the interpreted
   Component Manager glue call our native PPC code. The macros/constants it
   needs for that (kPascalStackBased, RESULT_SIZE, STACK_ROUTINE_PARAMETER,
   kFourByteCode, BUILD_ROUTINE_DESCRIPTOR) live in MixedMode.h. Some
   Universal Interfaces snapshots pull this in transitively through
   Components.h, but evidently not this one, since the first build choked
   on an undeclared "kPascalStackBased" right at that routine-descriptor
   line. Including it explicitly here is the fix -- and it's safe even if
   some other header already drags it in, since MixedMode.h is
   self-guarding against double inclusion like every other Universal
   Interfaces header. */
#include <MixedMode.h>

/* GetTimeBaseTime and TimeRecord, for the lateness policy (v14).
   QuickTimeComponents.h already includes Movies.h; naming it here just
   says what we depend on. */
#include <Movies.h>
#endif /* !H264_HOST_BUILD */

/* v17d: the AVCDecLib build (H264_BACKEND_AVCDEC 1) references no h264bsd
   header, type or function, so the old decoder is not linked in. The
   default for the switch has to be settled before the first test of it. */
#ifndef H264_BACKEND_AVCDEC
#define H264_BACKEND_AVCDEC 0
#endif
#if !H264_BACKEND_AVCDEC
#include "h264bsd_decoder.h"
#include "h264bsd_util.h"
#else
#include "basetype.h"   /* u8/u32 etc.: types only, no h264bsd function */
void *H264MemAlloc(unsigned long size);
void  H264MemFree(void *ptr);
#endif
#include "avcC_parse.h"
#include "H264Codec.h"

/* v16t: one source, two decoders. 0 (default) = h264bsd, linked in.
   1 = AVCDecLib (FFmpeg 0.8.15 H.264 in a separate LGPL 'shlb'), loaded at
   first use with GetSharedLibrary and called through a table of 8 entry
   points. Everything else (dispatch, pixel writers, lateness policy,
   logging, error survival) is shared. */
#if H264_BACKEND_AVCDEC
#include "AVCDecAPI.h"
#ifndef H264_HOST_BUILD
#include <CodeFragments.h>
#endif
#define H264_HAVE_DECODER(g) ((g)->avc != NULL)
#else
#define H264_HAVE_DECODER(g) ((g)->h264 != NULL)
#endif

/* ==================================================================
   Build configuration
   ==================================================================

   (v14: logging and the experiment switches are configured in the
   "v14 switches" block a little further down, after this section.)

   kOfferYUVOutput
   ---------------
   1 asks the Image Compression Manager for a YUV destination instead of
   32-bit RGB, in this order of preference:

     'y420'  planar 4:2:0 -- EXACTLY what h264bsd already produces, so
             our blit becomes three memcpys per frame and the colour
             conversion leaves OUR code. (It does not vanish: the screen
             is RGB, so QuickTime's transfer code or the video card's
             overlay still converts. Whether that is faster than our own
             table-driven converter on a G3 is for the Pismo to say.)
     '2vuy'  packed 4:2:2 -- needs chroma duplicated vertically and
             interleaved, but still no arithmetic per pixel, and it
             writes 2 bytes per pixel instead of 4.
     32-bit RGB -- what we do today, kept last as the guaranteed
             fallback.

   Why this is safe to turn on: the negotiation cannot produce a wrong
   picture, because the blit dispatches on what QuickTime ACTUALLY hands
   us at BandDecompress time, not on what we asked for. If the ICM
   ignores the list entirely and gives us a 32-bit PixMap, we take the
   RGB path exactly as before. Set to 0 to restore the old behaviour
   outright.

   Rough cost per pixel, for a sense of the prize:
     RGB   3 table lookups, 3 adds, 3 shifts, 3 clamp lookups, 4 stores
     2vuy  2 byte moves
     y420  one memcpy, amortised

   ------------------------------------------------------------------
   A note on PixMap.pixelFormat
   ------------------------------------------------------------------
   The name only exists when OLDPIXMAPSTRUCT is off; with it on, the
   same four bytes are called planeBytes. See H264_PIXMAP_FORMAT just
   below for why reading planeBytes is correct rather than a hack, and
   for how an earlier version of this block got it wrong. */

#define kOfferYUVOutput    1

/* ------------------------------------------------------------------
   v14 switches
   ------------------------------------------------------------------
   Up to v13 there was one compile-time switch, kH264Diagnostics, and
   two builds: a chatty one and a silent one. v14 is ONE quiet build
   that decides at run time:

     The decoder writes its statistics ONLY if the log file already
     exists. It never creates it. On a real Mac, make an empty text file
     called "H264 Decoder Log" on the desktop (SimpleText, Save) and the
     decoder appends a report every time a movie closes. Throw the file
     away and the decoder is silent again and makes no File Manager
     calls during playback at all. In the UTM VM, an existing
     share:h264log.txt is used in preference, as before.

   The switches below are for development and experiments. In a release
   build all of them are 0 except kH264LatenessPolicy. (Each sits inside
   an #ifndef only so that the host harness can build variants with -D;
   CodeWarrior always takes the value written here.) */

/* 1 = also write the old step-by-step lines (Open enter, PreDecompress
   width, the first few BandDecompress calls, ...) and the raw YUV dumps
   to the share. Still only if the log file exists. */
#ifndef kH264Verbose
#define kH264Verbose          0
#endif

/* 1 = when playback runs late, shed work so the picture stays with the
   sound. See "Lateness policy" above BandDecompressWork. With a decoder
   that keeps up this is inert: it only ever acts on a frame QuickTime
   delivers after its display time has passed. */
#ifndef kH264LatenessPolicy
#define kH264LatenessPolicy   1
#endif

/* Experiment (slow-CPU simulation): busy-wait this many milliseconds in
   every BandDecompress call, to see what QuickTime does with a decoder
   that cannot keep up. 0 = off. */
#ifndef kH264SimulateSlowMs
#define kH264SimulateSlowMs   0
#endif

/* Experiment: 1 = remember, for the first kTraceCalls BandDecompress
   calls of each instance, what QuickTime told us (frame number, frame
   time, time-base time, condition flags, caller flags) and what we did,
   and write the table to the log when the movie closes. Costs one
   NewPtr and nothing per call but a few stores. Only if the log file
   exists. */
/* v17c: the AVCDecLib build (H264 Decoder 17) ships with the trace on,
   exactly as the 17b guest build did; the h264bsd build keeps it off.
   -DkH264TimingTrace=0 still overrides it. */
#ifndef kH264TimingTrace
#if H264_BACKEND_AVCDEC
#define kH264TimingTrace      1
#else
#define kH264TimingTrace      0
#endif
#endif

/* Experiment: 1 = also offer 16-bit RGB (after the YUV formats, before
   32-bit RGB) to see what QuickTime hands over in Thousands. The 16-bit
   writer itself is always compiled in: if QuickTime ever gives us a
   16-bit RGB pixmap unasked, drawing it beats drawing nothing. */
#ifndef kOfferRGB16
#define kOfferRGB16           0
#endif

/* v17c experiment: 0 = do NOT offer 32-bit RGB in PreDecompress (only
   y420, 2vuy and, with kOfferRGB16, 16-bit RGB). 1 (default) = offer it
   last, as every version before 17c did. The 32-bit writer itself is
   always compiled in: if QuickTime hands us a 32-bit pixmap anyway (the
   poster frame drawn straight to the screen, say), BlitPicture paints it
   exactly as before -- it dispatches on the destination it is given,
   not on the list we offered. */
#ifndef kOfferRGB32
#define kOfferRGB32           1
#endif

/* ------------------------------------------------------------------
   Byte order, for the writers that store 32 bits at a time
   ------------------------------------------------------------------
   The Mac is big-endian and that is the only case that ships. The other
   branch exists so the host harness can run the very same loops on an
   Intel machine and compare the bytes they produce. gcc and clang define
   __BYTE_ORDER__; CodeWarrior does not, and falls through to big-endian.

   H264_PACK4(a,b,c,d)     a word whose bytes IN MEMORY are a b c d
   H264_PACK_2x16(p,q)     a word holding two big-endian 16-bit pixels */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    #define H264_PACK4(a, b, c, d) \
        (((u32)(d) << 24) | ((u32)(c) << 16) | ((u32)(b) << 8) | (u32)(a))
    #define H264_PACK_2x16(p, q) \
        H264_PACK4(((p) >> 8) & 0xFF, (p) & 0xFF, ((q) >> 8) & 0xFF, (q) & 0xFF)
#else
    #define H264_PACK4(a, b, c, d) \
        (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))
    #define H264_PACK_2x16(p, q)   (((u32)(p) << 16) | (u32)(q))
#endif

/*
    Reading the destination's pixel format.

    Universal Interfaces 3.4 declares the same four bytes of PixMap under
    two names. With OLDPIXMAPSTRUCT set the slot is `planeBytes`;
    without it, `pixelFormat`. The rename happened because QuickTime
    repurposed a field that chunky Mac pixmaps never used, and QuickTime
    4+ writes the destination's four-character pixel format there no
    matter which name the header gives it.

    The FIRST version of this block responded to OLDPIXMAPSTRUCT by
    switching the whole YUV path off. That was wrong, and it was wrong
    silently: classic (non-Carbon) builds get the old name by default,
    so the YUV code compiled away to nothing, the build reported no
    errors, and the only symptom was that H264Codec.c's object code came
    out SMALLER after 300 lines had been added to it -- 5852 bytes before,
    5580 after, with data up by only the 83 bytes that the YUV-off case
    predicts. The sizes are what gave it away.

    So: read the same slot under whichever name this build uses. For an
    ordinary 32-bit screen pixmap it holds 0 or 32, neither of which is
    a YUV code, and BlitPicture falls through to the RGB path on
    pixelSize exactly as before. */
#if defined(OLDPIXMAPSTRUCT) && OLDPIXMAPSTRUCT
    #define H264_PIXMAP_FORMAT(pm)  ((OSType)((pm).planeBytes))
#else
    #define H264_PIXMAP_FORMAT(pm)  ((pm).pixelFormat)
#endif

/* The globals record is defined a little further down; the loggers and
   the blit need its name before that. */
typedef struct H264Globals H264Globals;
typedef H264Globals *H264GlobalsPtr;

/* Forward declarations for the loggers, which are defined much further
   down this file.

   BlitPicture calls LogNum, and BlitPicture sits above the loggers. In
   C89 an undeclared call quietly invents `extern int LogNum()`, and the
   later `static void LogNum(...)` then collides with it -- a compile
   error in CodeWarrior.

   v14: every logger takes the instance, because whether to log at all
   is now decided per instance (does the log file exist?) and remembered
   in its globals, not in a static. A NULL instance logs nothing. */
static Boolean LogEnabled(H264GlobalsPtr glob);
static void LogAppend(H264GlobalsPtr glob, const char *msg);
static void LogNum(H264GlobalsPtr glob, const char *label, long v);
static void DumpBytesToShare(H264GlobalsPtr glob, const char *pathC,
                             const void *data, long len);

/* The step-by-step lines of the old diagnostic build. At kH264Verbose 0
   the compiler removes them entirely. */
#define VLogAppend(g, m)     do { if (kH264Verbose) LogAppend((g), (m)); } while (0)
#define VLogNum(g, l, v)     do { if (kH264Verbose) LogNum((g), (l), (v)); } while (0)

/* ==================================================================
   Frame-rate counters (diagnostics only)
   ==================================================================
   The one question the Pismo exists to answer is "can a 500 MHz G3 keep
   up?", and the movie's timecode cannot answer it: QuickTime's clock
   runs in real time regardless, and when the decoder falls behind it
   simply skips frames. So count what actually happened and log it when
   the movie closes:

     STATS pictures shown          pictures that reached BlitPicture
     STATS avg us per call         mean time inside BandDecompress --
                                   decode AND blit -- per call, timed
                                   only after the first 8 calls, because
                                   those carry the slow diagnostic file
                                   I/O and would skew the mean
     STATS max us per call         the slowest single call (same skip).
                                   A G3 that averages under budget but
                                   spikes over it will stutter; this is
                                   the number that shows it
     STATS avg/max us in blit      v17c: the same, for the destination
                                   writer alone (y420/2vuy/RGB32/RGB16),
                                   timed only on calls after the first 8
     STATS wall ms first->last     wall time from the first picture to
                                   the last, INCLUDING any pause. Since
                                   the poster-frame fix the first picture
                                   is drawn when the movie OPENS, so this
                                   includes however long the movie sat
                                   before Play was pressed -- measured:
                                   61 s for a 41 s clip. Kept for
                                   comparison with older logs; do not
                                   read it as a frame rate.
     STATS playback run ...        THE frame-rate number. A "run" is a
                                   stretch of pictures with no gap longer
                                   than kStatRunGapMicros between them;
                                   the longest run by duration is normal
                                   playback, and its pictures per second
                                   is the achieved frame rate. Pausing,
                                   the poster frame and QuickTime's
                                   redraw-from-last-key-frame bursts all
                                   fall outside it.

   Real time for a 29.97 fps clip means ~33,367 us per call or less, and
   a playback-run fps x100 near 2997. A machine that cannot keep up shows
   it two ways: the run's fps falls below the clip's rate, and "pictures
   shown" falls below the movie's frame count because QuickTime skips.

   Microseconds() fills an UnsignedWide. Only the low 32 bits are used,
   as unsigned differences: they wrap every ~71 minutes, and an unsigned
   subtraction across one wrap is still correct. No 64-bit arithmetic
   anywhere, deliberately: CodeWarrior 8 PPC miscompiles 64-bit
   shift-by-constant (see GetMovie_prefix.h). */
/* v14: the counters used to be file-level statics here (gStatPictures
   and friends). A component's statics live in the shared library's data
   section, and that is ONE copy for every instance in every application
   that has the component open: two movies playing at once added their
   pictures into the same counters, and a one-shot "already logged this"
   flag set by the first movie silenced the second. They are now fields
   of the per-instance globals record below, so the report at Close is
   about one movie.

   The only statics left in this file are the colour conversion tables,
   which are constants computed once and identical for everybody. */

#define kStatSkipCalls     8
#define kStatRunGapMicros  500000UL   /* half a second without a picture ends a run */

/* How many times in a row BandDecompress will hand h264bsd the same NAL
   when it consumes zero bytes. The API asks for exactly one retry (see
   the long comment in BandDecompressWork); 2 leaves room for one retry
   that itself reports another boundary, and still guarantees the loop
   ends. */
#define kMaxDecodeStalls  2

/* Calls remembered by the timing trace (kH264TimingTrace), and how many
   numbers are kept for each. 900 calls is 30 seconds at 30 fps. */
#define kTraceCalls   900
#define kTraceFields  19   /* v17: + 6 for the discontinuity trace */

/* ------------------------------------------------------------------
   Globals record
   ------------------------------------------------------------------
   One per open component instance. Open allocates it with NewPtrClear,
   so every field starts as zero / NULL / false, and the code below
   relies on that instead of initialising fields one by one. */

#define kMaxParamSets 8

struct H264Globals {
    ComponentInstance   self;
    ComponentInstance   target;        /* set by the Target selector */

#if !H264_BACKEND_AVCDEC
    storage_t          *h264;          /* h264bsd decoder instance */
    int                 h264Inited;    /* have we called h264bsdInit yet? */
#endif

    u32                 lengthSize;    /* NAL length-prefix width from avcC: 1/2/4 */

    u8                 *annexBScratch; /* scratch buffer for length->AnnexB conversion */
    u32                 annexBScratchSize;

    long                frameWidth;
    long                frameHeight;

    /* Cropping: h264bsd decodes in 16x16 macroblock multiples and reports
       the "clean aperture" crop separately (h264bsdCroppingParams). The
       ImageDescription's width/height is the *display* size we must
       actually paint into -- almost always smaller than or equal to the
       decoded macroblock-aligned buffer. */
    u32                 cropLeft, cropTop, cropWidth, cropHeight;
    u32                 haveCropInfo;

    /* The zero-terminated list of destination pixel formats we offer the
       ICM in PreDecompress. The CODEC owns this handle: we allocate it,
       point p->wantedDestinationPixelTypes at it, and dispose of it in
       Close. See the long comment in PreDecompress for why ownership
       runs this way round. */
    OSType            **wantedPixelTypes;

    /* The most recent picture h264bsd handed us, and a running count of
       pictures output on this instance. BandDecompress uses them to
       guarantee that a call which yields no new picture (QuickTime
       re-sending a sample it already sent, a sample that is only
       parameter sets or SEI, or a decode error) still leaves the last
       picture in the destination instead of whatever happened to be in
       QuickTime's buffer. The pointer is into h264bsd's DPB, so it is
       forgotten whenever the DPB can be reallocated, which is only ever
       at SPS activation -- the call that returns HDRS_RDY. It
       deliberately survives PreDecompress: the case it exists for is the
       poster-frame instance being re-prepared for playback and sent the
       same key frame again. */
    u8                 *lastPic;
    unsigned long       picsOut;

    /* ---- where the last blit went (v14) ----
       The lateness policy may skip painting a picture. That is only safe
       if the destination still holds the previous picture, i.e. if
       QuickTime handed us the same buffer and rectangle as last time.
       These remember the last destination actually painted. */
    /* v14: the avcC's SPS or PPS was refused by h264bsd -- in practice a
       Main or High profile stream (CABAC, 8x8 transforms, B-frames),
       which h264bsd cannot decode. See FeedParameterSets. */
    Boolean             unsupportedStream;
    u32                 streamProfile;      /* AVCProfileIndication from the avcC */

    Boolean             haveLastBlit;
    Ptr                 lastBlitBase;
    long                lastBlitRowBytes;
    OSType              lastBlitFmt;
    short               lastBlitSize;
    Rect                lastBlitRect;
    Boolean             skipBlitThisCall;   /* set per call by BandDecompressWork */

    /* ---- logging (v14: per instance, see LogEnabled) ---- */
    short               logState;       /* 0 untried, 1 log file exists, -1 no file: silent */
    FSSpec              logSpec;

    /* One-shot flags and small counters that used to be function-level
       statics (sBandCalls, sPicCount, sStallLogs, sLastFmt, ...). */
    long                bandCalls;
    long                stallLogs;
    Boolean             fmtSeen;        /* lastFmt/lastSize below are valid */
    OSType              lastFmt;
    short               lastSize;
    short               fmtReports;
    Boolean             moanedOnce;

    /* ---- lateness policy state (see "Lateness policy") ---- */
    long                lateStreak;     /* consecutive late calls */
    long                losingStreak;   /* consecutive late calls on which we fell further behind */
    long                onTimeStreak;   /* consecutive on-time calls */
    long                prevLateMs;

    /* The self-made clock (see H264LatenessMs), used because QuickTime 6
       passes no frameTime to a synchronous codec. */
    Boolean             scHavePrev;     /* a previous call has been seen */
    Boolean             scHaveReturn;   /* scReturnLo is valid */
    unsigned long       scPrevWallLo;   /* Microseconds() at the previous call's arrival */
    unsigned long       scReturnLo;     /* Microseconds() when the previous call returned */
    long                scPrevMediaMs;  /* previous frame's start time, ms */
    long                scLateMs;       /* current estimate */
    unsigned long       statSelfClockCalls;
    long                blitSkips;      /* pictures in a row decoded but not painted */
    Boolean             noDeblock;      /* deblocking filter currently off */
    Boolean             waitIDR;        /* dropping everything until a key frame */

    /* ---- statistics, reported at Close ---- */
    unsigned long       statPictures;
    unsigned long       statBandCalls;
    unsigned long       statTimedCalls;
    unsigned long       statDecodeMicros;
    unsigned long       statMaxMicros;      /* slowest timed call */
    unsigned long       statBlitTimed;      /* v17c: blits timed (after the first 8 calls) */
    unsigned long       statBlitMicros;     /* v17c: total us inside the destination writer */
    unsigned long       statBlitMaxMicros;  /* v17c: slowest timed blit */
    unsigned long       statFirstPicLo;
    unsigned long       statLastPicLo;
    unsigned long       statRepeats;        /* calls that re-blitted the last picture */
    unsigned long       statDecodeErrors;   /* NALs or samples h264bsd rejected */
    unsigned long       statConvertFails;   /* v15: samples AVCC->Annex-B refused */
    unsigned long       statScratchGrows;   /* v15: scratch regrown in BandDecompress */
    unsigned long       statPreBufferSize;  /* v15: bufferSize PreDecompress was given */
    unsigned long       statMaxSample;      /* v15: largest sample BandDecompress got */

    /* Which destination format each blit actually went to. The poster
       frame and playback can use DIFFERENT destinations -- measured
       2026-09-24: the poster frame is drawn straight to the 32-bit
       screen, playback into an offscreen '2vuy' buffer. */
    unsigned long       statBlitY420;
    unsigned long       statBlit2vuy;
    unsigned long       statBlitRGB32;
    unsigned long       statBlitRGB16;
    unsigned long       statBlitOther;

    unsigned long       statRunStartLo;     /* first picture of the current run */
    unsigned long       statRunPics;        /* pictures in the current run */
    unsigned long       statBestRunPics;    /* longest finished run, by duration */
    unsigned long       statBestRunMicros;

    unsigned long       statTimedFrames;    /* calls that carried a usable frame time */
    unsigned long       statLateCalls;      /* ... of which late */
    long                statMaxLateMs;
    unsigned long       statCatchUpCalls;   /* calls QuickTime flagged as catch-up */
    unsigned long       statBlitSkips;      /* pictures decoded but not painted */
    unsigned long       statNonRefSkips;    /* disposable frames not decoded */
    unsigned long       statDeblockOffs;    /* times the deblocking filter was switched off */
    unsigned long       statNoDeblockPics;  /* pictures finished without the filter */
    unsigned long       statIDRWaits;       /* times we gave up and waited for a key frame */
    unsigned long       statDroppedCalls;   /* samples not decoded while waiting */

    /* ---- timing trace (kH264TimingTrace) ---- */
    long               *trace;          /* kTraceCalls * kTraceFields longs, or NULL */
    long                traceCount;
    unsigned long       traceSum;       /* checksum of the last picture this call produced */
    Boolean             tracedThisCall; /* this call has a row in the trace */

#if H264_BACKEND_AVCDEC
    /* ---- AVCDecLib backend (v16t) ---- */
    H264AvcFns          av;             /* the library's 8 entry points */
    Boolean             avLoaded;
#ifndef H264_HOST_BUILD
    CFragConnectionID   avConn;
#endif
    AVCDecContext      *avc;            /* decoder context, or NULL */
    AVCDecPicture       avPic;          /* the picture lastPic belongs to */
    u8                 *avcCopy;        /* the avcC record, kept to reopen */
    long                avcCSize;
    Boolean             avAltiVec;      /* AltiVec requested from the library */
    Boolean             avInfoLogged;
    /* v17: fixed reorder delay (stage 5, B-frames). See H264AvSample. */
    long                avRdly;         /* N from the 'rdly' extension, -1 if none */
    long                avDelay;        /* N: pictures the decoder holds back */
    Boolean             avDelayFns;     /* library has the 0x00080F02 entry points */
    Boolean             avHavePrevSample; /* avPrevSize/avPrevSum are valid */
    Boolean             avHaveNext;     /* avNextLo: the start time (low word) a successor has */
    Boolean             avAfterReset;   /* paint the next decoded picture at once */
    Boolean             avGate;         /* open GOP: hide pictures before the key */
    long                avKeyPoc;
    unsigned long       avNextLo;
    long                avNextScale;
    long                avPrevFrameNumber;
    unsigned long       statResets;     /* discontinuities onto a key frame: decoder reset */
    unsigned long       statJumpsNoKey; /* discontinuities onto a non-key sample: no reset */
    unsigned long       statGated;      /* leading pictures not painted after a reset */
    unsigned long       statTail;       /* 1-byte samples: one held picture out */
    Boolean             avRdlyHolds;    /* v17b: N came from 'rdly': the component enforces it */
    unsigned long       statRdlyTakes;  /* pictures taken early to hold the delay at N */
    unsigned long       statRdlyLost;   /* pictures that came out together and were not shown */
    Boolean             avPicValid;
    Boolean             avMono;         /* v17c: after a non-key jump: no picture older than the last shown */
    Boolean             avLastPocValid;
    long                avLastPoc;      /* POC of the last picture shown */
    unsigned long       statStaleHidden; /* pictures not shown because older than one shown */     /* v17: lastPic was returned by the LAST library call */
    Boolean             avHaveSig;
    unsigned long       avDstSig;       /* first bytes of the destination after our last paint */
    unsigned long       statKept;       /* calls that left the destination as it was */
    unsigned long       statDstChanged; /* ... and found it changed behind our back */
    unsigned long       statBlackRepaints; /* repaint impossible (new buffer, stale picture): black */
    u8                 *avBuf;          /* v17: sample copy + kAvPad zero bytes (FFmpeg reads past the end) */
    unsigned long       avBufSize;
    unsigned long       avLibSpMod16;   /* r1 & 15 the library saw in avcdec_init */
    unsigned long       avSpBad;        /* calls with sp & 15 != 0 inside BandDecompress */
    unsigned long       statHeld;       /* samples decoded without a picture coming out */
    unsigned long       avPrevSum;      /* checksum and size of the last sample decoded */
    long                avPrevSize;
    unsigned long       statIdrRepeats; /* key frames re-sent back to back, not decoded */
#endif
};

/* Record one picture's arrival time for the run statistics. */
static void StatNotePicture(H264GlobalsPtr glob, unsigned long nowLo)
{
    if (glob->statPictures > 0 &&
        (unsigned long)(nowLo - glob->statLastPicLo) >= kStatRunGapMicros)
    {
        /* The gap ends the run that finished at statLastPicLo. */
        unsigned long dur = glob->statLastPicLo - glob->statRunStartLo;
        if (glob->statRunPics > 1 && dur > glob->statBestRunMicros)
        {
            glob->statBestRunMicros = dur;
            glob->statBestRunPics   = glob->statRunPics;
        }
        glob->statRunPics = 0;
    }

    if (glob->statPictures == 0) glob->statFirstPicLo = nowLo;
    if (glob->statRunPics == 0)  glob->statRunStartLo = nowLo;
    glob->statRunPics++;
    glob->statLastPicLo = nowLo;
    glob->statPictures++;
}

static void LogPlaybackStats(H264GlobalsPtr glob);   /* defined after BandDecompress */

/* ------------------------------------------------------------------
   Forward declarations of the routines Base Codec calls into.
   Names/signatures follow the standard IMAGECODEC_BASENAME() pattern
   used by every third-party ICM codec of this era (Sorenson, On2, etc).
   ------------------------------------------------------------------ */

pascal ComponentResult H264CodecOpen(H264GlobalsPtr glob, ComponentInstance self);
pascal ComponentResult H264CodecClose(H264GlobalsPtr glob, ComponentInstance self);
pascal ComponentResult H264CodecVersion(H264GlobalsPtr glob);
pascal ComponentResult H264CodecGetCodecInfo(H264GlobalsPtr glob, CodecInfo *info);

pascal ComponentResult H264CodecTarget(H264GlobalsPtr glob, ComponentInstance target);
/* no H264CodecCanDo -- ComponentDispatchHelper.c generates it; see below */
pascal ComponentResult H264CodecPreDecompress(H264GlobalsPtr glob,
                                               CodecDecompressParams *p);
pascal ComponentResult H264CodecBandDecompress(H264GlobalsPtr glob,
                                                CodecDecompressParams *params);

/* ==================================================================
   Internal helpers (not part of the Base Codec vtable)
   ================================================================== */

/* Feed the SPS/PPS NAL units pulled from the avcC box into h264bsd, each
   wrapped in an Annex-B start code, before any real sample arrives.
   Returns true if h264bsd accepted at least the SPS (PPS-without-SPS is
   a malformed-file situation we don't try to be clever about). */
#if H264_BACKEND_AVCDEC
static int FeedParameterSetsAvc(H264GlobalsPtr glob, const u8 *avcC, u32 avcCSize);
#define FeedParameterSets FeedParameterSetsAvc
#else
static int FeedParameterSets(H264GlobalsPtr glob, const u8 *avcC, u32 avcCSize)
{
    const u8 *sps[kMaxParamSets];
    u32       spsSizes[kMaxParamSets];
    int       numSps = 0;
    const u8 *pps[kMaxParamSets];
    u32       ppsSizes[kMaxParamSets];
    int       numPps = 0;
    int       i;
    u8        unitBuf[2048]; /* SPS/PPS are tiny -- a few dozen bytes, this is generous */

    if (!AVCC_Parse(avcC, avcCSize, &glob->lengthSize,
                     sps, spsSizes, kMaxParamSets, &numSps,
                     pps, ppsSizes, kMaxParamSets, &numPps))
    {
        return 0;
    }

    /* v14: h264bsd's answer to each parameter set is no longer thrown
       away. It refuses a PPS that asks for CABAC (entropy_coding_mode_flag,
       Main and High profile) and the extra SPS fields of High profile,
       and a stream whose parameter sets it refuses will never produce a
       picture. Measured 2026-11-11 in the VM: such a file used to "play"
       as a field of green noise with its sound, which looks like a broken
       decoder rather than an unsupported file. */
    glob->unsupportedStream = false;
    glob->streamProfile     = (avcCSize > 1) ? (u32)avcC[1] : 0;

    for (i = 0; i < numSps; i++)
    {
        u32 n = AVCC_WriteAnnexBUnit(sps[i], spsSizes[i], unitBuf, sizeof(unitBuf));
        u32 readBytes = 0;
        if (n == 0) return 0;
        if (h264bsdDecode(glob->h264, unitBuf, n, 0, &readBytes) == H264BSD_ERROR)
            glob->unsupportedStream = true;
    }
    for (i = 0; i < numPps; i++)
    {
        u32 n = AVCC_WriteAnnexBUnit(pps[i], ppsSizes[i], unitBuf, sizeof(unitBuf));
        u32 readBytes = 0;
        if (n == 0) return 0;
        if (h264bsdDecode(glob->h264, unitBuf, n, 0, &readBytes) == H264BSD_ERROR)
            glob->unsupportedStream = true;
    }

    return 1;
}
#endif /* !H264_BACKEND_AVCDEC */

/* ------------------------------------------------------------------
   YUV 4:2:0 -> 32-bit RGB conversion tables
   ------------------------------------------------------------------
   The previous version of the blit below copied h264bsd's planar YUV
   straight through, because the first draft intended to hand QuickTime
   k420YpCbCr8PlanarCodecType and let its own YUV blitter do the colour
   conversion. That would be faster, but it requires negotiating a planar
   destination through wantedDestinationPixelTypes and describing the
   plane offsets with a PlanarPixmapInfoYUV420 -- fiddly machinery to get
   right, and impossible to debug until the codec is running at all.

   PreDecompress now asks for a plain 32-bit RGB destination instead
   (capabilities->wantedPixelSize = 32), which every Mac OS 9 display path
   can supply without negotiation. So the conversion is ours to do, and
   the two halves finally agree -- before this, the blit wrote planar YUV
   into a buffer the ICM had formatted as RGB, which would have painted
   convincing-looking garbage.

   Conversion is BT.601 limited-range ("studio swing": Y 16-235,
   chroma 16-240), which is what H.264 signals by default and what
   h264bsd emits:

       C = Y - 16      D = Cb - 128     E = Cr - 128
       R = (298*C           + 409*E + 128) >> 8
       G = (298*C - 100*D   - 208*E + 128) >> 8
       B = (298*C + 516*D           + 128) >> 8

   Done with lookup tables rather than multiplies. A G3 has a real
   multiplier so this is not about avoiding the multiply -- it is about
   avoiding the clamp branches, which are what actually hurt on an
   in-order chip with a short pipeline. gClamp is a 1024-entry table
   covering the full over/underflow range of the expressions above, so
   the inner loop has no conditionals at all. Tables are built once on
   first use; they are ~5KB total and constant for the life of the
   component.

   The bias below is not arbitrary and is worth showing the arithmetic
   for, because getting it wrong is an out-of-bounds read that produces
   plausible-looking wrong pixels rather than a crash. Over all 8-bit
   inputs the pre-shift value spans:

       gYTab  : 298*(0-16)+128   .. 298*(255-16)+128  = -4640 .. 71350
       rv     : 409*(0-128)      .. 409*(255-128)     = -52352 .. 51943
       gu+gv  : -100*127-208*127 .. 100*128+208*128   = -39116 .. 39424
       bu     : 516*(0-128)      .. 516*(255-128)     = -66048 .. 65532

   so (Y + offset) spans -70688 .. 136882, and after >>8 that is
   -277 .. 534. A bias of 256 -- the obvious choice -- is therefore too
   small: the blue channel at minimum luma indexes -21. Bias 320 puts
   the used range at 43 .. 854, comfortably inside a 1024-entry table
   with headroom at both ends. */

#define kClampBias   320
#define kClampSize   1024

static Boolean gTablesReady = false;
static long    gYTab[256];    /* 298*(Y-16)                  */
static long    gRVTab[256];   /*  409*(Cr-128)               */
static long    gGUTab[256];   /* -100*(Cb-128)               */
static long    gGVTab[256];   /* -208*(Cr-128)               */
static long    gBUTab[256];   /*  516*(Cb-128)               */
static u8      gClamp[kClampSize]; /* saturate (x>>8)+kClampBias to 0-255 */

static void BuildColorTables(void)
{
    long i;

    if (gTablesReady) return;

    for (i = 0; i < 256; i++)
    {
        gYTab[i]  = 298 * (i - 16) + 128;  /* +128 folds in the rounding */
        gRVTab[i] =  409 * (i - 128);
        gGUTab[i] = -100 * (i - 128);
        gGVTab[i] = -208 * (i - 128);
        gBUTab[i] =  516 * (i - 128);
    }

    /* Indexed by (value >> 8) + kClampBias; see the bias derivation in
       the header comment above. */
    for (i = 0; i < kClampSize; i++)
    {
        long v = i - kClampBias;
        gClamp[i] = (u8)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    gTablesReady = true;
}

/* Convert h264bsd's planar 4:2:0 output into the destination PixMap as
   32-bit RGB, respecting rowBytes (QuickTime pads it -- never assume
   rowBytes == width*4) and the display crop rectangle.

   Chroma is subsampled 2x2, so each Cb/Cr pair serves a 2x2 block of
   luma. The loop walks two destination rows at a time and two pixels at
   a time, reading each chroma sample once and reusing it four times,
   which is both faster and simpler than recomputing chroma indices per
   pixel. Odd widths/heights are handled by the tail guards. */
/* ------------------------------------------------------------------
   Blit one decoded picture into QuickTime's destination.
   ------------------------------------------------------------------
   Rewritten after the first working decode, because the first version
   made two assumptions that the ICM does not honour, and the log of an
   actual BandDecompress call showed both:

       dstPixMap.pixelSize 32
       dstPixMap.rowBytes  5120          <- 1280 px * 4: a whole screen
       dstPixMap.bounds    (-24,-50)-(1256,718)
       dstRect             (88,24)-(264,168)   <- 176x144, our frame
       glob->frameWidth    0             <- !!
       glob->frameHeight   0

   1. THE DESTINATION IS NOT THE TOP-LEFT OF THE PIXMAP.
      QuickTime hands us a PixMap that can be the entire screen or
      window backing store, and tells us which rectangle inside it the
      frame belongs in. Both dstRect and the pixel data are in the
      pixmap's own coordinate system, whose origin is bounds.topLeft --
      which is routinely negative. The address of the frame's first
      pixel is therefore

          baseAddr + (dstRect.top  - bounds.top ) * rowBytes
                   + (dstRect.left - bounds.left) * 4

      not baseAddr. Writing at baseAddr paints the frame somewhere else
      entirely -- and with a 5120-byte stride, "somewhere else" is a
      long way from where anyone is looking.

   2. THE GEOMETRY MUST NOT COME FROM OUR GLOBALS.
      frameWidth was zero on the very call that was supposed to draw.
      QuickTime opens several instances of a codec while it negotiates,
      and the instance that receives BandDecompress is not always the
      one PreDecompress ran on -- so any geometry cached in per-instance
      globals can be missing exactly when it is needed. With srcW zero
      the blit loops run zero times and draw nothing at all.

      The decoder itself always knows, so ask it, every frame:
      h264bsdPicWidth/PicHeight are in macroblocks and cost nothing.
      The blit is now self-sufficient: given a valid decoder and the
      params block, it cannot be wrong about geometry.

   Clipping is against dstRect as well as the decoded size, so a
   destination smaller than the frame truncates instead of scribbling
   past the end of the row. */
/* v14: ONE 32-BIT STORE PER PIXEL.
   Up to v13 the inner loop wrote each pixel as four separate byte
   stores (dst[0]=0, dst[1]=R, dst[2]=G, dst[3]=B). When QuickTime hands
   us the screen itself -- which is what it does in Millions -- those are
   four writes into video memory per pixel, 345,600 of them per 360x240
   frame, and on the Pismo that path managed 21.6 fps where the 2vuy
   path managed 29.4. The pixel is now assembled in a register and
   stored once. The bytes that land in memory are identical (0 R G B);
   tools/host checks the old and the new writer against each other and
   against an independent conversion, byte for byte. The arithmetic and
   the tables are untouched.

   The loop is also split so the "is there a second row" and "is there a
   second pixel" tests are made once per row pair, not once per pixel. */
#define H264_RGB32_FROM(yv) \
    H264_PACK4(0, \
               gClamp[(((yv) + rv) >> 8) + kClampBias], \
               gClamp[(((yv) + gu) >> 8) + kClampBias], \
               gClamp[(((yv) + bu) >> 8) + kClampBias])

/* v16t: where the picture to paint lives. h264bsd hands over one
   contiguous planar buffer (coded size, crop reported separately);
   AVCDecLib hands over three planes with their own strides, already
   cropped. The writers take everything from here. */
typedef struct
{
    const u8 *y, *cb, *cr;
    long      yStride, cStride;
    u32       srcW, srcH;
    u32       cropFlag, cropL, cropW, cropT, cropH;
} H264Src;

static void H264GetSrc(H264GlobalsPtr glob, const u8 *yuv, H264Src *s)
{
#if H264_BACKEND_AVCDEC
    (void)yuv;
    s->y  = glob->avPic.plane[0];
    s->cb = glob->avPic.plane[1];
    s->cr = glob->avPic.plane[2];
    s->yStride = glob->avPic.stride[0];
    s->cStride = glob->avPic.stride[1];
    s->srcW = (u32)glob->avPic.width;
    s->srcH = (u32)glob->avPic.height;
    s->cropFlag = 0; s->cropL = s->cropW = s->cropT = s->cropH = 0;
    if (s->y == NULL || s->cb == NULL || s->cr == NULL) s->srcW = s->srcH = 0;
#else
    s->srcW = (u32)h264bsdPicWidth(glob->h264)  * 16;
    s->srcH = (u32)h264bsdPicHeight(glob->h264) * 16;
    s->cropFlag = s->cropL = s->cropW = s->cropT = s->cropH = 0;
    h264bsdCroppingParams(glob->h264, &s->cropFlag, &s->cropL, &s->cropW, &s->cropT, &s->cropH);
    s->y  = yuv;
    s->cb = yuv + ((long)s->srcW * s->srcH);
    s->cr = s->cb + (((long)s->srcW * s->srcH) >> 2);
    s->yStride = (long)s->srcW;
    s->cStride = (long)(s->srcW >> 1);
#endif
}

static void CopyPlanarYUVToPixMap(H264GlobalsPtr glob, const u8 *yuv,
                                   CodecDecompressParams *params)
{
    u32 srcW, srcH;
    u32 dstW, dstH;
    u32 left = 0, top = 0;
    u32 cropFlag = 0, cropL = 0, cropW = 0, cropT = 0, cropH = 0;
    H264Src sp;
    long lumaStride;

    const u8 *yPlane;
    const u8 *cbPlane;
    const u8 *crPlane;
    u32 chromaStride;

    Ptr  baseAddr;
    long rowBytes;
    long originX, originY;
    u8  *frameBase;
    u32  yy, pairs;

    if (glob == NULL || !H264_HAVE_DECODER(glob) || yuv == NULL || params == NULL) return;

    /* Geometry straight from the decoder -- never from our globals. */
    H264GetSrc(glob, yuv, &sp);
    srcW = sp.srcW;
    srcH = sp.srcH;
    if (srcW == 0 || srcH == 0) return;

    /* h264bsd decodes in whole macroblocks and reports the visible
       window separately; honour it so we paint the clean aperture. */
    cropFlag = sp.cropFlag; cropL = sp.cropL; cropW = sp.cropW;
    cropT = sp.cropT; cropH = sp.cropH;
    if (cropFlag && cropW > 0 && cropH > 0)
    {
        dstW = cropW; dstH = cropH; left = cropL; top = cropT;
    }
    else
    {
        dstW = srcW;  dstH = srcH;
    }

    baseAddr = (Ptr)params->dstPixMap.baseAddr;
    rowBytes = (long)(params->dstPixMap.rowBytes & 0x3FFF);
    if (baseAddr == NULL || rowBytes <= 0) return;

    /* Clip to the rectangle QuickTime actually asked us to fill. */
    {
        long rectW = (long)params->dstRect.right  - (long)params->dstRect.left;
        long rectH = (long)params->dstRect.bottom - (long)params->dstRect.top;
        if (rectW <= 0 || rectH <= 0) return;
        if ((long)dstW > rectW) dstW = (u32)rectW;
        if ((long)dstH > rectH) dstH = (u32)rectH;
    }

    /* Pixmap coordinates are relative to bounds.topLeft, which is
       frequently negative. This is the line the first version missed. */
    originX = (long)params->dstRect.left - (long)params->dstPixMap.bounds.left;
    originY = (long)params->dstRect.top  - (long)params->dstPixMap.bounds.top;
    if (originX < 0 || originY < 0) return;

    frameBase = (u8 *)baseAddr + originY * rowBytes + originX * 4;

    yPlane  = sp.y;
    cbPlane = sp.cb;
    crPlane = sp.cr;
    chromaStride = (u32)sp.cStride;
    lumaStride = sp.yStride;

    BuildColorTables();

    pairs = dstW >> 1;

    for (yy = 0; yy < dstH; yy += 2)
    {
        const u8 *yRow0 = yPlane + (long)(yy + top) * lumaStride + left;
        const u8 *yRow1 = yRow0 + lumaStride;

        u32 cRow = ((yy + top) >> 1);
        const u8 *cbRow = cbPlane + (long)cRow * chromaStride + (left >> 1);
        const u8 *crRow = crPlane + (long)cRow * chromaStride + (left >> 1);

        /* A 32-bit pixmap's rows start on 4-byte boundaries, so these
           are aligned stores. (A PowerPC would perform a misaligned one
           correctly anyway, only more slowly.) */
        u32 *dst0 = (u32 *)(frameBase + (long)yy * rowBytes);
        u32 *dst1 = (u32 *)((u8 *)dst0 + rowBytes);

        long rv, gu, bu, yv, cb, cr;
        u32  xx;

        if (yy + 1 < dstH)
        {
            /* The usual case: two rows share one row of chroma. */
            for (xx = pairs; xx > 0; xx--)
            {
                cb = *cbRow++;
                cr = *crRow++;
                rv = gRVTab[cr];
                gu = gGUTab[cb] + gGVTab[cr];
                bu = gBUTab[cb];

                /* Mac 32-bit PixMap pixel order is xRGB: byte 0 unused,
                   then R, G, B. */
                yv = gYTab[yRow0[0]]; dst0[0] = H264_RGB32_FROM(yv);
                yv = gYTab[yRow0[1]]; dst0[1] = H264_RGB32_FROM(yv);
                yv = gYTab[yRow1[0]]; dst1[0] = H264_RGB32_FROM(yv);
                yv = gYTab[yRow1[1]]; dst1[1] = H264_RGB32_FROM(yv);
                yRow0 += 2; yRow1 += 2;
                dst0  += 2; dst1  += 2;
            }
            if (dstW & 1)
            {
                /* odd visible width: one last column */
                cb = *cbRow;
                cr = *crRow;
                rv = gRVTab[cr];
                gu = gGUTab[cb] + gGVTab[cr];
                bu = gBUTab[cb];
                yv = gYTab[yRow0[0]]; dst0[0] = H264_RGB32_FROM(yv);
                yv = gYTab[yRow1[0]]; dst1[0] = H264_RGB32_FROM(yv);
            }
        }
        else
        {
            /* odd visible height: the last row stands alone */
            for (xx = pairs; xx > 0; xx--)
            {
                cb = *cbRow++;
                cr = *crRow++;
                rv = gRVTab[cr];
                gu = gGUTab[cb] + gGVTab[cr];
                bu = gBUTab[cb];
                yv = gYTab[yRow0[0]]; dst0[0] = H264_RGB32_FROM(yv);
                yv = gYTab[yRow0[1]]; dst0[1] = H264_RGB32_FROM(yv);
                yRow0 += 2;
                dst0  += 2;
            }
            if (dstW & 1)
            {
                cb = *cbRow;
                cr = *crRow;
                rv = gRVTab[cr];
                gu = gGUTab[cb] + gGVTab[cr];
                bu = gBUTab[cb];
                yv = gYTab[yRow0[0]]; dst0[0] = H264_RGB32_FROM(yv);
            }
        }
    }
}

/* ------------------------------------------------------------------
   16-bit RGB ("Thousands"), new in v14
   ------------------------------------------------------------------
   A Mac 16-bit pixel is big-endian x RRRRR GGGGG BBBBB: the top bit
   unused, then five bits each. Same tables and arithmetic as the 32-bit
   writer; each component is cut to its top five bits (no dithering),
   and two pixels are stored as one 32-bit word.

   Nothing asks for this format unless kOfferRGB16 is set -- in
   Thousands QuickTime has so far always taken our '2vuy' and converted
   it itself. It is here for the experiment "offer 16-bit RGB and see
   what QuickTime hands over", and because up to v13 a 16-bit RGB
   destination fell through to "UNHANDLED" and drew nothing. */
#define H264_RGB16_FROM(yv) \
    ( ((u32)(gClamp[(((yv) + rv) >> 8) + kClampBias] >> 3) << 10) | \
      ((u32)(gClamp[(((yv) + gu) >> 8) + kClampBias] >> 3) <<  5) | \
       (u32)(gClamp[(((yv) + bu) >> 8) + kClampBias] >> 3) )

static void CopyPlanarYUVToRGB16(H264GlobalsPtr glob, const u8 *yuv,
                                  CodecDecompressParams *params)
{
    u32 srcW, srcH;
    u32 dstW, dstH;
    u32 left = 0, top = 0;
    u32 cropFlag = 0, cropL = 0, cropW = 0, cropT = 0, cropH = 0;
    H264Src sp;
    long lumaStride;

    const u8 *yPlane;
    const u8 *cbPlane;
    const u8 *crPlane;
    u32 chromaStride;

    Ptr  baseAddr;
    long rowBytes;
    long originX, originY;
    u8  *frameBase;
    u32  yy, pairs;

    if (glob == NULL || !H264_HAVE_DECODER(glob) || yuv == NULL || params == NULL) return;

    H264GetSrc(glob, yuv, &sp);
    srcW = sp.srcW;
    srcH = sp.srcH;
    if (srcW == 0 || srcH == 0) return;

    cropFlag = sp.cropFlag; cropL = sp.cropL; cropW = sp.cropW;
    cropT = sp.cropT; cropH = sp.cropH;
    if (cropFlag && cropW > 0 && cropH > 0)
    {
        dstW = cropW; dstH = cropH; left = cropL; top = cropT;
    }
    else
    {
        dstW = srcW;  dstH = srcH;
    }

    baseAddr = (Ptr)params->dstPixMap.baseAddr;
    rowBytes = (long)(params->dstPixMap.rowBytes & 0x3FFF);
    if (baseAddr == NULL || rowBytes <= 0) return;

    {
        long rectW = (long)params->dstRect.right  - (long)params->dstRect.left;
        long rectH = (long)params->dstRect.bottom - (long)params->dstRect.top;
        if (rectW <= 0 || rectH <= 0) return;
        if ((long)dstW > rectW) dstW = (u32)rectW;
        if ((long)dstH > rectH) dstH = (u32)rectH;
    }

    originX = (long)params->dstRect.left - (long)params->dstPixMap.bounds.left;
    originY = (long)params->dstRect.top  - (long)params->dstPixMap.bounds.top;
    if (originX < 0 || originY < 0) return;

    frameBase = (u8 *)baseAddr + originY * rowBytes + originX * 2;

    yPlane  = sp.y;
    cbPlane = sp.cb;
    crPlane = sp.cr;
    chromaStride = (u32)sp.cStride;
    lumaStride = sp.yStride;

    BuildColorTables();

    pairs = dstW >> 1;

    /* One destination row at a time: simpler than the 32-bit writer's
       row pairs, and it handles an odd crop origin without special
       cases. The chroma row is simply read twice. */
    for (yy = 0; yy < dstH; yy++)
    {
        const u8 *yRow  = yPlane + (long)(yy + top) * lumaStride + left;
        u32       cRow  = ((yy + top) >> 1);
        const u8 *cbRow = cbPlane + (long)cRow * chromaStride + (left >> 1);
        const u8 *crRow = crPlane + (long)cRow * chromaStride + (left >> 1);
        u32      *dst   = (u32 *)(frameBase + (long)yy * rowBytes);
        long rv, gu, bu, yv, cb, cr;
        u32  xx, p0, p1;

        for (xx = pairs; xx > 0; xx--)
        {
            cb = *cbRow++;
            cr = *crRow++;
            rv = gRVTab[cr];
            gu = gGUTab[cb] + gGVTab[cr];
            bu = gBUTab[cb];
            yv = gYTab[yRow[0]]; p0 = H264_RGB16_FROM(yv);
            yv = gYTab[yRow[1]]; p1 = H264_RGB16_FROM(yv);
            *dst++ = H264_PACK_2x16(p0, p1);
            yRow += 2;
        }
        if (dstW & 1)
        {
            u8 *d8 = (u8 *)dst;
            cb = *cbRow;
            cr = *crRow;
            rv = gRVTab[cr];
            gu = gGUTab[cb] + gGVTab[cr];
            bu = gBUTab[cb];
            yv = gYTab[yRow[0]]; p0 = H264_RGB16_FROM(yv);
            d8[0] = (u8)(p0 >> 8);
            d8[1] = (u8)p0;
        }
    }
}

#if kOfferYUVOutput

/* ==================================================================
   The YUV output paths
   ==================================================================
   h264bsd hands us planar 4:2:0 -- a Y plane at full resolution
   followed by Cb and Cr planes at half resolution in both directions.
   That is byte-for-byte what 'y420' wants, and two-thirds of what
   '2vuy' wants. The colour conversion above exists only because a
   32-bit RGB destination needs one; given a YUV destination WE do not
   have to do it. QuickTime or the video hardware still does, on its way
   to the RGB screen -- the gain is that their converter may be faster
   than ours, not that the work disappears.

   These two functions deliberately repeat the geometry work that
   CopyPlanarYUVToPixMap does rather than refactoring it out. The RGB
   blit is the one path known to produce a correct picture on real
   hardware, and it stays untouched. A shared helper would be tidier and
   would put the working path at risk of a change made while nobody was
   watching; that is a bad trade. If all three paths are still here in a
   month and all three are proven, factor it then.
   ================================================================== */

/* Common setup: decoder geometry, crop, clipping to dstRect, and the
   pixmap-relative origin. Returns false if there is nothing to draw.
   Everything it reports is in terms of the DECODED picture (srcW/srcH
   are macroblock-aligned) and the VISIBLE window (dstW/dstH after crop
   and clip), with left/top the crop origin inside the decoded picture. */
typedef struct
{
    u32  srcW, srcH;
    u32  dstW, dstH;
    const u8 *y, *cb, *cr;      /* v16t: planes and strides from H264GetSrc */
    long yStride, cStride;
    u32  left, top;
    long originX, originY;
    long rowBytes;
    Ptr  baseAddr;
} H264BlitGeom;

static Boolean ComputeBlitGeometry(H264GlobalsPtr glob, const u8 *yuv,
                                    CodecDecompressParams *params,
                                    H264BlitGeom *g)
{
    u32 cropFlag = 0, cropL = 0, cropW = 0, cropT = 0, cropH = 0;
    H264Src sp;
    long lumaStride;

    g->left = 0;
    g->top  = 0;

    /* Geometry from the decoder, never from our globals -- the instance
       that receives BandDecompress is not always the one PreDecompress
       ran on, so cached geometry can read back as zero exactly when it
       is needed. This cost us a blank picture once already. */
    H264GetSrc(glob, yuv, &sp);
    g->srcW = sp.srcW;
    g->srcH = sp.srcH;
    g->y = sp.y; g->cb = sp.cb; g->cr = sp.cr;
    g->yStride = sp.yStride; g->cStride = sp.cStride;
    (void)lumaStride;
    if (g->srcW == 0 || g->srcH == 0) return false;

    cropFlag = sp.cropFlag; cropL = sp.cropL; cropW = sp.cropW;
    cropT = sp.cropT; cropH = sp.cropH;
    if (cropFlag && cropW > 0 && cropH > 0)
    {
        g->dstW = cropW; g->dstH = cropH;
        g->left = cropL; g->top  = cropT;
    }
    else
    {
        g->dstW = g->srcW; g->dstH = g->srcH;
    }

    g->baseAddr = (Ptr)params->dstPixMap.baseAddr;
    g->rowBytes = (long)(params->dstPixMap.rowBytes & 0x3FFF);
    if (g->baseAddr == NULL) return false;

    {
        long rectW = (long)params->dstRect.right  - (long)params->dstRect.left;
        long rectH = (long)params->dstRect.bottom - (long)params->dstRect.top;
        if (rectW <= 0 || rectH <= 0) return false;
        if ((long)g->dstW > rectW) g->dstW = (u32)rectW;
        if ((long)g->dstH > rectH) g->dstH = (u32)rectH;
    }

    g->originX = (long)params->dstRect.left - (long)params->dstPixMap.bounds.left;
    g->originY = (long)params->dstRect.top  - (long)params->dstPixMap.bounds.top;
    if (g->originX < 0 || g->originY < 0) return false;

    /* Chroma is shared across 2x2 luma blocks, so an odd origin would
       pair each chroma sample with the wrong luma pair and swap Cb/Cr
       across the row. Round down to even: that shifts the picture by at
       most one pixel, which nobody can see, where getting it wrong
       produces obviously wrong colour. In practice QuickTime's video
       rectangles are even-aligned anyway and this never fires. */
    g->originX &= ~1L;
    g->originY &= ~1L;

    return true;
}

/* ------------------------------------------------------------------
   '2vuy' -- packed 4:2:2, byte order Cb Y0 Cr Y1 per pixel pair.
   ------------------------------------------------------------------
   Two bytes per pixel instead of four, and no arithmetic at all: every
   output byte is a copy of an input byte. Chroma is duplicated down the
   vertical (4:2:0 has half as many chroma rows as 4:2:2 needs) rather
   than interpolated. Duplication is what every fast converter of this
   era did and the difference is not visible in motion; interpolating
   would put a multiply back in the inner loop, which is the entire
   thing we are trying to avoid. */
/* v14: one 32-bit store per pixel pair instead of four byte stores. The
   four bytes of a '2vuy' pair (Cb Y0 Cr Y1) are exactly one word, so
   the pair is assembled in a register and written once. Same bytes in
   memory as before; tools/host compares old and new byte for byte. */
static void CopyPlanarYUVTo2vuy(H264GlobalsPtr glob, const u8 *yuv,
                                 CodecDecompressParams *params)
{
    H264BlitGeom g;
    const u8 *yPlane, *cbPlane, *crPlane;
    u32 chromaStride, yy, pairs;
    u8 *frameBase;

    if (!ComputeBlitGeometry(glob, yuv, params, &g)) return;
    if (g.rowBytes <= 0) return;

    yPlane  = g.y;
    cbPlane = g.cb;
    crPlane = g.cr;
    chromaStride = (u32)g.cStride;

    frameBase = (u8 *)g.baseAddr + g.originY * g.rowBytes + g.originX * 2;

    pairs = g.dstW >> 1;

    for (yy = 0; yy < g.dstH; yy++)
    {
        const u8 *yRow  = yPlane + (long)(yy + g.top) * g.yStride + g.left;
        u32       cRow  = ((yy + g.top) >> 1);
        const u8 *cbRow = cbPlane + (long)cRow * chromaStride + (g.left >> 1);
        const u8 *crRow = crPlane + (long)cRow * chromaStride + (g.left >> 1);
        /* originX is even (ComputeBlitGeometry) so the row starts on a
           4-byte boundary whenever the pixmap's rows do. */
        u32      *dst   = (u32 *)(frameBase + (long)yy * g.rowBytes);
        u32       xx;

        for (xx = pairs; xx > 0; xx--)
        {
            *dst++ = H264_PACK4(cbRow[0], yRow[0], crRow[0], yRow[1]);
            cbRow++;
            crRow++;
            yRow += 2;
        }

        /* Odd visible width: one luma sample left over. Duplicate it
           into the pair's second slot so the macroblock edge does not
           show as a black column. */
        if (g.dstW & 1)
        {
            u8 *d8 = (u8 *)dst;
            d8[0] = *cbRow;
            d8[1] = *yRow;
            d8[2] = *crRow;
            d8[3] = d8[1];
        }
    }
}

/* ------------------------------------------------------------------
   'y420' -- planar 4:2:0, which is exactly h264bsd's own output.
   ------------------------------------------------------------------
   The destination is not a flat pixmap: baseAddr points at a
   PlanarPixmapInfoYUV420 (declared in ImageCodec.h) whose three
   PlanarComponentInfo records give each plane's offset -- measured from
   the start of that struct, not from the pixmap -- and its own
   rowBytes. The pixmap's own rowBytes is meaningless here and is not
   used.

   With that read, the blit is three runs of BlockMoveData per row group
   and no per-pixel work whatsoever. This is the cheapest OUR output
   stage can possibly be. Whether the whole pipeline is cheaper depends
   on what QuickTime does with the planes next, which is measured on
   hardware, not argued from here. */
static void CopyPlanarYUVToY420(H264GlobalsPtr glob, const u8 *yuv,
                                 CodecDecompressParams *params)
{
    H264BlitGeom g;
    PlanarPixmapInfoYUV420 *ppi;
    const u8 *yPlane, *cbPlane, *crPlane;
    u8   *dstY, *dstCb, *dstCr;
    long  yRB, cbRB, crRB;
    u32   chromaStride, yy, halfH, halfW;

    if (!ComputeBlitGeometry(glob, yuv, params, &g)) return;

    ppi = (PlanarPixmapInfoYUV420 *)g.baseAddr;

    yRB  = (long)ppi->componentInfoY.rowBytes;
    cbRB = (long)ppi->componentInfoCb.rowBytes;
    crRB = (long)ppi->componentInfoCr.rowBytes;

    /* A plane stride narrower than the picture means we have
       misunderstood the buffer; draw nothing rather than run off the
       end of somebody else's memory. */
    if (yRB  < (long)g.dstW      || cbRB < (long)(g.dstW >> 1)
     || crRB < (long)(g.dstW >> 1)) return;

    dstY  = (u8 *)ppi + ppi->componentInfoY.offset;
    dstCb = (u8 *)ppi + ppi->componentInfoCb.offset;
    dstCr = (u8 *)ppi + ppi->componentInfoCr.offset;

    dstY  += g.originY * yRB + g.originX;
    dstCb += (g.originY >> 1) * cbRB + (g.originX >> 1);
    dstCr += (g.originY >> 1) * crRB + (g.originX >> 1);

    yPlane  = g.y;
    cbPlane = g.cb;
    crPlane = g.cr;
    chromaStride = (u32)g.cStride;

    for (yy = 0; yy < g.dstH; yy++)
    {
        BlockMoveData(yPlane + (long)(yy + g.top) * g.yStride + g.left,
                      dstY + (long)yy * yRB,
                      (Size)g.dstW);
    }

    halfH = g.dstH >> 1;
    halfW = g.dstW >> 1;

    for (yy = 0; yy < halfH; yy++)
    {
        long srcRow = (long)(yy + (g.top >> 1)) * chromaStride + (g.left >> 1);
        BlockMoveData(cbPlane + srcRow, dstCb + (long)yy * cbRB, (Size)halfW);
        BlockMoveData(crPlane + srcRow, dstCr + (long)yy * crRB, (Size)halfW);
    }
}

#endif /* kOfferYUVOutput */

/* ------------------------------------------------------------------
   Dispatch on what QuickTime ACTUALLY handed us.
   ------------------------------------------------------------------
   Not on what we asked for in PreDecompress. Asking is not getting --
   that lesson is already written into the log lines in BandDecompress
   -- and this is what makes offering YUV safe: if the ICM ignores the
   wanted list entirely we simply take the RGB path, and the picture is
   still right.

   An unrecognised format draws nothing rather than guessing. A blank
   window is a diagnosable symptom; a wrong-format blit into a screen
   backing store is convincing-looking garbage at best and somebody
   else's memory at worst. */

/* Fill the destination rectangle with black, in whatever format it is.
   Used only for a stream this codec cannot decode (see
   FeedParameterSets), so that the viewer sees a plain black picture with
   the sound, not leftover memory. Geometry from the pixmap alone: there
   is no decoded picture to take it from. */
static void PaintBlack(CodecDecompressParams *params)
{
    OSType fmt  = H264_PIXMAP_FORMAT(params->dstPixMap);
    short  size = params->dstPixMap.pixelSize;
    long   w    = (long)params->dstRect.right  - (long)params->dstRect.left;
    long   h    = (long)params->dstRect.bottom - (long)params->dstRect.top;
    long   ox   = (long)params->dstRect.left - (long)params->dstPixMap.bounds.left;
    long   oy   = (long)params->dstRect.top  - (long)params->dstPixMap.bounds.top;
    long   rb   = (long)(params->dstPixMap.rowBytes & 0x3FFF);
    u8    *base = (u8 *)params->dstPixMap.baseAddr;
    long   y, x;

    if (base == NULL || w <= 0 || h <= 0 || ox < 0 || oy < 0) return;

    if (fmt == kYUV420CodecType)
    {
        PlanarPixmapInfoYUV420 *ppi = (PlanarPixmapInfoYUV420 *)base;
        u8 *py  = base + ppi->componentInfoY.offset;
        u8 *pcb = base + ppi->componentInfoCb.offset;
        u8 *pcr = base + ppi->componentInfoCr.offset;
        long yRB = (long)ppi->componentInfoY.rowBytes;
        long cRB = (long)ppi->componentInfoCb.rowBytes;
        long rRB = (long)ppi->componentInfoCr.rowBytes;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) py[(oy + y) * yRB + ox + x] = 16;
        for (y = 0; y < h / 2; y++)
            for (x = 0; x < w / 2; x++)
            {
                pcb[(oy / 2 + y) * cRB + ox / 2 + x] = 128;
                pcr[(oy / 2 + y) * rRB + ox / 2 + x] = 128;
            }
        return;
    }
    if (rb <= 0) return;
    if (fmt == k422YpCbCr8CodecType)
    {
        ox &= ~1L;
        for (y = 0; y < h; y++)
        {
            u8 *d = base + (oy + y) * rb + ox * 2;
            for (x = 0; x + 1 < w; x += 2) { d[0] = 128; d[1] = 16; d[2] = 128; d[3] = 16; d += 4; }
        }
        return;
    }
    if (size == 32 || size == 16)
    {
        long bpp = size / 8;
        for (y = 0; y < h; y++)
        {
            u8 *d = base + (oy + y) * rb + ox * bpp;
            for (x = 0; x < w * bpp; x++) d[x] = 0;
        }
    }
}

/* Is this the same buffer and rectangle the last blit on this instance
   painted? If so it still holds the last picture, and a call that
   produces nothing new may leave it alone. */
static Boolean SameDestinationAsLastBlit(H264GlobalsPtr glob,
                                         CodecDecompressParams *params)
{
    if (!glob->haveLastBlit) return false;
    return (Boolean)(
        glob->lastBlitBase     == (Ptr)params->dstPixMap.baseAddr &&
        glob->lastBlitRowBytes == (long)(params->dstPixMap.rowBytes & 0x3FFF) &&
        glob->lastBlitFmt      == H264_PIXMAP_FORMAT(params->dstPixMap) &&
        glob->lastBlitSize     == params->dstPixMap.pixelSize &&
        glob->lastBlitRect.top    == params->dstRect.top &&
        glob->lastBlitRect.left   == params->dstRect.left &&
        glob->lastBlitRect.bottom == params->dstRect.bottom &&
        glob->lastBlitRect.right  == params->dstRect.right);
}

/* The host harness's simulated clock charges for each blit through this
   hook. Empty on the Mac. */
#ifndef H264_HOST_BLIT_HOOK
#define H264_HOST_BLIT_HOOK()
#endif

#if H264_BACKEND_AVCDEC
/* v17: a checksum of the first (up to) 16 bytes of the destination's
   first row: lets the trace show whether the destination still holds
   what we painted when we leave it alone. */
static unsigned long H264DstSig(CodecDecompressParams *params)
{
    OSType fmt = H264_PIXMAP_FORMAT(params->dstPixMap);
    long   ox  = (long)params->dstRect.left - (long)params->dstPixMap.bounds.left;
    long   oy  = (long)params->dstRect.top  - (long)params->dstPixMap.bounds.top;
    long   rb  = (long)(params->dstPixMap.rowBytes & 0x3FFF), bpp, k;
    const u8 *base = (const u8 *)params->dstPixMap.baseAddr, *p;
    unsigned long a = 1, b = 0;
    if (base == NULL || ox < 0 || oy < 0) return 0;
    if (fmt == kYUV420CodecType)
    {
        const PlanarPixmapInfoYUV420 *ppi = (const PlanarPixmapInfoYUV420 *)base;
        p = base + ppi->componentInfoY.offset + oy * (long)ppi->componentInfoY.rowBytes + ox;
    }
    else
    {
        bpp = (fmt == k422YpCbCr8CodecType) ? 2 : params->dstPixMap.pixelSize / 8;
        if (fmt == k422YpCbCr8CodecType) ox &= ~1L;
        p = base + oy * rb + ox * bpp;
    }
    for (k = 0; k < 16; k++) { a += p[k]; b += a; }
    return (b << 16) ^ a;
}
#endif

static void NoteBlit(H264GlobalsPtr glob, CodecDecompressParams *params)
{
    H264_HOST_BLIT_HOOK();
#if H264_BACKEND_AVCDEC
    glob->avDstSig  = H264DstSig(params);
    glob->avHaveSig = true;
#endif
    glob->haveLastBlit     = true;
    glob->lastBlitBase     = (Ptr)params->dstPixMap.baseAddr;
    glob->lastBlitRowBytes = (long)(params->dstPixMap.rowBytes & 0x3FFF);
    glob->lastBlitFmt      = H264_PIXMAP_FORMAT(params->dstPixMap);
    glob->lastBlitSize     = params->dstPixMap.pixelSize;
    glob->lastBlitRect     = params->dstRect;
}

/* v17c: time one destination write (the y420 / 2vuy / 32-bit / 16-bit
   writer plus NoteBlit) with the same Microseconds() clock and the same
   "after the first 8 calls" rule as "STATS avg us per call". Only while
   this instance is logging, so a quiet playback reads no extra clock.
   Reported at Close as "STATS avg/max us in blit (after first 8)". */
static Boolean BlitTimeStart(H264GlobalsPtr glob, UnsignedWide *t0)
{
    if (glob->statBandCalls < kStatSkipCalls || !LogEnabled(glob)) return false;
    Microseconds(t0);
    return true;
}

static void BlitTimeEnd(H264GlobalsPtr glob, Boolean timing, const UnsignedWide *t0)
{
    UnsignedWide  t1;
    unsigned long us;

    if (!timing) return;
    Microseconds(&t1);
    us = (unsigned long)(t1.lo - t0->lo);   /* unsigned: wrap-safe */
    glob->statBlitMicros += us;
    if (us > glob->statBlitMaxMicros) glob->statBlitMaxMicros = us;
    glob->statBlitTimed++;
}

static void BlitPicture(H264GlobalsPtr glob, const u8 *yuv,
                        CodecDecompressParams *params)
{
    OSType       fmt;
    UnsignedWide bt0;
    Boolean      timing;

    if (glob == NULL || !H264_HAVE_DECODER(glob) || yuv == NULL || params == NULL)
        return;

    fmt = H264_PIXMAP_FORMAT(params->dstPixMap);

    /* Report what QuickTime ACTUALLY handed us, whenever the destination
       CHANGES (poster frame to the screen, then playback offscreen, for
       instance), capped at six reports per instance so a destination
       that flips every frame cannot flood the log. Printed as signed
       decimal because LogNum has no hex; decode with
           'y420' = 2033463856   '2vuy' = 846624121
           32 or 0 with pixelSize 32 = 32-bit RGB, 16 or 0 with
           pixelSize 16 = 16-bit RGB
       Written only if the log file exists. */
    if ((!glob->fmtSeen || fmt != glob->lastFmt ||
         params->dstPixMap.pixelSize != glob->lastSize) &&
        glob->fmtReports < 6)
    {
        glob->fmtSeen  = true;
        glob->lastFmt  = fmt;
        glob->lastSize = params->dstPixMap.pixelSize;
        glob->fmtReports++;
        if (LogEnabled(glob))
        {
            LogNum(glob, "BlitPicture: dst pixelFormat", (long)fmt);
            LogNum(glob, "BlitPicture: dst pixelSize", (long)params->dstPixMap.pixelSize);
            LogNum(glob, "BlitPicture: dst rowBytes (masked)",
                   (long)(params->dstPixMap.rowBytes & 0x3FFF));
#if kOfferYUVOutput
            if (fmt == kYUV420CodecType && params->dstPixMap.baseAddr != NULL)
            {
                PlanarPixmapInfoYUV420 *ppi =
                    (PlanarPixmapInfoYUV420 *)params->dstPixMap.baseAddr;
                LogNum(glob, "  y420 Y  offset",   (long)ppi->componentInfoY.offset);
                LogNum(glob, "  y420 Y  rowBytes", (long)ppi->componentInfoY.rowBytes);
                LogNum(glob, "  y420 Cb offset",   (long)ppi->componentInfoCb.offset);
                LogNum(glob, "  y420 Cb rowBytes", (long)ppi->componentInfoCb.rowBytes);
                LogNum(glob, "  y420 Cr offset",   (long)ppi->componentInfoCr.offset);
                LogNum(glob, "  y420 Cr rowBytes", (long)ppi->componentInfoCr.rowBytes);
            }
#endif
        }
    }

#if kOfferYUVOutput
    if (fmt == kYUV420CodecType)
    {
        timing = BlitTimeStart(glob, &bt0);
        CopyPlanarYUVToY420(glob, yuv, params);
        glob->statBlitY420++;
        NoteBlit(glob, params);
        BlitTimeEnd(glob, timing, &bt0);
        return;
    }
    if (fmt == k422YpCbCr8CodecType)
    {
        timing = BlitTimeStart(glob, &bt0);
        CopyPlanarYUVTo2vuy(glob, yuv, params);
        glob->statBlit2vuy++;
        NoteBlit(glob, params);
        BlitTimeEnd(glob, timing, &bt0);
        return;
    }
#endif

    if (params->dstPixMap.pixelSize == 32)
    {
        timing = BlitTimeStart(glob, &bt0);
        CopyPlanarYUVToPixMap(glob, yuv, params);
        glob->statBlitRGB32++;
        NoteBlit(glob, params);
        BlitTimeEnd(glob, timing, &bt0);
        return;
    }

    /* 16-bit RGB. The format slot of an ordinary 16-bit pixmap holds 0
       or 16 (k16BE555PixelFormat); anything else with pixelSize 16 is
       some format we do not know (a little-endian or 5-6-5 layout, say)
       and is left alone. */
    if (params->dstPixMap.pixelSize == 16 && (fmt == 0 || fmt == 16))
    {
        timing = BlitTimeStart(glob, &bt0);
        CopyPlanarYUVToRGB16(glob, yuv, params);
        glob->statBlitRGB16++;
        NoteBlit(glob, params);
        BlitTimeEnd(glob, timing, &bt0);
        return;
    }

    glob->statBlitOther++;
    if (!glob->moanedOnce)
    {
        glob->moanedOnce = true;
        LogNum(glob, "  UNHANDLED destination pixelSize",
               (long)params->dstPixMap.pixelSize);
        LogNum(glob, "  UNHANDLED destination pixelFormat", (long)fmt);
    }
}

/* ==================================================================
   Base Codec entry points
   ================================================================== */

/* v14 NOTE, read first: the text below describes the logger as it was
   built while the component was being brought up. Two things changed in
   v14. The logger never creates its file any more -- it writes only if
   the file already exists (see LogEnabled) -- and whether it is on is
   remembered per instance instead of in a static. */
/* ==================================================================
   Component-side logging
   ==================================================================

   Why a component needs its own log
   ---------------------------------
   ComponentProbe can see everything *around* the component -- that it is
   registered, that its 'cfrg' is well formed, that CFM prepares the
   fragment without error, that both exported symbols resolve -- and then
   it hits OpenComponent(), which answers every possible failure with the
   same bare NULL. No error code, no error string, nothing. Session 6
   burned a full build/install/reboot cycle establishing only that the
   NULL is not a Code Fragment Manager problem.

   The one place that still has the information is inside the component
   itself, so the component now says what it is doing. Each entry point
   appends a line to

       share:h264log.txt

   which is a plain file on the shared volume and therefore readable from
   the host with one shell command, exactly like the probe's report.

   What the log distinguishes, all of which look identical from outside:
     - file absent entirely      -> our code was never called at all; the
                                    Component Manager gave up before
                                    reaching the 'dlle' entry point
     - "Open enter" then nothing -> we crashed or hung inside Open
     - "Open enter" + a failure  -> we returned an error, and the line
                                    says which one

   Deliberately built on File Manager calls only -- no stdio, no malloc,
   no MSL anything. A component is loaded into whatever host application
   happens to want it, and the MSL runtime in a shared library is exactly
   the sort of thing that may not have been initialized. A logger that
   depends on the runtime it is meant to be debugging is useless. Even
   strlen is hand-rolled below for that reason.

   Slow (open/append/close/flush per line) on purpose: if the component
   dies mid-call, everything written so far is already on disk. Speed
   does not matter for a diagnostic that only runs a few dozen times. */

/* ==================================================================
   Diagnostics master switch
   ==================================================================
   The logger and the raw-YUV dumps below were what made this port
   debuggable -- the dump in particular is what proved the decoder,
   not the blit, was producing a grey first frame. They are also
   ruinously slow: every line and every frame opens, appends, closes and
   FlushVol's a file on a VirtFS shared volume, which on a 176x144 clip
   costs far more than decoding it.

   v14: the switch that brings the step-by-step lines and the dumps back
   is kH264Verbose, in the "v14 switches" block at the top of this file.
   The code stays in the file on purpose: the next hard bug will want
   it, and rewriting it from memory would cost more than the few hundred
   bytes of dead code. */

#define kLogPathC   "share:h264log.txt"

/* Where the log goes when there is no "share" volume.

   The share only exists inside the UTM VM, where the host reads it.
   On a real Mac -- the Pismo, the TiBook -- FSMakeFSSpec on
   "share:h264log.txt" fails with nsvErr, and the logger used to fall
   silent there: exactly the place a log is most wanted, since nobody can
   read that machine's state from outside. So it now falls back to a
   text file on the boot volume's DESKTOP, located with FindFolder so it
   works whatever the volume is called:

       <boot volume>:Desktop Folder:H264 Decoder Log

   which appears as an icon on the desktop and opens in SimpleText.

   The raw-YUV dumps (DumpBytesToShare) deliberately do NOT fall back:
   they are 130K each and only useful to someone with ffmpeg on the far
   end of the share. */
/* (Inside #ifndef only because the host harness's compiler has no "\p"
   Pascal strings and supplies the same name spelled with a length byte.) */
#ifndef kLogFallbackName
#define kLogFallbackName  "\pH264 Decoder Log"
#endif

static short LogCStrLen(const char *s)
{
    short n = 0;
    while (s[n] != '\0' && n < 1000) n++;
    return n;
}

/* ------------------------------------------------------------------
   v14: log only if the log file ALREADY EXISTS
   ------------------------------------------------------------------
   Decided once per instance, on the first thing that instance wants to
   log, and remembered in its globals (logState). The answer is "yes"
   only if FSMakeFSSpec returns noErr -- the file is there -- for

       share:h264log.txt                          (the UTM VM), or
       <boot volume>:Desktop Folder:H264 Decoder Log   (a real Mac)

   fnfErr used to count as success and the file was then created. It no
   longer is: a decoder that drops a file on somebody's desktop every
   time a movie plays is a diagnostic build, and this is meant to be the
   build people keep. To get a report, create the file first (an empty
   SimpleText document saved as "H264 Decoder Log" on the desktop is
   enough); to silence the decoder, throw it away. An instance that
   found no file stays silent for the rest of its life and never asks
   again, so a quiet playback makes three file-system calls in total
   (FSMakeFSSpec, FindFolder, FSMakeFSSpec), all on its first call. */
static Boolean LogEnabled(H264GlobalsPtr glob)
{
    if (glob == NULL) return false;

    if (glob->logState == 0)
    {
        Str255 path;
        short  n;
        short  vRefNum;
        long   dirID;

        glob->logState = -1;

        n = LogCStrLen(kLogPathC);
        path[0] = (unsigned char)n;
        BlockMoveData(kLogPathC, path + 1, n);

        if (FSMakeFSSpec(0, 0, path, &glob->logSpec) == noErr)
        {
            glob->logState = 1;
        }
        else if (FindFolder(kOnSystemDisk, kDesktopFolderType,
                            kDontCreateFolder, &vRefNum, &dirID) == noErr &&
                 FSMakeFSSpec(vRefNum, dirID, kLogFallbackName,
                              &glob->logSpec) == noErr)
        {
            glob->logState = 1;
        }
    }

    return (Boolean)(glob->logState == 1);
}

/* Append `count` bytes to the log as they are. The caller supplies the
   line endings (CR). Used for single lines and for the timing trace,
   which writes many lines in one go. */
static void LogWrite(H264GlobalsPtr glob, const char *bytes, long count)
{
    short  refNum;
    OSErr  err;

    if (!LogEnabled(glob) || count <= 0) return;

    err = FSpOpenDF(&glob->logSpec, fsWrPerm, &refNum);
    if (err != noErr)
    {
        /* The file went away (or is open elsewhere with write access).
           Do not create it, and stop trying. */
        if (err == fnfErr) glob->logState = -1;
        return;
    }

    SetFPos(refNum, fsFromLEOF, 0);
    FSWrite(refNum, &count, (Ptr)bytes);
    FSClose(refNum);
    FlushVol(NULL, glob->logSpec.vRefNum);
}

static void LogAppend(H264GlobalsPtr glob, const char *msg)
{
    char  line[160];
    short n = 0;

    if (!LogEnabled(glob)) return;

    while (msg[n] != '\0' && n < 158) { line[n] = msg[n]; n++; }
    line[n++] = '\r';
    LogWrite(glob, line, (long)n);
}

/* ------------------------------------------------------------------
   Raw-buffer dump, for comparing against a known-good reference
   ------------------------------------------------------------------
   Writes the exact bytes h264bsd handed us to the shared volume, where
   the host can decode the same frame with ffmpeg and compare the two
   planar buffers directly. This is what proved, early on, that the
   decoder and not the blit was producing a grey first frame.

   Development only: compiled out unless kH264Verbose is 1, and even
   then it runs only when the log file exists. It is the one place that
   still CREATES a file, on the share. */
static void DumpBytesToShare(H264GlobalsPtr glob, const char *pathC,
                             const void *data, long len)
{
    Str255 path;
    FSSpec spec;
    short  refNum;
    long   count;
    short  n;
    OSErr  err;

    if (!kH264Verbose) return;
    if (!LogEnabled(glob)) return;
    if (data == NULL || len <= 0) return;

    n = LogCStrLen(pathC);
    path[0] = (unsigned char)n;
    BlockMoveData(pathC, path + 1, n);

    err = FSMakeFSSpec(0, 0, path, &spec);
    if (err == fnfErr)
    {
        if (FSpCreate(&spec, 'ttxt', 'BINA', 0) != noErr) return;
    }
    else if (err != noErr)
    {
        return;
    }

    if (FSpOpenDF(&spec, fsWrPerm, &refNum) != noErr) return;

    SetEOF(refNum, 0);
    count = len;
    FSWrite(refNum, &count, (Ptr)data);
    FSClose(refNum);
    FlushVol(NULL, spec.vRefNum);
}

/* Write a signed decimal number at buf, return how many characters. No
   sprintf, for the same runtime-independence reason as the logger. */
static short LogFormatNum(char *buf, long v)
{
    char          digits[16];
    short         i = 0, t = 0, j;
    unsigned long uv;

    if (v < 0) { buf[i++] = '-'; uv = (unsigned long)(-v); }
    else       { uv = (unsigned long)v; }

    if (uv == 0) digits[t++] = '0';
    while (uv > 0 && t < 15) { digits[t++] = (char)('0' + (uv % 10)); uv /= 10; }
    for (j = (short)(t - 1); j >= 0; j--) buf[i++] = digits[j];
    return i;
}

/* "label <signed decimal>" */
static void LogNum(H264GlobalsPtr glob, const char *label, long v)
{
    char  buf[128];
    short i = 0;

    if (!LogEnabled(glob)) return;

    while (label[i] != '\0' && i < 100) { buf[i] = label[i]; i++; }
    buf[i++] = ' ';
    i = (short)(i + LogFormatNum(buf + i, v));
    buf[i] = '\0';
    LogAppend(glob, buf);
}

/* ==================================================================
   Memory for the decoder (v14)
   ==================================================================
   h264bsd used to call malloc and free. In this shared library that is
   MSL's pooled allocator, and its pool bookkeeping is ordinary static
   data -- one copy, shared by every application that has the component
   open, while the blocks it describes come from whichever heap was
   current when the pool grew. That is a hazard as soon as two
   applications use the decoder, or one of them quits first.

   The decoder now asks for memory through H264MemAlloc / H264MemFree
   (see h264bsd_util.h), and those are implemented here on the Memory
   Manager, with no bookkeeping of our own beyond a 16-byte header in
   front of each block:

     1. NewPtr, in the current heap zone -- the application that is
        playing the movie, which is where malloc's memory ultimately
        came from before. A nonrelocatable block remembers its own zone,
        so DisposePtr is correct whichever application is current when
        the block is freed.
     2. If that fails and the block is big (a picture buffer, typically),
        temporary memory: TempNewHandle, locked for its whole life. That
        is the system's pool of unused RAM, and it is what lets a movie
        play in an application whose own partition is too small for the
        decoder's picture buffers.

   The header says which of the two a block is, so H264MemFree needs no
   lookup. It is 16 bytes so that the caller's block keeps the 16-byte
   alignment the Memory Manager gives PowerPC blocks. Freeing NULL does
   nothing, like free(). A block whose header does not carry one of our
   two marks is not ours and is left strictly alone: leaking it is a
   nuisance, disposing of a wild pointer is a crash. */

#define kH264MemMarkPtr     0x48345074L    /* 'H4Pt' */
#define kH264MemMarkTemp    0x4834546DL    /* 'H4Tm' */
#define kH264TempMinBytes   16384UL        /* smaller blocks never use temporary memory */

typedef struct
{
    long   mark;        /* one of the two marks above; cleared on free */
    Handle tempH;       /* the temporary-memory handle, or NULL */
    long   size;        /* bytes the caller asked for */
    long   pad;         /* keeps the header at 16 bytes on PowerPC */
} H264MemHeader;

/* v16t: the block handed back is ALWAYS 16-byte aligned. Neither NewPtr
   nor a locked temporary handle promises that, and AVCDecLib's AltiVec
   code needs it, so 15 spare bytes are taken and the header is placed
   just below the first aligned address. pad keeps where the Ptr really
   starts. */
#define H264_ALIGN16(a) (((unsigned long)(a) + 15UL) & ~15UL)
void *H264MemAlloc(unsigned long size)
{
    H264MemHeader *hdr;
    char          *raw;
    Size           total;

    if (size == 0) size = 4;    /* like malloc(0): hand back something freeable */
    total = (Size)(size + sizeof(H264MemHeader) + 15);

    raw = (char *)NewPtr(total);
    if (raw != NULL)
    {
        hdr = ((H264MemHeader *)H264_ALIGN16(raw + sizeof(H264MemHeader))) - 1;
        hdr->mark  = kH264MemMarkPtr;
        hdr->tempH = NULL;
        hdr->size  = (long)size;
        hdr->pad   = (long)raw;
        return (void *)(hdr + 1);
    }

    /* v16t: with AVCDecLib every size may fall back -- FFmpeg 0.8 does not
       check all of its small allocations. */
    if (size >= kH264TempMinBytes || H264_BACKEND_AVCDEC)
    {
        OSErr  err = noErr;
        Handle h   = TempNewHandle(total, &err);
        if (h != NULL && err == noErr)
        {
            HLock(h);
            raw = (char *)*h;
            hdr = ((H264MemHeader *)H264_ALIGN16(raw + sizeof(H264MemHeader))) - 1;
            hdr->mark  = kH264MemMarkTemp;
            hdr->tempH = h;
            hdr->size  = (long)size;
            hdr->pad   = (long)raw;
            return (void *)(hdr + 1);
        }
        if (h != NULL) DisposeHandle(h);
    }
    return NULL;
}

void H264MemFree(void *ptr)
{
    H264MemHeader *hdr;

    if (ptr == NULL) return;

    hdr = ((H264MemHeader *)ptr) - 1;

    if (hdr->mark == kH264MemMarkPtr)
    {
        hdr->mark = 0;
        DisposePtr((Ptr)hdr->pad);
    }
    else if (hdr->mark == kH264MemMarkTemp)
    {
        Handle h = hdr->tempH;
        hdr->mark = 0;
        HUnlock(h);
        DisposeHandle(h);
    }
    /* else: not one of ours. Leave it. */
}

#if H264_BACKEND_AVCDEC
/* ==================================================================
   v16t: the AVCDecLib backend
   ==================================================================
   AVCDecLib is FFmpeg 0.8.15's H.264 decoder in its own CFM shared
   library (LGPL). This component only loads it and calls its 8 plain C
   entry points through glob->av; nothing of FFmpeg is compiled in here.

   The library allocates through our H264MemAlloc/H264MemFree (16-byte
   aligned, application heap first, temporary memory second). No log
   callback: FFmpeg reports every concealed error, and writing those to
   the shared volume would cost more than the decode. */

#ifdef H264_HOST_BUILD
/* the host harness links AVCDecLib's objects directly */
unsigned long  avcdec_version(void);
long           avcdec_init(const AVCDecCallbacks *, unsigned long, unsigned long *);
AVCDecContext *avcdec_open(const unsigned char *, long);
long           avcdec_feed_annexb(AVCDecContext *, const unsigned char *, long, long *, AVCDecPicture *);
long           avcdec_decode_sample(AVCDecContext *, const unsigned char *, long, AVCDecPicture *);
long           avcdec_flush(AVCDecContext *, AVCDecPicture *);
void           avcdec_reset(AVCDecContext *);
void           avcdec_close(AVCDecContext *);
long           avcdec_set_skip(AVCDecContext *, long, long);
long           avcdec_get_delay(AVCDecContext *);
long           avcdec_set_delay(AVCDecContext *, long);
long           avcdec_take_lowest(AVCDecContext *, AVCDecPicture *, long *);
long           avcdec_held(AVCDecContext *);
long           avcdec_peek_lowest(AVCDecContext *, AVCDecPicture *, long *);
#ifndef H264_HOST_AVC_DECODE
#define H264_HOST_AVC_DECODE avcdec_decode_sample
#endif
#ifndef H264_HOST_AVC_WRAP
#define H264_HOST_AVC_WRAP(n) avcdec_##n
#endif
#endif

/* v17: every call into AVCDecLib goes through H264_AV, which first marks
   the picture we hold (lastPic, avPic) as no longer readable: after any
   later library call FFmpeg may have released or reused its buffer. */
#define H264_AV(g) (*((g)->avPicValid = false, &(g)->av))
/* v17c: which exports invalidate (checked against avcdec.c, 0x00080F02):
     decode_sample, take_lowest, peek_lowest, reset, flush, feed_annexb,
     open, close, init  -> H264_AV: they decode, release, refill c->frame
                           or free pictures.
     get_delay, held     -> H264_AVQ: read has_b_frames / count the queue.
     set_delay, set_skip -> H264_AVQ: store avctx/H264Context fields only;
                           no picture is released or written.
     version             -> no context at all. */
#define H264_AVQ(g) ((g)->av)

static void *H264AvAlloc(void *opaque, unsigned long size)
{
    (void)opaque;
    return H264MemAlloc(size);
}

static void H264AvRelease(void *opaque, void *ptr)
{
    (void)opaque;
    H264MemFree(ptr);
}

/* r1, the stack pointer, as the caller sees it. AltiVec code saves
   vector registers with 16-byte stores that silently drop the low 4
   address bits, so FFmpeg's AltiVec paths need r1 & 15 == 0. */
#if defined(H264_HOST_BUILD)
static unsigned long H264GetSP(void)
{
#if defined(__powerpc__)
    unsigned long sp;
    __asm__ volatile ("mr %0,1" : "=r" (sp));
    return sp;
#else
    return (unsigned long)__builtin_frame_address(0);
#endif
}
#else
static asm unsigned long H264GetSP(void)
{
    nofralloc
    mr      r3, r1
    blr
}
#endif

static Boolean H264HasAltiVec(void)
{
    long resp = 0;
    /* gestaltPowerPCProcessorFeatures 'ppcf', bit
       gestaltPowerPCHasVectorInstructions (4) */
    if (Gestalt('ppcf', &resp) != noErr) return false;
    return (Boolean)((resp & (1L << 4)) != 0);
}

/* Find AVCDecLib and its 8 entry points. Once per instance: a CFM
   connection belongs to the process that made it, and QuickTime opens
   instances in every application that plays a movie. */
static OSErr H264LoadAvc(H264GlobalsPtr glob)
{
    unsigned long ver;
    if (glob->avLoaded) return noErr;
#ifdef H264_HOST_BUILD
    if (gHostAvcMissing)
    {
        LogNum(glob, "UNSUPPORTED (black): AVCDecLib not found; GetSharedLibrary err", -2804L);
        return -8962;
    }
    glob->av.version       = H264_HOST_AVC_WRAP(version);
    glob->av.init          = H264_HOST_AVC_WRAP(init);
    glob->av.open          = H264_HOST_AVC_WRAP(open);
    glob->av.feed_annexb   = H264_HOST_AVC_WRAP(feed_annexb);
    glob->av.decode_sample = H264_HOST_AVC_DECODE;
    glob->av.flush         = H264_HOST_AVC_WRAP(flush);
    glob->av.reset         = H264_HOST_AVC_WRAP(reset);
    glob->av.close         = H264_HOST_AVC_WRAP(close);
    glob->av.set_skip      = H264_HOST_AVC_WRAP(set_skip);
    glob->av.get_delay     = H264_HOST_AVC_WRAP(get_delay);
    glob->av.set_delay     = H264_HOST_AVC_WRAP(set_delay);
    glob->av.take_lowest   = H264_HOST_AVC_WRAP(take_lowest);
    glob->av.held          = H264_HOST_AVC_WRAP(held);
    glob->av.peek_lowest   = H264_HOST_AVC_WRAP(peek_lowest);
#else
    {
        static const unsigned char *const kNames[14] = {
            "\pavcdec_version", "\pavcdec_init", "\pavcdec_open",
            "\pavcdec_feed_annexb", "\pavcdec_decode_sample",
            "\pavcdec_flush", "\pavcdec_reset", "\pavcdec_close",
            "\pavcdec_set_skip", "\pavcdec_get_delay", "\pavcdec_set_delay",
            "\pavcdec_take_lowest", "\pavcdec_held", "\pavcdec_peek_lowest" };
        Ptr       mainAddr = NULL;
        Str255    errName;
        OSErr     err, err2 = noErr;
        int       i;
        Ptr      *slot = (Ptr *)&glob->av;

        /* 1. by name: CFM looks in the application's folder, the
              Extensions folder and the System file */
        err = GetSharedLibrary("\pAVCDecLib", kPowerPCCFragArch, kReferenceCFrag,
                               &glob->avConn, &mainAddr, errName);
        if (err == noErr)
            LogAppend(glob, "AVCDecLib found by GetSharedLibrary");
        else
        {
            /* 2. by file: Extensions folder, then the System Folder */
            short  vRef;
            long   dirID;
            FSSpec spec;
            OSType where[2];
            int    w;
            where[0] = kExtensionFolderType;
            where[1] = kSystemFolderType;
            err2 = fnfErr;
            for (w = 0; w < 2 && err2 != noErr; w++)
            {
                if (FindFolder(kOnSystemDisk, where[w], kDontCreateFolder, &vRef, &dirID) == noErr &&
                    FSMakeFSSpec(vRef, dirID, "\pAVCDecLib", &spec) == noErr)
                    err2 = GetDiskFragment(&spec, 0, kCFragGoesToEOF, "\pAVCDecLib",
                                           kReferenceCFrag, &glob->avConn, &mainAddr, errName);
            }
            if (err2 != noErr)
            {
                LogNum(glob, "UNSUPPORTED (black): AVCDecLib not found; GetSharedLibrary err", (long)err);
                return -8962;   /* codecUnimpErr */
            }
            LogNum(glob, "AVCDecLib found by GetDiskFragment (GetSharedLibrary err)", (long)err);
        }
        for (i = 0; i < 8; i++)
        {
            CFragSymbolClass cls;
            Ptr addr = NULL;
            if (FindSymbol(glob->avConn, kNames[i], &addr, &cls) != noErr || addr == NULL)
            {
                LogNum(glob, "UNSUPPORTED (black): AVCDecLib lacks export #", (long)i);
                CloseConnection(&glob->avConn);
                return -8962;
            }
            slot[i] = addr;
        }
        /* v17: the 6 delay/skip entry points, only from a library that
           says it has them; a missing one means "old library". */
        if (glob->av.version() >= kAVCDecVersionSkipDelay)
            for (i = 8; i < 14; i++)
            {
                CFragSymbolClass cls;
                Ptr addr = NULL;
                if (FindSymbol(glob->avConn, kNames[i], &addr, &cls) != noErr) addr = NULL;
                slot[i] = addr;
            }
    }
#endif
    ver = glob->av.version();
#ifdef H264_HOST_BUILD
    if (gHostAvcFakeVersion != 0) ver = gHostAvcFakeVersion;
#endif
    glob->avDelayFns = (Boolean)(ver >= kAVCDecVersionSkipDelay && glob->av.set_skip != NULL &&
                                 glob->av.get_delay != NULL && glob->av.set_delay != NULL &&
                                 glob->av.take_lowest != NULL && glob->av.held != NULL &&
                                 glob->av.peek_lowest != NULL);
    if (!glob->avDelayFns)
    {
        glob->av.set_skip = NULL; glob->av.get_delay = NULL; glob->av.set_delay = NULL;
        glob->av.take_lowest = NULL; glob->av.held = NULL; glob->av.peek_lowest = NULL;
    }
    if (ver < kAVCDecMinVersion || (ver >> 24) != 0)
    {
        LogNum(glob, "UNSUPPORTED (black): AVCDecLib version", (long)ver);
#ifndef H264_HOST_BUILD
        CloseConnection(&glob->avConn);
#endif
        return -8962;
    }
    glob->avLoaded = true;
    return noErr;
}

/* ---- a small SPS reader: only what decides "reordering or not" ---- */
typedef struct
{
    const u8 *p;
    u32       bits, pos;
    int       err;
} H264Bits;

static u32 BrBit(H264Bits *b)
{
    u32 v;
    if (b->pos >= b->bits) { b->err = 1; return 0; }
    v = (u32)(b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}
static u32 BrBits(H264Bits *b, int k)
{
    u32 v = 0;
    while (k-- > 0) v = (v << 1) | BrBit(b);
    return v;
}
static u32 BrUe(H264Bits *b)
{
    int z = 0;
    while (!BrBit(b))
    {
        if (b->err || ++z > 31) { b->err = 1; return 0; }
    }
    return ((1UL << z) - 1) + BrBits(b, z);
}
static long BrSe(H264Bits *b)
{
    u32 k = BrUe(b);
    return (k & 1) ? (long)((k + 1) >> 1) : -(long)(k >> 1);
}
/* Remove emulation-prevention bytes (00 00 03 -> 00 00). */
static u32 H264Unescape(const u8 *src, u32 n, u8 *dst, u32 max)
{
    u32 i, o = 0, zeros = 0;
    for (i = 0; i < n && o < max; i++)
    {
        if (zeros >= 2 && src[i] == 3) { zeros = 0; continue; }
        zeros = (src[i] == 0) ? zeros + 1 : 0;
        dst[o++] = src[i];
    }
    return o;
}
static void BrHrd(H264Bits *b)
{
    u32 cnt = BrUe(b) + 1, i;
    BrBits(b, 8);
    for (i = 0; i < cnt && i < 32 && !b->err; i++) { BrUe(b); BrUe(b); BrBit(b); }
    BrBits(b, 20);
}

typedef struct
{
    u32 profile, pocType, frameMbsOnly;
    u32 bsr, numReorder;
    u32 chroma, bitDepth;           /* v17: chroma_format_idc, max(luma, chroma) bit depth */
    u32 codedW, codedH, cropL, cropR, cropT, cropB;
} H264SpsInfo;

/* Returns 1 if parsed to the end of what we need. */
static int H264ParseSps(const u8 *nal, u32 n, H264SpsInfo *o)
{
    u8       buf[512];
    H264Bits b;
    u32      i, mbW, mapH;
    memset(o, 0, sizeof(*o));
    o->chroma = 1; o->bitDepth = 8;
    if (n < 4) return 0;
    b.p = buf; b.pos = 0; b.err = 0;
    b.bits = H264Unescape(nal + 1, n - 1, buf, sizeof(buf)) * 8;
    o->profile = BrBits(&b, 8);
    BrBits(&b, 16);                 /* constraint flags, level */
    BrUe(&b);                       /* seq_parameter_set_id */
    if (o->profile == 100 || o->profile == 110 || o->profile == 122 ||
        o->profile == 244 || o->profile == 44  || o->profile == 83  ||
        o->profile == 86  || o->profile == 118 || o->profile == 128)
    {
        u32 chroma = BrUe(&b), dl, dc;
        if (chroma == 3) BrBit(&b);
        dl = BrUe(&b) + 8; dc = BrUe(&b) + 8; BrBit(&b);
        o->chroma = chroma; o->bitDepth = (dl > dc) ? dl : dc;
        if (BrBit(&b))              /* seq_scaling_matrix_present_flag */
        {
            for (i = 0; i < ((chroma != 3) ? 8u : 12u) && !b.err; i++)
            {
                if (BrBit(&b))
                {
                    long last = 8, next = 8;
                    u32 j, size = (i < 6) ? 16 : 64;
                    for (j = 0; j < size && !b.err; j++)
                    {
                        if (next != 0) next = (last + BrSe(&b) + 256) % 256;
                        if (next != 0) last = next;
                    }
                }
            }
        }
    }
    BrUe(&b);                       /* log2_max_frame_num_minus4 */
    o->pocType = BrUe(&b);
    if (o->pocType == 0) BrUe(&b);
    else if (o->pocType == 1)
    {
        u32 k;
        BrBit(&b); BrSe(&b); BrSe(&b);
        k = BrUe(&b);
        for (i = 0; i < k && i < 256 && !b.err; i++) BrSe(&b);
    }
    BrUe(&b); BrBit(&b);            /* max_num_ref_frames, gaps */
    mbW  = BrUe(&b) + 1;
    mapH = BrUe(&b) + 1;
    o->frameMbsOnly = BrBit(&b);
    if (!o->frameMbsOnly) BrBit(&b);
    BrBit(&b);                      /* direct_8x8_inference */
    o->codedW = mbW * 16;
    o->codedH = mapH * 16 * (2 - o->frameMbsOnly);
    if (BrBit(&b))
    {
        u32 uy = 2 * (2 - o->frameMbsOnly);
        o->cropL = BrUe(&b) * 2; o->cropR = BrUe(&b) * 2;
        o->cropT = BrUe(&b) * uy; o->cropB = BrUe(&b) * uy;
    }
    if (BrBit(&b))                  /* vui_parameters_present_flag */
    {
        u32 nal_hrd, vcl_hrd;
        if (BrBit(&b)) { if (BrBits(&b, 8) == 255) BrBits(&b, 32); }
        if (BrBit(&b)) BrBit(&b);
        if (BrBit(&b)) { BrBits(&b, 4); if (BrBit(&b)) BrBits(&b, 24); }
        if (BrBit(&b)) { BrUe(&b); BrUe(&b); }
        if (BrBit(&b)) { BrBits(&b, 32); BrBits(&b, 32); BrBit(&b); }
        nal_hrd = BrBit(&b); if (nal_hrd) BrHrd(&b);
        vcl_hrd = BrBit(&b); if (vcl_hrd) BrHrd(&b);
        if (nal_hrd || vcl_hrd) BrBit(&b);
        BrBit(&b);                  /* pic_struct_present_flag */
        if (BrBit(&b))              /* bitstream_restriction_flag */
        {
            BrBit(&b); BrUe(&b); BrUe(&b); BrUe(&b); BrUe(&b);
            o->numReorder = BrUe(&b);
            BrUe(&b);
            o->bsr = !b.err;
        }
    }
    return !b.err;
}

/* v17: zero bytes kept after every buffer handed to AVCDecLib. FFmpeg
   0.8.15 needs FF_INPUT_BUFFER_PADDING_SIZE = 8; 16 keeps sizes aligned. */
#define kAvPad 16

/* v17: is this sample a key frame: an IDR slice, or only I slices
   (an open-GOP recovery point)? */
static Boolean H264SampleIsKey(const u8 *p, u32 size, u32 lengthSize)
{
    u32 pos = 0, slices = 0, iSlices = 0;
    if (p == NULL || lengthSize < 1 || lengthSize > 4) return false;
    while (pos + lengthSize < size)
    {
        u32 n = 0, i, type;
        for (i = 0; i < lengthSize; i++) n = (n << 8) | p[pos + i];
        pos += lengthSize;
        if (n == 0 || n > size - pos) break;
        type = (u32)(p[pos] & 0x1F);
        if (type == 5) return true;
        if (type == 1 || type == 2)
        {
            u8 buf[16];
            H264Bits b;
            u32 st;
            b.p = buf; b.pos = 0; b.err = 0;
            b.bits = H264Unescape(p + pos + 1, (n - 1 < 12) ? n - 1 : 12, buf, sizeof(buf)) * 8;
            BrUe(&b);               /* first_mb_in_slice */
            st = BrUe(&b);
            slices++;
            if (!b.err && (st % 5) == 2) iSlices++;
        }
        pos += n;
    }
    return (Boolean)(slices > 0 && iSlices == slices);
}

/* (Re)open the decoder context from the saved avcC. AltiVec only if the
   CPU has it AND the stack is 16-byte aligned on this call. */
static int H264AvOpen(H264GlobalsPtr glob)
{
    AVCDecCallbacks cb;
    unsigned long   flags = 0;
    if (glob->avc != NULL) { H264_AV(glob).close(glob->avc); glob->avc = NULL; }
    glob->lastPic = NULL;
    glob->avPic.plane[0] = NULL;
    cb.alloc   = H264AvAlloc;
    cb.release = H264AvRelease;
    cb.log     = NULL;
    cb.opaque  = NULL;
    if (glob->avAltiVec) flags |= AVCDEC_FLAG_ALTIVEC;
    if (H264_AV(glob).init(&cb, flags, &glob->avLibSpMod16) != 0) return 0;
    glob->avc = H264_AV(glob).open(glob->avcCopy, glob->avcCSize);
    glob->avAfterReset = true;     /* v17: the first picture is shown at once, as after a reset */
    glob->avGate       = false;
    glob->avPicValid   = false;
    glob->avHavePrevSample = false;   /* v17c: a new context has seen no sample */
    glob->avMono       = false;
    glob->avLastPocValid = false;
    if (glob->avc != NULL && glob->av.set_delay != NULL && glob->avDelay > 0)
        H264_AVQ(glob).set_delay(glob->avc, glob->avDelay);
    return glob->avc != NULL;
}

static int FeedParameterSetsAvc(H264GlobalsPtr glob, const u8 *avcC, u32 avcCSize)
{
    const u8   *sps[kMaxParamSets];
    u32         spsSizes[kMaxParamSets];
    int         numSps = 0;
    const u8   *pps[kMaxParamSets];
    u32         ppsSizes[kMaxParamSets];
    int         numPps = 0;
    H264SpsInfo si;
    long        newDelay;

    if (!AVCC_Parse(avcC, avcCSize, &glob->lengthSize,
                     sps, spsSizes, kMaxParamSets, &numSps,
                     pps, ppsSizes, kMaxParamSets, &numPps) || numSps < 1)
        return 0;

    glob->unsupportedStream = false;
    glob->streamProfile     = (avcCSize > 1) ? (u32)avcC[1] : 0;

    if (!H264ParseSps(sps[0], spsSizes[0], &si)) return 0;
    glob->frameWidth   = (long)si.codedW;
    glob->frameHeight  = (long)si.codedH;
    glob->haveCropInfo = (si.cropL | si.cropR | si.cropT | si.cropB) != 0;
    glob->cropLeft = si.cropL; glob->cropTop = si.cropT;
    glob->cropWidth = si.codedW - si.cropL - si.cropR;
    glob->cropHeight = si.codedH - si.cropT - si.cropB;

    /* What FFmpeg 0.8.15 cannot do here (AVD_8BIT_ONLY build, 4:2:0
       writers): refuse with black, never with an error. */
    if (si.bitDepth != 8 || si.chroma != 1)
    {
        glob->unsupportedStream = true;
        LogNum(glob, "UNSUPPORTED (black): bit depth > 8 or not 4:2:0; profile_idc", (long)si.profile);
        return 1;
    }

    /* v17: N, the fixed reorder delay. 1. the 'rdly' extension our moov
       rewriter writes; 2. the SPS VUI num_reorder_frames; 3. Baseline
       (profile 66, incl. Constrained Baseline) cannot reorder: 0;
       4. anything else: 2, said once in the log. */
    {
        long n;
        if (glob->avRdly >= 0) n = (glob->avRdly > 16) ? 16 : glob->avRdly;
        else if (si.bsr) n = (si.numReorder > 16) ? 16 : (long)si.numReorder;
        else if (si.profile == 66) n = 0;
        else
        {
            n = 2;
            LogNum(glob, "AVCDecLib: no 'rdly' and no VUI num_reorder_frames; reorder delay assumed", n);
        }
        if (n > 0 && !glob->avDelayFns)
        {
            glob->unsupportedStream = true;
            LogNum(glob, "UNSUPPORTED (black): B-frames need AVCDecLib 0x00080F02 or later; this one is", (long)glob->av.version());
            return 1;
        }
        newDelay = n;
        glob->avRdlyHolds = (Boolean)(glob->avRdly >= 0 && glob->avDelayFns);
    }

    /* Same avcC as the context already has: keep the decoder state. */
    if (glob->avc != NULL && glob->avcCopy != NULL && glob->avcCSize == (long)avcCSize &&
        memcmp(glob->avcCopy, avcC, avcCSize) == 0)
    {
        if (newDelay > glob->avDelay && glob->av.set_delay != NULL)
        {
            glob->avDelay = newDelay;
            H264_AVQ(glob).set_delay(glob->avc, newDelay);
        }
        return 1;
    }
    glob->avDelay = newDelay;

    if (glob->avcCopy != NULL) { H264MemFree(glob->avcCopy); glob->avcCopy = NULL; }
    /* v17: FFmpeg reads up to FF_INPUT_BUFFER_PADDING_SIZE (8 in 0.8.15)
       bytes past any input: keep kAvPad zero bytes after the copy. */
    glob->avcCopy = (u8 *)H264MemAlloc(avcCSize + kAvPad);
    if (glob->avcCopy == NULL) return 0;
    BlockMoveData(avcC, glob->avcCopy, (Size)avcCSize);
    memset(glob->avcCopy + avcCSize, 0, kAvPad);
    glob->avcCSize = (long)avcCSize;

    glob->avAltiVec = H264HasAltiVec() && ((H264GetSP() & 15) == 0);
    if (!H264AvOpen(glob))
    {
        glob->unsupportedStream = true;
        LogAppend(glob, "UNSUPPORTED (black): AVCDecLib refused the avcC");
    }
    else if (!glob->avRdlyHolds && glob->av.get_delay != NULL && H264_AVQ(glob).get_delay(glob->avc) > glob->avDelay)
        glob->avDelay = H264_AVQ(glob).get_delay(glob->avc);
    return 1;
}

/* Once per instance, only when the log file exists. */
static void H264AvLogInfo(H264GlobalsPtr glob, unsigned long spMod16)
{
    if (glob->avInfoLogged) return;
    glob->avInfoLogged = true;
    if (!LogEnabled(glob)) return;
    LogNum(glob, "AVCDecLib version", (long)glob->av.version());
    LogNum(glob, "AVCDecLib AltiVec (1 on, 0 off)", (long)glob->avAltiVec);
    LogNum(glob, "AVCDecLib sp&15 inside BandDecompress", (long)spMod16);
    LogNum(glob, "AVCDecLib sp&15 seen by avcdec_init", (long)glob->avLibSpMod16);
}
#endif /* H264_BACKEND_AVCDEC */

/* ==================================================================
   Lazy decoder construction
   ==================================================================

   h264bsd is allocated on first real use rather than in Open, and that
   is a correctness change, not just tidiness.

   h264bsdAlloc() is literally malloc(sizeof(storage_t)), and h264bsdInit
   mallocs again for the macroblock layer. Doing that in Open means every
   one of those allocations is a way for OpenComponent() to fail -- and
   fail invisibly, because the Component Manager converts any non-zero
   result from Open into a NULL instance with no error code. An
   application that merely *asks* whether an 'avc1' decoder exists would
   pay for a full decoder allocation and, if it failed, would be told
   only "the required compressor could not be found".

   After this change Open does nothing that can realistically fail:
   NewPtrClear of a small struct, and SetComponentInstanceStorage. If
   OpenComponent still returns NULL after this, the fault is not in our
   Open at all, and the log will show whether we were even entered.

   PreDecompress is the right home for the real allocation: it is the
   first call that means "I am actually going to decode", it already
   parses the avcC and needs a live decoder to do it, and it can return a
   real OSErr that QuickTime will surface. */
#if H264_BACKEND_AVCDEC
/* AVCDecLib: load the library now; the context is opened when the avcC
   arrives (FeedParameterSetsAvc). Missing library = codecUnimpErr, one
   log line. */
static OSErr EnsureDecoder(H264GlobalsPtr glob)
{
    OSErr err;
    if (glob == NULL) return paramErr;
    err = H264LoadAvc(glob);
    if (err != noErr) glob->unsupportedStream = true;
    return err;
}
#else
static OSErr EnsureDecoder(H264GlobalsPtr glob)
{
    if (glob == NULL) return paramErr;
    if (glob->h264 != NULL && glob->h264Inited) return noErr;

    if (glob->h264 == NULL)
    {
        glob->h264 = h264bsdAlloc();
        VLogNum(glob, "EnsureDecoder h264bsdAlloc ptr", (long)glob->h264);
        if (glob->h264 == NULL) return memFullErr;
    }

    if (!glob->h264Inited)
    {
        /* v14: second argument 1 = "no output reordering".

           h264bsd can hold finished pictures back and hand them over in
           display order, which is needed for streams with B-frames.
           Baseline has no B-frames: decode order IS display order. With
           the argument 0 (v13 and earlier) h264bsd still sized its
           picture buffer for reordering -- the full DPB the level allows,
           up to 16 pictures plus one -- and for streams whose SPS says
           pic_order_cnt_type 0 it also delayed output until the buffer
           filled or a later picture proved nothing earlier was coming.
           With 1 it keeps only the reference pictures the stream really
           uses plus the one being decoded, and every picture comes out
           on the call that finishes it. Less memory, and no display
           delay on type-0 streams.

           For the dv512 test clip this changes nothing: its SPS says
           pic_order_cnt_type 2, and h264bsd already switches reordering
           off by itself for type 2 (and for streams whose VUI says
           num_reorder_frames 0); see h264bsd_storage.c. The argument
           matters for type-0 and type-1 files without that VUI field,
           which is most of what other encoders produce. */
        u32 rv = h264bsdInit(glob->h264, 1);
        VLogNum(glob, "EnsureDecoder h264bsdInit rv", (long)rv);
        if (rv != H264BSD_RDY) return internalComponentErr;
        glob->h264Inited = 1;
    }

    return noErr;
}
#endif /* !H264_BACKEND_AVCDEC */

/* Open is the one entry point that arrives with glob == NULL: the
   Component Manager has no storage for this instance yet, and allocating
   it is precisely our job here. The session-1 draft called a
   "Base_ImageCodecOpen" to do this, but no such function exists anywhere
   in the SDK -- grepping the real headers turns up nothing, so it would
   have been an undefined symbol at link time. The genuine idiom, used by
   every component of this era, is NewPtrClear + SetComponentInstanceStorage.

   Note the (Handle) cast on a Ptr: that is not a bug and not a Handle in
   the usual double-indirection sense. The Component Manager treats
   instance storage as an opaque word it hands back to us verbatim
   through GetComponentInstanceStorage, and storing a plain pointer there
   is the conventional thing to do. NewPtrClear (rather than NewPtr) so
   every field starts zeroed and the Close path below can safely test
   each pointer before disposing it, even if Open fails halfway. */
pascal ComponentResult H264CodecOpen(H264GlobalsPtr glob, ComponentInstance self)
{
    #pragma unused(glob)

    glob = (H264GlobalsPtr)NewPtrClear(sizeof(H264Globals));
    if (glob == NULL) return memFullErr;

    SetComponentInstanceStorage(self, (Handle)glob);

    glob->self   = self;
    glob->target = self;

    /* Everything that can fail now happens later, in EnsureDecoder (see
       the note there). Open's whole job is to hand the Component Manager
       a zeroed globals block; NewPtrClear guarantees every pointer,
       counter and flag in it starts as zero, so Close can dispose safely
       no matter how far a later call got. QuickTime opens the component
       many times just to ask what it is, so Open also touches no files:
       the logger does not look for its file until an instance has
       something to say. */
    VLogAppend(glob, "--- Open OK ---");
    return noErr;
}

static void LogTimingTrace(H264GlobalsPtr glob);   /* defined after BandDecompress */

/* Close must tolerate a half-built glob: if a later call failed partway
   (say h264bsdAlloc succeeded but h264bsdInit did not), the Component
   Manager still calls Close on the instance. NewPtrClear in Open is what
   makes the pointer tests below trustworthy in that case. */
pascal ComponentResult H264CodecClose(H264GlobalsPtr glob, ComponentInstance self)
{
    #pragma unused(self)

    if (glob != NULL)
    {
        VLogAppend(glob, "--- Close enter ---");
        LogPlaybackStats(glob);
        LogTimingTrace(glob);

#if H264_BACKEND_AVCDEC
        if (glob->statIdrRepeats != 0) LogNum(glob, "AVCDecLib re-sent key frames not decoded", (long)glob->statIdrRepeats);
        if (glob->statHeld != 0) LogNum(glob, "AVCDecLib samples that gave no picture", (long)glob->statHeld);
        if (glob->avDelay != 0) LogNum(glob, "AVCDecLib reorder delay N", glob->avDelay);
        if (glob->statResets != 0) LogNum(glob, "AVCDecLib resets (jump onto a key frame)", (long)glob->statResets);
        if (glob->statJumpsNoKey != 0) LogNum(glob, "AVCDecLib jumps onto a non-key sample (no reset)", (long)glob->statJumpsNoKey);
        if (glob->statStaleHidden != 0) LogNum(glob, "AVCDecLib ... pictures older than one shown, not shown", (long)glob->statStaleHidden);
        if (glob->statGated != 0) LogNum(glob, "AVCDecLib open-GOP leading pictures hidden", (long)glob->statGated);
        if (glob->statTail != 0) LogNum(glob, "AVCDecLib one-byte tail samples", (long)glob->statTail);
        if (glob->statRdlyTakes != 0) LogNum(glob, "AVCDecLib pictures taken early to hold N from rdly", (long)glob->statRdlyTakes);
        if (glob->statRdlyLost != 0) LogNum(glob, "AVCDecLib pictures not shown (two out in one call)", (long)glob->statRdlyLost);
        if (glob->statKept != 0) LogNum(glob, "AVCDecLib calls that left the destination alone", (long)glob->statKept);
        if (glob->statDstChanged != 0) LogNum(glob, "AVCDecLib ... and found it changed (flag 512)", (long)glob->statDstChanged);
        if (glob->statBlackRepaints != 0) LogNum(glob, "AVCDecLib black repaints (new buffer, no valid picture)", (long)glob->statBlackRepaints);
        if (glob->avSpBad != 0) LogNum(glob, "AVCDecLib calls with sp&15 != 0", (long)glob->avSpBad);
        if (glob->avc != NULL) { H264_AV(glob).close(glob->avc); glob->avc = NULL; }
        if (glob->avcCopy != NULL) { H264MemFree(glob->avcCopy); glob->avcCopy = NULL; }
        if (glob->avBuf != NULL) { H264MemFree(glob->avBuf); glob->avBuf = NULL; glob->avBufSize = 0; }
#ifndef H264_HOST_BUILD
        if (glob->avLoaded) CloseConnection(&glob->avConn);
#endif
        glob->avLoaded = false;
#endif
#if !H264_BACKEND_AVCDEC
        if (glob->h264 != NULL)
        {
            if (glob->h264Inited) h264bsdShutdown(glob->h264);
            h264bsdFree(glob->h264);
            glob->h264 = NULL;
        }
#endif
        if (glob->annexBScratch != NULL)
        {
            H264MemFree(glob->annexBScratch);
            glob->annexBScratch = NULL;
        }
        if (glob->wantedPixelTypes != NULL)
        {
            DisposeHandle((Handle)glob->wantedPixelTypes);
            glob->wantedPixelTypes = NULL;
        }
        if (glob->trace != NULL)
        {
            DisposePtr((Ptr)glob->trace);
            glob->trace = NULL;
        }
        DisposePtr((Ptr)glob);
    }
    return noErr;
}

/* Target is how the Component Manager tells a component that someone has
   captured it -- calls should be routed to `target` rather than `self`
   from then on. We keep it purely for correctness/bookkeeping: with no
   delegate component in this architecture there is nothing to re-point,
   but answering the selector (rather than returning
   badComponentSelector) is what a well-behaved component does. */
pascal ComponentResult H264CodecTarget(H264GlobalsPtr glob, ComponentInstance target)
{
    if (glob == NULL) return paramErr;
    glob->target = target;
    return noErr;
}

/* There is deliberately no H264CodecCanDo here.

   A hand-written one used to live at this spot, a switch listing the
   selectors we implement. It compiled to "object 'H264CodecCanDo'
   redefined", because ComponentDispatchHelper.c already generates a
   CanDo for us (near its line 400) and does it better than we can: the
   generated version answers by running the actual selector through
   COMPONENTSELECTORLOOKUP and reporting false for anything that maps to
   ComponentError or ComponentDelegate. That is derived from the dispatch
   table itself, so it is incapable of drifting out of sync with it,
   whereas a hand-maintained switch would quietly start lying the first
   time someone edited H264CodecDispatch.h and forgot to edit here too.

   StdComponentCall(CanDo) in the dispatch table refers to that generated
   function, so the selector is still answered -- just not by us. (If we
   ever do need to special-case an answer, the helper supports an
   OVERRIDE_CANDO hook rather than a redefinition.) */

pascal ComponentResult H264CodecVersion(H264GlobalsPtr glob)
{
    VLogAppend(glob, "--- Version enter ---");
    return kH264CodecVersion; /* defined in H264Codec.h as 0x00010000 -- see below */
}

pascal ComponentResult H264CodecGetCodecInfo(H264GlobalsPtr glob, CodecInfo *info)
{
    OSErr err = noErr;

    VLogAppend(glob, "--- GetCodecInfo enter ---");

    if (info == NULL) return paramErr;

    /* Corrected against the real CodecInfo (ImageCompression.h): there is
       no single catch-all 'flags' field and no 'pixelCode' field -- those
       were guessed in the first draft written without the SDK headers in
       front of me. The real struct splits capability flags three ways:
       decompressFlags/compressFlags/formatFlags.

       The earlier version of this function set only those three flag
       fields and left the rest of the struct alone. That was a real bug,
       not just untidiness: CodecInfo has seventeen fields, the caller
       does not clear it for us, and the Image Compression Manager reads
       several of the ones we were ignoring -- notably minimumWidth and
       minimumHeight, which it uses to decide whether this codec can
       handle a given image at all. Leaving them as whatever happened to
       be on the stack means the ICM was making that decision from
       garbage. Zero the whole thing first, then fill in every field we
       can answer honestly. */
    memset(info, 0, sizeof(CodecInfo));

    /* Pascal string: byte 0 is the length. */
    BlockMove("\pH.264 (Software)", info->typeName, 17);

    info->version               = kH264CodecVersion >> 16;   /* 1 */
    info->revisionLevel         = kH264CodecVersion & 0xFFFF;/* 0 */
    info->vendor                = kH264CodecManufacturer;

    info->compressFlags         = 0;  /* decompress-only, by design */
    info->decompressFlags       = codecInfoDoes32 | codecInfoDoesTemporal;
    info->formatFlags           = codecInfoDepth24 | codecInfoDepth32;

    /* Accuracy is a 1-255 self-report. H.264 decoding is exact (bit-exact
       to the spec), so claim the top of the range for decompression and
       zero -- "unknown/not applicable" -- for compression. */
    info->compressionAccuracy   = 0;
    info->decompressionAccuracy = 255;

    /* Speed fields are "milliseconds to handle 320x240 on a base Mac II"
       -- an absurd yardstick for us, and 0 is the documented "unknown",
       which is the honest answer until we have measured anything. */
    info->compressionSpeed      = 0;
    info->decompressionSpeed    = 0;
    info->compressionLevel      = 0;

    /* H.264 codes in 16x16 macroblocks, so the smallest image we can
       meaningfully be handed is one macroblock. These two fields are the
       ones most likely to have been actively harmful as garbage. */
    info->minimumWidth          = 16;
    info->minimumHeight         = 16;

    info->decompressPipelineLatency = 0;  /* we are synchronous */
    info->compressPipelineLatency   = 0;
    info->privateData               = 0;

    return err;
}

/* H264CodecInitialize used to live here. It was an ImageCodecInitialize
   (selector 0x0201), which only ever gets called in the base-codec /
   sub-codec architecture -- Apple's base decompressor calls it to ask
   how big a per-band private record to allocate. Since we settled on the
   self-contained architecture (see the long note in
   H264CodecDispatch.h), nothing calls it, and the
   ImageSubCodecDecompressCapabilities struct it took is not part of our
   world at all. Deleted rather than left dangling: an unreferenced entry
   point that looks like it participates in the design is worse than no
   entry point.

   What Initialize used to do -- declaring our decompression
   capabilities -- is now done in PreDecompress below, which is where
   the self-contained architecture puts it. */

/* PreDecompress: called once per sequence, before any frames arrive.
   Two jobs.

   First, the H.264-specific setup: pull the avcC extension out of the
   sample description, feed its SPS/PPS to h264bsd, and record the real
   decoded dimensions and crop rectangle.

   Second -- and this is what the old Preflight was missing -- fill in
   p->capabilities to negotiate with the Image Compression Manager. The
   ICM reads these fields to decide what kind of destination buffer to
   hand us at BandDecompress time, so getting them wrong here shows up
   much later as a wrongly-sized or wrongly-formatted PixMap:

     wantedPixelSize  the destination depth we want. 32 asks for a
                      32-bit RGB PixMap, which every Mac OS 9 display
                      path can provide.
     bandMin/bandInc  the band granularity we can handle. H.264 frames
                      are only decodable as a whole -- there is no way
                      to decode the top third of a frame independently
                      -- so both are the full frame height, which tells
                      the ICM "one band, all of it, or nothing".
     extendWidth/Height  extra pixels we need beyond the visible frame.
                      Zero: h264bsd keeps its own macroblock-aligned
                      buffers internally and we crop on the way out, so
                      the destination only ever needs display size. */
pascal ComponentResult H264CodecPreDecompress(H264GlobalsPtr glob, CodecDecompressParams *p)
{
    ImageDescriptionHandle desc;
    Handle avcCHandle = NULL;
    OSErr err;

    VLogAppend(glob, "--- PreDecompress enter ---");

    if (p == NULL || glob == NULL) return paramErr;

    /* v17c: QuickTime may hand over a new buffer at the address of one we
       painted before: nothing painted so far counts after PreDecompress. */
    glob->haveLastBlit = false;
#if H264_BACKEND_AVCDEC
    glob->avHaveSig = false;
#endif

    /* Build the decoder now, on the first call that actually intends to
       decode. See the note on EnsureDecoder: doing this in Open turned
       every allocation failure into an invisible NULL from
       OpenComponent(), whereas here it becomes a real OSErr that
       QuickTime reports. */
    err = EnsureDecoder(glob);
    if (err != noErr)
    {
        VLogNum(glob, "PreDecompress FAIL EnsureDecoder", (long)err);
#if H264_BACKEND_AVCDEC
        /* v17: no library, or an unusable one: black, never an error
           (an error makes QuickTime show a dialog). Logged once. */
        desc = p->imageDescription;
        goto negotiate;
#else
        return err;
#endif
    }

    desc = p->imageDescription;
    glob->frameWidth  = (**desc).width;
    glob->frameHeight = (**desc).height;
    VLogNum(glob, "PreDecompress width", (long)glob->frameWidth);
    VLogNum(glob, "PreDecompress height", (long)glob->frameHeight);

    /* Pull the avcC extension out of the sample description. Signature
       shown here is the documented classic form; some SDK revisions
       spell the selector kAVCCImageDescriptionExtension vs a raw 'avcC'
       OSType -- try the OSType form first since it's the oldest/most
       universally supported spelling. */
#if H264_BACKEND_AVCDEC
    /* v17: 'rdly', 4 bytes big-endian: the reorder delay our moov
       rewriter stores next to the avcC. */
    glob->avRdly = -1;
    {
        Handle rd = NULL;
        if (GetImageDescriptionExtension(desc, &rd, 'rdly', 1) == noErr && rd != NULL)
        {
            if (GetHandleSize(rd) >= 4)
            {
                const u8 *q = (const u8 *)*rd;
                unsigned long v = ((unsigned long)q[0] << 24) | ((unsigned long)q[1] << 16) |
                                  ((unsigned long)q[2] << 8) | (unsigned long)q[3];
                glob->avRdly = (v > 16UL) ? 16L : (long)v;
            }
            DisposeHandle(rd);
        }
    }
#endif
    err = GetImageDescriptionExtension(desc, &avcCHandle, 'avcC', 1);
    if (err != noErr || avcCHandle == NULL)
    {
#if H264_BACKEND_AVCDEC
        glob->unsupportedStream = true;
        LogAppend(glob, "UNSUPPORTED (black): no avcC in the image description");
        goto negotiate;
#else
        return badFormat; /* no avcC -> we don't know how to frame the NALs */
#endif
    }

    HLock(avcCHandle);
    {
        int ok = FeedParameterSets(glob, (const u8 *)*avcCHandle,
                                    (u32)GetHandleSize(avcCHandle));
        HUnlock(avcCHandle);
        DisposeHandle(avcCHandle);
#if H264_BACKEND_AVCDEC
        if (!ok && !glob->unsupportedStream)
        {
            glob->unsupportedStream = true;
            LogAppend(glob, "UNSUPPORTED (black): avcC or SPS not readable");
        }
#else
        if (!ok) return badFormat;
#endif
    }

    if (glob->unsupportedStream)
    {
        /* Tell QuickTime this codec cannot do the stream (codecUnimpErr,
           -8962, written as a number because its header is not included
           here), and say why in the log if there is one. Profile 66 is
           Baseline, 77 Main, 88 Extended, 100 High. */
#if H264_BACKEND_AVCDEC
        /* v17: already said in one log line; play black (see BandDecompress). */
        goto negotiate;
#else
        LogNum(glob, "UNSUPPORTED H.264 stream, profile_idc", (long)glob->streamProfile);
        LogAppend(glob, "  h264bsd refused its SPS or PPS: only Baseline (no CABAC, no B-frames) can be decoded");
        return -8962;
#endif
    }

    /* Ask h264bsd what it actually decided after parsing the SPS -- this
       can legitimately differ from the ImageDescription's width/height
       (macroblock rounding), which is exactly why we keep both. */
#if !H264_BACKEND_AVCDEC
    glob->frameWidth  = (long)h264bsdPicWidth(glob->h264)  * 16;
    glob->frameHeight = (long)h264bsdPicHeight(glob->h264) * 16;

    {
        u32 cropFlag, left, width, top, height;
        h264bsdCroppingParams(glob->h264, &cropFlag, &left, &width, &top, &height);
        glob->haveCropInfo = cropFlag;
        glob->cropLeft = left; glob->cropTop = top;
        glob->cropWidth = width; glob->cropHeight = height;
    }

    /* Make sure our Annex-B scratch buffer is big enough for the worst
       case single sample we might be handed. p->bufferSize is the
       largest sample size QuickTime tells us to expect for this movie. */
    {
        u32 needed = AVCC_WorstCaseAnnexBSize((u32)p->bufferSize);
        glob->statPreBufferSize = (unsigned long)p->bufferSize;
        if (needed > glob->annexBScratchSize)
        {
            /* v14: through H264MemAlloc like the decoder's own buffers,
               so a big scratch buffer can come from temporary memory
               when the application's heap is short. */
            if (glob->annexBScratch != NULL) H264MemFree(glob->annexBScratch);
            glob->annexBScratchSize = 0;
            glob->annexBScratch = (u8 *)H264MemAlloc(needed);
            if (glob->annexBScratch == NULL) return memFullErr;
            glob->annexBScratchSize = needed;
        }
    }

#endif /* !H264_BACKEND_AVCDEC: AVCDecLib takes length-prefixed samples as they are */

#if H264_BACKEND_AVCDEC
negotiate:
#endif
    /* Negotiate the destination buffer with the ICM. See the header
       comment on this function for why each field is what it is.
       p->capabilities is supplied by the caller and is documented in
       ImageCodec.h as valid for both predecompress and banddecompress,
       so it is safe to write through here. */
    if (p->capabilities != NULL)
    {
        short displayHeight = (**desc).height;

        p->capabilities->wantedPixelSize = 32;
        p->capabilities->extendWidth     = 0;
        p->capabilities->extendHeight    = 0;
        p->capabilities->bandMin         = displayHeight;
        p->capabilities->bandInc         = displayHeight;
        p->capabilities->flags           = 0;

#if kOfferYUVOutput
        /* ------------------------------------------------------------
           Ask for a YUV destination.
           ------------------------------------------------------------
           wantedDestinationPixelTypes is an OSType** in
           CodecDecompressParams -- a handle to a ZERO-TERMINATED list,
           in our order of preference. It has been there since
           QuickTime 2.5, so it is safe against the 6.0.3 runtime and
           against the Universal Interfaces 3.4.1 headers this project
           builds with. (Verified in ImageCodec.h rather than assumed;
           it is NOT a field of CodecCapabilities, which is where I
           first went looking.)

           The order matters and is not arbitrary:

             'y420'  planar 4:2:0, identical in layout to h264bsd's own
                     output. The blit degenerates to BlockMoveData and
                     the colour conversion vanishes.
             '2vuy'  packed 4:2:2. Still no per-pixel arithmetic, and
                     half the bytes of 32-bit RGB.
             32      plain 32-bit RGB -- today's behaviour, last,
                     so there is always something we know works.

           The ICM walks the list and picks the first it can actually
           display on this machine, which is a decision that depends on
           the video card and cannot be predicted from here. That is
           precisely why the list exists and why the blit dispatches on
           the result instead of on this request.

           ------------------------------------------------------------
           WHO OWNS THE HANDLE -- the codec does.
           ------------------------------------------------------------
           The header gives the field's TYPE and nothing about its
           ownership, and the first draft of this code guessed wrong: it
           assumed the ICM hands us a handle to resize and fill. The
           pattern in Apple's own sample decompressors runs the other way
           -- the codec allocates the list, keeps it in its globals,
           points p->wantedDestinationPixelTypes at it, and disposes of
           it in Close. Under the first draft the field would have been
           NULL on entry (so YUV was silently never offered) or
           uninitialised (so SetHandleSize ran on garbage). Caught by
           re-reading, before it ever ran.

           So: allocate once per instance, reuse on later PreDecompress
           calls, never write through whatever pointer the field held on
           entry. If the allocation fails we leave wantedPixelSize at 32
           and carry on exactly as before -- there is no failure mode
           here worse than "no faster than yesterday".

           ------------------------------------------------------------
           What "YUV output" does and does not buy
           ------------------------------------------------------------
           Be precise about the prize. A YUV destination does NOT make the
           colour conversion disappear -- the screen is still RGB, so
           something still converts. It moves the conversion out of our
           plain C table lookups and into QuickTime's own transfer code,
           which may be faster, may be vectorised on a G4, and on some
           video hardware may be done by the card's overlay. Whether that
           beats our blit on a 500 MHz G3 is an empirical question for the
           Pismo, not something to assert from here. What IS certain is
           that our side of the work becomes nearly free. */
        if (glob->wantedPixelTypes == NULL)
            glob->wantedPixelTypes =
                (OSType **)NewHandleClear((Size)(5 * sizeof(OSType)));

        if (glob->wantedPixelTypes != NULL)
        {
            OSType *list = *glob->wantedPixelTypes;

            short   k    = 0;

            list[k++] = kYUV420CodecType;        /* 'y420' */
            list[k++] = k422YpCbCr8CodecType;    /* '2vuy' */
#if kOfferRGB16
            /* Experiment (v14): 16-bit RGB, big-endian 5-5-5, which is
               the value 16 (k16BE555PixelFormat). The handle has room
               for it: five entries. */
            list[k++] = (OSType)16;
#endif
#if kOfferRGB32
            list[k++] = (OSType)k32ARGBPixelFormat;
#endif
            list[k]   = 0;                       /* terminator */

            p->wantedDestinationPixelTypes = glob->wantedPixelTypes;

            /* Documented convention: zero here means "consult the list
               instead of the depth". */
            p->capabilities->wantedPixelSize = 0;

            VLogAppend(glob, "PreDecompress: offered y420 / 2vuy / 32-bit RGB");
        }
        else
        {
            VLogAppend(glob, "PreDecompress: no memory for the pixel-type list; "
                      "staying on 32-bit RGB");
        }
#endif
    }

    return noErr;
}

/* ------------------------------------------------------------------
   Hand every ready picture to the blit.
   ------------------------------------------------------------------
   Called from two places: once per completed picture inside the NAL
   loop, and once after it for anything still queued. h264bsd returns
   each picture exactly once, so calling this more often than necessary
   is free.

   v14: when the lateness policy has decided this call may skip painting
   (skipBlitThisCall), the picture is still taken from h264bsd and
   remembered as the last picture -- it is a reference for the pictures
   that follow and it is what a later repaint must show -- but it is not
   copied to the destination. */
/* v16t: what happens to one finished picture, for either decoder. */
static void OutputOnePicture(H264GlobalsPtr glob, u8 *pic, CodecDecompressParams *params)
{
    if (glob->skipBlitThisCall)
    {
        glob->statBlitSkips++;
        glob->blitSkips++;
    }
    else
    {
        BlitPicture(glob, pic, params);
        glob->blitSkips = 0;
    }
    glob->lastPic = pic;
    glob->picsOut++;

#if kH264TimingTrace
    if (glob->trace != NULL)
    {
        /* A checksum of the whole decoded picture (coded size, all
           three planes), so a trace from the Mac can be compared
           picture by picture with the host harness's output -- the
           way to tell whether a compiler optimisation level changed
           the decoder's results. Fletcher-style, 32 bits. */
#if H264_BACKEND_AVCDEC
            u32 a = 1, b = 0, k, row, plane;
            (void)pic;
            for (plane = 0; plane < 3; plane++)
            {
                u32 w = (u32)glob->avPic.width  >> (plane ? 1 : 0);
                u32 h = (u32)glob->avPic.height >> (plane ? 1 : 0);
                for (row = 0; row < h; row++)
                {
                    const u8 *r = glob->avPic.plane[plane] + (long)row * glob->avPic.stride[plane];
                    for (k = 0; k < w; k++) { a += r[k]; b += a; }
                }
            }
#else
        u32 n = (u32)h264bsdPicWidth(glob->h264) * 16 *
                (u32)h264bsdPicHeight(glob->h264) * 16 * 3 / 2;
        u32 a = 1, b = 0, k;
        for (k = 0; k < n; k++) { a += pic[k]; b += a; }
#endif
        glob->traceSum = (unsigned long)((b << 16) ^ a);
    }
#endif
    if (glob->noDeblock) glob->statNoDeblockPics++;
    if (LogEnabled(glob))
    {
        UnsignedWide now;
        Microseconds(&now);
        StatNotePicture(glob, (unsigned long)now.lo);
    }
}

#if !H264_BACKEND_AVCDEC
static void DrainOutputPictures(H264GlobalsPtr glob, CodecDecompressParams *params)
{
    u32 picId, isIdr, numErrMbs;
    u8 *pic;

    if (glob == NULL || !H264_HAVE_DECODER(glob) || params == NULL) return;

    while ((pic = h264bsdNextOutputPicture(glob->h264, &picId, &isIdr,
                                            &numErrMbs)) != NULL)
    {
        if (kH264Verbose && glob->picsOut < 3)
        {
            long planarBytes = (long)h264bsdPicWidth(glob->h264) * 16 *
                               (long)h264bsdPicHeight(glob->h264) * 16 * 3 / 2;

            LogNum(glob, "  output picture #", (long)glob->picsOut);
            if (glob->picsOut == 0)
                DumpBytesToShare(glob, "share:h264pic0.yuv", pic, planarBytes);
            /* Three frames is enough to compare against the reference;
               130K of synchronous shared-volume I/O per frame for a
               whole movie would drop playback to a crawl. */
            DumpBytesToShare(glob, "share:h264piclast.yuv", pic, planarBytes);
        }

        OutputOnePicture(glob, pic, params);
    }
}
#endif

/* ==================================================================
   Lateness policy (v14)
   ==================================================================
   What the Pismo logs of 24 September say, re-read in Session 13:
   QuickTime 6 does not drop frames for this decoder. On a Mac that is
   too slow it hands over every sample, in order, each one later than
   the last, and the picture slides further and further behind the
   sound (22 frames short over 41 s in Thousands; one picture every
   46 ms, back to back, in Millions). Nothing skips unless the decoder
   does it. So the decoder now watches the clock itself.

   HOW LATE ARE WE?
   QuickTime passes an ICMFrameTimeRecord with each sample: the time the
   frame is due (value, in units of scale), the movie's time base, and
   the rate the time base is running at. The time base's current time
   minus the frame's due time is how late this call is. It is only
   meaningful while the movie is PLAYING FORWARD -- rate greater than
   zero. Paused, stepping, scrubbing, drawing the poster frame or
   playing backwards, the rate is zero or negative and none of this
   code does anything at all. Likewise if QuickTime passes no frame
   time, or flags the sample as a catch-up frame of its own.

   Only the low 32 bits of the two times are used, as an unsigned
   difference read back as signed: correct across a wrap, and no 64-bit
   arithmetic (CodeWarrior 8 PPC miscompiles some of it; see
   GetMovie_prefix.h).

   WHAT WE DO ABOUT IT, mildest first. Each step is taken only while
   the frame in hand is at least kLateMs late:

   1. Do not decode disposable frames. A frame whose slices all carry
      nal_ref_idc 0 is used by no other frame; leaving it out costs one
      picture and no accuracy. (Many Baseline encoders make none. Then
      this step never fires.)

   2. Decode but do not paint. The decoder's output is still needed --
      the next frame is predicted from it -- but copying it to the
      destination is not, as long as the destination still holds the
      previous picture (same buffer, same rectangle; checked). At most
      kMaxBlitSkips pictures in a row go unpainted, so the image never
      freezes while the decoder is still running.

   3. Turn the deblocking filter off, once we have been late for
      kLateStreakNoDeblock calls in a row or are kLateNoDeblockMs
      behind, AND the lateness has grown on each of the last
      kLosingCallsNoDeblock calls (see "still losing ground" below).
      The filter is roughly a third of the decoding time (measured on
      the host: 27% of h264bsd's time as 32-bit PowerPC code, 40% as
      x86 code; the G3 itself is still to be measured).
      Without it pictures are blockier, and because they are also the
      references for the pictures that follow, the difference persists
      until the next key frame. It is switched back on after
      kOnTimeCallsToRecover on-time calls in a row.

   4. Give up until the next key frame, when we are kLateHopelessMs
      behind AND have lost ground on each of the last
      kLateHopelessCalls calls -- that is, the cheaper measures are
      demonstrably not enough. Every sample is then dropped unopened
      (that takes microseconds, so the clock catches up at once) and
      the last picture stays on screen until a key frame arrives, which
      is decoded normally and starts afresh. A key frame itself is
      never dropped.

      The "still losing ground" condition matters. After a jump in a
      playing movie QuickTime may feed the frames from the previous key
      frame up to the new position as fast as we can take them; each is
      very late by its own timestamp, but the lateness SHRINKS from one
      to the next. That is catching up, not falling behind, and it must
      not trigger a wait or switch the filter off.

   WHAT IS NOT YET KNOWN. All of the above rests on reading the Pismo
   logs and on the documented meaning of ICMFrameTimeRecord; it has been
   run only against a model of QuickTime in the host harness
   (tools/host, "sim" mode). Whether QuickTime really passes a frame
   time with a positive rate on every playing call, what it does when
   we return early, and whether it ever sets the catch-up flags, is
   what the slow-CPU experiment in the VM is for: set
   kH264SimulateSlowMs and kH264TimingTrace and read the log. The
   statistics at Close report how often each step fired. */

#define kLateMs                 66L     /* two frames at 30 fps */
#define kLateNoDeblockMs        250L
#define kLateStreakNoDeblock    12
#define kLosingCallsNoDeblock   3
#define kLateHopelessMs         1000L
#define kLateHopelessCalls      8
#define kMaxBlitSkips           2       /* paint at least every third picture */
#define kOnTimeCallsToRecover   30

enum
{
    kActDecodeAndBlit = 0,
    kActDecodeNoBlit  = 1,
    kActDrop          = 2
};

/* How late is this call, in milliseconds? 0 if it is on time or early,
   or if there is no usable clock. *timed says whether a clock was
   available. dueLo/nowLo are for the timing trace.

   Two clocks, best first:

   1. params->frameTime with a time base and a positive rate: the frame's
      due time against GetTimeBaseTime. The documented way. MEASURED
      2026-11-11 in the VM: QuickTime Player 6.0.3 passes NO frameTime to
      this codec at all (it is reserved for codecs that do scheduled
      asynchronous decompression, codecCanAsyncWhen). Kept for whatever
      host does pass one.

   2. A self-made clock, from what QuickTime does pass: syncFrameTime,
      each frame's start time in its own time scale (no time base), and
      our own Microseconds(). While playback keeps up, (wall time - frame
      start time) is constant: measured over 900 calls in the VM it stayed
      within about +-30 ms. Every millisecond it grows by is a millisecond
      we have fallen behind. So per call:

          late += (wall time since the last call) - (media time since it)
          late  = max(late, 0)

      The max() makes the earliest call seen the zero point. Reset to 0:
        - when QuickTime WAITED for us: more than kSelfClockIdleMs between
          our previous return and this call. A codec that is behind is
          called again at once; one that is ahead is made to wait until
          the next frame is due. This also covers pauses, the poster
          frame, and stepping.
        - when media time goes backwards (playing backwards, a seek back)
          or jumps forward by more than the wall time plus a second (a seek
          forward).
      A false reset only makes us under-estimate lateness, which errs on
      the side of doing less. */

#define kSelfClockIdleMs   15L

static long H264LatenessMs(H264GlobalsPtr glob, CodecDecompressParams *params,
                           Boolean *timed, unsigned long *dueLo, unsigned long *nowLo)
{
    const ICMFrameTimeRecord *ft = params->frameTime;

    *timed = false;
    *dueLo = 0;
    *nowLo = 0;

    if (ft != NULL && ft->base != NULL && ft->scale > 0 && ft->rate > 0)
    {
        TimeRecord now;
        long       diff;

        *dueLo = (unsigned long)ft->value.lo;
        GetTimeBaseTime((TimeBase)ft->base, (TimeScale)ft->scale, &now);
        *nowLo = (unsigned long)now.value.lo;
        *timed = true;

        diff = (long)((unsigned long)now.value.lo - (unsigned long)ft->value.lo);
        if (diff <= 0) return 0;

        /* diff * 1000 / scale without overflowing 32 bits */
        if (diff > 2000000L) return (diff / ft->scale) * 1000L;
        return (diff * 1000L) / ft->scale;
    }

    if (params->syncFrameTime != NULL && params->syncFrameTime->scale > 0)
    {
        const ICMFrameTimeInfo *st = params->syncFrameTime;
        unsigned long lo    = (unsigned long)st->startTime.lo;
        unsigned long scale = (unsigned long)st->scale;
        long          mediaMs;
        UnsignedWide  w;

        /* lo * 1000 / scale without overflowing 32 bits */
        mediaMs = (long)((lo / scale) * 1000UL + ((lo % scale) * 1000UL) / scale);
        Microseconds(&w);

        *dueLo = (unsigned long)mediaMs;
        *nowLo = (unsigned long)w.lo;
        *timed = true;
        glob->statSelfClockCalls++;

        if (!glob->scHavePrev)
        {
            glob->scLateMs = 0;
        }
        else
        {
            long wallMs  = (long)(((unsigned long)w.lo - glob->scPrevWallLo) / 1000UL);
            long mediaDt = mediaMs - glob->scPrevMediaMs;
            long idleMs  = glob->scHaveReturn ?
                (long)(((unsigned long)w.lo - glob->scReturnLo) / 1000UL) : 0;

            if (idleMs > kSelfClockIdleMs || mediaDt < 0 || mediaDt > wallMs + 1000L)
            {
                glob->scLateMs = 0;
            }
            else
            {
                glob->scLateMs += wallMs - mediaDt;
                if (glob->scLateMs < 0) glob->scLateMs = 0;
            }
        }

        glob->scHavePrev    = true;
        glob->scPrevWallLo  = (unsigned long)w.lo;
        glob->scPrevMediaMs = mediaMs;
        return glob->scLateMs;
    }

    return 0;
}

/* Look at a sample's NAL headers without decoding anything. The sample
   is as QuickTime delivers it: each NAL preceded by a big-endian length
   of lengthSize bytes. A NAL header is one byte: 1 bit zero, 2 bits
   nal_ref_idc, 5 bits type; types 1-5 are picture slices, 5 an IDR. */
static void H264ScanSample(const u8 *p, u32 size, u32 lengthSize,
                           Boolean *hasIDR, Boolean *disposable)
{
    u32 pos = 0, slices = 0, refSlices = 0;

    *hasIDR     = false;
    *disposable = false;
    if (p == NULL || lengthSize < 1 || lengthSize > 4) return;

    while (pos + lengthSize < size)
    {
        u32 n = 0, i, type;

        for (i = 0; i < lengthSize; i++) n = (n << 8) | p[pos + i];
        pos += lengthSize;
        if (n == 0 || n > size - pos) break;

        type = (u32)(p[pos] & 0x1F);
        if (type >= 1 && type <= 5)
        {
            slices++;
            if (p[pos] & 0x60) refSlices++;
            if (type == 5) *hasIDR = true;
        }
        pos += n;
    }

    *disposable = (Boolean)(slices > 0 && refSlices == 0);
}

/* The decision. Pure bookkeeping on the instance's state: no clock, no
   QuickTime, so the host harness can drive it with any sequence of
   lateness values. */
static int H264LatePolicy(H264GlobalsPtr glob, long lateMs,
                          Boolean hasIDR, Boolean disposable)
{
    Boolean late = (Boolean)(lateMs >= kLateMs);
#if H264_BACKEND_AVCDEC
    /* v17: with a reorder delay every sample must be decoded (a sample
       not decoded would shift every later picture): no drops. */
    Boolean mayDrop = (Boolean)(glob->avDelay == 0);
#else
    Boolean mayDrop = true;
#endif

    if (lateMs > glob->statMaxLateMs) glob->statMaxLateMs = lateMs;

    if (glob->waitIDR)
    {
        if (!hasIDR)
        {
            glob->statDroppedCalls++;
            return kActDrop;
        }
        /* The key frame we were waiting for. Start afresh. */
        glob->waitIDR      = false;
        glob->lateStreak   = 0;
        glob->losingStreak = 0;
        glob->prevLateMs   = 0;
    }

    if (!late)
    {
        glob->lateStreak   = 0;
        glob->losingStreak = 0;
        glob->prevLateMs   = lateMs;
        glob->onTimeStreak++;
        if (glob->noDeblock && glob->onTimeStreak >= kOnTimeCallsToRecover)
        {
            glob->noDeblock = false;
#if H264_BACKEND_AVCDEC
            if (glob->av.set_skip != NULL && glob->avc != NULL) H264_AVQ(glob).set_skip(glob->avc, 0, 0);
#else
            h264bsdSetSkipDeblocking(glob->h264, 0);
#endif
        }
        return kActDecodeAndBlit;
    }

    glob->statLateCalls++;
    glob->onTimeStreak = 0;
    glob->lateStreak++;
    if (lateMs >= glob->prevLateMs) glob->losingStreak++;
    else                            glob->losingStreak = 0;
    glob->prevLateMs = lateMs;

    /* 4. hopeless: wait for a key frame (never drop the key frame itself) */
    if (mayDrop && !hasIDR && lateMs >= kLateHopelessMs &&
        glob->losingStreak >= kLateHopelessCalls)
    {
        glob->waitIDR = true;
        glob->statIDRWaits++;
        glob->statDroppedCalls++;
        return kActDrop;
    }

    /* 1. a frame nothing depends on */
    if (disposable && mayDrop)
    {
        glob->statNonRefSkips++;
        return kActDrop;
    }

    /* 3. deblocking filter off -- only while we are still losing ground.
       Catching up after a jump (lateness shrinking every call) is not a
       reason to degrade the picture until the next key frame. */
    /* v17: AVCDecLib 0x00080F02+: skip_loop_filter on non-reference
       pictures only, so no error carries over to later pictures. */
#if H264_BACKEND_AVCDEC
    if (glob->av.set_skip != NULL && glob->avc != NULL &&
#else
    if (
#endif
        !glob->noDeblock && glob->losingStreak >= kLosingCallsNoDeblock &&
        (lateMs >= kLateNoDeblockMs || glob->lateStreak >= kLateStreakNoDeblock))
    {
        glob->noDeblock = true;
#if H264_BACKEND_AVCDEC
        H264_AVQ(glob).set_skip(glob->avc, 1, 0);
#else
        h264bsdSetSkipDeblocking(glob->h264, 1);
#endif
        glob->statDeblockOffs++;
    }

    /* 2. decode, but leave the destination alone */
    if (glob->blitSkips < kMaxBlitSkips) return kActDecodeNoBlit;

    return kActDecodeAndBlit;
}

/* Remember one call for the timing trace. */
static void H264TraceCall(H264GlobalsPtr glob, CodecDecompressParams *params,
                          unsigned long dueLo, unsigned long nowLo,
                          long lateMs, int action)
{
    long *t;

    if (!kH264TimingTrace || !LogEnabled(glob)) return;
    if (glob->trace == NULL)
    {
        if (glob->traceCount != 0) return;      /* allocation failed before */
        glob->trace = (long *)NewPtr((Size)(kTraceCalls * kTraceFields * sizeof(long)));
        if (glob->trace == NULL) { glob->traceCount = -1; return; }
    }
    if (glob->traceCount < 0 || glob->traceCount >= kTraceCalls) return;

    t = glob->trace + glob->traceCount * kTraceFields;
    t[0] = (long)params->frameNumber;
    t[1] = (long)dueLo;
    t[2] = (long)nowLo;
    t[3] = (params->frameTime != NULL) ? (long)params->frameTime->rate : -1;
    t[4] = (long)params->conditionFlags;
    t[5] = (long)params->callerFlags;
    t[6] = lateMs;
    t[7] = (long)action;
    /* What QuickTime actually supplied: bit 0 frameTime, bit 1 its time
       base, bit 2 syncFrameTime. Measured 2026-11-11 in the VM with
       QuickTime Player 6.0.3: no usable frame time on any call, so these
       say which part is missing. */
    t[8] = (params->frameTime != NULL ? 1 : 0) |
           ((params->frameTime != NULL && params->frameTime->base != NULL) ? 2 : 0) |
           (params->syncFrameTime != NULL ? 4 : 0);
    t[9]  = (params->frameTime != NULL) ? (long)params->frameTime->scale : 0;
    t[10] = (params->frameTime != NULL) ? (long)params->frameTime->duration : 0;
    {
        /* Wall-clock arrival time of the call, for lateness WITHOUT a
           movie clock: when the decoder is the bottleneck, calls arrive
           back to back. */
        UnsignedWide w;
        Microseconds(&w);
        t[11] = (long)w.lo;
    }
    t[12] = 0;      /* filled in at the end of the call: see BandDecompressWork */
    /* v17: the raw discontinuity signals (syncFrameTime; frameNumber is
       t[0]), the sample size, and what the AVCDecLib path did (avFlags,
       filled in at the end of the call: 1 discontinuity, 2 decoder reset,
       128 destination left as it was, 256 black (new buffer, no valid
       picture), 512 destination found changed since our last paint,
       4 re-sent key frame not decoded, 8 one-byte tail sample, 16 picture
       hidden (open GOP), 32 key frame, 64 first picture shown at once). */
    t[13] = (params->syncFrameTime != NULL) ? (long)params->syncFrameTime->startTime.hi : -1;
    t[14] = (params->syncFrameTime != NULL) ? (long)params->syncFrameTime->startTime.lo : -1;
    t[15] = (params->syncFrameTime != NULL) ? (long)params->syncFrameTime->scale : -1;
    t[16] = (params->syncFrameTime != NULL) ? (long)params->syncFrameTime->duration : -1;
    t[17] = (long)params->bufferSize;
    t[18] = 0;
    glob->traceCount++;
    glob->tracedThisCall = true;
}

#if H264_BACKEND_AVCDEC
/* v17: is this sample NOT the successor of the one before it (seek, loop,
   scrub)? QuickTime 6 passes syncFrameTime on every BandDecompress
   (measured 2026-11-11: start time, scale and duration of the frame; no
   frameTime). In linear play each start time is the previous start plus
   the previous duration, exactly, in the media's time scale. Without
   syncFrameTime, frameNumber (when non-zero) must count up by one.
   Called for every sample, so the expected time always follows the last
   sample QuickTime sent. */
static Boolean H264AvJump(H264GlobalsPtr glob, CodecDecompressParams *params)
{
    const ICMFrameTimeInfo *st = params->syncFrameTime;
    Boolean jump = false;
    if (st != NULL && st->scale > 0)
    {
        /* v17c: ImageCodec.h: struct ICMFrameTimeInfo { wide startTime;
           long scale; long duration; } with wide = { SInt32 hi; UInt32 lo; }.
           Measured in QuickTime 6 (guest run b21): the times are in the
           MOVIE time scale (1000, durations 33/34), and a negative start
           (our rewriter's edit shift) arrives as hi = 0, lo = -33: hi is
           not sign-extended. So hi is ignored and only the low word is
           compared, modulo 2^32: successor <=> lo == prev lo + prev
           duration (unsigned 32-bit arithmetic) and the same scale. A real
           wrap of lo (49.7 days at scale 1000, 39.8 hours at 30000) is
           handled by the same modular arithmetic; only a jump of exactly a
           multiple of 2^32 units would be missed. */
        unsigned long lo = (unsigned long)st->startTime.lo;
        if (glob->avHaveNext && (lo != glob->avNextLo || (long)st->scale != glob->avNextScale))
            jump = true;
        glob->avNextLo    = (lo + (unsigned long)st->duration) & 0xFFFFFFFFUL;
        glob->avNextScale = (long)st->scale;
        glob->avHaveNext  = (Boolean)(st->duration > 0);
    }
    else
    {
        glob->avHaveNext = false;
        if (params->frameNumber != 0 && glob->avPrevFrameNumber != 0)
            jump = (Boolean)(params->frameNumber != glob->avPrevFrameNumber + 1);
    }
    glob->avPrevFrameNumber = (long)params->frameNumber;
    return jump;
}

/* v17: one picture out of AVCDecLib. While the open-GOP gate is up
   (after a reset), pictures that come before the key picture in display
   order are the broken leading pictures: they are not shown, the key
   picture stays up. The gate falls with the first picture at or after
   the key. */
static void H264AvShow(H264GlobalsPtr glob, const AVCDecPicture *pic, CodecDecompressParams *params)
{
    if (glob->avGate)
    {
        if (pic->poc < glob->avKeyPoc) { glob->statGated++; return; }
        glob->avGate = false;
    }
    /* v17c: after a jump onto a non-key sample, the queue may still hold
       pictures from before it: never go back in display order. A key
       picture starts a new POC period and ends the guard. */
    if (glob->avMono && !pic->key && glob->avLastPocValid && pic->poc <= glob->avLastPoc)
    {
        glob->statStaleHidden++;
        return;
    }
    if (pic->key) glob->avMono = false;
    glob->avLastPoc = pic->poc;
    glob->avLastPocValid = glob->avDelayFns;
    glob->avPic = *pic;
    OutputOnePicture(glob, (u8 *)pic->plane[0], params);
    glob->avPicValid = true;
}

/* v17: a call that brings no new picture to show (held, decode error,
   re-sent key frame, empty tail, hidden open-GOP picture, lateness drop).
   1. Same buffer and rectangle as our last paint: QuickTime keeps the
      destination's contents between calls (assumption, see the trace
      flag 512), so it already shows the right picture: do not draw.
   2. Another buffer, and the last picture is still valid (no library
      call since it came out): paint it.
   3. Another buffer and no valid picture: black. Never read a picture
      after a later library call. */
static void H264AvKeep(H264GlobalsPtr glob, CodecDecompressParams *params, long *flags)
{
    if (SameDestinationAsLastBlit(glob, params))
    {
        glob->statKept++;
        *flags |= 128;
        if (glob->avHaveSig && H264DstSig(params) != glob->avDstSig)
        {
            glob->statDstChanged++;
            *flags |= 512;
        }
        return;
    }
    if (glob->lastPic != NULL && glob->avPicValid)
    {
        BlitPicture(glob, glob->lastPic, params);
        glob->statRepeats++;
        return;
    }
    PaintBlack(params);
    NoteBlit(glob, params);
    glob->statBlackRepaints++;
    *flags |= 256;
}
#endif

/* The real work. H264CodecBandDecompress, below it, is a thin timing
   wrapper so that every return path is measured without threading a
   stopwatch through each of them. */
static ComponentResult BandDecompressWork(H264GlobalsPtr glob,
                                          CodecDecompressParams *params)
{
#if !H264_BACKEND_AVCDEC
    u32 annexBSize;
    u32 readBytes = 0;
    u32 decodeStatus;
#endif
    long dstRowBytes;
    unsigned long picsBefore;
    int action = kActDecodeAndBlit;
#if H264_BACKEND_AVCDEC
    Boolean avJump = false;
    long    avFlags = 0;
    unsigned long avGated0;
#endif

    if (glob == NULL || params == NULL) return paramErr;
#if H264_BACKEND_AVCDEC
    {
        unsigned long sp = H264GetSP() & 15;
        if (glob->avc != NULL)
        {
            H264AvLogInfo(glob, sp);
            if (sp != 0)
            {
                glob->avSpBad++;
                if (glob->avAltiVec)
                {
                    /* Never run AltiVec code on a misaligned stack: back to C. */
                    LogAppend(glob, "AVCDecLib: sp&15 != 0 inside BandDecompress, AltiVec OFF, decoder reopened");
                    glob->avAltiVec = false;
                    if (!H264AvOpen(glob)) glob->unsupportedStream = true;
                }
            }
        }
    }
    if (glob->unsupportedStream && glob->lastPic == NULL)
    {
        PaintBlack(params);
        return noErr;
    }
    avJump = H264AvJump(glob, params);
    if (avJump) avFlags |= 1;
    avGated0 = glob->statGated;
#endif
    if (!H264_HAVE_DECODER(glob)) return internalComponentErr;

    if (glob->unsupportedStream && glob->lastPic == NULL)
    {
        /* PreDecompress refused this stream. If QuickTime calls anyway,
           show plain black rather than whatever was in its buffer. */
        PaintBlack(params);
        return noErr;
    }

    /* The step-by-step lines are written only for the first few calls:
       the logger flushes the volume on every line. */
    if (glob->bandCalls < 4) VLogNum(glob, "--- BandDecompress enter #", glob->bandCalls);
    glob->bandCalls++;

    picsBefore = glob->picsOut;
    glob->skipBlitThisCall = false;
    glob->tracedThisCall   = false;

#if kH264LatenessPolicy || kH264TimingTrace
    {
        Boolean       timed, hasIDR, disposable;
        unsigned long dueLo, nowLo;
        long          lateMs = H264LatenessMs(glob, params, &timed, &dueLo, &nowLo);

        if (timed) glob->statTimedFrames++;

#if kH264LatenessPolicy
        if (params->conditionFlags & codecConditionCatchUpDiff)
        {
            /* QuickTime's own catch-up. MEASURED 2026-11-11 in the VM with
               a decoder slowed to ~49 ms per call: QuickTime 6 does not
               let the picture drift behind the sound. It jumps ahead,
               goes back to the key frame before the target, and decodes
               forward to it; every frame on the way carries this flag
               (688 of 698 calls). Such a frame is decompressed to keep
               the decoder in step, not to be seen -- so decode it, and
               do not paint it, which is the expensive half of a call in
               Millions. Same limits as lateness step 2: at most
               kMaxBlitSkips in a row, and only into the buffer that
               already holds the previous picture (checked below), so the
               picture can never freeze while frames are being decoded.
               The decoder's output is unaffected. */
            glob->statCatchUpCalls++;
            if (glob->blitSkips < kMaxBlitSkips) action = kActDecodeNoBlit;
        }
        else
        {
            H264ScanSample((const u8 *)params->data, (u32)params->bufferSize,
                           glob->lengthSize, &hasIDR, &disposable);
            action = H264LatePolicy(glob, lateMs, hasIDR, disposable);
        }
#endif
        H264TraceCall(glob, params, dueLo, nowLo, lateMs, action);
    }
#endif

    if (action == kActDrop)
    {
        /* Nothing is decoded. The destination must still show a picture:
           if this is not the buffer we painted last, paint the last
           picture into it. */
#if H264_BACKEND_AVCDEC
        H264AvKeep(glob, params, &avFlags);
#else
        if (glob->lastPic != NULL && !SameDestinationAsLastBlit(glob, params))
            BlitPicture(glob, glob->lastPic, params);
#endif
        return noErr;
    }

    if (action == kActDecodeNoBlit &&
        glob->lastPic != NULL && SameDestinationAsLastBlit(glob, params))
    {
        glob->skipBlitThisCall = true;
    }

    /* v15: grow the Annex-B scratch buffer to fit THIS sample.
       Up to v14 it was sized once, in PreDecompress, from p->bufferSize,
       on the belief that this is "the largest sample size QuickTime tells
       us to expect". It is not: QuickTime 6 passes the size of the
       sample that opens the sequence (the first key frame). The host
       harness always passed the largest sample, so it never saw this.
       archive.org's Popeye_forPresident_512kb.mp4 opens on a 616-byte
       black key frame, so the scratch was 4*616+64 = 2528 bytes, and
       every later sample bigger than that (2140 of 11098; 88 of the
       first 836) failed to convert, was counted as a decode error and
       repainted the last picture -- the 89 errors in the VM log. The
       dv512 test clip opens on its biggest key frame, so it never hit it.
       Allocating here is as safe as h264bsd's own reallocation when a
       new SPS activates, which also happens inside BandDecompress. The
       new block is taken BEFORE the old one is freed, so if memory is
       short we keep the old buffer and lose only this sample. */
#if H264_BACKEND_AVCDEC
    if ((u32)params->bufferSize > glob->statMaxSample)
        glob->statMaxSample = (unsigned long)params->bufferSize;
    {
        /* v17: fixed reorder delay. Every sample is decoded; whatever
           comes out is painted; a call that gives nothing repaints the
           last picture (or black) below. */
        const u8     *d = (const u8 *)params->data;
        long          n = (long)params->bufferSize, k, r, poc;
        unsigned long a = 1, b = 0;
        Boolean       isKey, got;
        AVCDecPicture pic;

        if (n == 1)
        {
            /* A one-byte sample from our moov rewriter: no new data,
               let the next held picture out. */
            glob->statTail++;
            avFlags |= 8;
            memset(&pic, 0, sizeof(pic));
            if (glob->av.take_lowest != NULL && H264_AV(glob).take_lowest(glob->avc, &pic, &poc) == 1)
                H264AvShow(glob, &pic, params);
            goto avDone;
        }
        isKey = H264SampleIsKey(d, (u32)n, glob->lengthSize);
        if (isKey) avFlags |= 32;
        /* QuickTime sends a key frame twice in a row (poster frame, then
           playback; also after a seek). Decoding the copy would hold
           every later picture back one more: not decoded, the call shows
           what the last one showed. */
        for (k = 0; k < n; k++) { a += d[k]; b += a; }
        a = ((b & 0xFFFF) << 16) ^ a;
        if (isKey && glob->avHavePrevSample && n == glob->avPrevSize && a == glob->avPrevSum)
        {
            glob->statIdrRepeats++;
            avFlags |= 4;
            goto avDone;
        }
        if (avJump)
        {
            if (isKey)
            {
                /* Seek, loop or scrub onto a key frame: start afresh. */
                H264_AV(glob).reset(glob->avc);
                if (glob->av.set_delay != NULL && glob->avDelay > 0)
                    H264_AVQ(glob).set_delay(glob->avc, glob->avDelay);
                glob->avAfterReset = true;
                glob->avGate = false;
                glob->avMono = false;
                glob->avLastPocValid = false;
                glob->avHavePrevSample = false;   /* v17c */
                glob->statResets++;
                avFlags |= 2;
            }
            else
            {
                /* v17c: QuickTime skipped samples (late, rate != 1, edit
                   boundary). No reset: the references we have are the
                   best there are. Until the next key picture, never show
                   a picture older (lower POC) than one already shown. */
                glob->statJumpsNoKey++;
                glob->avMono = glob->avDelayFns;
            }
        }
        /* QuickTime's sample has no padding after it: decode a padded copy.
           The copy buffer only grows; the new block is taken before the
           old one is freed (H264MemAlloc: application heap, then
           temporary memory). */
        if ((unsigned long)n + kAvPad > glob->avBufSize)
        {
            unsigned long want = ((unsigned long)n + kAvPad + 4095UL) & ~4095UL;
            u8 *nb = (u8 *)H264MemAlloc(want);
            if (nb != NULL)
            {
                if (glob->avBuf != NULL) H264MemFree(glob->avBuf);
                glob->avBuf = nb;
                glob->avBufSize = want;
            }
        }
        if (glob->avBuf == NULL || (unsigned long)n + kAvPad > glob->avBufSize)
        {
            glob->statDecodeErrors++;   /* no memory for the copy: this sample is lost */
            goto avDone;
        }
        BlockMoveData(d, glob->avBuf, (Size)n);
        memset(glob->avBuf + n, 0, kAvPad);
        memset(&pic, 0, sizeof(pic));
        r = H264_AV(glob).decode_sample(glob->avc, glob->avBuf, n, &pic);
        if (r < 0) { glob->statDecodeErrors++; glob->avHavePrevSample = false; }
        else
        {
            /* v17c: only a sample the decoder took counts as "already decoded" */
            glob->avHavePrevSample = true;
            glob->avPrevSize = n;
            glob->avPrevSum  = a;
        }
        got = (Boolean)(r == 1);
        /* v17b: N from 'rdly' is held by the component. FFmpeg raises its
           own delay back to the SPS's num_reorder_frames on every decode,
           which can be more than the file really needs (x264 b-pyramid
           declares 2, uses 1). Whenever FFmpeg holds more than N
           pictures, take the lowest one out now. In steady state that is
           exactly one per sample; if more come out at once only the last
           is shown (logged). */
        if (glob->avRdlyHolds && !got)
        {
            AVCDecPicture tp;
            long          tpoc, extra;
            /* Only when decode gave nothing (a picture it gave must not be
               followed by any library call before it is shown). held() is
               asked once, BEFORE taking, for the same reason. */
            extra = H264_AVQ(glob).held(glob->avc) - glob->avDelay;
            memset(&tp, 0, sizeof(tp));
            while (extra-- > 0 && H264_AV(glob).take_lowest(glob->avc, &tp, &tpoc) == 1)
            {
                if (got)
                {
                    glob->statRdlyLost++;   /* a picture taken before is dropped unread */
                    if (glob->statRdlyLost == 1) LogAppend(glob, "AVCDecLib: two pictures out in one call, one not shown");
                }
                pic = tp;
                got = true;
                glob->statRdlyTakes++;
            }
        }
        if (!got)
        {
            if (r == 0) glob->statHeld++;
            /* Right after a reset (or the first sample of the instance),
               show the key picture now rather than N samples later. It
               stays queued in the decoder. */
            if (glob->avAfterReset && glob->av.peek_lowest != NULL &&
                H264_AV(glob).peek_lowest(glob->avc, &pic, &poc) == 1)
            {
                glob->avAfterReset = false;
                avFlags |= 64;
                H264AvShow(glob, &pic, params);
                glob->avGate = glob->avDelayFns;
                glob->avKeyPoc = pic.poc;
            }
        }
        else
        {
            H264AvShow(glob, &pic, params);
            if (glob->avAfterReset)
            {
                glob->avAfterReset = false;
                glob->avGate = glob->avDelayFns;
                glob->avKeyPoc = pic.poc;
            }
        }
        if (!glob->avRdlyHolds && glob->av.get_delay != NULL)
        {
            long dl = H264_AVQ(glob).get_delay(glob->avc);
            if (dl > glob->avDelay)
            {
                glob->avDelay = dl;
                LogNum(glob, "AVCDecLib: reorder delay grew to", dl);
            }
        }
    }
avDone:
    if (glob->statGated != avGated0) avFlags |= 16;
#if kH264TimingTrace
    if (glob->tracedThisCall && glob->trace != NULL && glob->traceCount > 0)
        glob->trace[(glob->traceCount - 1) * kTraceFields + 18] = avFlags;
#endif
#else
    if ((u32)params->bufferSize > glob->statMaxSample)
        glob->statMaxSample = (unsigned long)params->bufferSize;
    {
        u32 needed = (glob->lengthSize == 4)
                   ? (u32)params->bufferSize + 64     /* 4-byte prefix -> 4-byte start code */
                   : AVCC_WorstCaseAnnexBSize((u32)params->bufferSize);
        if (needed > glob->annexBScratchSize)
        {
            u32 grown = needed + needed / 4 + 1024;   /* headroom: fewer regrowths */
            u8 *nb = (u8 *)H264MemAlloc(grown);
            if (nb != NULL)
            {
                if (glob->annexBScratch != NULL) H264MemFree(glob->annexBScratch);
                glob->annexBScratch     = nb;
                glob->annexBScratchSize = grown;
                glob->statScratchGrows++;
            }
        }
    }

    annexBSize = AVCC_ConvertSampleToAnnexB((const u8 *)params->data,
                                             (u32)params->bufferSize,
                                             glob->lengthSize,
                                             glob->annexBScratch,
                                             glob->annexBScratchSize);

    /* v14: a corrupt or truncated sample is no longer a failed call.
       Up to v13 this returned badFormat, and an error from
       BandDecompress makes QuickTime stop the movie or show a blank
       frame -- for one damaged sample in a file that is otherwise fine.
       Now the sample is counted and skipped, the last good picture is
       painted again below, and the call succeeds. The decoder recovers
       by itself at the next key frame at the latest. */
    if (annexBSize == 0) { glob->statDecodeErrors++; glob->statConvertFails++; }

    /* ------------------------------------------------------------------
       h264bsdDecode consumes ONE NAL unit per call.
       ------------------------------------------------------------------
       This was the bug that produced a blank grey first frame and then
       an accumulating mess. A QuickTime sample is not one NAL unit: for
       this ffmpeg-produced clip each sample is an SEI followed by the
       slice. Calling h264bsdDecode once consumed only the SEI and the
       actual picture was never decoded at all, so the very first output
       was h264bsd's initialised buffer -- a flat grey field, Y=Cb=Cr=128.

       That is also why the damage got worse over time rather than
       staying constant: every later frame predicts from earlier ones, so
       an undecoded IDR poisons everything after it. The parts that did
       render -- the frame counter, the moving diagonal -- were the
       intra-coded macroblocks being redrawn from scratch each frame,
       sitting on top of a background that had never been decoded.

       The API is explicit about this: it returns readBytes, and the
       caller advances and calls again until the buffer is exhausted.

       Pictures are drained inside the loop rather than after it, because
       a multi-slice sample can complete a picture partway through the
       buffer, and h264bsd hands each one over exactly once.

       ------------------------------------------------------------------
       readBytes == 0 means "call me again with THIS buffer", not "stuck".
       ------------------------------------------------------------------
       h264bsd returns zero bytes consumed on purpose in two situations,
       and in both the NAL at this offset has NOT been decoded yet:

         * H264BSD_HDRS_RDY -- the first slice of a picture activated a
           new SPS. This happens on the FIRST sample every fresh decoder
           instance ever sees, i.e. on every IDR that opens a
           decompression sequence.
         * H264BSD_PIC_RDY  -- a new slice arrived while the previous
           picture was still incomplete, so h264bsd concealed and
           finished that one first.

       It sets prevBufNotFinished and expects the SAME pointer back, and
       it decodes the stored NAL on that second call. (h264bsd_decoder.c,
       the "prevBufNotFinished && byteStrm == prevBufPointer" branch.)

       The first version of this loop treated readBytes == 0 as "no
       progress" and broke out. Measured consequence, 2026-09-23: the
       poster frame of every movie was blank -- the IDR on the instance
       QuickTime uses to draw it was activated and then abandoned. Playback
       only came out right because QuickTime happened to send the IDR a
       second time to the same instance, and that second call slipped
       down the "same pointer" path. The previous session's explanation
       ("h264bsd only hands the first picture over once the next one
       begins") was wrong: for this POC-type-2 stream h264bsd outputs
       immediately; we simply never asked it to finish.

       The guard against spinning stays, because an infinite loop inside
       a QuickTime callback on Mac OS 9 is a hung machine and a power
       cycle. It just allows the retry the API asks for: up to
       kMaxDecodeStalls consecutive zero-byte returns at one offset, then
       give up on the rest of this sample. */
    {
        u32 offset = 0;
        int stalls = 0;           /* consecutive zero-byte returns */

        while (offset < annexBSize)
        {
            readBytes    = 0;
            decodeStatus = h264bsdDecode(glob->h264,
                                          glob->annexBScratch + offset,
                                          annexBSize - offset,
                                          0, &readBytes);

            /* Out of memory is the one decoder failure that is still
               reported: h264bsd could not allocate its picture buffers,
               there is nothing to show and nothing will come until it
               can. The picture buffers are gone, so forget lastPic. */
            if (decodeStatus == H264BSD_MEMALLOC_ERROR)
            {
                glob->lastPic = NULL;
                return memFullErr;
            }

            /* v14: any other decode error is survived (see above). The
               damaged NAL is skipped and the loop carries on with the
               next one in the sample; h264bsd conceals what is missing
               from the picture when the next picture starts. If it
               could not even tell where the NAL ends (readBytes 0),
               the rest of this sample is abandoned. */
            if (decodeStatus == H264BSD_ERROR)
            {
                glob->statDecodeErrors++;
                if (readBytes == 0) break;
                stalls  = 0;
                offset += readBytes;
                continue;
            }

            /* A newly activated SPS may have reallocated the DPB that
               lastPic points into. Forget it until a new picture arrives. */
            if (decodeStatus == H264BSD_HDRS_RDY)
                glob->lastPic = NULL;

            /* Drain on PIC_RDY whether or not bytes were consumed: the
               concealment case finishes a picture AND asks to be called
               again, and that picture should not wait for a later call. */
            if (decodeStatus == H264BSD_PIC_RDY)
                DrainOutputPictures(glob, params);

            if (readBytes == 0)
            {
                if (glob->stallLogs < 4)
                {
                    VLogNum(glob, "  decode: 0 bytes consumed, retrying same NAL, status",
                            (long)decodeStatus);
                    glob->stallLogs++;
                }
                if (++stalls > kMaxDecodeStalls) break;   /* never spin */
                continue;                                 /* same offset */
            }

            stalls  = 0;
            offset += readBytes;
        }
    }

#endif /* !H264_BACKEND_AVCDEC */

    /* PixMap.rowBytes packs flags (e.g. pixel-depth/pixmap-vs-bitmap bit)
       into its top two bits alongside the real byte count -- always mask
       with 0x3FFF before using it as a stride, per Inside Macintosh. */
    dstRowBytes = (long)(params->dstPixMap.rowBytes & 0x3FFF);

    /* One-shot description of what QuickTime actually handed us, in the
       verbose build only. */
    if (kH264Verbose && glob->bandCalls == 1)
    {
        LogNum(glob, "  dstPixMap.pixelSize", (long)params->dstPixMap.pixelSize);
        LogNum(glob, "  dstPixMap.rowBytes (masked)", dstRowBytes);
        LogNum(glob, "  dstPixMap.bounds.left", (long)params->dstPixMap.bounds.left);
        LogNum(glob, "  dstPixMap.bounds.top", (long)params->dstPixMap.bounds.top);
        LogNum(glob, "  dstPixMap.bounds.right", (long)params->dstPixMap.bounds.right);
        LogNum(glob, "  dstPixMap.bounds.bottom", (long)params->dstPixMap.bounds.bottom);
        LogNum(glob, "  dstRect.left", (long)params->dstRect.left);
        LogNum(glob, "  dstRect.top", (long)params->dstRect.top);
        LogNum(glob, "  dstRect.right", (long)params->dstRect.right);
        LogNum(glob, "  dstRect.bottom", (long)params->dstRect.bottom);
        /* The instance pointer identifies WHICH globals block this call
           landed on. QuickTime opens several instances while it
           negotiates, and the one that gets BandDecompress is not always
           the one PreDecompress ran on -- which is why the blit asks
           h264bsd for the geometry every frame instead of trusting the
           cached numbers. */
        LogNum(glob, "  instance (glob)", (long)glob);
        LogNum(glob, "  glob->frameWidth (cached, may be 0)", (long)glob->frameWidth);
#if !H264_BACKEND_AVCDEC
        LogNum(glob, "  h264bsdPicWidth*16", (long)h264bsdPicWidth(glob->h264) * 16);
        LogNum(glob, "  h264bsdPicHeight*16", (long)h264bsdPicHeight(glob->h264) * 16);
#endif
    }

    /* Anything still queued (a sample whose last NAL completed a
       picture) goes out here. */
#if !H264_BACKEND_AVCDEC
    DrainOutputPictures(glob, params);
#endif

#if kH264TimingTrace
    if (glob->tracedThisCall && glob->trace != NULL && glob->traceCount > 0)
    {
        glob->tracedThisCall = false;
        glob->trace[(glob->traceCount - 1) * kTraceFields + 12] =
            (glob->picsOut != picsBefore) ? (long)glob->traceSum : 0;
    }
#endif

    /* Every call must leave a picture in the destination. QuickTime
       treats BandDecompress as "decompress THIS sample into THIS
       buffer", and it does send samples that produce nothing new:
       measured on the host harness, a key frame sent twice to one
       instance -- the pattern the Mac log shows for poster frame then
       playback -- is recognised by h264bsd as a redundant copy of the
       picture it just finished and skipped. Without this, that call
       would leave QuickTime's offscreen buffer holding whatever it held
       before. Repeating the last picture is exactly right for a re-sent
       sample and, since v14, is also what a damaged sample gets.

       (The deliberate exception is a call on which the lateness policy
       skipped the blit: that one produced a picture, and was allowed to
       skip only because this same buffer already holds the previous
       one.) */
#if H264_BACKEND_AVCDEC
    if (glob->picsOut == picsBefore)
    {
        H264AvKeep(glob, params, &avFlags);
#if kH264TimingTrace
        if (glob->tracedThisCall && glob->trace != NULL && glob->traceCount > 0)
            glob->trace[(glob->traceCount - 1) * kTraceFields + 18] = avFlags;
#endif
    }
#else
    if (glob->picsOut == picsBefore && glob->lastPic != NULL)
    {
        BlitPicture(glob, glob->lastPic, params);
        glob->statRepeats++;
    }
#endif

    return noErr;
}

/* The entry point QuickTime actually calls. It times the call for the
   statistics -- but only when this instance is logging, so a quiet
   playback reads the clock only for the lateness policy (twice per call). */
pascal ComponentResult H264CodecBandDecompress(H264GlobalsPtr glob,
                                                CodecDecompressParams *params)
{
    ComponentResult result;
    UnsignedWide    t0, t1;
    Boolean         logging = LogEnabled(glob);

    if (logging) Microseconds(&t0);

#if kH264SimulateSlowMs
    {
        /* Slow-CPU experiment: burn time, so the call is as slow as it
           would be on a machine that cannot keep up. */
        UnsignedWide a, b;
        Microseconds(&a);
        do { Microseconds(&b); }
        while ((unsigned long)(b.lo - a.lo) < (unsigned long)kH264SimulateSlowMs * 1000UL);
    }
#endif

    result = BandDecompressWork(glob, params);

#if kH264LatenessPolicy || kH264TimingTrace
    /* When this call returned: the self-made clock measures how long
       QuickTime waits before the next one. */
    if (glob != NULL)
    {
        UnsignedWide r;
        Microseconds(&r);
        glob->scReturnLo   = (unsigned long)r.lo;
        glob->scHaveReturn = true;
    }
#endif

    if (logging)
    {
        Microseconds(&t1);
        if (glob->statBandCalls >= kStatSkipCalls)
        {
            unsigned long us = (unsigned long)(t1.lo - t0.lo);
            glob->statDecodeMicros += us;
            if (us > glob->statMaxMicros) glob->statMaxMicros = us;
            glob->statTimedCalls++;
        }
        glob->statBandCalls++;
    }

    return result;
}

/* Report this instance's counters. Called from Close. QuickTime opens
   several instances per movie, most of them short-lived ones that never
   decode anything; an instance that showed no picture reports nothing.
   v14: the counters are per instance, so this is one movie's playback,
   not everything since the component was loaded. */
static void LogPlaybackStats(H264GlobalsPtr glob)
{
    unsigned long wallMicros, runPics, runMicros, runCentis;

    if (glob == NULL || glob->statPictures == 0) return;
    if (!LogEnabled(glob)) return;

    LogAppend(glob, "=== H264 Decoder v17d: playback statistics ===");
    LogNum(glob, "STATS pictures shown", (long)glob->statPictures);
    LogNum(glob, "STATS BandDecompress calls", (long)glob->statBandCalls);
    LogNum(glob, "STATS calls that repeated the last picture", (long)glob->statRepeats);
    LogNum(glob, "STATS decode errors survived", (long)glob->statDecodeErrors);
    LogNum(glob, "STATS ... of which samples that would not convert", (long)glob->statConvertFails);
    LogNum(glob, "STATS PreDecompress bufferSize", (long)glob->statPreBufferSize);
    LogNum(glob, "STATS largest sample bytes", (long)glob->statMaxSample);
    LogNum(glob, "STATS Annex-B scratch regrown", (long)glob->statScratchGrows);
    LogNum(glob, "STATS blits to y420", (long)glob->statBlitY420);
    LogNum(glob, "STATS blits to 2vuy", (long)glob->statBlit2vuy);
    LogNum(glob, "STATS blits to 32-bit RGB", (long)glob->statBlitRGB32);
    LogNum(glob, "STATS blits to 16-bit RGB", (long)glob->statBlitRGB16);
    if (glob->statBlitOther > 0)
        LogNum(glob, "STATS blits to an UNHANDLED format", (long)glob->statBlitOther);
    if (glob->statTimedCalls > 0)
    {
        LogNum(glob, "STATS avg us per call (after first 8)",
               (long)(glob->statDecodeMicros / glob->statTimedCalls));
        LogNum(glob, "STATS max us per call (after first 8)", (long)glob->statMaxMicros);
    }
    if (glob->statBlitTimed > 0)
    {
        LogNum(glob, "STATS avg us in blit (after first 8)",
               (long)(glob->statBlitMicros / glob->statBlitTimed));
        LogNum(glob, "STATS max us in blit (after first 8)", (long)glob->statBlitMaxMicros);
    }

    wallMicros = glob->statLastPicLo - glob->statFirstPicLo;   /* unsigned: wrap-safe */
    LogNum(glob, "STATS wall ms first->last picture, incl. pauses",
           (long)(wallMicros / 1000UL));

    /* The longest run, counting the one still in progress. */
    runPics   = glob->statBestRunPics;
    runMicros = glob->statBestRunMicros;
    if (glob->statRunPics > 1 &&
        (unsigned long)(glob->statLastPicLo - glob->statRunStartLo) > runMicros)
    {
        runPics   = glob->statRunPics;
        runMicros = glob->statLastPicLo - glob->statRunStartLo;
    }
    runCentis = runMicros / 10000UL;
    LogNum(glob, "STATS playback run pictures", (long)runPics);
    LogNum(glob, "STATS playback run ms", (long)(runMicros / 1000UL));
    if (runCentis > 0 && runPics > 1)
        LogNum(glob, "STATS playback run fps x100",
               (long)(((runPics - 1) * 10000UL) / runCentis));

    /* What the lateness policy saw and did. "calls with a frame time"
       at zero means QuickTime never gave us a usable clock and the
       policy could not act at all. */
    LogNum(glob, "LATE calls with a clock (frameTime or self-made)", (long)glob->statTimedFrames);
    LogNum(glob, "LATE ... of which the self-made clock", (long)glob->statSelfClockCalls);
    LogNum(glob, "LATE calls that were late", (long)glob->statLateCalls);
    LogNum(glob, "LATE worst lateness ms", glob->statMaxLateMs);
    LogNum(glob, "LATE calls QuickTime flagged catch-up", (long)glob->statCatchUpCalls);
    LogNum(glob, "LATE pictures decoded but not painted", (long)glob->statBlitSkips);
    LogNum(glob, "LATE disposable frames not decoded", (long)glob->statNonRefSkips);
    LogNum(glob, "LATE times deblocking was switched off", (long)glob->statDeblockOffs);
    LogNum(glob, "LATE pictures without deblocking", (long)glob->statNoDeblockPics);
    LogNum(glob, "LATE times we waited for a key frame", (long)glob->statIDRWaits);
    LogNum(glob, "LATE samples dropped while waiting", (long)glob->statDroppedCalls);
}

/* Write the timing trace (kH264TimingTrace), one line per call:

       T <call> <frameNumber> <due> <now> <rate> <conditionFlags> <callerFlags> <lateMs> <action> <present> <scale> <duration> <wallUs> <picsum>

   due and now are the low 32 bits of the frame time and the time-base
   time, in the frame time's own scale; rate is a 16.16 Fixed (65536 =
   normal speed, -1 = no frame time); action is 0 decode and paint,
   1 decode only, 2 dropped; present is a bit set (1 frameTime, 2 its
   time base, 4 syncFrameTime); scale and duration come from frameTime;
   wallUs is Microseconds() when the call arrived (low 32 bits); picsum
   is a checksum of the picture the call produced (0 = none). Lines are gathered a few dozen at a time so
   a 900-call trace costs about 25 file writes, not 900. */
static void LogTimingTrace(H264GlobalsPtr glob)
{
    char  buf[2048];
    long  used = 0;
    long  i;
    short f;

    if (glob == NULL || glob->trace == NULL || glob->traceCount <= 0) return;
    if (!LogEnabled(glob)) return;

    LogAppend(glob, "=== timing trace: T call frameNumber due now rate conditionFlags callerFlags lateMs action present scale duration wallUs picsum syncHi syncLo syncScale syncDur size avFlags ===");

    for (i = 0; i < glob->traceCount; i++)
    {
        const long *t = glob->trace + i * kTraceFields;

        buf[used++] = 'T';
        buf[used++] = ' ';
        used += LogFormatNum(buf + used, i);
        for (f = 0; f < kTraceFields; f++)
        {
            buf[used++] = ' ';
            used += LogFormatNum(buf + used, t[f]);
        }
        buf[used++] = '\r';

        if (used > (long)sizeof(buf) - 128)
        {
            LogWrite(glob, buf, used);
            used = 0;
        }
    }
    if (used > 0) LogWrite(glob, buf, used);
}

/* ==================================================================
   Base Codec dispatch glue
   ==================================================================
   This is the standard, documented way every third-party QuickTime
   Image Compressor/Decompressor component of this era wires itself up:
   define which of your functions implement which Base Codec callback,
   then #include the SDK-provided glue that expands into the actual
   Component Manager entry point (ComponentMain) and the giant selector
   switch, handling all the PowerPC routine-descriptor/calling-convention
   plumbing for you.

   ImageCodec.k.h and ComponentDispatchHelper.c ship as part of the
   QuickTime headers bundled with CodeWarrior's Universal Interfaces
   (look under something like "CIncludes" / "QuickTime" in the
   CodeWarrior support tree). If the build can't find them, that's the
   single biggest "we're missing an SDK piece" signal to watch for --
   see the porting notes file for what to do about it.

   Routines we do NOT implement (GetCompressedImageSize, GetSimilarity,
   TrimImage, and the compression-side calls) simply aren't listed here;
   Base Codec supplies harmless defaults for anything we leave out. */

/* ------------------------------------------------------------------
   Corrected against the real ImageCodec.k.h / ComponentDispatchHelper.c
   pulled out of the VM. The first draft defined only the two
   IMAGECODEC_* macros and invented two more (IMAGECODEC_DECOMPRESS,
   IMAGECODEC_QUALITY_CHECK) that nothing in Apple's headers actually
   reads. That's not enough, because the two headers do two *different*
   jobs and want two *different* sets of macros:

     - ImageCodec.k.h reads only IMAGECODEC_BASENAME/IMAGECODEC_GLOBALS,
       and all it does with them is emit the prototypes for our
       callbacks (H264CodecBandDecompress and friends).

     - ComponentDispatchHelper.c reads COMPONENT_BASENAME,
       COMPONENT_GLOBALS, COMPONENT_DISPATCH_FILE and either
       COMPONENT_UPP_SELECT_ROOT or the UPP/SELECT prefix pair. It
       falls back to CALLCOMPONENT_BASENAME/CALLCOMPONENT_GLOBALS if
       the COMPONENT_* pair isn't set, which is the spelling Apple's
       own sample codecs use -- so we set those. Nothing anywhere
       bridges IMAGECODEC_* to COMPONENT_* automatically; wiring the
       two together is the codec author's job, and skipping it is
       exactly what produced the three #error directives at
       ComponentDispatchHelper.c lines 16/25/31.

   Two details worth writing down because they're easy to get wrong:

   1. GLOBALS must expand to a complete parameter *declaration*,
      including the variable name `storage` -- not just a type. The
      dispatcher ComponentDispatchHelper.c generates is literally

          ComponentResult H264CodecComponentDispatch(
              ComponentParameters *params, COMPONENT_GLOBALS())
          { ... CallComponentFunctionWithStorageProcInfo(
                    (Handle) storage, params, ...); }

      so it refers to `storage` by name in the body. Expanding to a
      bare `H264GlobalsPtr` would produce an unnamed parameter and an
      undeclared-identifier error on `storage`. (The prototypes
      ImageCodec.k.h emits then read
      `H264CodecBandDecompress(H264GlobalsPtr storage, ...)`, which is
      fine -- C doesn't require parameter names to match between a
      declaration and the definition, so our definitions can keep
      calling it `glob`.)

   2. COMPONENT_UPP_SELECT_ROOT() is the root that gets glued onto both
      `upp...ProcInfo` and `k...Select` names. `ImageCodec` is right
      because the constants ImageCodec.k.h actually defines are spelled
      uppImageCodecBandDecompressProcInfo, uppImageCodecPreflightProcInfo,
      and so on -- verified by grepping the real header rather than
      guessing.

   COMPONENT_DISPATCH_FILE names the selector table that gets #included
   twice (once to build the per-range dispatch arrays, once to build the
   range table). The first attempt pointed this at "ImageCodecDispatch.h"
   on the assumption Apple shipped a ready-made one. Apple does not --
   this CodeWarrior's ComponentIncludes folder contains exactly 16 files
   and none of them is a dispatch table. Every component of this era
   wrote its own, so H264CodecDispatch.h is ours; see the long comment
   at the top of that file for the selector-numbering rules, which are
   unforgiving and mostly not compiler-checkable. */

#define IMAGECODEC_BASENAME()          H264Codec
#define IMAGECODEC_GLOBALS()           H264GlobalsPtr storage

#define CALLCOMPONENT_BASENAME()       IMAGECODEC_BASENAME()
#define CALLCOMPONENT_GLOBALS()        IMAGECODEC_GLOBALS()

#define COMPONENT_UPP_SELECT_ROOT()    ImageCodec
#define COMPONENT_DISPATCH_FILE        "H264CodecDispatch.h"

/* ------------------------------------------------------------------
   Exporting the entry point -- the thing that made registration fail
   ------------------------------------------------------------------
   This bracket is not cosmetic; without it the component builds, links,
   installs, loads at boot, and then simply never registers, with no
   error message anywhere. Worth understanding, because it cost a whole
   debugging cycle.

   Registration works like this: the Component Manager reads our 'thng'
   resource, follows its platform entry to 'dlle' 256, reads the *symbol
   name* stored there ("H264CodecComponentDispatch"), loads our PEF code
   fragment, and calls FindSymbol() looking for that name. If the symbol
   is not in the fragment's export table, FindSymbol fails and the
   component is silently skipped -- no crash, no console message, it just
   never appears in the codec list. Which is exactly the symptom we saw:
   QuickTime opened the test movie fine and reported "the required
   compressor could not be found".

   CodeWarrior's PPC PEF panel defaults Export Symbols to *None*, so our
   fragment was exporting nothing at all. Setting it to "All globals"
   does fix that, but it also tries to export every global in the link,
   including MSL's C++ template instantiations, whose mangled names run
   to hundreds of bytes and overflow the Str255 that CFM uses for symbol
   names -- that produced a flood of "Symbol may be too long for export"
   diagnostics.

   So: set PPC PEF -> Export Symbols to "Use #pragma", and export exactly
   the handful of symbols ComponentDispatchHelper.c generates -- the
   dispatcher itself, its routine descriptor, and the CanDo it
   synthesises. Nothing else in the component needs to be visible from
   outside, and h264bsd's ~200 internal functions stay private, which is
   what we want anyway.

   ImageCodec.k.h is inside the bracket only because it is harmless
   there: it contains prototypes, not definitions, so it contributes no
   exported symbols. */
/* H264_HOST_BUILD: the host harness calls the entry points above
   directly and has no Component Manager, so everything from here to the
   end of the file -- the dispatch glue and the old diagnostic entry
   point -- is for the Mac only. */
#ifndef H264_HOST_BUILD

#pragma export on

#include "ImageCodec.k.h"
#include "ComponentDispatchHelper.c"

/* ==================================================================
   Diagnostic entry point -- temporary, and here to answer one question
   ==================================================================

   Where we are: the Component Manager now follows our 'thng' to the
   'dlle' and tries to call whatever symbol it names, and then the
   CALLER dies with a type 3 (illegal instruction) before a single
   instruction of ours runs. Both plausible symbols have been tried and
   both crash the caller:

       H264CodecComponentDispatchRD   (class 1, data -- a RoutineDescriptor)
       H264CodecComponentDispatch     (class 2, TVector -- the function)

   Meanwhile RegisterComponent(), handed the RoutineDescriptor directly,
   works completely: Open, Version, CanDo, Close, all logged. So our
   code is callable -- just not the way the 'dlle' path calls it.

   Two possibilities remain and they need completely different fixes:

     (a) The Component Manager's call mechanism is fine, and something
         about the real dispatcher -- its ProcInfo, its globals
         argument, the helper's generated glue -- is wrong when reached
         this particular way.

     (b) The address the Component Manager ends up with is simply wrong,
         whatever symbol we name, and the 'dlle' route cannot work here.

   Nothing measured so far separates those, because both look identical
   from outside: a crash with an empty log.

   So: this entry point does nothing except write one line to the log
   and return. Point 'dlle' 256 at it and the answer falls out.

       a line appears   -> (a). The call mechanism works. The fault is
                           in the real dispatcher, and the selector
                           number in the log says what it was called
                           with, which is a strong hint.
       nothing, crash   -> (b). The address is wrong no matter what.
                           Abandon 'dlle' and register the component
                           programmatically instead -- already proven
                           to work.

   It deliberately answers badComponentSelector to everything, so a
   build carrying it is a diagnostic build, not a working codec. Both
   forms are exported so either can be named in the 'dlle' without
   another rebuild.

   DELETE THIS once the question is settled. ================== */

pascal ComponentResult H264CodecTinyEntry(ComponentParameters *params,
                                          void *storage);

pascal ComponentResult H264CodecTinyEntry(ComponentParameters *params,
                                          void *storage)
{
    #pragma unused(storage)

    /* v14: this entry point used to write a line to the log. The
       loggers now need an instance to decide whether logging is on, and
       there is none here, so it only answers. Nothing refers to it any
       more; it is kept so the library's export list does not change. */
    if (params == NULL) return paramErr;
    return badComponentSelector;
}

/* Same descriptor shape ComponentDispatchHelper.c builds for its own
   dispatcher: pascal, 4-byte result, two 4-byte stack parameters. */
struct RoutineDescriptor H264CodecTinyEntryRD =
    BUILD_ROUTINE_DESCRIPTOR((kPascalStackBased | RESULT_SIZE(kFourByteCode) |
                              STACK_ROUTINE_PARAMETER(1, kFourByteCode) |
                              STACK_ROUTINE_PARAMETER(2, kFourByteCode)),
                             H264CodecTinyEntry);

#pragma export off

#endif /* !H264_HOST_BUILD */
