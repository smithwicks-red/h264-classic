/* AVCDecLib minimal libc for the FFmpeg h264 subset: no newlib, no stdio, no imports.
   Compiled with -fno-builtin -fno-tree-loop-distribute-patterns (so loops stay loops). */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "avd_shim.h"

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = d; const unsigned char *sp = s;
    if ((((uintptr_t)dp | (uintptr_t)sp) & 3) == 0) {
        uint32_t *dw = (uint32_t *)dp; const uint32_t *sw = (const uint32_t *)sp;
        while (n >= 16) { dw[0] = sw[0]; dw[1] = sw[1]; dw[2] = sw[2]; dw[3] = sw[3]; dw += 4; sw += 4; n -= 16; }
        while (n >= 4) { *dw++ = *sw++; n -= 4; }
        dp = (unsigned char *)dw; sp = (const unsigned char *)sw;
    }
    while (n--) *dp++ = *sp++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = d; const unsigned char *sp = s;
    if (dp == sp || n == 0) return d;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    dp += n; sp += n;
    while (n--) *--dp = *--sp;
    return d;
}
void *memset(void *d, int c, size_t n)
{
    unsigned char *dp = d; unsigned char b = (unsigned char)c;
    while (n && ((uintptr_t)dp & 3)) { *dp++ = b; n--; }
    if (n >= 4) {
        uint32_t w = b * 0x01010101U, *dw = (uint32_t *)dp;
        while (n >= 16) { dw[0] = w; dw[1] = w; dw[2] = w; dw[3] = w; dw += 4; n -= 16; }
        while (n >= 4) { *dw++ = w; n -= 4; }
        dp = (unsigned char *)dw;
    }
    while (n--) *dp++ = b;
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return p - s; }
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
char *strchr(const char *s, int c)
{
    for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; }
}
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

void abort(void)
{
    __builtin_trap();
}

/* shell sort; FFmpeg's only user (init_vlc_sparse) has unique keys, so order is defined */
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    unsigned char *b = base, tmp[64];
    size_t gap, i, j, k;
    if (sz > sizeof(tmp)) { __builtin_trap(); }
    for (gap = n / 2; gap > 0; gap /= 2)
        for (i = gap; i < n; i++) {
            for (k = 0; k < sz; k++) tmp[k] = b[i * sz + k];
            for (j = i; j >= gap && cmp(b + (j - gap) * sz, tmp) > 0; j -= gap)
                for (k = 0; k < sz; k++) b[j * sz + k] = b[(j - gap) * sz + k];
            for (k = 0; k < sz; k++) b[j * sz + k] = tmp[k];
        }
}

/* sscanf subset: literal characters, whitespace (matches any amount), %d. Enough for
   h264_sei.c "x264 - core %d" (x264 build detection, drives bug workarounds). */
static int is_sp(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap; int n = 0;
    va_start(ap, fmt);
    while (*fmt) {
        if (is_sp((unsigned char)*fmt)) { while (is_sp((unsigned char)*s)) s++; fmt++; continue; }
        if (fmt[0] == '%' && fmt[1] == 'd') {
            int neg = 0, v = 0, any = 0;
            while (is_sp((unsigned char)*s)) s++;
            if (*s == '-' || *s == '+') neg = *s++ == '-';
            while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); any = 1; }
            if (!any) break;
            *va_arg(ap, int *) = neg ? -v : v;
            n++; fmt += 2; continue;
        }
        if (*fmt != *s) break;
        fmt++; s++;
    }
    va_end(ap);
    return n;
}
#ifdef __linux__
int __isoc99_sscanf(const char *s, const char *fmt, ...) __attribute__((alias("sscanf")));
#endif

/* math: only what the kept code paths reference */
double log(double x)   /* used by av_d2q (option defaults) */
{
    union { double d; uint64_t u; } v; int e; double m, t, t2, sum, term; int k;
    if (!(x > 0)) return -1e300;
    v.d = x;
    e = (int)((v.u >> 52) & 0x7FF) - 1023;
    if (e == -1023) { v.d = x * 4503599627370496.0; e = (int)((v.u >> 52) & 0x7FF) - 1023 - 52; }
    v.u = (v.u & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
    m = v.d;                                   /* [1,2) */
    if (m > 1.4142135623730951) { m *= 0.5; e++; }
    t = (m - 1) / (m + 1); t2 = t * t; sum = 0; term = t;
    for (k = 1; k < 60; k += 2) { sum += term / k; term *= t2; }
    return 2 * sum + e * 0.6931471805599453;
}
long long llrint(double x)   /* round half to even, default rounding mode */
{
    long long i = (long long)x; double f = x - (double)i;
    if (f > 0.5 || (f == 0.5 && (i & 1))) i++;
    else if (f < -0.5 || (f == -0.5 && (i & 1))) i--;
    return i;
}
long lrintf(float x) { return (long)llrint(x); }

/* libavutil/eval.c replacement: string expressions in AVOptions are not supported. */
int av_expr_parse_and_eval(double *res, const char *s, const char * const *const_names,
                           const double *const_values, const char * const *func1_names,
                           double (* const *funcs1)(void *, double), const char * const *func2_names,
                           double (* const *funcs2)(void *, double, double), void *opaque,
                           int log_offset, void *log_ctx)
{
    (void)s; (void)const_names; (void)const_values; (void)func1_names; (void)funcs1;
    (void)func2_names; (void)funcs2; (void)opaque; (void)log_offset; (void)log_ctx;
    *res = 0;
    return -38; /* AVERROR(ENOSYS) on Linux; any negative value means failure to opt.c */
}

#ifndef __linux__
/* newlib ctype table (ASCII, C locale): newlib's ctype.h macros index _ctype_+1 */
const char _ctype_[257] = {
  0,32,32,32,32,32,32,32,32,32,40,40,40,40,40,32,
  32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
  32,136,16,16,16,16,16,16,16,16,16,16,16,16,16,16,
  16,4,4,4,4,4,4,4,4,4,4,16,16,16,16,16,
  16,16,65,65,65,65,65,65,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,16,16,16,16,
  16,16,66,66,66,66,66,66,2,2,2,2,2,2,2,2,
  2,2,2,2,2,2,2,2,2,2,2,2,16,16,16,16,
  32,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,
};
#endif
