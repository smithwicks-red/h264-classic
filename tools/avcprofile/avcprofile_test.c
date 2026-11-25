/*
    avcprofile_test.c -- host tests for codec-src/AVCProfile.c
    (the moov profile check H264 Stream runs before it plays a file).
    Moved unchanged from Media Center's mc_test.c (test_moov) when
    Media Center left this repository. Run: sh tools/avcprofile/run_tests.sh
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "AVCProfile.h"

static int gPass = 0, gFail = 0;

#define CHECK(cond) do { if (cond) gPass++; else { gFail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKSTR(a, b) do { if (strcmp((a), (b)) == 0) gPass++; else { \
    gFail++; printf("FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, \
    __LINE__, (a), (b)); } } while (0)

static char *dup_exact(const char *src, long n)
{
    char *p = (char *)malloc((size_t)(n > 0 ? n : 1));
    memcpy(p, src, (size_t)n);
    return p;
}

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

int main(void)
{
    test_moov();
    printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
