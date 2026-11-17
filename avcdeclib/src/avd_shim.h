#ifndef AVD_SHIM_H
#define AVD_SHIM_H
#include <stddef.h>
#include "avcdec.h"
extern AVCDecCallbacks avd_cb;
unsigned long avd_get_sp(void);
void *avd_malloc(size_t size);
void *avd_realloc(void *ptr, size_t size);
void  avd_free(void *ptr);
#endif
