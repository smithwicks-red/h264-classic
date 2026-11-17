/* AVCDecLib shims: host allocator, av_log replacement (replaces libavutil/log.c),
   PPC cpu-flag hook (replaces libavutil/ppc/cpu.c). */
#include <stdarg.h>
#include <string.h>
#include "avd_shim.h"
#include "libavutil/log.h"
#include "libavutil/cpu.h"

AVCDecCallbacks avd_cb;

unsigned long avd_get_sp(void)
{
    unsigned long sp;
    __asm__ volatile ("mr %0,1" : "=r" (sp));
    return sp;
}

/* 16-byte header in front of every block keeps the user pointer 16-aligned and remembers
   the size for realloc. mem.c is compiled with -DMALLOC_PREFIX=avd_ so av_malloc/av_free/
   av_realloc land here. */
#define HDR 16
void *avd_malloc(size_t size)
{
    unsigned char *p;
    if (!avd_cb.alloc) return NULL;
    p = avd_cb.alloc(avd_cb.opaque, (unsigned long)size + HDR);
    if (!p) return NULL;
    *(size_t *)p = size;
    return p + HDR;
}
void avd_free(void *ptr)
{
    if (ptr) avd_cb.release(avd_cb.opaque, (unsigned char *)ptr - HDR);
}
void *avd_realloc(void *ptr, size_t size)
{
    void *n;
    size_t old;
    if (!ptr) return avd_malloc(size);
    old = *(size_t *)((unsigned char *)ptr - HDR);
    n = avd_malloc(size);
    if (!n) return NULL;
    memcpy(n, ptr, old < size ? old : size);
    avd_free(ptr);
    return n;
}

/* ---- libavutil/log.c replacement: no stdio ---- */
static int av_log_level = AV_LOG_ERROR;
static int log_flags;
static void (*av_log_callback)(void*, int, const char*, va_list) = av_log_default_callback;

const char *av_default_item_name(void *ptr)
{
    return (*(AVClass **) ptr)->class_name;
}
void av_log_default_callback(void *ptr, int level, const char *fmt, va_list vl)
{
    (void)ptr;
    if (level > av_log_level || !avd_cb.log) return;
    avd_cb.log(avd_cb.opaque, level, fmt, vl);
}
void av_log(void *avcl, int level, const char *fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
    av_vlog(avcl, level, fmt, vl);
    va_end(vl);
}
void av_vlog(void *avcl, int level, const char *fmt, va_list vl)
{
    av_log_callback(avcl, level, fmt, vl);
}
int  av_log_get_level(void) { return av_log_level; }
void av_log_set_level(int level) { av_log_level = level; }
void av_log_set_flags(int arg) { log_flags = arg; }
void av_log_set_callback(void (*callback)(void*, int, const char*, va_list)) { av_log_callback = callback; }

/* ---- libavutil/ppc/cpu.c replacement: flags are forced by avcdec_init() ---- */
int ff_get_cpu_flags_ppc(void) { return 0; }
