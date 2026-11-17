/* AVCDecLib: FFmpeg 0.8.15 H.264 decoder as a plain-C library (Mac OS 9 CFM 'shlb').
   LGPL 2.1+ (FFmpeg). All calls are plain C, PowerPC CFM/PowerOpen ABI. */
#ifndef AVCDEC_H
#define AVCDEC_H
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Host-supplied callbacks. alloc MUST return 16-byte aligned memory (or 0). */
typedef struct AVCDecCallbacks {
    void *(*alloc)(void *opaque, unsigned long size);
    void  (*release)(void *opaque, void *ptr);
    void  (*log)(void *opaque, int level, const char *fmt, va_list vl); /* optional (0) */
    void  *opaque;
} AVCDecCallbacks;

#define AVCDEC_FLAG_ALTIVEC 1UL   /* caller checked Gestalt: AltiVec present */

/* One output picture, display order, already cropped by FFmpeg.
   Pointers stay valid until the next call on the same context. */
typedef struct AVCDecPicture {
    const unsigned char *plane[3]; /* Y, Cb, Cr (4:2:0, 8-bit) */
    long  stride[3];
    long  width, height;           /* cropped luma size; chroma is width/2 x height/2 */
    long  key;                     /* 1 if key frame */
    long  count;                   /* pictures output so far on this context (1-based) */
    long  poc;                     /* Since 0x00080F02: picture order count of this picture (frame POC).
                                      Appended at the END, so every older field keeps its offset and old
                                      callers' reads are unchanged. NOTE: the library writes this field on
                                      every returned picture, so a caller compiled with the old 9-long
                                      struct must give room for it (allocate sizeof of this struct) before
                                      running against 0x00080F02 or later. */
} AVCDecPicture;

typedef struct AVCDecContext AVCDecContext;

unsigned long  avcdec_version(void);             /* 0xMMmmppBB: FFmpeg 0.8.15, API build BB */
/* Set callbacks and CPU flags. May be called again between contexts (flags apply to
   contexts opened afterwards). Returns 0, or <0 on bad args. *sp_mod16 = r1 & 15 at entry. */
long avcdec_init(const AVCDecCallbacks *cb, unsigned long flags, unsigned long *sp_mod16);
/* avcC==0: Annex B stream (use avcdec_feed_annexb or avcdec_decode_sample with start codes).
   avcC!=0: 'avcC' record (ISO 14496-15); then use avcdec_decode_sample with length-prefixed samples. */
AVCDecContext *avcdec_open(const unsigned char *avcC, long avcC_len);
/* Annex B byte stream through FFmpeg's h264 parser. Consumes some of data (*consumed), decodes at
   most one access unit. Returns 1 if *pic was filled, 0 if not, <0 on error (decode errors are
   concealed, not returned). len==0 signals end of stream (parser flush). */
long avcdec_feed_annexb(AVCDecContext *c, const unsigned char *data, long len, long *consumed, AVCDecPicture *pic);
/* One complete access unit (sample). Returns 1 if *pic was filled, 0 if not, <0 on error. */
long avcdec_decode_sample(AVCDecContext *c, const unsigned char *data, long len, AVCDecPicture *pic);
/* Drain delayed pictures at end of stream: call until it returns 0. */
long avcdec_flush(AVCDecContext *c, AVCDecPicture *pic);
/* Discard all decoder/parser state (seek). Next input should start at an IDR/recovery point. */
void avcdec_reset(AVCDecContext *c);
void avcdec_close(AVCDecContext *c);
/* Since build 0x00080F02. Decode-speed controls; take effect from the next decoded picture.
   skip_loop_filter: 0 = none skipped (default), 1 = deblocking skipped on non-reference pictures, 2 = on all.
   skip_frame:       0 = none skipped (default), 1 = non-reference pictures dropped (not decoded, not output),
                     2 = additionally only I slices decoded: FFmpeg 0.8 still OUTPUTS the other reference pictures,
                         undecoded (stale content). Use 2 only together with your own non-key frame dropping.
   Returns 0, or <0 on bad args. */
long avcdec_set_skip(AVCDecContext *c, long skip_loop_filter, long skip_frame);
/* Since build 0x00080F02. Current reorder delay in pictures (FFmpeg has_b_frames), valid once the
   parameter sets are parsed (after avcdec_open with avcC, or after the first decode); may grow
   during decoding. Returns -1 if c is 0. */
long avcdec_get_delay(AVCDecContext *c);
/* Since build 0x00080F02. Force the reorder delay to n pictures (0..16). FFmpeg may raise it again
   (SPS num_reorder_frames, out-of-order detection) but never lowers it. Returns 0, <0 on bad args. */
long avcdec_set_delay(AVCDecContext *c, long n);
/* Since build 0x00080F02. Remove the lowest-POC picture FFmpeg is holding for reordering (same rule as
   FFmpeg's own output, stops at a key frame) and return it: 1 = *pic filled, 0 = none held, <0 bad args.
   *poc (may be 0) receives its POC. Pointers valid until the next call on c. Use instead of
   avcdec_flush to drain, or to output early. */
long avcdec_take_lowest(AVCDecContext *c, AVCDecPicture *pic, long *poc);
/* Since build 0x00080F02. Same choice as avcdec_take_lowest but the picture STAYS queued and FFmpeg's
   reordering state is not touched (e.g. paint the first picture at once after a seek). pic->count is
   not advanced. Returns 1 = *pic filled, 0 = none held, <0 bad args. Pointers valid until next call. */
long avcdec_peek_lowest(AVCDecContext *c, AVCDecPicture *pic, long *poc);
/* Since build 0x00080F02. Pictures currently held in the reorder queue; -1 if c is 0. */
long avcdec_held(AVCDecContext *c);

#ifdef __cplusplus
}
#endif
#endif
