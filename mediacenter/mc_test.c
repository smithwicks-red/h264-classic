/* mc_test.c -- host tests for mc_core.c (gcc, C89). Run: ./run_tests.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc_core.h"

static int gPass = 0, gFail = 0;

#define CHECK(cond) do { if (cond) gPass++; else { gFail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKSTR(a, b) do { if (strcmp((a), (b)) == 0) gPass++; else { \
    gFail++; printf("FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, \
    __LINE__, (a), (b)); } } while (0)

static char gDir[512];

static char *load(const char *name, long *len)
{
    char path[700];
    FILE *f;
    char *b;
    long n;
    sprintf(path, "%s/%s", gDir, name);
    f = fopen(path, "rb");
    if (f == NULL) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)(n > 0 ? n : 1));
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { printf("read\n"); exit(2); }
    fclose(f);
    /* drop trailing whitespace so every shorter prefix is incomplete */
    while (n > 0 && (b[n - 1] == '\n' || b[n - 1] == ' ' || b[n - 1] == '\r'))
        n--;
    *len = n;
    return b;
}

/* exact-size heap copy: ASan flags any read past the end */
static char *dup_exact(const char *src, long n)
{
    char *p = (char *)malloc((size_t)(n > 0 ? n : 1));
    memcpy(p, src, (size_t)n);
    return p;
}

static int search_buf(const char *b, long n, McSearch *s, long chunk)
{
    long at = 0;
    mc_search_begin(s);
    while (at < n)
    {
        long k = n - at;
        if (k > chunk) k = chunk;
        if (mc_search_feed(s, b + at, k) != 0) break;
        at += k;
    }
    return mc_search_end(s);
}

static int meta_buf(const char *b, long n, McMeta *m, long chunk)
{
    long at = 0;
    mc_meta_begin(m);
    while (at < n)
    {
        long k = n - at;
        if (k > chunk) k = chunk;
        if (mc_meta_feed(m, b + at, k) != 0) break;
        at += k;
    }
    return mc_meta_end(m);
}

static McSearch gS, gS2;
static McMeta gM, gM2;

/* every proper prefix must fail cleanly; whole must equal 'want' */
static void prefixes(const char *name, int isMeta)
{
    long n, i;
    char *b = load(name, &n);
    int bad = 0;
    for (i = 0; i < n; i++)
    {
        char *p = dup_exact(b, i);
        int r = isMeta ? meta_buf(p, i, &gM2, 7) : search_buf(p, i, &gS2, 7);
        if (r >= 0 && !(isMeta && r == 0 && 0)) bad++;
        free(p);
    }
    if (bad) printf("  %s: %d prefixes wrongly accepted\n", name, bad);
    CHECK(bad == 0);
    free(b);
}

/* ---------------- a ---------------- */
static void test_urls(void)
{
    char out[MC_URLMAX];
    CHECK(mc_search_url("popeye", 1, out, sizeof(out)) == 0);
    CHECKSTR(out, "http://archive.org/advancedsearch.php?q=%28popeye%29"
        "+AND+mediatype%3Amovies+AND+format%3A%28512Kb+MPEG4%29"
        "&fl%5B%5D=identifier%2Ctitle&sort%5B%5D=downloads+desc"
        "&rows=25&page=1&output=json");
    CHECK(mc_search_url("  popeye   (president):  \"1956\"  ", 3, out,
                        sizeof(out)) == 0);
    CHECK(strstr(out, "q=%28popeye+president+1956%29+AND") != NULL);
    CHECK(strstr(out, "&page=3&") != NULL);
    CHECK(mc_search_url("bugs&bunny+co/op", 0, out, sizeof(out)) == 0);
    CHECK(strstr(out, "q=%28bugs%26bunny%2Bco%2Fop%29") != NULL);
    CHECK(strstr(out, "&page=1&") != NULL);
    CHECK(mc_search_url("caf\216 noir", 1, out, sizeof(out)) == 0);
    CHECK(strstr(out, "q=%28caf+noir%29") != NULL);
    CHECK(mc_search_url("  ()::  ", 1, out, sizeof(out)) == -1);
    CHECK(mc_search_url("", 1, out, sizeof(out)) == -1);
    CHECK(mc_search_url("popeye", 1, out, 100) == -2);
    CHECK(mc_path_escape("My Film (1950) #1.mp4", out, sizeof(out)) == 0);
    CHECKSTR(out, "My%20Film%20%281950%29%20%231.mp4");
    CHECK(mc_path_escape("sub/dir/a b.mp4", out, sizeof(out)) == 0);
    CHECKSTR(out, "sub/dir/a%20b.mp4");
    CHECK(mc_path_escape("abc", out, 4) == 0);
    CHECK(mc_path_escape("abc", out, 3) == -1);
    CHECK(mc_path_escape(" ", out, 3) == -1);
    CHECK(mc_query_escape("a b", out, sizeof(out)) == 0);
    CHECKSTR(out, "a+b");
}

/* ---------------- b ---------------- */
static void test_search(void)
{
    long n, chunk;
    char *b;
    int r;

    b = load("search_popeye.json", &n);
    r = search_buf(b, n, &gS, n);
    CHECK(r == 9);
    CHECK(gS.numFound == 9);
    CHECKSTR(gS.hits[0].identifier, "Popeye_forPresident");
    CHECKSTR(gS.hits[0].title, "Popeye for President");
    CHECKSTR(gS.hits[3].title, "Big Bounce, The");
    CHECKSTR(gS.hits[8].identifier, "popeye-for-president-512kb");
    for (chunk = 1; chunk <= 64; chunk *= 2)
    {
        CHECK(search_buf(b, n, &gS2, chunk) == 9);
        CHECK(memcmp(gS.hits, gS2.hits, sizeof(gS.hits)) == 0);
    }
    free(b);

    b = load("search_edge.json", &n);
    r = search_buf(b, n, &gS, 3);
    CHECK(r == 5);
    CHECK(gS.numFound == 1234);
    CHECKSTR(gS.hits[0].identifier, "arr_title");
    CHECKSTR(gS.hits[0].title, "First Title");
    CHECKSTR(gS.hits[1].identifier, "uni_title");
    CHECKSTR(gS.hits[1].title, "Cafe \"Noir\" ? ? A/B \\ \"q\" z");
    CHECKSTR(gS.hits[2].identifier, "no_title");
    CHECKSTR(gS.hits[2].title, "no_title");
    CHECKSTR(gS.hits[3].title, "Nested OK");
    CHECKSTR(gS.hits[4].identifier, "empty_arr");
    CHECKSTR(gS.hits[4].title, "empty_arr");
    free(b);

    b = load("search_utf8.json", &n);
    CHECK(search_buf(b, n, &gS, 5) == 2);
    CHECKSTR(gS.hits[0].title, "Naive - Francais ? bad?byte");
    free(b);

    /* metadata JSON is valid JSON but not a search response */
    b = load("meta_popeye.json", &n);
    CHECK(search_buf(b, n, &gS, 100) == -1);
    free(b);
    b = load("garbage.html", &n);
    CHECK(search_buf(b, n, &gS, 100) == -1);
    free(b);

    /* raw JSON scanner edge cases */
    {
        static const char *const bad[] =
        {
            "", "{", "[1,]", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "[1 2]",
            "\"abc", "\"\\x\"", "\"\\u12G4\"", "tru", "truex", "nul",
            "[[[[[[[[[[[[[1]]]]]]]]]]]]]", "{}}", "[]]", "{\"a\":1}x",
            "\"a\nb\"", "-", "1a", "{\"a\":01e}", "[}", "{]", NULL
        };
        static const char *const good[] =
        {
            "{}", "[]", " [ 1 , -2.5e+3 , true , false , null , \"\" ] ",
            "{\"a\":{\"b\":[{\"c\":[]}]}}", "\"\\u0041\\/\"", "123",
            "[[[[[[[[[[[[1]]]]]]]]]]]]", NULL
        };
        McJson j;
        int i;
        for (i = 0; bad[i] != NULL; i++)
        {
            long len = (long)strlen(bad[i]);
            char *p = dup_exact(bad[i], len);
            int fr;
            mc_json_init(&j, NULL, NULL);
            fr = mc_json_feed(&j, p, len);
            if (!(fr != 0 || mc_json_finish(&j) != 0))
                printf("  bad JSON accepted: %s\n", bad[i]);
            CHECK(fr != 0 || mc_json_finish(&j) != 0);
            free(p);
        }
        for (i = 0; good[i] != NULL; i++)
        {
            long len = (long)strlen(good[i]);
            char *p = dup_exact(good[i], len);
            mc_json_init(&j, NULL, NULL);
            if (!(mc_json_feed(&j, p, len) == 0 && mc_json_finish(&j) == 0))
                printf("  good JSON rejected: %s\n", good[i]);
            CHECK(mc_json_feed(&j, "", 0) == 0 && mc_json_finish(&j) == 0);
            free(p);
        }
    }
    /* very long string: truncated, not overflowed */
    {
        char *big = (char *)malloc(5000);
        long k = 0;
        int i;
        k += sprintf(big, "{\"response\":{\"numFound\":1,\"docs\":[{\"identifier\":\"");
        for (i = 0; i < 300; i++) big[k++] = 'i';
        k += sprintf(big + k, "\"},{\"identifier\":\"ok\",\"title\":\"");
        for (i = 0; i < 1000; i++) big[k++] = 't';
        k += sprintf(big + k, "\"}]}}");
        CHECK(search_buf(big, k, &gS, 13) == 1);
        CHECKSTR(gS.hits[0].identifier, "ok");
        CHECK((long)strlen(gS.hits[0].title) == MC_TITLEMAX - 1);
        free(big);
    }
    /* more than 25 docs: capped */
    {
        char *big = (char *)malloc(4000);
        long k = 0;
        int i;
        k += sprintf(big, "{\"response\":{\"numFound\":40,\"docs\":[");
        for (i = 0; i < 40; i++)
            k += sprintf(big + k, "%s{\"identifier\":\"id%d\"}", i ? "," : "", i);
        k += sprintf(big + k, "]}}");
        CHECK(search_buf(big, k, &gS, 64) == MC_MAXHITS);
        CHECKSTR(gS.hits[24].identifier, "id24");
        free(big);
    }
    prefixes("search_popeye.json", 0);
    prefixes("search_edge.json", 0);
    prefixes("search_utf8.json", 0);
}

/* ---------------- c + d ---------------- */
static void test_meta(void)
{
    long n;
    char *b;
    char urls[MC_MAXATTEMPT][MC_URLMAX];
    int k;

    b = load("meta_popeye.json", &n);
    CHECK(meta_buf(b, n, &gM, 1) == 0);
    CHECK(gM.ncand == 1);
    CHECKSTR(gM.cand[0].name, "Popeye_forPresident_512kb.mp4");
    CHECKSTR(gM.cand[0].format, "512Kb MPEG4");
    CHECK(gM.cand[0].size == 26989036UL);
    CHECK(gM.cand[0].width == 320 && gM.cand[0].height == 240);
    CHECK(gM.nsrv == 1);
    CHECKSTR(gM.server[0], "dn801208.us.archive.org");
    CHECKSTR(gM.dir, "/0/items/Popeye_forPresident");
    CHECKSTR(gM.title, "Popeye for President");
    k = mc_attempt_urls("Popeye_forPresident", &gM, &gM.cand[0], urls,
                        MC_MAXATTEMPT);
    CHECK(k == 2);
    CHECKSTR(urls[0], "http://archive.org/download/Popeye_forPresident/"
                      "Popeye_forPresident_512kb.mp4");
    CHECKSTR(urls[1], "http://dn801208.us.archive.org/0/items/"
                      "Popeye_forPresident/Popeye_forPresident_512kb.mp4");
    free(b);

    b = load("meta_vietnam.json", &n);
    CHECK(meta_buf(b, n, &gM, 4096) == 0);
    CHECK(gM.ncand == 2);
    CHECKSTR(gM.cand[0].format, "MPEG4");
    CHECKSTR(gM.cand[1].format, "h.264 IA");
    CHECK(gM.cand[1].rank == MC_RANK_H264IA);
    k = mc_attempt_urls("vietnam-travel-guide-hanoi-ha-long-bay-ho-chi-minh-1_202101",
                        &gM, &gM.cand[0], urls, MC_MAXATTEMPT);
    CHECK(k == 2);
    CHECKSTR(urls[0], "http://archive.org/download/"
        "vietnam-travel-guide-hanoi-ha-long-bay-ho-chi-minh-1_202101/"
        "Vietnam%20Travel%20Guide_%20Hanoi%2C%20Ha%20Long%20Bay%2C%20Ho%20Chi%20Minh1.mp4");
    free(b);

    b = load("meta_multi.json", &n);
    CHECK(meta_buf(b, n, &gM, 9) == 0);
    CHECK(gM.ncand == MC_MAXCAND);
    CHECK(gM.droppedCand == 3);
    CHECKSTR(gM.cand[0].name, "My Film (1950) #1.mp4");
    CHECK(gM.cand[0].size == 0xFFFFFFFFUL);
    CHECKSTR(gM.cand[1].name, "e_512kb.mp4");
    CHECK(gM.cand[1].size == 400UL);
    CHECKSTR(gM.cand[2].name, "c.mp4");
    CHECKSTR(gM.cand[7].name, "l.mp4");
    CHECKSTR(gM.title, "Array Title");
    CHECK(gM.nsrv == 3);
    k = mc_attempt_urls("multi_item", &gM, &gM.cand[0], urls, MC_MAXATTEMPT);
    CHECK(k == 4);
    CHECKSTR(urls[0], "http://archive.org/download/multi_item/"
                      "My%20Film%20%281950%29%20%231.mp4");
    CHECKSTR(urls[1], "http://ia800101.us.archive.org/12/items/multi_item/"
                      "My%20Film%20%281950%29%20%231.mp4");
    CHECK(strncmp(urls[2], "http://ia900101.us.", 19) == 0);
    CHECK(strncmp(urls[3], "http://ia600101.ca.", 19) == 0);
    k = mc_attempt_urls("multi_item", &gM, &gM.cand[0], urls, 2);
    CHECK(k == 2);
    free(b);

    b = load("meta_dark.json", &n);
    CHECK(meta_buf(b, n, &gM, 8) == -3);
    free(b);
    b = load("meta_empty.json", &n);
    CHECK(meta_buf(b, n, &gM, 8) == -2);
    free(b);
    b = load("garbage.html", &n);
    CHECK(meta_buf(b, n, &gM, 8) == -1);
    free(b);
    CHECK(meta_buf("[1,2]", 5, &gM, 8) == -1);

    prefixes("meta_popeye.json", 1);
    prefixes("meta_vietnam.json", 1);
    prefixes("meta_multi.json", 1);
}

/* ---------------- e: synthetic moov ---------------- */
static unsigned char gMoov[4096];
static long gAt;
static long gStack[16];
static int gSp;

static void put8(int v) { gMoov[gAt++] = (unsigned char)v; }
static void put16(int v) { put8(v >> 8); put8(v); }
static void put32(unsigned long v)
{ put8((int)(v >> 24)); put8((int)(v >> 16)); put8((int)(v >> 8)); put8((int)v); }
static void zeros(int n) { while (n-- > 0) put8(0); }
static void open_box(const char *t)
{ gStack[gSp++] = gAt; put32(0); put8(t[0]); put8(t[1]); put8(t[2]); put8(t[3]); }
static void open_box64(const char *t)
{ gStack[gSp++] = -(gAt + 1); put32(1); put8(t[0]); put8(t[1]); put8(t[2]); put8(t[3]);
  put32(0); put32(0); }
static void close_box(void)
{
    long s = gStack[--gSp];
    unsigned long size;
    if (s < 0)
    {
        s = -s - 1;
        size = (unsigned long)(gAt - s);
        gMoov[s + 12] = (unsigned char)(size >> 24);
        gMoov[s + 13] = (unsigned char)(size >> 16);
        gMoov[s + 14] = (unsigned char)(size >> 8);
        gMoov[s + 15] = (unsigned char)size;
        return;
    }
    size = (unsigned long)(gAt - s);
    gMoov[s] = (unsigned char)(size >> 24);
    gMoov[s + 1] = (unsigned char)(size >> 16);
    gMoov[s + 2] = (unsigned char)(size >> 8);
    gMoov[s + 3] = (unsigned char)size;
}

/* codec "avc1"/"mp4v"; profile < 0 = no avcC */
static long build_moov(const char *codec, int profile, int compat, int level,
                       int w, int h, int audioFirst, int big)
{
    gAt = 0;
    gSp = 0;
    open_box("moov");
    open_box("mvhd"); zeros(100); close_box();
    if (audioFirst)
    {
        open_box("trak");
        open_box("mdia"); open_box("minf"); open_box("stbl");
        open_box("stsd"); put32(0); put32(1);
        open_box("mp4a"); zeros(28); open_box("esds"); zeros(30); close_box();
        close_box(); close_box();
        open_box("stts"); zeros(8); close_box();
        close_box(); close_box(); close_box();
        close_box();
    }
    if (big) open_box64("trak"); else open_box("trak");
    open_box("tkhd"); zeros(84); close_box();
    open_box("mdia");
    open_box("hdlr"); zeros(25); close_box();
    open_box("minf");
    open_box("vmhd"); zeros(12); close_box();
    open_box("stbl");
    open_box("stsd"); put32(0); put32(1);
    open_box(codec);
    zeros(6); put16(1); zeros(16); put16(w); put16(h);
    put32(0x00480000UL); put32(0x00480000UL); put32(0); put16(1);
    zeros(32); put16(24); put16(0xFFFF);
    if (profile >= 0)
    {
        open_box("avcC");
        put8(1); put8(profile); put8(compat); put8(level); put8(0xFF);
        put8(0xE1); put16(4); put8(0x67); put8(profile); put8(compat); put8(level);
        put8(1); put16(4); put8(0x68); put8(0xCE); put8(0x38); put8(0x80);
        close_box();
    }
    open_box("pasp"); put32(1); put32(1); close_box();
    close_box();          /* codec */
    close_box();          /* stsd */
    open_box("stts"); zeros(8); close_box();
    close_box(); close_box(); close_box();
    close_box();          /* trak */
    open_box("udta"); zeros(10); close_box();
    close_box();          /* moov */
    return gAt;
}

static unsigned long gSeed = 12345;
static unsigned long rnd(void)
{
    gSeed = gSeed * 1103515245UL + 12345UL;
    return (gSeed >> 16) & 0x7FFF;
}

static void test_moov(void)
{
    McAvcInfo info;
    char msg[200];
    long n, i;
    int r, bad;

    n = build_moov("avc1", 66, 0xC0, 30, 320, 240, 1, 0);
    r = mc_avc_profile(gMoov, n, &info);
    CHECK(r == 0);
    CHECKSTR(info.codec, "avc1");
    CHECK(info.profile == 66 && info.compat == 0xC0 && info.level == 30);
    CHECK(info.width == 320 && info.height == 240);
    CHECK(mc_avc_verdict(&info, r, msg, sizeof(msg)) == MC_PLAY_OK);
    CHECKSTR(msg, "Baseline H.264, level 3.0, 320x240.");

    n = build_moov("avc1", 100, 0, 31, 427, 240, 0, 0);
    r = mc_avc_profile(gMoov, n, &info);
    CHECK(r == 0 && info.profile == 100 && info.width == 427);
    CHECK(mc_avc_verdict(&info, r, msg, sizeof(msg)) == MC_PLAY_NOT_BASE);
    CHECKSTR(msg, "This video is High profile (100); this decoder plays "
                  "Baseline only.");

    /* Main with constraint_set1 (and even set0): still refused */
    n = build_moov("avc1", 77, 0xC0, 30, 320, 240, 0, 0);
    r = mc_avc_profile(gMoov, n, &info);
    CHECK(mc_avc_verdict(&info, r, msg, sizeof(msg)) == MC_PLAY_NOT_BASE);

    n = build_moov("avc1", 66, 0, 40, 1920, 1080, 1, 1);   /* 64-bit trak */
    r = mc_avc_profile(gMoov, n, &info);
    CHECK(r == 0 && info.height == 1080);
    CHECK(mc_avc_verdict(&info, r, msg, sizeof(msg)) == MC_PLAY_TOO_BIG);

    n = build_moov("avc1", -1, 0, 0, 320, 240, 0, 0);
    r = mc_avc_profile(gMoov, n, &info);
    CHECK(r == 0 && info.profile == -1);
    CHECK(mc_avc_verdict(&info, r, msg, sizeof(msg)) == MC_PLAY_UNKNOWN);

    n = build_moov("mp4v", -1, 0, 0, 320, 240, 1, 0);
    r = mc_avc_profile(gMoov, n, &info);
    CHECK(r == 0);
    CHECK(mc_avc_verdict(&info, r, msg, sizeof(msg)) == MC_PLAY_NATIVE);

    n = build_moov("hev1", -1, 0, 0, 320, 240, 0, 0);
    r = mc_avc_profile(gMoov, n, &info);
    CHECK(mc_avc_verdict(&info, r, msg, sizeof(msg)) == MC_PLAY_UNKNOWN);

    /* audio only: build then cut the video trak by making moov end early */
    {
        unsigned char a[64];
        memcpy(a, "\0\0\0 moov\0\0\0\030trak\0\0\0\020mdia\0\0\0\010minf", 32);
        r = mc_avc_profile(a, 32, &info);
        CHECK(r == -1);
        CHECK(mc_avc_verdict(&info, r, msg, sizeof(msg)) == MC_PLAY_NO_VIDEO);
        memcpy(a, "\0\0\0 free", 8);
        CHECK(mc_avc_profile(a, 32, &info) == -2);
        CHECK(mc_avc_profile(a, 3, &info) == -2);
        CHECK(mc_avc_profile(NULL, 0, &info) == -2);
    }

    /* every truncation of a good moov: clean -2, never a read past end */
    n = build_moov("avc1", 66, 0xC0, 30, 320, 240, 1, 1);
    bad = 0;
    for (i = 0; i < n; i++)
    {
        unsigned char *p = (unsigned char *)dup_exact((char *)gMoov, i);
        if (mc_avc_profile(p, i, &info) != -2) bad++;
        free(p);
    }
    CHECK(bad == 0);
    /* random corruption: any answer is fine, crashing is not */
    for (i = 0; i < 20000; i++)
    {
        unsigned char *p = (unsigned char *)dup_exact((char *)gMoov, n);
        int k, flips = 1 + (int)(rnd() % 4);
        for (k = 0; k < flips; k++) p[rnd() % (unsigned long)n] = (unsigned char)rnd();
        r = mc_avc_profile(p, n, &info);
        (void)mc_avc_verdict(&info, r, msg, sizeof(msg));
        free(p);
    }
    CHECK(1);
}

/* random bytes and mutated fixtures through both JSON parsers */
static void test_fuzz(void)
{
    static const char *const names[] =
    { "search_popeye.json", "search_edge.json", "meta_popeye.json",
      "meta_multi.json", "meta_vietnam.json", NULL };
    int f, i;
    for (f = 0; names[f] != NULL; f++)
    {
        long n;
        char *b = load(names[f], &n);
        for (i = 0; i < 2000; i++)
        {
            char *p = dup_exact(b, n);
            int k, flips = 1 + (int)(rnd() % 6);
            for (k = 0; k < flips; k++) p[rnd() % (unsigned long)n] = (char)rnd();
            (void)search_buf(p, n, &gS2, 1 + (long)(rnd() % 50));
            (void)meta_buf(p, n, &gM2, 1 + (long)(rnd() % 50));
            free(p);
        }
        free(b);
    }
    for (i = 0; i < 3000; i++)
    {
        long n = (long)(rnd() % 300), k;
        char *p = (char *)malloc((size_t)(n + 1));
        for (k = 0; k < n; k++) p[k] = "{}[]\",:\\u0aZ \x80\xC3\xE2\xF0"[rnd() % 18];
        (void)search_buf(p, n, &gS2, 1 + (long)(rnd() % 9));
        (void)meta_buf(p, n, &gM2, 1 + (long)(rnd() % 9));
        free(p);
    }
    CHECK(1);
}

int main(int argc, char **argv)
{
    strcpy(gDir, argc > 1 ? argv[1] : "fixtures");
    test_urls();
    test_search();
    test_meta();
    test_moov();
    test_fuzz();
    printf("sizeof McSearch=%lu McMeta=%lu McJson=%lu\n",
           (unsigned long)sizeof(McSearch), (unsigned long)sizeof(McMeta),
           (unsigned long)sizeof(McJson));
    printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
