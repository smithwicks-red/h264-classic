/*
    ostls_classic_shim.h  --  build macTLS into a classic (non-Carbon)
                              PowerPC application without editing it

    ------------------------------------------------------------------
    The problem this solves
    ------------------------------------------------------------------
    macTLS is written for Carbon. Every Open Transport endpoint it opens
    goes through the Carbon-only "InContext" spellings, which take an
    extra OTClientContextPtr argument that classic Open Transport does
    not have:

        OTOpenEndpointInContext(cfg, flags, info, &err, ctx)
        OTAsyncOpenEndpointInContext(cfg, flags, info, notifier,
                                     contextPtr, ctx)

    Our side of this project is emphatically NOT Carbon -- the
    decompressor is a classic CFM shared library and the registrar is a
    plain classic PPC console application -- and we have a specific
    reason to stay that way. macTLS's own research established that a
    Carbon CFM application cannot bind a listening Open Transport
    endpoint to a port of its choosing (OTBind returns -3150 for any
    explicit address), and that Apple's pre-Carbon HTTP Server sample
    could, precisely because it did not go through CarbonLib's wrapper.
    Staying classic is what keeps the local-proxy architecture possible
    at all. See OTProbe.c, which settles that question on hardware.

    ------------------------------------------------------------------
    Why a header and not a patch
    ------------------------------------------------------------------
    The Carbon surface in macTLS is astonishingly small. Across the
    whole os9/ tree there are exactly two Carbon-only functions in use,
    at nineteen call sites:

        OTOpenEndpointInContext        14 call sites
        OTAsyncOpenEndpointInContext    5 call sites

    and no TARGET_API_MAC_CARBON conditionals anywhere. The classic
    equivalents take exactly the same arguments minus the trailing
    context pointer. So instead of editing nineteen places in a
    third-party library we will want to re-sync later, we define the
    Carbon names as macros that discard the last argument and call the
    classic functions.

    Add this file as the project's prefix file (CodeWarrior: C/C++
    Language panel -> Prefix File), or #include it at the top of
    macTLS's own prefix header ostls_cw8_prefix.h. Either way macTLS
    compiles unmodified and links against OpenTransportLib rather than
    CarbonLib.

    This means macTLS stays a clean vendored drop. When upstream moves,
    re-copy the tree and this header still applies.

    ------------------------------------------------------------------
    The OT context global
    ------------------------------------------------------------------
    macTLS expects a global named g_ostls_ot_context to exist and be
    valid, and passes it as the context argument to both calls. Under
    classic OT there is no such thing, so the macros throw it away --
    but the symbol still has to exist for the library to link. Define it
    once in your application, and initialise Open Transport the classic
    way:

        OTClientContextPtr g_ostls_ot_context = NULL;   // never used

        InitOpenTransport();          // NOT InitOpenTransportInContext
        ...
        CloseOpenTransport();         // NOT CloseOpenTransportInContext

    The type OTClientContextPtr is still declared by OpenTransport.h in
    the classic headers, so the definition compiles; it simply stays
    NULL forever and nothing reads it.

    ------------------------------------------------------------------
    Caveat
    ------------------------------------------------------------------
    macTLS carries host-side test stubs that define static functions
    with these same names (so its logic can be exercised off-Mac). Those
    stubs live behind the host-test build and must not see these macros.
    If you ever build macTLS's host tests with this prefix in scope,
    define OSTLS_CLASSIC_SHIM_OFF to disable the whole file.
*/

#ifndef OSTLS_CLASSIC_SHIM_H
#define OSTLS_CLASSIC_SHIM_H

#ifndef OSTLS_CLASSIC_SHIM_OFF

/* We are deliberately not Carbon. Say so loudly if something in the
   build is trying to make us Carbon behind our back -- if this fires,
   the project settings are wrong and the whole reason for staying
   classic has been lost. */
#if defined(TARGET_API_MAC_CARBON) && TARGET_API_MAC_CARBON
#error "ostls_classic_shim.h is for CLASSIC PPC builds. TARGET_API_MAC_CARBON is set -- see the header comment."
#endif

#include <OpenTransport.h>
#include <OpenTptInternet.h>

/*
    Carbon:
        EndpointRef OTOpenEndpointInContext(OTConfigurationRef cfg,
                                            OTOpenFlags        flags,
                                            TEndpointInfo     *info,
                                            OSStatus          *err,
                                            OTClientContextPtr ctx);
    Classic:
        EndpointRef OTOpenEndpoint(OTConfigurationRef cfg,
                                   OTOpenFlags        flags,
                                   TEndpointInfo     *info,
                                   OSStatus          *err);

    Identical but for the trailing context. Cast it to void so an unused
    g_ostls_ot_context does not produce a warning at every call site.
*/
#define OTOpenEndpointInContext(cfg, flags, info, errp, ctx) \
    ( (void)(ctx), OTOpenEndpoint((cfg), (flags), (info), (errp)) )

/*
    Carbon:
        OSStatus OTAsyncOpenEndpointInContext(OTConfigurationRef cfg,
                                              OTOpenFlags        flags,
                                              TEndpointInfo     *info,
                                              OTNotifyUPP        notifier,
                                              void              *contextPtr,
                                              OTClientContextPtr ctx);
    Classic:
        OSStatus OTAsyncOpenEndpoint(OTConfigurationRef cfg,
                                     OTOpenFlags        flags,
                                     TEndpointInfo     *info,
                                     OTNotifyUPP        notifier,
                                     void              *contextPtr);

    Same shape, same trailing context to discard. Note macTLS passes the
    connection object as contextPtr and recovers it in the notifier, so
    that argument matters and must be preserved -- it is the SECOND to
    last, not the last.
*/
#define OTAsyncOpenEndpointInContext(cfg, flags, info, notifier, ctxPtr, ctx) \
    ( (void)(ctx), OTAsyncOpenEndpoint((cfg), (flags), (info), (notifier), (ctxPtr)) )

/*
    The initialise/close pair, for completeness. macTLS's documentation
    tells the CALLER to do these, so in practice you will write the
    classic spellings yourself and never need these macros -- but if any
    vendored file grows a call to them later, this keeps it building.
*/
#ifndef InitOpenTransportInContext
#define InitOpenTransportInContext(mask, ctxp) \
    ( (void)(mask), (void)((ctxp) ? (*(ctxp) = NULL) : NULL), InitOpenTransport() )
#endif

#ifndef CloseOpenTransportInContext
#define CloseOpenTransportInContext(ctx) \
    ( (void)(ctx), CloseOpenTransport() )
#endif

#endif /* OSTLS_CLASSIC_SHIM_OFF */
#endif /* OSTLS_CLASSIC_SHIM_H */
