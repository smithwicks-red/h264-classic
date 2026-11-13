/*
    H264Startup.r  --  the 'SIZE' resource that makes H264Startup faceless

    ------------------------------------------------------------------
    What this is for
    ------------------------------------------------------------------
    Without a 'SIZE' resource the registrar still works, but it shows up
    in the Application menu with an icon and a name, which invites
    somebody to quit it -- and quitting it makes the Component Manager
    remove the codec it registered (measured: see the top of
    H264Startup.c). The failure would look like "H.264 randomly stopped
    working", which is about the worst symptom to debug.

    The key flag is onlyBackground. A background-only application has no
    menu bar, no windows, and no entry in the Application menu. It
    cannot be switched to and it cannot be quit from the UI.

    ------------------------------------------------------------------
    If Rez rejects a flag name
    ------------------------------------------------------------------
    The names below come from the 'SIZE' template in Types.r. Two are
    worth knowing because different SDK snapshots spell them
    differently:

      onlyBackground        <-> backgroundAndForeground   (we want the
                                first: background only)
      needsActivateOnFGSwitch <-> doesActivateOnFGSwitch

    If the compile stops on one of these, open Types.r and use whichever
    spelling its 'SIZE' template declares. The bit pattern we want is
    canBackground set and backgroundAndForeground clear; everything else
    here is conventional.

    ------------------------------------------------------------------
    Memory
    ------------------------------------------------------------------
    256K minimum, 512K preferred. The process holds a CFM connection and
    an event loop and allocates essentially nothing, so this is already
    generous; it is sized for comfort rather than measured need, because
    running out of memory in a faceless process is another invisible
    failure.

    ------------------------------------------------------------------
    Adding it to the project
    ------------------------------------------------------------------
    CodeWarrior 8: add this file to the same project as H264Startup.c.
    It compiles with the Rez/MacOS Resource compiler target that the
    Standard Application stationery already includes. No other resources
    are needed -- the failure alert uses StandardAlert precisely so that
    no ALRT or DITL has to exist.
*/

#include "SysTypes.r"
#include "Types.r"

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,              /* yes, we run in the background      */
    needsActivateOnFGSwitch,
    onlyBackground,             /* <-- the important one: no face     */
    dontGetFrontClicks,
    ignoreAppDiedEvents,
    is32BitCompatible,
    isHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    512 * 1024,                 /* preferred size */
    256 * 1024                  /* minimum size   */
};
