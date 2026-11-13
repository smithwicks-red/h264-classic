#define kOfferYUVOutput    1

/* Diagnostics master switch -- see "Diagnostics master switch" further
   down for everything it controls. Defined up here, not there, because
   code above that point (BlitPicture's per-format counters) uses it. */
#define kH264Diagnostics   1   /* HARDWARE TEST BUILD (2026-09-24): pixel format + playback STATS to the desktop log on a real Mac. 0 = quiet build. */

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

/* Forward declarations for the diagnostic loggers, which are defined
   much further down this file.

   BlitPicture calls LogNum, and BlitPicture sits above the loggers. In
   C89 an undeclared call quietly invents `extern int LogNum()`, and the
   later `static void LogNum(...)` then collides with it -- a compile
   error in CodeWarrior, found by reading the definition order rather
   than by building, because the host blit test extracted only the
   blit functions and never saw BlitPicture. Nothing written before the
   YUV path needed these; everything in the original file calls the
   loggers after they are defined. */
static void LogAppend(const char *msg);
static void LogNum(const char *label, long v);
static void DumpBytesToShare(const char *pathC, const void *data, long len);

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
static unsigned long gStatPictures      = 0;
static unsigned long gStatBandCalls     = 0;
static unsigned long gStatTimedCalls    = 0;
static unsigned long gStatDecodeMicros  = 0;
static unsigned long gStatMaxMicros     = 0;   /* slowest timed call */
static unsigned long gStatFirstPicLo    = 0;
static unsigned long gStatLastPicLo     = 0;
static unsigned long gStatPrintedAt     = 0;   /* pictures at last report */
static unsigned long gStatRepeats       = 0;   /* calls that re-blitted the last picture */

/* Which destination format each blit actually went to. The poster frame
   and playback can use DIFFERENT destinations -- measured 2026-09-24:
   the poster frame is drawn straight to the 32-bit screen, playback into
   an offscreen '2vuy' buffer -- so a one-shot "first blit" report is not
   enough to say what playback used. On the TiBook's Radeon the answer
   may be 'y420'; these counters will say. */
static unsigned long gStatBlitY420      = 0;
static unsigned long gStatBlit2vuy      = 0;
static unsigned long gStatBlitRGB32     = 0;
static unsigned long gStatBlitOther     = 0;

static unsigned long gStatRunStartLo    = 0;   /* first picture of the current run */
static unsigned long gStatRunPics       = 0;   /* pictures in the current run */
static unsigned long gStatBestRunPics   = 0;   /* longest finished run, by duration */
static unsigned long gStatBestRunMicros = 0;

#define kStatSkipCalls     8
#define kStatRunGapMicros  500000UL   /* half a second without a picture ends a run */

/* Record one picture's arrival time for the run statistics. Defined up
   here, next to the counters, so the drain loop can call it without a
   forward declaration. */
static void StatNotePicture(unsigned long nowLo)
{
    if (gStatPictures > 0 &&
        (unsigned long)(nowLo - gStatLastPicLo) >= kStatRunGapMicros)
    {
        /* The gap ends the run that finished at gStatLastPicLo. */
        unsigned long dur = gStatLastPicLo - gStatRunStartLo;
        if (gStatRunPics > 1 && dur > gStatBestRunMicros)
        {
            gStatBestRunMicros = dur;
            gStatBestRunPics   = gStatRunPics;
        }
        gStatRunPics = 0;
    }

    if (gStatPictures == 0) gStatFirstPicLo = nowLo;
    if (gStatRunPics == 0)  gStatRunStartLo = nowLo;
    gStatRunPics++;
    gStatLastPicLo = nowLo;
    gStatPictures++;
}

static void LogPlaybackStats(void);   /* defined after BandDecompress */

/* How many times in a row BandDecompress will hand h264bsd the same NAL
   when it consumes zero bytes. The API asks for exactly one retry (see
   the long comment in BandDecompressWork); 2 leaves room for one retry
   that itself reports another boundary, and still guarantees the loop
   ends. */
#define kMaxDecodeStalls  2
