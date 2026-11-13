typedef struct {
    ComponentInstance   self;
    ComponentInstance   target;        /* set by the Target selector */

    storage_t          *h264;          /* h264bsd decoder instance */
    int                 h264Inited;    /* have we called h264bsdInit yet? */

    u32                 lengthSize;    /* NAL length-prefix width from avcC: 1/2/4 */

    u8                 *annexBScratch; /* scratch buffer for length->AnnexB conversion */
    u32                 annexBScratchSize;

    long                frameWidth;
    long                frameHeight;

    /* Cropping: h264bsd decodes in 16x16 macroblock multiples and reports
       the "clean aperture" crop separately (h264bsdCroppingParams). The
       ImageDescription's width/height is the *display* size we must
       actually paint into -- almost always smaller than or equal to the
       decoded macroblock-aligned buffer. */
    u32                 cropLeft, cropTop, cropWidth, cropHeight;
    u32                 haveCropInfo;

    /* The zero-terminated list of destination pixel formats we offer the
       ICM in PreDecompress. The CODEC owns this handle: we allocate it,
       point p->wantedDestinationPixelTypes at it, and dispose of it in
       Close. See the long comment in PreDecompress for why ownership
       runs this way round. Starts NULL because Open uses NewPtrClear. */
    OSType            **wantedPixelTypes;

    /* The most recent picture h264bsd handed us, and a running count of
       pictures output on this instance. BandDecompress uses them to
       guarantee that EVERY call paints the destination: when a sample
       yields no new picture (QuickTime re-sending a sample it already
       sent, a sample that is only parameter sets or SEI, or a decode
       error) the last picture is blitted again instead of leaving
       whatever happened to be in QuickTime's buffer. The pointer is into
       h264bsd's DPB, so it is forgotten whenever the DPB can be
       reallocated, which is only ever at SPS activation -- the call that
       returns HDRS_RDY. (Re-feeding the same SPS in PreDecompress is a
       no-op inside h264bsd; a CHANGED SPS only deactivates the old one,
       and the DPB is rebuilt at the next activation, i.e. HDRS_RDY.) It
       deliberately survives PreDecompress: the case it exists for is the
       poster-frame instance being re-prepared for playback and sent the
       same key frame again. */
    u8                 *lastPic;
    unsigned long       picsOut;

} H264Globals, *H264GlobalsPtr;
