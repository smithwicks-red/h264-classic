/*
    AVCReorder.c -- see AVCReorder.h. Mirrors rewrite_A() in the Python
    reference (sim/rewrite.py) table for table.

    Numeric limits (anything outside -> AVCR_ERR_RANGE, movie untouched):
      - moov smaller than 1 GB; at most 4,194,304 video samples;
      - sum of the video stts durations <= 0x6FFFFFFF;
      - every ctts offset within +/- 0x0FFFFFFF;
      - chunk offsets and sample ends below 4 GB (co64 high word 0);
      - edit-list media_time (v1 included) <= 0x7FFFFFFF; leading empty
        edits sum to <= 0xFFFFFFFF;
      - new media duration, edit durations and movie duration <= 0xFFFFFFFF.
    With these limits every signed value fits 32 bits, so the code is
    exact whether long is 32 bits (Mac) or 64 bits (host). Unsigned 32-bit
    sums are masked with 0xFFFFFFFF where they could wrap. The only wider
    product (edit duration * movie timescale / media timescale) is done with
    two 32-bit words; no 64-bit type is used anywhere (CodeWarrior 8 PPC
    miscompiles 64-bit shifts, see MP4Fix.c).
*/
#include "AVCReorder.h"

#define M32        0xFFFFFFFFUL
#define MAXSAMPLES 0x00400000UL
#define MAXMOOV    0x40000000UL
#define MAXDTS     0x6FFFFFFFUL
#define MAXCTTS    0x0FFFFFFFL
#define MAXDEPTH   16

/* ---------------- big-endian access ---------------- */

static unsigned long RD32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
         | ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
}

static int ISTYPE(const unsigned char *p, const char *t)
{
    return p[0] == (unsigned char)t[0] && p[1] == (unsigned char)t[1]
        && p[2] == (unsigned char)t[2] && p[3] == (unsigned char)t[3];
}

/* ---------------- output writer (b == 0: count only) ---------------- */

typedef struct { unsigned char *b; unsigned long pos; } W;

static void W8(W *w, unsigned long v)
{
    if (w->b) w->b[w->pos] = (unsigned char)(v & 0xFFUL);
    w->pos++;
}
static void W32(W *w, unsigned long v)
{
    W8(w, v >> 24); W8(w, v >> 16); W8(w, v >> 8); W8(w, v);
}
static void WTYPE(W *w, const char *t)
{
    W8(w, (unsigned long)(unsigned char)t[0]); W8(w, (unsigned long)(unsigned char)t[1]);
    W8(w, (unsigned long)(unsigned char)t[2]); W8(w, (unsigned long)(unsigned char)t[3]);
}
static void WBYTES(W *w, const unsigned char *p, unsigned long n)
{
    unsigned long i;
    if (w->b) for (i = 0; i < n; i++) w->b[w->pos + i] = p[i];
    w->pos += n;
}
static void PATCH32(W *w, unsigned long at, unsigned long v)
{
    if (w->b)
    {
        w->b[at]     = (unsigned char)((v >> 24) & 0xFFUL);
        w->b[at + 1] = (unsigned char)((v >> 16) & 0xFFUL);
        w->b[at + 2] = (unsigned char)((v >> 8) & 0xFFUL);
        w->b[at + 3] = (unsigned char)(v & 0xFFUL);
    }
}

/* ---------------- context ---------------- */

typedef struct
{
    const AVCR_Input *in;
    const unsigned char *m;     /* input moov */
    unsigned long msize;

    /* box offsets in the input moov (0 = absent) */
    unsigned long mvhd, vtrak, tkhd, edts, elst, mdia, mdhd, minf, stbl;
    unsigned long stsd, stts, ctts, stsz, stsc, stco, co64, stss;
    unsigned long avcCEnd;      /* offset just past avcC, inside stsd */

    unsigned long n;            /* samples */
    unsigned long *durs, *sizes, *offs, *disp;
    long          *pts;
    unsigned long mts, ts;
    long          elstMt;
    unsigned long lead;         /* leading empty edits, movie timescale */
    unsigned long editDur;      /* duration of the media edit (0 = untrimmed) */

    unsigned long N, dtsN, last;
    long          start;
    unsigned long mediaDur;
    unsigned long seg[2][2];    /* (duration, media_time as 32-bit pattern) */
    int           nseg;
    unsigned long movieDur;
    unsigned long nSttsEnt, nChunks, nStscEnt;
} Ctx;

static void *Alloc(Ctx *c, unsigned long n)
{
    if (n == 0) n = 4;
    return c->in->alloc(n, c->in->refCon);
}
static void Free(Ctx *c, void *p)
{
    if (p) c->in->dispose(p, c->in->refCon);
}

/* Read a box header at off within [.., end). Returns 0 and the size, or -1.
   64-bit (size 1) and to-end (size 0) boxes are refused inside moov. */
static int BoxAt(const Ctx *c, unsigned long off, unsigned long end, unsigned long *size)
{
    unsigned long s;
    if (off > end || end - off < 8) return -1;
    s = RD32(c->m + off);
    if (s < 8 || s > end - off) return -1;
    *size = s;
    return 0;
}

/* Find the only child of a given type in the container at box offset 'box'
   (children start 8 bytes in). 0 = none; returns 1 if duplicated via *dup. */
static unsigned long Child(const Ctx *c, unsigned long box, const char *t, int *dup)
{
    unsigned long end = box + RD32(c->m + box), off = box + 8, s, found = 0;
    while (off + 8 <= end)
    {
        if (BoxAt(c, off, end, &s) != 0) { *dup = 1; return 0; }
        if (ISTYPE(c->m + off + 4, t))
        {
            if (found) *dup = 1;
            else found = off;
        }
        off += s;
    }
    return found;
}

/* The containers Python's mp4.py descends into. */
static int IsCont(const unsigned char *t)
{
    return ISTYPE(t, "moov") || ISTYPE(t, "trak") || ISTYPE(t, "mdia")
        || ISTYPE(t, "minf") || ISTYPE(t, "stbl") || ISTYPE(t, "edts")
        || ISTYPE(t, "dinf");
}

/* Validate the tree under [start,end) and every stco/co64 table in it. */
static int ValidateTree(const Ctx *c, unsigned long start, unsigned long end, int depth)
{
    unsigned long off = start, s, cnt;
    if (depth > MAXDEPTH) return AVCR_ERR_STRUCT;
    while (off < end)
    {
        if (BoxAt(c, off, end, &s) != 0) return AVCR_ERR_STRUCT;
        if (ISTYPE(c->m + off + 4, "stco") || ISTYPE(c->m + off + 4, "co64"))
        {
            if (s < 16) return AVCR_ERR_STRUCT;
            cnt = RD32(c->m + off + 12);
            if (cnt > (s - 16) / (ISTYPE(c->m + off + 4, "co64") ? 8UL : 4UL)) return AVCR_ERR_STRUCT;
        }
        if (IsCont(c->m + off + 4))
        {
            int r = ValidateTree(c, off + 8, off + s, depth + 1);
            if (r != AVCR_OK) return r;
        }
        off += s;
    }
    return AVCR_OK;
}

/* ---------------- two-word arithmetic ---------------- */

/* (a*b) -> hi:lo, all 32-bit */
static void Mul32(unsigned long a, unsigned long b, unsigned long *hi, unsigned long *lo)
{
    unsigned long al = a & 0xFFFFUL, ah = (a >> 16) & 0xFFFFUL;
    unsigned long bl = b & 0xFFFFUL, bh = (b >> 16) & 0xFFFFUL;
    unsigned long ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    unsigned long mid = (ll >> 16) + (lh & 0xFFFFUL) + (hl & 0xFFFFUL);
    *lo = ((ll & 0xFFFFUL) | ((mid & 0xFFFFUL) << 16)) & M32;
    *hi = (hh + (lh >> 16) + (hl >> 16) + (mid >> 16)) & M32;
}

/* round(x * num / den), ties to even (Python round()). -1 if > 32 bits. */
static int MulDivRound(unsigned long x, unsigned long num, unsigned long den, unsigned long *res)
{
    unsigned long hi, lo, q = 0, r = 0, carry, bit;
    int i;
    if (den == 0) return -1;
    Mul32(x, num, &hi, &lo);
    if (hi >= den) return -1;                 /* quotient would not fit */
    for (i = 63; i >= 0; i--)
    {
        bit = (i >= 32) ? ((hi >> (i - 32)) & 1UL) : ((lo >> i) & 1UL);
        carry = (r >> 31) & 1UL;
        r = ((r << 1) | bit) & M32;
        q = (q << 1) & M32;
        if (carry || r >= den) { r = (r - den) & M32; q |= 1UL; }
    }
    if (r > den - r || (r == den - r && (q & 1UL)))
    {
        if (q == M32) return -1;
        q++;
    }
    *res = q;
    return 0;
}

/* ---------------- heap sort of indices by (pts, index) ---------------- */

static int Less(const long *pts, unsigned long a, unsigned long b)
{
    return pts[a] < pts[b] || (pts[a] == pts[b] && a < b);
}
static void Sift(const long *pts, unsigned long *v, unsigned long i, unsigned long n)
{
    unsigned long ch, t;
    for (;;)
    {
        ch = 2 * i + 1;
        if (ch >= n) break;
        if (ch + 1 < n && Less(pts, v[ch], v[ch + 1])) ch++;
        if (!Less(pts, v[i], v[ch])) break;
        t = v[i]; v[i] = v[ch]; v[ch] = t;
        i = ch;
    }
}
static void SortIdx(const long *pts, unsigned long *v, unsigned long n)
{
    unsigned long i, t;
    if (n < 2) return;
    for (i = n / 2; i > 0; i--) Sift(pts, v, i - 1, n);
    for (i = n - 1; i > 0; i--)
    {
        t = v[0]; v[0] = v[i]; v[i] = t;
        Sift(pts, v, 0, i);
    }
}

/* ---------------- parsing ---------------- */

#define SKIP 100   /* private: this trak is not an avc1 video track */

/* moov level: structure, mvhd, mvex */
static int FindBoxes(Ctx *c)
{
    unsigned long off, s, end = c->msize;

    if (c->msize < 8 || RD32(c->m) != c->msize || !ISTYPE(c->m + 4, "moov")) return AVCR_ERR_STRUCT;
    if (ValidateTree(c, 8, end, 0) != AVCR_OK) return AVCR_ERR_STRUCT;

    off = 8;
    while (off < end)
    {
        (void)BoxAt(c, off, end, &s);
        if (ISTYPE(c->m + off + 4, "mvex")) return AVCR_ERR_FRAGMENTED;
        if (ISTYPE(c->m + off + 4, "mvhd") && !c->mvhd) c->mvhd = off;
        off += s;
    }
    if (!c->mvhd) return AVCR_ERR_STRUCT;
    return AVCR_OK;
}

/* Is this trak a video track whose (first) sample entry is avc1? */
static int IsAvcVideo(const Ctx *c, unsigned long trak, int *isVideo)
{
    int dup = 0;
    unsigned long t, h, mi, st, sd;
    *isVideo = 0;
    t = Child(c, trak, "mdia", &dup);
    h = t ? Child(c, t, "hdlr", &dup) : 0;
    if (!h || RD32(c->m + h) < 20 || !ISTYPE(c->m + h + 16, "vide")) return 0;
    *isVideo = 1;
    mi = Child(c, t, "minf", &dup);
    st = mi ? Child(c, mi, "stbl", &dup) : 0;
    sd = st ? Child(c, st, "stsd", &dup) : 0;
    if (!sd || RD32(c->m + sd) < 16 + 8) return 0;
    return ISTYPE(c->m + sd + 20, "avc1");
}

/* track level: the boxes of the chosen video trak */
static int TrackBoxes(Ctx *c, unsigned long trak)
{
    int dup = 0;
    c->vtrak = trak;
    c->tkhd = Child(c, c->vtrak, "tkhd", &dup);
    c->edts = Child(c, c->vtrak, "edts", &dup);
    c->mdia = Child(c, c->vtrak, "mdia", &dup);
    if (c->edts) c->elst = Child(c, c->edts, "elst", &dup);
    if (c->mdia) { c->mdhd = Child(c, c->mdia, "mdhd", &dup); c->minf = Child(c, c->mdia, "minf", &dup); }
    if (c->minf) c->stbl = Child(c, c->minf, "stbl", &dup);
    if (dup || !c->tkhd || !c->mdhd || !c->stbl || (c->edts && !c->elst)) return AVCR_ERR_STRUCT;

    if (Child(c, c->stbl, "stz2", &dup)) return AVCR_ERR_STZ2;
    c->stsd = Child(c, c->stbl, "stsd", &dup);
    c->stts = Child(c, c->stbl, "stts", &dup);
    c->ctts = Child(c, c->stbl, "ctts", &dup);
    c->stsz = Child(c, c->stbl, "stsz", &dup);
    c->stsc = Child(c, c->stbl, "stsc", &dup);
    c->stco = Child(c, c->stbl, "stco", &dup);
    c->co64 = Child(c, c->stbl, "co64", &dup);
    c->stss = Child(c, c->stbl, "stss", &dup);
    if (dup || !c->stsd || !c->stts || !c->stsz || !c->stsc) return AVCR_ERR_STRUCT;
    if ((c->stco != 0) == (c->co64 != 0)) return AVCR_ERR_STRUCT;
    return AVCR_OK;
}

static int CheckEntry(Ctx *c)
{
    unsigned long sd = c->stsd, ssz = RD32(c->m + sd), e, esz, off, s;
    int found = 0;
    if (ssz < 16) return AVCR_ERR_STRUCT;
    if (RD32(c->m + sd + 12) == 0) return AVCR_ERR_STRUCT;
    if (RD32(c->m + sd + 12) != 1) return AVCR_ERR_MULTIDESC;
    e = sd + 16;
    if (BoxAt(c, e, sd + ssz, &esz) != 0) return AVCR_ERR_STRUCT;
    if (!ISTYPE(c->m + e + 4, "avc1")) return AVCR_ERR_NOTAVC;
    if (esz < 86) return AVCR_ERR_STRUCT;
    off = e + 86;
    while (off + 8 <= e + esz)
    {
        if (BoxAt(c, off, e + esz, &s) != 0) return AVCR_ERR_STRUCT;
        if (ISTYPE(c->m + off + 4, "rdly")) return AVCR_ERR_ALREADY;
        if (ISTYPE(c->m + off + 4, "avcC") && !found) { found = 1; c->avcCEnd = off + s; }
        off += s;
    }
    return found ? AVCR_OK : AVCR_ERR_NOTAVC;
}

/* payload helpers: p = box offset; returns entry count after checking that
   'hdr' bytes of header+fixed fields and cnt*esz bytes of entries fit */
static int Table(const Ctx *c, unsigned long p, unsigned long hdr, unsigned long esz, unsigned long *cnt)
{
    unsigned long s = RD32(c->m + p);
    if (s < hdr) return -1;
    *cnt = RD32(c->m + p + hdr - 4);
    if (*cnt > (s - hdr) / esz) return -1;
    return 0;
}

static int ReadTables(Ctx *c)
{
    unsigned long cnt, i, k, j, v, total, sz, stscN, nchunk, ci, e, spc, o, si, sampEnd;
    unsigned long moovEnd = c->in->moovFileOffset + c->msize;
    long off;
    int wide;
    const unsigned char *m = c->m;

    /* mvhd / mdhd / tkhd */
    if (RD32(m + c->mvhd) < 8 + 20) return AVCR_ERR_STRUCT;
    if (m[c->mvhd + 8] == 0) c->mts = RD32(m + c->mvhd + 20);
    else
    {
        if (RD32(m + c->mvhd) < 8 + 32) return AVCR_ERR_STRUCT;
        c->mts = RD32(m + c->mvhd + 28);
    }
    if (RD32(m + c->mdhd) < 8 + 20) return AVCR_ERR_STRUCT;
    if (m[c->mdhd + 8] != 0) return AVCR_ERR_VERSION;
    c->ts = RD32(m + c->mdhd + 20);
    if (c->ts == 0) return AVCR_ERR_RANGE;
    if (RD32(m + c->tkhd) < 8 + 24) return AVCR_ERR_STRUCT;

    /* stts -> n */
    if (Table(c, c->stts, 16, 8, &cnt)) return AVCR_ERR_STRUCT;
    total = 0;
    for (i = 0; i < cnt; i++)
    {
        k = RD32(m + c->stts + 16 + 8 * i);
        if (k > MAXSAMPLES - total) return AVCR_ERR_RANGE;
        total += k;
    }
    c->n = total;
    if (c->n < 2) return AVCR_ERR_TABLES;

    /* stsz */
    if (RD32(m + c->stsz) < 20) return AVCR_ERR_STRUCT;
    sz = RD32(m + c->stsz + 12);
    if (RD32(m + c->stsz + 16) != c->n) return AVCR_ERR_TABLES;
    if (sz == 0 && (RD32(m + c->stsz) - 20) / 4 < c->n) return AVCR_ERR_STRUCT;

    c->durs  = (unsigned long *)Alloc(c, c->n * (unsigned long)sizeof(unsigned long));
    c->sizes = (unsigned long *)Alloc(c, c->n * (unsigned long)sizeof(unsigned long));
    c->offs  = (unsigned long *)Alloc(c, c->n * (unsigned long)sizeof(unsigned long));
    c->disp  = (unsigned long *)Alloc(c, c->n * (unsigned long)sizeof(unsigned long));
    c->pts   = (long *)Alloc(c, c->n * (unsigned long)sizeof(long));
    if (!c->durs || !c->sizes || !c->offs || !c->disp || !c->pts) return AVCR_ERR_MEM;

    /* durations and dts (pts temporarily holds dts) */
    j = 0; total = 0;
    for (i = 0; i < cnt; i++)
    {
        k = RD32(m + c->stts + 16 + 8 * i);
        v = RD32(m + c->stts + 20 + 8 * i);
        while (k--)
        {
            c->durs[j] = v;
            c->pts[j] = (long)total;
            if (v > MAXDTS - total) return AVCR_ERR_RANGE;
            total += v;
            j++;
        }
    }
    for (i = 0; i < c->n; i++)
        c->sizes[i] = sz ? sz : RD32(m + c->stsz + 20 + 4 * i);

    /* ctts (absent -> no reordering, but keep validating) */
    if (c->ctts)
    {
        if (Table(c, c->ctts, 16, 8, &cnt)) return AVCR_ERR_STRUCT;
        j = 0;
        for (i = 0; i < cnt; i++)
        {
            k = RD32(m + c->ctts + 16 + 8 * i);
            v = RD32(m + c->ctts + 20 + 8 * i);
            if (m[c->ctts + 8] != 0 && v >= 0x80000000UL)
            {
                v = (~v + 1UL) & M32;                   /* magnitude of a negative */
                if (v > (unsigned long)MAXCTTS) return AVCR_ERR_RANGE;
                off = -(long)v;
            }
            else
            {
                if (v > (unsigned long)MAXCTTS) return AVCR_ERR_RANGE;
                off = (long)v;
            }
            if (k > c->n - j) return AVCR_ERR_TABLES;    /* ctts longer than stsz */
            while (k--) c->pts[j++] += off;
        }
        if (j != c->n) return AVCR_ERR_TABLES;           /* ctts shorter */
    }

    /* stsc + stco/co64 -> absolute offset of every sample */
    if (Table(c, c->stsc, 16, 12, &stscN) || stscN == 0) return AVCR_ERR_STRUCT;
    wide = c->co64 != 0;
    e = wide ? c->co64 : c->stco;
    if (Table(c, e, 16, wide ? 8UL : 4UL, &nchunk)) return AVCR_ERR_STRUCT;
    if (RD32(m + c->stsc + 16) != 1) return AVCR_ERR_TABLES;
    for (i = 0; i < stscN; i++)
    {
        if (RD32(m + c->stsc + 20 + 12 * i) == 0) return AVCR_ERR_TABLES;   /* samples per chunk */
        if (i > 0 && RD32(m + c->stsc + 16 + 12 * i) <= RD32(m + c->stsc + 4 + 12 * i)) return AVCR_ERR_TABLES;
    }
    si = 0; k = 0;
    for (ci = 0; ci < nchunk && si < c->n; ci++)
    {
        while (k + 1 < stscN && RD32(m + c->stsc + 16 + 12 * (k + 1)) - 1 <= ci) k++;
        spc = RD32(m + c->stsc + 20 + 12 * k);
        if (wide)
        {
            if (RD32(m + e + 16 + 8 * ci) != 0) return AVCR_ERR_RANGE;
            o = RD32(m + e + 20 + 8 * ci);
        }
        else o = RD32(m + e + 16 + 4 * ci);
        for (j = 0; j < spc && si < c->n; j++)
        {
            if (c->sizes[si] > M32 - o) return AVCR_ERR_OFFSETS;
            sampEnd = o + c->sizes[si];
            if (sampEnd > c->in->fileSize) return AVCR_ERR_OFFSETS;
            if (c->in->inPlace && c->sizes[si] && o < moovEnd && sampEnd > c->in->moovFileOffset) return AVCR_ERR_OFFSETS;
            c->offs[si++] = o;
            o = sampEnd;
        }
    }
    if (si != c->n) return AVCR_ERR_TABLES;            /* chunks cover too few samples */
    if (c->sizes[c->n - 1] == 0) return AVCR_ERR_OFFSETS;

    /* old edit list: the media_time of the ONE media edit (>= 0), and the
       summed duration (movie timescale) of the empty edits before it, which
       is kept. Empty edits after the media edit are dropped (as before);
       a second media edit is refused. */
    c->elstMt = 0; c->lead = 0;
    if (c->elst)
    {
        int v1 = m[c->elst + 8] != 0, media = 0;
        unsigned long es = v1 ? 20UL : 12UL, hi, lo, dhi, dlo;
        if (Table(c, c->elst, 16, es, &cnt)) return AVCR_ERR_STRUCT;
        for (i = 0; i < cnt; i++)
        {
            unsigned long p = c->elst + 16 + es * i;
            if (v1) { dhi = RD32(m + p); dlo = RD32(m + p + 4); hi = RD32(m + p + 8); lo = RD32(m + p + 12); }
            else    { dhi = 0; dlo = RD32(m + p); lo = RD32(m + p + 4); hi = (lo & 0x80000000UL) ? M32 : 0; }
            if (hi & 0x80000000UL)                         /* negative: empty edit */
            {
                if (media) continue;                       /* trailing: dropped */
                if (dhi != 0 || dlo > M32 - c->lead) return AVCR_ERR_RANGE;
                c->lead += dlo;
                continue;
            }
            if (media) return AVCR_ERR_EDITS;
            if (hi != 0 || lo > 0x7FFFFFFFUL || dhi != 0) return AVCR_ERR_RANGE;
            c->elstMt = (long)lo;
            c->editDur = dlo;                              /* 0: not trimmed */
            media = 1;
        }
    }
    return AVCR_OK;
}

/* ---------------- the Design A computation ---------------- */

static unsigned long NewDur(const Ctx *c, unsigned long i)
{
    if (i < c->N) return c->durs[i];
    i -= c->N;
    if (i + 1 < c->n)
        return ((unsigned long)c->pts[c->disp[i + 1]] - (unsigned long)c->pts[c->disp[i]]) & M32;
    return c->durs[c->n - 1];
}
static unsigned long SOff(const Ctx *c, unsigned long i)  { return i < c->n ? c->offs[i] : c->last; }
static unsigned long SSize(const Ctx *c, unsigned long i) { return i < c->n ? c->sizes[i] : 1UL; }

static int Compute(Ctx *c)
{
    unsigned long i, d, prev, tot = c->n, nch, spcPrev, spc, eNeg;
    long minPts, a;

    for (i = 0; i < c->n; i++) c->disp[i] = i;
    SortIdx(c->pts, c->disp, c->n);
    c->N = 0;
    /* N = max over decode index d of (d - pos[d]) = max over p of (disp[p] - p) */
    for (i = 0; i < c->n; i++)
        if (c->disp[i] > i && c->disp[i] - i > c->N) c->N = c->disp[i] - i;
    if (c->N == 0) return AVCR_NOCHANGE;
    if (c->N > 16) return AVCR_ERR_RANGE;            /* decoder's maximum delay */
    if (!c->stss) return AVCR_ERR_NOSTSS;            /* tails would become key frames */
    if (c->m[c->tkhd + 8] != 0 || c->m[c->mvhd + 8] != 0) return AVCR_ERR_VERSION;

    tot = c->n + c->N;
    /* dts[N] */
    c->dtsN = 0;
    for (i = 0; i < c->N; i++) c->dtsN += c->durs[i];
    minPts = c->pts[c->disp[0]];
    c->last = c->offs[c->n - 1] + c->sizes[c->n - 1] - 1;

    /* media duration */
    c->mediaDur = 0;
    for (i = 0; i < tot; i++)
    {
        d = NewDur(c, i);
        if (d > M32 - c->mediaDur) return AVCR_ERR_RANGE;
        c->mediaDur += d;
    }
    /* start = dts[N] - min(pts) + old media_time */
    a = (long)c->dtsN - minPts;
    if (a > 0 && c->elstMt > 0x7FFFFFFFL - a) return AVCR_ERR_RANGE;
    c->start = a + c->elstMt;

    c->nseg = 0;
    d = c->lead;                                      /* kept leading empty edit */
    eNeg = 0;
    if (c->start < 0)
    {
        if (MulDivRound((unsigned long)(-c->start), c->mts, c->ts, &eNeg)) return AVCR_ERR_RANGE;
        if (eNeg > M32 - d) return AVCR_ERR_RANGE;
        d += eNeg;                                    /* one empty edit for both */
        c->start = 0;
    }
    if (d)
    {
        c->seg[0][0] = d; c->seg[0][1] = M32;         /* media_time -1 */
        c->nseg = 1;
    }
    if ((unsigned long)c->start > c->mediaDur) return AVCR_ERR_RANGE;
    if (MulDivRound(c->mediaDur - (unsigned long)c->start, c->mts, c->ts, &d)) return AVCR_ERR_RANGE;
    if (c->editDur)
    {   /* keep the original trim: the edit ends where the old one ended */
        if (eNeg >= c->editDur) return AVCR_ERR_RANGE;
        if (d > c->editDur - eNeg) d = c->editDur - eNeg;
    }
    c->seg[c->nseg][0] = d; c->seg[c->nseg][1] = (unsigned long)c->start;
    c->nseg++;
    c->movieDur = 0;
    for (i = 0; i < (unsigned long)c->nseg; i++)
    {
        if (c->seg[i][0] > M32 - c->movieDur) return AVCR_ERR_RANGE;
        c->movieDur += c->seg[i][0];
    }

    /* table entry counts */
    c->nSttsEnt = 0; prev = 0;
    for (i = 0; i < tot; i++)
    {
        d = NewDur(c, i);
        if (i == 0 || d != prev) c->nSttsEnt++;
        prev = d;
    }
    nch = 0; c->nStscEnt = 0; spc = 0; spcPrev = 0;
    for (i = 0; i < tot; i++)
    {
        if (i > 0 && SOff(c, i - 1) + SSize(c, i - 1) == SOff(c, i)) { spc++; continue; }
        if (i > 0) { if (nch == 1 || spc != spcPrev) c->nStscEnt++; spcPrev = spc; }
        nch++; spc = 1;
    }
    if (nch == 1 || spc != spcPrev) c->nStscEnt++;
    c->nChunks = nch;
    return AVCR_OK;
}

/* ---------------- emitting the new moov ---------------- */

static void EmitStts(Ctx *c, W *w)
{
    unsigned long i, tot = c->n + c->N, d, prev = 0, run = 0;
    W32(w, 16 + 8 * c->nSttsEnt); WTYPE(w, "stts"); W32(w, 0); W32(w, c->nSttsEnt);
    for (i = 0; i < tot; i++)
    {
        d = NewDur(c, i);
        if (i > 0 && d != prev) { W32(w, run); W32(w, prev); run = 0; }
        prev = d; run++;
    }
    W32(w, run); W32(w, prev);
}
static void EmitStsz(Ctx *c, W *w)
{
    unsigned long i, tot = c->n + c->N;
    W32(w, 20 + 4 * tot); WTYPE(w, "stsz"); W32(w, 0); W32(w, 0); W32(w, tot);
    for (i = 0; i < tot; i++) W32(w, SSize(c, i));
}
/* which: 0 = stsc, 1 = stco */
static void EmitChunks(Ctx *c, W *w, int which)
{
    unsigned long i, tot = c->n + c->N, spc = 0, spcPrev = 0, nch = 0;
    if (which == 0) { W32(w, 16 + 12 * c->nStscEnt); WTYPE(w, "stsc"); W32(w, 0); W32(w, c->nStscEnt); }
    else            { W32(w, 16 + 4 * c->nChunks);   WTYPE(w, "stco"); W32(w, 0); W32(w, c->nChunks); }
    for (i = 0; i < tot; i++)
    {
        if (i > 0 && SOff(c, i - 1) + SSize(c, i - 1) == SOff(c, i)) { spc++; continue; }
        if (which == 1) W32(w, SOff(c, i));
        if (i > 0 && which == 0 && (nch == 1 || spc != spcPrev)) { W32(w, nch); W32(w, spc); W32(w, 1); }
        if (i > 0) spcPrev = spc;
        nch++; spc = 1;
    }
    if (which == 0 && (nch == 1 || spc != spcPrev)) { W32(w, nch); W32(w, spc); W32(w, 1); }
}
static void EmitEdts(Ctx *c, W *w)
{
    int i;
    W32(w, 8 + 16 + 12 * (unsigned long)c->nseg); WTYPE(w, "edts");
    W32(w, 16 + 12 * (unsigned long)c->nseg); WTYPE(w, "elst"); W32(w, 0); W32(w, (unsigned long)c->nseg);
    for (i = 0; i < c->nseg; i++) { W32(w, c->seg[i][0]); W32(w, c->seg[i][1]); W32(w, 0x10000UL); }
}

static void CopyBox(Ctx *c, W *w, unsigned long off)
{
    WBYTES(w, c->m + off, RD32(c->m + off));
}

/* A box whose 32-bit field at payload offset 'at' is replaced */
static void CopyPatched(Ctx *c, W *w, unsigned long off, unsigned long at, unsigned long v)
{
    unsigned long p = w->pos;
    CopyBox(c, w, off);
    PATCH32(w, p + 8 + at, v);
}

static void EmitStsd(Ctx *c, W *w)
{
    unsigned long sd = c->stsd, p = w->pos, cut = c->avcCEnd - sd;
    unsigned long e = sd + 16;
    WBYTES(w, c->m + sd, cut);
    W32(w, 12); WTYPE(w, "rdly"); W32(w, c->N);
    WBYTES(w, c->m + sd + cut, RD32(c->m + sd) - cut);
    PATCH32(w, p, RD32(c->m + sd) + 12);
    PATCH32(w, p + 16, RD32(c->m + e) + 12);
}

static void EmitCont(Ctx *c, W *w, unsigned long box);

static void EmitChild(Ctx *c, W *w, unsigned long off)
{
    const unsigned char *t = c->m + off + 4;
    unsigned long cur;
    if (off == c->vtrak || off == c->mdia || off == c->minf || off == c->stbl) { EmitCont(c, w, off); return; }
    if (off == c->mvhd)
    {
        if (c->m[off + 8] == 0)
        {
            cur = RD32(c->m + off + 24);
            CopyPatched(c, w, off, 16, cur > c->movieDur ? cur : c->movieDur);
        }
        else CopyBox(c, w, off);
        return;
    }
    if (off == c->tkhd)
    {
        if (c->m[off + 8] == 0) CopyPatched(c, w, off, 20, c->movieDur);
        else CopyBox(c, w, off);
        EmitEdts(c, w);
        return;
    }
    if (off == c->edts) return;
    if (off == c->mdhd) { CopyPatched(c, w, off, 16, c->mediaDur); return; }
    if (off == c->stsd) { EmitStsd(c, w); return; }
    if (off == c->stts) { EmitStts(c, w); return; }
    if (off == c->stsz) { EmitStsz(c, w); return; }
    if (off == c->stsc) { EmitChunks(c, w, 0); return; }
    if (off == c->stco || off == c->co64) { EmitChunks(c, w, 1); return; }
    if (c->stbl && off > c->stbl && off < c->stbl + RD32(c->m + c->stbl)
        && (ISTYPE(t, "ctts") || ISTYPE(t, "sdtp") || ISTYPE(t, "cslg")
            || ISTYPE(t, "stdp") || ISTYPE(t, "padb"))) return;   /* per-sample: would go stale */
    CopyBox(c, w, off);
}

static void EmitCont(Ctx *c, W *w, unsigned long box)
{
    unsigned long p = w->pos, end = box + RD32(c->m + box), off = box + 8, s;
    W32(w, 0); WBYTES(w, c->m + box + 4, 4);
    while (off < end)
    {
        s = RD32(c->m + off);
        EmitChild(c, w, off);
        off += s;
    }
    PATCH32(w, p, w->pos - p);
}

/* shift every stco/co64 entry >= splice by growth, in the new moov */
static int Shift(unsigned char *b, unsigned long start, unsigned long end,
                 unsigned long splice, long growth)
{
    unsigned long off = start, s, n, i, q, hi, lo, g;
    int r;
    while (off + 8 <= end)
    {
        s = RD32(b + off);
        if (ISTYPE(b + off + 4, "stco") || ISTYPE(b + off + 4, "co64"))
        {
            int wide = ISTYPE(b + off + 4, "co64");
            n = RD32(b + off + 12);
            for (i = 0; i < n; i++)
            {
                q = off + 16 + i * (wide ? 8UL : 4UL);
                hi = wide ? RD32(b + q) : 0;
                lo = RD32(b + q + (wide ? 4UL : 0UL));
                if (hi == 0 && lo < splice) continue;
                if (growth >= 0)
                {
                    g = (unsigned long)growth;
                    if (lo > M32 - g) { if (!wide) return AVCR_ERR_RANGE; hi = (hi + 1) & M32; }
                    lo = (lo + g) & M32;
                }
                else
                {
                    g = (unsigned long)(-growth);
                    if (lo < g) { if (!wide || hi == 0) return AVCR_ERR_RANGE; hi = hi - 1; }
                    lo = (lo - g) & M32;
                }
                if (wide)
                {
                    b[q] = (unsigned char)(hi >> 24); b[q + 1] = (unsigned char)((hi >> 16) & 0xFFUL);
                    b[q + 2] = (unsigned char)((hi >> 8) & 0xFFUL); b[q + 3] = (unsigned char)(hi & 0xFFUL);
                    q += 4;
                }
                b[q] = (unsigned char)(lo >> 24); b[q + 1] = (unsigned char)((lo >> 16) & 0xFFUL);
                b[q + 2] = (unsigned char)((lo >> 8) & 0xFFUL); b[q + 3] = (unsigned char)(lo & 0xFFUL);
            }
        }
        if (IsCont(b + off + 4))
        {
            r = Shift(b, off + 8, off + s, splice, growth);
            if (r != AVCR_OK) return r;
        }
        off += s;
    }
    return AVCR_OK;
}

/* ---------------- entry point ---------------- */

/* Free the per-track arrays and zero the context (no memset: no libc);
   keeps the moov-level mvhd offset. */
static void ResetCtx(Ctx *c, const AVCR_Input *in, unsigned long mvhd)
{
    unsigned char *z = (unsigned char *)c;
    unsigned long i;
    if (c->in)
    {
        Free(c, c->durs); Free(c, c->sizes); Free(c, c->offs); Free(c, c->disp); Free(c, c->pts);
    }
    for (i = 0; i < (unsigned long)sizeof(*c); i++) z[i] = 0;
    c->in = in; c->m = in->moov; c->msize = in->moovSize; c->mvhd = mvhd;
    c->durs = 0; c->sizes = 0; c->offs = 0; c->disp = 0; c->pts = 0;
}

int AVCR_Rewrite(const AVCR_Input *in, AVCR_Output *out)
{
    Ctx c;
    W w;
    int r;
    unsigned char *nb = 0;

    c.in = 0;
    if (!out) return AVCR_ERR_PARAM;
    out->moov = 0; out->moovSize = 0; out->N = 0; out->growth = 0; out->samples = 0;
    if (!in || !in->moov || !in->alloc || !in->dispose) return AVCR_ERR_PARAM;
    if (in->moovSize > MAXMOOV) return AVCR_ERR_PARAM;
    if (in->inPlace && (in->moovFileOffset > M32 - in->moovSize
        || in->moovFileOffset + in->moovSize > in->fileSize)) return AVCR_ERR_PARAM;

    ResetCtx(&c, in, 0);
    r = FindBoxes(&c);
    if (r == AVCR_OK)
    {   /* the first avc1 video track with N > 0; NOCHANGE ones are passed
           over, any error on an avc1 video track stops (clean reject) */
        unsigned long off = 8, s, mvhd = c.mvhd;
        int sawVideo = 0, sawAvc = 0, isVideo;
        r = SKIP;
        while (off < c.msize && (r == SKIP || r == AVCR_NOCHANGE))
        {
            s = RD32(c.m + off);
            if (ISTYPE(c.m + off + 4, "trak"))
            {
                int avc = IsAvcVideo(&c, off, &isVideo);
                if (isVideo) sawVideo = 1;
                if (avc)
                {
                    sawAvc = 1;
                    ResetCtx(&c, in, mvhd);
                    r = TrackBoxes(&c, off);
                    if (r == AVCR_OK) r = CheckEntry(&c);
                    if (r == AVCR_OK) r = ReadTables(&c);
                    if (r == AVCR_OK) { out->samples = c.n; r = Compute(&c); }
                }
            }
            off += s;
        }
        if (r == SKIP) r = sawAvc ? AVCR_NOCHANGE : (sawVideo ? AVCR_ERR_NOTAVC : AVCR_ERR_NOVIDEO);
    }
    if (r == AVCR_OK)
    {
        w.b = 0; w.pos = 0;
        EmitCont(&c, &w, 0);
        if (w.pos > MAXMOOV) r = AVCR_ERR_RANGE;
    }
    if (r == AVCR_OK)
    {
        nb = (unsigned char *)Alloc(&c, w.pos);
        if (!nb) r = AVCR_ERR_MEM;
    }
    if (r == AVCR_OK)
    {
        unsigned long size = w.pos;
        w.b = nb; w.pos = 0;
        EmitCont(&c, &w, 0);
        if (w.pos != size) r = AVCR_ERR_STRUCT;    /* cannot happen */
        else
        {
            out->growth = (long)size - (long)c.msize;
            r = in->inPlace ? Shift(nb, 8, size, in->moovFileOffset + c.msize, out->growth) : AVCR_OK;
        }
        if (r == AVCR_OK)
        {
            out->moov = nb; out->moovSize = size; out->N = c.N;
            nb = 0;
        }
        else out->growth = 0;
    }
    if (r == AVCR_NOCHANGE) out->N = 0;
    Free(&c, nb);
    Free(&c, c.durs); Free(&c, c.sizes); Free(&c, c.offs); Free(&c, c.disp); Free(&c, c.pts);
    return r;
}

const char *AVCR_ErrorString(int code)
{
    switch (code)
    {
    case AVCR_OK:             return "rewritten";
    case AVCR_NOCHANGE:       return "no B-frame reordering: unchanged";
    case AVCR_ERR_PARAM:      return "bad parameters";
    case AVCR_ERR_MEM:        return "out of memory";
    case AVCR_ERR_STRUCT:     return "malformed moov";
    case AVCR_ERR_NOVIDEO:    return "no video track";
    case AVCR_ERR_FRAGMENTED: return "fragmented movie";
    case AVCR_ERR_STZ2:       return "stz2 sample sizes not supported";
    case AVCR_ERR_MULTIDESC:  return "more than one video sample description";
    case AVCR_ERR_NOTAVC:     return "video is not avc1 with avcC";
    case AVCR_ERR_TABLES:     return "sample tables disagree";
    case AVCR_ERR_RANGE:      return "value out of 32-bit range";
    case AVCR_ERR_VERSION:    return "version-1 mdhd not supported";
    case AVCR_ERR_OFFSETS:    return "video sample outside the file or inside moov";
    case AVCR_ERR_ALREADY:    return "already rewritten (rdly present)";
    case AVCR_ERR_EDITS:      return "more than one media edit in the video edit list";
    case AVCR_ERR_NOSTSS:     return "no stss (every sample a key frame) with B-frames";
    }
    return "unknown error";
}
