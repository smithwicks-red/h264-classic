/*
    AVCProfile.c -- see AVCProfile.h. Moved unchanged from mc_core.c
    (the small helpers it needs, and section e). Strict C89.
*/
#include <string.h>
#include <stdio.h>
#include "AVCProfile.h"

/* ==================================================================
   small helpers
   ================================================================== */

static void mc_copy(char *dst, const char *src, long max)
{
    long n = (long)strlen(src);
    if (n > max - 1) n = max - 1;
    memcpy(dst, src, (size_t)n);
    dst[n] = 0;
}

static unsigned long mc_rd32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
         | ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
}

static unsigned int mc_rd16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

/* ==================================================================
   e. what is in the moov
   ================================================================== */

/* Box at off within [off,end): header length and total size.
   0 ok, -1 malformed. 64-bit sizes accepted only if the high half
   is 0 (as in H264Stream.c). */
static int mc_box(const unsigned char *b, long off, long end,
                  long *hdr, long *size)
{
    unsigned long s;
    if (off < 0 || end - off < 8) return -1;
    s = mc_rd32(b + off);
    *hdr = 8;
    if (s == 1)
    {
        if (end - off < 16) return -1;
        if (mc_rd32(b + off + 8) != 0) return -1;
        s = mc_rd32(b + off + 12);
        *hdr = 16;
    }
    else if (s == 0)
    {
        s = (unsigned long)(end - off);
    }
    if (s < (unsigned long)*hdr || s > (unsigned long)(end - off)) return -1;
    *size = (long)s;
    return 0;
}

static int mc_is(const unsigned char *t, const char *four)
{
    return t[0] == (unsigned char)four[0] && t[1] == (unsigned char)four[1]
        && t[2] == (unsigned char)four[2] && t[3] == (unsigned char)four[3];
}

static int mc_visual(const unsigned char *t)
{
    static const char *const kVis[] =
    {
        "avc1", "avc3", "mp4v", "hvc1", "hev1", "vp08", "vp09", "av01",
        "s263", "h263", "jpeg", "mjpa", "SVQ1", "SVQ3", "cvid", "encv",
        NULL
    };
    int i;
    for (i = 0; kVis[i] != NULL; i++)
        if (mc_is(t, kVis[i])) return 1;
    return 0;
}

/* 1 found, 0 not here, -1 malformed */
static int mc_stsd(const unsigned char *b, long off, long end,
                   McAvcInfo *info)
{
    unsigned long count, i;
    long e, hdr, size;

    if (end - off < 8) return -1;
    count = mc_rd32(b + off + 4);
    e = off + 8;
    for (i = 0; i < count && e < end; i++)
    {
        if (mc_box(b, e, end, &hdr, &size) != 0) return -1;
        if (mc_visual(b + e + 4))
        {
            long v = e + hdr, c, ch, cs;
            memcpy(info->codec, b + e + 4, 4);
            info->codec[4] = 0;
            if (size - hdr < 78) return -1;
            info->width  = (int)mc_rd16(b + v + 24);
            info->height = (int)mc_rd16(b + v + 26);
            for (c = v + 78; c < e + size; c += cs)
            {
                if (mc_box(b, c, e + size, &ch, &cs) != 0) return -1;
                if (mc_is(b + c + 4, "avcC") && cs - ch >= 4)
                {
                    info->profile = b[c + ch + 1];
                    info->compat  = b[c + ch + 2];
                    info->level   = b[c + ch + 3];
                    break;
                }
            }
            return 1;
        }
        e += size;
    }
    return 0;
}

static int mc_walk(const unsigned char *b, long off, long end,
                   McAvcInfo *info, int depth)
{
    long hdr, size;
    int r;

    if (depth > 8) return -1;
    while (off < end)
    {
        if (mc_box(b, off, end, &hdr, &size) != 0) return -1;
        if (mc_is(b + off + 4, "trak") || mc_is(b + off + 4, "mdia")
            || mc_is(b + off + 4, "minf") || mc_is(b + off + 4, "stbl"))
        {
            r = mc_walk(b, off + hdr, off + size, info, depth + 1);
            if (r != 0) return r;
        }
        else if (mc_is(b + off + 4, "stsd"))
        {
            r = mc_stsd(b, off + hdr, off + size, info);
            if (r != 0) return r;
        }
        off += size;
    }
    return 0;
}

int mc_avc_profile(const unsigned char *moov, long len, McAvcInfo *info)
{
    long hdr, size;
    int r;

    memset(info, 0, sizeof(*info));
    info->profile = -1;
    if (moov == NULL || mc_box(moov, 0, len, &hdr, &size) != 0) return -2;
    if (!mc_is(moov + 4, "moov")) return -2;
    r = mc_walk(moov, hdr, size, info, 0);
    if (r < 0) return -2;
    return r == 1 ? 0 : -1;
}

static const char *mc_profile_name(int p)
{
    switch (p)
    {
    case 66:  return "Baseline";
    case 77:  return "Main";
    case 88:  return "Extended";
    case 100: return "High";
    case 110: return "High 10";
    case 122: return "High 4:2:2";
    case 244: return "High 4:4:4";
    }
    return "an unknown";
}

int mc_avc_verdict(const McAvcInfo *info, int found, char *msg, long msgMax)
{
    char line[200];
    int  r;

    if (found == -2)
    { strcpy(line, "The movie header is damaged."); r = MC_PLAY_UNKNOWN; }
    else if (found != 0)
    { strcpy(line, "This file has no video track."); r = MC_PLAY_NO_VIDEO; }
    else if (mc_is((const unsigned char *)info->codec, "avc1")
             || mc_is((const unsigned char *)info->codec, "avc3"))
    {
        if (info->profile < 0)
        {
            strcpy(line, "This H.264 video has no avcC settings box.");
            r = MC_PLAY_UNKNOWN;
        }
        else if (info->profile != 66)
        {
            /* h264bsd parses the SPS as Baseline syntax only (no
               chroma_format_idc etc.) and has no CABAC, B-slices or
               interlace: profile 66 is the only safe answer. */
            sprintf(line, "This video is %s profile (%d); this decoder "
                    "plays Baseline only.", mc_profile_name(info->profile),
                    info->profile);
            r = MC_PLAY_NOT_BASE;
        }
        else if (info->width > MC_MAX_WIDTH || info->height > MC_MAX_HEIGHT)
        {
            sprintf(line, "This video is %dx%d; the limit is %dx%d.",
                    info->width, info->height, MC_MAX_WIDTH, MC_MAX_HEIGHT);
            r = MC_PLAY_TOO_BIG;
        }
        else
        {
            sprintf(line, "Baseline H.264, level %d.%d, %dx%d.",
                    info->level / 10, info->level % 10,
                    info->width, info->height);
            r = MC_PLAY_OK;
        }
    }
    else if (mc_is((const unsigned char *)info->codec, "mp4v"))
    {
        sprintf(line, "MPEG-4 video, %dx%d (QuickTime plays this itself).",
                info->width, info->height);
        r = MC_PLAY_NATIVE;
    }
    else
    {
        sprintf(line, "This video is '%.4s', which QuickTime 6 cannot play.",
                info->codec);
        r = MC_PLAY_UNKNOWN;
    }
    if (msg != NULL && msgMax > 0) mc_copy(msg, line, msgMax);
    return r;
}
