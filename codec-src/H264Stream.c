/*
    H264Stream.c  --  "H264 Stream": play an H.264 + AAC .mp4 straight
                      off the web in QuickTime Player 6 on Mac OS 9

    ==================================================================
    THE IDEA IN ONE PARAGRAPH
    ==================================================================
    QuickTime 6 already knows how to fetch movie data over plain HTTP.
    What it cannot do is understand a modern .mp4's 'moov' box well
    enough to play it (the MPEG-4 importer drops the 'avcC' that our
    decoder needs, and the MOV parser does not like an ISO audio entry
    -- see MP4Fix.c for the long story). So this program downloads ONLY
    the 'moov' box of the remote file -- usually a few hundred KB, not
    the whole movie -- rewrites it the way mp4fixer does, and saves it
    as a tiny "reference movie": a QuickTime file that holds the sample
    tables but says, in its data reference, "the actual bytes live at
    http://...". QuickTime Player then opens that little file and pulls
    the video and audio from the web server by itself, as it plays.

    ==================================================================
    WHAT THE REFERENCE MOVIE CONTAINS (proven on the bench first)
    ==================================================================
    Exactly what the host tool mkref.py writes, byte for byte:

      * the remote file's 'moov', with every box header rewritten in
        the plain 32-bit form;
      * the AAC sample description converted from the ISO version-0
        layout to QuickTime's version-1 layout with a 'wave' atom
        (identical to mp4fixer / qtify.py);
      * every 'dref' replaced by ONE 'url ' entry: version/flags 0
        followed by the C string of the http URL. Because the flags
        are 0 (not 1 = "self-contained"), QuickTime looks for the media
        at that URL rather than in this file;
      * 'stco' / 'co64' chunk offsets left EXACTLY as in the remote
        file. They are file offsets into the REMOTE file, and that file
        is not changing -- so they are already right. (mkref.py arrives
        at the same numbers by shifting and un-shifting.)
      * 'iods' dropped (QuickTime 6's MOV parser has no use for it and
        it is an MPEG-4-systems object, not a QuickTime one).

    Saved with file type 'MooV' and creator 'TVOD' so the Finder knows
    it is a QuickTime movie belonging to QuickTime Player.

    ==================================================================
    HOW THE MOOV IS FOUND WITHOUT DOWNLOADING THE WHOLE FILE
    ==================================================================
    An MP4 is a list of top-level boxes. We read them one at a time
    from the HTTP body:

        ftyp (tiny)   -> read it and throw it away
        moov          -> read it into memory, then hang up. Done.
        mdat (huge)   -> do NOT read through it. Hang up and ask the
                         server again, with "Range: bytes=N-", for the
                         part of the file that starts just after it.
        free / skip / anything small -> read and discard

    "Fast start" files (moov first, like the Internet Archive's
    *_512kb.mp4 derivatives) are cheap: one request, a few hundred KB.
    "mdat first" files cost one extra request -- as long as the server
    honours Range, which essentially every static file server does. If
    it answers 200 instead of 206 we say so plainly and stop rather
    than downloading the whole movie through a 2002 TCP stack.

    ==================================================================
    OPEN TRANSPORT NOTES (two traps already known in this project)
    ==================================================================
      1. OTInitDNSAddress RETURNS the length to put in TCall.addr.len.
         Do not compute it from sizeof(DNSAddress).
      2. OTRcv does not "return 0 at end of stream". It returns a
         negative kOTLookErr, and you must call OTLook and then
         OTRcvOrderlyDisconnect (T_ORDREL, the polite close an
         HTTP/1.0 server does after the body) or OTRcvDisconnect
         (T_DISCONNECT, a reset) to acknowledge it.

    We connect in synchronous BLOCKING mode (simple: OTConnect returns
    when the TCP handshake is done or has failed) and then receive in
    synchronous NON-blocking mode, polling. Between polls we call
    WaitNextEvent with an empty event mask: that receives nothing, but
    it gives other applications (and the Finder) time, so the Mac does
    not appear frozen while we wait for the server, and it lets us
    enforce our own 60-second "the server went quiet" timeout.

    ==================================================================
    HOW TO USE IT
    ==================================================================
      * Double-click "H264 Stream". Type or paste an http:// URL of an
        .mp4 (H.264 video, AAC audio) and press Return.
      * OR put the URL in a text file named  H264 Stream URL  in the
        same folder as the application. If that file exists the URL is
        read from it and no question is asked -- handy for testing, or
        for keeping a favourite link.
      * The reference movie appears on the Desktop as <name>.mov and
        QuickTime Player is asked (via the Finder) to open it. Press
        Play. Because the data streams in as it plays, QuickTime's
        progress bar fills gradually, exactly like a web movie.
      * Everything printed to the console is also written to the file
        H264 Stream Log  next to the application.

    Only plain http:// works. QuickTime 6 itself cannot fetch https,
    so an https URL is useless in the reference movie even if we could
    fetch the moov ourselves. If a server redirects http -> https we
    try the same address over http once more and say so.

    ==================================================================
    BUILDING (CodeWarrior Pro 8.3)
    ==================================================================
    Version 1.1 has a small dialog instead of the SIOUX console. The
    dialog and its item list are built in code, so no resource file is
    needed; the SIOUX library may stay in the project unused.
    Same as mp4fixer: a "C Console Classic" (Mac OS PPC, Standard
    Console, NOT Carbon) project. Remove HelloWorld.c, add this file,
    and add OpenTransportLib and OpenTptInternetLib (stub libraries),
    plus OpenTransportAppPPC.o and OpenTptInetPPC.o (the static glue
    that implements OTCreateConfiguration, OTInitDNSAddress etc.).
    No prefix file. C89 throughout: declarations at the top of blocks.
    No 64-bit arithmetic anywhere (CodeWarrior 8 PPC miscompiles some
    64-bit shifts; see MP4Fix.c) -- 64-bit box sizes are read as two
    32-bit halves.

    The host regression test (tools/h264stream_hosttest.c) defines
    H264STREAM_HOSTTEST and #includes this very file, replacing only
    the "platform" section (network, memory, files) with fakes, so the
    box walking, HTTP parsing and moov rewriting compiled by
    CodeWarrior are the exact lines that were proven on the host.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "mc_core.h"      /* mc_avc_profile / mc_avc_verdict */

#ifndef H264STREAM_HOSTTEST
#include <Types.h>
#include <Errors.h>
#include <Memory.h>
#include <Files.h>
#include <Folders.h>
#include <Events.h>
#include <Processes.h>
#include <AppleEvents.h>
#include <Aliases.h>
#include <Script.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Controls.h>
#include <Scrap.h>
#include <ToolUtils.h>
#include <Sound.h>
#endif


/* ==================================================================
   Limits and names
   ================================================================== */

#define kMaxURL          1024
#define kMaxHost         256
#define kMaxRedirects    8

/* The moov holds the sample tables: for a feature-length movie at
   30 fps that is a few MB. 8 MB is generous for anything a G3 can
   actually play. */
#define kMaxMoov         (8L * 1024L * 1024L)

/* A non-moov box smaller than this is simply read and discarded;
   anything bigger (in practice: mdat) is jumped over with a Range
   request instead. */
#define kSkipByReading   (512L * 1024L)

#define kHeaderMax       8192
#define kRxBufSize       16384

#define kAACSamplesPerPacket   1024
#define kVariableCompression   0xFFFEU

/* An app that #includes this file (MediaCenter.c, with
   H264STREAM_LIBRARY defined) may set its own names first. */
#ifndef kUserAgent
#define kUserAgent  "H264Stream/1.2 (Macintosh; PPC Mac OS 9; QuickTime 6)"
#endif
#define kURLFileName "H264 Stream URL"
#ifndef kLogFileName
#define kLogFileName "H264 Stream Log"
#endif


/* ==================================================================
   Platform interface. Implemented at the bottom of this file for the
   Mac, and by the host test harness for gcc.
   ================================================================== */

static void *MemAlloc(long n);
static void  MemFree(void *p);
static int   NetOpen(const char *host, unsigned short port); /* 0 = ok */
static long  NetSend(const char *buf, long n);   /* bytes sent, <0 err */
static long  NetRecv(char *buf, long max);       /* >0 bytes, 0 EOF, <0 err */
static void  NetClose(void);
static int   SaveAndOpenMovie(const char *name,
                              const unsigned char *data, long n);

/* Progress output: written to the log file, and the latest message
   is shown in the dialog's status line. */
static FILE *gLog = NULL;
static char  gLastMsg[256];

#ifndef H264STREAM_HOSTTEST
static void UIStatus(const char *line);
#endif

static void Say(const char *fmt, ...)
{
    static char buf[4096];
    va_list ap;
    const char *p;
    long n = 0;

    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    if (gLog != NULL)
    {
        fputs(buf, gLog);
        fflush(gLog);
    }
#ifdef H264STREAM_HOSTTEST
    fputs(buf, stdout);
    fflush(stdout);
#endif
    /* one line for the status item: newlines become spaces, runs of
       blanks collapse, leading/trailing blanks go */
    for (p = buf; *p && n < 250; p++)
    {
        char c = *p;
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c == ' ' && (n == 0 || gLastMsg[n - 1] == ' ')) continue;
        gLastMsg[n++] = c;
    }
    while (n > 0 && gLastMsg[n - 1] == ' ') n--;
    gLastMsg[n] = 0;
#ifndef H264STREAM_HOSTTEST
    if (n > 0) UIStatus(gLastMsg);
#endif
}


/* ==================================================================
   Byte helpers -- big-endian, assembled by hand, 32-bit only
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

static int ISTYPE(const unsigned char *p, const char *fourcc)
{
    return (p[0] == (unsigned char)fourcc[0]
         && p[1] == (unsigned char)fourcc[1]
         && p[2] == (unsigned char)fourcc[2]
         && p[3] == (unsigned char)fourcc[3]);
}

static void FourCCText(const unsigned char *b, char *out)
{
    int i;
    for (i = 0; i < 4; i++)
        out[i] = (char)((b[i] >= 32 && b[i] < 127) ? b[i] : '.');
    out[4] = 0;
}

static int LowerC(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int StrNCaseEq(const char *a, const char *b, long n)
{
    long i;
    for (i = 0; i < n; i++)
    {
        if (LowerC((unsigned char)a[i]) != LowerC((unsigned char)b[i]))
            return 0;
        if (a[i] == 0) return 1;
    }
    return 1;
}


/* ==================================================================
   PART 1: rewriting the moov into a reference movie

   One recursive function, run twice: the first pass with out == NULL
   only measures, the second writes into a block of exactly that size.
   This mirrors mkref.py's rebuild() exactly, plus qtify's audio-entry
   rewrite done in place of copying the stsd verbatim.
   ================================================================== */

/* The same container set as mkref.py. Deliberately NOT 'meta' or
   'wave': those are copied verbatim, as mkref does. */
static int RefIsContainer(const unsigned char *t)
{
    return ISTYPE(t, "moov") || ISTYPE(t, "trak") || ISTYPE(t, "mdia")
        || ISTYPE(t, "minf") || ISTYPE(t, "dinf") || ISTYPE(t, "stbl")
        || ISTYPE(t, "edts") || ISTYPE(t, "udta");
}

/* Read one box header at off (inside [off, end)). Returns 0 on success
   and fills *hdrLen / *boxLen; -1 if the box is malformed or bigger
   than 4 GB. A size of 0 means "to the end of the enclosing box". */
static int BoxAt(const unsigned char *b, long off, long end,
                 long *hdrLen, long *boxLen)
{
    unsigned long n = RD32(b + off);
    long h = 8;

    if (n == 1)
    {
        if (off + 16 > end) return -1;
        if (RD32(b + off + 8) != 0) return -1;     /* >4 GB: refuse */
        n = RD32(b + off + 12);
        h = 16;
    }
    else if (n == 0)
    {
        n = (unsigned long)(end - off);
    }
    if (n < (unsigned long)h || n > (unsigned long)(end - off)) return -1;
    *hdrLen = h;
    *boxLen = (long)n;
    return 0;
}

/* Emit helpers: write only if out is non-NULL, always advance. */
static void Put(unsigned char *out, long *at, const void *src, long n)
{
    if (out != NULL && n > 0) memcpy(out + *at, src, (size_t)n);
    *at += n;
}

static void Put32(unsigned char *out, long *at, unsigned long v)
{
    if (out != NULL) WR32(out + *at, v);
    *at += 4;
}

static void Put16(unsigned char *out, long *at, unsigned short v)
{
    if (out != NULL) WR16(out + *at, v);
    *at += 2;
}

static void PutType(unsigned char *out, long *at, const char *t)
{
    Put(out, at, t, 4);
}

/* The QuickTime version-1 sound description, field for field the same
   as MP4Fix.c's BuildQTAudioEntry and qtify.py's build_qt_audio_entry.

   old/oldLen is the whole ISO 'mp4a' sample entry. Returns the number
   of bytes emitted, or 0 if this entry should be copied unchanged
   (already version 1, too short, or no esds inside). */
static long EmitQTAudioEntry(const unsigned char *old, long oldLen,
                             unsigned char *out, long at0)
{
    const unsigned char *esds = NULL;
    long esdsLen = 0;
    long off, waveLen, newLen, at;

    if (oldLen < 36) return 0;
    if (RD16(old + 16) != 0) return 0;          /* already version 1+ */

    /* The bare esds among the extensions after the 36-byte record.
       Any other extension (typically 'btrt') is dropped on purpose --
       see MP4Fix.c for why. The LAST esds wins, as in qtify.py. */
    off = 36;
    while (off + 8 <= oldLen)
    {
        unsigned long sz = RD32(old + off);
        if (sz < 8 || off + (long)sz > oldLen) break;
        if (ISTYPE(old + off + 4, "esds"))
        {
            esds = old + off;
            esdsLen = (long)sz;
        }
        off += (long)sz;
    }
    if (esds == NULL) return 0;

    /* wave = header + frma(12) + mp4a(12) + esds + terminator(8) */
    waveLen = 8 + 12 + 12 + esdsLen + 8;
    newLen  = 8 + 6 + 2 + 2 + 2 + 4 + 2 + 2 + 2 + 2 + 4 + 16 + waveLen;

    at = at0;
    Put32(out, &at, (unsigned long)newLen);
    PutType(out, &at, "mp4a");
    Put32(out, &at, 0); Put16(out, &at, 0);       /* reserved[6]        */
    Put(out, &at, old + 14, 2);                   /* data ref index     */
    Put16(out, &at, 1);                           /* VERSION 1          */
    Put16(out, &at, 0);                           /* revision           */
    Put32(out, &at, 0);                           /* vendor             */
    Put(out, &at, old + 24, 2);                   /* channels           */
    Put(out, &at, old + 26, 2);                   /* sample size        */
    Put16(out, &at, (unsigned short)kVariableCompression); /* -2        */
    Put16(out, &at, 0);                           /* packet size        */
    Put(out, &at, old + 32, 4);                   /* rate, 16.16 fixed  */
    Put32(out, &at, kAACSamplesPerPacket);        /* samplesPerPacket   */
    Put32(out, &at, 0);                           /* bytesPerPacket     */
    Put32(out, &at, 0);                           /* bytesPerFrame      */
    Put32(out, &at, 2);                           /* bytesPerSample     */
    Put32(out, &at, (unsigned long)waveLen);
    PutType(out, &at, "wave");
    Put32(out, &at, 12); PutType(out, &at, "frma"); PutType(out, &at, "mp4a");
    Put32(out, &at, 12); PutType(out, &at, "mp4a"); Put32(out, &at, 0);
    Put(out, &at, esds, esdsLen);
    Put32(out, &at, 8); Put32(out, &at, 0);       /* terminator atom    */

    return at - at0;
}

/* Counters for the progress report (filled on the measuring pass). */
static long gDrefCount, gAudioCount, gIodsCount;

/* Rebuild the boxes in in[start, end) into out at *at.
   Returns 0, or -1 on a malformed box. */
static int Rebuild(const unsigned char *in, long start, long end,
                   unsigned char *out, long *at,
                   const char *url, long urlLen)
{
    long off = start;

    while (off + 8 <= end)
    {
        long h, n, bodyStart, sizeAt;
        const unsigned char *t;

        if (BoxAt(in, off, end, &h, &n) != 0) return -1;
        t = in + off + 4;
        bodyStart = off + h;

        if (ISTYPE(t, "iods"))
        {
            if (out == NULL) gIodsCount++;
            off += n;
            continue;
        }

        sizeAt = *at;
        Put32(out, at, 0);                 /* size, patched below */
        Put(out, at, t, 4);

        if (RefIsContainer(t))
        {
            if (Rebuild(in, bodyStart, off + n, out, at, url, urlLen) != 0)
                return -1;
        }
        else if (ISTYPE(t, "dref"))
        {
            /* version/flags 0, one entry: 'url ' with flags 0 (= data is
               NOT in this file) and the URL as a C string. */
            Put32(out, at, 0);
            Put32(out, at, 1);
            Put32(out, at, (unsigned long)(12 + urlLen + 1));
            PutType(out, at, "url ");
            Put32(out, at, 0);
            Put(out, at, url, urlLen);
            Put(out, at, "", 1);
            if (out == NULL) gDrefCount++;
        }
        else if (ISTYPE(t, "stsd") && n - h >= 8)
        {
            /* version/flags + entry count, then the sample entries.
               An ISO 'mp4a' entry is replaced by the QuickTime form;
               every other entry (avc1 above all, with its avcC) is
               copied byte for byte. */
            long e = bodyStart + 8;
            Put(out, at, in + bodyStart, 8);
            while (e + 8 <= off + n)
            {
                long eh, en, w = 0;
                if (BoxAt(in, e, off + n, &eh, &en) != 0) break;
                if (ISTYPE(in + e + 4, "mp4a"))
                {
                    w = EmitQTAudioEntry(in + e, en, out, *at);
                    if (w > 0 && out == NULL) gAudioCount++;
                }
                if (w > 0) *at += w;
                else       Put(out, at, in + e, en);
                e += en;
            }
            /* any trailing bytes the entry walk could not parse */
            Put(out, at, in + e, off + n - e);
        }
        else
        {
            /* Everything else verbatim -- including stco/co64, whose
               offsets point into the REMOTE file and stay correct. */
            Put(out, at, in + bodyStart, n - h);
        }

        if (out != NULL) WR32(out + sizeAt, (unsigned long)(*at - sizeAt));
        off += n;
    }
    return 0;
}

/* moov (raw, header included) -> complete reference movie file.
   Returns the new block (caller frees with MemFree) or NULL. */
static unsigned char *BuildReferenceMovie(const unsigned char *moov,
                                          long moovLen, const char *url,
                                          long *outLen)
{
    long h, n, len, at;
    long urlLen = (long)strlen(url);
    unsigned char *out;

    if (BoxAt(moov, 0, moovLen, &h, &n) != 0 || !ISTYPE(moov + 4, "moov"))
    {
        Say("  The downloaded box is not a well-formed 'moov'.\n");
        return NULL;
    }

    gDrefCount = gAudioCount = gIodsCount = 0;
    len = 8;
    if (Rebuild(moov, h, n, NULL, &len, url, urlLen) != 0)
    {
        Say("  The moov contains a malformed box; giving up.\n");
        return NULL;
    }

    out = (unsigned char *)MemAlloc(len);
    if (out == NULL)
    {
        Say("  Out of memory for the %ld-byte reference movie.\n", len);
        return NULL;
    }
    at = 8;
    (void)Rebuild(moov, h, n, out, &at, url, urlLen);
    WR32(out, (unsigned long)at);
    memcpy(out + 4, "moov", 4);

    if (at != len)
    {
        Say("  Internal size mismatch (%ld vs %ld); refusing to save.\n",
            at, len);
        MemFree(out);
        return NULL;
    }

    Say("  Rewrote moov: %ld -> %ld bytes.\n", moovLen, len);
    Say("    %ld data reference(s) now point at the URL\n", gDrefCount);
    Say("    %ld AAC sample description(s) converted to QuickTime form\n",
        gAudioCount);
    if (gIodsCount) Say("    %ld 'iods' box dropped\n", gIodsCount);
    if (gDrefCount == 0)
        Say("  WARNING: no 'dref' found -- QuickTime will not know where\n"
            "  the media is. This movie will probably not play.\n");

    *outLen = len;
    return out;
}


/* ==================================================================
   PART 2: URLs
   ================================================================== */

typedef struct
{
    char           host[kMaxHost];
    unsigned short port;
    char           path[kMaxURL];     /* always starts with '/' */
} URLParts;

/* Parse "http://host[:port][/path]". Returns 0 ok, -1 not http,
   -2 malformed. https is reported separately by the caller. */
static int ParseURL(const char *url, URLParts *u)
{
    const char *p, *hostEnd, *slash;
    long hl;

    if (!StrNCaseEq(url, "http://", 7)) return -1;
    p = url + 7;

    slash = strchr(p, '/');
    if (slash == NULL) slash = p + strlen(p);
    /* a '?' with no path: http://host?x -> treat '?' as path start */
    {
        const char *q = strchr(p, '?');
        if (q != NULL && q < slash) slash = q;
    }

    hostEnd = p;
    while (hostEnd < slash && *hostEnd != ':') hostEnd++;
    hl = (long)(hostEnd - p);
    if (hl <= 0 || hl >= kMaxHost) return -2;
    memcpy(u->host, p, (size_t)hl);
    u->host[hl] = 0;

    u->port = 80;
    if (hostEnd < slash)               /* ":port" */
    {
        long v = 0;
        const char *d = hostEnd + 1;
        if (d == slash) return -2;
        while (d < slash)
        {
            if (*d < '0' || *d > '9') return -2;
            v = v * 10 + (*d - '0');
            if (v > 65535) return -2;
            d++;
        }
        u->port = (unsigned short)v;
    }

    if (*slash == 0)
        strcpy(u->path, "/");
    else if (*slash == '?')
    {
        if (strlen(slash) + 2 > kMaxURL) return -2;
        u->path[0] = '/';
        strcpy(u->path + 1, slash);
    }
    else
    {
        if (strlen(slash) + 1 > kMaxURL) return -2;
        strcpy(u->path, slash);
    }
    return 0;
}

static void FormatURL(const URLParts *u, char *out)
{
    if (u->port == 80)
        sprintf(out, "http://%s%s", u->host, u->path);
    else
        sprintf(out, "http://%s:%u%s", u->host, (unsigned)u->port, u->path);
}

/* Resolve a Location header against the URL that produced it.
   Handles absolute http, https (downgraded, with a warning), scheme-
   relative //host/x, root-relative /x and plain relative x.
   Returns 0 ok, -1 on anything unusable. */
/* https redirects. Rewriting https -> http once is worth a try (some
   servers answer both), but archive.org's *.ca.archive.org storage
   hosts send the http request straight back to https, which used to
   loop until "Too many redirects". Now the second https redirect in
   one HttpGet is final. MediaCenter sets gHttpsIsFailure = 1 so the
   FIRST one is final and it can try its next mirror instead. */
static int gHttpsIsFailure = 0;   /* set by the caller */
static int gHttpsSeen = 0;        /* reset by HttpGet */

static int ResolveLocation(const URLParts *base, const char *loc, char *out)
{
    if (strlen(loc) + 16 > kMaxURL) return -1;

    if (StrNCaseEq(loc, "http://", 7))
    {
        strcpy(out, loc);
        return 0;
    }
    if (StrNCaseEq(loc, "https://", 8))
    {
        gHttpsSeen++;
        if (gHttpsIsFailure || gHttpsSeen > 1)
        {
            Say("  The server insists on https:\n  %.200s\n"
                "  This item needs HTTPS, which QuickTime 6 cannot fetch.\n",
                loc);
            return -1;
        }
        Say("  (The server redirects to https, which QuickTime 6 cannot\n"
            "   fetch. Trying the same address over plain http.)\n");
        sprintf(out, "http://%s", loc + 8);
        return 0;
    }
    if (loc[0] == '/' && loc[1] == '/')
    {
        sprintf(out, "http:%s", loc);
        return 0;
    }
    if (strstr(loc, "://") != NULL) return -1;     /* ftp:// etc. */

    {
        URLParts u = *base;
        if (loc[0] == '/')
        {
            strcpy(u.path, loc);
        }
        else
        {
            /* relative: replace everything after the last '/' of the
               base path (ignoring any query string). */
            char *q = strchr(u.path, '?');
            char *last;
            if (q != NULL) *q = 0;
            last = strrchr(u.path, '/');
            if (last == NULL) { u.path[0] = '/'; last = u.path; }
            if ((long)(last - u.path) + 1 + (long)strlen(loc) + 1 > kMaxURL)
                return -1;
            strcpy(last + 1, loc);
        }
        FormatURL(&u, out);
    }
    return 0;
}


/* ==================================================================
   PART 3: a tiny buffered reader over the connection
   ================================================================== */

static char gRx[kRxBufSize];
static long gRxPos = 0, gRxLen = 0;
static int  gRxEOF = 0;
static long gNetErr = 0;
static unsigned long gBytesIn = 0;

static void RxReset(void)
{
    gRxPos = gRxLen = 0;
    gRxEOF = 0;
    gNetErr = 0;
}

/* Ensure at least one unread byte is buffered. 0 ok, -1 EOF/error. */
static int RxFill(void)
{
    long n;
    if (gRxPos < gRxLen) return 0;
    if (gRxEOF) return -1;
    n = NetRecv(gRx, kRxBufSize);
    if (n <= 0)
    {
        gRxEOF = 1;
        if (n < 0) gNetErr = n;
        return -1;
    }
    gRxPos = 0;
    gRxLen = n;
    gBytesIn += (unsigned long)n;
    return 0;
}

/* Copy exactly n bytes (dst may be NULL to discard). 0 ok, -1 short. */
static int RxRead(unsigned char *dst, unsigned long n)
{
    unsigned long lastReport = 0, done = 0, total = n;

    while (n > 0)
    {
        unsigned long take;
        if (RxFill() != 0) return -1;
        take = (unsigned long)(gRxLen - gRxPos);
        if (take > n) take = n;
        if (dst != NULL)
        {
            memcpy(dst, gRx + gRxPos, (size_t)take);
            dst += take;
        }
        gRxPos += (long)take;
        n -= take;
        done += take;
        if (total >= 262144UL && done - lastReport >= 262144UL)
        {
            Say("    ... %lu of %lu KB\n", done / 1024UL, total / 1024UL);
            lastReport = done;
        }
    }
    return 0;
}


/* ==================================================================
   PART 4: HTTP
   ================================================================== */

/* Find header 'name' in the header block (case-insensitive, at line
   start, exact name followed by ':'), copy its value to val.
   Returns 1 if found. */
static int GetHeader(const char *hdrs, const char *name, char *val, long max)
{
    const char *line = hdrs;
    long nl = (long)strlen(name);

    while (*line)
    {
        const char *eol = strchr(line, '\n');
        if (eol == NULL) eol = line + strlen(line);

        if (StrNCaseEq(line, name, nl) && line[nl] == ':')
        {
            const char *v = line + nl + 1;
            long len;
            while (*v == ' ' || *v == '\t') v++;
            len = (long)(eol - v);
            while (len > 0 && (v[len - 1] == '\r' || v[len - 1] == ' '
                               || v[len - 1] == '\t'))
                len--;
            if (len >= max) len = max - 1;
            memcpy(val, v, (size_t)len);
            val[len] = 0;
            return 1;
        }
        if (*eol == 0) break;
        line = eol + 1;
    }
    return 0;
}

static char gHdr[kHeaderMax];

/* Read the response headers into gHdr. Leaves the first body bytes in
   the reader. Returns 0 ok, -1 on EOF/overflow. */
static int ReadHeaders(void)
{
    long len = 0;

    for (;;)
    {
        if (RxFill() != 0) return -1;
        if (len >= kHeaderMax - 1) return -1;
        gHdr[len++] = gRx[gRxPos++];
        gHdr[len] = 0;
        if (len >= 2 && gHdr[len - 1] == '\n'
            && (gHdr[len - 2] == '\n'
                || (len >= 4 && gHdr[len - 2] == '\r' && gHdr[len - 3] == '\n')))
            return 0;
    }
}

/* Perform a GET, following redirects. rangeFrom < 0 means "no Range".
   On success the connection is open, gHdr holds the final headers,
   *status is 200 or 206, and finalURL holds the URL that answered.
   Returns 0 ok, -1 failure (already reported). */
static int HttpGet(const char *startURL, long rangeFrom,
                   char *finalURL, int *status)
{
    char     url[kMaxURL];
    char     req[kMaxURL + 512];
    URLParts u;
    int      hops, r;

    if (strlen(startURL) >= kMaxURL) { Say("  URL too long.\n"); return -1; }
    strcpy(url, startURL);
    gHttpsSeen = 0;

    for (hops = 0; hops <= kMaxRedirects; hops++)
    {
        int code = 0;
        const char *sp;

        r = ParseURL(url, &u);
        if (r != 0)
        {
            if (StrNCaseEq(url, "https://", 8))
                Say("  This is an https URL. Only plain http:// works:\n"
                    "  QuickTime 6 itself cannot fetch https, so the movie\n"
                    "  could not play even if we downloaded it.\n"
                    "  Try the same address with http:// instead.\n");
            else
                Say("  Not a usable http:// URL: %s\n", url);
            return -1;
        }

        Say("Connecting to %s port %u ...\n", u.host, (unsigned)u.port);
        if (NetOpen(u.host, u.port) != 0) return -1;
        RxReset();

        /* HTTP/1.0: the server closes the connection after the body, so
           there is no chunked encoding and no keep-alive to deal with.
           A Range header is an HTTP/1.1 invention, but every server we
           care about honours it on a 1.0 request too. */
        if (u.port == 80)
            sprintf(req, "GET %s HTTP/1.0\r\nHost: %s\r\n", u.path, u.host);
        else
            sprintf(req, "GET %s HTTP/1.0\r\nHost: %s:%u\r\n",
                    u.path, u.host, (unsigned)u.port);
        sprintf(req + strlen(req),
                "User-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n",
                kUserAgent);
        if (rangeFrom >= 0)
            sprintf(req + strlen(req), "Range: bytes=%lu-\r\n",
                    (unsigned long)rangeFrom);
        strcat(req, "\r\n");

        if (NetSend(req, (long)strlen(req)) != (long)strlen(req))
        {
            Say("  Could not send the request.\n");
            NetClose();
            return -1;
        }

        if (ReadHeaders() != 0)
        {
            if (gNetErr != 0)
                Say("  No answer from the server (error/timeout %ld).\n",
                    gNetErr);
            else
                Say("  The server closed the connection without a valid\n"
                    "  HTTP response.\n");
            NetClose();
            return -1;
        }

        sp = strchr(gHdr, ' ');
        if (strncmp(gHdr, "HTTP/", 5) != 0 || sp == NULL)
        {
            Say("  That is not an HTTP response.\n");
            NetClose();
            return -1;
        }
        code = atoi(sp + 1);
        {
            /* print the status line */
            char line[128];
            long i = 0;
            while (gHdr[i] && gHdr[i] != '\r' && gHdr[i] != '\n' && i < 127)
            { line[i] = gHdr[i]; i++; }
            line[i] = 0;
            Say("  %s\n", line);
        }

        if (code == 301 || code == 302 || code == 303 || code == 307
            || code == 308)
        {
            char loc[kMaxURL];
            char next[kMaxURL];
            NetClose();
            if (!GetHeader(gHdr, "Location", loc, sizeof(loc)))
            {
                Say("  Redirect without a Location header.\n");
                return -1;
            }
            if (ResolveLocation(&u, loc, next) != 0)
            {
                Say("  Cannot follow redirect to: %s\n", loc);
                return -1;
            }
            Say("  Redirected to %s\n", next);
            strcpy(url, next);
            continue;
        }

        if (code == 200 || code == 206)
        {
            strcpy(finalURL, url);
            *status = code;
            return 0;
        }

        Say("  The server said %d; cannot continue.\n", code);
        NetClose();
        return -1;
    }

    Say("  Too many redirects (more than %d).\n", kMaxRedirects);
    return -1;
}


/* ==================================================================
   PART 5: walking the top-level boxes of the remote file
   ================================================================== */

/* Fetch the moov of the file at url. On success *moovOut is a MemAlloc
   block (header included), and finalURL is where the file really is. */
static int FetchMoov(const char *url, char *finalURL,
                     unsigned char **moovOut, long *moovLenOut)
{
    unsigned long pos = 0;     /* absolute offset in the remote file */
    int status;
    int boxes = 0;

    if (HttpGet(url, -1, finalURL, &status) != 0) return -1;

    for (;;)
    {
        unsigned char hb[16];
        unsigned long size;
        long hdr = 8;
        char tname[5];

        if (++boxes > 64)
        {
            Say("  More than 64 top-level boxes and no moov yet. This does\n"
                "  not look like an MP4 file.\n");
            NetClose();
            return -1;
        }

        if (RxRead(hb, 8) != 0)
        {
            Say("  Reached the end of the file without finding a 'moov'.\n");
            NetClose();
            return -1;
        }
        size = RD32(hb);
        FourCCText(hb + 4, tname);

        if (boxes == 1 && !ISTYPE(hb + 4, "ftyp") && !ISTYPE(hb + 4, "moov")
            && !ISTYPE(hb + 4, "mdat") && !ISTYPE(hb + 4, "free")
            && !ISTYPE(hb + 4, "wide") && !ISTYPE(hb + 4, "skip"))
        {
            Say("  The file does not start like an MP4 (first box '%s').\n"
                "  Is the URL really an .mp4 file and not a web page?\n",
                tname);
            NetClose();
            return -1;
        }

        if (size == 1)
        {
            if (RxRead(hb + 8, 8) != 0)
            {
                Say("  Truncated box header.\n");
                NetClose();
                return -1;
            }
            hdr = 16;
            if (RD32(hb + 8) != 0)
            {
                Say("  Box '%s' at offset %lu is larger than 4 GB; this\n"
                    "  program cannot skip that far.\n", tname, pos);
                NetClose();
                return -1;
            }
            size = RD32(hb + 12);
        }
        else if (size == 0)
        {
            Say("  Box '%s' at offset %lu runs to the end of the file, and\n"
                "  no 'moov' came before it. Cannot continue.\n", tname, pos);
            NetClose();
            return -1;
        }
        if (size < (unsigned long)hdr)
        {
            Say("  Corrupt box '%s' (size %lu) at offset %lu.\n",
                tname, size, pos);
            NetClose();
            return -1;
        }

        Say("  box '%s'  %lu bytes at offset %lu\n", tname, size, pos);

        if (ISTYPE(hb + 4, "moov"))
        {
            unsigned char *m;
            if (size > (unsigned long)kMaxMoov)
            {
                Say("  The moov is %lu bytes; the limit is %ld. Giving up.\n",
                    size, kMaxMoov);
                NetClose();
                return -1;
            }
            m = (unsigned char *)MemAlloc((long)size);
            if (m == NULL)
            {
                Say("  Not enough memory for a %lu-byte moov. Give the\n"
                    "  application more memory in Get Info.\n", size);
                NetClose();
                return -1;
            }
            memcpy(m, hb, (size_t)hdr);
            Say("  Reading the moov (%lu KB)...\n", size / 1024UL);
            if (RxRead(m + hdr, size - (unsigned long)hdr) != 0)
            {
                Say("  The connection ended in the middle of the moov.\n");
                MemFree(m);
                NetClose();
                return -1;
            }
            /* We have all we came for. Hanging up now abandons the rest
               of the body (the media), which is the whole point. */
            NetClose();
            *moovOut = m;
            *moovLenOut = (long)size;
            return 0;
        }

        if (size - (unsigned long)hdr <= (unsigned long)kSkipByReading)
        {
            if (RxRead(NULL, size - (unsigned long)hdr) != 0)
            {
                Say("  The file ended inside box '%s' without a moov.\n",
                    tname);
                NetClose();
                return -1;
            }
            pos += size;
            continue;
        }

        /* A big box -- in practice the media data. Jump over it. */
        if (pos + size < pos)
        {
            Say("  Offsets beyond 4 GB are not supported.\n");
            NetClose();
            return -1;
        }
        pos += size;
        NetClose();
        Say("  Skipping '%s' (%lu KB): asking for bytes %lu onward.\n",
            tname, size / 1024UL, pos);

        if (HttpGet(finalURL, (long)pos, finalURL, &status) != 0) return -1;
        if (status != 206)
        {
            Say("  The server ignored the Range request (it answered 200\n"
                "  instead of 206), so the only way to reach the moov\n"
                "  would be to download the whole movie first.\n"
                "  This file has its moov at the END (not \"fast start\").\n"
                "  Remux it with the moov first (e.g. ffmpeg -c copy\n"
                "  -movflags +faststart) or use a server that supports\n"
                "  byte ranges.\n");
            NetClose();
            return -1;
        }
        {
            char cr[128];
            if (GetHeader(gHdr, "Content-Range", cr, sizeof(cr)))
            {
                const char *d = cr;
                unsigned long first = 0;
                while (*d && (*d < '0' || *d > '9')) d++;
                while (*d >= '0' && *d <= '9')
                    first = first * 10UL + (unsigned long)(*d++ - '0');
                if (first != pos)
                {
                    Say("  The server sent bytes from %lu, not %lu.\n",
                        first, pos);
                    NetClose();
                    return -1;
                }
            }
        }
    }
}


/* ==================================================================
   PART 6: naming the output
   ================================================================== */

static int HexVal(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = LowerC(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Last path component of the URL, %-decoded, made HFS-safe, with the
   extension changed to .mov, at most 31 characters. */
static void MovieNameFromURL(const char *url, char *name)
{
    const char *p = url, *start, *end;
    char base[256];
    long n = 0, maxBase;
    char *dot;

    if (strstr(p, "://") != NULL) p = strstr(p, "://") + 3;
    end = p;
    while (*end && *end != '?' && *end != '#') end++;
    start = end;
    while (start > p && start[-1] != '/') start--;
    if (start == p) start = end;            /* no path at all */

    while (start < end && n < 255)
    {
        int c = (unsigned char)*start;
        if (c == '%' && start + 2 < end && HexVal(start[1]) >= 0
            && HexVal(start[2]) >= 0)
        {
            c = HexVal(start[1]) * 16 + HexVal(start[2]);
            start += 3;
        }
        else
            start++;
        if (c == ':' || c < 32 || c > 126) c = '_';
        base[n++] = (char)c;
    }
    base[n] = 0;

    /* drop an existing extension we recognise */
    dot = strrchr(base, '.');
    if (dot != NULL && (StrNCaseEq(dot, ".mp4", 5) || StrNCaseEq(dot, ".m4v", 5)
                        || StrNCaseEq(dot, ".mov", 5)))
        *dot = 0;
    if (base[0] == 0 || base[0] == '.') strcpy(base, "stream");

    maxBase = 31 - 4;
    if ((long)strlen(base) > maxBase) base[maxBase] = 0;
    sprintf(name, "%s.mov", base);
}


/* ==================================================================
   PART 7: the whole job
   ================================================================== */

static int StreamURL(const char *url)
{
    char           finalURL[kMaxURL];
    char           name[64];
    unsigned char *moov = NULL, *ref = NULL;
    long           moovLen = 0, refLen = 0;
    int            rc;

    Say("\nURL: %s\n", url);
    gBytesIn = 0;

    if (FetchMoov(url, finalURL, &moov, &moovLen) != 0) return -1;
    Say("Got the moov (%ld bytes; %lu bytes received in all).\n",
        moovLen, gBytesIn);
    if (strcmp(finalURL, url) != 0)
        Say("Media URL after redirects: %s\n", finalURL);

    /* Never hand QuickTime a stream our decoder cannot play: a High
       profile file left QuickTime Player stuck behind a modal "required
       compressor could not be found" alert. */
    {
        McAvcInfo info;
        char      why[200];
        int       found = mc_avc_profile(moov, moovLen, &info);
        int       v = mc_avc_verdict(&info, found, why, sizeof(why));
        Say("  %s\n", why);
        if (v < 0)
        {
            MemFree(moov);
            return -1;
        }
    }

    /* The reference movie keeps the ORIGINAL URL: QuickTime follows
       the redirect itself. A stored post-redirect storage-node URL
       failed an hour later with -3162 (session 15). */
    ref = BuildReferenceMovie(moov, moovLen, url, &refLen);
    MemFree(moov);
    if (ref == NULL) return -1;

    MovieNameFromURL(finalURL, name);
    rc = SaveAndOpenMovie(name, ref, refLen);
    MemFree(ref);
    return rc;
}


/* Trim whitespace, CR and LF from both ends, in place. */
static void Trim(char *s)
{
    long n = (long)strlen(s), i = 0;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n'
                     || s[n - 1] == '\t'))
        s[--n] = 0;
    while (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n') i++;
    if (i > 0) memmove(s, s + i, (size_t)(n - i + 1));
}


#ifndef H264STREAM_HOSTTEST
/* ==================================================================
   PART 8: the Mac side -- memory, Open Transport, files, Finder
   ================================================================== */

/* ---------------- memory ----------------
   A moov can be megabytes. We try the application heap first and then
   Process Manager temporary memory, which is whatever free RAM the
   machine has outside every partition -- so a big moov does not
   require raising the application's memory size. A 16-byte prefix
   remembers which kind of block it is, so MemFree can undo it. */

#define kTagPtr   0x50545220UL   /* 'PTR ' */
#define kTagTemp  0x544D5048UL   /* 'TMPH' */

static void *MemAlloc(long n)
{
    Ptr p;
    Handle h;
    OSErr err;

    p = NewPtr((Size)(n + 16));
    if (p != NULL)
    {
        WR32((unsigned char *)p, kTagPtr);
        return p + 16;
    }
    h = TempNewHandle((Size)(n + 16), &err);
    if (h == NULL || err != noErr) return NULL;
    HLock(h);
    WR32((unsigned char *)*h, kTagTemp);
    memcpy(*h + 4, &h, sizeof(h));
    return *h + 16;
}

static void MemFree(void *v)
{
    unsigned char *p;
    if (v == NULL) return;
    p = (unsigned char *)v - 16;
    if (RD32(p) == kTagTemp)
    {
        Handle h;
        memcpy(&h, p + 4, sizeof(h));
        HUnlock(h);
        DisposeHandle(h);
    }
    else
        DisposePtr((Ptr)p);
}

/* ---------------- Open Transport ---------------- */

static EndpointRef gEP = kOTInvalidEndpointRef;
static int         gOTReady = 0;

/* Give other applications time while we wait. Only update events
   are taken (so the dialog redraws if uncovered); clicks, keys and
   Apple events stay queued until the download is over, but the
   Process Manager can switch to the Finder etc. */
static void DoUpdate(WindowPtr w);
static void Yield(void)
{
    EventRecord ev;
    if (WaitNextEvent(updateMask, &ev, 1, NULL) && ev.what == updateEvt)
        DoUpdate((WindowPtr)ev.message);
}

static int NetOpen(const char *host, unsigned short port)
{
    OSStatus   err;
    TCall      call;
    DNSAddress dns;
    char       hostPort[kMaxHost + 8];
    long       dnsLen;

    if (!gOTReady)
    {
        err = InitOpenTransport();
        if (err != kOTNoError)
        {
            Say("  InitOpenTransport failed (err %ld). Is TCP/IP set up\n"
                "  in the TCP/IP control panel?\n", (long)err);
            return -1;
        }
        gOTReady = 1;
    }

    gEP = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, &err);
    if (gEP == kOTInvalidEndpointRef || err != kOTNoError)
    {
        Say("  OTOpenEndpoint failed (err %ld).\n", (long)err);
        gEP = kOTInvalidEndpointRef;
        return -1;
    }
    (void)OTSetSynchronous(gEP);
    (void)OTSetBlocking(gEP);

    /* Outbound only: a NULL bind lets OT pick the local port. */
    err = OTBind(gEP, NULL, NULL);
    if (err != kOTNoError)
    {
        Say("  OTBind failed (err %ld).\n", (long)err);
        OTCloseProvider(gEP);
        gEP = kOTInvalidEndpointRef;
        return -1;
    }

    /* "host:port" as a DNS address. The resolver runs inside OTConnect.
       PITFALL: addr.len is what OTInitDNSAddress RETURNS. */
    sprintf(hostPort, "%s:%u", host, (unsigned)port);
    memset(&call, 0, sizeof(call));
    dnsLen = (long)OTInitDNSAddress(&dns, hostPort);
    if (dnsLen <= 0)
    {
        Say("  OTInitDNSAddress returned %ld.\n", dnsLen);
        OTCloseProvider(gEP);
        gEP = kOTInvalidEndpointRef;
        return -1;
    }
    call.addr.buf = (UInt8 *)&dns;
    call.addr.len = (ByteCount)dnsLen;

    err = OTConnect(gEP, &call, NULL);
    if (err != kOTNoError)
    {
        Say("  Could not connect to %s (err %ld).\n", hostPort, (long)err);
        if (err == kOTLookErr)
        {
            OTResult look = OTLook(gEP);
            if (look == T_DISCONNECT) (void)OTRcvDisconnect(gEP, NULL);
            Say("  (The connection was refused or reset; OTLook = %ld.)\n",
                (long)look);
        }
        else if (err == kOTBadNameErr || err == -3201)
            Say("  (The host name could not be resolved. Check DNS in\n"
                "   the TCP/IP control panel.)\n");
        OTCloseProvider(gEP);
        gEP = kOTInvalidEndpointRef;
        return -1;
    }
    return 0;
}

static long NetSend(const char *buf, long n)
{
    long sent = 0;
    if (gEP == kOTInvalidEndpointRef) return -1;
    (void)OTSetBlocking(gEP);
    while (sent < n)
    {
        OTResult r = OTSnd(gEP, (void *)(buf + sent), (OTByteCount)(n - sent), 0);
        if (r < 0) return (long)r;
        sent += (long)r;
    }
    /* From here on, receive without blocking so we can time out and
       let the rest of the Mac breathe. */
    (void)OTSetNonBlocking(gEP);
    return sent;
}

static long NetRecv(char *buf, long max)
{
    OTFlags       flags;
    OTResult      r;
    unsigned long lastData = TickCount();

    if (gEP == kOTInvalidEndpointRef) return -1;

    for (;;)
    {
        flags = 0;
        r = OTRcv(gEP, buf, (OTByteCount)max, &flags);
        if (r > 0) return (long)r;

        if (r == kOTNoDataErr)
        {
            if (TickCount() - lastData > 60UL * 60UL)
            {
                Say("  Timed out: no data for 60 seconds.\n");
                return -2;
            }
            Yield();
            continue;
        }

        if (r == kOTLookErr)
        {
            /* PITFALL: an event, not an error. Acknowledge it. */
            OTResult look = OTLook(gEP);
            if (look == T_ORDREL)
            {
                (void)OTRcvOrderlyDisconnect(gEP);
                return 0;                       /* normal end of body */
            }
            if (look == T_DISCONNECT)
            {
                (void)OTRcvDisconnect(gEP, NULL);
                return 0;                       /* reset: treat as end */
            }
            if (look == T_DATA || look == T_EXDATA || look == T_GODATA)
                continue;
            Say("  Unexpected Open Transport event %ld.\n", (long)look);
            return -3;
        }

        if (r == 0) return 0;
        return (long)r;                          /* some other error */
    }
}

static void NetClose(void)
{
    if (gEP != kOTInvalidEndpointRef)
    {
        /* Closing the provider is an abortive close, which is exactly
           right when we are walking away from 20 MB of media we do not
           want. The server just sees the connection drop. */
        (void)OTSetSynchronous(gEP);
        OTCloseProvider(gEP);
        gEP = kOTInvalidEndpointRef;
    }
}

/* ---------------- the URL file next to the application ---------------- */

static int GetAppSpec(FSSpec *app)
{
    ProcessSerialNumber psn;
    ProcessInfoRec      info;
    Str255              pname;

    if (GetCurrentProcess(&psn) != noErr) return -1;
    memset(&info, 0, sizeof(info));
    info.processInfoLength = sizeof(info);
    info.processName = pname;
    info.processAppSpec = app;
    return (GetProcessInformation(&psn, &info) == noErr) ? 0 : -1;
}

static void CToPStr(const char *c, Str255 p)
{
    long n = (long)strlen(c);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, c, (size_t)n);
}

/* 1 = URL read from the file, 0 = no file. */
static int ReadURLFile(char *url, long max)
{
    FSSpec app, spec;
    Str255 pname;
    short  ref;
    long   n;

    if (GetAppSpec(&app) != 0) return 0;
    CToPStr(kURLFileName, pname);
    if (FSMakeFSSpec(app.vRefNum, app.parID, pname, &spec) != noErr) return 0;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return 0;
    n = max - 1;
    (void)FSRead(ref, &n, url);
    FSClose(ref);
    if (n < 0) n = 0;
    url[n] = 0;
    /* only the first line counts */
    {
        char *e = url;
        while (*e && *e != '\r' && *e != '\n') e++;
        *e = 0;
    }
    Trim(url);
    return url[0] != 0;
}

static void OpenLog(void)
{
    FSSpec app, spec;
    Str255 pname;

    /* MSL's fopen takes a path relative to the default directory, which
       for an application launched from the Finder is its own folder.
       To be safe we make the default directory the app's folder first. */
    if (GetAppSpec(&app) == 0)
    {
        WDPBRec wd;
        memset(&wd, 0, sizeof(wd));
        wd.ioVRefNum = app.vRefNum;
        wd.ioWDDirID = app.parID;
        (void)PBHSetVolSync(&wd);
        /* make sure the log is a TEXT file SimpleText can open */
        CToPStr(kLogFileName, pname);
        if (FSMakeFSSpec(app.vRefNum, app.parID, pname, &spec) == fnfErr)
            (void)FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript);
        /* MSL's fopen "w" does not cut the file: a shorter run left the
           previous run's tail behind. Cut it to 0 bytes ourselves. */
        if (FSMakeFSSpec(app.vRefNum, app.parID, pname, &spec) == noErr)
        {
            short ref;
            if (FSpOpenDF(&spec, fsRdWrPerm, &ref) == noErr)
            {
                (void)SetEOF(ref, 0);
                (void)FSClose(ref);
            }
        }
    }
    gLog = fopen(kLogFileName, "w");
}

/* ---------------- saving and opening the movie ---------------- */

static OSErr OpenWithFinder(const FSSpec *spec)
{
    AEAddressDesc target;
    AppleEvent    ev, reply;
    AEDescList    list;
    AliasHandle   alias = NULL;
    OSType        finder = 'MACS';
    OSErr         err;

    target.dataHandle = NULL; ev.dataHandle = NULL;
    reply.dataHandle = NULL; list.dataHandle = NULL;

    err = NewAlias(NULL, spec, &alias);
    if (err != noErr) return err;

    err = AECreateDesc(typeApplSignature, &finder, sizeof(finder), &target);
    if (err == noErr)
        err = AECreateAppleEvent(kCoreEventClass, kAEOpenDocuments, &target,
                                 kAutoGenerateReturnID, kAnyTransactionID, &ev);
    if (err == noErr)
        err = AECreateList(NULL, 0, false, &list);
    if (err == noErr)
    {
        HLock((Handle)alias);
        err = AEPutPtr(&list, 0, typeAlias, *alias,
                       GetHandleSize((Handle)alias));
        HUnlock((Handle)alias);
    }
    if (err == noErr)
        err = AEPutParamDesc(&ev, keyDirectObject, &list);
    if (err == noErr)
        err = AESend(&ev, &reply, kAENoReply, kAENormalPriority,
                     kAEDefaultTimeout, NULL, NULL);

    if (list.dataHandle)   AEDisposeDesc(&list);
    if (ev.dataHandle)     AEDisposeDesc(&ev);
    if (target.dataHandle) AEDisposeDesc(&target);
    if (reply.dataHandle)  AEDisposeDesc(&reply);
    DisposeHandle((Handle)alias);
    return err;
}

static int SaveAndOpenMovie(const char *name, const unsigned char *data,
                            long n)
{
    short  vRef, ref;
    long   dirID, count;
    FSSpec spec;
    Str255 pname;
    OSErr  err;
    char   tryName[64];
    int    i;

    err = FindFolder(kOnSystemDisk, kDesktopFolderType, kDontCreateFolder,
                     &vRef, &dirID);
    if (err != noErr)
    {
        Say("  Cannot find the Desktop folder (err %d).\n", (int)err);
        return -1;
    }

    /* Never overwrite: if the name is taken, try "name 2.mov" etc. */
    strcpy(tryName, name);
    for (i = 2; ; i++)
    {
        CToPStr(tryName, pname);
        err = FSMakeFSSpec(vRef, dirID, pname, &spec);
        if (err == fnfErr) break;
        if (err != noErr)
        {
            Say("  Bad file name \"%s\" (err %d).\n", tryName, (int)err);
            return -1;
        }
        if (i > 99)
        {
            Say("  Too many files called %s on the Desktop.\n", name);
            return -1;
        }
        {
            char base[40];
            char suffix[8];
            long bl = (long)strlen(name) - 4;   /* without ".mov" */
            sprintf(suffix, " %d", i);
            if (bl > 27 - (long)strlen(suffix)) bl = 27 - (long)strlen(suffix);
            memcpy(base, name, (size_t)bl);
            base[bl] = 0;
            sprintf(tryName, "%s%s.mov", base, suffix);
        }
    }

    err = FSpCreate(&spec, 'TVOD', 'MooV', smSystemScript);
    if (err == noErr) err = FSpOpenDF(&spec, fsRdWrPerm, &ref);
    if (err != noErr)
    {
        Say("  Could not create \"%s\" on the Desktop (err %d).\n",
            tryName, (int)err);
        return -1;
    }
    count = n;
    err = FSWrite(ref, &count, data);
    FSClose(ref);
    FlushVol(NULL, vRef);
    if (err != noErr || count != n)
    {
        Say("  Write failed (err %d).\n", (int)err);
        return -1;
    }
    Say("Saved the reference movie on the Desktop as \"%s\" (%ld bytes).\n",
        tryName, n);

    err = OpenWithFinder(&spec);
    if (err != noErr)
    {
        Say("  Could not ask the Finder to open it (err %d).\n"
            "  Double-click it on the Desktop instead.\n", (int)err);
        return 0;     /* the file is fine; only the convenience failed */
    }
    Say("Asked the Finder to open it in QuickTime Player.\n"
        "Press Play. The media streams from the web as it plays.\n");
    return 0;
}

#ifndef H264STREAM_LIBRARY
/* Everything from here to main() is H264 Stream's own user interface.
   An app that #includes this file with H264STREAM_LIBRARY defined
   supplies its own UIStatus(), DoUpdate() and main(). */

/* ---------------- the dialog ---------------- */

/* Item numbers in the dialog built by MakeDialog. */
#define kItemOK      1
#define kItemExit    2
#define kItemLabel   3
#define kItemURL     4
#define kItemStatus  5

#define kLastURLFileName "H264 Stream Last URL"

static DialogPtr gDlg = NULL;
static int       gQuit = 0;
static int       gBusy = 0;

/* Show one line in the status item and draw it at once (no update
   event needed, so it also works while a download is in progress). */
static void UIStatus(const char *line)
{
    short   type;
    Handle  h;
    Rect    r;
    Str255  p;
    GrafPtr save;

    if (gDlg == NULL) return;
    GetDialogItem(gDlg, kItemStatus, &type, &h, &r);
    CToPStr(line, p);
    GetPort(&save);
    SetPort(gDlg);
    SetDialogItemText(h, p);
    EraseRect(&r);
    TETextBox(p + 1, p[0], &r, teFlushDefault);
    SetPort(save);
}

/* The bold ring round the default button. */
static void DrawDefaultRing(void)
{
    short   type;
    Handle  h;
    Rect    r;
    GrafPtr save;

    GetDialogItem(gDlg, kItemOK, &type, &h, &r);
    GetPort(&save);
    SetPort(gDlg);
    PenSize(3, 3);
    InsetRect(&r, -4, -4);
    FrameRoundRect(&r, 16, 16);
    PenNormal();
    SetPort(save);
}

static void DoUpdate(WindowPtr w)
{
    GrafPtr save;
    GetPort(&save);
    SetPort(w);
    BeginUpdate(w);
    if (w == gDlg)
    {
        UpdateDialog(gDlg, gDlg->visRgn);
        DrawDefaultRing();
    }
    EndUpdate(w);
    SetPort(save);
}

/* Append one item to an in-memory DITL. */
static void AddItem(Handle ditl, short top, short left, short bottom,
                    short right, unsigned char kind, const char *text)
{
    long          at = GetHandleSize(ditl);
    long          len = (long)strlen(text);
    long          size = 4 + 8 + 2 + len + (len & 1);
    unsigned char *p;
    short          *count;

    SetHandleSize(ditl, at + size);
    if (MemError() != noErr) return;
    p = (unsigned char *)*ditl + at;
    memset(p, 0, (size_t)size);
    p[4]  = (unsigned char)(top >> 8);    p[5]  = (unsigned char)top;
    p[6]  = (unsigned char)(left >> 8);   p[7]  = (unsigned char)left;
    p[8]  = (unsigned char)(bottom >> 8); p[9]  = (unsigned char)bottom;
    p[10] = (unsigned char)(right >> 8);  p[11] = (unsigned char)right;
    p[12] = kind;
    p[13] = (unsigned char)len;
    memcpy(p + 14, text, (size_t)len);
    count = (short *)*ditl;
    if (at > 2) (*count)++;            /* stored as "items - 1" */
}

static DialogPtr MakeDialog(void)
{
    Handle ditl;
    Rect   bounds, scr;
    Str255 title;
    short  w = 480, h = 132, left, top;

    ditl = NewHandleClear(2);
    if (ditl == NULL) return NULL;
    AddItem(ditl, 100, 380, 120, 450, ctrlItem + btnCtrl, "OK");
    AddItem(ditl, 100, 290, 120, 360, ctrlItem + btnCtrl, "Exit");
    AddItem(ditl,  12,  20,  28, 460, statText, "Insert URL:");
    AddItem(ditl,  35,  23,  51, 463, editText, "");
    AddItem(ditl,  60,  20,  92, 460, statText, "");

    scr = qd.screenBits.bounds;
    left = (short)((scr.right - scr.left - w) / 2);
    top  = (short)((scr.bottom - scr.top - h) / 3 + 20);
    if (top < 44) top = 44;
    SetRect(&bounds, left, top, left + w, top + h);
    CToPStr("H264 Stream", title);
    return NewDialog(NULL, &bounds, title, true, movableDBoxProc,
                     (WindowPtr)-1L, false, 0, ditl);
}

static void GetURLText(char *url, long max)
{
    short  type;
    Handle h;
    Rect   r;
    Str255 p;

    GetDialogItem(gDlg, kItemURL, &type, &h, &r);
    GetDialogItemText(h, p);
    if ((long)p[0] > max - 1) p[0] = (unsigned char)(max - 1);
    memcpy(url, p + 1, p[0]);
    url[p[0]] = 0;
}

static void SetURLText(const char *url)
{
    short  type;
    Handle h;
    Rect   r;
    Str255 p;

    GetDialogItem(gDlg, kItemURL, &type, &h, &r);
    CToPStr(url, p);
    SetDialogItemText(h, p);
    SelectDialogItemText(gDlg, kItemURL, 0, 32767);
}

/* Flash a button as if it had been clicked. */
static void FlashButton(short item)
{
    short         type;
    Handle        h;
    Rect          r;
    unsigned long t;

    GetDialogItem(gDlg, item, &type, &h, &r);
    HiliteControl((ControlHandle)h, 10);   /* inButton */
    Delay(8, &t);
    HiliteControl((ControlHandle)h, 0);
}

/* ---------------- the small "last URL" file ---------------- */

static void ReadLastURL(char *url, long max)
{
    FSSpec app, spec;
    Str255 pname;
    short  ref;
    long   n;

    url[0] = 0;
    if (GetAppSpec(&app) != 0) return;
    CToPStr(kLastURLFileName, pname);
    if (FSMakeFSSpec(app.vRefNum, app.parID, pname, &spec) != noErr) return;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return;
    n = max - 1;
    (void)FSRead(ref, &n, url);
    FSClose(ref);
    if (n < 0) n = 0;
    url[n] = 0;
    {
        char *e = url;
        while (*e && *e != '\r' && *e != '\n') e++;
        *e = 0;
    }
    Trim(url);
}

static void WriteLastURL(const char *url)
{
    FSSpec app, spec;
    Str255 pname;
    short  ref;
    long   n;
    OSErr  err;

    if (GetAppSpec(&app) != 0) return;
    CToPStr(kLastURLFileName, pname);
    err = FSMakeFSSpec(app.vRefNum, app.parID, pname, &spec);
    if (err == fnfErr)
        err = FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript);
    if (err != noErr) return;
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return;
    n = (long)strlen(url);
    (void)SetEOF(ref, 0);
    (void)FSWrite(ref, &n, url);
    n = 1;
    (void)FSWrite(ref, &n, "\r");
    FSClose(ref);
    FlushVol(NULL, spec.vRefNum);
}

/* ---------------- Apple events ---------------- */

static pascal OSErr AEQuit(const AppleEvent *e, AppleEvent *r, long refcon)
{
#pragma unused(e, r, refcon)
    gQuit = 1;
    return noErr;
}

static pascal OSErr AEIgnore(const AppleEvent *e, AppleEvent *r, long refcon)
{
#pragma unused(e, r, refcon)
    return noErr;
}

static void InstallAEHandlers(void)
{
    (void)AEInstallEventHandler(kCoreEventClass, kAEQuitApplication,
            NewAEEventHandlerUPP(AEQuit), 0, false);
    (void)AEInstallEventHandler(kCoreEventClass, kAEOpenApplication,
            NewAEEventHandlerUPP(AEIgnore), 0, false);
    (void)AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments,
            NewAEEventHandlerUPP(AEIgnore), 0, false);
    (void)AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments,
            NewAEEventHandlerUPP(AEIgnore), 0, false);
}

/* ---------------- one fetch, started from the dialog ---------------- */

static void RunFromDialog(void)
{
    char url[kMaxURL];
    int  rc;

    GetURLText(url, 256);
    Trim(url);
    if (url[0] == 0)
    {
        SysBeep(10);
        UIStatus("Please type the http:// address of an .mp4 file.");
        SelectDialogItemText(gDlg, kItemURL, 0, 32767);
        return;
    }
    WriteLastURL(url);
    SetCursor(*GetCursor(watchCursor));
    gBusy = 1;
    rc = StreamURL(url);
    NetClose();
    gBusy = 0;
    InitCursor();
    /* clicks and keys typed while we were busy are thrown away */
    FlushEvents(mDownMask | mUpMask | keyDownMask | keyUpMask | autoKeyMask, 0);
    if (rc == 0)
    {
        Say("Done.\n");
        UIStatus("Opened in QuickTime Player.");
    }
    else
    {
        char msg[300];
        sprintf(msg, "Failed: %.250s", gLastMsg);
        Say("FAILED.\n");
        UIStatus(msg);
        SysBeep(10);
    }
    SelectDialogItemText(gDlg, kItemURL, 0, 32767);
}

static void DoKey(EventRecord *ev)
{
    char  c = (char)(ev->message & charCodeMask);
    short item;
    DialogPtr d;

    if (ev->modifiers & cmdKey)
    {
        if (c == '.') { FlashButton(kItemExit); gQuit = 1; return; }
        if (c == 'x' || c == 'X')
        { DialogCut(gDlg); (void)ZeroScrap(); (void)TEToScrap(); return; }
        if (c == 'c' || c == 'C')
        { DialogCopy(gDlg); (void)ZeroScrap(); (void)TEToScrap(); return; }
        if (c == 'v' || c == 'V')
        { (void)TEFromScrap(); DialogPaste(gDlg); return; }
        if (c == 'a' || c == 'A')
        { SelectDialogItemText(gDlg, kItemURL, 0, 32767); return; }
        if (c == 'q' || c == 'Q') { gQuit = 1; return; }
        return;
    }
    if (c == 0x0D || c == 0x03) { FlashButton(kItemOK); RunFromDialog(); return; }
    if (c == 0x1B) { FlashButton(kItemExit); gQuit = 1; return; }
    if (DialogSelect(ev, &d, &item)) { /* typing only */ }
}

static void DialogLoop(void)
{
    EventRecord ev;
    DialogPtr   d;
    short       item;
    WindowPtr   w;
    char        last[kMaxURL];

    gDlg = MakeDialog();
    if (gDlg == NULL) { SysBeep(10); return; }
    SetPort(gDlg);
    ReadLastURL(last, 256);
    SetURLText(last);
    UIStatus("Type or paste the http:// address of an H.264 .mp4 file.");

    while (!gQuit)
    {
        if (!WaitNextEvent(everyEvent, &ev, 15, NULL))
        {
            /* null event: blink the caret */
            if (FrontWindow() == gDlg) (void)DialogSelect(&ev, &d, &item);
            continue;
        }
        switch (ev.what)
        {
        case kHighLevelEvent:
            (void)AEProcessAppleEvent(&ev);
            break;
        case updateEvt:
            DoUpdate((WindowPtr)ev.message);
            break;
        case keyDown:
        case autoKey:
            DoKey(&ev);
            break;
        case osEvt:
            if (((ev.message >> 24) & 0xFF) == suspendResumeMessage)
            {
                EventRecord a = ev;
                a.what = activateEvt;
                a.message = (long)gDlg;
                a.modifiers = (ev.message & resumeFlag) ? activeFlag : 0;
                if (ev.message & resumeFlag) (void)TEFromScrap();
                (void)DialogSelect(&a, &d, &item);
            }
            break;
        case mouseDown:
            switch (FindWindow(ev.where, &w))
            {
            case inDrag:
                DragWindow(w, ev.where, &qd.screenBits.bounds);
                break;
            case inContent:
                if (w != FrontWindow()) { SelectWindow(w); break; }
                if (DialogSelect(&ev, &d, &item))
                {
                    if (item == kItemOK)   RunFromDialog();
                    if (item == kItemExit) gQuit = 1;
                }
                break;
            case inSysWindow:
                SystemClick(&ev, w);
                break;
            }
            break;
        default:
            if (IsDialogEvent(&ev)) (void)DialogSelect(&ev, &d, &item);
            break;
        }
    }
    DisposeDialog(gDlg);
    gDlg = NULL;
}

/* ---------------- main ---------------- */

int main(void)
{
    char url[kMaxURL];
    int  rc = 0;

    MaxApplZone();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);
    InstallAEHandlers();

    OpenLog();
    Say("H264 Stream 1.2 -- play an H.264/AAC .mp4 from the web\n");
    Say("in QuickTime Player 6. Plain http:// only.\n");

    if (ReadURLFile(url, sizeof(url)))
    {
        /* test hook: no dialog, quit by itself either way */
        Say("(URL taken from the file \"%s\".)\n", kURLFileName);
        rc = StreamURL(url);
        NetClose();
        Say(rc == 0 ? "\nDone.\n" : "\nFAILED.\n");
    }
    else
    {
        DialogLoop();
        Say("Exit.\n");
    }

    if (gOTReady) CloseOpenTransport();
    if (gLog) { fclose(gLog); gLog = NULL; }
    return rc == 0 ? 0 : 1;
}
#endif /* !H264STREAM_LIBRARY */
#endif /* !H264STREAM_HOSTTEST */
