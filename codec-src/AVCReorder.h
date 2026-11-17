/*
    AVCReorder.h -- "Design A" sample-table rewrite for H.264 movies with
    B-frames, so that QuickTime 6 (which feeds samples in decode order and
    ignores 'ctts') shows every picture at the right time.

    Portable C89. No stdio, no malloc, no 64-bit types, no host-endian
    assumptions: every multi-byte field is read and written big-endian by
    hand. Memory comes from the two callbacks the caller supplies (on the
    Mac: NewPtr / DisposePtr wrappers).

    Which track: the first video track whose sample entry is avc1 and whose
    reorder delay N > 0 (other video tracks, avc1 or not, are left alone).
    What it does to that track (media bytes are never touched):
      - N = reorder delay = max(decode index - display index).
        N == 0 (no ctts, or ctts that does not reorder) -> AVCR_NOCHANGE.
      - drops ctts / sdtp / cslg / stdp / padb, retimes stts from display order,
      - appends N one-byte samples, each in its own chunk, each pointing at
        the last byte of the last real video sample,
      - rebuilds stsz / stsc / stco (co64 becomes stco; > 4 GB rejected),
      - new video edit list: one empty edit (the original leading empty
        edits, plus whatever a negative start needs) then one media edit,
        which ends where the original media edit ended (trim kept);
        empty edits after the media edit are dropped; mdhd / tkhd / mvhd
        durations,
      - adds a 12-byte 'rdly' atom (payload: N, big-endian) to the avc1
        sample entry, right after 'avcC'.
    With inPlace = 1, every stco/co64 entry of EVERY track that points at or
    past the end of the old moov in the file is then shifted by the moov's
    growth (which may be negative: dropping ctts usually shrinks the moov).
*/
#ifndef AVCREORDER_H
#define AVCREORDER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*AVCR_AllocProc)(unsigned long size, void *refCon);
typedef void  (*AVCR_FreeProc)(void *block, void *refCon);

typedef struct
{
    const unsigned char *moov;          /* the whole moov box, header included */
    unsigned long        moovSize;      /* bytes in moov[]; must equal its size field */
    unsigned long        moovFileOffset;/* where this moov sits in the file that
                                           will be written (bytes before it are
                                           not moved by the rewrite)            */
    unsigned long        fileSize;      /* size of that file, moov included      */
    int                  inPlace;       /* 1: the new moov replaces the old one in
                                           the SAME file (MP4Fix): video samples
                                           may not overlap the moov, and chunk
                                           offsets past the old moov's end are
                                           shifted by the growth.
                                           0: offsets refer to a file that does not
                                           change (H264Stream / Media Center
                                           reference movies): nothing is shifted;
                                           moovFileOffset is ignored, fileSize may
                                           be 0xFFFFFFFF if unknown.              */
    AVCR_AllocProc       alloc;
    AVCR_FreeProc        dispose;
    void                *refCon;        /* passed back to alloc / dispose        */
} AVCR_Input;

typedef struct
{
    unsigned char *moov;      /* new moov (allocated with alloc; the caller
                                 frees it with dispose). NULL unless AVCR_OK. */
    unsigned long  moovSize;
    unsigned long  N;         /* reorder delay written into 'rdly' (0 if none) */
    long           growth;    /* moovSize(new) - moovSize(old); may be < 0      */
    unsigned long  samples;   /* video samples before the rewrite              */
} AVCR_Output;

#define AVCR_OK               0   /* rewritten: write out->moov in place of the old one */
#define AVCR_NOCHANGE         1   /* N == 0: leave the movie exactly as it is            */
#define AVCR_ERR_PARAM      (-1)  /* bad arguments                                       */
#define AVCR_ERR_MEM        (-2)  /* an allocation failed                                */
#define AVCR_ERR_STRUCT     (-3)  /* malformed / truncated box tree, missing box,
                                     duplicate table, 64-bit or size-0 box in moov      */
#define AVCR_ERR_NOVIDEO    (-4)  /* no 'vide' track                                     */
#define AVCR_ERR_FRAGMENTED (-5)  /* 'mvex' present (fragmented movie)                   */
#define AVCR_ERR_STZ2       (-6)  /* compact sample sizes: not supported                 */
#define AVCR_ERR_MULTIDESC  (-7)  /* video stsd has more than one sample description     */
#define AVCR_ERR_NOTAVC     (-8)  /* video entry is not 'avc1' with an 'avcC'            */
#define AVCR_ERR_TABLES     (-9)  /* tables disagree (sample counts, stsc, chunks)       */
#define AVCR_ERR_RANGE     (-10)  /* a value does not fit the limits (see .c); N > 16    */
#define AVCR_ERR_VERSION   (-11)  /* version-1 mdhd/tkhd/mvhd (64-bit times): unsupported */
#define AVCR_ERR_OFFSETS   (-12)  /* a video sample lies outside the file or inside moov */
#define AVCR_ERR_ALREADY   (-13)  /* entry already carries 'rdly'                        */
#define AVCR_ERR_EDITS     (-14)  /* video edit list has more than one media edit        */
#define AVCR_ERR_NOSTSS    (-15)  /* B-frames but no stss: tails would be key frames     */

int         AVCR_Rewrite(const AVCR_Input *in, AVCR_Output *out);
const char *AVCR_ErrorString(int code);

#ifdef __cplusplus
}
#endif
#endif
