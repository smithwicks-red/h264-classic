/*
    OneFileProbe.c -- does a ONE-FILE component register and answer?

    Written 11 Nov 2026 (Session 14). Built in the "Probe" CodeWarrior
    project in place of ComponentProbe.c (copy it over that file's name;
    the v5 registrar source is kept in the source tarball).

    The decoder needs two files today: "H264 Decoder" (the code) and
    "H264 Auto-Register" (a faceless helper that registers it with
    RegisterComponent at startup), because the 'thng' -> 'dlle' route
    never reaches PowerPC code on Mac OS 9 ('dlle' is the Windows and
    Mac OS X mechanism). Session 13's plan names the classic one-file
    layout instead:

        layout B1
          'thng' 256   extended template, one platform entry:
                       flags 0, code 'cdec' 256, platform 2 (PowerPC)
          'cdec' 256   the decoder's PEF container, unchanged except
                       that its loader header's main symbol is set to
                       H264CodecComponentDispatchRD (section 1, the
                       data section: a RoutineDescriptor)
          no 'cfrg', empty data fork

    Gestalt('copl') is undefined on this system (-5551, measured), so the
    Component Manager compares the platform field with
    gestaltSysArchitecture, which is 2. That is what B1 uses.

    The file is built on the host (see the Session 14 notes) and lives at
    kTestFileC, OUTSIDE the Extensions folder, so nothing loads it at
    startup: a wrong layout in Extensions can stop QuickTime Player from
    launching (Session 7).

    What this program does:
      1. open the test file's resource fork read-only
      2. RegisterComponentResourceFile(refNum, global = 0): this process
         only. The return value is the number of components registered,
         or a negative error.
      3. find the component it registered: of all 'imdc'/'avc1'
         components, the one whose info string mentions "B1". (The
         running registrar's own copy has a different info string, so
         both can be registered at once.)
      4. OpenComponent, CallComponentVersion, CanDo for the selectors
         QuickTime needs, CloseComponent. Under the old 'dlle' layout the
         caller died with an illegal-instruction error at exactly this
         point, before a single instruction of ours ran. If it gets
         through here, the Component Manager reached our code through
         the file's own resources.
      5. unregister, close the file, write the report, quit.

    Report: share:h264onefile.txt (also on the console).
*/

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <Types.h>
#include <Files.h>
#include <Resources.h>
#include <Components.h>
#include <Errors.h>
#include <Memory.h>

#ifdef __MWERKS__
#include <SIOUX.h>
#endif

#define kTestFileC    "Macintosh HD:H264 Tests:H264 B1 test"
#define kReportPathC  "share:h264onefile.txt"

#define kCodecType    'imdc'
#define kCodecSubType 'avc1'

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

/* Copy a Pascal string held in a handle into a C buffer. */
static void HandleToC(Handle h, char *dst, int dstSize)
{
    int len;

    dst[0] = '\0';
    if (h == NULL || *h == NULL || GetHandleSize(h) < 1) return;
    len = ((unsigned char *)*h)[0];
    if (len > GetHandleSize(h) - 1) len = (int)GetHandleSize(h) - 1;
    if (len > dstSize - 1) len = dstSize - 1;
    BlockMoveData(*h + 1, dst, len);
    dst[len] = '\0';
}

static long CountMatching(OSType type, OSType sub)
{
    ComponentDescription cd;
    memset(&cd, 0, sizeof(cd));
    cd.componentType    = type;
    cd.componentSubType = sub;
    return CountComponents(&cd);
}

int main(void)
{
    FSSpec               spec;
    Str255               path;
    OSErr                err;
    short                refNum = -1;
    long                 registered;
    ComponentDescription looking;
    Component            c, ours = NULL;
    char                 nameC[256], infoC[256];

#ifdef __MWERKS__
    /* Quit without asking to save the console when main returns. */
    SIOUXSettings.asktosaveonclose = false;
    SIOUXSettings.autocloseonquit  = true;
#endif

    gLog = fopen(kReportPathC, "w");

    L("================================================================\n");
    L("One-file component probe (layout B1)\n");
    L("================================================================\n");
    L("Test file: %s\n", kTestFileC);
    L("'imdc'/'avc1' registered before: %ld\n", CountMatching(kCodecType, kCodecSubType));

    CToP(kTestFileC, path);
    err = FSMakeFSSpec(0, 0, path, &spec);
    L("FSMakeFSSpec err = %d\n", (int)err);
    if (err != noErr) goto done;

    refNum = FSpOpenResFile(&spec, fsRdPerm);
    L("FSpOpenResFile refNum = %d, ResError = %d\n", (int)refNum, (int)ResError());
    if (refNum == -1) goto done;

    registered = RegisterComponentResourceFile(refNum, 0);
    L("RegisterComponentResourceFile(global 0) -> %ld\n", registered);
    L("'imdc'/'avc1' registered after:  %ld\n", CountMatching(kCodecType, kCodecSubType));
    if (registered <= 0)
    {
        L("  Nothing registered. 0 = the 'thng' did not match this\n");
        L("  platform or could not be read; negative = an error.\n");
        goto done;
    }

    memset(&looking, 0, sizeof(looking));
    looking.componentType    = kCodecType;
    looking.componentSubType = kCodecSubType;
    c = NULL;
    while ((c = FindNextComponent(c, &looking)) != NULL)
    {
        ComponentDescription got;
        Handle nameH = NewHandle(0), infoH = NewHandle(0);

        GetComponentInfo(c, &got, nameH, infoH, NULL);
        HandleToC(nameH, nameC, sizeof(nameC));
        HandleToC(infoH, infoC, sizeof(infoC));
        L("  found 0x%08lX  manufacturer '%.4s'  flags 0x%08lX\n",
          (unsigned long)c, (char *)&got.componentManufacturer,
          (unsigned long)got.componentFlags);
        L("     name \"%s\"\n     info \"%s\"\n", nameC, infoC);
        if (strstr(infoC, "B1") != NULL) ours = c;
        DisposeHandle(nameH);
        DisposeHandle(infoH);
    }

    if (ours == NULL)
    {
        L("The B1 component was not found among them.\n");
        goto done;
    }

    L("\nOpening the B1 component 0x%08lX ...\n", (unsigned long)ours);
    if (gLog) fflush(gLog);
    {
        ComponentInstance ci = OpenComponent(ours);

        L("  OpenComponent -> 0x%08lX%s\n", (unsigned long)ci,
          ci ? "" : "  <-- FAILED (our code was not reached, or Open failed)");
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
            L("\n  *** B1 WORKS: the Component Manager reached the decoder\n");
            L("      through the file's own 'thng' and 'cdec'. ***\n");
        }
    }

    L("UnregisterComponent -> %d\n", (int)UnregisterComponent(ours));

done:
    if (refNum != -1) CloseResFile(refNum);
    L("================================================================\n");
    L("Report written to %s\n", kReportPathC);
    if (gLog) { fclose(gLog); gLog = NULL; }
    return 0;
}
