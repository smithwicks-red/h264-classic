/* AVCDecAPI.h -- the H264 Decoder component's view of AVCDecLib (MIT, ours).
   AVCDecLib is a separate CFM shared library ('shlb', LGPL, FFmpeg 0.8.15).
   This header only describes its 14 C entry points (8 since 0x00080F01, 6 since 0x00080F02) so the component can
   load them at run time with GetSharedLibrary + FindSymbol; no FFmpeg code
   or header is used here. Layouts must match AVCDecLib's own avcdec.h. */
#ifndef AVCDECAPI_H
#define AVCDECAPI_H
#include <stdarg.h>

typedef struct AVCDecCallbacks {
    void *(*alloc)(void *opaque, unsigned long size);   /* must return 16-byte aligned */
    void  (*release)(void *opaque, void *ptr);
    void  (*log)(void *opaque, int level, const char *fmt, va_list vl);
    void  *opaque;
} AVCDecCallbacks;

#define AVCDEC_FLAG_ALTIVEC 1UL

typedef struct AVCDecPicture {
    const unsigned char *plane[3];
    long  stride[3];
    long  width, height;      /* cropped */
    long  key;
    long  count;
    long  poc;                /* since 0x00080F02: picture order count. Appended at the end; the
                                 library WRITES it, so this struct (44 bytes on PPC; was 40) must be what is passed
                                 to any 0x00080F02+ library, even if the field is not read. */
} AVCDecPicture;

typedef struct AVCDecContext AVCDecContext;

typedef unsigned long  (*AVCDecVersionFn)(void);
typedef long           (*AVCDecInitFn)(const AVCDecCallbacks *, unsigned long, unsigned long *);
typedef AVCDecContext *(*AVCDecOpenFn)(const unsigned char *, long);
typedef long           (*AVCDecFeedFn)(AVCDecContext *, const unsigned char *, long, long *, AVCDecPicture *);
typedef long           (*AVCDecDecodeFn)(AVCDecContext *, const unsigned char *, long, AVCDecPicture *);
typedef long           (*AVCDecFlushFn)(AVCDecContext *, AVCDecPicture *);
typedef void           (*AVCDecResetFn)(AVCDecContext *);
typedef void           (*AVCDecCloseFn)(AVCDecContext *);
/* since 0x00080F02 (kAVCDecVersionSkipDelay) */
typedef long           (*AVCDecSetSkipFn)(AVCDecContext *, long skip_loop_filter, long skip_frame);
typedef long           (*AVCDecGetDelayFn)(AVCDecContext *);
typedef long           (*AVCDecSetDelayFn)(AVCDecContext *, long n);
typedef long           (*AVCDecTakeLowestFn)(AVCDecContext *, AVCDecPicture *, long *poc);
typedef long           (*AVCDecHeldFn)(AVCDecContext *);
typedef long           (*AVCDecPeekLowestFn)(AVCDecContext *, AVCDecPicture *, long *poc);

typedef struct H264AvcFns {
    AVCDecVersionFn version;
    AVCDecInitFn    init;
    AVCDecOpenFn    open;
    AVCDecFeedFn    feed_annexb;
    AVCDecDecodeFn  decode_sample;
    AVCDecFlushFn   flush;
    AVCDecResetFn   reset;
    AVCDecCloseFn   close;
    /* since 0x00080F02: look these up only if version() >= kAVCDecVersionSkipDelay */
    AVCDecSetSkipFn    set_skip;      /* "avcdec_set_skip"    */
    AVCDecGetDelayFn   get_delay;     /* "avcdec_get_delay"   */
    AVCDecSetDelayFn   set_delay;     /* "avcdec_set_delay"   */
    AVCDecTakeLowestFn take_lowest;   /* "avcdec_take_lowest" */
    AVCDecHeldFn       held;          /* "avcdec_held"        */
    AVCDecPeekLowestFn peek_lowest;   /* "avcdec_peek_lowest" */
} H264AvcFns;

/* Oldest library this component accepts: 0xMMmmppBB, FFmpeg 0.8.15 build 1. */
#define kAVCDecMinVersion 0x00080F01UL
/* First library with set_skip/get_delay/set_delay/take_lowest/held/peek_lowest and AVCDecPicture.poc. */
#define kAVCDecVersionSkipDelay 0x00080F02UL
#endif
