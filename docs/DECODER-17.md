# Decoder 17: Main and High profile, with B-frames (experimental)

Decoder 17 adds a second backend to the QuickTime component: the H.264 decoder of
FFmpeg 0.8.15, built as a separate Mac OS 9 shared library (**AVCDecLib**, LGPL) and
loaded at run time. h264bsd remains in the source as the default backend (that is v15).

Status on 16 November 2026: built with CodeWarrior Pro 8.3, run in a QEMU Mac OS 9.2.2
guest and on a **Mac mini G4** (QuickTime 6.0.3). Every decoded picture was compared with
FFmpeg's output by checksum and matched, including seeks.

| Movie on the Mac mini G4 (1.25 GHz, AltiVec) | avg ms per frame | max | fps |
|---|---|---|---|
| High 640x480, no B-frames, 32-bit RGB to the screen | 23.9 | 33.7 | 29.94 |
| High 320x240, B-frames | 5.1 | 14.7 | 30.01 |
| High 640x480, B-frames | 15.6 | 34.7 | 30.04 |

No ready-made binaries are published for 17 yet; this is source only.

## What plays
- Baseline, Main and High profile, 8-bit 4:2:0, progressive. CABAC, B-frames, 8x8 transform.
- Refused (black picture, no error dialog): 10-bit, 4:2:2/4:4:4, and B-frame movies when
  the library is missing or too old.
- Untested: interlaced video, a G3 (no AltiVec), anything above 640x480.

## How B-frames work on QuickTime 6
QuickTime 6 cannot reorder frames: it hands the decoder one sample at a time in file
(decode) order and shows whatever the decoder paints, at that sample's time. Decoder 17
therefore runs a fixed delay of N pictures: sample n shows picture n-N.

- **Plain files** (opened as they are): N comes from the stream's SPS. Pictures and timing
  are right (QuickTime 6 ignores `ctts` and honours the edit list the muxer wrote), but the
  last N pictures of the movie are never shown.
- **Rewritten files**: `codec-src/AVCReorder.c` rewrites only the video sample tables of the
  `moov`: `ctts` removed, N one-byte "tail" samples appended (each means "show the next held
  picture"), the edit list shifted, and a private `rdly` atom (4 bytes, N) added to the
  `avc1` sample description. Media data is untouched, so this also works for a reference
  movie that streams over http. With `rdly` the component enforces N itself.
- Seeks: a jump onto a key sample resets the decoder and shows that picture at once.
  Jumps are detected from `syncFrameTime` (QuickTime passes it in the movie time scale;
  a negative start arrives with a zero high word, which the comparison allows for).
- The library's pictures are only valid until the next library call; the component never
  reads one later than that.

## Files
- `codec-src/H264Codec.c`: both backends; build with `H264_BACKEND_AVCDEC 1`,
  `kH264CodecVersion 0x00010011`. `codec-src/AVCDecAPI.h`: the library's 14 entry points.
- `codec-src/AVCReorder.c/.h`: the rewriter (C89, no libc). Not yet wired into the
  published MP4Fix / H264 Stream sources.
- `avcdeclib/`: everything needed to rebuild AVCDecLib except FFmpeg itself (see its README).
- `tools/host/`: the host harness (x86 and qemu-ppc) that checks every painted picture;
  `tools/host/stage5/` holds the B-frame, seek and refusal scenarios.
- `tools/avcreorder/`: rewriter test driver, fuzzer, and the Python reference and
  QuickTime-6 model it was proven against.
- `retro68-makepef/`: fixes to Retro68's MakePEF that the library build needs.

## Known issues
- The statistics banner in the log still says "v15".
- FFmpeg 0.8's IDCT can read past a table on corrupt streams (found with AddressSanitizer;
  no crash seen).
- An old 16-series test component must never be used with this library (the picture
  structure grew by one field).
- Key frames at 640x480 take 32-35 ms on the G4: 30 fps material has no margin on those frames.
