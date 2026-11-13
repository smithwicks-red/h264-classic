/*
    ComponentProbe.c  --  version 5: the REGISTRAR

    ------------------------------------------------------------------
    What this file is now, and why it changed
    ------------------------------------------------------------------
    It started as a diagnostic and it has finished the diagnosis. The
    conclusion, established by measurement over several sessions, is
    that on this system the resource-driven route into a component --
    'thng' -> 'dlle' -> FindSymbol -> call -- does not work, while
    handing the Component Manager the entry point directly does.

    The evidence, in order:

      - the component registers from its 'thng': it shows up as one of
        88 'imdc' decompressors with the right subtype, name and info
      - its 'cfrg' is correct: usage 0 (kImportLibraryCFrag -- 0 is the
        good value, kApplicationCFrag is 1), data fork, length to EOF
      - the fragment prepares: GetDiskFragment returns 0
      - both symbols resolve: the dispatcher as class 2 (TVector), its
        RoutineDescriptor as class 1 (data)
      - calling the dispatcher directly with the Version selector
        returns 0x00010000, so the dispatch table is right
      - RegisterComponent() with the RoutineDescriptor gives a component
        that opens, answers Version and CanDo, and closes cleanly, with
        the codec's own log confirming Open ran

    and against all that:

      - with the extended 'thng' template, OpenComponent returns NULL
        and our code is never entered
      - with the simple 13-field template, the caller dies with a type 3
        (illegal instruction) before our code is entered
      - that crash happens with EVERY symbol tried in the 'dlle':
        the dispatcher, its RoutineDescriptor, and -- decisively -- a
        four-line throwaway entry point that did nothing but write one
        line to a log and return. It never wrote the line.

    A stub that cannot survive being called is proof that the address
    being called is not the stub. So the 'dlle' path is abandoned, and
    this program does the job the resources were supposed to do.

    ------------------------------------------------------------------
    How it works
    ------------------------------------------------------------------
      1. GetDiskFragment on the codec file, and HOLD the connection.
         The fragment must stay prepared for as long as anything might
         call into it, so the connection is deliberately never closed.
      2. FindSymbol for the RoutineDescriptor. RegisterComponent is
         declared to take a ComponentRoutineUPP, and on classic PowerPC
         a UPP is a pointer to a RoutineDescriptor -- not the TVector.
         Getting this backwards hangs the machine, which is how we
         learned it.
      3. RegisterComponent with the real 'imdc'/'avc1'/'wY9H' triple and
         global = 1, so every application sees the codec rather than
         just this one.
      4. Open it and exercise Version and CanDo as a self-test, so the
         report says whether the codec is actually usable, not merely
         registered.
      5. Park in getchar(). This is load-bearing, not laziness: a
         component registered by an application is unregistered when
         that application quits, and the code fragment would unload with
         it. SIOUX runs a normal event loop while blocked on input, so
         parking here keeps both the registration and the fragment alive
         while QuickTime uses them.

    Leave this running, then open an H.264 movie in QuickTime Player.

    ------------------------------------------------------------------
    Packaging, once this is confirmed working
    ------------------------------------------------------------------
    Drop it in the System Folder's Startup Items and it runs at every
    boot. Nicer still would be a faceless background application so no
    console window appears, but that is cosmetics; the mechanism is the
    same either way.

    Note the codec file itself must NOT carry a 'thng' resource any
    more. With one, the Component Manager finds it during startup
    scanning, follows the broken 'dlle' path and crashes QuickTime
    before its window even appears. Without one the file is an inert
    CFM library sitting in Extensions, which is exactly what we want --
    this program is what turns it into a component.
*/

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <Types.h>
#include <Files.h>
#include <Resources.h>
#include <Components.h>
#include <CodeFragments.h>
#include <Errors.h>
#include <Memory.h>

/* Where the codec fragment lives. It stays in Extensions purely out of
   tidiness -- nothing loads it from there automatically any more. */
#define kCodecPathC   "Macintosh HD:System Folder:Extensions:H264 Decoder"

/* The RoutineDescriptor, NOT the bare dispatcher. See the note above. */
#define kSymbolRD     "H264CodecComponentDispatchRD"
#define kSymbolTV     "H264CodecComponentDispatch"

#define kCodecType    'imdc'
#define kCodecSubType 'avc1'
#define kCodecManu    'wY9H'

#define kReportPathC  "share:h264register.txt"

/* CFragLoadOptions kLoadCFrag / kReferenceCFrag. Written as a literal
   because the spelling of this constant moved between SDK versions. */
#define kLoadOptions  1

static FILE *gLog = NULL;

static void L(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    if (gLog != NULL)
    {
        va_start(ap, fmt);
        vfprintf(gLog, fmt, ap);
        va_end(ap);
        fflush(gLog);
    }
}

static void CToP(const char *src, unsigned char *dst)
{
    int len = (int)strlen(src);
    if (len > 255) len = 255;
    dst[0] = (unsigned char)len;
    if (len > 0) BlockMoveData(src, dst + 1, len);
}

static void PStrToC(ConstStr255Param src, char *dst, int dstSize)
{
    int len = (src == NULL) ? 0 : src[0];
    if (len > dstSize - 1) len = dstSize - 1;
    if (len > 0) BlockMoveData(src + 1, dst, len);
    dst[len] = '\0';
}

/* A Handle holding a Pascal string, which is the shape RegisterComponent
   wants for the name and info arguments. Passing NULL is legal but then
   the component has no name, and a nameless codec is confusing to find
   in a component list -- worth the eight lines. */
static Handle PascalStringHandle(const char *text)
{
    Handle h;
    int    len = (int)strlen(text);

    if (len > 255) len = 255;
    h = NewHandle((Size)(len + 1));
    if (h == NULL) return NULL;

    HLock(h);
    ((unsigned char *)*h)[0] = (unsigned char)len;
    BlockMoveData(text, (*h) + 1, len);
    HUnlock(h);
    return h;
}

static long CountMatching(OSType type, OSType sub, OSType manu)
{
    ComponentDescription cd;
    memset(&cd, 0, sizeof(cd));
    cd.componentType         = type;
    cd.componentSubType      = sub;
    cd.componentManufacturer = manu;
    return CountComponents(&cd);
}

int main(void)
{
    FSSpec            spec;
    Str255            path;
    Str255            fragName;
    Str255            errName;
    Str255            sym;
    OSErr             err;
    CFragConnectionID connID  = NULL;
    Ptr               mainAddr = NULL;
    Ptr               rdAddr   = NULL;
    CFragSymbolClass  cls      = 0;
    char              errBuf[256];

    gLog = fopen(kReportPathC, "w");

    L("================================================================\n");
    L("H.264 codec registrar\n");
    L("================================================================\n");
    L("Registers the decoder by handing the Component Manager its entry\n");
    L("point directly. The 'thng'/'dlle' route does not work on this\n");
    L("system -- see the notes at the top of this source file.\n");

    L("\nBefore: 'imdc'/'avc1' registered = %ld\n",
      CountMatching(kCodecType, kCodecSubType, 0));

    /* ---- locate the codec file ---- */
    L("\nCodec fragment: %s\n", kCodecPathC);
    CToP(kCodecPathC, path);

    err = FSMakeFSSpec(0, 0, path, &spec);
    if (err != noErr)
    {
        L("FSMakeFSSpec failed, err = %d\n", (int)err);
        L("  (-43 = not found. Check the volume name and that the file\n");
        L("   really is in System Folder:Extensions.)\n");
        goto park;
    }

    /* ---- prepare the fragment, and keep it prepared ---- */
    fragName[0] = 0;
    errName[0]  = 0;

    err = GetDiskFragment(&spec, 0, kCFragGoesToEOF, fragName,
                          kLoadOptions, &connID, &mainAddr, errName);
    PStrToC(errName, errBuf, sizeof(errBuf));

    L("GetDiskFragment err = %d\n", (int)err);
    if (errBuf[0]) L("  errName = \"%s\"\n", errBuf);
    if (err != noErr)
    {
        L("  Could not prepare the fragment, so there is nothing to\n");
        L("  register. -2804 cfragNoLibraryErr, -2807 cfragUnresolvedErr\n");
        L("  (errName names the missing import).\n");
        goto park;
    }
    L("  connID = 0x%08lX  (held open deliberately)\n", (unsigned long)connID);

    /* ---- resolve the RoutineDescriptor ---- */
    CToP(kSymbolRD, sym);
    err = FindSymbol(connID, sym, &rdAddr, &cls);
    L("FindSymbol(\"%s\") err = %d\n", kSymbolRD, (int)err);
    if (err != noErr || rdAddr == NULL)
    {
        L("  Without the RoutineDescriptor there is nothing to hand to\n");
        L("  RegisterComponent. Check PPC PEF -> Export Symbols is set\n");
        L("  to \"Use #pragma\" and that the #pragma export bracket in\n");
        L("  H264Codec.c still surrounds ComponentDispatchHelper.c.\n");
        goto park;
    }
    L("  addr = 0x%08lX, class = %d  (1 = data, which a RoutineDescriptor is)\n",
      (unsigned long)rdAddr, (int)cls);

    /* ---- register it ---- */
    {
        ComponentDescription cd;
        Component            comp;
        Handle               nameH;
        Handle               infoH;

        memset(&cd, 0, sizeof(cd));
        cd.componentType         = kCodecType;
        cd.componentSubType      = kCodecSubType;
        cd.componentManufacturer = kCodecManu;
        cd.componentFlags        = 0;
        cd.componentFlagsMask    = 0;

        nameH = PascalStringHandle("H.264 (Software)");
        infoH = PascalStringHandle(
            "H.264/AVC Baseline decoder for QuickTime, powered by h264bsd.");

        /* global = 1: visible to every application, not just this one. */
        comp = RegisterComponent(&cd, (ComponentRoutineUPP)rdAddr, 1,
                                 nameH, infoH, NULL);

        L("\nRegisterComponent (global) -> 0x%08lX\n", (unsigned long)comp);
        if (comp == NULL)
        {
            L("  Registration failed.\n");
            goto park;
        }

        L("After:  'imdc'/'avc1' registered = %ld\n",
          CountMatching(kCodecType, kCodecSubType, 0));

        /* ---- self-test: is it actually usable? ---- */
        {
            ComponentInstance ci = OpenComponent(comp);

            L("\nSelf-test\n");
            L("  OpenComponent -> 0x%08lX %s\n",
              (unsigned long)ci, ci ? "" : "  <-- FAILED");

            if (ci != NULL)
            {
                L("  CallComponentVersion       = 0x%08lX (expect 0x00010000)\n",
                  (unsigned long)CallComponentVersion(ci));
                L("  CanDo(0x00 GetCodecInfo)   = %ld (expect 1)\n",
                  (long)CallComponentCanDo(ci, 0x0000));
                L("  CanDo(0x05 PreDecompress)  = %ld (expect 1)\n",
                  (long)CallComponentCanDo(ci, 0x0005));
                L("  CanDo(0x06 BandDecompress) = %ld (expect 1)\n",
                  (long)CallComponentCanDo(ci, 0x0006));
                CloseComponent(ci);
                L("  closed.\n");
                L("\n  *** The codec is registered and answering. ***\n");
                L("  Leave this program running and open an H.264 movie\n");
                L("  in QuickTime Player.\n");
            }
        }
    }

park:
    L("\n================================================================\n");
    L("Report written to %s\n", kReportPathC);
    L("================================================================\n");

    if (gLog) { fflush(gLog); }

    /* Do NOT close the fragment connection and do NOT unregister.
       Quitting undoes both. SIOUX pumps events while blocked here, so
       the machine stays usable and the codec stays available. */
    printf("\nLeave this window open. Press return ONLY when you want to\n");
    printf("unregister the codec and quit.\n");
    getchar();

    if (gLog) { fclose(gLog); gLog = NULL; }
    return 0;
}
