/*
    OTProbe.c  --  can a NON-CARBON classic PPC app bind a listening
                   socket to a port of our choosing?

    ------------------------------------------------------------------
    Why this program exists
    ------------------------------------------------------------------
    The plan for streaming video into QuickTime on this machine is a
    small local proxy: it listens on 127.0.0.1:<port>, QuickTime is
    pointed at it through the Streaming Proxy pane of the QuickTime
    Settings control panel, and on the way through it terminates TLS,
    patches the ftyp brand to 'qt  ', and fixes up Content-Type. One
    local binary, no second machine, which is the whole constraint.

    That plan has a serious problem. The macTLS project (the TLS stack
    inside MacSurf) tried exactly this architecture first and abandoned
    it. Their finding, after fourteen rounds of testing on real
    hardware, is unambiguous:

        A Carbon CFM application opening Open Transport endpoints via
        OTOpenEndpointInContext CANNOT bind to a caller-chosen
        InetAddress. OTBind returns kOTBadAddressErr (-3150) for every
        combination of port, host, qlen, sync/async and protocol stack.
        Only OTBind(ep, NULL, NULL) and OTBind(ep, {addr=NULL, qlen=N})
        are accepted, with OT picking the local address.

    They tested ports 8765/12345/0, hosts INADDR_ANY/127.0.0.1/real LAN
    IP, qlen 0 and 1, "tcp" and "tilisten,tcp", sync and async, both
    maxlen conventions, with and without a retBind buffer. Everything
    explicit failed. Their conclusion was that CarbonLib's Open
    Transport wrapper simply does not support caller-chosen passive
    binds, and they noted that no Carbon CFM TCP server appears to have
    ever been published in twenty years.

    ------------------------------------------------------------------
    Why we might not be stuck
    ------------------------------------------------------------------
    Buried in their own research is the escape hatch. Apple's HTTP
    Server sample code DOES bind successfully to a chosen port -- and
    they explain why it doesn't help them:

        "The sample is pre-Carbon and doesn't go through CarbonLib's
         OT wrapper."

    They also record that plain OTOpenEndpoint (without the InContext
    suffix) "is not exported by CarbonLib" at all.

    So the failure is specific to CarbonLib's wrapper applying extra
    validation. The classic, non-Carbon Open Transport path -- plain
    InitOpenTransport, plain OTOpenEndpoint -- is a different code path
    that Apple's own server sample used successfully.

    MacSurf is a Carbon application and is committed to that. It could
    never run this experiment. We are not Carbon: our decompressor is a
    classic CFM shared library and the registrar is a plain classic PPC
    console app. So the question is open for us, and it is worth about
    a hundred lines to settle.

    ------------------------------------------------------------------
    CRITICAL BUILD REQUIREMENT
    ------------------------------------------------------------------
    This program MUST be built as a classic PPC application, NOT Carbon.
    If it is built against CarbonLib the answer is already known to be
    -3150 and the experiment is worthless.

    In CodeWarrior 8, start from "Mac OS PPC / Standard Console"
    stationery (the same one the registrar uses) -- NOT any Carbon
    stationery. Link InterfaceLib, MathLib, MSL, and OpenTransportLib +
    OpenTptInternetLib. Do NOT link CarbonLib. If the project has
    TARGET_API_MAC_CARBON defined anywhere, remove it.

    The program prints which OT path it compiled against, so the output
    itself proves whether the test was valid.

    ------------------------------------------------------------------
    What it tests
    ------------------------------------------------------------------
    The same 2x2 matrix macTLS documented, plus the cases that matter
    to us, so the results line up directly against their table:

        1. OTBind(NULL, NULL)                       expect: OK
        2. OTBind({addr=NULL, qlen=1})              expect: OK, OT port
        3. OTBind(127.0.0.1:8080, qlen=1)           the question
        4. OTBind(INADDR_ANY:8080, qlen=1)          the question
        5. OTBind(INADDR_ANY:0,    qlen=1)          sanity

    Cases 3 and 4 are the ones that decide the architecture. If either
    returns noErr, the proxy plan is alive and we can point QuickTime at
    a fixed port. If both return -3150, we fall back to accepting an
    OT-assigned ephemeral port and retyping it into the QuickTime
    control panel at each launch -- clunky but workable, since that pane
    has both a host and a port field.

    Nothing here sends or receives a byte. It opens an endpoint, tries
    to bind, reports, and tears down. It is safe to run repeatedly.
*/

#include <stdio.h>
#include <string.h>

#include <Types.h>
#include <Errors.h>
#include <OpenTransport.h>
#include <OpenTptInternet.h>

#define kProbePort   8080
#define kLoopbackIP  0x7F000001UL   /* 127.0.0.1 */

/* -3150. Spelled as a literal as well as by name because the constant
   moved headers between SDK revisions and we want the number visible in
   the output regardless. */
#ifndef kOTBadAddressErr
#define kOTBadAddressErr (-3150)
#endif

static void PrintIP(UInt32 ip)
{
    printf("%lu.%lu.%lu.%lu",
           (unsigned long)((ip >> 24) & 0xFF),
           (unsigned long)((ip >> 16) & 0xFF),
           (unsigned long)((ip >>  8) & 0xFF),
           (unsigned long)((ip      ) & 0xFF));
}

/*
    Run one bind case.

    useExplicitAddr  0 = hand OTBind a NULL address and let OT choose
                     1 = hand OTBind an InetAddress we picked
    ip, port         only meaningful when useExplicitAddr is 1
    qlen             0 = client endpoint, >=1 = listening endpoint
    passNullReq      1 = OTBind(ep, NULL, NULL) entirely
*/
static void RunBindCase(const char *label,
                        int    passNullReq,
                        int    useExplicitAddr,
                        UInt32 ip,
                        UInt16 port,
                        OTQLen qlen)
{
    EndpointRef ep = kOTInvalidEndpointRef;
    OSStatus    err;
    TBind       req, ret;
    InetAddress reqAddr, retAddr;

    printf("\n--- %s\n", label);

    /* Plain OTOpenEndpoint, NOT OTOpenEndpointInContext. This is the
       call CarbonLib does not export, and the one Apple's pre-Carbon
       HTTP Server sample used successfully. */
    ep = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, &err);
    if (ep == kOTInvalidEndpointRef || err != kOTNoError)
    {
        printf("    OTOpenEndpoint failed, err = %d\n", (int)err);
        return;
    }

    /* Synchronous + blocking keeps the probe simple: OTBind returns a
       real answer rather than completing via a notifier. macTLS tested
       async too and it made no difference to the outcome. */
    (void)OTSetSynchronous(ep);
    (void)OTSetBlocking(ep);

    memset(&req, 0, sizeof(req));
    memset(&ret, 0, sizeof(ret));
    memset(&retAddr, 0, sizeof(retAddr));

    /* Always give OT somewhere to report the address it actually bound,
       so a success tells us the port as well as the fact of success. */
    ret.addr.buf    = (UInt8 *)&retAddr;
    ret.addr.len    = 0;
    ret.addr.maxlen = sizeof(retAddr);

    if (passNullReq)
    {
        err = OTBind(ep, NULL, NULL);
    }
    else
    {
        if (useExplicitAddr)
        {
            OTInitInetAddress(&reqAddr, port, ip);
            req.addr.buf    = (UInt8 *)&reqAddr;
            req.addr.len    = sizeof(reqAddr);
            req.addr.maxlen = sizeof(reqAddr);

            printf("    requesting ");
            PrintIP(ip);
            printf(":%u  (qlen = %d)\n", (unsigned)port, (int)qlen);

            /* Show the exact bytes going to OT. macTLS hex-dumped these
               to rule out a malformed InetAddress, and so do we -- if
               this probe fails we want to be able to compare directly
               against their dump rather than re-derive it. */
            {
                int i;
                const unsigned char *b = (const unsigned char *)&reqAddr;
                printf("    InetAddress bytes:");
                for (i = 0; i < 8; i++) printf(" %02X", b[i]);
                printf("\n");
            }
        }
        else
        {
            req.addr.buf    = NULL;
            req.addr.len    = 0;
            req.addr.maxlen = 0;
            printf("    requesting OT-chosen address  (qlen = %d)\n", (int)qlen);
        }

        req.qlen = qlen;
        err = OTBind(ep, &req, &ret);
    }

    if (err == kOTNoError)
    {
        printf("    OTBind -> OK");
        if (!passNullReq && ret.addr.len >= sizeof(InetAddress))
        {
            printf("   bound to ");
            PrintIP(retAddr.fHost);
            printf(":%u", (unsigned)retAddr.fPort);
        }
        printf("\n");
        if (useExplicitAddr)
            printf("    *** THIS IS THE ANSWER WE WANTED. ***\n");
        (void)OTUnbind(ep);
    }
    else
    {
        printf("    OTBind -> FAILED, err = %d", (int)err);
        if (err == kOTBadAddressErr)
            printf("  (kOTBadAddressErr -- same as CarbonLib)");
        printf("\n");
    }

    OTCloseProvider(ep);
}

int main(void)
{
    OSStatus err;

    printf("================================================================\n");
    printf("OTProbe -- passive bind test, classic (non-Carbon) PPC\n");
    printf("================================================================\n");
    printf("\n");
    printf("Question: can a non-Carbon classic PPC application bind a\n");
    printf("listening Open Transport endpoint to a port we choose?\n");
    printf("\n");
    printf("macTLS proved a CARBON app cannot -- OTBind returns -3150 for\n");
    printf("any explicit address. Their research noted Apple's pre-Carbon\n");
    printf("HTTP Server sample succeeds, because it does not go through\n");
    printf("CarbonLib's OT wrapper. This program is that pre-Carbon path.\n");
    printf("\n");

    /* Prove which world we were compiled into. If this says Carbon then
       the build is wrong and the results below mean nothing. */
#if defined(TARGET_API_MAC_CARBON) && TARGET_API_MAC_CARBON
    printf("!!! BUILT AGAINST CARBON -- THIS TEST IS INVALID !!!\n");
    printf("!!! Rebuild from Mac OS PPC / Standard Console stationery. !!!\n\n");
#else
    printf("Build: classic PPC (not Carbon). Good -- the test is valid.\n\n");
#endif

    /* Classic initialisation. The Carbon spelling would be
       InitOpenTransportInContext(kInitOTForApplicationMask, &ctx). */
    err = InitOpenTransport();
    if (err != kOTNoError)
    {
        printf("InitOpenTransport failed, err = %d\n", (int)err);
        printf("Is TCP/IP configured and active?\n");
        printf("\nPress return to quit. ");
        (void)getchar();
        return 1;
    }
    printf("InitOpenTransport OK.\n");

    /* --- the controls: these are known to work even on Carbon --- */
    RunBindCase("1. OTBind(ep, NULL, NULL)  [control, expect OK]",
                1, 0, 0, 0, 0);

    RunBindCase("2. OTBind(addr = NULL, qlen = 1)  [control, expect OK]",
                0, 0, 0, 0, 1);

    /* --- the questions: these are -3150 on Carbon --- */
    RunBindCase("3. OTBind(127.0.0.1:8080, qlen = 1)  [THE QUESTION]",
                0, 1, kLoopbackIP, kProbePort, 1);

    RunBindCase("4. OTBind(INADDR_ANY:8080, qlen = 1)  [THE QUESTION]",
                0, 1, (UInt32)0, kProbePort, 1);

    /* --- sanity: explicit address but port 0 (let OT pick the port) --- */
    RunBindCase("5. OTBind(INADDR_ANY:0, qlen = 1)  [sanity]",
                0, 1, (UInt32)0, 0, 1);

    CloseOpenTransport();

    printf("\n================================================================\n");
    printf("How to read this\n");
    printf("================================================================\n");
    printf("Cases 3 or 4 OK    -> the local proxy plan is ALIVE. QuickTime\n");
    printf("                      can be pointed at a fixed port in the\n");
    printf("                      Streaming Proxy pane and left there.\n");
    printf("Both -3150         -> the classic path has the same limit as\n");
    printf("                      CarbonLib. Fall back to case 2: let OT\n");
    printf("                      assign a port, print it, and retype it\n");
    printf("                      into the control panel each launch.\n");
    printf("Case 5 OK only     -> addresses are fine, PORTS are the thing\n");
    printf("                      being refused. Same fallback.\n");
    printf("\nPress return to quit. ");
    (void)getchar();
    return 0;
}
