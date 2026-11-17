/* Fuzz AVCR_Rewrite with mutated moovs. Build with ASan/UBSan.
   usage: fuzz <iterations> <seed> file...  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/AVCReorder.h"

static long gA, gF, gLive;
static void *A(unsigned long n, void *r) { (void)r; gA++; gLive++; return malloc((size_t)n); }
static void F(void *p, void *r) { (void)r; gF++; gLive--; free(p); }
static unsigned long R32(const unsigned char *p)
{ return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3]; }
static void P32(unsigned char *p, unsigned long v)
{ p[0]=(unsigned char)(v>>24); p[1]=(unsigned char)(v>>16); p[2]=(unsigned char)(v>>8); p[3]=(unsigned char)v; }
static unsigned long rs = 1;
static unsigned long rnd(void) { rs = (rs * 1103515245UL + 12345UL) & 0xFFFFFFFFUL; return (rs >> 8) ^ (rs << 13 & 0xFFFFFFFFUL); }

typedef struct { unsigned char *m; unsigned long ms, off, fs; const char *name; } Seed;

/* structural walk of a moov: every box inside containers nests exactly */
static int Walk(const unsigned char *b, unsigned long s, unsigned long e, int d)
{
    while (s < e)
    {
        unsigned long z;
        if (e - s < 8) return 0;
        z = R32(b + s);
        if (z < 8 || z > e - s) return 0;
        if (!memcmp(b+s+4,"trak",4)||!memcmp(b+s+4,"mdia",4)||!memcmp(b+s+4,"minf",4)||!memcmp(b+s+4,"stbl",4)||!memcmp(b+s+4,"edts",4)||!memcmp(b+s+4,"dinf",4))
            if (d < 20 && !Walk(b, s + 8, s + z, d + 1)) return 0;
        s += z;
    }
    return 1;
}

/* offsets of interesting boxes (type found anywhere) */
static unsigned long Find(const unsigned char *m, unsigned long ms, const char *t, int k)
{
    unsigned long i; int seen = 0;
    for (i = 4; i + 4 <= ms; i++) if (!memcmp(m + i, t, 4)) { if (seen++ == k) return i - 4; }
    return 0;
}

int main(int argc, char **argv)
{
    long iters = atol(argv[1]), it, rc[32], valid = 0, invalid = 0, i2nochange = 0;
    int ns = 0, i;
    Seed s[64];
    static const char *types[] = {"stts","ctts","stsz","stsc","stco","co64","elst","mdhd","mvhd","tkhd","stsd","avc1","avcC","hdlr","trak","mdia","minf","stbl","edts","stss","moov","mp4a"};
    rs = (unsigned long)atol(argv[2]);
    memset(rc, 0, sizeof rc);
    for (i = 3; i < argc && ns < 64; i++)
    {
        FILE *f = fopen(argv[i], "rb"); long len, pos = 0; unsigned char *b;
        if (!f) continue;
        fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
        b = malloc((size_t)len); if (fread(b, 1, (size_t)len, f) != (size_t)len) return 2; fclose(f);
        while (pos + 8 <= len) { unsigned long z = R32(b + pos); if (z < 8) break;
            if (!memcmp(b + pos + 4, "moov", 4)) { s[ns].m = malloc(z); memcpy(s[ns].m, b + pos, z); s[ns].ms = z; s[ns].off = (unsigned long)pos; s[ns].fs = (unsigned long)len; s[ns].name = argv[i]; ns++; }
            pos += (long)z; }
        free(b);
    }
    for (it = 0; it < iters; it++)
    {
        Seed *sd = &s[rnd() % (unsigned long)ns];
        unsigned long ms = sd->ms, k, nm = 1 + rnd() % 4, j, p;
        unsigned char *m = malloc(ms + 64);
        AVCR_Input in; AVCR_Output out; int r;
        memcpy(m, sd->m, ms);
        in.moovFileOffset = sd->off; in.fileSize = sd->fs; in.inPlace = 1;
        for (k = 0; k < nm; k++)
        {
            unsigned long op = rnd() % 10;
            const char *t = types[rnd() % (sizeof types / sizeof *types)];
            unsigned long b = Find(m, ms, t, (int)(rnd() % 2));
            if (!b) b = Find(m, ms, t, 0);
            switch (op)
            {
            case 0: m[rnd() % ms] = (unsigned char)rnd(); break;                 /* random byte */
            case 1: if (b) { static const unsigned long v[] = {0,1,7,8,9,0xFFFFFFFFUL,0x80000000UL,12,16};
                        P32(m + b, rnd() % 2 ? v[rnd() % 9] : R32(m + b) + (rnd() % 17) - 8); } break;  /* box size */
            case 2: if (b && b + 16 <= ms) { static const unsigned long v[] = {0,1,2,0xFFFFFFFFUL,0x7FFFFFFFUL,0x00400001UL,3};
                        p = b + 12; P32(m + p, rnd() % 2 ? v[rnd() % 7] : R32(m + p) + (rnd() % 5) - 2); } break; /* count */
            case 3: if (b && b + 24 <= ms) { p = b + 16 + 4 * (rnd() % 8); if (p + 4 <= ms) P32(m + p, rnd() % 3 ? rnd() : (rnd() % 2 ? 0xFFFFFFFFUL : 0)); } break; /* first entries */
            case 4: if (b && b + 8 < ms) { p = b + 8 + rnd() % (R32(m+b) > 8 && R32(m+b) < ms ? R32(m+b) - 8 : 8); if (p + 4 <= ms) P32(m + p, rnd()); } break; /* anywhere in box */
            case 5: ms = 8 + rnd() % ms; if (rnd() % 2) P32(m, ms); break;                         /* truncate */
            case 6: if (b) m[b + 8] = (unsigned char)(rnd() % 3); break;                             /* version */
            case 7: in.fileSize = rnd() % 2 ? sd->fs - rnd() % (sd->fs / 2 + 1) : sd->fs + rnd() % 1000; break;
            case 8: if (b) { m[b+4] ^= 0x20; } break;                                                /* retype */
            case 9: if (b && b + 16 <= ms) { p = b + 12; P32(m + p, 1); } break;                    /* 1-entry */
            }
        }
        in.moov = m; in.moovSize = ms; in.inPlace = (rnd() % 4) != 0; in.alloc = A; in.dispose = F; in.refCon = 0;
        if (in.moovFileOffset + ms > in.fileSize && (rnd() % 8)) in.fileSize = in.moovFileOffset + ms + rnd() % 2;
        r = AVCR_Rewrite(&in, &out);
        rc[r + 16]++;
        if (r == AVCR_OK)
        {
            unsigned char *c2; AVCR_Input i2 = in; AVCR_Output o2; int r2;
            if (R32(out.moov) != out.moovSize || memcmp(out.moov + 4, "moov", 4) || !Walk(out.moov, 8, out.moovSize, 0)) { invalid++; fprintf(stderr, "INVALID tree it=%ld\n", it); }
            else
            {   /* hide rdly and re-run: all tables must validate and give N=0 */
                c2 = malloc(out.moovSize); memcpy(c2, out.moov, out.moovSize);
                p = Find(c2, out.moovSize, "rdly", 0); if (p) memcpy(c2 + p + 4, "xdly", 4);
                {   /* only the rewritten track is re-validated: other traks are
                       left as they were (a second avc1 B-frame track stays N>0) */
                    unsigned long q = 8, z;
                    while (q + 8 <= out.moovSize)
                    {
                        z = R32(c2 + q);
                        if (z < 8 || z > out.moovSize - q) break;
                        if (!memcmp(c2 + q + 4, "trak", 4) && !(p > q && p < q + z)) memcpy(c2 + q + 4, "skip", 4);
                        q += z;
                    }
                }
                i2.moov = c2; i2.moovSize = out.moovSize;
                i2.fileSize = in.inPlace ? (unsigned long)((long)in.fileSize + out.growth) : in.fileSize;
                r2 = AVCR_Rewrite(&i2, &o2);
                if (r2 == AVCR_NOCHANGE && p && R32(out.moov + p + 8) == out.N) { valid++; i2nochange++; }
                else { invalid++; fprintf(stderr, "INVALID re-run it=%ld r2=%d seed=%s\n", it, r2, sd->name); }
                if (r2 == AVCR_OK) F(o2.moov, 0);
                free(c2);
            }
            F(out.moov, 0);
        }
        else if (out.moov) { fprintf(stderr, "LEAK: moov set on error\n"); }
        if (gLive != 0) { fprintf(stderr, "LIVE ALLOCS %ld at it=%ld\n", gLive, it); return 3; }
        free(m);
    }
    printf("iterations %ld seeds %d allocs %ld frees %ld live %ld\n", iters, ns, gA, gF, gLive);
    for (i = 0; i < 32; i++) if (rc[i]) printf("  rc %3d %-45s %ld\n", i - 16, AVCR_ErrorString(i - 16), rc[i]);
    printf("rewritten outputs: structurally valid %ld, invalid %ld\n", valid, invalid);
    for (i = 0; i < ns; i++) free(s[i].m);
    return invalid ? 1 : 0;
}
