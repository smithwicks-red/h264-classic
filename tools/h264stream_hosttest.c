/*
    h264stream_hosttest.c -- run the REAL H264Stream.c on the host
    against a fake HTTP server, and write the reference movie it
    builds to a file, for byte comparison with mkref.py.

    Build:  gcc -std=c89 -Wall -o h264stream_hosttest h264stream_hosttest.c
    Usage:  ./h264stream_hosttest FILE.mp4 MODE URL OUT.mov
      MODE: plain     serve FILE at any path
            redirect  /r/... answers 302 -> http://media.example:8080/<rest>
            norange   like plain, but ignore Range (always 200)
            rel       302 with a root-relative Location
    The fake socket hands data back in random 1..3000 byte pieces.
*/
#define H264STREAM_HOSTTEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *MemAlloc(long n) { return malloc((size_t)n); }
static void  MemFree(void *p) { free(p); }

static unsigned char *gFile; static long gFileLen;
static const char *gMode; static const char *gOut;
static char gReq[4096]; static long gReqLen;
static unsigned char *gResp; static long gRespLen, gRespPos;
static int gOpen; static char gHost[256];
static int gRequests;

static int NetOpen(const char *host, unsigned short port)
{
    sprintf(gHost, "%s:%u", host, port);
    gOpen = 1; gReqLen = 0; gRespLen = gRespPos = 0;
    return 0;
}

static void MakeResponse(void)
{
    char path[2048], hdr[4096];
    long from = -1, n;
    char *r;
    gRequests++;
    sscanf(gReq, "GET %2047s", path);
    r = strstr(gReq, "Range: bytes=");
    if (r) from = atol(r + 13);
    fprintf(stderr, "[server %s] GET %s range=%ld\n", gHost, path, from);
    if (strncmp(path, "/r/", 3) == 0 && strcmp(gMode, "redirect") == 0)
        sprintf(hdr, "HTTP/1.1 302 Found\r\nLocation: http://media.example:8080/%s\r\nContent-Length: 0\r\n\r\n", path + 3);
    else if (strncmp(path, "/r/", 3) == 0 && strcmp(gMode, "rel") == 0)
        sprintf(hdr, "HTTP/1.0 301 Moved\r\nlocation: /x/%s\n\n", path + 3);
    else hdr[0] = 0;
    if (hdr[0]) { gResp = malloc(strlen(hdr) + 1); strcpy((char *)gResp, hdr); gRespLen = (long)strlen(hdr); return; }
    if (from >= 0 && strcmp(gMode, "norange") != 0)
        sprintf(hdr, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %ld-%ld/%ld\r\nContent-Length: %ld\r\n\r\n", from, gFileLen - 1, gFileLen, gFileLen - from);
    else
    {   from = 0;
        sprintf(hdr, "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nContent-Length: %ld\r\n\r\n", gFileLen); }
    n = (long)strlen(hdr);
    gResp = malloc((size_t)(n + gFileLen - from));
    memcpy(gResp, hdr, (size_t)n);
    memcpy(gResp + n, gFile + from, (size_t)(gFileLen - from));
    gRespLen = n + gFileLen - from;
}

static long NetSend(const char *buf, long n)
{
    memcpy(gReq + gReqLen, buf, (size_t)n); gReqLen += n; gReq[gReqLen] = 0;
    if (strstr(gReq, "\r\n\r\n")) MakeResponse();
    return n;
}
static long NetRecv(char *buf, long max)
{
    long k = 1 + rand() % 3000;
    if (!gOpen) return -1;
    if (gRespPos >= gRespLen) return 0;
    if (k > max) k = max;
    if (k > gRespLen - gRespPos) k = gRespLen - gRespPos;
    memcpy(buf, gResp + gRespPos, (size_t)k); gRespPos += k;
    return k;
}
static void NetClose(void)
{
    if (gOpen) fprintf(stderr, "[server] closed after %ld of %ld bytes\n", gRespPos, gRespLen);
    gOpen = 0;
}
static int SaveAndOpenMovie(const char *name, const unsigned char *d, long n)
{
    FILE *f = fopen(gOut, "wb");
    fwrite(d, 1, (size_t)n, f); fclose(f);
    printf("[host] would save as \"%s\" (%ld bytes)\n", name, n);
    return 0;
}

#include "H264Stream.c"

int main(int argc, char **argv)
{
    FILE *f; int rc;
    if (argc < 5) return 2;
    f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); gFileLen = ftell(f);
    rewind(f); gFile = malloc((size_t)gFileLen); fread(gFile, 1, (size_t)gFileLen, f); fclose(f);
    gMode = argv[2]; gOut = argv[4];
    srand(1234);
    rc = StreamURL(argv[3]);
    printf("[host] rc=%d requests=%d\n", rc, gRequests);
    return rc ? 1 : 0;
}
