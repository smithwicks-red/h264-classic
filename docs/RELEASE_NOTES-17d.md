# H264 Decoder 17d: release notes

21 November 2026. The first binary release of the Main/High-profile decoder for
QuickTime 6 on Mac OS 9. (17c, its predecessor, was prepared but never released as a
binary: see [`RELEASE_NOTES-17c.md`](RELEASE_NOTES-17c.md).)

## What it is

Two files, both installed in System Folder:Extensions:

| Asset | Contents | Check |
|---|---|---|
| `H264 Decoder 17d.hqx` | Component 'thng', version 0x00010011 (65553), name "H264 Decoder 17d"; resource fork 30524 bytes, data fork empty | sha256 `906d5f5985d550bb0a3bb7d3d768342956526341f23f955bf15d017a93feec1d` |
| `AVCDecLib.hqx` | Shared library 'shlb'/'AVCd', fragment "AVCDecLib", version 0x00080F02 (r2), data fork 648524 bytes (unchanged since 17c) | sha256 `687f9d2605f31681cdc6f9e231a433fea4fc4804386cca02ac81f772f954c1d2` |
| `AVCDecLib LGPL source/` | `b22e-avcdeclib-r2-lgpl-src.tgz` (md5 `ac2c84489690e684ec1ddd55f17c66d2`; the complete source, without the compiled MakePEF tool), `COPYING.LGPLv2.1`, rebuild and replacement notes | |
| `Licences/`, `READ ME FIRST.txt`, `LICENCE AUDIT.md`, `SHA256SUMS.txt` | | |

## What plays

- Baseline, Main and High profile, 8-bit 4:2:0, progressive: CABAC, B-frames, 8x8
  transform. QuickTime 6.0.3 on Mac OS 9.2.2.
- Refused with a black picture and a log line: 10-bit, 4:2:2, 4:4:4, and anything when
  AVCDecLib is missing or older than 0x00080F01.
- **640x480 needs a G4 with AltiVec.** Mac mini G4 1.25 GHz: High 640x480 with B-frames
  about 15.6 ms/frame, without B-frames 23.9 ms (32-bit RGB), High 320x240 with B-frames
  about 5 ms. Key frames at 640x480 take 32-35 ms.
- **G3: untested.** A PowerBook G3 (Pismo) test is pending.
- Standard `.mp4` files still need mp4fixer (v14/v15 kits), H264 Stream, or
  [Classic Media Center](https://github.com/smithwicks-red/classic-media-center).

## Memory (important for 640x480)

QuickTime Player's standard memory partition is too small for 640x480 Main/High video
with B-frames. When it runs out it says it is "running low on memory" and can then
freeze the whole Mac (in testing: pressing Play after a pause). Before playing 640x480
films, quit QuickTime Player, select it, File > Get Info > Memory, and set Preferred
Size to about 32768 K (24576 K on a Mac short of RAM), Minimum about 16384 K. 320x240
films are fine with the standard partition.

## B-frames (by design)

QuickTime 6 cannot reorder pictures, so the component runs a fixed delay of N pictures
(from the stream's SPS, or from the `rdly` atom of a rewritten file):

- plain B-frame files lose their last N pictures;
- files with negative `ctts` offsets play N frames late against the sound.

Classic Media Center rewrites files with `AVCReorder.c` so that neither happens.

## Changes since 17c

- **No h264bsd code in the component.** Every h264bsd include, structure member and call
  in `H264Codec.c` (the last two were `h264bsdShutdown`/`h264bsdFree` in Close, dead code
  in this build) is behind `#if !H264_BACKEND_AVCDEC`. The AVCDecLib build includes only
  h264bsd's `basetype.h` (type names) and declares `H264MemAlloc`/`H264MemFree` itself.
- The CodeWarrior project builds with all 26 `decoder-src/h264bsd_*.c` removed:
  0 errors, 0 warnings; the code fragment shrank from 30,759 to 29,959 bytes. Imports:
  InterfaceLib (26) and QuickTimeLib (2) only.
- Log banner `=== H264 Decoder v17d: playback statistics ===`.
- Decoding is byte-identical to 17c (host harness: output pictures and destination
  buffers compared with `cmp` on two reference films).

## How it was tested

- Host: the whole of `H264Codec.c` against a Toolbox stand-in, every painted picture
  compared with FFmpeg: 17/17 plain, 17/17 A_, 19/19 rewritten (C_) scenarios, 2/2
  bf2pyr, 2/2 + 2/2 pixel formats; base 320x240 and high 640x480, 60/60 each.
  `nm -u` on the AVCDecLib build: 0 `h264bsd*` symbols (17c: 2).
- QEMU Mac OS 9.2.2 guest (CodeWarrior Pro 8.3 build of this source, registered as
  65553 only): m05 (320x240, B-frames, sound): 301 checksums matched, 0 mismatches,
  300/300 pictures; m06 (640x480, B-frames): 299 matched, 0 mismatches, 0 decode errors.
- Mac mini G4: 17c (same decoding code) and AVCDecLib r2 passed every kit movie, and
  Classic Media Center 0.5u streamed 640x480 archive.org films through it.

## Known issues

- B-frame behaviour above (by design; Classic Media Center rewrites such files).
- 640x480 key frames leave no margin at 30 fps on a 1.25 GHz G4.
- QuickTime Player needs more memory for 640x480 (above).
- FFmpeg 0.8's IDCT can read past a table on corrupt streams (AddressSanitizer; no crash
  seen). FFmpeg also reads past the end of unpadded samples.
- An old 16-series test component must never be used with AVCDecLib r2 (the picture
  structure grew by one field).

## Licences

Component: MIT. It contains no h264bsd code; because it is compiled with h264bsd's
`basetype.h`, h264bsd's notices are kept (Apache-2.0, Android Open Source Project; MIT,
Sam Leitch). AVCDecLib: LGPL 2.1+ (FFmpeg 0.8.15, no GPL parts), a separate library
loaded by name, complete source in the release; it contains IJG code (this software is
based in part on the work of the Independent JPEG Group) and five libgcc helpers under
the GCC Runtime Library Exception.
