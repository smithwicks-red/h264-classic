# H.264 for Mac OS 9

A native H.264 video decoder for QuickTime 6 on classic Mac OS. Apple only
ever shipped H.264 for Mac OS X; this lets a PowerPC Mac running Mac OS 9.2.2
play H.264 movies in QuickTime Player, with AAC sound handled by QuickTime
itself.

The decoder is [h264bsd](https://github.com/oneam/h264bsd), a small portable
C decoder, wrapped as a QuickTime image decompressor component and built with
CodeWarrior Pro 8.3 under Mac OS 9. As far as we know nothing else like it
exists for Mac OS 9.

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

### mp4fixer

"C Console Classic" stationery (Mac OS PPC, not Carbon), remove
HelloWorld.c, add `codec-src/MP4Fix.c`; no prefix file. Details in the
file's header.

### H264 Stream 1.2

"C Console Classic" stationery (Mac OS PPC, not Carbon). Remove
HelloWorld.c and add `codec-src/H264Stream.c` and `mediacenter/mc_core.c`
(1.2 uses its H.264 profile check), plus `OpenTransportLib`,
`OpenTptInternetLib` (stub libraries), `OpenTransportAppPPC.o` and
`OpenTptInetPPC.o` (static glue). Access paths: `codec-src`, `mediacenter`.
No prefix file; Require Prototypes off. `mc_core.c` and its tests also
build on any Unix: `sh mediacenter/run_tests.sh`.

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

- **The port's own code** (`codec-src/`, `mediacenter/`, `tools/`, `docs/`):
  MIT, see [`LICENSE`](LICENSE).
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
