/*
    mc_core.c -- portable core of Classic Media Center 0.1.
    See mc_core.h. Strict C89, no Mac headers, no malloc, 32-bit only.
*/
#include <string.h>
#include <stdio.h>
#include "mc_core.h"

/* ==================================================================
   small helpers
   ================================================================== */

static int mc_lower(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

static int mc_streqi(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (mc_lower((unsigned char)*a) != mc_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int mc_startsi(const char *s, const char *prefix)
{
    while (*prefix)
    {
        if (mc_lower((unsigned char)*s) != mc_lower((unsigned char)*prefix))
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static int mc_endsi(const char *s, const char *suffix)
{
    long ls = (long)strlen(s), lx = (long)strlen(suffix);
    if (lx > ls) return 0;
    return mc_streqi(s + ls - lx, suffix);
}

/* copy with truncation; dst always terminated */
static void mc_copy(char *dst, const char *src, long max)
{
    long n = (long)strlen(src);
    if (n > max - 1) n = max - 1;
    memcpy(dst, src, (size_t)n);
    dst[n] = 0;
}

/* decimal text -> unsigned long, clamped; stops at first non-digit
   (so "26989036" and 26989036 and "370.3" all work) */
static unsigned long mc_atoul(const char *s)
{
    unsigned long v = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9')
    {
        unsigned long d = (unsigned long)(*s - '0');
        if (v > (0xFFFFFFFFUL - d) / 10UL) return 0xFFFFFFFFUL;
        v = v * 10UL + d;
        s++;
    }
    return v;
}

static unsigned long mc_rd32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
         | ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
}

static unsigned int mc_rd16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

static const char kHex[] = "0123456789ABCDEF";

/* ==================================================================
   a. URL building
   ================================================================== */

static int mc_unreserved(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'
        || c == '~';
}

static int mc_escape(const char *s, char *out, long outMax, int isPath)
{
    long at = 0;
    const unsigned char *p = (const unsigned char *)s;

    for (; *p; p++)
    {
        int c = *p;
        if (mc_unreserved(c) || (isPath && c == '/'))
        {
            if (at + 1 >= outMax) return -1;
            out[at++] = (char)c;
        }
        else if (!isPath && c == ' ')
        {
            if (at + 1 >= outMax) return -1;
            out[at++] = '+';
        }
        else
        {
            if (at + 3 >= outMax) return -1;
            out[at++] = '%';
            out[at++] = kHex[(c >> 4) & 15];
            out[at++] = kHex[c & 15];
        }
    }
    if (at >= outMax) return -1;
    out[at] = 0;
    return 0;
}

int mc_query_escape(const char *s, char *out, long outMax)
{
    return mc_escape(s, out, outMax, 0);
}

int mc_path_escape(const char *s, char *out, long outMax)
{
    return mc_escape(s, out, outMax, 1);
}

int mc_search_url(const char *terms, int page, char *out, long outMax)
{
    char clean[204];
    char enc[620];
    long n = 0;
    const unsigned char *p = (const unsigned char *)terms;

    for (; *p && n < 200; p++)
    {
        int c = *p;
        if (c < 0x21 || c > 0x7E || strchr("():[]{}^~\"\\", c) != NULL)
            c = ' ';
        if (c == ' ' && (n == 0 || clean[n - 1] == ' ')) continue;
        clean[n++] = (char)c;
    }
    while (n > 0 && clean[n - 1] == ' ') n--;
    clean[n] = 0;
    if (n == 0) return -1;
    if (page < 1) page = 1;
    if (page > 9999) page = 9999;
    if (mc_query_escape(clean, enc, (long)sizeof(enc)) != 0) return -2;

    /* fixed parts: about 200 chars */
    if ((long)strlen(enc) + 220 > outMax) return -2;
    sprintf(out,
        "http://archive.org/advancedsearch.php?q=%%28%s%%29"
        "+AND+mediatype%%3Amovies+AND+format%%3A%%28512Kb+MPEG4%%29"
        "&fl%%5B%%5D=identifier%%2Ctitle&sort%%5B%%5D=downloads+desc"
        "&rows=%d&page=%d&output=json", enc, MC_MAXHITS, page);
    return 0;
}

/* ==================================================================
   b. push JSON scanner
   ================================================================== */

enum
{
    ST_VALUE = 0, ST_ARR_FIRST, ST_OBJ_FIRST, ST_OBJ_KEY, ST_COLON,
    ST_AFTER, ST_STR, ST_ESC, ST_HEX, ST_LIT, ST_DONE, ST_ERR
};

/* U+00C0..U+00FF folded to ASCII */
static const char kLatin[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
    "aaaaaaaceeeeiiiidnooooo/ouuuuyty";

static void js_put(McJson *j, int c)
{
    if (j->slen < MC_JSON_STRMAX - 1) j->str[j->slen++] = (char)c;
    else j->truncated = 1;
}

static void js_put_code(McJson *j, unsigned long cp)
{
    int c = '?';
    if (cp < 0x20) c = ' ';
    else if (cp < 0x7F) c = (int)cp;
    else if (cp == 0xA0) c = ' ';
    else if (cp >= 0xC0 && cp <= 0xFF) c = kLatin[cp - 0xC0];
    else if (cp == 0x2018 || cp == 0x2019 || cp == 0xB4) c = '\'';
    else if (cp == 0x201C || cp == 0x201D || cp == 0xAB || cp == 0xBB)
        c = '"';
    else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2010) c = '-';
    else if (cp == 0x2026) c = '.';
    js_put(j, c);
}

void mc_json_init(McJson *j, McJsonEvent cb, void *user)
{
    memset(j, 0, sizeof(*j));
    j->state = ST_VALUE;
    j->cb = cb;
    j->user = user;
}

static void js_emit(McJson *j, int kind, const char *v, long n)
{
    if (j->cb != NULL) j->cb(j->user, j, kind, v, n);
}

static void js_value_done(McJson *j)
{
    j->state = (j->depth == 0) ? ST_DONE : ST_AFTER;
}

static int js_fail(McJson *j)
{
    j->err = 1;
    j->state = ST_ERR;
    return -1;
}

static int js_open(McJson *j, int c)
{
    if (j->depth >= MC_JSON_DEPTH) return js_fail(j);
    j->kind[j->depth] = (char)(c == '{' ? 'o' : 'a');
    j->key[j->depth][0] = 0;
    j->index[j->depth] = 0;
    j->depth++;
    js_emit(j, c == '{' ? MC_J_OBJ_BEGIN : MC_J_ARR_BEGIN, NULL, 0);
    j->state = (c == '{') ? ST_OBJ_FIRST : ST_ARR_FIRST;
    return 0;
}

static int js_close(McJson *j, int c)
{
    char want = (char)(c == '}' ? 'o' : 'a');
    if (j->depth < 1 || j->kind[j->depth - 1] != want) return js_fail(j);
    js_emit(j, c == '}' ? MC_J_OBJ_END : MC_J_ARR_END, NULL, 0);
    j->depth--;
    js_value_done(j);
    return 0;
}

static void js_begin_str(McJson *j, int isKey)
{
    j->isKey = isKey;
    j->slen = 0;
    j->truncated = 0;
    j->utfNeed = 0;
    j->state = ST_STR;
}

static int js_is_litchar(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z') || c == '+' || c == '-' || c == '.';
}

static int js_end_lit(McJson *j)
{
    j->str[j->slen] = 0;
    if (strcmp(j->str, "true") == 0 || strcmp(j->str, "false") == 0
        || strcmp(j->str, "null") == 0)
    {
        js_emit(j, MC_J_LIT, j->str, j->slen);
    }
    else
    {
        /* -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)? */
        const char *q = j->str;
        if (*q == '-') q++;
        if (*q == '0') q++;
        else if (*q >= '1' && *q <= '9')
        { while (*q >= '0' && *q <= '9') q++; }
        else return js_fail(j);
        if (*q == '.')
        {
            q++;
            if (!(*q >= '0' && *q <= '9')) return js_fail(j);
            while (*q >= '0' && *q <= '9') q++;
        }
        if (*q == 'e' || *q == 'E')
        {
            q++;
            if (*q == '+' || *q == '-') q++;
            if (!(*q >= '0' && *q <= '9')) return js_fail(j);
            while (*q >= '0' && *q <= '9') q++;
        }
        if (*q != 0) return js_fail(j);
        js_emit(j, MC_J_NUM, j->str, j->slen);
    }
    js_value_done(j);
    return 0;
}

/* a value may start here (ST_VALUE / ST_ARR_FIRST) */
static int js_value_start(McJson *j, int c)
{
    if (c == '{' || c == '[') return js_open(j, c);
    if (c == '"') { js_begin_str(j, 0); return 0; }
    if (c == '-' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))
    {
        j->slen = 0;
        j->str[j->slen++] = (char)c;
        j->state = ST_LIT;
        return 0;
    }
    return js_fail(j);
}

static int js_end_str(McJson *j)
{
    if (j->utfNeed > 0) { js_put(j, '?'); j->utfNeed = 0; }
    j->str[j->slen] = 0;
    if (j->isKey)
    {
        mc_copy(j->key[j->depth - 1], j->str, MC_JSON_KEYMAX);
        j->state = ST_COLON;
    }
    else
    {
        js_emit(j, MC_J_STR, j->str, j->slen);
        js_value_done(j);
    }
    return 0;
}

static int js_byte(McJson *j, int c)
{
    int ws = (c == ' ' || c == '\t' || c == '\r' || c == '\n');

    switch (j->state)
    {
    case ST_ERR:
        return -1;

    case ST_DONE:
        return ws ? 0 : js_fail(j);

    case ST_VALUE:
        if (ws) return 0;
        return js_value_start(j, c);

    case ST_ARR_FIRST:
        if (ws) return 0;
        if (c == ']') return js_close(j, c);
        return js_value_start(j, c);

    case ST_OBJ_FIRST:
        if (ws) return 0;
        if (c == '}') return js_close(j, c);
        if (c == '"') { js_begin_str(j, 1); return 0; }
        return js_fail(j);

    case ST_OBJ_KEY:
        if (ws) return 0;
        if (c == '"') { js_begin_str(j, 1); return 0; }
        return js_fail(j);

    case ST_COLON:
        if (ws) return 0;
        if (c == ':') { j->state = ST_VALUE; return 0; }
        return js_fail(j);

    case ST_AFTER:
        if (ws) return 0;
        if (c == '}' || c == ']') return js_close(j, c);
        if (c == ',')
        {
            if (j->kind[j->depth - 1] == 'o') j->state = ST_OBJ_KEY;
            else { j->index[j->depth - 1]++; j->state = ST_VALUE; }
            return 0;
        }
        return js_fail(j);

    case ST_STR:
        if (c >= 0x80)
        {
            if ((c & 0xC0) == 0x80)             /* continuation byte */
            {
                if (j->utfNeed > 0)
                {
                    j->utfCode = (j->utfCode << 6) | (unsigned long)(c & 0x3F);
                    if (--j->utfNeed == 0) js_put_code(j, j->utfCode);
                }
                else js_put(j, '?');
                return 0;
            }
            if (j->utfNeed > 0) js_put(j, '?');  /* broken sequence */
            if (c >= 0xC0 && c <= 0xDF)
            { j->utfNeed = 1; j->utfCode = (unsigned long)(c & 0x1F); }
            else if (c >= 0xE0 && c <= 0xEF)
            { j->utfNeed = 2; j->utfCode = (unsigned long)(c & 0x0F); }
            else if (c >= 0xF0 && c <= 0xF7)
            { j->utfNeed = 3; j->utfCode = (unsigned long)(c & 0x07); }
            else { j->utfNeed = 0; js_put(j, '?'); }
            return 0;
        }
        if (j->utfNeed > 0) { js_put(j, '?'); j->utfNeed = 0; }
        if (c == '"') return js_end_str(j);
        if (c == '\\') { j->state = ST_ESC; return 0; }
        if (c < 0x20) return js_fail(j);
        js_put(j, c);
        return 0;

    case ST_ESC:
        j->state = ST_STR;
        switch (c)
        {
        case '"': case '\\': case '/': js_put(j, c); return 0;
        case 'b': case 'f': case 'n': case 'r': case 't':
            js_put(j, ' ');
            return 0;
        case 'u':
            j->hexN = 0;
            j->hexV = 0;
            j->state = ST_HEX;
            return 0;
        }
        return js_fail(j);

    case ST_HEX:
        {
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return js_fail(j);
            j->hexV = (j->hexV << 4) | (unsigned long)d;
            if (++j->hexN < 4) return 0;
            j->state = ST_STR;
            if (j->hexV >= 0xDC00 && j->hexV <= 0xDFFF) return 0; /* low half */
            if (j->hexV >= 0xD800 && j->hexV <= 0xDBFF) { js_put(j, '?'); return 0; }
            js_put_code(j, j->hexV);
            return 0;
        }

    case ST_LIT:
        if (js_is_litchar(c))
        {
            if (j->slen >= 40) return js_fail(j);
            j->str[j->slen++] = (char)c;
            return 0;
        }
        if (js_end_lit(j) != 0) return -1;
        return js_byte(j, c);       /* reprocess the terminator (once) */
    }
    return js_fail(j);
}

int mc_json_feed(McJson *j, const char *buf, long n)
{
    long i;
    if (j->err) return -1;
    for (i = 0; i < n; i++)
    {
        if (js_byte(j, (unsigned char)buf[i]) != 0) return -1;
        j->bytes++;
    }
    return 0;
}

int mc_json_finish(McJson *j)
{
    if (j->err) return -1;
    if (j->state == ST_LIT && j->depth == 0)
    {
        if (js_end_lit(j) != 0) return -1;
    }
    return (j->state == ST_DONE) ? 0 : -1;
}

/* path test: are we at depth d with these keys? keys[i]==NULL means
   "any" (array level). */
static int js_at(const McJson *j, int d, const char *k0, const char *k1)
{
    if (j->depth != d) return 0;
    if (d >= 1 && k0 != NULL &&
        (j->kind[0] != 'o' || strcmp(j->key[0], k0) != 0)) return 0;
    if (d >= 2 && k1 != NULL &&
        (j->kind[1] != 'o' || strcmp(j->key[1], k1) != 0)) return 0;
    return 1;
}

/* ==================================================================
   search results: { "response": { "numFound": N,
                       "docs": [ { "identifier": "...",
                                   "title": "..." or ["...",..] } ] } }
   ================================================================== */

static void search_cb(void *user, const McJson *j, int kind,
                      const char *val, long len)
{
    McSearch *s = (McSearch *)user;
    (void)len;

    if (kind == MC_J_OBJ_BEGIN && js_at(j, 4, "response", "docs")
        && j->kind[2] == 'a')
    {
        memset(&s->tmp, 0, sizeof(s->tmp));
        s->titleSeen = 0;
        s->cur = 1;
        return;
    }
    if (kind == MC_J_OBJ_END && s->cur && j->depth == 4)
    {
        s->cur = 0;
        if (s->tmp.identifier[0] != 0 && s->count < MC_MAXHITS)
        {
            if (s->tmp.title[0] == 0)
                mc_copy(s->tmp.title, s->tmp.identifier, MC_TITLEMAX);
            s->hits[s->count++] = s->tmp;
        }
        return;
    }
    if (kind == MC_J_NUM && js_at(j, 2, "response", "numFound"))
    {
        s->numFound = (long)mc_atoul(val);
        return;
    }
    if (kind != MC_J_STR || !s->cur) return;

    if (j->depth == 4 && j->kind[3] == 'o')
    {
        if (strcmp(j->key[3], "identifier") == 0)
        {
            /* a cut identifier is useless: leave it empty -> skipped */
            if (!j->truncated && len < MC_IDMAX)
                mc_copy(s->tmp.identifier, val, MC_IDMAX);
        }
        else if (strcmp(j->key[3], "title") == 0 && !s->titleSeen)
        {
            mc_copy(s->tmp.title, val, MC_TITLEMAX);
            s->titleSeen = 1;
        }
    }
    else if (j->depth == 5 && j->kind[3] == 'o' && j->kind[4] == 'a'
             && strcmp(j->key[3], "title") == 0 && !s->titleSeen)
    {
        mc_copy(s->tmp.title, val, MC_TITLEMAX);
        s->titleSeen = 1;
    }
}

void mc_search_begin(McSearch *s)
{
    memset(s, 0, sizeof(*s));
    s->numFound = -1;
    mc_json_init(&s->j, search_cb, s);
}

int mc_search_feed(McSearch *s, const char *buf, long n)
{
    return mc_json_feed(&s->j, buf, n);
}

int mc_search_end(McSearch *s)
{
    if (mc_json_finish(&s->j) != 0) return -1;
    if (s->numFound < 0) return -1;    /* not a search response */
    return s->count;
}

/* ==================================================================
   c. item metadata
   ================================================================== */

static int mc_format_rank(const char *fmt)
{
    if (strcmp(fmt, "512Kb MPEG4") == 0) return MC_RANK_512KB;
    if (strcmp(fmt, "MPEG4") == 0) return MC_RANK_MPEG4;
    if (mc_streqi(fmt, "h.264 IA")) return MC_RANK_H264IA;
    if (mc_startsi(fmt, "h.264")) return MC_RANK_H264;
    return 0;
}

static void meta_add_server(McMeta *m, const char *s, long len, int cut)
{
    int i;
    if (cut || len == 0 || len >= MC_SRVMAX) return;
    for (i = 0; i < m->nsrv; i++)
        if (mc_streqi(m->server[i], s)) return;
    if (m->nsrv >= MC_MAXSRV) return;
    mc_copy(m->server[m->nsrv++], s, MC_SRVMAX);
}

static void meta_commit(McMeta *m)
{
    McCand *c = &m->tmp;
    int i, at;

    c->rank = mc_format_rank(c->format);
    if (c->rank == 0) return;
    if (!(mc_endsi(c->name, ".mp4") || mc_endsi(c->name, ".m4v")
          || mc_endsi(c->name, ".mov")))
        return;
    if (m->tmpBad || c->name[0] == 0) { m->droppedCand++; return; }

    /* stable insert: after every entry of the same or better rank */
    at = m->ncand;
    while (at > 0 && m->cand[at - 1].rank > c->rank) at--;
    if (at >= MC_MAXCAND) { m->droppedCand++; return; }
    if (m->ncand == MC_MAXCAND) { m->ncand--; m->droppedCand++; }
    for (i = m->ncand; i > at; i--) m->cand[i] = m->cand[i - 1];
    m->cand[at] = *c;
    m->ncand++;
}

static void meta_cb(void *user, const McJson *j, int kind,
                    const char *val, long len)
{
    McMeta *m = (McMeta *)user;
    int scalar = (kind == MC_J_STR || kind == MC_J_NUM || kind == MC_J_LIT);

    if ((scalar && j->depth == 1) ||
        ((kind == MC_J_OBJ_BEGIN || kind == MC_J_ARR_BEGIN) && j->depth == 2))
        m->topKeys++;

    /* files[i] objects */
    if (kind == MC_J_OBJ_BEGIN && js_at(j, 3, "files", NULL)
        && j->kind[1] == 'a')
    {
        memset(&m->tmp, 0, sizeof(m->tmp));
        m->tmpBad = 0;
        return;
    }
    if (kind == MC_J_OBJ_END && js_at(j, 3, "files", NULL)
        && j->kind[1] == 'a')
    {
        meta_commit(m);
        return;
    }
    if (scalar && js_at(j, 3, "files", NULL) && j->kind[1] == 'a'
        && j->kind[2] == 'o')
    {
        const char *k = j->key[2];
        if (kind == MC_J_STR && strcmp(k, "name") == 0)
        {
            if (j->truncated || len >= MC_NAMEMAX) m->tmpBad = 1;
            else mc_copy(m->tmp.name, val, MC_NAMEMAX);
        }
        else if (kind == MC_J_STR && strcmp(k, "format") == 0)
            mc_copy(m->tmp.format, val, MC_FMTMAX);
        else if (strcmp(k, "size") == 0)
            m->tmp.size = mc_atoul(val);
        else if (strcmp(k, "width") == 0)
            m->tmp.width = (int)(mc_atoul(val) & 0x7FFF);
        else if (strcmp(k, "height") == 0)
            m->tmp.height = (int)(mc_atoul(val) & 0x7FFF);
        return;
    }

    if (kind == MC_J_STR && j->depth == 1 && j->kind[0] == 'o')
    {
        const char *k = j->key[0];
        if (strcmp(k, "server") == 0 || strcmp(k, "d1") == 0
            || strcmp(k, "d2") == 0)
            meta_add_server(m, val, len, j->truncated);
        else if (strcmp(k, "dir") == 0 && !j->truncated && len < MC_DIRMAX)
            mc_copy(m->dir, val, MC_DIRMAX);
        return;
    }
    if (kind == MC_J_LIT && js_at(j, 1, "is_dark", NULL)
        && strcmp(val, "true") == 0)
    {
        m->isDark = 1;
        return;
    }
    if (kind == MC_J_STR && js_at(j, 2, "workable_servers", NULL)
        && j->kind[1] == 'a')
    {
        meta_add_server(m, val, len, j->truncated);
        return;
    }
    if (kind == MC_J_STR && !m->titleSeen &&
        (js_at(j, 2, "metadata", "title") ||
         (js_at(j, 3, "metadata", "title") && j->kind[2] == 'a')))
    {
        mc_copy(m->title, val, MC_TITLEMAX);
        m->titleSeen = 1;
    }
}

void mc_meta_begin(McMeta *m)
{
    memset(m, 0, sizeof(*m));
    mc_json_init(&m->j, meta_cb, m);
}

int mc_meta_feed(McMeta *m, const char *buf, long n)
{
    return mc_json_feed(&m->j, buf, n);
}

int mc_meta_end(McMeta *m)
{
    if (mc_json_finish(&m->j) != 0) return -1;
    if (m->j.kind[0] != 'o') return -1;          /* top level not {} */
    if (m->topKeys == 0) return -2;
    if (m->isDark) return -3;
    return 0;
}

/* ==================================================================
   d. URL attempts
   ================================================================== */

int mc_attempt_urls(const char *id, const McMeta *m, const McCand *c,
                    char urls[][MC_URLMAX], int maxUrls)
{
    char escName[MC_URLMAX];
    char escId[MC_URLMAX / 4];
    int  n = 0, pass, i;

    if (maxUrls < 1) return -1;
    if (mc_path_escape(c->name, escName, (long)sizeof(escName)) != 0)
        return -1;
    if (mc_path_escape(id, escId, (long)sizeof(escId)) != 0) return -1;
    if (strlen(escName) + strlen(escId) + 32 > MC_URLMAX) return -1;
    sprintf(urls[n++], "http://archive.org/download/%s/%s", escId, escName);

    if (m->dir[0] != '/') return n;
    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < m->nsrv && n < maxUrls; i++)
        {
            int us = strstr(m->server[i], ".us.") != NULL;
            if (us != (pass == 0)) continue;
            if (strlen(m->server[i]) + strlen(m->dir) + strlen(escName) + 10
                > MC_URLMAX) continue;
            sprintf(urls[n++], "http://%s%s/%s", m->server[i], m->dir,
                    escName);
        }
    }
    return n;
}

/* ==================================================================
   e. what is in the moov
   ================================================================== */

/* Box at off within [off,end): header length and total size.
   0 ok, -1 malformed. 64-bit sizes accepted only if the high half
   is 0 (as in H264Stream.c). */
static int mc_box(const unsigned char *b, long off, long end,
                  long *hdr, long *size)
{
    unsigned long s;
    if (off < 0 || end - off < 8) return -1;
    s = mc_rd32(b + off);
    *hdr = 8;
    if (s == 1)
    {
        if (end - off < 16) return -1;
        if (mc_rd32(b + off + 8) != 0) return -1;
        s = mc_rd32(b + off + 12);
        *hdr = 16;
    }
    else if (s == 0)
    {
        s = (unsigned long)(end - off);
    }
    if (s < (unsigned long)*hdr || s > (unsigned long)(end - off)) return -1;
    *size = (long)s;
    return 0;
}

static int mc_is(const unsigned char *t, const char *four)
{
    return t[0] == (unsigned char)four[0] && t[1] == (unsigned char)four[1]
        && t[2] == (unsigned char)four[2] && t[3] == (unsigned char)four[3];
}

static int mc_visual(const unsigned char *t)
{
    static const char *const kVis[] =
    {
        "avc1", "avc3", "mp4v", "hvc1", "hev1", "vp08", "vp09", "av01",
        "s263", "h263", "jpeg", "mjpa", "SVQ1", "SVQ3", "cvid", "encv",
        NULL
    };
    int i;
    for (i = 0; kVis[i] != NULL; i++)
        if (mc_is(t, kVis[i])) return 1;
    return 0;
}

/* 1 found, 0 not here, -1 malformed */
static int mc_stsd(const unsigned char *b, long off, long end,
                   McAvcInfo *info)
{
    unsigned long count, i;
    long e, hdr, size;

    if (end - off < 8) return -1;
    count = mc_rd32(b + off + 4);
    e = off + 8;
    for (i = 0; i < count && e < end; i++)
    {
        if (mc_box(b, e, end, &hdr, &size) != 0) return -1;
        if (mc_visual(b + e + 4))
        {
            long v = e + hdr, c, ch, cs;
            memcpy(info->codec, b + e + 4, 4);
            info->codec[4] = 0;
            if (size - hdr < 78) return -1;
            info->width  = (int)mc_rd16(b + v + 24);
            info->height = (int)mc_rd16(b + v + 26);
            for (c = v + 78; c < e + size; c += cs)
            {
                if (mc_box(b, c, e + size, &ch, &cs) != 0) return -1;
                if (mc_is(b + c + 4, "avcC") && cs - ch >= 4)
                {
                    info->profile = b[c + ch + 1];
                    info->compat  = b[c + ch + 2];
                    info->level   = b[c + ch + 3];
                    break;
                }
            }
            return 1;
        }
        e += size;
    }
    return 0;
}

static int mc_walk(const unsigned char *b, long off, long end,
                   McAvcInfo *info, int depth)
{
    long hdr, size;
    int r;

    if (depth > 8) return -1;
    while (off < end)
    {
        if (mc_box(b, off, end, &hdr, &size) != 0) return -1;
        if (mc_is(b + off + 4, "trak") || mc_is(b + off + 4, "mdia")
            || mc_is(b + off + 4, "minf") || mc_is(b + off + 4, "stbl"))
        {
            r = mc_walk(b, off + hdr, off + size, info, depth + 1);
            if (r != 0) return r;
        }
        else if (mc_is(b + off + 4, "stsd"))
        {
            r = mc_stsd(b, off + hdr, off + size, info);
            if (r != 0) return r;
        }
        off += size;
    }
    return 0;
}

int mc_avc_profile(const unsigned char *moov, long len, McAvcInfo *info)
{
    long hdr, size;
    int r;

    memset(info, 0, sizeof(*info));
    info->profile = -1;
    if (moov == NULL || mc_box(moov, 0, len, &hdr, &size) != 0) return -2;
    if (!mc_is(moov + 4, "moov")) return -2;
    r = mc_walk(moov, hdr, size, info, 0);
    if (r < 0) return -2;
    return r == 1 ? 0 : -1;
}

static const char *mc_profile_name(int p)
{
    switch (p)
    {
    case 66:  return "Baseline";
    case 77:  return "Main";
    case 88:  return "Extended";
    case 100: return "High";
    case 110: return "High 10";
    case 122: return "High 4:2:2";
    case 244: return "High 4:4:4";
    }
    return "an unknown";
}

int mc_avc_verdict(const McAvcInfo *info, int found, char *msg, long msgMax)
{
    char line[200];
    int  r;

    if (found == -2)
    { strcpy(line, "The movie header is damaged."); r = MC_PLAY_UNKNOWN; }
    else if (found != 0)
    { strcpy(line, "This file has no video track."); r = MC_PLAY_NO_VIDEO; }
    else if (mc_is((const unsigned char *)info->codec, "avc1")
             || mc_is((const unsigned char *)info->codec, "avc3"))
    {
        if (info->profile < 0)
        {
            strcpy(line, "This H.264 video has no avcC settings box.");
            r = MC_PLAY_UNKNOWN;
        }
        else if (info->profile != 66)
        {
            /* h264bsd parses the SPS as Baseline syntax only (no
               chroma_format_idc etc.) and has no CABAC, B-slices or
               interlace: profile 66 is the only safe answer. */
            sprintf(line, "This video is %s profile (%d); this decoder "
                    "plays Baseline only.", mc_profile_name(info->profile),
                    info->profile);
            r = MC_PLAY_NOT_BASE;
        }
        else if (info->width > MC_MAX_WIDTH || info->height > MC_MAX_HEIGHT)
        {
            sprintf(line, "This video is %dx%d; the limit is %dx%d.",
                    info->width, info->height, MC_MAX_WIDTH, MC_MAX_HEIGHT);
            r = MC_PLAY_TOO_BIG;
        }
        else
        {
            sprintf(line, "Baseline H.264, level %d.%d, %dx%d.",
                    info->level / 10, info->level % 10,
                    info->width, info->height);
            r = MC_PLAY_OK;
        }
    }
    else if (mc_is((const unsigned char *)info->codec, "mp4v"))
    {
        sprintf(line, "MPEG-4 video, %dx%d (QuickTime plays this itself).",
                info->width, info->height);
        r = MC_PLAY_NATIVE;
    }
    else
    {
        sprintf(line, "This video is '%.4s', which QuickTime 6 cannot play.",
                info->codec);
        r = MC_PLAY_UNKNOWN;
    }
    if (msg != NULL && msgMax > 0) mc_copy(msg, line, msgMax);
    return r;
}
