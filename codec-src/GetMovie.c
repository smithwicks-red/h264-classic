/*
    GetMovie.c  --  fetch an HTTPS movie straight onto this Mac,
                    patched on the way in so QuickTime will play it

    ------------------------------------------------------------------
    What this is for
    ------------------------------------------------------------------
    Paste a URL, get a playable .mov on the desktop. No second computer,
    no proxy, no browser download-then-convert dance.

    It exists because of two separate walls we hit, and it goes around
    both of them:

    1. QuickTime 6 has no TLS whatsoever, so it can never fetch from a
       modern HTTPS source on its own. macTLS solves that -- it does a
       real, validated TLS 1.2/1.3 handshake natively on the Mac.

    2. QuickTime 6 cannot open a standard MP4 containing H.264. Its
       MPEG-4 importer predates H.264 and drops the 'avcC' box our
       decoder needs, so the open fails with -8971 before a single NAL
       unit is decoded. The fix, established across six probes of one
       identical bitstream, is to route the file to QuickTime's MOV
       parser instead -- which copies every sample-description extension
       through verbatim, 'avcC' included. That needs the ftyp major
       brand to read 'qt  ', and the file to be typed/named as a movie.

    So: fetch over TLS, patch four bytes as they go past, write a .mov.

    ------------------------------------------------------------------
    Why fetch-to-disk and not a streaming proxy
    ------------------------------------------------------------------
    The tidier design would be a local proxy on a fixed port with
    QuickTime pointed at it through the Streaming Proxy pane. That may
    yet be possible -- see OTProbe.c -- but it depends on a passive
    OTBind to a chosen port, which macTLS proved impossible from a
    Carbon app and which is untested from a classic one.

    This program needs no listener at all. It only makes OUTBOUND
    connections, which is precisely the path macTLS has already verified
    on a real G3 running OS 9.1. So it works whatever OTProbe says,
    which makes it the higher-certainty thing to build first.

    ------------------------------------------------------------------
    Where to point it
    ------------------------------------------------------------------
    Any HTTPS URL serving a progressive MP4. For YouTube the sane source
    is an Invidious instance, which does the stream extraction
    server-side so we never have to run YouTube's obfuscated player
    JavaScript on a G3:

        https://<instance>/latest_version?id=<video id>&itag=18

    itag 18 is 360p H.264 Constrained Baseline + AAC -- exactly our
    decoder's format. On a late-gen G3, itag 36 (320x180) is the more
    realistic target; 640x360 at 30fps is roughly four times the work.

    That endpoint normally answers with a 302 to googlevideo, which is
    why redirect handling below is not optional.

    ------------------------------------------------------------------
    BUILD
    ------------------------------------------------------------------
    Classic PPC console app (Mac OS PPC / Standard Console stationery),
    NOT Carbon -- same as the registrar and OTProbe.

      - add this file, plus the whole macTLS os9/ and bearssl/ trees
      - set the project prefix file to ostls_classic_shim.h, which maps
        macTLS's Carbon-only OTOpenEndpointInContext /
        OTAsyncOpenEndpointInContext onto the classic spellings so
        macTLS compiles unmodified
      - link InterfaceLib, MathLib, MSL, OpenTransportLib,
        OpenTptInternetLib. Not CarbonLib.

    macTLS expects a global g_ostls_ot_context to exist. Under classic
    Open Transport it is meaningless but must still link, so it is
    defined below as NULL and never read.
*/

#include <stdio.h>
#include <stdlib.h>      /* strtol, used to read the HTTP status code */
#include <string.h>

#include <Types.h>
#include <Files.h>
#include <StandardFile.h>
#include <Errors.h>
#include <Memory.h>
#include <Events.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>

#include "ostls_async.h"
#include "ostls_http.h"
#include "ostls_entropy.h"   /* CollectEntropy / Load+SaveSeed */
#include "ostls_time.h"      /* GetBearSSLTime -- clock sanity check */

/* macTLS reads this global at every endpoint open. Classic OT has no
   such concept, and the shim discards the argument, so it stays NULL
   forever -- but the symbol has to exist for the link to succeed. */
OTClientContextPtr g_ostls_ot_context = NULL;

#define kMaxRedirects     5
#define kNetBufSize   16384L     /* wire bytes per Read                */
#define kOutBufSize   16384L     /* decoded bytes per write to disk    */
#define kHdrBufSize    8192L     /* response headers must fit in here  */
#define kUrlMax        1024

#define kMovieType     'MooV'
#define kMovieCreator  'TVOD'

/* Static, not stack: a 16 KB local in a classic app is asking for
   trouble, and these live for the whole fetch anyway. */
static char gNetBuf[kNetBufSize];
static char gOutBuf[kOutBufSize];
static char gHdrBuf[kHdrBufSize];
static char gReqBuf[2048];

/* ==================================================================
   URL handling
   ================================================================== */

typedef struct {
    char   scheme[8];
    char   host[256];
    UInt16 port;
    char   path[768];
} ParsedURL;

/*
    Minimal URL splitter. Deliberately strict: we only ever feed this
    URLs we or a Location header produced, not arbitrary user text, so
    it rejects rather than guesses.
*/
static int ParseURL(const char *url, ParsedURL *out)
{
    const char *p = url;
    const char *hostStart;
    const char *q;
    int n;

    memset(out, 0, sizeof(*out));

    if (strncmp(p, "https://", 8) == 0)      { strcpy(out->scheme, "https"); out->port = 443; p += 8; }
    else if (strncmp(p, "http://", 7) == 0)  { strcpy(out->scheme, "http");  out->port = 80;  p += 7; }
    else return 0;   /* no scheme we understand */

    hostStart = p;
    while (*p && *p != '/' && *p != ':') p++;
    n = (int)(p - hostStart);
    if (n <= 0 || n >= (int)sizeof(out->host)) return 0;
    memcpy(out->host, hostStart, (size_t)n);
    out->host[n] = '\0';

    if (*p == ':')
    {
        UInt32 v = 0;
        p++;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (UInt32)(*p - '0'); p++; }
        if (v == 0 || v > 65535) return 0;
        out->port = (UInt16)v;
    }

    q = (*p == '/') ? p : "/";
    if (strlen(q) >= sizeof(out->path)) return 0;
    strcpy(out->path, q);

    return 1;
}

/*
    Resolve a Location header against the URL it came from. Handles the
    three shapes that actually occur: absolute, scheme-relative (//host/…)
    and root-relative (/path). Anything else is refused rather than
    mangled.
*/
static int ResolveLocation(const ParsedURL *base, const char *loc,
                           char *out, UInt32 outCap)
{
    if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0)
    {
        if (strlen(loc) >= outCap) return 0;
        strcpy(out, loc);
        return 1;
    }
    if (loc[0] == '/' && loc[1] == '/')
    {
        if (strlen(loc) + 8 >= outCap) return 0;
        sprintf(out, "%s:%s", base->scheme, loc);
        return 1;
    }
    if (loc[0] == '/')
    {
        if (strlen(loc) + strlen(base->host) + 16 >= outCap) return 0;
        if ((strcmp(base->scheme, "https") == 0 && base->port == 443) ||
            (strcmp(base->scheme, "http")  == 0 && base->port == 80))
            sprintf(out, "%s://%s%s", base->scheme, base->host, loc);
        else
            sprintf(out, "%s://%s:%u%s", base->scheme, base->host,
                    (unsigned)base->port, loc);
        return 1;
    }
    return 0;   /* relative path without a leading slash: not worth guessing */
}

/* ==================================================================
   Response header inspection
   ================================================================== */

/* Case-insensitive search for a header line; copies its value. */
static int GetHeader(const char *headers, const char *name,
                     char *out, UInt32 outCap)
{
    const char *p = headers;
    UInt32 nameLen = (UInt32)strlen(name);

    while (*p)
    {
        const char *eol = strstr(p, "\r\n");
        if (eol == NULL) eol = p + strlen(p);

        if ((UInt32)(eol - p) > nameLen)
        {
            UInt32 i;
            int match = 1;
            for (i = 0; i < nameLen; i++)
            {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { match = 0; break; }
            }
            if (match && p[nameLen] == ':')
            {
                const char *v = p + nameLen + 1;
                UInt32 len;
                while (*v == ' ' || *v == '\t') v++;
                len = (UInt32)(eol - v);
                if (len >= outCap) len = outCap - 1;
                memcpy(out, v, len);
                out[len] = '\0';
                return 1;
            }
        }

        if (*eol == '\0') break;
        p = eol + 2;
    }
    return 0;
}

static int StatusCodeOf(const char *headers)
{
    /* "HTTP/1.1 302 Found" */
    if (strncmp(headers, "HTTP/", 5) != 0) return 0;
    while (*headers && *headers != ' ') headers++;
    while (*headers == ' ') headers++;
    return (int)strtol(headers, NULL, 10);
}

/* ==================================================================
   The ftyp patch
   ==================================================================

   The whole reason this program exists, and it is four bytes.

   We only ever see the start of the body once, so the patch is applied
   to the first 12 bytes as they stream past. If the body does not begin
   with an ftyp box we leave it completely alone -- a real .mov starts
   'moov' or 'mdat' and needs no help.
*/
static void PatchFtypPrefix(char *buf, UInt32 len, int *outPatched, int *outSawFtyp)
{
    *outPatched  = 0;
    *outSawFtyp  = 0;

    if (len < 12) return;
    if (memcmp(buf + 4, "ftyp", 4) != 0) return;

    *outSawFtyp = 1;
    if (memcmp(buf + 8, "qt  ", 4) == 0) return;   /* already fine */

    buf[8]  = 'q';
    buf[9]  = 't';
    buf[10] = ' ';
    buf[11] = ' ';
    *outPatched = 1;
}

/* ==================================================================
   One HTTPS transaction
   ==================================================================

   Returns 1 on success. On a redirect it fills nextURL and returns 2,
   leaving it to the caller to loop.
*/
static int FetchOnce(const ParsedURL *url, short fileRef,
                     char *nextURL, UInt32 nextCap,
                     UInt32 *outBytesWritten)
{
    OSTLSConfig       cfg;
    OSTLSConnection  *conn = NULL;
    OSTLSChunkDecoder chunker;
    OSErr             err;
    OSTLSEvent        ev;
    UInt32            reqLen, written = 0;
    UInt32            hdrLen = 0;
    int               headersDone = 0;
    int               isChunked = 0;
    int               requestSent = 0;
    int               sawFtyp = 0, patched = 0;
    int               prefixHandled = 0;
    UInt32            bodyBytes = 0;
    UInt32            lastReport = 0;
    int               result = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.host        = url->host;
    cfg.port        = url->port;
    cfg.server_name = url->host;

    err = OSTLS_New(&conn, &cfg);
    if (err != noErr || conn == NULL)
    {
        printf("  Could not create the TLS connection (err %d).\n", (int)err);
        return 0;
    }

    err = OSTLS_Start(conn);
    if (err != noErr)
    {
        printf("  Could not start the connection (err %d).\n", (int)err);
        OSTLS_Dispose(conn);
        return 0;
    }

    printf("  Connecting to %s:%u ...\n", url->host, (unsigned)url->port);

    OSTLS_HTTP_ChunkDecoderInit(&chunker);

    for (;;)
    {
        /* Feed the entropy pool every time round. macTLS gathers its
           noise from mouse deltas and microsecond jitter during the
           host's idle loop, and this tight fetch loop IS our idle loop
           -- there is nothing else running. Skip this and the pool
           starves, which is not the kind of bug that announces itself:
           the handshake still completes, just with less entropy behind
           the key material than anyone intended. */
        OSTLS_CollectEntropy();

        ev = kOSTLSEventNone;
        err = OSTLS_Pump(conn, 6, &ev);
        if (err != noErr)
        {
            printf("  Connection error (err %d).\n", (int)err);
            break;
        }

        if (ev == kOSTLSEventFailed)
        {
            printf("  Connection failed (last error %d, suite 0x%04X).\n",
                   (int)OSTLS_GetLastError(conn),
                   (unsigned)OSTLS_GetCipherSuite(conn));
            break;
        }

        if (ev == kOSTLSEventHandshakeDone && !requestSent)
        {
            printf("  TLS up (cipher suite 0x%04X). Requesting.\n",
                   (unsigned)OSTLS_GetCipherSuite(conn));

            reqLen = OSTLS_HTTP_FormatGet(gReqBuf, sizeof(gReqBuf),
                                          url->host, url->path);
            if (reqLen == 0)
            {
                printf("  Request did not fit in the buffer (path too long?).\n");
                break;
            }
            err = OSTLS_Write(conn, gReqBuf, reqLen, &written);
            if (err != noErr)
            {
                printf("  Could not send the request (err %d).\n", (int)err);
                break;
            }
            requestSent = 1;
        }

        /* Drain everything readable before pumping again, as the async
           API documentation asks. */
        if (ev == kOSTLSEventReadable)
        {
            for (;;)
            {
                UInt32 got = 0;

                err = OSTLS_Read(conn, gNetBuf, sizeof(gNetBuf), &got);
                if (err != noErr) break;
                if (got == 0) break;

                {
                    char  *src = gNetBuf;
                    UInt32 left = got;

                    /* ---- still collecting headers? ---- */
                    if (!headersDone)
                    {
                        UInt32 take = left;
                        char  *blank;

                        if (hdrLen + take > kHdrBufSize - 1)
                            take = (kHdrBufSize - 1) - hdrLen;
                        memcpy(gHdrBuf + hdrLen, src, take);
                        hdrLen += take;
                        gHdrBuf[hdrLen] = '\0';

                        blank = strstr(gHdrBuf, "\r\n\r\n");
                        if (blank == NULL)
                        {
                            if (hdrLen >= kHdrBufSize - 1)
                            {
                                printf("  Response headers too large.\n");
                                goto done;
                            }
                            continue;   /* need more */
                        }

                        headersDone = 1;
                        {
                            UInt32 hdrUsed = (UInt32)(blank - gHdrBuf) + 4;
                            UInt32 leftover = hdrLen - hdrUsed;
                            int    status;
                            char   val[512];

                            *blank = '\0';       /* terminate the header block */
                            status = StatusCodeOf(gHdrBuf);
                            printf("  HTTP %d\n", status);

                            if (status >= 300 && status < 400 &&
                                GetHeader(gHdrBuf, "Location", val, sizeof(val)))
                            {
                                if (ResolveLocation(url, val, nextURL, nextCap))
                                {
                                    printf("  Redirected.\n");
                                    result = 2;
                                    goto done;
                                }
                                printf("  Could not understand the redirect: %s\n", val);
                                goto done;
                            }

                            if (status != 200 && status != 206)
                            {
                                printf("  Server refused (HTTP %d).\n", status);
                                goto done;
                            }

                            if (GetHeader(gHdrBuf, "Content-Type", val, sizeof(val)))
                                printf("  Content-Type: %s\n", val);
                            if (GetHeader(gHdrBuf, "Content-Length", val, sizeof(val)))
                                printf("  Content-Length: %s\n", val);
                            if (GetHeader(gHdrBuf, "Transfer-Encoding", val, sizeof(val)))
                            {
                                if (strstr(val, "chunked") != NULL) isChunked = 1;
                            }

                            /* Body bytes that arrived in the same read as
                               the headers. Shuffle them to the front. */
                            if (leftover > 0)
                                memmove(gNetBuf, blank + 4, leftover);
                            src  = gNetBuf;
                            left = leftover;
                        }
                    }

                    /* ---- body ---- */
                    while (left > 0)
                    {
                        char  *payload;
                        UInt32 payloadLen;
                        UInt32 consumed = 0;

                        if (isChunked)
                        {
                            UInt32 decoded = 0;
                            err = OSTLS_HTTP_ChunkDecoderProcess(
                                      &chunker, src, left,
                                      gOutBuf, sizeof(gOutBuf),
                                      &decoded, &consumed);
                            if (err != noErr)
                            {
                                printf("  Chunked decode failed (err %d).\n", (int)err);
                                goto done;
                            }
                            if (consumed == 0 && decoded == 0) break;
                            payload    = gOutBuf;
                            payloadLen = decoded;
                        }
                        else
                        {
                            payload    = src;
                            payloadLen = left;
                            consumed   = left;
                        }

                        if (payloadLen > 0)
                        {
                            /* The four bytes. Only the very first bytes of
                               the body can carry an ftyp box, so this runs
                               once and then never again. */
                            if (!prefixHandled)
                            {
                                PatchFtypPrefix(payload, payloadLen,
                                                &patched, &sawFtyp);
                                if (payloadLen >= 12)
                                {
                                    prefixHandled = 1;
                                    if (sawFtyp && patched)
                                        printf("  ftyp found -- brand patched to 'qt  '.\n");
                                    else if (sawFtyp)
                                        printf("  ftyp found -- already 'qt  '.\n");
                                    else
                                        printf("  No ftyp box; leaving the stream untouched.\n");
                                }
                            }

                            {
                                long count = (long)payloadLen;
                                OSErr werr = FSWrite(fileRef, &count, (Ptr)payload);
                                if (werr != noErr)
                                {
                                    printf("  Disk write failed (err %d).\n", (int)werr);
                                    goto done;
                                }
                            }
                            bodyBytes += payloadLen;

                            if (bodyBytes - lastReport >= 262144UL)
                            {
                                lastReport = bodyBytes;
                                printf("    %lu KB\n", (unsigned long)(bodyBytes / 1024));
                                fflush(stdout);
                            }
                        }

                        src  += consumed;
                        left -= consumed;

                        if (isChunked && chunker.state == kOSTLSChunkStateDone)
                        {
                            left = 0;
                            break;
                        }
                    }
                }
            }
        }

        if (ev == kOSTLSEventClosed)
        {
            if (headersDone) result = 1;
            break;
        }
    }

done:
    OSTLS_Close(conn);
    OSTLS_Dispose(conn);

    if (outBytesWritten) *outBytesWritten = bodyBytes;
    if (result == 1 && bodyBytes == 0) result = 0;
    return result;
}

/* ==================================================================
   Driver
   ================================================================== */

static void FetchToFile(const char *startURL, const FSSpec *spec)
{
    char      url[kUrlMax];
    char      next[kUrlMax];
    ParsedURL parsed;
    short     fileRef = 0;
    OSErr     err;
    int       hop;
    UInt32    bytes = 0;
    int       ok = 0;

    strncpy(url, startURL, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';

    /* Create fresh: a half-written movie from a previous attempt is
       worse than no movie. */
    (void)FSpDelete(spec);
    err = FSpCreate(spec, kMovieCreator, kMovieType, 0);
    if (err != noErr && err != dupFNErr)
    {
        printf("  Could not create the file (err %d).\n", (int)err);
        return;
    }
    err = FSpOpenDF(spec, fsRdWrPerm, &fileRef);
    if (err != noErr)
    {
        printf("  Could not open the file for writing (err %d).\n", (int)err);
        return;
    }

    for (hop = 0; hop < kMaxRedirects; hop++)
    {
        int r;

        if (!ParseURL(url, &parsed))
        {
            printf("  That URL is not one I can parse: %s\n", url);
            break;
        }
        if (strcmp(parsed.scheme, "https") != 0)
        {
            printf("  Only https:// is supported -- macTLS is a TLS stack.\n");
            printf("  (A plain http source can be fetched with Classilla.)\n");
            break;
        }

        r = FetchOnce(&parsed, fileRef, next, sizeof(next), &bytes);
        if (r == 1) { ok = 1; break; }
        if (r != 2) break;

        /* Redirected: start the file again from empty. */
        (void)SetFPos(fileRef, fsFromStart, 0);
        (void)SetEOF(fileRef, 0);
        strcpy(url, next);
    }

    FSClose(fileRef);
    FlushVol(NULL, spec->vRefNum);

    if (!ok)
    {
        printf("\n  Fetch did not complete. Leaving the partial file in place\n");
        printf("  so you can look at it; delete it when you are done.\n");
        return;
    }

    /* Make sure the Finder agrees it is a movie. FSpCreate already set
       the type, but a redirect or a re-run may have disturbed it. */
    {
        FInfo fi;
        if (FSpGetFInfo(spec, &fi) == noErr)
        {
            fi.fdType    = kMovieType;
            fi.fdCreator = kMovieCreator;
            (void)FSpSetFInfo(spec, &fi);
        }
    }

    printf("\n  *** Done -- %lu KB written. ***\n",
           (unsigned long)(bytes / 1024));
    printf("  Open it in QuickTime Player.\n");
}

int main(void)
{
    OSStatus oterr;

    printf("================================================================\n");
    printf("GetMovie -- fetch an HTTPS movie and make QuickTime play it\n");
    printf("================================================================\n");
    printf("\n");
    printf("Fetches over TLS with macTLS, patches the ftyp major brand to\n");
    printf("'qt  ' as the bytes go past, and writes a QuickTime movie.\n");
    printf("\n");
    printf("For YouTube, point it at an Invidious instance:\n");
    printf("  https://<instance>/latest_version?id=<video id>&itag=18\n");
    printf("itag 18 is 360p Baseline + AAC. On a late-gen G3 prefer itag 36\n");
    printf("(320x180) -- 640x360 at 30fps is about four times the work.\n");
    printf("\n");

    oterr = InitOpenTransport();
    if (oterr != kOTNoError)
    {
        printf("InitOpenTransport failed (err %d). Is TCP/IP configured?\n",
               (int)oterr);
        printf("\nPress return to quit. ");
        (void)getchar();
        return 1;
    }

    /* Restore whatever entropy we banked last run, and start gathering
       more immediately -- the pool wants to be warm before the first
       handshake, not during it. */
    OSTLS_LoadSeed();
    {
        int i;
        for (i = 0; i < 256; i++) OSTLS_CollectEntropy();
    }

    /* A vintage Mac with a flat PRAM battery is the single most likely
       way this fails, and the error it produces otherwise
       (kOSTLSAsync_ClockBefore2000, 2002) is not self-explanatory.
       Certificate validity is checked against this clock, so a wrong
       date means every site on the internet looks expired or not yet
       valid. Say so plainly and up front. */
    {
        UInt32 days = 0, secs = 0;
        if (OSTLS_GetBearSSLTime(&days, &secs) != noErr || days == 0)
        {
            printf("!! The Mac's clock looks wrong or unset.\n");
            printf("!! TLS validates certificate dates against it, so every\n");
            printf("!! fetch will fail until the date and time are correct.\n");
            printf("!! Set them in the Date & Time control panel first.\n\n");
        }
    }

    for (;;)
    {
        char url[kUrlMax];
        StandardFileReply reply;
        Str255 suggested;
        int n;

        printf("----------------------------------------------------------\n");
        printf("URL (blank line to quit):\n> ");
        fflush(stdout);

        if (fgets(url, sizeof(url), stdin) == NULL) break;

        /* strip trailing newline and any stray whitespace */
        n = (int)strlen(url);
        while (n > 0 && (url[n-1] == '\n' || url[n-1] == '\r' ||
                         url[n-1] == ' '  || url[n-1] == '\t')) url[--n] = '\0';
        if (n == 0) break;

        /* Suggest a name. Anything sensible will do -- the extension is
           what routes QuickTime to the MOV parser, so it must end .mov. */
        strcpy((char *)suggested + 1, "downloaded.mov");
        suggested[0] = (unsigned char)strlen("downloaded.mov");

        StandardPutFile("\pSave the movie as:", suggested, &reply);
        if (!reply.sfGood)
        {
            printf("  Cancelled.\n\n");
            continue;
        }

        /* Warn if the chosen name will not route to the MOV parser.
           This is the difference between a playing movie and -8971. */
        {
            int len = reply.sfFile.name[0];
            int bad = 1;
            if (len >= 4)
            {
                char a = (char)reply.sfFile.name[len-3];
                char b = (char)reply.sfFile.name[len-2];
                char c = (char)reply.sfFile.name[len-1];
                char d = (char)reply.sfFile.name[len-0];
                if (a == '.' &&
                    (b == 'm' || b == 'M') &&
                    (c == 'o' || c == 'O') &&
                    (d == 'v' || d == 'V')) bad = 0;
            }
            if (bad)
                printf("  Note: the name does not end in .mov. The file type is\n"
                       "  set to MooV anyway, which is enough on a local disk.\n");
        }

        printf("\n");
        FetchToFile(url, &reply.sfFile);
        printf("\n");
    }

    /* Bank the entropy pool so the next run starts warm rather than
       cold. On a machine with no OS random source this is most of what
       makes the second launch better than the first. */
    OSTLS_SaveSeed();

    CloseOpenTransport();
    printf("\nBye.\n");
    return 0;
}
