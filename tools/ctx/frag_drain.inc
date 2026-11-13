static void DrainOutputPictures(H264GlobalsPtr glob, CodecDecompressParams *params)
{
    static long sPicCount = 0;

    u32 picId, isIdr, numErrMbs;
    u8 *pic;

    if (glob == NULL || glob->h264 == NULL || params == NULL) return;

    while ((pic = h264bsdNextOutputPicture(glob->h264, &picId, &isIdr,
                                            &numErrMbs)) != NULL)
    {
        long planarBytes = (long)h264bsdPicWidth(glob->h264) * 16 *
                           (long)h264bsdPicHeight(glob->h264) * 16 * 3 / 2;

        if (sPicCount < 3) LogNum("  output picture #", sPicCount);
        if (sPicCount == 0)
            DumpBytesToShare("share:h264pic0.yuv", pic, planarBytes);
        /* The "last picture" dump used to run on EVERY frame. It is
           already inert at kH264Diagnostics 0, but bounded here as well
           so that turning diagnostics on to chase some unrelated bug
           does not silently drop playback to a crawl -- 130K of
           synchronous shared-volume I/O per frame is around four
           megabytes a second at 30fps, inside the decode loop. Three
           frames is enough to compare against the reference. */
        if (sPicCount < 3)
            DumpBytesToShare("share:h264piclast.yuv", pic, planarBytes);
        sPicCount++;

        BlitPicture(glob, pic, params);
        glob->lastPic = pic;
        glob->picsOut++;

        if (kH264Diagnostics)
        {
            UnsignedWide now;
            Microseconds(&now);
            StatNotePicture((unsigned long)now.lo);
        }
    }
}
