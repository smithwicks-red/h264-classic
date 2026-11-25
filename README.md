# H.264 for Mac OS 9

A native H.264 video decoder for QuickTime 6 on classic Mac OS. Apple only
ever shipped H.264 for Mac OS X; this lets a PowerPC Mac running Mac OS 9.2.2
play H.264 movies in QuickTime Player, with AAC sound handled by QuickTime
itself.

The decoder is [h264bsd](https://github.com/oneam/h264bsd), a small portable
C decoder, wrapped as a QuickTime image decompressor component and built with
CodeWarrior Pro 8.3 under Mac OS 9. As far as we know nothing else like it
exists for Mac OS 9.

## New: decoder 17d (Main/High profile and B-frames)

**Decoder 17d** is the first binary release that plays **Main and High profile** H.264,
with B-frames, CABAC and the 8x8 transform. It comes as two files that both go in
System Folder:Extensions:

| File | What it is | Licence |
|---|---|---|
| **H264 Decoder 17d** | The QuickTime 6 decompressor component, version 0x00010011 (65553). | MIT (no h264bsd code; built with h264bsd's `basetype.h` typedefs, notice kept) |
| **AVCDecLib** | A separate CFM shared library ('shlb'): FFmpeg 0.8.15's H.264 decoder, cut down to H.264 only, with AltiVec code chosen at run time. Version 0x00080F02 (r2). | LGPL 2.1 or later |

- Plays Baseline, Main and High profile, 8-bit 4:2:0, in QuickTime 6 on Mac OS 9.2.2.
- **640x480 needs a G4 with AltiVec.** On a Mac mini G4 (1.25 GHz) decoder 17 played
  High 640x480 with B-frames at about 15.6 ms per frame, 320x240 at about 5 ms.
  A G3 is untested (a PowerBook G3 test is pending).
- **Memory:** QuickTime Player's standard partition is too small for 640x480 Main/High
  with B-frames; when it runs out it warns "running low on memory" and can freeze the
  whole Mac. Give QuickTime Player about 32 MB (Get Info > Memory: preferred 32768 K,
  minimum 16384 K; 24576 K on a Mac short of RAM).
- B-frames on QuickTime 6, by design: plain files lose their last N pictures (N = the
  stream's reorder delay, usually 1 or 2), and files with negative `ctts` offsets play
  N frames late. [Classic Media Center](https://github.com/smithwicks-red/classic-media-center)
  rewrites such files so that neither happens. Details: [`docs/DECODER-17.md`](docs/DECODER-17.md).
- New in 17d over 17c: the AVCDecLib build references no h264bsd code at all (every
  h264bsd call, member and include is behind `#if !H264_BACKEND_AVCDEC`), so the
  CodeWarrior project builds with all `decoder-src/*.c` removed; the log banner says
  `v17d`. Decoding is bit-identical to 17c.
- New in 17c over 17/17b: two new log lines, `STATS avg/max us in blit`, time the drawing
  apart from the decoding; `H264Codec.h` builds under Rez (`#ifdef H264_BACKEND_AVCDEC`);
  a compile-time switch `kOfferRGB32` (default on, as before).
- Release notes: [`docs/RELEASE_NOTES-17d.md`](docs/RELEASE_NOTES-17d.md)
  (17c: [`docs/RELEASE_NOTES-17c.md`](docs/RELEASE_NOTES-17c.md), never released as a binary).
- **Classic Media Center**, the Internet Archive browser that streams films (https
  included, through an in-app proxy) into QuickTime Player with this decoder, now lives in
  its own repository: [smithwicks-red/classic-media-center](https://github.com/smithwicks-red/classic-media-center).

**AVCDecLib's source.** `avcdeclib/` holds everything we wrote for the library (API,
shims, libc, scripts, configuration and our patches to FFmpeg). The release assets also
carry the complete corresponding source as one tarball (`b21a-avcdeclib-r2-src.tgz`:
the patched FFmpeg 0.8.15 tree, the five libgcc objects, MakePEF), with the LGPL text,
rebuild notes and how to swap in your own build. The FFmpeg tree itself (36 MB
unpacked) is not kept in this repository.

The rest of this page describes the released v15 kit.

## What is in this release

| Part | Version | Status |
|---|---|---|
| **H264 Decoder** (system extension, one file) | v15 | The main thing. Tested in an emulator and on a Mac mini G4. |
| **mp4fixer** (application) | from the v14 kit | Converts a standard `.mp4` into a `.mov` QuickTime 6 will open, with sound. |
| **H264 Stream** (application) | 1.2 | **Experimental.** Plays an `.mp4` straight from a plain `http://` address. |

Ready-to-use binaries are in the release assets as BinHex (`.hqx`) files
and as an HFS disk image (`H264 Kit.dsk`), with a `READ ME FIRST.txt` for
the Mac side. Decode the `.hqx` files on the Mac with StuffIt Expander.

## Requirements

- A **PowerPC** Mac (G3 or G4). Not 68k, not Carbon/Mac OS X.
- **Mac OS 9.2.2.** Earlier versions are untested.
- **QuickTime 6.0.3**, the last QuickTime for Mac OS 9. Earlier QuickTime is
  untested.
- StuffIt Expander, to decode the `.hqx` files.

## Limits

- **Baseline profile only.** h264bsd decodes the H.264 Baseline profile: no
  CABAC, no B-frames, no 8x8 transform. Main and High profile video (almost
  everything from modern phones, cameras and most of today's web) will not
  play. The decoder recognises such files and declines them. Baseline
  sources that do work include the Internet Archive's "512Kb MPEG4"
  derivatives, YouTube's old 360p MP4 (format 18) and many older phones.
- **Plain HTTP only** (H264 Stream). QuickTime 6 has no TLS, so it cannot
  fetch `https://` media. H264 Stream fetches only the movie's index over
  `http://` and lets QuickTime stream the rest; if a server redirects to
  https it stops and says "This item needs HTTPS".
- **Standard `.mp4` files need converting** with mp4fixer first.
  QuickTime 6's MPEG-4 importer predates H.264 and throws away the part of
  the file the decoder needs (error -8971). mp4fixer makes QuickTime use its
  movie parser instead, and fixes the AAC sound description.
- **Speed.** About 6-7 ms per frame at 320x240 on a Mac mini G4. A 500 MHz
  G3 has little headroom: v13 managed 29.4 of 29.97 fps in Thousands of colours on
  a PowerBook G3 (Pismo) and about 21.6 fps in Millions.

## What it was tested on

- **QEMU** Mac OS 9.2.2 emulator (UTM on an Apple Silicon Mac): v15 and
  H264 Stream 1.2, including bit-exact checks of every decoded picture
  against FFmpeg via the host harness in `tools/host/`.
- **Mac mini G4**, Mac OS 9.2.2, QuickTime 6.0.3: v15 decoder and H264
  Stream 1.1 streaming from archive.org.
- **PowerBook G3 (Pismo)**: decoder **v13** only. v15 has not yet been run
  on a G3. Reports welcome.

## Known issues

- **Seeking straight onto a key frame** (found just before release, with
  the new test clip): after a jump that lands exactly on a key frame, the
  following frames up to the next key frame can show small decoding errors.
  Normal playback from the start is unaffected. A fix is planned for v16.
- The decoder's speed on a G3 has not been measured since v13.

## How to build

Everything is built with **Metrowerks CodeWarrior Pro 8.3** under Mac OS 9
(classic PPC targets, not Carbon). The full click-by-click history,
including every dead end, is in [`docs/PORTING_NOTES.md`](docs/PORTING_NOTES.md)
and [`docs/CHANGES-v14.md`](docs/CHANGES-v14.md); each source file's header
comment also has its own build notes.

### Apple's QuickTime headers (not included)

The component needs Apple's QuickTime component headers, which are Apple's
copyright and are **not** in this repository. They come with
**Apple Universal Interfaces 3.4.x** (developed against 3.4.1), which
CodeWarrior Pro 8 installs in its `MacOS Support` folder. The files used are:

- `ImageCodec.h`, `ImageCodec.k.h`, `ImageCompression.h`
- `ComponentDispatchHelper.c`
- `Components.r`

If your CodeWarrior install lacks them, get Universal Interfaces 3.4.x or
the QuickTime 6 SDK for Mac OS from a classic Mac software archive.

### The decoder (`H264 Decoder`)

1. New **Empty Project**, PPC target. Project type **Shared Library**,
   file type `'thng'`, output file name `H264 Decoder` (not the same as the
   project's name). PPC Linker → Entry Points → **Main left empty**.
   Exports via `#pragma export` (already in `H264Codec.c`).
2. Add `codec-src/H264Codec.c`, `codec-src/avcC_parse.c`, all of
   `decoder-src/*.c`, plus `MSL_All_PPC.Lib`, `InterfaceLib` and
   `QuickTimeLib`. Access paths: `codec-src`, `decoder-src`.
3. Optimisation level 4 is what the release uses (checked bit-exact against
   level 0).
4. Make. Then, on any machine with Python 3, turn CodeWarrior's output into
   the one-file component:
   `python3 tools/kit/make_onefile.py "H264 Decoder" OUTDIR`
   (it adds the `'thng'` resource and points the code fragment at the
   dispatcher; see its header).

### Decoder 17d (`H264 Decoder 17d`, AVCDecLib backend)

Same project as above, with the backend switched to AVCDecLib:

1. Put `#define H264_BACKEND_AVCDEC 1` before anything else in `H264Codec.c` (the 17c/17d
   release builds did exactly this; a prefix file works too). Leave the macro
   **undefined** for the h264bsd build: `H264Codec.h` is also read by Rez, which only
   understands `#ifdef` there, so `-DH264_BACKEND_AVCDEC=0` would select the wrong
   version number. With the macro defined, `kH264CodecVersion` is 0x00010011 and the
   timing trace is on.
2. Remove `decoder-src/*.c` from the project (17d references no h264bsd function; 17c
   still needed them). Keep `decoder-src/` on the access path for `basetype.h`, and
   `AVCDecAPI.h` too.
3. Make, then wrap the output with the version in the `'thng'` resource:
   `H264_THNG_VERSION=0x00010011 python3 tools/kit/make_onefile.py "H264 Decoder" OUTDIR "H264 Decoder 17d"`
4. AVCDecLib is built separately with Retro68: see [`avcdeclib/README.md`](avcdeclib/README.md).

### mp4fixer

"C Console Classic" stationery (Mac OS PPC, not Carbon), remove
HelloWorld.c, add `codec-src/MP4Fix.c`; no prefix file. Details in the
file's header.

### H264 Stream 1.2

"C Console Classic" stationery (Mac OS PPC, not Carbon). Remove
HelloWorld.c and add `codec-src/H264Stream.c` and `codec-src/AVCProfile.c`
(the H.264 profile check 1.2 runs before it plays a file), plus `OpenTransportLib`,
`OpenTptInternetLib` (stub libraries), `OpenTransportAppPPC.o` and
`OpenTptInetPPC.o` (static glue). Access path: `codec-src`.
No prefix file; Require Prototypes off. `AVCProfile.c` and its tests also
build on any Unix: `sh tools/avcprofile/run_tests.sh`.

### Line endings

Sources are stored with Unix (LF) line endings. CodeWarrior 8 reads them,
but if your editor or transfer tool insists on classic Mac CR endings,
convert with `tr '\n' '\r'`.

### Host-side tools (`tools/`)

Python 3 and C tools that run on a modern machine:

- `tools/host/` builds the whole of `H264Codec.c` against a stand-in for
  the Toolbox and checks every decoded picture against FFmpeg
  (`run_all.sh MOVIE`).
- `tools/kit/` makes the `.hqx` files and the HFS disk image (`make_kit.py`
  needs `pip install machfs`).
- `tools/h264stream_hosttest.c` runs the real `H264Stream.c` against a fake
  web server; `tools/avcprofile/` tests its profile check.
- `tools/qtify.py` does on a modern machine what mp4fixer does on the Mac.
- `tools/make_test_clip.sh` regenerates the kit's test movie (needs
  FFmpeg with libx264).

`codec-src/` also holds a few unfinished experiments that are not part of
any build yet (`GetMovie.c`, `QTProxy.c`, `OTProbe.c`, `ostls_classic_shim.h`,
the `*Probe.c` diagnostics). They are kept because the porting notes refer
to them.

## Test movie

The kit's `H264 test movie.mov` is a 30-second synthetic test pattern with a
beeping tone, generated from scratch with FFmpeg (`tools/make_test_clip.sh`).
It contains nobody's footage and is dedicated to the public domain (CC0).

## Licences

- **The port's own code** (`codec-src/`, `tools/`, `docs/`):
  MIT, see [`LICENSE`](LICENSE).
- **AVCDecLib** (the shared library built from `avcdeclib/` and FFmpeg 0.8.15): LGPL
  2.1 or later, as FFmpeg 0.8.15 configured without its GPL parts. The files in
  `avcdeclib/` that we wrote are MIT. The library contains FFmpeg's `jrevdct.c`, so:
  this software is based in part on the work of the Independent JPEG Group. It also
  contains five libgcc helpers, covered by the GCC Runtime Library Exception. It is a
  separate file that the component loads by name, so users can replace it with their
  own build. See the 17d release's `LICENCE AUDIT.md`.
- **h264bsd** (`decoder-src/`): Apache License 2.0 for the code derived from
  the Android Open Source Project, and MIT for h264bsd's own wrapper code
  (Copyright (c) 2013-2022 Sam Leitch). See
  [`decoder-src/H264BSD-LICENSE.md`](decoder-src/H264BSD-LICENSE.md) and
  [`decoder-src/LICENSE-Apache-2.0.txt`](decoder-src/LICENSE-Apache-2.0.txt).
  Files changed for this port say so in their header and mark each change
  "Mac OS 9 port".
- QuickTime, Mac OS and CodeWarrior are trademarks of their owners. No
  Apple headers or libraries are included.

## Built with AI assistance

This project was built with substantial help from an AI assistant
(Anthropic's Claude), which wrote most of the code and documentation and
ran the builds and tests in the emulator, under the direction of a human
maintainer who tested it on real hardware. The porting notes keep the
whole record, mistakes included.

## Contributing and reports

Reports from real hardware are the most useful thing you can send: which
Mac, which colour depth, and the `H264 Decoder Log` file (see `READ ME
FIRST.txt` in the kit for how to make one).
