/*
    avcC_parse.c   -- see avcC_parse.h for the why.

    Written from scratch for this project (not part of h264bsd). Pure
    ANSI C, no Mac OS calls, so it can be unit-tested outside the VM if
    that ever turns out to be useful (e.g. on the Pismo under Mac OS X's
    Terminal with a command-line CodeWarrior-less gcc, or even here on the
    cloud side) before trusting it inside the component.
*/

#include "avcC_parse.h"

static u32 ReadU16BE(const u8 *p)
{
    return ((u32)p[0] << 8) | (u32)p[1];
}

int AVCC_Parse(const u8 *pAvcC, u32 avcCSize,
               u32 *pLengthSize,
               const u8 **spsList, u32 *spsSizes, int maxSps, int *outNumSps,
               const u8 **ppsList, u32 *ppsSizes, int maxPps, int *outNumPps)
{
    u32 pos;
    int i, count;

    *outNumSps = 0;
    *outNumPps = 0;

    /* configurationVersion, profile, compat, level, lengthSizeMinusOne byte,
       numOfSPS byte = 6 bytes minimum before we can even start */
    if (avcCSize < 6) return 0;
    if (pAvcC[0] != 1) return 0; /* configurationVersion must be 1 */

    *pLengthSize = (u32)(pAvcC[4] & 0x03) + 1; /* low 2 bits + 1 */

    pos = 5;
    count = pAvcC[pos] & 0x1f; /* low 5 bits = numOfSequenceParameterSets */
    pos += 1;

    for (i = 0; i < count; i++)
    {
        u32 len;
        if (pos + 2 > avcCSize) return 0;
        len = ReadU16BE(pAvcC + pos);
        pos += 2;
        if (pos + len > avcCSize) return 0;
        if (i < maxSps)
        {
            spsList[i] = pAvcC + pos;
            spsSizes[i] = len;
            (*outNumSps)++;
        }
        pos += len;
    }

    if (pos >= avcCSize) return (*outNumSps > 0); /* no room for PPS count byte */
    count = pAvcC[pos]; /* numOfPictureParameterSets, full byte, no mask needed */
    pos += 1;

    for (i = 0; i < count; i++)
    {
        u32 len;
        if (pos + 2 > avcCSize) break;
        len = ReadU16BE(pAvcC + pos);
        pos += 2;
        if (pos + len > avcCSize) break;
        if (i < maxPps)
        {
            ppsList[i] = pAvcC + pos;
            ppsSizes[i] = len;
            (*outNumPps)++;
        }
        pos += len;
    }

    /* We deliberately ignore anything past the PPS list (chroma format /
       bit depth fields that only appear for High-profile-and-above avcC
       records) -- Baseline/Main streams, which is all h264bsd handles,
       never have them, and if a High-profile file slips through, the SPS
       profile_idc check we do before starting the session will refuse it
       with a clear error rather than misreading garbage here. */

    return (*outNumSps > 0 && *outNumPps > 0);
}

u32 AVCC_WorstCaseAnnexBSize(u32 sampleSize)
{
    /* Pessimistic: assume every single byte is its own 1-byte-payload NAL
       unit with a 1-byte length prefix (the smallest legal framing), each
       replaced by a 4-byte start code. That's obviously never going to
       happen in a real stream, so this way overestimates -- which is
       exactly what we want for a scratch buffer we allocate once and
       reuse for the largest sample we've seen. */
    return sampleSize * 4 + 64;
}

u32 AVCC_WriteAnnexBUnit(const u8 *pNal, u32 nalSize, u8 *pOut, u32 outBufSize)
{
    if (outBufSize < nalSize + 4) return 0;
    pOut[0] = 0x00;
    pOut[1] = 0x00;
    pOut[2] = 0x00;
    pOut[3] = 0x01;
    /* BlockMove/memcpy -- see note in H264Codec.c about which one we use
       inside the component proper; this standalone file sticks to ANSI
       memcpy so it stays portable/testable outside the Mac toolbox. */
    {
        u32 i;
        for (i = 0; i < nalSize; i++) pOut[4 + i] = pNal[i];
    }
    return nalSize + 4;
}

u32 AVCC_ConvertSampleToAnnexB(const u8 *pIn, u32 inSize, u32 lengthSize,
                                u8 *pOut, u32 outBufSize)
{
    u32 inPos = 0, outPos = 0;

    if (lengthSize != 1 && lengthSize != 2 && lengthSize != 4) return 0;

    while (inPos + lengthSize <= inSize)
    {
        u32 nalLen, i;

        nalLen = 0;
        for (i = 0; i < lengthSize; i++)
            nalLen = (nalLen << 8) | pIn[inPos + i];
        inPos += lengthSize;

        if (nalLen == 0) continue;               /* tolerate zero-length junk */
        if (inPos + nalLen > inSize) return 0;    /* truncated sample -- bail */

        if (outPos + nalLen + 4 > outBufSize) return 0; /* caller under-allocated */

        pOut[outPos + 0] = 0x00;
        pOut[outPos + 1] = 0x00;
        pOut[outPos + 2] = 0x00;
        pOut[outPos + 3] = 0x01;
        for (i = 0; i < nalLen; i++)
            pOut[outPos + 4 + i] = pIn[inPos + i];

        outPos += nalLen + 4;
        inPos  += nalLen;
    }

    return outPos;
}
