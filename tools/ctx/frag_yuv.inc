#if kOfferYUVOutput

/* ==================================================================
   The YUV output paths
   ==================================================================
   h264bsd hands us planar 4:2:0 -- a Y plane at full resolution
   followed by Cb and Cr planes at half resolution in both directions.
   That is byte-for-byte what 'y420' wants, and two-thirds of what
   '2vuy' wants. The colour conversion above exists only because a
   32-bit RGB destination needs one; given a YUV destination WE do not
   have to do it. QuickTime or the video hardware still does, on its way
   to the RGB screen -- the gain is that their converter may be faster
   than ours, not that the work disappears.

   These two functions deliberately repeat the geometry work that
   CopyPlanarYUVToPixMap does rather than refactoring it out. The RGB
   blit is the one path known to produce a correct picture on real
   hardware, and it stays untouched. A shared helper would be tidier and
   would put the working path at risk of a change made while nobody was
   watching; that is a bad trade. If all three paths are still here in a
   month and all three are proven, factor it then.
   ================================================================== */

/* Common setup: decoder geometry, crop, clipping to dstRect, and the
   pixmap-relative origin. Returns false if there is nothing to draw.
   Everything it reports is in terms of the DECODED picture (srcW/srcH
   are macroblock-aligned) and the VISIBLE window (dstW/dstH after crop
   and clip), with left/top the crop origin inside the decoded picture. */
typedef struct
{
    u32  srcW, srcH;
    u32  dstW, dstH;
    u32  left, top;
    long originX, originY;
    long rowBytes;
    Ptr  baseAddr;
} H264BlitGeom;

static Boolean ComputeBlitGeometry(H264GlobalsPtr glob,
                                    CodecDecompressParams *params,
                                    H264BlitGeom *g)
{
    u32 cropFlag = 0, cropL = 0, cropW = 0, cropT = 0, cropH = 0;

    g->left = 0;
    g->top  = 0;

    /* Geometry from the decoder, never from our globals -- the instance
       that receives BandDecompress is not always the one PreDecompress
       ran on, so cached geometry can read back as zero exactly when it
       is needed. This cost us a blank picture once already. */
    g->srcW = (u32)h264bsdPicWidth(glob->h264)  * 16;
    g->srcH = (u32)h264bsdPicHeight(glob->h264) * 16;
    if (g->srcW == 0 || g->srcH == 0) return false;

    h264bsdCroppingParams(glob->h264, &cropFlag, &cropL, &cropW, &cropT, &cropH);
    if (cropFlag && cropW > 0 && cropH > 0)
    {
        g->dstW = cropW; g->dstH = cropH;
        g->left = cropL; g->top  = cropT;
    }
    else
    {
        g->dstW = g->srcW; g->dstH = g->srcH;
    }

    g->baseAddr = (Ptr)params->dstPixMap.baseAddr;
    g->rowBytes = (long)(params->dstPixMap.rowBytes & 0x3FFF);
    if (g->baseAddr == NULL) return false;

    {
        long rectW = (long)params->dstRect.right  - (long)params->dstRect.left;
        long rectH = (long)params->dstRect.bottom - (long)params->dstRect.top;
        if (rectW <= 0 || rectH <= 0) return false;
        if ((long)g->dstW > rectW) g->dstW = (u32)rectW;
        if ((long)g->dstH > rectH) g->dstH = (u32)rectH;
    }

    g->originX = (long)params->dstRect.left - (long)params->dstPixMap.bounds.left;
    g->originY = (long)params->dstRect.top  - (long)params->dstPixMap.bounds.top;
    if (g->originX < 0 || g->originY < 0) return false;

    /* Chroma is shared across 2x2 luma blocks, so an odd origin would
       pair each chroma sample with the wrong luma pair and swap Cb/Cr
       across the row. Round down to even: that shifts the picture by at
       most one pixel, which nobody can see, where getting it wrong
       produces obviously wrong colour. In practice QuickTime's video
       rectangles are even-aligned anyway and this never fires. */
    g->originX &= ~1L;
    g->originY &= ~1L;

    return true;
}

/* ------------------------------------------------------------------
   '2vuy' -- packed 4:2:2, byte order Cb Y0 Cr Y1 per pixel pair.
   ------------------------------------------------------------------
   Two bytes per pixel instead of four, and no arithmetic at all: every
   output byte is a copy of an input byte. Chroma is duplicated down the
   vertical (4:2:0 has half as many chroma rows as 4:2:2 needs) rather
   than interpolated. Duplication is what every fast converter of this
   era did and the difference is not visible in motion; interpolating
   would put a multiply back in the inner loop, which is the entire
   thing we are trying to avoid. */
static void CopyPlanarYUVTo2vuy(H264GlobalsPtr glob, const u8 *yuv,
                                 CodecDecompressParams *params)
{
    H264BlitGeom g;
    const u8 *yPlane, *cbPlane, *crPlane;
    u32 chromaStride, yy;
    u8 *frameBase;

    if (!ComputeBlitGeometry(glob, params, &g)) return;
    if (g.rowBytes <= 0) return;

    yPlane  = yuv;
    cbPlane = yuv + ((long)g.srcW * g.srcH);
    crPlane = cbPlane + (((long)g.srcW * g.srcH) >> 2);
    chromaStride = g.srcW >> 1;

    frameBase = (u8 *)g.baseAddr + g.originY * g.rowBytes + g.originX * 2;

    for (yy = 0; yy < g.dstH; yy++)
    {
        const u8 *yRow  = yPlane + (long)(yy + g.top) * g.srcW + g.left;
        u32       cRow  = ((yy + g.top) >> 1);
        const u8 *cbRow = cbPlane + (long)cRow * chromaStride + (g.left >> 1);
        const u8 *crRow = crPlane + (long)cRow * chromaStride + (g.left >> 1);
        u8       *dst   = frameBase + (long)yy * g.rowBytes;
        u32       xx;

        for (xx = 0; xx + 1 < g.dstW; xx += 2)
        {
            dst[0] = *cbRow++;
            dst[1] = *yRow++;
            dst[2] = *crRow++;
            dst[3] = *yRow++;
            dst += 4;
        }

        /* Odd visible width: one luma sample left over. Duplicate it
           into the pair's second slot so the macroblock edge does not
           show as a black column. */
        if (xx < g.dstW)
        {
            dst[0] = *cbRow;
            dst[1] = *yRow;
            dst[2] = *crRow;
            dst[3] = dst[1];
        }
    }
}

/* ------------------------------------------------------------------
   'y420' -- planar 4:2:0, which is exactly h264bsd's own output.
   ------------------------------------------------------------------
   The destination is not a flat pixmap: baseAddr points at a
   PlanarPixmapInfoYUV420 (declared in ImageCodec.h) whose three
   PlanarComponentInfo records give each plane's offset -- measured from
   the start of that struct, not from the pixmap -- and its own
   rowBytes. The pixmap's own rowBytes is meaningless here and is not
   used.

   With that read, the blit is three runs of BlockMoveData per row group
   and no per-pixel work whatsoever. This is the cheapest OUR output
   stage can possibly be. Whether the whole pipeline is cheaper depends
   on what QuickTime does with the planes next, which is measured on
   hardware, not argued from here. */
static void CopyPlanarYUVToY420(H264GlobalsPtr glob, const u8 *yuv,
                                 CodecDecompressParams *params)
{
    H264BlitGeom g;
    PlanarPixmapInfoYUV420 *ppi;
    const u8 *yPlane, *cbPlane, *crPlane;
    u8   *dstY, *dstCb, *dstCr;
    long  yRB, cbRB, crRB;
    u32   chromaStride, yy, halfH, halfW;

    if (!ComputeBlitGeometry(glob, params, &g)) return;

    ppi = (PlanarPixmapInfoYUV420 *)g.baseAddr;

    yRB  = (long)ppi->componentInfoY.rowBytes;
    cbRB = (long)ppi->componentInfoCb.rowBytes;
    crRB = (long)ppi->componentInfoCr.rowBytes;

    /* A plane stride narrower than the picture means we have
       misunderstood the buffer; draw nothing rather than run off the
       end of somebody else's memory. */
    if (yRB  < (long)g.dstW      || cbRB < (long)(g.dstW >> 1)
     || crRB < (long)(g.dstW >> 1)) return;

    dstY  = (u8 *)ppi + ppi->componentInfoY.offset;
    dstCb = (u8 *)ppi + ppi->componentInfoCb.offset;
    dstCr = (u8 *)ppi + ppi->componentInfoCr.offset;

    dstY  += g.originY * yRB + g.originX;
    dstCb += (g.originY >> 1) * cbRB + (g.originX >> 1);
    dstCr += (g.originY >> 1) * crRB + (g.originX >> 1);

    yPlane  = yuv;
    cbPlane = yuv + ((long)g.srcW * g.srcH);
    crPlane = cbPlane + (((long)g.srcW * g.srcH) >> 2);
    chromaStride = g.srcW >> 1;

    for (yy = 0; yy < g.dstH; yy++)
    {
        BlockMoveData(yPlane + (long)(yy + g.top) * g.srcW + g.left,
                      dstY + (long)yy * yRB,
                      (Size)g.dstW);
    }

    halfH = g.dstH >> 1;
    halfW = g.dstW >> 1;

    for (yy = 0; yy < halfH; yy++)
    {
        long srcRow = (long)(yy + (g.top >> 1)) * chromaStride + (g.left >> 1);
        BlockMoveData(cbPlane + srcRow, dstCb + (long)yy * cbRB, (Size)halfW);
        BlockMoveData(crPlane + srcRow, dstCr + (long)yy * crRB, (Size)halfW);
    }
}

#endif /* kOfferYUVOutput */

/* ------------------------------------------------------------------
   Dispatch on what QuickTime ACTUALLY handed us.
   ------------------------------------------------------------------
   Not on what we asked for in PreDecompress. Asking is not getting --
   that lesson is already written into the log lines in BandDecompress
   -- and this is what makes turning kOfferYUVOutput on safe: if the ICM
   ignores the wanted list entirely we simply take the RGB path, exactly
   as before, and the picture is still right.

   An unrecognised format draws nothing rather than guessing. A blank
   window is a diagnosable symptom; a wrong-format blit into a screen
   backing store is convincing-looking garbage at best and somebody
   else's memory at worst. */
static void BlitPicture(H264GlobalsPtr glob, const u8 *yuv,
                        CodecDecompressParams *params)
{
    if (glob == NULL || glob->h264 == NULL || yuv == NULL || params == NULL)
        return;

    /* One-shot report of what QuickTime ACTUALLY handed us. This is the
       whole point of the YUV experiment: the list in PreDecompress is a
       request, and this is the answer. Printed as signed decimal because
       LogNum has no hex; decode with
           'y420' = 2033463856   '2vuy' = 846624121   32 = 32-bit RGB
       For y420 it also prints the three plane offsets and strides, so the
       PlanarPixmapInfoYUV420 interpretation can be checked against a real
       buffer rather than trusted. Silent at kH264Diagnostics 0. */
    {
        /* Reported whenever the destination CHANGES (poster frame to the
           screen, then playback offscreen, for instance), capped so that
           a destination that flips every frame cannot flood the log. */
        static OSType sLastFmt  = 0xFFFFFFFFUL;
        static short  sLastSize = -1;
        static short  sReports  = 0;
        OSType f = H264_PIXMAP_FORMAT(params->dstPixMap);
        if ((f != sLastFmt || params->dstPixMap.pixelSize != sLastSize) &&
            sReports < 6)
        {
            sLastFmt  = f;
            sLastSize = params->dstPixMap.pixelSize;
            sReports++;
            LogNum("BlitPicture: dst pixelFormat", (long)f);
            LogNum("BlitPicture: dst pixelSize", (long)params->dstPixMap.pixelSize);
            LogNum("BlitPicture: dst rowBytes (masked)",
                   (long)(params->dstPixMap.rowBytes & 0x3FFF));
#if kOfferYUVOutput
            if (f == kYUV420CodecType && params->dstPixMap.baseAddr != NULL)
            {
                PlanarPixmapInfoYUV420 *ppi =
                    (PlanarPixmapInfoYUV420 *)params->dstPixMap.baseAddr;
                LogNum("  y420 Y  offset",   (long)ppi->componentInfoY.offset);
                LogNum("  y420 Y  rowBytes", (long)ppi->componentInfoY.rowBytes);
                LogNum("  y420 Cb offset",   (long)ppi->componentInfoCb.offset);
                LogNum("  y420 Cb rowBytes", (long)ppi->componentInfoCb.rowBytes);
                LogNum("  y420 Cr offset",   (long)ppi->componentInfoCr.offset);
                LogNum("  y420 Cr rowBytes", (long)ppi->componentInfoCr.rowBytes);
            }
#endif
        }
    }

#if kOfferYUVOutput
    {
        OSType fmt = H264_PIXMAP_FORMAT(params->dstPixMap);

        if (fmt == kYUV420CodecType)
        {
            CopyPlanarYUVToY420(glob, yuv, params);
            if (kH264Diagnostics) gStatBlitY420++;
            return;
        }
        if (fmt == k422YpCbCr8CodecType)
        {
            CopyPlanarYUVTo2vuy(glob, yuv, params);
            if (kH264Diagnostics) gStatBlit2vuy++;
            return;
        }
    }
#endif

    if (params->dstPixMap.pixelSize == 32)
    {
        CopyPlanarYUVToPixMap(glob, yuv, params);
        if (kH264Diagnostics) gStatBlitRGB32++;
        return;
    }

    if (kH264Diagnostics) gStatBlitOther++;
    {
        static Boolean sMoanedOnce = false;
        if (!sMoanedOnce)
        {
            sMoanedOnce = true;
            LogNum("  UNHANDLED destination pixelSize",
                   (long)params->dstPixMap.pixelSize);
            LogNum("  UNHANDLED destination pixelFormat",
                   (long)H264_PIXMAP_FORMAT(params->dstPixMap));
        }
    }
}
