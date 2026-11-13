/*
    MP4Fix.c  --  make a standard MP4 open AND play, with sound, in
                  QuickTime on Mac OS 9

    ==================================================================
    THE SHORT VERSION
    ==================================================================
    QuickTime has two parsers for what is essentially the same box
    structure, and they fail in opposite directions.

      * The MPEG-4 importer understands 'esds' natively -- audio is what
        it was written for -- but it PREDATES H.264 ('avc1' arrived with
        QuickTime 7). Handed an 'avc1' sample entry it builds an
        ImageDescription and silently drops the 'avcC' extension that
        carries the SPS/PPS. PreDecompress then asks for an extension
        that is not there and you get

            error -8971   codecExtensionNotFoundErr

      * The MOV parser copies EVERY sample-description extension
        verbatim, including ones it has never heard of, so 'avcC'
        survives and our decoder works. But it expects audio described
        the QuickTime way, and an ISO 'mp4a' entry is not that.

    So routing the file to the MOV parser fixes video and breaks audio.
    This program does both halves: it routes the file to the MOV parser
    AND rewrites the audio sample description into the form the MOV
    parser expects.

    ==================================================================
    PART ONE: getting to the MOV parser (four bytes)
    ==================================================================
    Measured on the bench across six probes of one identical bitstream,
    this needs two things and neither alone is enough:

      1. the FILE TYPE / extension must say QuickTime movie, because
         that is what chooses the importer; and
      2. the ftyp major brand must read 'qt  ', because the MOV parser
         rejects a leading ftyp box branded 'isom'.

    Patching the brand while leaving the name .mp4 still fails with
    -8971. Renaming to .mov without patching the brand fails with "not a
    file that QuickTime understands". Doing both works.

    ==================================================================
    PART TWO: making the sound play (fifty-six bytes)
    ==================================================================
    An ISO file describes AAC with a VERSION 0 AudioSampleEntry:

        mp4a  (87 bytes)
          channels=2 sampleSize=16 compressionID=0x0000 packetSize=0
          sampleRate=48000.0
            esds  (51 bytes)          <- bare, at the top of the entry

    In a QuickTime sound description, version 0 means "one sample per
    packet, fixed frame size". That is true for raw PCM and nonsense for
    AAC, where one entry in the stsz table is a compressed frame of
    perhaps 230 bytes carrying 1024 audio samples. QuickTime cannot work
    out the packet framing and the decompressor produces silence.

    Note that it still READS the esds -- Get Movie Properties correctly
    reports "MPEG-4 Audio, 48 kHz, Stereo, 16 bits". It knows exactly
    what the codec is. It just cannot work out how the bytes are cut up.

    QuickTime writes a VERSION 1 SoundDescription instead:

        mp4a  (143 bytes)
          channels=2 sampleSize=16 compressionID=0xFFFE packetSize=0
          sampleRate=48000.0
          samplesPerPacket=1024 bytesPerPacket=0 bytesPerFrame=0
          bytesPerSample=2
            wave  (91 bytes)
              frma = 'mp4a'
              mp4a = 00 00 00 00
              esds = (the original bytes, untouched)
              <8-byte terminator: size 8, type 0>

    compressionID 0xFFFE is -2, kVariableCompression: "the frame size
    varies, consult samplesPerPacket". The 'wave' atom is QuickTime's
    siDecompressionParam extension -- the Sound Manager looks for codec
    configuration THERE, not loose in the entry.

    This layout was not guessed. The reference file was produced with
    `ffmpeg -c copy -f mov`, whose MOV muxer has interoperated with real
    QuickTime for twenty years, and the entry this program builds is
    identical to ffmpeg's in every field that matters -- including the
    8-byte terminator atom rather than the bare four zero bytes some
    documentation describes.

    Verified playing on Mac OS 9.2.2 under QuickTime 6.0.3: H.264 video
    through our component and AAC audio through QuickTime's, from one
    file, no error dialog.

    ==================================================================
    THE OFFSET TRAP -- read this before changing anything
    ==================================================================
    The new entry is 56 bytes longer than the old one. That grows 'stsd',
    which grows 'stbl', 'minf', 'mdia', 'trak' and 'moov' -- and in a
    faststart file (ftyp / moov / mdat) it pushes 'mdat' 56 bytes down
    the file.

    'stco' holds ABSOLUTE FILE OFFSETS to every chunk. Grow moov without
    fixing them and every chunk read lands 56 bytes early; a decoder sees
    garbage NAL lengths and the file is quietly ruined.

    We have already walked into this once. The earlier idea of DELETING
    the ftyp box to make the file start at 'moov' corrupts files for
    exactly this reason: measured, an intact file had stco = 3327 with
    mdat at 3319, and with ftyp removed stco was still 3327 while mdat
    had moved to 3287. Every read 32 bytes late.

    So this program, after splicing the new entry in, walks every stco
    and co64 in the file and adds the delta to every offset that points
    at or past the splice point. A chunk offset never lands inside moov,
    so the test is simple and safe: shift it if it is >= the splice.

    co64 is handled with manual 32-bit carry rather than a 64-bit type,
    on purpose. macTLS discovered that CodeWarrior 8 PPC MISCOMPILES
    64-bit shift-by-constant -- `(int64_t)a * const` writes
    `a >> log2(const)` into the high word -- and that is the exact
    compiler this project uses. Do not introduce 64-bit arithmetic here.

    ==================================================================
    WHY THIS ONE WRITES A NEW FILE
    ==================================================================
    The four-byte patch could be done in place because nothing moved.
    The audio fix moves bytes, so in-place editing would mean a
    backwards block copy over the user's only copy of the movie, with no
    way back if anything failed halfway. Instead we read, transform and
    write a new file, and the original is never touched.

    Only 'moov' is held in memory. It is usually tens of kilobytes; the
    media data is copied through a 64K buffer and never resident. A
    41-second 360x240 clip has a 26K moov.

    ==================================================================
    BUILDING
    ==================================================================
    Same recipe as the registrar. In CodeWarrior 8.3 the path is
    File -> New -> Mac OS C Stationery -> Mac OS Classic ->
    Standard Console -> "C Console Classic" (not the AltiVec variant:
    the Pismo is a G3). Remove the sample HelloWorld.c, add this one
    file, keep the MSL and InterfaceLib the stationery already links.
    No resources, no #pragma export. An ordinary classic PPC
    application, NOT Carbon.

    If the first build stops on "illegal precompiled header version",
    clear Settings -> C/C++ Language -> Prefix File. That is a known
    quirk of this CW 8.3 install, not a problem with this file.
*/

#include <stdio.h>
#include <string.h>

#include <Types.h>
#include <Files.h>
#include <StandardFile.h>
#include <Errors.h>
#include <Memory.h>
#include <Script.h>   /* smSystemScript, passed to FSpCreate. It lives here,
                         and none of the headers above is guaranteed to
                         pull it in -- H264Codec.c avoids the constant for
                         exactly that reason. */

#ifdef __MWERKS__
#include <SIOUX.h>    /* CodeWarrior's console window; see the top of
                         main(). Guarded so the host-side regression test,
                         which compiles this file with gcc, never sees it. */
#endif


/* ==================================================================
   Options
   ================================================================== */

/*
    kPatchMode -- how to route the file to the MOV parser.

    Mode 1 (default, PROVEN ON HARDWARE)
        Overwrite the ftyp major brand at offset 8 with 'qt  '. The MOV
        parser accepts a leading ftyp box branded 'qt  ' and rejects one
        branded 'isom'.

    Mode 2 (alternative, structurally verified, untried on the Mac)
        Overwrite the ATOM TYPE at offset 4, turning 'ftyp' into 'free'.
        A free box is padding every parser skips, so the file then reads
        free / moov / mdat -- an ordinary QuickTime layout with no ftyp
        box to have an opinion about at all.

    Mode 2 may prove more robust because it does not depend on the MOV
    parser liking any particular brand. Both are offset-safe: the box
    keeps its length either way, so nothing moves.
*/
#define kPatchMode      1

/*
    kFixAudio -- 1 to also rewrite the audio sample description (writes
    a new file), 0 for the original four-byte-only behaviour (edits in
    place). Set this to 0 if you only care about picture, or if you are
    debugging and want the smallest possible change.
*/
#define kFixAudio       1

#define kQTBrand        "qt  "
#define kFreeAtom       "free"

#define kMovieType      'MooV'
#define kMovieCreator   'TVOD'

/* One AAC-LC frame. AAC-HE would be 2048, but h264bsd's companion here
   is plain LC and QuickTime 6 has no HE decoder anyway. */
#define kAACSamplesPerPacket   1024

/* -2 as an unsigned 16-bit field: kVariableCompression. */
#define kVariableCompression   0xFFFEUL

/* Refuse absurd moov sizes rather than trying to allocate them. 16 MB
   is far more sample table than any movie this machine can play. */
#define kMaxMoovSize    (16L * 1024L * 1024L)

#define kCopyBufSize    (64L * 1024L)

#define kMaxDepth       24


/* ==================================================================
   Byte helpers

   Everything here is big-endian, and so is the PowerPC, so a direct
   cast would usually work. We assemble bytes by hand anyway: sample
   entries are not guaranteed aligned, and being explicit costs nothing
   and removes a whole class of "works until it doesn't" bug.

   These shift 32-bit values, which is safe. See the warning above
   about 64-bit shifts under CodeWarrior 8.
   ================================================================== */

static unsigned long RD32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
         | ((unsigned long)p[2] <<  8) | ((unsigned long)p[3]);
}

static unsigned short RD16(const unsigned char *p)
{
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static void WR32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >>  8) & 0xFF);
    p[3] = (unsigned char)( v        & 0xFF);
}

static void WR16(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)((v >> 8) & 0xFF);
    p[1] = (unsigned char)( v       & 0xFF);
}

static void WRTYPE(unsigned char *p, const char *fourcc)
{
    p[0] = (unsigned char)fourcc[0];
    p[1] = (unsigned char)fourcc[1];
    p[2] = (unsigned char)fourcc[2];
    p[3] = (unsigned char)fourcc[3];
}

static int ISTYPE(const unsigned char *p, const char *fourcc)
{
    return (p[0] == (unsigned char)fourcc[0]
         && p[1] == (unsigned char)fourcc[1]
         && p[2] == (unsigned char)fourcc[2]
         && p[3] == (unsigned char)fourcc[3]);
}

static void PrintPStr(ConstStr255Param p)
{
    int i;
    for (i = 1; i <= p[0]; i++) putchar(p[i]);
}

static void PrintFourCC(const unsigned char *b)
{
    int i;
    for (i = 0; i < 4; i++)
    {
        unsigned char c = b[i];
        putchar((c >= 32 && c < 127) ? c : '.');
    }
}

static int PStrEndsWith(ConstStr255Param p, const char *suffix)
{
    int slen = (int)strlen(suffix);
    int plen = p[0];
    int i;

    if (plen < slen) return 0;

    for (i = 0; i < slen; i++)
    {
        char a = (char)p[plen - slen + 1 + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}


/* ==================================================================
   Box walking, inside an in-memory moov
   ================================================================== */

/* The boxes we descend into. 'stsd' is NOT here because its children
   start 8 bytes further in (past version/flags and entry_count) and so
   it needs its own case. */
static int IsContainer(const unsigned char *typ)
{
    return ISTYPE(typ, "moov") || ISTYPE(typ, "trak") || ISTYPE(typ, "mdia")
        || ISTYPE(typ, "minf") || ISTYPE(typ, "stbl") || ISTYPE(typ, "edts")
        || ISTYPE(typ, "udta") || ISTYPE(typ, "dinf") || ISTYPE(typ, "wave");
}

typedef struct
{
    unsigned char  *buf;          /* the moov box, header included      */
    long            size;

    long            entryOff;     /* the mp4a entry, -1 if not found    */
    long            entrySize;
    int             entryCount;   /* how many we found; >1 is refused   */

    long            anc[kMaxDepth];   /* offsets of enclosing boxes     */
    int             ancCount;
} MoovScan;


static void ScanBoxes(MoovScan *s, long start, long end,
                      long *stack, int depth)
{
    long off = start;

    while (off + 8 <= end)
    {
        unsigned long  size = RD32(s->buf + off);
        unsigned char *typ  = s->buf + off + 4;
        long           hdr  = 8;

        /* A 64-bit size field. We never write one and no sane moov has
           one, but read it rather than misparsing. The high word is
           read separately so no 64-bit arithmetic appears. */
        if (size == 1)
        {
            if (off + 16 > end) return;
            if (RD32(s->buf + off + 8) != 0) return;   /* > 4 GB moov: no */
            size = RD32(s->buf + off + 12);
            hdr  = 16;
        }
        else if (size == 0)
        {
            size = (unsigned long)(end - off);
        }

        if (size < (unsigned long)hdr || off + (long)size > end) return;

        if (ISTYPE(typ, "mp4a") && depth > 0)
        {
            /* Only count it as a sample entry if its parent was stsd.
               The identically-named 12-byte box we write inside 'wave'
               must never be mistaken for one -- which is exactly the
               bug that would appear if you ran this program twice. */
            long  parent = stack[depth - 1];
            if (ISTYPE(s->buf + parent + 4, "stsd"))
            {
                s->entryCount++;
                if (s->entryOff < 0)
                {
                    int i;
                    s->entryOff  = off;
                    s->entrySize = (long)size;
                    s->ancCount  = depth;
                    for (i = 0; i < depth && i < kMaxDepth; i++)
                        s->anc[i] = stack[i];
                }
            }
        }

        if (depth < kMaxDepth)
        {
            stack[depth] = off;

            if (IsContainer(typ))
                ScanBoxes(s, off + hdr, off + (long)size, stack, depth + 1);
            else if (ISTYPE(typ, "stsd"))
                ScanBoxes(s, off + hdr + 8, off + (long)size, stack, depth + 1);
        }

        off += (long)size;
    }
}


/* ==================================================================
   Building the QuickTime version-1 sound description
   ==================================================================
   Returns the new entry in a freshly allocated block, or NULL with a
   reason printed. The caller disposes it.
*/

static unsigned char *BuildQTAudioEntry(const unsigned char *old,
                                        long  oldSize,
                                        long *newSizeOut)
{
    unsigned short  version;
    unsigned short  channels, sampleSize;
    const unsigned char *rate;
    const unsigned char *esds = NULL;
    long            esdsSize = 0;
    long            off;
    long            waveSize, newSize;
    unsigned char  *nw;
    long            p;

    if (oldSize < 36)
    {
        printf("  Audio: the mp4a entry is only %ld bytes -- too short to\n"
               "         be a sound description. Leaving audio alone.\n",
               oldSize);
        return NULL;
    }

    version = RD16(old + 16);
    if (version != 0)
    {
        printf("  Audio: sound description is already version %d.\n"
               "         Nothing to do -- this file was already written\n"
               "         the QuickTime way.\n", (int)version);
        return NULL;
    }

    channels   = RD16(old + 24);
    sampleSize = RD16(old + 26);
    rate       = old + 32;          /* 16.16 fixed, copied verbatim */

    /* Everything past the 36-byte version-0 record is extensions.
       Find the bare esds among them. */
    off = 36;
    while (off + 8 <= oldSize)
    {
        unsigned long sz = RD32(old + off);
        if (sz < 8 || off + (long)sz > oldSize) break;
        if (ISTYPE(old + off + 4, "esds"))
        {
            esds     = old + off;
            esdsSize = (long)sz;
        }
        off += (long)sz;
    }

    if (esds == NULL)
    {
        printf("  Audio: no 'esds' extension inside the mp4a entry.\n"
               "         Without the decoder config there is nothing to\n"
               "         wrap, so audio is left exactly as it was.\n");
        return NULL;
    }

    /* NOTE, because it is a real behaviour and not an accident: any
       extension in the entry OTHER than esds is DROPPED. ffmpeg-written
       files commonly carry a 'btrt' bitrate box alongside the esds, so
       this fires often -- their mp4a entries come in at 107 or 110 bytes
       rather than the 87 of the Archive file.

       Dropping it is deliberate. We are translating the entry into
       QuickTime's dialect, QuickTime 6 does not read 'btrt', and
       carrying unknown boxes across into a version-1 record risks
       confusing the very parser we are trying to satisfy. Nothing we
       drop affects decoding: the esds holds the entire decoder
       configuration. If some future format puts something load-bearing
       beside the esds, this is the line that will need revisiting. */

    /* wave = header + frma(12) + mp4a(12) + esds + terminator(8) */
    waveSize = 8 + 12 + 12 + esdsSize + 8;
    newSize  = 8 + 6 + 2          /* box header, reserved, dref index   */
             + 2 + 2 + 4          /* version, revision, vendor          */
             + 2 + 2 + 2 + 2      /* channels, size, compID, packetSize */
             + 4                  /* sample rate                        */
             + 16                 /* the four version-1 fields          */
             + waveSize;

    nw = (unsigned char *)NewPtrClear(newSize);
    if (nw == NULL)
    {
        printf("  Audio: out of memory building the new entry.\n");
        return NULL;
    }

    p = 0;
    WR32(nw + p, (unsigned long)newSize);      p += 4;
    WRTYPE(nw + p, "mp4a");                    p += 4;
    /* reserved[6] stays zero */                p += 6;
    WR16(nw + p, RD16(old + 14));              p += 2;  /* dref index   */
    WR16(nw + p, 1);                           p += 2;  /* VERSION 1    */
    WR16(nw + p, 0);                           p += 2;  /* revision     */
    /* vendor stays zero */                     p += 4;
    WR16(nw + p, channels);                    p += 2;
    WR16(nw + p, sampleSize);                  p += 2;
    WR16(nw + p, (unsigned short)kVariableCompression);  p += 2;
    WR16(nw + p, 0);                           p += 2;  /* packetSize   */
    nw[p] = rate[0]; nw[p+1] = rate[1];
    nw[p+2] = rate[2]; nw[p+3] = rate[3];      p += 4;

    /* --- the four fields that version 1 adds --- */
    WR32(nw + p, (unsigned long)kAACSamplesPerPacket);  p += 4;
    WR32(nw + p, 0);                           p += 4;  /* bytesPerPacket */
    WR32(nw + p, 0);                           p += 4;  /* bytesPerFrame  */
    WR32(nw + p, 2);                           p += 4;  /* bytesPerSample */

    /* --- the wave atom --- */
    WR32(nw + p, (unsigned long)waveSize);     p += 4;
    WRTYPE(nw + p, "wave");                    p += 4;

    WR32(nw + p, 12);                          p += 4;
    WRTYPE(nw + p, "frma");                    p += 4;
    WRTYPE(nw + p, "mp4a");                    p += 4;

    WR32(nw + p, 12);                          p += 4;
    WRTYPE(nw + p, "mp4a");                    p += 4;
    WR32(nw + p, 0);                           p += 4;

    BlockMoveData(esds, nw + p, (Size)esdsSize);  p += esdsSize;

    WR32(nw + p, 8);                           p += 4;  /* terminator    */
    WR32(nw + p, 0);                           p += 4;

    if (p != newSize)
    {
        printf("  Audio: internal size mismatch (%ld built, %ld expected).\n"
               "         Refusing to write a file I do not trust.\n",
               p, newSize);
        DisposePtr((Ptr)nw);
        return NULL;
    }

    printf("  Audio: mp4a entry %ld -> %ld bytes (delta %+ld)\n",
           oldSize, newSize, newSize - oldSize);
    printf("         version 0 -> 1, compressionID 0x0000 -> 0xFFFE,\n");
    printf("         samplesPerPacket = %d, esds (%ld bytes) wrapped in\n"
           "         a wave atom.\n", kAACSamplesPerPacket, esdsSize);
    printf("         %d channel(s), %d-bit, rate 0x%08lX (16.16 fixed).\n",
           (int)channels, (int)sampleSize, RD32(rate));

    *newSizeOut = newSize;
    return nw;
}


/* ==================================================================
   Shift every chunk offset at or past the splice point
   ================================================================== */

static void ShiftChunkOffsets(unsigned char *buf, long start, long end,
                              unsigned long splice, long delta,
                              long *shifted, long *total)
{
    long off = start;

    while (off + 8 <= end)
    {
        unsigned long  size = RD32(buf + off);
        unsigned char *typ  = buf + off + 4;
        long           hdr  = 8;

        if (size == 1)
        {
            if (off + 16 > end) return;
            if (RD32(buf + off + 8) != 0) return;
            size = RD32(buf + off + 12);
            hdr  = 16;
        }
        else if (size == 0)
        {
            size = (unsigned long)(end - off);
        }
        if (size < (unsigned long)hdr || off + (long)size > end) return;

        if (ISTYPE(typ, "stco") || ISTYPE(typ, "co64"))
        {
            int   wide = ISTYPE(typ, "co64");
            long  step = wide ? 8 : 4;
            unsigned long n = RD32(buf + off + hdr + 4);
            unsigned long i;
            long  base = off + hdr + 8;

            for (i = 0; i < n; i++)
            {
                long q = base + (long)i * step;
                if (q + step > off + (long)size) break;

                (*total)++;

                if (wide)
                {
                    /* Manual carry, deliberately avoiding a 64-bit type.
                       CodeWarrior 8 PPC miscompiles 64-bit
                       shift-by-constant -- see the header comment. */
                    unsigned long hi = RD32(buf + q);
                    unsigned long lo = RD32(buf + q + 4);
                    if (hi != 0 || lo >= splice)
                    {
                        unsigned long nlo = lo + (unsigned long)delta;
                        if (nlo < lo) hi++;          /* carried */
                        WR32(buf + q,     hi);
                        WR32(buf + q + 4, nlo);
                        (*shifted)++;
                    }
                }
                else
                {
                    unsigned long v = RD32(buf + q);
                    if (v >= splice)
                    {
                        WR32(buf + q, v + (unsigned long)delta);
                        (*shifted)++;
                    }
                }
            }
        }

        if (IsContainer(typ))
            ShiftChunkOffsets(buf, off + hdr, off + (long)size,
                              splice, delta, shifted, total);

        off += (long)size;
    }
}


/* ==================================================================
   Copy n bytes from one open fork to another
   ================================================================== */

static OSErr CopyBytes(short src, short dst, long n, Ptr buf)
{
    OSErr err = noErr;

    while (n > 0)
    {
        long want = (n > kCopyBufSize) ? kCopyBufSize : n;
        long got  = want;

        err = FSRead(src, &got, buf);
        if (err != noErr && err != eofErr) return err;
        if (got <= 0) return eofErr;

        err = FSWrite(dst, &got, buf);
        if (err != noErr) return err;

        n -= got;
    }
    return noErr;
}


/* ==================================================================
   The four-byte patch, applied to a header already in memory
   ================================================================== */

static void ApplyBrandPatch(unsigned char *head)
{
#if kPatchMode == 1
    if (ISTYPE(head + 8, kQTBrand))
    {
        printf("  Brand is already 'qt  ' -- no byte patch needed.\n");
    }
    else
    {
        printf("  Brand '");
        PrintFourCC(head + 8);
        printf("' -> 'qt  '  (this is what forces the MOV parser, and so\n");
        printf("  what saves the avcC box our decoder needs).\n");
        WRTYPE(head + 8, kQTBrand);
    }
#else
    printf("  ftyp -> 'free'. The file will begin free / moov, like an\n");
    printf("  ordinary QuickTime movie, with no ftyp box at all.\n");
    WRTYPE(head + 4, kFreeAtom);
#endif
}


/* ==================================================================
   The in-place four-byte-only path (kFixAudio == 0)
   ================================================================== */

static OSErr PatchInPlace(const FSSpec *spec)
{
    short   refNum = 0;
    OSErr   err;
    long    count;
    unsigned char head[16];
    FInfo   fi;

    err = FSpOpenDF(spec, fsRdWrPerm, &refNum);
    if (err != noErr)
    {
        printf("  Could not open the file for writing, err = %d\n", (int)err);
        if (err == opWrErr || err == permErr)
            printf("  (It may be locked, or open in another program.)\n");
        return err;
    }

    err = SetFPos(refNum, fsFromStart, 0);
    if (err == noErr)
    {
        count = 16;
        err = FSRead(refNum, &count, (Ptr)head);
        if (err == noErr && count < 16) err = eofErr;
    }
    if (err != noErr)
    {
        printf("  Could not read the file header, err = %d\n", (int)err);
        FSClose(refNum);
        return err;
    }

    if (!ISTYPE(head + 4, "ftyp"))
    {
        printf("  First atom is '");
        PrintFourCC(head + 4);
        printf("', not 'ftyp'. Nothing to patch.\n");
        printf("  (A normal .mov starts 'moov' or 'mdat' and already works.)\n");
        FSClose(refNum);
        return noErr;
    }

    ApplyBrandPatch(head);

    err = SetFPos(refNum, fsFromStart, 0);
    if (err == noErr)
    {
        count = 16;
        err = FSWrite(refNum, &count, (Ptr)head);
    }
    FSClose(refNum);
    FlushVol(NULL, spec->vRefNum);

    if (err != noErr)
    {
        printf("  Write failed, err = %d\n", (int)err);
        return err;
    }

    if (FSpGetFInfo(spec, &fi) == noErr)
    {
        fi.fdType    = kMovieType;
        fi.fdCreator = kMovieCreator;
        (void)FSpSetFInfo(spec, &fi);
    }

    if (PStrEndsWith(spec->name, ".mp4") || PStrEndsWith(spec->name, ".m4v"))
    {
        Str255 newName;
        int    len = spec->name[0];
        BlockMoveData(spec->name, newName, (Size)(len + 1));
        newName[len - 3] = '.';
        newName[len - 2] = 'm';
        newName[len - 1] = 'o';
        newName[len - 0] = 'v';
        (void)FSpRename(spec, newName);
    }

    printf("\n  *** Done (four-byte patch only, no audio fix). ***\n");
    return noErr;
}


/* ==================================================================
   The full path: read, transform, write a new file
   ================================================================== */

static OSErr RewriteWithAudioFix(const FSSpec *src, const FSSpec *dst)
{
    short           inRef = 0, outRef = 0;
    OSErr           err;
    long            count, eof;
    long            pos;
    unsigned char   head[16];
    long            moovOff = -1, moovSize = 0;
    unsigned char  *moov = NULL;
    unsigned char  *newMoov = NULL;
    unsigned char  *newEntry = NULL;
    long            newEntrySize = 0, delta = 0;
    long            newMoovSize = 0;
    MoovScan        scan;
    long            stack[kMaxDepth];
    Ptr             copyBuf = NULL;
    FInfo           fi;
    long            shifted = 0, total = 0;
    int             i;

    /* ---------------- open the source ---------------- */
    err = FSpOpenDF(src, fsRdPerm, &inRef);
    if (err != noErr)
    {
        printf("  Could not open the file, err = %d\n", (int)err);
        return err;
    }
    if (GetEOF(inRef, &eof) != noErr) eof = 0;

    count = 16;
    if (SetFPos(inRef, fsFromStart, 0) != noErr
        || FSRead(inRef, &count, (Ptr)head) != noErr || count < 16)
    {
        printf("  Could not read the file header.\n");
        FSClose(inRef);
        return ioErr;
    }

    if (!ISTYPE(head + 4, "ftyp"))
    {
        printf("  First atom is '");
        PrintFourCC(head + 4);
        printf("', not 'ftyp'.\n");
        printf("  This does not look like an MP4. A normal .mov starts\n");
        printf("  'moov' or 'mdat' and needs nothing from this program.\n");
        FSClose(inRef);
        return noErr;
    }
    printf("  ftyp found, %ld bytes total.\n", eof);

    /* ---------------- find moov by walking the top level ------------- */
    pos = 0;
    while (pos + 8 <= eof)
    {
        unsigned char  bh[16];
        unsigned long  bsize;
        long           bhdr = 8;

        count = 8;
        if (SetFPos(inRef, fsFromStart, pos) != noErr) break;
        if (FSRead(inRef, &count, (Ptr)bh) != noErr || count < 8) break;

        bsize = RD32(bh);
        if (bsize == 1)
        {
            count = 8;
            if (FSRead(inRef, &count, (Ptr)(bh + 8)) != noErr || count < 8) break;
            if (RD32(bh + 8) != 0) break;       /* a >4 GB box: give up  */
            bsize = RD32(bh + 12);
            bhdr  = 16;
        }
        else if (bsize == 0)
        {
            bsize = (unsigned long)(eof - pos);
        }
        if (bsize < (unsigned long)bhdr) break;

        printf("    %c%c%c%c at %ld, %lu bytes\n",
               bh[4], bh[5], bh[6], bh[7], pos, bsize);

        if (ISTYPE(bh + 4, "moov"))
        {
            moovOff  = pos;
            moovSize = (long)bsize;
        }

        pos += (long)bsize;
    }

    if (moovOff < 0)
    {
        printf("  No 'moov' box found. This file is not something I can\n");
        printf("  safely rewrite; try kFixAudio 0 for the brand patch only.\n");
        FSClose(inRef);
        return noErr;
    }
    if (moovSize > kMaxMoovSize)
    {
        printf("  The moov box is %ld bytes, larger than the %ld-byte limit\n"
               "  this program will hold in memory. Not rewriting.\n",
               moovSize, kMaxMoovSize);
        FSClose(inRef);
        return noErr;
    }

    /* ---------------- pull moov into memory ---------------- */
    moov = (unsigned char *)NewPtr(moovSize);
    if (moov == NULL)
    {
        printf("  Not enough memory for a %ld-byte moov. Try giving the\n"
               "  application a bigger partition in Get Info.\n", moovSize);
        FSClose(inRef);
        return memFullErr;
    }
    count = moovSize;
    if (SetFPos(inRef, fsFromStart, moovOff) != noErr
        || FSRead(inRef, &count, (Ptr)moov) != noErr || count < moovSize)
    {
        printf("  Could not read the moov box.\n");
        DisposePtr((Ptr)moov);
        FSClose(inRef);
        return ioErr;
    }

    /* ---------------- find the audio sample entry ---------------- */
    scan.buf        = moov;
    scan.size       = moovSize;
    scan.entryOff   = -1;
    scan.entrySize  = 0;
    scan.entryCount = 0;
    scan.ancCount   = 0;
    stack[0] = 0;
    ScanBoxes(&scan, 8, moovSize, stack, 1);   /* children of moov */

    if (scan.entryCount > 1)
    {
        printf("  Found %d mp4a sample entries. This program handles one.\n"
               "  Audio left untouched; doing the brand patch only.\n",
               scan.entryCount);
        scan.entryOff = -1;
    }

    if (scan.entryOff >= 0)
    {
        newEntry = BuildQTAudioEntry(moov + scan.entryOff, scan.entrySize,
                                     &newEntrySize);
        if (newEntry != NULL)
            delta = newEntrySize - scan.entrySize;
    }
    else if (scan.entryCount == 0)
    {
        printf("  Audio: no mp4a sample entry in this movie (silent, or\n"
               "         some other audio format). Brand patch only.\n");
    }

    /* ---------------- build the new moov ---------------- */
    if (delta != 0)
    {
        long  tailOff  = scan.entryOff + scan.entrySize;
        long  tailSize = moovSize - tailOff;

        newMoovSize = moovSize + delta;
        newMoov = (unsigned char *)NewPtr(newMoovSize);
        if (newMoov == NULL)
        {
            printf("  Not enough memory to build the new moov.\n");
            DisposePtr((Ptr)newEntry);
            DisposePtr((Ptr)moov);
            FSClose(inRef);
            return memFullErr;
        }

        BlockMoveData(moov, newMoov, (Size)scan.entryOff);
        BlockMoveData(newEntry, newMoov + scan.entryOff, (Size)newEntrySize);
        BlockMoveData(moov + tailOff, newMoov + scan.entryOff + newEntrySize,
                      (Size)tailSize);

        /* Grow every box that encloses the entry. Their offsets are all
           BEFORE the splice, so they are unchanged in the new buffer. */
        for (i = 0; i < scan.ancCount; i++)
        {
            long a = scan.anc[i];
            unsigned long sz = RD32(newMoov + a);
            if (sz == 1) continue;          /* 64-bit: refused earlier  */
            WR32(newMoov + a, sz + (unsigned long)delta);
            printf("         grew %c%c%c%c  %lu -> %lu\n",
                   newMoov[a+4], newMoov[a+5], newMoov[a+6], newMoov[a+7],
                   sz, sz + (unsigned long)delta);
        }

        /* Shift every chunk offset at or past the splice. */
        ShiftChunkOffsets(newMoov, 8, newMoovSize,
                          (unsigned long)(moovOff + scan.entryOff),
                          delta, &shifted, &total);
        printf("         shifted %ld of %ld chunk offsets by %+ld\n",
               shifted, total, delta);

        if (shifted == 0 && total > 0 && moovOff < eof / 2)
        {
            printf("  !! moov comes before the media but NO chunk offset\n");
            printf("     needed shifting. That is suspicious. Check the\n");
            printf("     result plays before you delete the original.\n");
        }
    }
    else
    {
        newMoov     = moov;
        newMoovSize = moovSize;
    }

    ApplyBrandPatch(head);

    /* ---------------- write the new file ---------------- */
    (void)FSpDelete(dst);                 /* ignore "not found"         */
    err = FSpCreate(dst, kMovieCreator, kMovieType, smSystemScript);
    if (err != noErr && err != dupFNErr)
    {
        printf("  Could not create the output file, err = %d\n", (int)err);
        goto bail;
    }
    err = FSpOpenDF(dst, fsWrPerm, &outRef);
    if (err != noErr)
    {
        printf("  Could not open the output file, err = %d\n", (int)err);
        goto bail;
    }
    (void)SetEOF(outRef, 0);

    copyBuf = NewPtr(kCopyBufSize);
    if (copyBuf == NULL)
    {
        printf("  Not enough memory for the copy buffer.\n");
        err = memFullErr;
        goto bail;
    }

    /* everything before moov, with the patched 16-byte header first */
    count = 16;
    err = FSWrite(outRef, &count, (Ptr)head);
    if (err != noErr) goto bail;

    if (moovOff > 16)
    {
        if (SetFPos(inRef, fsFromStart, 16) != noErr) { err = ioErr; goto bail; }
        err = CopyBytes(inRef, outRef, moovOff - 16, copyBuf);
        if (err != noErr) goto bail;
    }

    /* the rebuilt moov */
    count = newMoovSize;
    err = FSWrite(outRef, &count, (Ptr)newMoov);
    if (err != noErr) goto bail;

    /* and everything after it, verbatim */
    if (moovOff + moovSize < eof)
    {
        if (SetFPos(inRef, fsFromStart, moovOff + moovSize) != noErr)
        { err = ioErr; goto bail; }
        err = CopyBytes(inRef, outRef, eof - (moovOff + moovSize), copyBuf);
        if (err != noErr) goto bail;
    }

    FSClose(outRef); outRef = 0;
    FlushVol(NULL, dst->vRefNum);

    if (FSpGetFInfo(dst, &fi) == noErr)
    {
        fi.fdType    = kMovieType;
        fi.fdCreator = kMovieCreator;
        (void)FSpSetFInfo(dst, &fi);
    }

    printf("\n  Wrote \"");
    PrintPStr(dst->name);
    printf("\" -- %ld bytes (%+ld).\n", eof + delta, delta);
    printf("  The original is untouched.\n");
    printf("\n  *** Done. Open it in QuickTime Player. ***\n");
    err = noErr;

bail:
    if (copyBuf)              DisposePtr(copyBuf);
    if (outRef)               FSClose(outRef);
    if (newEntry)             DisposePtr((Ptr)newEntry);
    if (newMoov && newMoov != moov) DisposePtr((Ptr)newMoov);
    if (moov)                 DisposePtr((Ptr)moov);
    FSClose(inRef);
    if (err != noErr)
        printf("  Failed, err = %d. The original is untouched.\n", (int)err);
    return err;
}


/* ==================================================================
   Suggest an output name: foo.mp4 -> foo.mov, foo.mov -> foo-qt.mov
   ================================================================== */

static void SuggestName(ConstStr255Param in, Str255 out)
{
    int len = in[0];

    BlockMoveData(in, out, (Size)(len + 1));

    if (PStrEndsWith(in, ".mp4") || PStrEndsWith(in, ".m4v"))
    {
        out[len - 3] = '.';
        out[len - 2] = 'm';
        out[len - 1] = 'o';
        out[len - 0] = 'v';
        return;
    }

    /* No recognised extension: append "-qt.mov", trimming if we would
       overflow the 31-character HFS limit. */
    if (len > 31 - 7) len = 31 - 7;
    out[len + 1] = '-';  out[len + 2] = 'q';  out[len + 3] = 't';
    out[len + 4] = '.';  out[len + 5] = 'm';  out[len + 6] = 'o';
    out[len + 7] = 'v';
    out[0] = (unsigned char)(len + 7);
}


int main(void)
{
    StandardFileReply reply;
    SFTypeList        types;

#ifdef __MWERKS__
    /* SIOUX -- the console window the C Console stationery gives us --
       defaults to keeping the window open after main() returns and then,
       on File -> Quit, asking whether to save a transcript called
       "mp4fixer.out". Measured on the first Mac-built run: "q + return"
       printed Bye and then the program just sat there. These two lines
       make "q + return" (or Cancel in the first dialog) simply quit.

       They must run BEFORE the first printf, because the first output
       is what creates the window and reads these settings. */
    SIOUXSettings.asktosaveonclose = false;
    SIOUXSettings.autocloseonquit  = true;
#endif

    printf("================================================================\n");
    printf("MP4Fix -- make a standard MP4 play, with sound, on Mac OS 9\n");
    printf("================================================================\n");
    printf("\n");
#if kFixAudio
    printf("Two fixes in one pass:\n");
    printf("\n");
    printf("  1. Routes the file to QuickTime's MOV parser, which copies\n");
    printf("     the avcC box through verbatim instead of dropping it.\n");
    printf("     That is the difference between error -8971 and a picture.\n");
    printf("\n");
    printf("  2. Rewrites the AAC sample description from the ISO form\n");
    printf("     (version 0, bare esds) to the QuickTime form (version 1,\n");
    printf("     compressionID -2, samplesPerPacket 1024, esds inside a\n");
    printf("     wave atom). That is the difference between silence and\n");
    printf("     sound, and it is why the brand patch alone played mute.\n");
    printf("\n");
    printf("Fix 2 grows the file, so every chunk offset in stco is\n");
    printf("corrected to match. A NEW file is written; your original is\n");
    printf("never modified.\n");
#else
    printf("Four-byte mode: writes 'qt  ' over the ftyp major brand, sets\n");
    printf("the type to MooV/TVOD and renames .mp4 to .mov. Edits IN\n");
    printf("PLACE. Picture only -- audio will be silent. Set kFixAudio to\n");
    printf("1 and rebuild for sound.\n");
#endif
    printf("\n");

    for (;;)
    {
        int c;

        printf("----------------------------------------------------------\n");
        printf("Choose a movie file (Cancel in the dialog to quit)...\n");

        /* -1 shows every type on purpose: downloads often arrive with a
           missing or useless file type, and those are exactly the files
           this program exists to fix. */
        StandardGetFile(NULL, -1, types, &reply);
        if (!reply.sfGood)
        {
            printf("\nCancelled. Bye.\n");
            break;
        }

        printf("\nFile: \"");
        PrintPStr(reply.sfFile.name);
        printf("\"\n");

#if kFixAudio
        {
            StandardFileReply saveReply;
            Str255            suggested;

            SuggestName(reply.sfFile.name, suggested);
            printf("Where should the fixed copy go?\n");
            StandardPutFile("\pSave the fixed movie as:", suggested, &saveReply);

            if (!saveReply.sfGood)
                printf("  Cancelled -- nothing written.\n");
            else
                (void)RewriteWithAudioFix(&reply.sfFile, &saveReply.sfFile);
        }
#else
        (void)PatchInPlace(&reply.sfFile);
#endif

        printf("\nFix another? (return = yes, q then return = quit) ");
        fflush(stdout);
        c = getchar();
        while (c != '\n' && c != EOF) { if (c == 'q' || c == 'Q') break; c = getchar(); }
        if (c == 'q' || c == 'Q')
        {
            printf("\nBye.\n");
            break;
        }
        printf("\n");
    }

    return 0;
}
