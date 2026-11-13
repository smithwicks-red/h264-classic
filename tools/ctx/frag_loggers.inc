#define kLogPathC   "share:h264log.txt"

/* Where the log goes when there is no "share" volume.

   The share only exists inside the UTM VM, where the host reads it.
   On a real Mac -- the Pismo, the TiBook -- FSMakeFSSpec on
   "share:h264log.txt" fails with nsvErr, and the logger used to fall
   silent there: exactly the place a log is most wanted, since nobody can
   read that machine's state from outside. So it now falls back to a
   text file on the boot volume's DESKTOP, located with FindFolder so it
   works whatever the volume is called:

       <boot volume>:Desktop Folder:H264 Decoder Log

   which appears as an icon on the desktop and opens in SimpleText.

   The raw-YUV dumps (DumpBytesToShare) deliberately do NOT fall back:
   they are 130K each and only useful to someone with ffmpeg on the far
   end of the share. */
#define kLogFallbackName  "\pH264 Decoder Log"

static short LogCStrLen(const char *s)
{
    short n = 0;
    while (s[n] != '\0' && n < 1000) n++;
    return n;
}

/* Resolve the log file's FSSpec once per process and remember it.
   Returns false if there is nowhere to log, in which case the logger
   stays silent -- a diagnostic must never be the thing that fails. */
static Boolean LogGetSpec(FSSpec *specOut)
{
    static short   sState = 0;      /* 0 untried, 1 usable, -1 nowhere */
    static FSSpec  sSpec;

    if (sState == 0)
    {
        Str255 path;
        short  n;
        OSErr  err;

        n = LogCStrLen(kLogPathC);
        path[0] = (unsigned char)n;
        BlockMoveData(kLogPathC, path + 1, n);

        /* fnfErr is success here: the folder exists and the spec is
           filled in, the file just has not been created yet. */
        err = FSMakeFSSpec(0, 0, path, &sSpec);
        if (err == noErr || err == fnfErr)
        {
            sState = 1;
        }
        else
        {
            short vRefNum;
            long  dirID;

            sState = -1;
            if (FindFolder(kOnSystemDisk, kDesktopFolderType,
                           kDontCreateFolder, &vRefNum, &dirID) == noErr)
            {
                err = FSMakeFSSpec(vRefNum, dirID, kLogFallbackName, &sSpec);
                if (err == noErr || err == fnfErr) sState = 1;
            }
        }
    }

    if (sState != 1) return false;
    *specOut = sSpec;
    return true;
}

static void LogAppend(const char *msg)
{
    FSSpec spec;
    short  refNum;
    long   count;
    OSErr  err;
    char   cr = '\r';

    if (!kH264Diagnostics) return;
    if (!LogGetSpec(&spec)) return;

    err = FSpOpenDF(&spec, fsWrPerm, &refNum);
    if (err == fnfErr)
    {
        /* 'ttxt'/'TEXT' so it opens in SimpleText inside the VM and comes
           across the bridge as readable text on the host side. */
        /* Script code 0 (smRoman) rather than smSystemScript: that
           constant lives in Script.h, which this file does not include
           and does not otherwise need. A literal keeps the logger's
           dependency list at zero. */
        if (FSpCreate(&spec, 'ttxt', 'TEXT', 0) != noErr) return;
        err = FSpOpenDF(&spec, fsWrPerm, &refNum);
    }
    if (err != noErr) return;

    SetFPos(refNum, fsFromLEOF, 0);

    count = LogCStrLen(msg);
    if (count > 0) FSWrite(refNum, &count, (Ptr)msg);
    count = 1;
    FSWrite(refNum, &count, (Ptr)&cr);

    FSClose(refNum);
    FlushVol(NULL, spec.vRefNum);
}

/* ------------------------------------------------------------------
   Raw-buffer dump, for comparing against a known-good reference
   ------------------------------------------------------------------
   The codec now decodes and paints, and the picture is structurally
   right -- the frame-counter text is legible, the test pattern's
   diagonal sweep is in the right place -- but the colour is wrong.
   Wrong colour with correct structure is a narrow class of bug, and
   guessing between its members (chroma plane offset, chroma stride,
   destination pixel depth, channel order) wastes a build cycle each.

   So instead: write the exact bytes h264bsd handed us to the shared
   volume, where the host can decode the same frame with ffmpeg and
   compare the two planar buffers directly. That answers the only
   question that matters -- is the decoder's output wrong, or is my
   conversion of it wrong -- without another round of theorising.

   Binary, so no line-ending translation: type 'BINA'. Same
   File-Manager-only approach as the logger above, for the same reason
   (a component must not depend on the host application's runtime). */
static void DumpBytesToShare(const char *pathC, const void *data, long len)
{
    Str255 path;
    FSSpec spec;
    short  refNum;
    long   count;
    short  n;
    OSErr  err;

    if (!kH264Diagnostics) return;
    if (data == NULL || len <= 0) return;

    n = LogCStrLen(pathC);
    path[0] = (unsigned char)n;
    BlockMoveData(pathC, path + 1, n);

    err = FSMakeFSSpec(0, 0, path, &spec);
    if (err == fnfErr)
    {
        if (FSpCreate(&spec, 'ttxt', 'BINA', 0) != noErr) return;
    }
    else if (err != noErr)
    {
        return;
    }

    if (FSpOpenDF(&spec, fsWrPerm, &refNum) != noErr) return;

    SetEOF(refNum, 0);
    count = len;
    FSWrite(refNum, &count, (Ptr)data);
    FSClose(refNum);
    FlushVol(NULL, spec.vRefNum);
}

/* "label <signed decimal>" without sprintf, for the same runtime-
   independence reason as above. */
static void LogNum(const char *label, long v)
{
    char          buf[128];
    char          digits[16];
    short         i = 0, t = 0, j;
    unsigned long uv;

    while (label[i] != '\0' && i < 100) { buf[i] = label[i]; i++; }
    buf[i++] = ' ';

    if (v < 0) { buf[i++] = '-'; uv = (unsigned long)(-v); }
    else       { uv = (unsigned long)v; }

    if (uv == 0) digits[t++] = '0';
    while (uv > 0 && t < 15) { digits[t++] = (char)('0' + (uv % 10)); uv /= 10; }
    for (j = (short)(t - 1); j >= 0; j--) buf[i++] = digits[j];

    buf[i] = '\0';
    LogAppend(buf);
}
