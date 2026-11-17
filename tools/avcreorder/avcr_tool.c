/* Host driver: apply AVCR_Rewrite to a file, writing the new moov in place
   (bytes before moov kept, bytes after moov moved by growth). Prints JSON. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "AVCReorder.h"

static long gAllocs, gFrees;
static void *A(unsigned long n, void *r) { (void)r; gAllocs++; return malloc((size_t)n); }
static void F(void *p, void *r) { (void)r; gFrees++; free(p); }
static unsigned long R32(const unsigned char *p)
{ return ((unsigned long)p[0]<<24)|((unsigned long)p[1]<<16)|((unsigned long)p[2]<<8)|p[3]; }

int main(int argc, char **argv)
{
    FILE *f; unsigned char *buf; long len, pos = 0, mo = -1; unsigned long ms = 0;
    AVCR_Input in; AVCR_Output out; int r;
    if (argc < 3) { fprintf(stderr, "usage: avcr_tool in out\n"); return 2; }
    f = fopen(argv[1], "rb"); if (!f) return 2;
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)len);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) return 2;
    fclose(f);
    while (pos + 8 <= len)
    {
        unsigned long s = R32(buf + pos);
        if (s == 1) s = R32(buf + pos + 12);       /* host test files < 4 GB */
        else if (s == 0) s = (unsigned long)(len - pos);
        if (s < 8) break;
        if (!memcmp(buf + pos + 4, "moov", 4)) { mo = pos; ms = s; }
        pos += (long)s;
    }
    if (mo < 0) { printf("{\"rc\": -100}\n"); return 1; }
    in.moov = buf + mo; in.moovSize = ms; in.moovFileOffset = (unsigned long)mo;
    in.fileSize = (unsigned long)len; in.inPlace = 1; in.alloc = A; in.dispose = F; in.refCon = 0;
    r = AVCR_Rewrite(&in, &out);
    f = fopen(argv[2], "wb");
    if (r == AVCR_OK)
    {
        fwrite(buf, 1, (size_t)mo, f);
        fwrite(out.moov, 1, (size_t)out.moovSize, f);
        fwrite(buf + mo + (long)ms, 1, (size_t)(len - mo - (long)ms), f);
        F(out.moov, 0);
    }
    else fwrite(buf, 1, (size_t)len, f);
    fclose(f);
    printf("{\"rc\": %d, \"msg\": \"%s\", \"N\": %lu, \"growth\": %ld, \"moov_off\": %ld, \"old_moov\": %lu, \"new_moov\": %lu, \"allocs\": %ld, \"frees\": %ld}\n",
           r, AVCR_ErrorString(r), out.N, out.growth, mo, ms, out.moovSize, gAllocs, gFrees);
    free(buf);
    return gAllocs == gFrees ? 0 : 3;
}
