/*
    H264Codec.h -- component identity constants shared between the C
    source and the Rez resource description (H264Codec.r).

    NOTE: Rez and C can both read this file as long as we stick to
    constructs both understand (plain #define with numeric/OSType
    literals) -- no C-only syntax here.
*/

#ifndef H264CODEC_H_INCLUDED
#define H264CODEC_H_INCLUDED

/* v17c: the AVCDecLib build is H264 Decoder 17, version 0x00010011
   (65553). Registration (auto-versioning against v15/16t) and Media
   Center's decoder-level gate depend on this number: do not change it
   without both. The h264bsd build keeps 1.0.0. -D overrides either. */
#ifndef kH264CodecVersion
/* Rez reads this file too (H264Codec.r) and rejects an undefined macro inside an #if
   expression, so only #ifdef here (b22 guest build). Consequence: -DH264_BACKEND_AVCDEC=0
   must not be passed; leave it undefined for the h264bsd build. */
#ifdef H264_BACKEND_AVCDEC
#define kH264CodecVersion       0x00010011  /* 17 / 17b / 17c guest build */
#else
#define kH264CodecVersion       0x00010000  /* 1.0.0, bump as we go (16t test build: 0x00010010) */
#endif
#endif

/* Component identity -- MUST match between the .r resource's 'thng'
   template and anywhere else we refer to ourselves. Subtype 'avc1' is
   not a free choice: it has to match the sample description FourCC that
   real H.264-in-QuickTime/MP4 files use, or the Component Manager will
   never hand us a movie to decode. Manufacturer is arbitrary -- picked
   to be visibly not 'appl' and not collide with any real registered
   codec vendor code. */
#define kH264CodecType          'imdc'
#define kH264CodecSubType       'avc1'
#define kH264CodecManufacturer  'wY9H'

#endif /* H264CODEC_H_INCLUDED */
