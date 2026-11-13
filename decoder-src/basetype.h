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


#ifndef BASETYPE_H_INCLUDED
#define BASETYPE_H_INCLUDED


#ifdef __arm
#define VOLATILE volatile
#else
#define VOLATILE
#endif

typedef unsigned char   u8;
typedef signed char     i8;
typedef unsigned short  u16;
typedef signed short    i16;
typedef unsigned int    u32;
typedef signed int      i32;

/* CodeWarrior 8's MSL predates C99 and has no <stdint.h> uintptr_t.
   h264bsd_dpb.c and h264bsd_slice_data.c both do pointer-alignment
   arithmetic through a uintptr_t cast (e.g. rounding a pointer up to a
   16-byte boundary). Pointers are 32-bit on our PowerPC/Mac OS 9 target,
   so unsigned long is the correct-width stand-in. Guarded in case a
   future build environment (or a different compiler pass) already has
   a real one. */
#ifndef _UINTPTR_T_DEFINED
#define _UINTPTR_T_DEFINED
typedef unsigned long uintptr_t;
#endif

#if defined(VC1SWDEC_16BIT) || defined(MP4ENC_ARM11)
typedef unsigned short  u16x;
typedef signed short    i16x;
#else
typedef unsigned int    u16x;
typedef signed int      i16x;
#endif


#ifndef NULL
#ifdef  __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif
#endif

#endif  /* BASETYPE_H_INCLUDED */
