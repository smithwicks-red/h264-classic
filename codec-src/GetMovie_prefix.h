/*
    GetMovie_prefix.h  --  the single project prefix file for any of our
                           classic PPC apps that link macTLS

    ------------------------------------------------------------------
    Set this as the project's prefix file
    ------------------------------------------------------------------
    CodeWarrior 8: Settings -> Language Settings -> C/C++ Language ->
    "Prefix File" -> GetMovie_prefix.h

    It pulls in the two headers that must both be in scope for every
    translation unit, in the order that matters.

    ------------------------------------------------------------------
    Why the order matters
    ------------------------------------------------------------------
    1. ostls_cw8_prefix.h comes FIRST. It is macTLS's own prefix and it
       configures the vendored BearSSL build -- forcing the 32-bit
       integer path, disabling every platform autodetect, and (crucially)
       defining `inline` to nothing, because CodeWarrior 8's C89 mode
       rejects the keyword and BearSSL uses `static inline` in around
       180 places. Those settings have to be visible before any BearSSL
       header is parsed, so nothing may precede it.

    2. ostls_classic_shim.h comes SECOND. It maps macTLS's Carbon-only
       OTOpenEndpointInContext / OTAsyncOpenEndpointInContext onto the
       classic spellings, which is what lets macTLS compile unmodified
       into a non-Carbon application. It includes <OpenTransport.h>, so
       it must come after the BearSSL configuration and not before it.

    ------------------------------------------------------------------
    One setting in macTLS's prefix deserves reading before you trust it
    ------------------------------------------------------------------
    ostls_cw8_prefix.h pins BR_CT_MUL31 to 0, and the comment explaining
    why is the most valuable thing in the whole macTLS tree for us:

        CodeWarrior 8 PPC MISCOMPILES 64-bit shift-by-constant.
        Specifically, `(int64_t)a * const` writes `a >> log2(const)`
        into the high word.

    It bit them in BearSSL's constant-time multiply, and the symptom was
    beautifully misleading: RSA chains up to 2048 bits verified fine
    while 3072-bit and larger ones failed as NOT_TRUSTED, because the
    corruption only accumulates over enough Montgomery iterations.

    That is a defect in the exact compiler we are using. Our decoder is
    pure 32-bit integer code so it should be untouched, but it is worth
    knowing about the next time something in this project produces
    arithmetic that is wrong only sometimes. Do not add 64-bit
    multiply-by-constant arithmetic to the codec without checking it on
    hardware.

    ------------------------------------------------------------------
    Also required, once, in exactly one source file
    ------------------------------------------------------------------
        OTClientContextPtr g_ostls_ot_context = NULL;

    macTLS reads that global at every endpoint open. Classic Open
    Transport has no such concept and the shim discards the argument, so
    it stays NULL forever -- but the symbol has to exist for the link to
    succeed. GetMovie.c already defines it; if you build a different app
    against macTLS, define it there.

    And initialise OT the classic way, not the Carbon way:

        InitOpenTransport();      /* not InitOpenTransportInContext */
        ...
        CloseOpenTransport();     /* not CloseOpenTransportInContext */
*/

#ifndef GETMOVIE_PREFIX_H
#define GETMOVIE_PREFIX_H

/* 1. BearSSL build configuration + CW8 language workarounds. */
#include "ostls_cw8_prefix.h"

/* 2. Carbon -> classic Open Transport mapping. */
#include "ostls_classic_shim.h"

#endif /* GETMOVIE_PREFIX_H */
