/*
    H264Codec.h -- component identity constants shared between the C
    source and the Rez resource description (H264Codec.r).

    NOTE: Rez and C can both read this file as long as we stick to
    constructs both understand (plain #define with numeric/OSType
    literals) -- no C-only syntax here.
*/

#ifndef H264CODEC_H_INCLUDED
#define H264CODEC_H_INCLUDED

#define kH264CodecVersion       0x00010000  /* 1.0.0, bump as we go */

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
