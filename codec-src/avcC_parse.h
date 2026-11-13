/*
    avcC_parse.h

    Helpers for dealing with the 'avcC' AVCDecoderConfigurationRecord that
    QuickTime movies (and MP4/M4V files with an 'avc1' sample description)
    carry in the ImageDescription's extension list, and for converting the
    length-prefixed NAL unit framing used inside 'avc1' sample data into the
    Annex-B start-code framing that h264bsd's NAL scanner expects.

    Why this file exists:
    ---------------------
    H.264 elementary streams "in the wild" (e.g. raw .264/.h264 files) use
    Annex-B framing: each NAL unit is preceded by a start code of
    0x000001 or 0x00000001, and the decoder finds NAL boundaries by
    scanning for those start codes. That's what h264bsd (see
    h264bsd_byte_stream.c) expects on its input.

    QuickTime/MP4 "avc1" sample data does NOT use start codes. Instead each
    NAL unit is prefixed with a fixed-width big-endian length field (1, 2,
    or 4 bytes, chosen per-file and recorded in the avcC box). The SPS and
    PPS NAL units are not in the sample data at all -- they live once, up
    front, inside the avcC box itself (which is an "image description
    extension" on the sample description, reachable via
    GetImageDescriptionExtension(desc, 'avcC', 1, &avcCHandle) in classic
    QuickTime's Image Compression Manager).

    So our BandDecompress routine has to do two conversion jobs before it
    can hand anything to h264bsdDecode():
      1. Once, at PreDecompress/Preflight time: pull the SPS/PPS NAL units
         out of the avcC box and feed THOSE to h264bsdDecode() first (with
         start codes prepended), so the decoder has parameter sets before
         it ever sees a frame.
      2. Per sample: walk the length-prefixed NALs in the sample data and
         rewrite each length prefix as a 4-byte 0x00000001 start code
         (in place is fine since a start code is never longer than a
         length prefix wider than 3 bytes; for 1- or 2-byte length fields
         we must copy into a scratch buffer instead -- see the .c file).

    avcC box layout (ISO/IEC 14496-15):
        u8  configurationVersion        (always 1)
        u8  AVCProfileIndication
        u8  profile_compatibility
        u8  AVCLevelIndication
        u8  lengthSizeMinusOne : 2 bits in the low bits, top 6 bits = 0x3f
        u8  numOfSequenceParameterSets : 5 bits in the low bits, top 3 = 0x7
        for each SPS:
            u16 sps_length
            u8  sps_data[sps_length]
        u8  numOfPictureParameterSets
        for each PPS:
            u16 pps_length
            u8  pps_data[pps_length]
        -- (high-profile fields may follow; we don't need them for Baseline)
*/

#ifndef AVCC_PARSE_H_INCLUDED
#define AVCC_PARSE_H_INCLUDED

#include "basetype.h"

/* Pull out everything we need from an avcC box in one pass.
   pAvcC / avcCSize:      the raw avcC box contents (NOT including the
                          8-byte atom header -- just the payload that
                          GetImageDescriptionExtension hands back).
   pLengthSize:           receives the NAL length field width (1, 2, or 4).
   spsList / spsSizes:    caller-supplied arrays of up to maxSps entries;
                          filled with pointers *into pAvcC* (no copy) and
                          their lengths.
   ppsList / ppsSizes:    same, for PPS.
   Returns 1 on success, 0 if the box is malformed/too short. */
int AVCC_Parse(const u8 *pAvcC, u32 avcCSize,
               u32 *pLengthSize,
               const u8 **spsList, u32 *spsSizes, int maxSps, int *outNumSps,
               const u8 **ppsList, u32 *ppsSizes, int maxPps, int *outNumPps);

/* Worst-case output size for converting one sample's length-prefixed NALs
   to Annex-B: every length prefix (>=1 byte) is replaced by a 4-byte start
   code, so the buffer can only grow by at most 3 bytes per NAL unit. Give
   this function the NAL count upper bound (sampleSize / 1, a safe
   overestimate since each NAL is at least 1 byte of payload plus its
   length prefix) if you want an exact allocation; otherwise just add a
   handful of bytes of slop per expected NAL and be generous. */
u32 AVCC_WorstCaseAnnexBSize(u32 sampleSize);

/* Convert one sample's length-prefixed NAL sequence into Annex-B framing.
   pIn/inSize:     the raw sample data (as delivered in
                   ImageSubCodecDecompressRecord.codecData).
   lengthSize:     1, 2, or 4 -- from AVCC_Parse's pLengthSize.
   pOut/outBufSize:destination buffer (must NOT alias pIn -- see .c file
                   for why we can't reliably convert in place when
                   lengthSize < 4).
   Returns the number of bytes written to pOut, or 0 on a framing error
   (truncated length prefix, length running past the end of the sample --
   which we treat as a corrupt/partial sample rather than crashing). */
u32 AVCC_ConvertSampleToAnnexB(const u8 *pIn, u32 inSize, u32 lengthSize,
                                u8 *pOut, u32 outBufSize);

/* Writes a single NAL unit (already framed as [start code][NAL bytes])
   into pOut, for injecting the avcC's SPS/PPS ahead of the first sample.
   Returns bytes written. */
u32 AVCC_WriteAnnexBUnit(const u8 *pNal, u32 nalSize, u8 *pOut, u32 outBufSize);

#endif /* AVCC_PARSE_H_INCLUDED */
