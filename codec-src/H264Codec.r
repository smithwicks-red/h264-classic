/*
    H264Codec.r

    Component resource description. This is what registers us with the
    Component Manager at runtime -- without a correct 'thng' resource the
    compiled code fragment is just an inert file as far as QuickTime is
    concerned.

    Components.r (Apple's Rez template collection, shipped alongside the
    C headers in CodeWarrior's Universal Interfaces) defines the `thng`
    and `dlle` templates used below.
*/

/* Types.r defines the 'STR ' template used below; Components.r alone
   only defines Component-Manager-specific templates and does not pull
   Types.r in for us. Must come first. */
#include "Types.r"

/* ==================================================================
   Template version 0: Apple's original 13-field 'thng'.
   ==================================================================

   This file has now been all three ways round, so the reasoning is
   written out rather than left to be re-derived a fourth time.

   ------------------------------------------------------------------
   What is actually broken, established by measurement
   ------------------------------------------------------------------
   Every layer below the resources has been proven good, individually,
   by ComponentProbe:

     - the component registers: it appears as one of 88 'imdc'
       decompressors with the right name, info and subtype
     - the 'cfrg' is correct: usage 0 = kImportLibraryCFrag (0 is the
       GOOD value here -- kApplicationCFrag is 1, and having those two
       backwards cost a session), where 1 = data fork, length 0 = to EOF
     - the fragment prepares: GetDiskFragment returns 0
     - both symbols resolve: FindSymbol finds the dispatcher (class 2,
       TVector) and its RoutineDescriptor (class 1, data)
     - the dispatch table works: calling the dispatcher directly with
       the Version selector returns 0x00010000

   And then the decisive test. RegisterComponent() hands the Component
   Manager an entry point directly, with no 'thng' and no 'dlle'
   anywhere in the path:

       RegisterComponent(RoutineDescriptor) -> 0x00100266
       OpenComponent                        -> 0x00940016  SUCCESS
       CallComponentVersion                 -> 0x00010000
       CanDo(0x05 PreDecompress)            -> 1
       CanDo(0x06 BandDecompress)           -> 1

   with the component's own log showing "Open enter / NewPtrClear /
   storage set / Open OK / Version enter / Close enter".

   The component works. What does not work is the resource path that is
   supposed to hand the Component Manager that same entry point, and by
   elimination the only ingredient of that path never given a fair test
   is the extended template's platform array.

   ------------------------------------------------------------------
   Why the platform array is the remaining suspect
   ------------------------------------------------------------------
   Session 4 wrote this file in the 13-field form for exactly this
   reason, and then concluded the theory was wrong. It was not wrong --
   it was never tested. The file timestamps show the source was edited
   eleven minutes AFTER the last link of that era, so the simple
   template went into a build for the first time only now. Every
   "13-field template fails" data point in the earlier notes was
   actually measuring the extended-template binary still sitting in
   Extensions.

   So: code type and code ID go back into the top-level fields, there is
   no version field, no registration flags, and no platform array. This
   is the form every pre-QuickTime-3 component shipped with, and the
   form with the least machinery between the 'thng' and the code.

   ------------------------------------------------------------------
   Why 'dlle' names the bare TVector and not the RoutineDescriptor
   ------------------------------------------------------------------
   Two Component Manager entry points want two DIFFERENT things, which
   is the trap this project fell into twice. Both halves were measured:

     RegisterComponent()      wants a RoutineDescriptor (a UPP)
        RD      -> OpenComponent SUCCEEDED, Version, CanDo, Close all OK
        TVector -> hung the machine mid-call

     'dlle' + FindSymbol      wants the bare TVector
        RD      -> hung the machine mid-call
        TVector -> this build

   Mirror images, and sensible once seen: RegisterComponent is declared
   to take a ComponentRoutineUPP, and on classic PowerPC a UPP is a
   pointer to a RoutineDescriptor; the 'dlle' route instead resolves a
   symbol by name through CFM and gets a native code pointer it calls
   directly. Give either one the other's form and it jumps into
   something that is not code for it, and the machine stops -- no error
   code, because there is nobody left to return one.

   The hang is therefore a useful signal rather than a disaster: it only
   happens once the Component Manager is actually calling our entry
   point. Every earlier attempt failed the quieter way, with
   OpenComponent returning NULL because the platform array meant it
   never got that far.

   Both symbols are genuinely exported (confirmed by parsing the PEF
   export table), which required two CodeWarrior settings that default
   to the wrong thing for a component: PPC PEF -> Export Symbols set to
   "Use #pragma" (it defaults to None, which silently exports nothing),
   plus the #pragma export bracket around the ComponentDispatchHelper.c
   include in H264Codec.c.

   ------------------------------------------------------------------
   If this build still does not register a callable component
   ------------------------------------------------------------------
   Then the 'thng'/'dlle' route is exhausted and the fallback is to stop
   using it: ship a tiny init-time extension (or have the component's
   own CFM initialization routine) call RegisterComponent with the
   RoutineDescriptor directly, which is already proven to work. Less
   tidy, entirely functional.
   ================================================================== */
#define thng_RezTemplateVersion 0
#include "Components.r"

#include "H264Codec.h"

/* ==================================================================
   THE 'thng' AND 'dlle' RESOURCES ARE DELIBERATELY GONE
   ==================================================================

   Everything above this line documents an investigation that reached a
   negative result: on this system the Component Manager cannot be made
   to reach our entry point through 'thng' -> 'dlle'. Every combination
   was built and measured --

     extended template + RoutineDescriptor  -> OpenComponent NULL,
                                               our code never entered
     extended template + TVector            -> same
     simple template   + RoutineDescriptor  -> caller dies, type 3
     simple template   + TVector            -> caller dies, type 3
     simple template   + a four-line stub
                         that only logs      -> caller dies, type 3,
                                               and the stub never logged

   -- and that last one is conclusive. A stub which cannot survive being
   called proves the address being called is not the stub.

   Worse, leaving a 'thng' in place is actively harmful: the Component
   Manager finds it during startup scanning, follows the broken path,
   and takes QuickTime Player down with a type 3 before its window
   appears. The file has to be inert.

   So the resources are removed and the codec is registered
   programmatically instead, by the registrar built from
   ComponentProbe.c, which calls RegisterComponent with the
   RoutineDescriptor -- the one route with a complete chain of evidence
   behind it: OpenComponent succeeds, Version returns 0x00010000, CanDo
   answers correctly for PreDecompress and BandDecompress, and the
   codec's own log shows Open running and Close running.

   What remains in this file: the 'STR ' resources, kept because the
   registrar could use them and because they cost nothing, and the
   'cfrg' that CodeWarrior generates automatically, which is what lets
   GetDiskFragment prepare the fragment at all. The file's type stays
   'thng' for recognisability; with no 'thng' RESOURCE inside, the
   Component Manager opens it, finds nothing to register, and moves on.

   If a future Mac OS 9 / QuickTime combination is ever found where the
   'dlle' route does work, the resource is preserved in this file's
   history and in the porting notes -- nothing was lost by removing it.
   ================================================================== */


/* Names the exported symbol the Code Fragment Manager resolves with
   FindSymbol to get our entry point. The bare dispatcher function, NOT
   the RoutineDescriptor -- see the long note above. Naming ...RD here
   hangs the machine. */

resource 'STR ' (256) {
    "H.264 (Software)"
};

resource 'STR ' (257) {
    "H.264/AVC Baseline decoder for QuickTime, powered by h264bsd. "
    "Homebrew port for Mac OS 9.2.2 / PowerPC."
};
