pascal ComponentResult H264CodecPreDecompress(H264GlobalsPtr glob, CodecDecompressParams *p)
{
    ImageDescriptionHandle desc;
    Handle avcCHandle = NULL;
    OSErr err;

    LogAppend("--- PreDecompress enter ---");

    if (p == NULL || glob == NULL) return paramErr;

    /* Build the decoder now, on the first call that actually intends to
       decode. See the note on EnsureDecoder: doing this in Open turned
       every allocation failure into an invisible NULL from
       OpenComponent(), whereas here it becomes a real OSErr that
       QuickTime reports. */
    err = EnsureDecoder(glob);
    if (err != noErr)
    {
        LogNum("PreDecompress FAIL EnsureDecoder", (long)err);
        return err;
    }

    desc = p->imageDescription;
    glob->frameWidth  = (**desc).width;
    glob->frameHeight = (**desc).height;
    LogNum("PreDecompress width", (long)glob->frameWidth);
    LogNum("PreDecompress height", (long)glob->frameHeight);

    /* Pull the avcC extension out of the sample description. Signature
       shown here is the documented classic form; some SDK revisions
       spell the selector kAVCCImageDescriptionExtension vs a raw 'avcC'
       OSType -- try the OSType form first since it's the oldest/most
       universally supported spelling. */
    err = GetImageDescriptionExtension(desc, &avcCHandle, 'avcC', 1);
    if (err != noErr || avcCHandle == NULL)
    {
        return badFormat; /* no avcC -> we don't know how to frame the NALs */
    }

    HLock(avcCHandle);
    {
        int ok = FeedParameterSets(glob, (const u8 *)*avcCHandle,
                                    (u32)GetHandleSize(avcCHandle));
        HUnlock(avcCHandle);
        DisposeHandle(avcCHandle);
        if (!ok) return badFormat;
    }

    /* Ask h264bsd what it actually decided after parsing the SPS -- this
       can legitimately differ from the ImageDescription's width/height
       (macroblock rounding), which is exactly why we keep both. */
    glob->frameWidth  = (long)h264bsdPicWidth(glob->h264)  * 16;
    glob->frameHeight = (long)h264bsdPicHeight(glob->h264) * 16;

    {
        u32 cropFlag, left, width, top, height;
        h264bsdCroppingParams(glob->h264, &cropFlag, &left, &width, &top, &height);
        glob->haveCropInfo = cropFlag;
        glob->cropLeft = left; glob->cropTop = top;
        glob->cropWidth = width; glob->cropHeight = height;
    }

    /* Make sure our Annex-B scratch buffer is big enough for the worst
       case single sample we might be handed. p->bufferSize is the
       largest sample size QuickTime tells us to expect for this movie. */
    {
        u32 needed = AVCC_WorstCaseAnnexBSize((u32)p->bufferSize);
        if (needed > glob->annexBScratchSize)
        {
            if (glob->annexBScratch != NULL) DisposePtr((Ptr)glob->annexBScratch);
            glob->annexBScratch = (u8 *)NewPtr(needed);
            if (glob->annexBScratch == NULL) return memFullErr;
            glob->annexBScratchSize = needed;
        }
    }

    /* Negotiate the destination buffer with the ICM. See the header
       comment on this function for why each field is what it is.
       p->capabilities is supplied by the caller and is documented in
       ImageCodec.h as valid for both predecompress and banddecompress,
       so it is safe to write through here. */
    if (p->capabilities != NULL)
    {
        short displayHeight = (**desc).height;

        p->capabilities->wantedPixelSize = 32;
        p->capabilities->extendWidth     = 0;
        p->capabilities->extendHeight    = 0;
        p->capabilities->bandMin         = displayHeight;
        p->capabilities->bandInc         = displayHeight;
        p->capabilities->flags           = 0;

#if kOfferYUVOutput
        /* ------------------------------------------------------------
           Ask for a YUV destination.
           ------------------------------------------------------------
           wantedDestinationPixelTypes is an OSType** in
           CodecDecompressParams -- a handle to a ZERO-TERMINATED list,
           in our order of preference. It has been there since
           QuickTime 2.5, so it is safe against the 6.0.3 runtime and
           against the Universal Interfaces 3.4.1 headers this project
           builds with. (Verified in ImageCodec.h rather than assumed;
           it is NOT a field of CodecCapabilities, which is where I
           first went looking.)

           The order matters and is not arbitrary:

             'y420'  planar 4:2:0, identical in layout to h264bsd's own
                     output. The blit degenerates to BlockMoveData and
                     the colour conversion vanishes.
             '2vuy'  packed 4:2:2. Still no per-pixel arithmetic, and
                     half the bytes of 32-bit RGB.
             32      plain 32-bit RGB -- today's behaviour, last,
                     so there is always something we know works.

           The ICM walks the list and picks the first it can actually
           display on this machine, which is a decision that depends on
           the video card and cannot be predicted from here. That is
           precisely why the list exists and why the blit dispatches on
           the result instead of on this request.

           ------------------------------------------------------------
           WHO OWNS THE HANDLE -- the codec does.
           ------------------------------------------------------------
           The header gives the field's TYPE and nothing about its
           ownership, and the first draft of this code guessed wrong: it
           assumed the ICM hands us a handle to resize and fill. The
           pattern in Apple's own sample decompressors runs the other way
           -- the codec allocates the list, keeps it in its globals,
           points p->wantedDestinationPixelTypes at it, and disposes of
           it in Close. Under the first draft the field would have been
           NULL on entry (so YUV was silently never offered) or
           uninitialised (so SetHandleSize ran on garbage). Caught by
           re-reading, before it ever ran.

           So: allocate once per instance, reuse on later PreDecompress
           calls, never write through whatever pointer the field held on
           entry. If the allocation fails we leave wantedPixelSize at 32
           and carry on exactly as before -- there is no failure mode
           here worse than "no faster than yesterday".

           ------------------------------------------------------------
           What "YUV output" does and does not buy
           ------------------------------------------------------------
           Be precise about the prize. A YUV destination does NOT make the
           colour conversion disappear -- the screen is still RGB, so
           something still converts. It moves the conversion out of our
           plain C table lookups and into QuickTime's own transfer code,
           which may be faster, may be vectorised on a G4, and on some
           video hardware may be done by the card's overlay. Whether that
           beats our blit on a 500 MHz G3 is an empirical question for the
           Pismo, not something to assert from here. What IS certain is
           that our side of the work becomes nearly free. */
        if (glob->wantedPixelTypes == NULL)
            glob->wantedPixelTypes =
                (OSType **)NewHandleClear((Size)(4 * sizeof(OSType)));

        if (glob->wantedPixelTypes != NULL)
        {
            OSType *list = *glob->wantedPixelTypes;

            list[0] = kYUV420CodecType;        /* 'y420' */
            list[1] = k422YpCbCr8CodecType;    /* '2vuy' */
            list[2] = (OSType)k32ARGBPixelFormat;
            list[3] = 0;                       /* terminator */

            p->wantedDestinationPixelTypes = glob->wantedPixelTypes;

            /* Documented convention: zero here means "consult the list
               instead of the depth". */
            p->capabilities->wantedPixelSize = 0;

            LogAppend("PreDecompress: offered y420 / 2vuy / 32-bit RGB");
        }
        else
        {
            LogAppend("PreDecompress: no memory for the pixel-type list; "
                      "staying on 32-bit RGB");
        }
#endif
    }

    return noErr;
}
