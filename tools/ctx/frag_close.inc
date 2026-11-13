pascal ComponentResult H264CodecClose(H264GlobalsPtr glob, ComponentInstance self)
{
    #pragma unused(self)

    LogAppend("--- Close enter ---");
    LogPlaybackStats();

    if (glob != NULL)
    {
        if (glob->h264 != NULL)
        {
            if (glob->h264Inited) h264bsdShutdown(glob->h264);
            h264bsdFree(glob->h264);
            glob->h264 = NULL;
        }
        if (glob->annexBScratch != NULL)
        {
            DisposePtr((Ptr)glob->annexBScratch);
            glob->annexBScratch = NULL;
        }
        if (glob->wantedPixelTypes != NULL)
        {
            DisposeHandle((Handle)glob->wantedPixelTypes);
            glob->wantedPixelTypes = NULL;
        }
        DisposePtr((Ptr)glob);
    }
    return noErr;
}
