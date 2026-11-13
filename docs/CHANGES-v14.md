# H264 Decoder v14 -- what changed, how it was checked, how to build it

Written 11 Nov 2026 (Session 14). v14 is the "decoder changes" list from
the Session 13 handoff, done on the host. **It has not yet been compiled by
CodeWarrior or run on a Mac.** Everything below was verified on the host:
it runs the whole of `H264Codec.c` and the real h264bsd against the dv512
test clip, and compares the output with ffmpeg. It ran both as x86 code and
as 32-bit big-endian PowerPC code (the Mac's byte order and widths) under
`qemu-ppc`.

Five source files changed. Nothing else in the Mac build is affected
(`H264Startup.c`, `MP4Fix.c` and the `.r` files are untouched):

    codec-src/H264Codec.c          most of the work
    decoder-src/h264bsd_util.h     ALLOCATE/FREE go through hooks
    decoder-src/h264bsd_decoder.c  remaining malloc/free; deblocking switch
    decoder-src/h264bsd_decoder.h  h264bsdSetSkipDeblocking prototype
    decoder-src/h264bsd_storage.h  one new field, skipDeblocking

## The seven changes

### 1. No more malloc: the Memory Manager

Every allocation in h264bsd now goes through `H264MemAlloc` and
`H264MemFree` (`h264bsd_util.h` and the three direct calls in
`h264bsd_decoder.c`). `H264Codec.c` implements them:

1. `NewPtr` in the current heap (the playing application);
2. if that fails and the block is 16 KB or more, temporary memory
   (`TempNewHandle`, locked for life).

A 16-byte header in front of each block records which of the two it came
from, so freeing needs no lookup. The header keeps the 16-byte alignment,
and freeing NULL is allowed, as with `free()`. A block without one of the
two marks is never disposed. The Annex-B scratch buffer uses the same
hooks.

Why: MSL's malloc keeps its pool bookkeeping in the shared library's static
data, and that data is one copy shared by every application that has the
component open.

Under CodeWarrior the hooks are always on. Any **other** CodeWarrior project
that compiles h264bsd *without* `H264Codec.c` (the old Phase 0 console
decoder, say) will fail to link with `H264MemAlloc` undefined. Give that
project `#define H264BSD_STDLIB_ALLOC` and it gets malloc/free back.

Measured: 16 blocks per movie, 660 KB peak for the 360x240 clip, all freed
at Close. When NewPtr refuses everything over 16,000 bytes, 6 blocks move to
temporary memory and the output is still bit-exact. With both refusing,
PreDecompress returns `memFullErr` (-108) cleanly, with nothing leaked.

### 2. Statics into the per-instance globals

The 17 file-level statistics counters and the 9 function-level statics
(one-shot log flags, `sPicCount`, `sBandCalls`, `sLastFmt`, the logger's
FSSpec) are now fields of `H264Globals`. The only statics left are the
constant colour tables. Two movies playing at once now report separately,
and a one-shot flag set by one movie no longer silences another.

### 3. `h264bsdInit(storage, 1)`: no output reordering

Baseline has no B-frames. With 1, h264bsd keeps only the reference
pictures the stream uses, and outputs each picture on the call that
finishes it. **For the dv512 clip this changes nothing**: its SPS is
POC type 2, and h264bsd already switches reordering off by itself for type 2
(`h264bsd_storage.c`). It matters for POC type 0/1 files without
`num_reorder_frames`, which is most other encoders. Those were held back by
up to a full DPB of pictures and used a buffer sized for 16+1 pictures.

### 4. Decode errors are survived

A damaged sample no longer makes `BandDecompress` fail. Up to v13, a bad
sample returned `badFormat`, and any `H264BSD_ERROR` returned
`internalComponentErr`; either made QuickTime stop or show a blank frame.
Now:

- the bad NAL is skipped and the rest of the sample is still decoded;
- h264bsd conceals what is missing;
- a call that produces nothing paints the last good picture again;
- the call returns `noErr`.

Only `H264BSD_MEMALLOC_ERROR` is still reported, as `memFullErr`.

Tested: 2% to 40% of samples damaged (bit flips, truncation, random runs,
broken length prefixes), five seeds, under AddressSanitizer and
UndefinedBehaviorSanitizer. Results: no crash, no sanitizer report, no error
return, no leak, no byte written outside the destination rectangle.

### 5. 32-bit stores in the pixel writers, plus a 16-bit writer

- **32-bit RGB** (`CopyPlanarYUVToPixMap`): one 32-bit store per pixel
  instead of four byte stores. In Millions QuickTime hands us the screen
  itself, so that was four writes into video memory per pixel, and it is
  the prime suspect for Millions being 21.6 fps against 29.4. The edge tests
  moved out of the inner loop.
- **'2vuy'**: one 32-bit store per pixel pair instead of four byte stores.
- **New 16-bit RGB writer** (big-endian x555, two pixels per store). Up to
  v13 a 16-bit RGB destination drew nothing ("UNHANDLED"). QuickTime is
  only *offered* 16-bit when `kOfferRGB16` is 1, an experiment switch.

The bytes in memory are unchanged. Checked for all four destination
formats ('2vuy', RGB32, RGB16, 'y420'), at 360x240, 359x239 and 101x77,
against an independent numpy conversion, on x86 and big-endian PowerPC:
1,230 of 1,230 calls bit-exact every time. A 32-bit store compiles
differently on a little-endian host, so the PowerPC run is the one that
counts.

Speed on the G3 is **not** measured. Only the Pismo can say what this buys.

### 6. Lateness policy

Read the time the frame is due (`params->frameTime`) and the movie's clock
(`GetTimeBaseTime`). The policy acts only while the movie plays forward
(rate > 0) and QuickTime has not flagged the call as its own catch-up.
Mildest step first:

1. skip disposable frames (nal_ref_idc 0; this clip has none);
2. decode but do not paint, at most 2 pictures in a row, and only if the
   destination is the same buffer and rectangle as last time;
3. deblocking off, when late for 12 calls or by 250 ms **and** still
   losing ground for 3 calls; back on after 30 on-time calls;
4. drop everything until the next key frame, when 1 s late and losing
   ground for 8 calls. A key frame is never dropped.

Simulated on the host. In the model, QuickTime hands over every frame in
order, as soon as it is due and the previous call has returned; that is
what the Pismo logs fit. Lag at the end of the 41 s clip:

| decode + paint per frame | v13 | v14 | pictures |
|---|---|---|---|
| 22 + 3.4 ms (Pismo Thousands) | 0 | 0 | all bit-exact |
| 22 + 24 ms (Pismo Millions) | **15.6 s** | 71 ms | all bit-exact, every 2nd painted |
| 30 + 10 ms | 8.2 s | 87 ms | all bit-exact |
| 45 + 5 ms | 20.5 s | 67 ms | deblocking off: 41-51 dB PSNR |
| 70 + 5 ms | 51.2 s | 0.6 s | waits for key frames; frozen between them |
| 22 + 3.4 ms, jump 300 frames | -- | caught up | all bit-exact, no wait, filter left on |

Deblocking measured at 27% of h264bsd's time as PowerPC code (40% as x86).

What the model cannot know, and the VM experiment must answer:

- whether QuickTime 6 really passes a frame time with a positive rate on
  every playing call;
- what it does when we return early;
- whether skipping our '2vuy' write saves anything. QuickTime still copies
  its offscreen buffer to the screen. The whole saving is in Millions, when
  it gives us the screen itself.

### 7. One quiet build; logging only if the file already exists

`kH264Diagnostics` is gone. The decoder logs only if the log file **already
exists**, and it never creates it:

- `share:h264log.txt` in the VM (it exists on the share today, so the VM
  will log);
- `H264 Decoder Log` on the desktop on a real Mac. Make an empty SimpleText
  file with that name to get a report; throw it away to silence the decoder.

Each instance looks once, on its first call. Without the file, a playback
makes three file-system calls in total and never reads the clock.

The report at Close now also has `LATE ...` lines showing what the
lateness policy did, and `STATS decode errors survived`.

Development switches, all near the top of `H264Codec.c` (0 in a release
build, except the policy):

    kH264Verbose          the old step-by-step lines and the YUV dumps
    kH264LatenessPolicy   1
    kH264SimulateSlowMs   busy-wait N ms per call (the slow-CPU experiment)
    kH264TimingTrace      per call: frameNumber, due, now, rate,
                          conditionFlags, callerFlags, lateness, action
                          (first 900 calls, written at Close)
    kOfferRGB16           offer 16-bit RGB in the pixel-type list

## Also

- The host harness is now `tools/host/`. It compiles the **whole**
  `H264Codec.c` (`H264_HOST_BUILD` removes only the Mac includes and the
  dispatch glue), so it cannot drift from the real file.
  `tools/host/run_all.sh MOVIE.mp4` runs every check above in about
  3 minutes and prints ALL PASSED. `tools/decode_run` and `tools/ctx` are
  v13-only now; see `tools/decode_run/SUPERSEDED.txt`.
- `H264CodecTinyEntry` no longer logs (it has no instance) but is still
  exported, so the export list is unchanged. **The PEF offsets in the
  Session 13 doc (`...RD` at 0x3A0, dispatcher at 0x210) are for the v13
  kit build.** Read them again from the v14 build's export table before
  making the one-file layouts.

## Building it in the VM

1. On the share, at the top level: `H264Codec.c`, `h264bsd_util.h`,
   `h264bsd_decoder.c`, `h264bsd_decoder.h` and `h264bsd_storage.h` are
   v14. The copies they replaced were byte-identical to the v13 tarball,
   so nothing was lost.
2. Close any CodeWarrior editor windows on those files. In the guest
   Finder, drag the five onto `Desktop Folder:H264 Classic Project Folder`
   and accept "replace".
3. `H264Codec` project: **Remove Binaries, then Make.** The optimizer is
   still at level 4 from Session 13. For the first v14 build, consider
   level 0, so a problem is v14's and not the optimizer's.
4. Possible compile errors, and what to do:
   - `TempNewHandle` undeclared: add `#include <Memory.h>`. It is already
     included, so this should not happen.
   - `GetTimeBaseTime` / `TimeRecord` undeclared: `#include <Movies.h>`
     is added near the top.
   - `ICMFrameTimeRecord` field names: taken from the VM's own
     `ImageCompression.h`.
5. Test as before: quit the registrar, put the new decoder in Extensions,
   launch DevRegister, play, read `share:h264log.txt`. Expect the `LATE`
   lines at the end. `LATE calls with a frame time 0` would mean QuickTime
   gives us no usable clock, and the policy is inert.

## Session 14, part 2 (night of 11 Nov): built and run in the VM

v14 compiled in CodeWarrior with **no errors and no warnings** on the first
try, and plays in QuickTime Player 6.0.3 in the VM. Several things were
learned there and changed the code again; the tarball on the share has
the final version.

### Measured in the VM (QEMU on the M3; Millions; dv512 clip)

| build | pictures | avg us per call | fps |
|---|---|---|---|
| v13, level 0 (Session 12) | 917 of 1229 | 7379 | 29.55 |
| v14, level 0 | 1229 of 1229 | 3714 | 30.00 |
| v14, level 4 (release) | 1229 of 1229 | **2745** | 29.97 |

- **Level 4 is bit-exact.** With the trace switched on, every one of 900
  pictures had the same checksum as the host build, at level 0 and at
  level 4. The release build therefore uses level 4. (The project is left
  at level 4.)
- **Quiet build confirmed:** with no log file present, playback created no
  file on the share or the desktop.

### What QuickTime 6 actually does (trace builds)

- **It passes no `frameTime`** to this codec. That is reserved for codecs
  doing scheduled asynchronous decompression (`codecCanAsyncWhen`). It
  does pass `syncFrameTime` (each frame's start time, scale and duration,
  but no time base).
- So the lateness policy now uses a **self-made clock** when there is no
  `frameTime`: lateness = growth of (wall time - frame start time), reset
  to zero whenever QuickTime waited more than 15 ms after our previous
  return (we are ahead), on a jump backwards, or on a jump forward of more
  than a second. In the host model with only `syncFrameTime` it keeps the
  Millions-like case within about 120 ms (15.6 s without the policy).
- **QuickTime does its own catch-up.** With a decoder slowed to 49 ms per
  call it handed over 698 of 1229 frames and stayed in sync. It jumps
  ahead, goes back to the key frame before the target and decodes forward,
  and it flags every frame on the way with `codecConditionCatchUpDiff` (688
  of 698 calls). Those frames are decompressed to stay in step, not to be
  seen. v14 now **decodes them without painting**, at most two in a row
  and only into the buffer that already holds the previous picture (484 of
  737 in the second slow run). In Millions on the Pismo painting is about
  half of each call, so this should let QuickTime catch up much sooner.
- During catch-up QuickTime sometimes switches the destination between
  the 32-bit screen and an offscreen '2vuy' buffer.
- The September conclusion "QuickTime does not drop frames, a slow Mac
  just lags" was too strong. The Pismo was only slightly slow in
  Thousands, but in Millions it got 895 calls for 1229 frames, which
  matches what the slow VM runs show.

### Gestalt

`Gestalt('copl')` is undefined on this system (-5551), so the Component
Manager uses `gestaltSysArchitecture` = 2 (PowerPC) as the platform. For
the one-file extension, layout B1 (`'thng'` platform 2) matches; A1
(platform 5) probably does not. Component Manager `cpnt` = 8, QuickTime
`qtim` = 0x06038000 (6.0.3), system 9.2.2.

### PEF offsets of the v14 release build (level 4)

The release build on the share and in the kit (108,699 bytes data fork):
`H264CodecComponentDispatchRD` at **0x318** (data, class 1) and
`H264CodecComponentDispatch` at **0x184** (TVector, class 2), section 1.
Any other build: run `tools/host/pefinfo.py FILE`.

## Session 14, part 3 (morning of 11 Nov): ONE FILE, and unsupported files

### The one-file decoder works (layout B1)

The decoder is now a single file in Extensions. No `H264 Auto-Register`.

    'thng' 256  extended template, registration flags 9
                (componentHasMultiplePlatforms | componentDoAutoVersion),
                version 0x00010000, one platform entry:
                flags 0, code 'cdec' 256, platform 2 (PowerPC)
    'STR ' 256  "H.264 (Software)";  'STR ' 257  description
    'cdec' 256  CodeWarrior's PEF, unchanged except that the loader
                header's main symbol is set to
                H264CodecComponentDispatchRD (section 1, a RoutineDescriptor)
    empty data fork, type 'thng'

How it was proven, in order:

1. `OneFileProbe.c` (built in the Probe project in place of
   ComponentProbe.c; the v5 registrar source is on the share as
   `ComponentProbe v5.c`) opened the file and called
   `RegisterComponentResourceFile`. First result 0: nothing registered,
   because DevRegister had already registered the same
   type/subtype/manufacturer at the same version, and the 'thng' asks for
   auto-versioning. With DevRegister quit: 1 registered, OpenComponent OK,
   Version 0x00010000, CanDo GetCodecInfo / PreDecompress /
   BandDecompress all 1, clean close and unregister. The old 'dlle'
   layout crashed the caller at exactly this point.
2. Only the B1 file in Extensions (registrar and two-file decoder parked
   in `Macintosh HD:H264 Tests:held:`), shut down, cold start: QuickTime
   Player played the test movie, 1229 of 1229 pictures, 29.97 fps.
3. The same for the release one-file build (v14a), then for v14b (below).
   Shut Down works without the registrar's Quit handling.

`tools/kit/make_onefile.py PEF OUTDIR` makes the one-file component from
any CodeWarrior build: it finds the RoutineDescriptor in the export
table, patches main, and writes NAME + .rdump/.idump + .hqx. Release
procedure: build in CodeWarrior, copy `H264 Decoder` to the share, run
make_onefile.py on the host, then decode the .hqx in the guest with
StuffIt Expander (on Macintosh HD, never on the share).

The long-standing mystery from Sessions 5-7 is closed. `'dlle'` was the
wrong mechanism (Windows/Mac OS X); the classic resource layout with
platform 2 is the right one. `Gestalt('copl')` is undefined on 9.2.2, so
the platform compared is `gestaltSysArchitecture` = 2.

### Main and High profile files (v14b)

Before: such a file "played" as a field of green noise (QuickTime's
uninitialised buffer) with its sound. h264bsd refuses its PPS
(CABAC) or High-profile SPS, and FeedParameterSets ignored that.

Now FeedParameterSets records the refusal and the profile from the avcC.
PreDecompress returns codecUnimpErr (-8962) and, if the log exists,
writes `UNSUPPORTED H.264 stream, profile_idc 77` (or 100) and why. Measured in
the VM: QuickTime shows a blank light-grey picture area, with no error
dialog, and plays the sound. It retries PreDecompress about once per
frame, so the log line repeats. **Follow-up:** log it once per instance;
or accept the sequence and paint black. BandDecompress already paints
plain black if it is called for a refused stream (host-tested for all
four formats).

### State at the end of the session

- VM: shut down cleanly (Special > Shut Down via the Finder).
- `System Folder:Extensions:` holds the **one-file v14b `H264 Decoder`**
  and the old `H264 Register Log.txt`. No `H264 Auto-Register`.
- `Macintosh HD:H264 Tests:held:` holds the parked two-file v14
  (`H264 Auto-Register`, the two-file `H264 Decoder`), the B1 test file
  and the earlier one-file builds. `H264 Tests:` also has
  `prof_main.mov` and `prof_high.mov`.
- CodeWarrior: the H264Codec project is at Global Optimizer level 4. The
  Probe project now builds OneFileProbe.
- Share: `H264 OS9 Kit v14/` (+ .zip) is the ONE-FILE kit: `H264
  Decoder.hqx` (byte-identical to the BinHex installed in the VM), mp4fixer,
  test movie, disk image, READ ME with upgrade steps.
- On the share, `h264log.txt` exists (empty), so the VM logs.
- Throw-away items are in the share's `_to_delete/` (rm is not
  allowed from the host shell): trace logs, the mislabelled copies, the
  two-file kit's registrar.

### Two host-side hazards seen this session

- `device_commit_files` delivered a **stale** copy once (the previous
  `H264 Decoder 1file.hqx`), as Session 13 warned. Always md5 the share
  copy before the guest uses it; a new file name avoids it.
- Two `.mov` files written to the share grew by about 6 KB: something on
  the Mac inserted a `uuid` "manifest" box after `ftyp`. QuickTime still
  opened them. The kit's movie is unaffected (verified by SHA-256).

### Performance note

From about 9:30 AM guest time the whole VM slowed down a lot: QuickTime
called the decoder only 135 times in 41 s, at 35 ms per call, for code that
ran at 2.8 ms per call two hours earlier. Most likely the host was in
use, or macOS throttled UTM in the background. Timing numbers from that
period are not about the decoder.

