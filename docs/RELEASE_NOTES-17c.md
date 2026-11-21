# H264 Decoder 17c: release notes (DRAFT)

Draft of 20 November 2026. Not yet published.

## What it is

The first binary release of the Main/High-profile decoder for QuickTime 6 on Mac OS 9.
Two files, both installed in System Folder:Extensions:

| Asset | Contents | Fork check |
|---|---|---|
| `H264 Decoder 17c.hqx` | Component 'thng', version 0x00010011 (65553), name "H264 Decoder 17c" | resource fork 31324 bytes, md5 `55cd64b8c540f290941241c52ab1e027`; data fork empty |
| `AVCDecLib.hqx` | Shared library 'shlb'/'AVCd', fragment "AVCDecLib", version 0x00080F02 (r2) | data fork 648524 bytes, md5 `aef43a21a7e6e0851298db90c195e8ea` |
| `AVCDecLib LGPL source/` | `b21a-avcdeclib-r2-src.tgz` (md5 `1b365480796973b237f5a609fe689f49`), `COPYING.LGPLv2.1`, rebuild and replacement notes | |
| `Licences/`, `READ ME FIRST.txt`, `LICENCE AUDIT.md`, `SHA256SUMS.txt` | | |

## What plays

- Baseline, Main and High profile, 8-bit 4:2:0, progressive: CABAC, B-frames, 8x8
  transform. QuickTime 6.0.3 on Mac OS 9.2.2.
- Refused with a black picture and a log line: 10-bit, 4:2:2, 4:4:4, and anything when
  AVCDecLib is missing or older than 0x00080F01.
- **640x480 needs a G4 with AltiVec.** Mac mini G4 1.25 GHz (decoder 17, 16 Nov):
  High 640x480 with B-frames 15.6 ms/frame, without B-frames 23.9 ms (32-bit RGB),
  High 320x240 with B-frames 5.1 ms. Key frames at 640x480 take 32-35 ms.
- **G3: untested.** A PowerBook G3 (Pismo) test is pending.
- Standard `.mp4` files still need mp4fixer (v14/v15 kits) or H264 Stream.

## B-frames (by design)

QuickTime 6 cannot reorder pictures, so the component runs a fixed delay of N pictures
(from the stream's SPS, or from the `rdly` atom of a rewritten file):

- plain B-frame files lose their last N pictures;
- files with negative `ctts` offsets play N frames late against the sound.

Media Center (a later release) rewrites files with `AVCReorder.c` so that neither
happens.

## Changes since 17b (test build of 16 November)

- Log banner `=== H264 Decoder v17c: playback statistics ===`.
- New log lines `STATS avg/max us in blit (after first 8)`: the drawing time alone.
- `H264Codec.h`: the version is chosen with `#ifdef H264_BACKEND_AVCDEC`, because Rez
  (which reads the same header) rejects an undefined macro in `#if`. The first guest
  build failed on this; fixed in the source used for the release.
- `kOfferRGB32` compile-time switch (default 1: unchanged behaviour).
- Three variables used only by h264bsd moved under `#if !H264_BACKEND_AVCDEC`.
- `tools/kit/make_onefile.py`: `H264_THNG_VERSION` sets the 'thng' version.
- Decoding is bit-identical to 17b (host harness, x86 and qemu-ppc).

## How it was tested

- Host: the whole of `H264Codec.c` against a Toolbox stand-in, every painted picture
  compared with FFmpeg: 17/17 plain, 17/17 A_, 19/19 rewritten (C_) scenarios;
  run_all base 320x240 and high 640x480, 60/60 each.
- QEMU Mac OS 9.2.2 guest (CodeWarrior Pro 8.3 build of this source): registers as
  65553 only; m05 (320x240, B-frames, sound): 301 checksums matched, 0 mismatches,
  300/300 pictures shown; m06 (640x480, B-frames): 300 matched, 0 mismatches, 299/300
  shown (QuickTime did not send the last tail sample).
- Mac mini G4: decoder 17/17b and AVCDecLib r2 (the same library file as this release).
  17c on the Mini: pending.

## Known issues

- B-frame behaviour above (by design until Media Center).
- 640x480 key frames leave no margin at 30 fps on a 1.25 GHz G4.
- FFmpeg 0.8's IDCT can read past a table on corrupt streams (AddressSanitizer; no
  crash seen). FFmpeg also reads past the end of unpadded samples.
- In this build `H264Codec.c` still references `h264bsdShutdown`/`h264bsdFree` in
  Close, so a small part of h264bsd (Apache-2.0) is linked into the component. Planned:
  put that block under `#if !H264_BACKEND_AVCDEC` and drop `decoder-src` from the 17-series
  project.
- An old 16-series test component must never be used with AVCDecLib r2 (the picture
  structure grew by one field).

## Licences

Component: MIT, plus h264bsd (Apache-2.0, AOSP; MIT, Sam Leitch). AVCDecLib: LGPL 2.1+
(FFmpeg 0.8.15, no GPL parts), separate library loaded by name, complete source in the
release; contains IJG code (this software is based in part on the work of the Independent
JPEG Group) and five libgcc helpers under the GCC Runtime Library Exception.
