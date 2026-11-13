static ComponentResult BandDecompressWork(H264GlobalsPtr glob,
                                          CodecDecompressParams *params)
{
    u32 annexBSize;
    u32 readBytes = 0;
    u32 decodeStatus;
    long dstRowBytes;
    unsigned long picsBefore;

    /* Logged only for the first few frames. This runs once per frame, and
       the logger flushes the volume on every line, so leaving it on for a
       whole movie would dominate the decode time and tell us nothing the
       first frames did not. */
    static long sBandCalls = 0;
    if (sBandCalls < 4) LogNum("--- BandDecompress enter #", sBandCalls);
    sBandCalls++;

    if (glob == NULL || params == NULL) return paramErr;
    if (glob->h264 == NULL) return internalComponentErr;

    picsBefore = glob->picsOut;

    annexBSize = AVCC_ConvertSampleToAnnexB((const u8 *)params->data,
                                             (u32)params->bufferSize,
                                             glob->lengthSize,
                                             glob->annexBScratch,
                                             glob->annexBScratchSize);
    if (annexBSize == 0) return badFormat; /* corrupt/truncated sample */

    /* ------------------------------------------------------------------
       h264bsdDecode consumes ONE NAL unit per call.
       ------------------------------------------------------------------
       This was the bug that produced a blank grey first frame and then
       an accumulating mess. A QuickTime sample is not one NAL unit: for
       this ffmpeg-produced clip each sample is an SEI followed by the
       slice. Calling h264bsdDecode once consumed only the SEI and the
       actual picture was never decoded at all, so the very first output
       was h264bsd's initialised buffer -- a flat grey field, Y=Cb=Cr=128.

       That is also why the damage got worse over time rather than
       staying constant: every later frame predicts from earlier ones, so
       an undecoded IDR poisons everything after it. The parts that did
       render -- the frame counter, the moving diagonal -- were the
       intra-coded macroblocks being redrawn from scratch each frame,
       sitting on top of a background that had never been decoded.

       The API is explicit about this: it returns readBytes, and the
       caller advances and calls again until the buffer is exhausted.

       Pictures are drained inside the loop rather than after it, because
       a multi-slice sample can complete a picture partway through the
       buffer, and h264bsd hands each one over exactly once.

       ------------------------------------------------------------------
       readBytes == 0 means "call me again with THIS buffer", not "stuck".
       ------------------------------------------------------------------
       h264bsd returns zero bytes consumed on purpose in two situations,
       and in both the NAL at this offset has NOT been decoded yet:

         * H264BSD_HDRS_RDY -- the first slice of a picture activated a
           new SPS. This happens on the FIRST sample every fresh decoder
           instance ever sees, i.e. on every IDR that opens a
           decompression sequence.
         * H264BSD_PIC_RDY  -- a new slice arrived while the previous
           picture was still incomplete, so h264bsd concealed and
           finished that one first.

       It sets prevBufNotFinished and expects the SAME pointer back, and
       it decodes the stored NAL on that second call. (h264bsd_decoder.c,
       the "prevBufNotFinished && byteStrm == prevBufPointer" branch.)

       The first version of this loop treated readBytes == 0 as "no
       progress" and broke out. Measured consequence, 2026-09-23: the
       poster frame of every movie was blank -- the IDR on the instance
       QuickTime uses to draw it was activated and then abandoned. Playback
       only came out right because QuickTime happened to send the IDR a
       second time to the same instance, and that second call slipped
       down the "same pointer" path. The previous session's explanation
       ("h264bsd only hands the first picture over once the next one
       begins") was wrong: for this POC-type-2 stream h264bsd outputs
       immediately; we simply never asked it to finish.

       The guard against spinning stays, because an infinite loop inside
       a QuickTime callback on Mac OS 9 is a hung machine and a power
       cycle. It just allows the retry the API asks for: up to
       kMaxDecodeStalls consecutive zero-byte returns at one offset, then
       give up on the rest of this sample. */
    {
        u32 offset = 0;
        int stalls = 0;           /* consecutive zero-byte returns */

        while (offset < annexBSize)
        {
            readBytes    = 0;
            decodeStatus = h264bsdDecode(glob->h264,
                                          glob->annexBScratch + offset,
                                          annexBSize - offset,
                                          0, &readBytes);

            if (decodeStatus == H264BSD_ERROR ||
                decodeStatus == H264BSD_MEMALLOC_ERROR)
            {
                return internalComponentErr;
            }

            /* A newly activated SPS may have reallocated the DPB that
               lastPic points into. Forget it until a new picture arrives. */
            if (decodeStatus == H264BSD_HDRS_RDY)
                glob->lastPic = NULL;

            /* Drain on PIC_RDY whether or not bytes were consumed: the
               concealment case finishes a picture AND asks to be called
               again, and that picture should not wait for a later call. */
            if (decodeStatus == H264BSD_PIC_RDY)
                DrainOutputPictures(glob, params);

            if (readBytes == 0)
            {
                static long sStallLogs = 0;
                if (sStallLogs < 4)
                {
                    LogNum("  decode: 0 bytes consumed, retrying same NAL, status",
                           (long)decodeStatus);
                    sStallLogs++;
                }
                if (++stalls > kMaxDecodeStalls) break;   /* never spin */
                continue;                                 /* same offset */
            }

            stalls  = 0;
            offset += readBytes;
        }
    }

    /* PixMap.rowBytes packs flags (e.g. pixel-depth/pixmap-vs-bitmap bit)
       into its top two bits alongside the real byte count -- always mask
       with 0x3FFF before using it as a stride, per Inside Macintosh. */
    dstRowBytes = (long)(params->dstPixMap.rowBytes & 0x3FFF);

    /* One-shot description of what QuickTime actually handed us. The
       blit assumes a 32-bit xRGB PixMap starting at baseAddr row 0; if
       any of that is untrue the picture would be structurally mangled,
       so these numbers rule several theories in or out at a glance.
       pixelSize in particular: we ask for 32 in PreDecompress, but
       asking is not getting. */
    if (sBandCalls == 1)
    {
        LogNum("  dstPixMap.pixelSize", (long)params->dstPixMap.pixelSize);
        LogNum("  dstPixMap.rowBytes (masked)", dstRowBytes);
        LogNum("  dstPixMap.bounds.left", (long)params->dstPixMap.bounds.left);
        LogNum("  dstPixMap.bounds.top", (long)params->dstPixMap.bounds.top);
        LogNum("  dstPixMap.bounds.right", (long)params->dstPixMap.bounds.right);
        LogNum("  dstPixMap.bounds.bottom", (long)params->dstPixMap.bounds.bottom);
        LogNum("  dstRect.left", (long)params->dstRect.left);
        LogNum("  dstRect.top", (long)params->dstRect.top);
        LogNum("  dstRect.right", (long)params->dstRect.right);
        LogNum("  dstRect.bottom", (long)params->dstRect.bottom);
        /* The instance pointer identifies WHICH globals block this call
           landed on. QuickTime opens several instances while it
           negotiates, and the one that gets BandDecompress is not always
           the one PreDecompress ran on -- which is why frameWidth read
           back as 0 here and the blit drew nothing. Kept as a log line
           rather than relied on: the blit now asks h264bsd for the
           geometry every frame instead of trusting these. */
        LogNum("  instance (glob)", (long)glob);
        LogNum("  glob->frameWidth (cached, may be 0)", (long)glob->frameWidth);
        LogNum("  h264bsdPicWidth*16", (long)h264bsdPicWidth(glob->h264) * 16);
        LogNum("  h264bsdPicHeight*16", (long)h264bsdPicHeight(glob->h264) * 16);
    }

    /* Anything still queued (reorder delay, or a sample whose last
       NAL completed a picture) goes out here. */
    DrainOutputPictures(glob, params);

    /* Every call must leave a picture in the destination. QuickTime
       treats BandDecompress as "decompress THIS sample into THIS
       buffer", and it does send samples that produce nothing new:
       measured on the host harness (tools/decode_run), a key frame sent
       twice to one instance -- the pattern the Mac log shows for poster
       frame then playback -- is recognised by h264bsd as a redundant
       copy of the picture it just finished and skipped. Without this,
       that call would leave QuickTime's offscreen buffer holding
       whatever it held before. Repeating the last picture is exactly
       right for a re-sent sample and the least-bad answer for a sample
       that decoded to nothing. */
    if (glob->picsOut == picsBefore && glob->lastPic != NULL)
    {
        BlitPicture(glob, glob->lastPic, params);
        if (kH264Diagnostics) gStatRepeats++;
    }

    return noErr;
}

/* The entry point QuickTime actually calls. At kH264Diagnostics 0 the
   `if` blocks fold away and this is one extra function call per frame. */
pascal ComponentResult H264CodecBandDecompress(H264GlobalsPtr glob,
                                                CodecDecompressParams *params)
{
    ComponentResult result;
    UnsignedWide    t0, t1;

    if (kH264Diagnostics) Microseconds(&t0);

    result = BandDecompressWork(glob, params);

    if (kH264Diagnostics)
    {
        Microseconds(&t1);
        if (gStatBandCalls >= kStatSkipCalls)
        {
            unsigned long us = (unsigned long)(t1.lo - t0.lo);
            gStatDecodeMicros += us;
            if (us > gStatMaxMicros) gStatMaxMicros = us;
            gStatTimedCalls++;
        }
        gStatBandCalls++;
    }

    return result;
}

/* Report the counters. Called from Close, which QuickTime invokes for
   every instance it opens -- several per movie, most of them short-lived
   preflight instances -- so this only prints when new pictures have been
   shown since the last report. The final report is the one that covers
   the whole playback. */
static void LogPlaybackStats(void)
{
    unsigned long wallMicros, runPics, runMicros, runCentis;

    if (!kH264Diagnostics) return;
    if (gStatPictures == 0 || gStatPictures == gStatPrintedAt) return;
    gStatPrintedAt = gStatPictures;

    LogAppend("=== playback statistics ===");
    LogNum("STATS pictures shown", (long)gStatPictures);
    LogNum("STATS BandDecompress calls", (long)gStatBandCalls);
    LogNum("STATS calls that repeated the last picture", (long)gStatRepeats);
    LogNum("STATS blits to y420", (long)gStatBlitY420);
    LogNum("STATS blits to 2vuy", (long)gStatBlit2vuy);
    LogNum("STATS blits to 32-bit RGB", (long)gStatBlitRGB32);
    if (gStatBlitOther > 0)
        LogNum("STATS blits to an UNHANDLED format", (long)gStatBlitOther);
    if (gStatTimedCalls > 0)
    {
        LogNum("STATS avg us per call (after first 8)",
               (long)(gStatDecodeMicros / gStatTimedCalls));
        LogNum("STATS max us per call (after first 8)", (long)gStatMaxMicros);
    }

    wallMicros = gStatLastPicLo - gStatFirstPicLo;     /* unsigned: wrap-safe */
    LogNum("STATS wall ms first->last picture, incl. pauses",
           (long)(wallMicros / 1000UL));

    /* The longest run, counting the one still in progress -- without
       closing it, because QuickTime closes short-lived instances in the
       middle of playback and this report must not split the run. */
    runPics   = gStatBestRunPics;
    runMicros = gStatBestRunMicros;
    if (gStatRunPics > 1 &&
        (unsigned long)(gStatLastPicLo - gStatRunStartLo) > runMicros)
    {
        runPics   = gStatRunPics;
        runMicros = gStatLastPicLo - gStatRunStartLo;
    }
    runCentis = runMicros / 10000UL;
    LogNum("STATS playback run pictures", (long)runPics);
    LogNum("STATS playback run ms", (long)(runMicros / 1000UL));
    if (runCentis > 0 && runPics > 1)
        LogNum("STATS playback run fps x100",
               (long)(((runPics - 1) * 10000UL) / runCentis));
}
