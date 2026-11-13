/*
    QTProxy.c  --  a local HTTP proxy that makes MP4 streams playable
                   in QuickTime on Mac OS 9

    ------------------------------------------------------------------
    What it is for
    ------------------------------------------------------------------
    QuickTime 6 has an HTTP proxy setting -- QuickTime Settings control
    panel, Streaming Proxy pane, "HTTP Proxy Server" plus a Port ID
    field. Point it at this program and every movie QuickTime fetches
    comes through here first.

    That gives us a place to stand. On the way past we do the one thing
    that turns error -8971 into a playing movie: patch the ftyp major
    brand to 'qt  ', and label the response as a QuickTime movie so
    QuickTime routes it to its MOV parser rather than its MPEG-4
    importer. The MOV parser copies every sample-description extension
    through verbatim -- including the 'avcC' box carrying the SPS/PPS our
    decoder needs -- which the MPEG-4 importer, predating H.264 entirely,
    silently drops.

    Four bytes and a Content-Type header. That is the whole trick.

    ------------------------------------------------------------------
    This version is plain HTTP only, deliberately
    ------------------------------------------------------------------
    No TLS, no crypto, no BearSSL, nothing to link but Open Transport.
    That is not a limitation for the first target: S60Tube serves over
    plain http://, so this alone is enough to make it stream.

    HTTPS comes later by dropping macTLS in behind the same structure --
    see GetMovie.c, which already drives macTLS's async API, and
    ostls_classic_shim.h, which lets it compile into a classic
    non-Carbon app. Keeping v1 crypto-free means the listener half can
    be proven on its own, which matters given what follows.

    ------------------------------------------------------------------
    The bind problem, and why this program works either way
    ------------------------------------------------------------------
    macTLS's team established, over fourteen rounds on real hardware,
    that a CARBON application cannot OTBind to a port of its choosing:
    kOTBadAddressErr (-3150) for every combination they tried. Only
    OTBind with a NULL address works, with Open Transport picking an
    ephemeral port. They abandoned this exact architecture because of it.

    Their own research contains the reason to hope: Apple's pre-Carbon
    HTTP Server sample binds successfully, "because the sample is
    pre-Carbon and doesn't go through CarbonLib's OT wrapper", and plain
    OTOpenEndpoint is not even exported by CarbonLib. We are not Carbon,
    so the question is open for us. OTProbe.c settles it.

    This program does not wait for that answer. It tries the fixed port
    first; if that is refused it falls back to letting Open Transport
    assign one and prints it in large friendly letters for you to type
    into the control panel. So:

      - if the classic path can bind, you configure QuickTime once
      - if it cannot, you retype a port number at each launch

    Clunky in the second case, but working in both, and the program
    tells you which world you are in.

    ------------------------------------------------------------------
    BUILD
    ------------------------------------------------------------------
    Classic PPC console app (Mac OS PPC / Standard Console stationery),
    NOT Carbon. Link InterfaceLib, MathLib, MSL, OpenTransportLib,
    OpenTptInternetLib. No prefix file needed -- there is no macTLS here
    yet.

    ------------------------------------------------------------------
    Scope and honesty about it
    ------------------------------------------------------------------
    One connection at a time, synchronous, HTTP/1.0-style: read the
    request, open upstream, relay until the server closes. QuickTime
    fetches a movie with a single GET, so serialising is fine and keeps
    this readable. It is emphatically not a general-purpose proxy --
    no CONNECT, no keep-alive, no caching, no concurrency.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Types.h>
#include <Errors.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>

#define kPreferredPort   8080
#define kBufSize         16384L
#define kReqMax          4096
#define kHdrMax          8192

#ifndef kOTBadAddressErr
#define kOTBadAddressErr (-3150)
#endif

static char gReq[kReqMax];
static char gBuf[kBufSize];
static char gHdr[kHdrMax];

/* ==================================================================
   Tiny helpers
   ================================================================== */

static void PrintIP(UInt32 ip)
{
    printf("%lu.%lu.%lu.%lu",
           (unsigned long)((ip >> 24) & 0xFF),
           (unsigned long)((ip >> 16) & 0xFF),
           (unsigned long)((ip >>  8) & 0xFF),
           (unsigned long)((ip      ) & 0xFF));
}

static int CaseEq(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++)
    {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

/* ==================================================================
   Request parsing

   A proxy receives the ABSOLUTE form of the request line:

       GET http://host:port/path HTTP/1.1

   which is convenient -- the host is right there and we do not have to
   go hunting in the Host: header.
   ================================================================== */

typedef struct {
    char   host[256];
    UInt16 port;
    char   path[1024];
} ProxyTarget;

static int ParseProxyRequest(const char *req, ProxyTarget *out)
{
    const char *p = req;
    const char *hostStart;
    const char *pathStart;
    int n;

    memset(out, 0, sizeof(*out));
    out->port = 80;

    if (!CaseEq(p, "GET ", 4)) return 0;
    p += 4;
    while (*p == ' ') p++;

    if (!CaseEq(p, "http://", 7))
    {
        /* Not an absolute URL. Either QuickTime is not actually
           configured to use us as a proxy, or it asked for https --
           which this version cannot do. */
        return 0;
    }
    p += 7;

    hostStart = p;
    while (*p && *p != '/' && *p != ':' && *p != ' ') p++;
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

    pathStart = p;
    while (*p && *p != ' ') p++;
    n = (int)(p - pathStart);
    if (n <= 0) { strcpy(out->path, "/"); return 1; }
    if (n >= (int)sizeof(out->path)) return 0;
    memcpy(out->path, pathStart, (size_t)n);
    out->path[n] = '\0';

    return 1;
}

/* ==================================================================
   The ftyp patch

   Identical to the one in GetMovie.c and MP4Fix.c. Only the first
   twelve bytes of the body can carry an ftyp box, so this runs once per
   response and then never again.
   ================================================================== */

/*
    Two levers, both four bytes, both leaving every offset in the file
    untouched. Flip kPatchMode to choose.

      1  (default, proven on hardware)
         brand at offset 8 -> 'qt  '. The MOV parser accepts a leading
         ftyp box branded 'qt  ' and rejects one branded 'isom'.

      2  (alternative, structurally verified, untried on the Mac)
         atom type at offset 4 -> 'free'. The box becomes padding that
         every parser skips, so the stream reads as free/moov -- an
         ordinary QuickTime layout with no ftyp to have an opinion
         about. Worth reaching for if mode 1 streams still fail.

    What NOT to do: delete the ftyp box. The sample tables inside moov
    hold absolute file offsets into mdat, so dropping bytes from the
    front leaves every one of them pointing 32 bytes short. Measured on
    a real file: stco stays 3327 while mdat moves from 3319 to 3287, and
    a decoder reads garbage NAL lengths. Renaming rather than removing
    is precisely what keeps mode 2 safe.
*/
#define kPatchMode 1

static int PatchFtypPrefix(char *buf, UInt32 len)
{
    if (len < 12) return 0;
    if (memcmp(buf + 4, "ftyp", 4) != 0) return 0;
#if kPatchMode == 1
    if (memcmp(buf + 8, "qt  ", 4) == 0) return 2;   /* already right */
    buf[8] = 'q'; buf[9] = 't'; buf[10] = ' '; buf[11] = ' ';
#else
    buf[4] = 'f'; buf[5] = 'r'; buf[6] = 'e'; buf[7] = 'e';
#endif
    return 1;
}

/* ==================================================================
   Response header rewriting

   Two changes on the way back to QuickTime:

     Content-Type  ->  video/quicktime
         This is the routing decision. On disk the file extension picks
         the importer; over the wire it must be the Content-Type, since
         a URL has no extension. Proving that is one of the open
         questions -- see the note at the end of this file.

     Location:     https -> http
         If the origin redirects to an https URL, QuickTime cannot
         follow it: no TLS, ever. Rewriting the scheme sends it back
         through us, and a future macTLS-enabled build can then make the
         secure connection on its behalf. In this plain-HTTP version the
         rewrite will fail at the next hop, but leaving it unrewritten
         fails too and more confusingly.
   ================================================================== */

static UInt32 RewriteHeaders(const char *in, UInt32 inLen, char *out, UInt32 outCap)
{
    const char *p   = in;
    const char *end = in + inLen;
    UInt32      w   = 0;
    int         wroteType = 0;

    while (p < end)
    {
        const char *eol = p;
        UInt32 lineLen;

        while (eol < end && !(eol[0] == '\r' && eol + 1 < end && eol[1] == '\n')) eol++;
        lineLen = (UInt32)(eol - p);

        if (lineLen == 0) break;    /* blank line: end of headers */

        if (lineLen > 13 && CaseEq(p, "Content-Type:", 13))
        {
            const char *repl = "Content-Type: video/quicktime";
            UInt32 rl = (UInt32)strlen(repl);
            if (w + rl + 2 >= outCap) return 0;
            memcpy(out + w, repl, rl); w += rl;
            out[w++] = '\r'; out[w++] = '\n';
            wroteType = 1;
        }
        else if (lineLen > 9 && CaseEq(p, "Location:", 9))
        {
            /* copy the line, downgrading an https:// scheme to http:// */
            const char *v = p + 9;
            UInt32 vlen;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            vlen = (UInt32)(eol - v);

            if (w + vlen + 32 >= outCap) return 0;
            memcpy(out + w, "Location: ", 10); w += 10;
            if (vlen > 8 && CaseEq(v, "https://", 8))
            {
                memcpy(out + w, "http://", 7); w += 7;
                memcpy(out + w, v + 8, vlen - 8); w += vlen - 8;
                printf("    (rewrote an https redirect to http)\n");
            }
            else
            {
                memcpy(out + w, v, vlen); w += vlen;
            }
            out[w++] = '\r'; out[w++] = '\n';
        }
        else
        {
            /* Drop hop-by-hop and range-related headers we cannot honour
               honestly, pass everything else through untouched. */
            if (!(lineLen > 18 && CaseEq(p, "Transfer-Encoding:", 18)))
            {
                if (w + lineLen + 2 >= outCap) return 0;
                memcpy(out + w, p, lineLen); w += lineLen;
                out[w++] = '\r'; out[w++] = '\n';
            }
        }

        p = eol + 2;
        if (p >= end) break;
    }

    if (!wroteType)
    {
        const char *repl = "Content-Type: video/quicktime\r\n";
        UInt32 rl = (UInt32)strlen(repl);
        if (w + rl >= outCap) return 0;
        memcpy(out + w, repl, rl); w += rl;
    }

    if (w + 2 >= outCap) return 0;
    out[w++] = '\r'; out[w++] = '\n';   /* end of headers */
    return w;
}

/* ==================================================================
   Open Transport plumbing
   ================================================================== */

/* Bind a listening endpoint. Tries the port we want, then falls back to
   whatever Open Transport will give us. Returns the bound port, or 0. */
static UInt16 BindListener(EndpointRef ep, UInt16 wantPort, int *outWasPreferred)
{
    TBind       req, ret;
    InetAddress reqAddr, retAddr;
    OSStatus    err;

    *outWasPreferred = 0;

    /* --- first choice: the port we actually want --- */
    memset(&req, 0, sizeof(req));
    memset(&ret, 0, sizeof(ret));
    OTInitInetAddress(&reqAddr, wantPort, (InetHost)0);   /* INADDR_ANY */
    req.addr.buf = (UInt8 *)&reqAddr;
    req.addr.len = sizeof(reqAddr);
    req.addr.maxlen = sizeof(reqAddr);
    req.qlen = 1;
    ret.addr.buf = (UInt8 *)&retAddr;
    ret.addr.maxlen = sizeof(retAddr);

    err = OTBind(ep, &req, &ret);
    if (err == kOTNoError)
    {
        *outWasPreferred = 1;
        return wantPort;
    }

    printf("  Could not bind port %u (err %d%s).\n",
           (unsigned)wantPort, (int)err,
           (err == kOTBadAddressErr) ? " = kOTBadAddressErr, the Carbon symptom" : "");
    printf("  Falling back to an Open-Transport-assigned port.\n");

    /* --- fallback: let OT choose --- */
    memset(&req, 0, sizeof(req));
    memset(&ret, 0, sizeof(ret));
    memset(&retAddr, 0, sizeof(retAddr));
    req.addr.buf = NULL;
    req.addr.len = 0;
    req.addr.maxlen = 0;
    req.qlen = 1;
    ret.addr.buf = (UInt8 *)&retAddr;
    ret.addr.maxlen = sizeof(retAddr);

    err = OTBind(ep, &req, &ret);
    if (err != kOTNoError)
    {
        printf("  Fallback bind also failed (err %d). Cannot listen at all.\n",
               (int)err);
        return 0;
    }
    return retAddr.fPort;
}

/* Connect outbound to host:port. Returns an endpoint or NULL. */
static EndpointRef ConnectUpstream(const char *host, UInt16 port)
{
    EndpointRef  ep;
    OSStatus     err;
    TCall        call;
    DNSAddress   dns;
    char         hostPort[320];
    long         dnsLen;   /* OTInitDNSAddress's return; signed so a
                              zero/negative result is detectable */

    ep = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, &err);
    if (ep == kOTInvalidEndpointRef || err != kOTNoError)
    {
        printf("    upstream OTOpenEndpoint failed (err %d)\n", (int)err);
        return NULL;
    }
    (void)OTSetSynchronous(ep);
    (void)OTSetBlocking(ep);

    err = OTBind(ep, NULL, NULL);      /* outbound: NULL bind always works */
    if (err != kOTNoError)
    {
        printf("    upstream OTBind failed (err %d)\n", (int)err);
        OTCloseProvider(ep);
        return NULL;
    }

    sprintf(hostPort, "%s:%u", host, (unsigned)port);

    /* OTInitDNSAddress RETURNS the address length to put in
       TCall.addr.len -- it is not something to compute by hand from
       sizeof(DNSAddress). Getting this wrong is a classic Open Transport
       trap: the connect either fails obscurely or reads past the end of
       the name. macTLS's own B1 stage does exactly this and notes that a
       zero return is suspect even though the call is documented not to
       fail. DNS is not resolved here -- that happens inside OTConnect
       when the TCP module hands the address to the resolver. */
    memset(&call, 0, sizeof(call));
    dnsLen = OTInitDNSAddress(&dns, hostPort);
    if (dnsLen <= 0)
    {
        printf("    OTInitDNSAddress returned %ld for \"%s\"\n",
               (long)dnsLen, hostPort);
        OTCloseProvider(ep);
        return NULL;
    }
    call.addr.buf = (UInt8 *)&dns;
    call.addr.len = (ByteCount)dnsLen;

    err = OTConnect(ep, &call, NULL);
    if (err != kOTNoError)
    {
        printf("    could not connect to %s (err %d)\n", hostPort, (int)err);
        OTCloseProvider(ep);
        return NULL;
    }
    return ep;
}

/* ==================================================================
   Serve exactly one proxied request on an accepted endpoint
   ================================================================== */

static void ServeOne(EndpointRef client)
{
    ProxyTarget tgt;
    EndpointRef up = NULL;
    OTResult    n;
    OTFlags     flags;
    UInt32      reqLen = 0;
    UInt32      hdrLen = 0;
    int         headersDone = 0;
    int         prefixHandled = 0;
    UInt32      bodyBytes = 0;

    /* ---- read the request (headers only; GET has no body) ---- */
    for (;;)
    {
        n = OTRcv(client, gReq + reqLen, (OTByteCount)(kReqMax - 1 - reqLen), &flags);
        if (n <= 0) break;
        reqLen += (UInt32)n;
        gReq[reqLen] = '\0';
        if (strstr(gReq, "\r\n\r\n") != NULL) break;
        if (reqLen >= kReqMax - 1) break;
    }

    if (reqLen == 0) return;

    if (!ParseProxyRequest(gReq, &tgt))
    {
        const char *msg =
            "HTTP/1.0 501 Not Implemented\r\n"
            "Content-Type: text/plain\r\n\r\n"
            "QTProxy handles plain http:// proxy GETs only.\r\n";
        (void)OTSnd(client, (void *)msg, (OTByteCount)strlen(msg), 0);
        printf("  Request I could not handle:\n    %.80s\n", gReq);
        return;
    }

    printf("  -> %s:%u%s\n", tgt.host, (unsigned)tgt.port, tgt.path);

    up = ConnectUpstream(tgt.host, tgt.port);
    if (up == NULL)
    {
        const char *msg =
            "HTTP/1.0 502 Bad Gateway\r\n"
            "Content-Type: text/plain\r\n\r\n"
            "QTProxy could not reach the origin server.\r\n";
        (void)OTSnd(client, (void *)msg, (OTByteCount)strlen(msg), 0);
        return;
    }

    /* ---- forward a clean origin-form request ---- */
    {
        int len = sprintf(gBuf,
            "GET %s HTTP/1.0\r\n"
            "Host: %s\r\n"
            "User-Agent: QTProxy/1.0 (Mac OS 9)\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n"
            "\r\n",
            tgt.path, tgt.host);
        (void)OTSnd(up, gBuf, (OTByteCount)len, 0);
    }

    /* ---- relay the response, patching as it goes ---- */
    for (;;)
    {
        n = OTRcv(up, gBuf, (OTByteCount)kBufSize, &flags);
        if (n <= 0)
        {
            /* Open Transport does not simply return 0 at end of stream.
               A negative result may be an EVENT that has to be
               acknowledged: kOTLookErr means "something happened, call
               OTLook to find out what". For us that is nearly always the
               origin closing its half after sending the whole body --
               which, with Connection: close and no Content-Length, is
               precisely how an HTTP/1.0 response signals that it is
               finished.

               OT delivers any buffered data before raising the event, so
               by the time we see this we have already relayed the last
               real bytes. Acknowledging it lets the endpoint close
               cleanly instead of being torn down under OT's feet. */
            if (n == kOTLookErr)
            {
                OTResult look = OTLook(up);
                if (look == T_ORDREL)
                    (void)OTRcvOrderlyDisconnect(up);
                else if (look == T_DISCONNECT)
                    (void)OTRcvDisconnect(up, NULL);
            }
            break;
        }

        {
            char  *src  = gBuf;
            UInt32 left = (UInt32)n;

            if (!headersDone)
            {
                UInt32 take = left;
                char  *blank;

                if (hdrLen + take > kHdrMax - 1) take = (kHdrMax - 1) - hdrLen;
                memcpy(gHdr + hdrLen, src, take);
                hdrLen += take;
                gHdr[hdrLen] = '\0';

                blank = strstr(gHdr, "\r\n\r\n");
                if (blank == NULL) continue;      /* need more header bytes */

                headersDone = 1;
                {
                    UInt32 used     = (UInt32)(blank - gHdr) + 4;
                    UInt32 leftover = hdrLen - used;
                    char   statusLine[64];
                    char   rewritten[kHdrMax];
                    UInt32 rlen;
                    const char *firstEol = strstr(gHdr, "\r\n");
                    UInt32 slen = firstEol ? (UInt32)(firstEol - gHdr) : 0;

                    if (slen > 0 && slen < sizeof(statusLine) - 3)
                    {
                        memcpy(statusLine, gHdr, slen);
                        statusLine[slen] = '\0';
                        printf("    %s\n", statusLine);
                    }
                    else
                    {
                        strcpy(statusLine, "HTTP/1.0 200 OK");
                    }

                    /* status line, then the rewritten header block */
                    rlen = RewriteHeaders(firstEol ? firstEol + 2 : gHdr,
                                          firstEol ? (used - slen - 2) : used,
                                          rewritten, sizeof(rewritten));
                    if (rlen == 0)
                    {
                        printf("    header rewrite overflowed; passing through\n");
                        (void)OTSnd(client, gHdr, (OTByteCount)used, 0);
                    }
                    else
                    {
                        (void)OTSnd(client, statusLine, (OTByteCount)strlen(statusLine), 0);
                        (void)OTSnd(client, "\r\n", 2, 0);
                        (void)OTSnd(client, rewritten, (OTByteCount)rlen, 0);
                    }

                    if (leftover > 0) memmove(gBuf, blank + 4, leftover);
                    src  = gBuf;
                    left = leftover;
                }
            }

            if (left > 0)
            {
                if (!prefixHandled)
                {
                    int r = PatchFtypPrefix(src, left);
                    if (left >= 12)
                    {
                        prefixHandled = 1;
                        if (r == 1)      printf("    ftyp patched -> 'qt  '\n");
                        else if (r == 2) printf("    ftyp already 'qt  '\n");
                        else             printf("    no ftyp box; passing through untouched\n");
                    }
                }
                (void)OTSnd(client, src, (OTByteCount)left, 0);
                bodyBytes += left;
            }
        }
    }

    printf("    relayed %lu KB\n", (unsigned long)(bodyBytes / 1024));

    (void)OTSndOrderlyDisconnect(client);
    OTCloseProvider(up);
}

/* ==================================================================
   Main
   ================================================================== */

int main(void)
{
    OSStatus    err;
    EndpointRef listener;
    UInt16      boundPort;
    int         wasPreferred = 0;

    printf("================================================================\n");
    printf("QTProxy -- local HTTP proxy that makes MP4 playable in QuickTime\n");
    printf("================================================================\n");
    printf("\n");
    printf("Patches the ftyp major brand to 'qt  ' and labels responses as\n");
    printf("video/quicktime, so QuickTime uses its MOV parser -- which keeps\n");
    printf("the 'avcC' box our H.264 decoder needs. Its MPEG-4 importer\n");
    printf("drops it, which is the -8971 you get without this.\n");
    printf("\n");
    printf("Plain http:// only in this version. No TLS yet.\n\n");

#if defined(TARGET_API_MAC_CARBON) && TARGET_API_MAC_CARBON
    printf("!! Built against Carbon. The fixed-port bind will fail (-3150).\n");
    printf("!! Rebuild from Mac OS PPC / Standard Console stationery.\n\n");
#endif

    err = InitOpenTransport();
    if (err != kOTNoError)
    {
        printf("InitOpenTransport failed (err %d). Is TCP/IP configured?\n", (int)err);
        printf("\nPress return to quit. ");
        (void)getchar();
        return 1;
    }

    listener = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, &err);
    if (listener == kOTInvalidEndpointRef || err != kOTNoError)
    {
        printf("OTOpenEndpoint failed (err %d)\n", (int)err);
        CloseOpenTransport();
        printf("\nPress return to quit. ");
        (void)getchar();
        return 1;
    }
    (void)OTSetSynchronous(listener);
    (void)OTSetBlocking(listener);

    boundPort = BindListener(listener, kPreferredPort, &wasPreferred);
    if (boundPort == 0)
    {
        OTCloseProvider(listener);
        CloseOpenTransport();
        printf("\nPress return to quit. ");
        (void)getchar();
        return 1;
    }

    printf("\n================================================================\n");
    if (wasPreferred)
    {
        printf("  Listening on 127.0.0.1:%u  (the port we asked for)\n",
               (unsigned)boundPort);
        printf("\n  *** The classic Open Transport path CAN bind a chosen\n");
        printf("      port. That is a result worth recording -- macTLS\n");
        printf("      concluded it was impossible from Carbon. ***\n");
    }
    else
    {
        printf("  Listening on 127.0.0.1:%u  (assigned by Open Transport)\n",
               (unsigned)boundPort);
        printf("\n  The chosen port was refused, same as Carbon. This number\n");
        printf("  will change at every launch, so it has to be retyped.\n");
    }
    printf("\n  Set this in QuickTime Settings -> Streaming Proxy:\n");
    printf("      HTTP Proxy Server:  127.0.0.1\n");
    printf("      Port ID:            %u\n", (unsigned)boundPort);
    printf("  and untick \"Use System Settings\" so the fields are editable.\n");
    printf("================================================================\n\n");
    printf("Waiting for QuickTime. Press command-period to stop.\n\n");

    for (;;)
    {
        TCall       call;
        InetAddress peer;
        EndpointRef worker;

        memset(&call, 0, sizeof(call));
        call.addr.buf    = (UInt8 *)&peer;
        call.addr.maxlen = sizeof(peer);

        err = OTListen(listener, &call);
        if (err != kOTNoError)
        {
            if (err == kOTNoDataErr) continue;
            printf("OTListen failed (err %d). Stopping.\n", (int)err);
            break;
        }

        printf("Connection from ");
        PrintIP(peer.fHost);
        printf(":%u\n", (unsigned)peer.fPort);

        /* Classic OT accept: the accepting endpoint must be a SECOND,
           separately opened and bound endpoint. Handing OTAccept the
           listener itself would tie up the listening socket. */
        worker = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, &err);
        if (worker == kOTInvalidEndpointRef || err != kOTNoError)
        {
            printf("  could not open a worker endpoint (err %d)\n", (int)err);
            continue;
        }
        (void)OTSetSynchronous(worker);
        (void)OTSetBlocking(worker);
        (void)OTBind(worker, NULL, NULL);

        err = OTAccept(listener, worker, &call);
        if (err != kOTNoError)
        {
            printf("  OTAccept failed (err %d)\n", (int)err);
            OTCloseProvider(worker);
            continue;
        }

        ServeOne(worker);
        OTCloseProvider(worker);
        printf("\n");
    }

    OTCloseProvider(listener);
    CloseOpenTransport();
    return 0;
}

/*
    ------------------------------------------------------------------
    The open question this program is built to answer
    ------------------------------------------------------------------
    On disk we proved, across six probes of one identical bitstream, that
    the FILE EXTENSION chooses QuickTime's importer and the ftyp BRAND
    decides whether that importer accepts the file. Both are needed:
    patching the brand while leaving a .mp4 name still fails with -8971,
    and renaming to .mov without patching the brand fails differently.

    Over the wire there is no extension, so something else must do the
    routing -- almost certainly the Content-Type. That is why this proxy
    forces video/quicktime. But it is a hypothesis, not a measurement:
    we have never had a working stream to test it against.

    If a stream still fails with -8971 while carrying video/quicktime and
    a patched brand, then QuickTime is sniffing content rather than
    trusting the header, and the next thing to try is making the response
    look even more like a classic movie. Set kPatchMode to 2, which
    renames the ftyp box to 'free' so the stream reads as free/moov with
    no ftyp in it at all.

    Note carefully what mode 2 does NOT do: it does not delete the box.
    Deleting it is the obvious idea and it corrupts the file. The sample
    tables inside moov (stco, or co64 on large files) hold ABSOLUTE file
    offsets into mdat, so removing a 32-byte box from the front shifts
    the media while leaving every offset pointing at where it used to be.
    Measured on a real file: stco stays 3327 while mdat moves from 3319
    to 3287, so every chunk read lands 32 bytes late and the decoder gets
    garbage NAL lengths. ffmpeg refuses it outright.

    Doing it properly would mean parsing down through
    moov/trak/mdia/minf/stbl to stco and subtracting the removed length
    from every entry -- which is possible in a stream for a faststart
    file, since moov arrives before mdat, but is real work. Renaming the
    box to 'free' gets the same result for four bytes and no risk, which
    is why mode 2 does that instead.
*/
