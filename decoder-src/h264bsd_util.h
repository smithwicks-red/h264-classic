/*
 * Copyright (C) 2009 The Android Open Source Project
 * Modified for use by h264bsd standalone library
 * Modified in 2026 for the Mac OS 9 QuickTime port: see the comments
 * marked "Mac OS 9 port" (deblocking switch, allocator hooks, CW8 types).
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*------------------------------------------------------------------------------

    Table of contents

    1. Include headers
    2. Module defines
    3. Data types
    4. Function prototypes

------------------------------------------------------------------------------*/

#ifndef H264SWDEC_UTIL_H
#define H264SWDEC_UTIL_H

/*------------------------------------------------------------------------------
    1. Include headers
------------------------------------------------------------------------------*/

#include "basetype.h"
#include "h264bsd_cfg.h"
#include "h264bsd_stream.h"
#include "h264bsd_image.h"

#ifdef _ASSERT_USED
#include <assert.h>
#endif

#if defined(_RANGE_CHECK) || defined(_DEBUG_PRINT) || defined(_ERROR_PRINT)
#include <stdio.h>
#endif

/*------------------------------------------------------------------------------
    2. Module defines
------------------------------------------------------------------------------*/

#define HANTRO_OK   0
#define HANTRO_NOK  1

#define HANTRO_TRUE     (1)
#define HANTRO_FALSE    (0)

#ifndef NULL
#define NULL 0
#endif

#define MEMORY_ALLOCATION_ERROR 0xFFFF
#define PARAM_SET_ERROR 0xFFF0

/* value to be returned by GetBits if stream buffer is empty */
#define END_OF_STREAM 0xFFFFFFFFU

#define EMPTY_RESIDUAL_INDICATOR 0xFFFFFF

/* macro to mark a residual block empty, i.e. contain zero coefficients */
#define MARK_RESIDUAL_EMPTY(residual) ((residual)[0] = EMPTY_RESIDUAL_INDICATOR)
/* macro to check if residual block is empty */
#define IS_RESIDUAL_EMPTY(residual) ((residual)[0] == EMPTY_RESIDUAL_INDICATOR)

/* macro for assertion, used only if compiler flag _ASSERT_USED is defined */
#ifdef _ASSERT_USED
#define ASSERT(expr) assert(expr)
#else
#define ASSERT(expr)
#endif

/* macro for range checking an value, used only if compiler flag _RANGE_CHECK
 * is defined */
#ifdef _RANGE_CHECK
#define RANGE_CHECK(value, minBound, maxBound) \
{ \
    if ((value) < (minBound) || (value) > (maxBound)) \
        fprintf(stderr, "Warning: Value exceeds given limit(s)!\n"); \
}
#else
#define RANGE_CHECK(value, minBound, maxBound)
#endif

/* macro for range checking an array, used only if compiler flag _RANGE_CHECK
 * is defined */
#ifdef _RANGE_CHECK
#define RANGE_CHECK_ARRAY(array, minBound, maxBound, length) \
{ \
    i32 i; \
    for (i = 0; i < (length); i++) \
        if ((array)[i] < (minBound) || (array)[i] > (maxBound)) \
            fprintf(stderr,"Warning: Value [%d] exceeds given limit(s)!\n",i); \
}
#else
#define RANGE_CHECK_ARRAY(array, minBound, maxBound, length)
#endif

/* macro for debug printing, used only if compiler flag _DEBUG_PRINT is
 * defined */
#ifdef _DEBUG_PRINT
#define DEBUG(args) printf args
#else
#define DEBUG(args)
#endif

/* macro for error printing, used only if compiler flag _ERROR_PRINT is
 * defined */
#ifdef _ERROR_PRINT
#define EPRINT(msg) fprintf(stderr,"ERROR: %s\n",msg)
#else
#define EPRINT(msg)
#endif

/* macro to get smaller of two values */
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

/* macro to get greater of two values */
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

/* macro to get absolute value */
#define ABS(a) (((a) < 0) ? -(a) : (a))

/* macro to clip a value z, so that x <= z =< y */
#define CLIP3(x,y,z) (((z) < (x)) ? (x) : (((z) > (y)) ? (y) : (z)))

/* macro to clip a value z, so that 0 <= z =< 255 */
#define CLIP1(z) (((z) < 0) ? 0 : (((z) > 255) ? 255 : (z)))

/* ------------------------------------------------------------------
   Where the decoder's memory comes from (Mac OS 9 port, v14)
   ------------------------------------------------------------------
   Upstream h264bsd calls malloc/free directly. Inside a QuickTime
   component on Mac OS 9 that means MSL's pooled allocator, whose
   bookkeeping lives in this shared library's data section -- and that
   data is shared by every application that has the component open.

   So every allocation in the decoder now goes through two hooks:

       void *H264MemAlloc(unsigned long size);
       void  H264MemFree(void *ptr);          (NULL is allowed)

   Under CodeWarrior they are always used, and H264Codec.c implements
   them with the Memory Manager (NewPtr, falling back to temporary
   memory). On a host build they are used only when
   H264BSD_EXTERNAL_ALLOC is defined (the test harness does, so that it
   can count allocations and force failures); otherwise the decoder
   builds stand-alone on malloc/free exactly as upstream does.

   A CodeWarrior project that links h264bsd WITHOUT H264Codec.c (the old
   console test programs) can define H264BSD_STDLIB_ALLOC to get
   malloc/free back. */
#if (defined(__MWERKS__) && !defined(H264BSD_STDLIB_ALLOC)) || \
    defined(H264BSD_EXTERNAL_ALLOC)
void *H264MemAlloc(unsigned long size);
void  H264MemFree(void *ptr);
#define H264BSD_MALLOC(size)  H264MemAlloc((unsigned long)(size))
#define H264BSD_FREE(ptr)     H264MemFree((void *)(ptr))
#else
#define H264BSD_MALLOC(size)  malloc(size)
#define H264BSD_FREE(ptr)     free(ptr)
#endif

/* macro to allocate memory */
#define ALLOCATE(ptr, count, type) \
{ \
    (ptr) = H264BSD_MALLOC((count) * sizeof(type)); \
}

/* macro to free allocated memory */
#define FREE(ptr) \
{ \
    H264BSD_FREE((ptr)); (ptr) = NULL; \
}

#define ALIGN(ptr, bytePos) \
        (ptr + ( ((bytePos - (uintptr_t)ptr) & (bytePos - 1)) / sizeof(*ptr) ))

extern const u32 h264bsdQpC[52];

/*------------------------------------------------------------------------------
    3. Data types
------------------------------------------------------------------------------*/

/*------------------------------------------------------------------------------
    4. Function prototypes
------------------------------------------------------------------------------*/
#ifndef H264DEC_NEON
u32 h264bsdCountLeadingZeros(u32 value, u32 length);
#else
u32 h264bsdCountLeadingZeros(u32 value);
#endif
u32 h264bsdRbspTrailingBits(strmData_t *strmData);

u32 h264bsdMoreRbspData(strmData_t *strmData);

u32 h264bsdNextMbAddress(u32 *pSliceGroupMap, u32 picSizeInMbs, u32 currMbAddr);

void h264bsdSetCurrImageMbPointers(image_t *image, u32 mbNum);

i32 abs(i32 a);
i32 clip(i32 x, i32 y, i32 z);

#endif /* #ifdef H264SWDEC_UTIL_H */

