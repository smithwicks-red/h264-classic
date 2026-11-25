/*
    AVCProfile.h -- what is in an .mp4's 'moov', and may our decoder
                    play it? Used by H264 Stream (H264Stream.c).

    Moved here unchanged from mc_core.h (section e) when Classic Media
    Center left this repository for its own
    (smithwicks-red/classic-media-center). The mc_/Mc/MC_ names are
    kept so the code stays identical to the copy Media Center ships.

    Strict C89. No Mac headers, no 64-bit types, no malloc. Byte order
    never matters: multi-byte values are assembled by hand.
*/
#ifndef AVC_PROFILE_H
#define AVC_PROFILE_H

/* Largest picture we will hand to QuickTime. h264bsd itself has no
   hard cap (an SPS whose size does not fit its level only makes it
   size the DPB from num_ref_frames), but the codec's blitter is
   written around a 1280-pixel row (H264Codec.c), and a G3/G4 cannot
   decode more than that in real time anyway. */
#define MC_MAX_WIDTH   1280
#define MC_MAX_HEIGHT  1024

typedef struct
{
    char codec[5];        /* sample entry type: "avc1", "mp4v", ... */
    int  profile;         /* profile_idc, -1 if no avcC */
    int  compat;          /* constraint_set flags byte */
    int  level;           /* level_idc */
    int  width, height;   /* from the visual sample entry */
} McAvcInfo;

/* moov = the whole 'moov' box, header included (as FetchMoov gives).
   Finds the first video sample entry in any trak's stsd.
   Returns 0 found, -1 no video sample entry, -2 malformed. */
int mc_avc_profile(const unsigned char *moov, long len, McAvcInfo *info);

#define MC_PLAY_OK          0   /* Baseline H.264 within size limits  */
#define MC_PLAY_NATIVE      1   /* mp4v etc.: QuickTime 6 plays it    */
#define MC_PLAY_NOT_BASE   -1   /* Main/High/...: refuse              */
#define MC_PLAY_TOO_BIG    -2
#define MC_PLAY_NO_VIDEO   -3
#define MC_PLAY_UNKNOWN    -4   /* hev1, vp09, avc1 without avcC ...  */

/* Verdict plus a one-line plain-English reason in msg[msgMax]. */
int mc_avc_verdict(const McAvcInfo *info, int found, char *msg, long msgMax);

#endif /* AVC_PROFILE_H */
