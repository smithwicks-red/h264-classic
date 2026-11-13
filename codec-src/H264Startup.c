/*
    H264Startup.c  --  register the H.264 decoder automatically at boot

    ==================================================================
    WHY THIS IS NOT AN APPLICATION THAT QUITS
    ==================================================================
    The obvious design is a tiny application in System Folder:Startup
    Items that registers the codec and quits. That design does not work:
    the codec would be gone again before anyone could use it.

    MEASURED, not assumed (2026-09-23, Mac OS 9.2.2, QuickTime 6.0.3):
    the bench registrar registers with RegisterComponent(..., global=1)
    and never calls UnregisterComponent. After quitting it and running
    it again, its first line read

        Before: 'imdc'/'avc1' registered = 0

    and the new registration came back with a fresh component ID
    (0x0010027E where the previous run had 0x000F027E). The Component
    Manager removes a registering application's components when that
    application quits, "global" or not. "global" controls who can SEE
    the component, not how long it lives.

    That is also the safe behaviour: RegisterComponent stores the address
    of the RoutineDescriptor inside the codec's CFM fragment, the
    fragment belongs to the process that prepared it, and CFM unloads it
    when that process quits. Removing the registration at the same moment
    means no caller is left with a pointer into freed memory.

    (An earlier draft of this comment asserted the opposite -- that
    quitting would leave a dangling registration. That was reasoning, not
    evidence, and the evidence disagreed. The conclusion below survives;
    the stated failure mode did not.)

    So SOMETHING must stay alive for as long as the codec should be
    available. Two ways to do that:

      1. A system extension (INIT). Code in an INIT loads into the
         System heap at boot and stays for the life of the session --
         which is exactly what real codecs do. But real codecs get there
         via 'thng'/'dlle' resources, and that route is precisely what
         does not work on this system; see the notes at the top of
         ComponentProbe.c. Writing a PowerPC INIT means a 68K 'INIT'
         stub that bridges into CFM, which is real work and a new class
         of bug.

      2. A FACELESS BACKGROUND APPLICATION. It launches at startup,
         registers the codec, and then stays running until the system
         shuts down or restarts. It holds the fragment connection open
         for the whole session. With the background-only bit set in its
         'SIZE' resource it has no menu bar, no windows, and no entry in
         the Application menu -- the user never knows it is there. (It
         DOES answer the Finder's Quit event at Shut Down -- see
         "Quitting when the system asks", which records the hang the
         first build caused by not doing so.)

    This file is option 2, because it is a few hundred lines of ordinary
    code rather than a new toolchain problem, and because the cost is
    one process slot and a couple of hundred K of RAM on a machine with
    hundreds of megabytes. Option 1 remains the tidier long-term answer
    and the reasoning above is written down so that whoever attempts it
    knows what it has to guarantee.

    While idle this process sleeps in WaitNextEvent with the longest
    possible timeout, so it costs no measurable CPU. It is not polling
    anything.

    ==================================================================
    FINDING THE EXTENSION
    ==================================================================
    ComponentProbe.c hardcodes "Macintosh HD:System Folder:Extensions:
    H264 Decoder". That is fine for a bench tool on one machine and
    useless on a second one -- and the entire point of this file is to
    work unattended on the Pismo and the TiBook, whose boot volumes are
    not going to share a name.

    So this asks the system where its Extensions folder actually is, via
    FindFolder(kOnSystemDisk, kExtensionFolderType), and falls back to
    the hardcoded path only if that fails. FindFolder is the answer on
    any Mac OS 9 machine regardless of volume name, and it correctly
    follows the ACTIVE System Folder on a multi-boot disk, which matters
    on a machine that also has an OS X install.

    ==================================================================
    REPORTING FAILURE WITHOUT A FACE
    ==================================================================
    A faceless process that silently does nothing is worse than no
    process at all, because the symptom shows up as "H.264 stopped
    working" with nothing to look at. Two reporting channels:

      - A log file, always written, next to the codec in Extensions. It
        records what happened in a handful of lines. This is what to
        read first on a machine you are not sitting in front of.

      - On FAILURE only, a Notification Manager alert. NOT StandardAlert:
        a background-only application is never frontmost, so it cannot
        reliably put up a modal dialog of its own -- the first draft of
        this file did exactly that, and it was a bug waiting for the
        first failure. The Notification Manager is the mechanism Mac OS
        provides for faceless processes and INITs to get the user's
        attention: it shows the text in a system alert when it is safe
        to, and the user dismisses it.

        The request lives in the SYSTEM heap with nmResp = -1, which
        tells the Notification Manager to remove it by itself once it
        has been shown. That is what allows this process to quit after
        a failure: nothing the Notification Manager still needs belongs
        to us. Set kAlertOnFailure to 0 if a machine must boot
        unattended and you would rather read the log.

    On success it says nothing at all, which is the whole point.

    ==================================================================
    INSTALLING
    ==================================================================
    Verified in the VM, 2026-09-23:

      1. Build with file type 'appe' (PPC Target panel, "Type") and the
         name "H264 Auto-Register".
      2. Drag it AND "H264 Decoder" onto the closed System Folder icon.
         The Finder recognises both and offers to put them in the
         Extensions folder ("This item needs to be stored in the
         Extensions folder in order to be available to the computer").
         Say OK.
      3. Restart (on the real Mac -- in the UTM VM, Shut Down and start
         again, never Restart). Nothing visible should happen. Open any
         .mov that uses the codec -- it just works.

    Why 'appe' in Extensions rather than an 'APPL' in Startup Items: the
    Process Manager launches 'appe' files from the Extensions folder by
    itself, early; both halves of the codec then live in one folder;
    Extensions Manager can switch them off together; and booting with
    Shift held (extensions off) disables both at once instead of leaving
    a registrar looking for a codec that did not load. An 'APPL' copy in
    Startup Items also works -- that is how it was first tested.

    To check it is alive: System Folder:Extensions:H264 Register Log.txt
    gains a new "=== H264 Auto-Register, startup ===" block every boot,
    and a "Quit requested" block at every Shut Down.

    ==================================================================
    BUILDING
    ==================================================================
    CodeWarrior 8, "Mac OS PPC / Standard Application" stationery (NOT
    Standard Console -- console means SIOUX, and SIOUX means a window,
    which is the one thing this must not have). Add this file and
    H264Startup.r. Keep InterfaceLib and MSL. No #pragma export.

    H264Startup.r carries the 'SIZE' resource that makes it background
    only. Without that resource it still works but it appears in the
    Application menu, which is untidy rather than wrong. CodeWarrior
    warns "A 'SIZE'(-1) resource was found. It will override some
    project settings" -- that is the point of the file; ignore it.

    In the PPC Target panel set Type to 'appe' (see INSTALLING) and File
    Name to "H264 Auto-Register". The Apple Event Manager calls it makes
    are in InterfaceLib, which the stationery already links.
*/

#include <Types.h>
#include <Files.h>
#include <Folders.h>
#include <Components.h>
#include <CodeFragments.h>
#include <Memory.h>
#include <Events.h>
#include <Dialogs.h>
#include <Notification.h>
#include <TextUtils.h>
#include <Resources.h>
#include <Gestalt.h>
#include <AppleEvents.h>
#include <string.h>


/* ==================================================================
   Configuration
   ================================================================== */

/* Must match H264Codec.r and ComponentProbe.c exactly. The Component
   Manager finds us by this triple; a mismatch means QuickTime looks for
   'avc1' and finds nothing. */
#define kCodecType      'imdc'
#define kCodecSubType   'avc1'
#define kCodecManu      'wY9H'

/* The file in Extensions, by name. The folder is located at runtime. */
#define kCodecFileName  "\pH264 Decoder"

/* Last-resort path if FindFolder fails, which it should not. */
#define kFallbackPathC  "Macintosh HD:System Folder:Extensions:H264 Decoder"

/* The exported RoutineDescriptor inside the codec fragment. */
#define kSymbolRD       "\pH264CodecComponentDispatchRD"

#define kLogFileName    "\pH264 Register Log.txt"

/* 1 = put an alert up if registration fails. 0 = log only, boot clean. */
#define kAlertOnFailure 1

/* GetDiskFragment load option, matching ComponentProbe.c. */
#define kLoadOptions    1

/* How long, after the Finder asks us to quit (i.e. at Shut Down or
   Restart), to wait for other applications to close any codec
   instances they still hold before letting go anyway. See
   QuitWhenAsked() below. 600 ticks = 10 seconds. */
#define kQuitGraceTicks 600UL


/* ==================================================================
   Logging -- no stdio, because there is no console
   ================================================================== */

static FSSpec  gLogSpec;
static Boolean gHaveLogSpec = false;

/* Set by the Quit Application Apple event handler; ends the idle loop. */
static Boolean   gQuitRequested = false;

/* The component we registered, kept so that we can count its open
   instances and unregister it in an orderly way when told to quit. */
static Component gComp = NULL;

static short CStrLen(const char *s)
{
    short n = 0;
    while (s[n] != '\0' && n < 2000) n++;
    return n;
}

/* Append a line. Opens and closes every time: slow, but this runs a
   handful of times per boot and a log that survives a crash is worth
   more here than a fast one. */
static void Log(const char *msg)
{
    short refNum;
    long  count, eof;
    char  cr = '\r';

    if (!gHaveLogSpec) return;

    if (FSpOpenDF(&gLogSpec, fsWrPerm, &refNum) != noErr)
    {
        if (FSpCreate(&gLogSpec, 'ttxt', 'TEXT', smSystemScript) != noErr) return;
        if (FSpOpenDF(&gLogSpec, fsWrPerm, &refNum) != noErr) return;
        eof = 0;
    }
    else
    {
        if (GetEOF(refNum, &eof) != noErr) eof = 0;
    }

    SetFPos(refNum, fsFromStart, eof);

    count = CStrLen(msg);
    if (count > 0) FSWrite(refNum, &count, (Ptr)msg);
    count = 1;
    FSWrite(refNum, &count, (Ptr)&cr);

    FSClose(refNum);
    FlushVol(NULL, gLogSpec.vRefNum);
}

/* "label = <signed decimal>" without sprintf. */
static void LogNum(const char *label, long v)
{
    char buf[160];
    char digits[16];
    short i = 0, t = 0, j;
    unsigned long uv;

    while (label[i] != '\0' && i < 120) { buf[i] = label[i]; i++; }
    buf[i++] = ' '; buf[i++] = '='; buf[i++] = ' ';

    if (v < 0) { buf[i++] = '-'; uv = (unsigned long)(-v); }
    else uv = (unsigned long)v;

    if (uv == 0) digits[t++] = '0';
    while (uv > 0) { digits[t++] = (char)('0' + (uv % 10)); uv /= 10; }
    for (j = t - 1; j >= 0; j--) buf[i++] = digits[j];

    buf[i] = '\0';
    Log(buf);
}

static void Complain(const char *title, const char *why)
{
    Log("FAILED:");
    Log(why);

#if kAlertOnFailure
    {
        /* Notification Manager request in the SYSTEM heap, auto-removed.
           See "REPORTING FAILURE WITHOUT A FACE" at the top of the file
           for why this is not StandardAlert.

           Both the record and its string are deliberately leaked into
           the system heap: a few hundred bytes, only on failure, once
           per boot -- the price of being allowed to quit afterwards
           without leaving the Notification Manager pointing at memory
           that no longer exists. */
        NMRecPtr  nm;
        StringPtr s;
        short     nt, nw;

        nm = (NMRecPtr)NewPtrSysClear((Size)sizeof(NMRec));
        s  = (StringPtr)NewPtrSys(256);
        if (nm == NULL || s == NULL)
        {
            if (nm != NULL) DisposePtr((Ptr)nm);
            if (s  != NULL) DisposePtr((Ptr)s);
            return;
        }

        /* "<title>: <why>", clipped to one Pascal string. */
        nt = CStrLen(title); if (nt > 60) nt = 60;
        nw = CStrLen(why);   if (nw > 255 - nt - 2) nw = (short)(255 - nt - 2);
        BlockMoveData(title, s + 1, nt);
        s[1 + nt] = ':';
        s[2 + nt] = ' ';
        BlockMoveData(why, s + 3 + nt, nw);
        s[0] = (unsigned char)(nt + 2 + nw);

        nm->qType    = nmType;
        nm->nmMark   = 0;               /* no mark in the Application menu */
        nm->nmIcon   = NULL;            /* no flashing menu-bar icon       */
        nm->nmSound  = (Handle)-1;      /* the user's system alert sound   */
        nm->nmStr    = s;
        nm->nmResp   = (NMUPP)-1;       /* remove automatically once shown */
        nm->nmRefCon = 0;

        if (NMInstall(nm) != noErr)
        {
            DisposePtr((Ptr)s);
            DisposePtr((Ptr)nm);
        }
    }
#else
    (void)title;
#endif
}


/* A Handle holding a Pascal string: the shape RegisterComponent wants
   for the component's name and info. */
static Handle PStrHandle(const char *c)
{
    short  n = CStrLen(c);
    Handle h;

    if (n > 255) n = 255;
    h = NewHandle((Size)(n + 1));
    if (h == NULL) return NULL;
    (*h)[0] = (char)n;
    BlockMoveData(c, *h + 1, n);
    return h;
}


/* ==================================================================
   Is it already registered?
   ==================================================================
   Re-registering would work, but it would stack a second identical
   component on every boot if this file ever ended up in Startup Items
   twice, and a duplicate component is a confusing thing to debug later.
   Cheap to check, so check. */
static long CountMatching(void)
{
    ComponentDescription cd;
    Component            c = NULL;
    long                 n = 0;

    memset(&cd, 0, sizeof(cd));
    cd.componentType         = kCodecType;
    cd.componentSubType      = kCodecSubType;
    cd.componentManufacturer = 0;   /* any manufacturer */

    while ((c = FindNextComponent(c, &cd)) != NULL) n++;
    return n;
}


/* ==================================================================
   Locate the codec file
   ================================================================== */
static Boolean FindCodecFile(FSSpec *spec)
{
    short vRefNum;
    long  dirID;
    OSErr err;

    err = FindFolder(kOnSystemDisk, kExtensionFolderType,
                     kDontCreateFolder, &vRefNum, &dirID);
    if (err == noErr)
    {
        err = FSMakeFSSpec(vRefNum, dirID, kCodecFileName, spec);
        if (err == noErr) return true;
        LogNum("  FSMakeFSSpec in Extensions failed, err", (long)err);
    }
    else
    {
        LogNum("  FindFolder(Extensions) failed, err", (long)err);
    }

    /* Fall back to the hardcoded path. */
    {
        Str255 path;
        short  n = CStrLen(kFallbackPathC);
        path[0] = (unsigned char)n;
        BlockMoveData(kFallbackPathC, path + 1, n);
        err = FSMakeFSSpec(0, 0, path, spec);
        if (err == noErr)
        {
            Log("  (found via the hardcoded fallback path)");
            return true;
        }
        LogNum("  fallback path failed too, err", (long)err);
    }

    return false;
}

/* Put the log beside the codec, or beside nothing if we cannot find
   Extensions at all -- in which case there is nowhere sensible and we
   stay silent rather than scattering files about. */
static void SetUpLog(void)
{
    short vRefNum;
    long  dirID;

    OSErr err;

    if (FindFolder(kOnSystemDisk, kExtensionFolderType,
                   kDontCreateFolder, &vRefNum, &dirID) != noErr) return;

    /* fnfErr is success: the folder exists, the log just has not been
       created yet, and the spec is filled in either way. */
    err = FSMakeFSSpec(vRefNum, dirID, kLogFileName, &gLogSpec);
    if (err == noErr || err == fnfErr) gHaveLogSpec = true;
}


/* ==================================================================
   The work
   ================================================================== */
static Boolean RegisterTheCodec(CFragConnectionID *connOut)
{
    FSSpec            spec;
    CFragConnectionID connID = NULL;
    Ptr               mainAddr = NULL;
    Ptr               rdAddr   = NULL;
    CFragSymbolClass  cls;
    Str255            fragName, errName;
    OSErr             err;

    if (CountMatching() > 0)
    {
        Log("Already registered by someone else this boot; nothing to do.");
        /* Return true, but with no connection: we are not the owner, so
           we must not pretend to be. The other owner holds it. */
        *connOut = NULL;
        return true;
    }

    if (!FindCodecFile(&spec))
    {
        Complain("H.264 Decoder",
                 "Could not find the file \"H264 Decoder\" in the "
                 "Extensions folder, so the H.264 codec was not "
                 "registered. Movies using it will not play.");
        return false;
    }

    fragName[0] = 0;
    errName[0]  = 0;

    err = GetDiskFragment(&spec, 0, kCFragGoesToEOF, fragName,
                          kLoadOptions, &connID, &mainAddr, errName);
    if (err != noErr)
    {
        LogNum("  GetDiskFragment err", (long)err);
        Complain("H.264 Decoder",
                 "The H264 Decoder extension could not be prepared. "
                 "-2804 means a library it needs is missing; -2807 means "
                 "an import could not be resolved. The log in the "
                 "Extensions folder has the error number.");
        return false;
    }

    err = FindSymbol(connID, kSymbolRD, &rdAddr, &cls);
    if (err != noErr || rdAddr == NULL)
    {
        LogNum("  FindSymbol err", (long)err);
        CloseConnection(&connID);
        Complain("H.264 Decoder",
                 "The H264 Decoder extension is missing its dispatch "
                 "symbol. It was probably built without \"Export "
                 "Symbols: Use #pragma\" set.");
        return false;
    }

    {
        ComponentDescription cd;
        Component            comp;

        memset(&cd, 0, sizeof(cd));
        cd.componentType         = kCodecType;
        cd.componentSubType      = kCodecSubType;
        cd.componentManufacturer = kCodecManu;

        /* global = 1: every application sees it, not just this one.
           That is the entire purpose of this program.

           Name and info handles match what the bench registrar passes,
           so the component looks the same whichever registered it. The
           Component Manager keeps them; they are not ours to dispose. */
        comp = RegisterComponent(&cd, (ComponentRoutineUPP)rdAddr, 1,
                                 PStrHandle("H.264 (Software)"),
                                 PStrHandle("H.264/AVC Baseline decoder for "
                                            "QuickTime, powered by h264bsd."),
                                 NULL);

        if (comp == NULL)
        {
            CloseConnection(&connID);
            Complain("H.264 Decoder",
                     "RegisterComponent refused the H.264 decoder. The "
                     "codec will not be available this session.");
            return false;
        }

        LogNum("  registered, component", (long)comp);
        LogNum("  'imdc'/'avc1' now count", CountMatching());
        gComp = comp;
    }

    *connOut = connID;
    return true;
}


/* ==================================================================
   Quitting when the system asks -- and only then
   ==================================================================
   MEASURED, 2026-09-23: the first build of this file had no quit path
   at all, on the theory that a process nobody can see is a process
   nobody can quit by accident. True -- but the Finder quits every
   running process at Shut Down and Restart by sending it a Quit
   Application Apple event, and the 'SIZE' resource says we understand
   high-level events. We received it and ignored it, so Special > Shut
   Down stalled forever: the second attempt got "The command could not
   be completed, because you have already chosen Shut Down or Restart",
   and the only way out was a forced power-off. On a real PowerBook
   that would have been every shutdown.

   So the idle loop now hands high-level events to the Apple Event
   Manager, and a Quit Application handler ends it. Nothing else can
   send that event to a background-only process in practice, so the
   original concern -- an accidental quit -- still cannot happen.

   The one subtle part is what "quit" must not do. A codec instance
   that some other application still has open (a movie left playing in
   QuickTime Player at shutdown) calls into the code this process
   loaded. If we unloaded that code first, the other application's
   next call into its instance -- typically its Close as it quits --
   would jump into freed memory. So QuitWhenAsked() waits, up to
   kQuitGraceTicks, for the instance count to reach zero, giving the
   other applications time to run and close their movies. Only then
   does it unregister the component and close the fragment connection
   itself. If the time runs out it leaves anyway -- the Component
   Manager's own clean-up when a process quits is the same as it always
   was -- because stalling shutdown again would be the worse failure.
   The log records which of the two happened, and how many instances
   were open, so the ordering OS 9 actually uses at shutdown can be
   read off a real machine. */
static pascal OSErr HandleQuitAE(const AppleEvent *evt, AppleEvent *reply, long refcon)
{
    (void)evt; (void)reply; (void)refcon;
    gQuitRequested = true;
    return noErr;
}

static pascal OSErr HandleOpenAppAE(const AppleEvent *evt, AppleEvent *reply, long refcon)
{
    (void)evt; (void)reply; (void)refcon;
    return noErr;
}

static void InstallAppleEventHandlers(void)
{
    OSErr err;

    /* The casts keep this independent of whether the Universal
       Interfaces in use spell the refcon parameter long or UInt32: they
       are the same width on PowerPC, and the Routine Descriptor built
       here describes the real calling convention either way. */
    err = AEInstallEventHandler(kCoreEventClass, kAEOpenApplication,
              NewAEEventHandlerUPP((AEEventHandlerProcPtr)HandleOpenAppAE),
              0, false);
    if (err != noErr) LogNum("  AEInstallEventHandler(oapp) err", (long)err);

    err = AEInstallEventHandler(kCoreEventClass, kAEQuitApplication,
              NewAEEventHandlerUPP((AEEventHandlerProcPtr)HandleQuitAE),
              0, false);
    if (err != noErr) LogNum("  AEInstallEventHandler(quit) err", (long)err);
}

/* One turn of the event loop: sleep until something arrives, and pass
   Apple events on. Background-only processes receive nothing else. */
static void RunEventLoopOnce(unsigned long sleepTicks)
{
    EventRecord evt;

    if (WaitNextEvent(everyEvent, &evt, sleepTicks, NULL))
    {
        if (evt.what == kHighLevelEvent)
            (void)AEProcessAppleEvent(&evt);
    }
}

static void QuitWhenAsked(CFragConnectionID *conn)
{
    unsigned long deadline = TickCount() + kQuitGraceTicks;
    long          open     = CountComponentInstances(gComp);

    Log("Quit requested (Shut Down or Restart).");
    LogNum("  codec instances still open", open);

    /* Keep yielding, so the applications that hold those instances get
       the time to close them. 15 ticks between checks. */
    while (open > 0 && TickCount() < deadline)
    {
        RunEventLoopOnce(15);
        open = CountComponentInstances(gComp);
    }

    if (open > 0)
    {
        LogNum("  gave up waiting; instances still open", open);
        return;     /* leave the rest to the Component Manager */
    }

    {
        OSErr err = UnregisterComponent(gComp);
        LogNum("  UnregisterComponent", (long)err);
        if (err == noErr)
        {
            CloseConnection(conn);
            Log("  Unregistered and closed. Goodbye.");
        }
    }
}


/* ==================================================================
   main
   ==================================================================
   Note what is NOT here: no InitWindows, no InitMenus, no InitDialogs
   beyond what StandardAlert needs, and above all no window. InitGraf is
   required before almost any Toolbox call and costs nothing.
*/
int main(void)
{
    CFragConnectionID conn = NULL;
    long              qtVersion = 0;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);

    MaxApplZone();
    MoreMasters();
    MoreMasters();

    SetUpLog();
    Log("");
    Log("=== H264 Auto-Register, startup ===");

    /* Is QuickTime installed at all? Registering an image decompressor
       where there is no QuickTime to call it is pointless.

       Asked through Gestalt, NOT by calling EnterMovies(). The first
       draft called EnterMovies, and its first build on the Mac failed to
       link: EnterMovies lives in QuickTimeLib, which the Toolbox
       stationery does not link. It was never needed anyway --
       RegisterComponent is a Component Manager call, and the bench
       registrar (ComponentProbe.c) registers and self-tests the codec
       without ever initialising the Movie Toolbox. Gestalt is in
       InterfaceLib, so this keeps the app's only dependency the one
       every application already has. */
    if (Gestalt(gestaltQuickTimeVersion, &qtVersion) != noErr || qtVersion == 0)
    {
        Complain("H.264 Decoder",
                 "QuickTime is not installed, so the H.264 decoder was "
                 "not registered.");
        return 0;
    }
    LogNum("  QuickTime present, Gestalt 'qtim' value", qtVersion);

    if (!RegisterTheCodec(&conn))
    {
        /* Already complained and logged. Quitting is correct here:
           there is no connection worth holding. */
        return 0;
    }

    if (conn == NULL)
    {
        /* Somebody else owns the registration. We hold nothing, so we
           have no reason to stay resident. */
        Log("Exiting; another owner holds the codec.");
        return 0;
    }

    Log("Holding the fragment connection for the rest of this session.");

    /* ------------------------------------------------------------------
       Stay alive, doing nothing, until the system shuts down.
       ------------------------------------------------------------------
       This is the load-bearing part of the whole file. The moment this
       process quits, the Component Manager removes the codec it
       registered and CFM unloads the fragment -- measured, see the top
       of the file -- and H.264 movies stop opening.

       The sleep value is the maximum, so the Process Manager gives us no
       time at all until an event arrives, and essentially none ever
       does: a background-only application receives no keyboard, no
       mouse, and no update events. Measured cost is indistinguishable
       from zero.

       The one event that does arrive is the Finder's Quit Application
       at Shut Down or Restart. It MUST be honoured -- see "Quitting when
       the system asks" above for the measured hang when it was not. */
    InstallAppleEventHandlers();

    while (!gQuitRequested)
        RunEventLoopOnce(0x7FFFFFFFUL);

    QuitWhenAsked(&conn);
    return 0;
}
