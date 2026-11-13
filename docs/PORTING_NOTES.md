# H.264 for QuickTime on Mac OS 9.2.2 — porting notes

## Status: registered and visible to QuickTime; OpenComponent is the last blocker

**Latest (session 5):** a purpose-built diagnostic (`ComponentProbe.c`)
proved the component **was registered all along** — it appears in the
live component list as `'imdc'/'avc1'`, "H.264 (Software)", with our own
info string. The real failure is that `OpenComponent()` returns NULL: the
Component Manager finds the component but cannot prepare its code
fragment. Root cause identified from the PEF loader header — for a CFM
shared library the `main` symbol is the fragment's *initialization*
routine, and we had it pointing at the component dispatcher, so CFM ran
the dispatcher as an initializer, got a non-zero result, and refused to
prepare the fragment. Fix: clear PPC Linker → Entry Points → Main. See
"Session 5" at the bottom.

**Latest (session 4):** every structural ingredient has now been verified
*by dumping the built binary* rather than assumed — file type, `'thng'`
fields, `'dlle'` symbol names, `'cfrg'`, a valid PEF, and the entry point
genuinely present in the PEF export table. One real bug was found and
fixed along the way (CodeWarrior exports nothing by default), and the
platform-code ambiguity was eliminated by testing both readings at once.
QuickTime still reports *"the required compressor could not be found."*
Two fixes are staged but untested: a fully-populated `GetCodecInfo`, and
a `'thng'` reverted to the classic single-platform template. See
"Session 4" at the bottom — including the `.rdump` inspection technique,
which is the most useful thing this project has found so far.

**Latest (session 3):** the component is installed in the Extensions
folder, Mac OS 9 boots normally with it, and QuickTime opens a real
Baseline H.264 test file — but reports *"the required compressor could
not be found"*. So it builds and loads safely, yet never registers with
the Component Manager. The prime suspect is that the CFM entry point is
not **exported** from the code fragment (CodeWarrior exports nothing by
default), which would make the `'dlle'` symbol lookup fail silently.
See "Session 3" at the bottom for the full diagnosis and the ordered
plan to fix it.

## IT BUILDS. 🎉

**As of the end of session 2, `Project → Make` completes with zero errors
and produces `H264 Decoder` — a 112K file of type `'thng'`, which the Mac
OS 9 Finder shows as Kind "system extension". That is a real, linked
QuickTime component.**

All 60 h264bsd decoder sources compile clean, the codec glue compiles
clean, Rez builds the `'thng'`/`'dlle'`/`'STR '` resources clean, and the
PPC linker writes the CFM shared library. Project totals: 312K code, 51K
data.

That does *not* yet mean it decodes anything, or that QuickTime will even
load it — see "What's actually proven" below, which is deliberately
strict about the difference. But the entire "will this 25-year-old
toolchain accept a modern H.264 decoder at all" question is now answered
yes, and every remaining unknown is a runtime question rather than a
build question.

---

This file is meant to be read by future contributors (human or AI) before the
next work session — it's the map of what's real, what's a guess, and what
to do next.

## The plan in one paragraph

Take **h264bsd** (Nokia's Baseline/Main-profile H.264 decoder, later
folded into Android AOSP, Apache-2.0 licensed) essentially unmodified as
the decode engine, and write a thin QuickTime **Image Decompressor
("Base Codec")** component around it that (a) converts the length-
prefixed NAL framing QuickTime/MP4 'avc1' sample data uses into the
Annex-B framing h264bsd expects, and (b) copies h264bsd's planar YUV 4:2:0
output straight into the destination PixMap, letting QuickTime's existing
YUV blit path handle the screen. No new decoding math had to be written —
the actual H.264 bitstream/CABAC-or-CAVLC/intra-inter-prediction/deblocking
work is entirely h264bsd's, which is the only way this is tractable as a
retro side project rather than a PhD.

## Why h264bsd

Researched three candidates:
- **openh264** (Cisco): too big/modern (build system assumes a real OS,
  SIMD-first design), not a good CodeWarrior 8 fit.
- **minih264** (lieff): encoder-only as of this writing — no decoder
  exists in that repo despite the name similarity. Ruled out.
- **h264bsd** (Nokia → AOSP, mirrored at github.com/oneam/h264bsd):
  chosen. See vetting results below.

## Vetting results (the good news)

Cloned `oneam/h264bsd`, grepped the whole `src/` tree (~25.8k lines, 28
.c/.h pairs) for everything that usually kills a 2001-era-compiler port:

- **No stdint.h dependency in practice.** One dead `#include <stdint.h>`
  in `h264bsd_util.h` — nothing in the file actually uses a `uintN_t`
  type (it defines and uses its own `u8/i8/u16/i16/u32/i32` via
  `basetype.h` throughout). Deleted the include line; verified nothing
  broke by re-grepping for stdint symbols afterward.
- **No GCC extensions, no `__attribute__`, no inline asm, no `restrict`.**
- **No 64-bit types, no VLAs, no `float`/`double` anywhere** — H.264's
  integer transform means the whole decode path is integer math, which
  is exactly what we want on a G3/G4 without leaning on the FPU for
  video math.
- **ARM-NEON/OpenMAX-accelerated code paths exist but are dead code for
  us**: every `#include "omxVC.h"` / `"armVC.h"` is guarded by
  `#ifdef H264DEC_OMXDL`, which nothing defines by default. As long as
  our CodeWarrior project never defines that macro, the portable C
  fallback path is all that compiles.
- **Bitstream reader is byte-at-a-time and endian-safe by construction**
  (`h264bsdShowBits32` builds its 32-bit value with explicit
  `pStrm[0]<<24 | pStrm[1]<<16 | ...`, never a raw pointer-cast word
  load). This matters a lot: it means the decoder behaves identically on
  our big-endian PowerPC target as it does on the little-endian x86/ARM
  machines it's normally built for. The handful of other `(u32*)` pointer
  casts in the codebase (deblocking boundary-strength arrays, motion
  vector zero-checks, raw pixel block copies) are all same-endianness
  read-then-read-back tricks, not numeric reinterpretation, so they're
  safe too.
- Uses `<stdlib.h>` (malloc/free), `<memory.h>` (memcpy/memset/memmove),
  `<assert.h>` (compiled to nothing unless `_ASSERT_USED` is defined —
  we won't define it), `<stdio.h>` (only reachable from commented-out
  debug `printf` calls — dead code, no runtime dependency). All standard
  MSL (Metrowerks Standard Library) territory; CodeWarrior 8 should have
  every one of these except *possibly* `memory.h` under that exact
  filename (some MSL versions only ship `string.h`, which has the same
  functions). **If the build complains "file not found: memory.h"**, the
  fix is a one-line local shim: create `memory.h` next to the other
  sources containing just `#include <string.h>`, so it's picked up from
  the project's own search path before MSL's.

**Bottom line: the decoder core (`decoder-src/`) was copied in with
exactly one line removed.** That's a much better starting position than
a from-scratch port would have been.

## What's new code (and therefore the risky part)

`codec-src/` — written this session, never compiled:

- `avcC_parse.h` / `avcC_parse.c` — pure ANSI C, no Mac Toolbox calls.
  Parses the `avcC` AVCDecoderConfigurationRecord to pull out SPS/PPS and
  the NAL length-prefix width, and converts length-prefixed sample data
  to Annex-B. This part is self-contained enough that it could be sanity
  checked outside the VM (e.g. against a real `avcC` box's bytes) if
  useful before trusting it inside the component.
- `H264Codec.c` / `H264Codec.h` / `H264Codec.r` — the actual QuickTime
  component. Written against the *documented* Image Compression Manager
  API (the Base Codec pattern used by every third-party codec of that
  era: `IMAGECODEC_BASENAME()`/`IMAGECODEC_GLOBALS()` macros +
  `#include "ImageCodec.k.h"` + `#include "ComponentDispatchHelper.c"`),
  but **not yet checked against the actual header files**, because I
  don't have them open in this sandbox — they live in CodeWarrior's
  Universal Interfaces on the Mac OS 9 side. Expect the first compile
  pass to surface small mismatches: a renamed struct field, an off-by-
  one on `GetImageDescriptionExtension`'s index parameter, that sort of
  thing. The *shape* of the code (which callbacks exist, what each one is
  responsible for, the avcC→AnnexB→h264bsdDecode→copy-to-PixMap data
  flow) should hold up; only leaf-level API spelling should need fixing.

## Open dependency to verify first thing in CodeWarrior

The component needs `Components.h`, `ImageCodec.h`, `ImageCodec.k.h`,
`ComponentDispatchHelper.c`, `QuickTimeComponents.h`, and
`Components.r` (for the resource file) from Apple's QuickTime headers.
These normally ship as part of CodeWarrior's bundled "Universal
Interfaces" — QuickTime was core system software by OS 9, so a typical
CodeWarrior 8 install should already have them under its CIncludes/RIncludes
search paths. **First thing to check when we sit down in CodeWarrior:
create an empty project, add `H264Codec.c`, and see whether
`#include <ImageCodec.h>` even resolves.** If it doesn't, the fix is
locating/installing the QuickTime SDK portion of Universal Interfaces
(a known, well-documented process in the retro Mac dev community — not
a dead end, just an extra download).

## CodeWarrior project settings checklist (for when we build)

- Target: PowerPC (no 68k — h264bsd's integer-only design would run on
  68k in principle, but it'd be painfully slow; not a goal).
- Project type: **Code Fragment / Shared Library (PEF)**, not an
  application — components are loaded as shared libraries by the
  Component Manager.
- Link against: `InterfaceLib`, `QuickTimeLib` (or `QTMLClient.lib`
  depending on exactly which import stubs this CodeWarrior ships),
  MSL RuntimePPC library (for malloc/free/memcpy/etc — pulled in
  automatically once the project's language settings are ANSI C).
- Export the component's main entry point with the exact name used in
  the `'dlle'` resource (`H264CodecComponentDispatch` in the current
  draft — this name is arbitrary, it just has to match between the .r
  file and whatever CodeWarrior's linker settings export).
- Add every file in `decoder-src/` and `codec-src/` to the project; add
  `decoder-src/` to the header search path (the decoder's own files
  `#include "h264bsd_whatever.h"` with no path prefix).
- Do **not** define `H264DEC_OMXDL`, `_ASSERT_USED`, or `_RANGE_CHECK`
  for the first working build (keep the portable/fast path; assertions
  can be turned on temporarily for debugging a specific crash).

## Next session's concrete to-do list

1. Confirm QuickTime SDK headers are reachable from CodeWarrior 8 (see
   above) — this is the single biggest unknown.
2. Create the CodeWarrior project, add all files, attempt a build,
   fix compile errors as they come up (expected: mostly in `H264Codec.c`
   against the real `ImageCodec.h`).
3. Once it links: test registration only (does QuickTime's "Get Info" on
   a movie, or a component-listing utility, show "H.264 (Software)" as
   an installed codec?) before worrying about actual decode correctness.
4. Find or make a **small, Baseline-profile, low-resolution** H.264 test
   file (QCIF/CIF, a few seconds) — Baseline specifically, since that's
   the profile h264bsd targets most completely and it keeps macroblock
   counts low while we're debugging.
5. First real decode attempt; expect to debug the avcC/Annex-B
   conversion and the PixMap plane-copy math (rowBytes padding, crop
   rect) before picture quality is the thing to look at.

## Where everything lives right now

- Cloud sandbox: `~/h264-mac9-port/{decoder-src,codec-src}/` plus this
  file.
- Mirrored into the Mac OS 9 VM via the new VirtFS shared folder
  (`share` volume on the Mac OS 9 desktop ↔ `a host folder shared with the VM`
  on the host Mac) into `H264 Classic Project Folder` on the VM's
  desktop.
- **New in session 2**: a real CodeWarrior project now lives at
  `Macintosh HD:H264 Classic Project Folder:H264Codec` (the `.mcp` project
  file, plus its auto-created `H264Codec Data` folder). All 60 real source
  files plus 3 libraries are already members of the project — see below.

## Session 2 — first compile, inside the VM, with CodeWarrior 8

This session actually sat down in the UTM Mac OS 9.2.2 VM (a
different VM than his earlier work, per his instructions — CodeWarrior 8
freshly installed there) and drove the whole thing by mouse/keyboard
through Claude's computer-use bridge. Highlights, problems, and exact
fixes below so none of this has to be re-discovered.

### Getting the files into the VM: the VirtFS directory bug

The shared folder (`share` volume, backed by `a host folder shared with the VM`
on the host) is a **mac9p/VirtFS bridge**, and it has a real, repeatable
limitation: **subfolders inside the shared volume do not survive the
bridge as folders.** A directory created host-side (e.g. via `tar` or
`mkdir`) and a directory created guest-side (e.g. "Desktop Folder", made
by the Mac itself) both show up in the guest Finder as zero-byte
`Kind: document` items that cannot be opened, dragged into, or inspected
— confirmed via Get Info on multiple different folders across a VM
restart, so it's not a stale-cache artifact. One nuance found *this*
session: after a full VM restart (forcing a fresh VirtFS mount), a
top-level folder directly under the shared volume root does at least
render as `Kind: folder` with a working disclosure triangle in Finder's
list view — but folders **nested inside** that top-level folder are still
broken 0-byte documents. So the bridge can apparently represent one level
of "this is a folder" metadata after a remount, but not real nested
directory contents. Net effect is unchanged: **don't rely on subfolders
across this bridge at all.**

**Working recipe** (already executed successfully this session):
1. Flatten everything to one directory level, no subfolders, converting
   line endings to classic Mac CR (`tr '\n' '\r'`) — this was already
   done last session as `mac9-flat.tgz` (62 files).
2. Send the tarball in via `SendUserFile` + `device_commit_files` into
   the connected host folder, then `tar xzf ... --strip-components=1`
   **directly at the share volume's root** on the host side. Individual
   files transfer across the bridge fine; only directories are broken.
3. In the guest, restart the VM (Special menu → Restart) so the VirtFS
   mount refreshes and Finder's file list isn't stale. (A plain window
   refresh was not enough in practice — a full restart was what actually
   made the new files visible.)
4. Select-all in the `share` window, drag-copy into a **real** folder on
   Macintosh HD (`H264 Classic Project Folder` — an HFS folder, not
   across the bridge, so no directory bug applies once files land there).
5. Clean up the junk that rides along in a flat select-all copy
   (`mac9-export` — an old nested-folder test artifact, `mac9-export.tgz`,
   `mac9-flat.tgz`, `README.txt`, `TheVolumeSettingsFolder`): select each,
   Cmd+Delete (Move to Trash) one at a time. End state: exactly the 62
   flattened files, cleanly, in `H264 Classic Project Folder`.

### Setting up the CodeWarrior project

Used **File → New → Empty Project** (not one of the Apple stationery
templates — this matters, see below), named `H264Codec`, saved directly
inside `H264 Classic Project Folder` with "Create Folder" unchecked so it
sits alongside the sources rather than in its own subfolder.

**Project → Add Files**, select-all inside `H264 Classic Project Folder`
in the file picker, then click **Choose** (not "Open" — for a multi-file
selection in this dialog, "Open" appears greyed/inert; "Choose" is the
button that actually commits a multi-select). This picked up all 60 real
source files and correctly skipped the two documentation files
(`H264BSD-LICENSE.md`, `PORTING_NOTES.txt`) since Add Files only offers
files not already in the project and apparently filters by an internal
notion of "addable" — either way, the result was exactly right: 60 files,
no docs.

**Target Settings → PPC Target**: changed `Project` type from
`Application` to **`Shared Library`** (this is CodeWarrior 8's name for
what the porting-notes checklist above calls "Code Fragment/Shared
Library (PEF)" — same thing). Set **File Name** to `H264Codec` and, this
is the important bit, changed **Type** from the auto-filled `shlb` to
**`thng`** — classic QuickTime/Component Manager components are files of
type `'thng'` sitting in the Extensions folder; the Component Manager's
Extensions-folder scan at boot looks for that file type specifically.
Creator code left as the default `????` (doesn't matter for Component
Manager registration).

**Target Settings → Access Paths — the thing an Empty Project does NOT
give you for free.** Unlike an Apple stationery project, Empty Project's
System Paths list starts as just `{Compiler}` — none of the Universal
Interfaces/QuickTime header paths are pre-wired. Had to add them by hand.
Found the actual folder layout on this CodeWarrior 8 install (useful if
this ever needs redoing):

```
Applications (Mac OS 9):Metrowerks CodeWarrior 8.0:
  Metrowerks CodeWarrior:
    MacOS Support:
      Universal:
        Interfaces:
          CIncludes           <- Components.h, ImageCodec.h, etc.
          ComponentIncludes   <- ImageCodec.k.h, QuickTimeComponents.h, etc.
          RIncludes           <- Components.r, Types.r, etc.
          PInterfaces, AIncludes  (not needed — Pascal/asm)
        Libraries:
          PPCLibraries        <- OpenTransport stub objects only, NOT what we need
          StubLibraries       <- InterfaceLib, QuickTimeLib live here (see below)
          MacOS68kLibraries   <- not needed, 68k only
      Libraries:
        Runtime:
          Libs                <- MSL_All_PPC.Lib and friends live here
```

Added as **System Paths** (Target Settings → Access Paths → System Paths
→ Add…, navigating to the exact folder and clicking **Choose** with
*nothing else selected in the list* — clicking Choose while a different
row is still highlighted from earlier browsing silently adds *that* row's
folder instead of the one you navigated into; caught this twice, easy
mistake to make with this dialog):
- `...Universal:Interfaces:CIncludes`
- `...Universal:Interfaces:ComponentIncludes`
- `...Universal:Interfaces:RIncludes`

(`{Project}` was already present under **User Paths** by default, which
is what lets the flat, all-siblings source layout's `#include
"h264bsd_whatever.h"` quoted includes resolve with no extra work.)

**Project → Add Files, again, for libraries** (same dialog, same
"Choose" button, navigated to the library folders above instead of the
project folder):
- `MSL_All_PPC.Lib` (from `MacOS Support:Libraries:Runtime:Libs` — this
  is the non-Carbon, classic, PowerPC MSL runtime; there's also a
  `_D` debug variant and `.Shlb`/`_Carbon` variants, deliberately not
  used)
- `InterfaceLib` (from `...Universal:Libraries:StubLibraries` — the
  classic Toolbox import-stub library; note this folder is a long
  alphabetical list of ~150 per-manager stub libraries, InterfaceLib is
  just one entry in it, not a special top-level file)
- `QuickTimeLib` (same `StubLibraries` folder)

Final project: **63 files** (60 source + 3 libraries).

### First build attempt: `Project → Make`

This is the real result of the session: **Make actually ran the compiler
across all 60 source files** — meaning the Access Paths fix above is
correct and the QuickTime/Universal Interfaces headers genuinely resolve
from this CodeWarrior 8 install with no extra downloads needed. That was
flagged as "the single biggest unknown" in session 1's notes, and it's
now answered: **not a dead end, just needed the manual Access Paths
above.**

Result: **22 errors**, zero warnings, cleanly split into four categories,
all diagnosed (none are mysteries):

**1. Two `uintptr_t` errors — a gap in session 1's h264bsd vetting.**
`h264bsd_dpb.c` line 1030 and `h264bsd_slice_data.c` line 111 both do
pointer-alignment arithmetic via a cast through `uintptr_t`
(e.g. `(mbData + ((16 - (uintptr_t)mbData) & 15))` — rounding a pointer
up to a 16-byte boundary). Session 1's grep only checked whether
`stdint.h` was *included* and unused; it missed that `uintptr_t` is
*used* without any include at all in these two files. CodeWarrior 8's
MSL is pre-C99 and has no `uintptr_t`. **Fix**: add
`typedef unsigned long uintptr_t;` (32-bit pointers on this target) to
`basetype.h`, guarded with `#ifndef _UINTPTR_T_DEFINED` /
`#define _UINTPTR_T_DEFINED`, since `basetype.h` is already the common
header these files pull in. One-line fix, low risk.

**2. `H264Codec.c` struct-field mismatches against the real
`ImageCodec.h`/`ImageCodec.k.h`** — exactly what session 1 predicted
would need fixing, now with an exact list from the compiler:
   - `info->flags` — `'flags' is not a member of struct CodecInfo`
   - `info->pixelCode` — `'pixelCode' is not a member of struct CodecInfo`
   - `cap->baseCodecShouldCallDecodeBandForAllFrames` — not a member of
     `struct ImageSubCodecDecompressCapabilities` (or similarly-named cap
     struct — name was truncated in the errors pane, needs confirming
     against the header)
   - `drp->bufferSize` — not a member of `struct
     ImageSubCodecDecompressRecord`
   - (the errors list was 22 long and error-list scrolling in this small
     CodeWarrior panel is fiddly by mouse; there are more of this same
     category between "bufferSize" and the ComponentDispatchHelper.c
     error below that weren't individually transcribed yet — expect
     more `struct CodecInfo`/`ImageSubCodecDecompressCapabilities`/
     `ImageSubCodecDecompressRecord` field-name misses, not new kinds of
     problems)
   - **Fix plan**: pull the real `ImageCodec.h` (and `ImageCodec.k.h`)
     out of the VM — a single-file copy across the VirtFS bridge is
     fine, it's only directories that are broken — read the actual
     struct definitions, and correct every field name/order in
     `H264Codec.c` in one pass rather than guessing one at a time
     through the small errors panel.

**3. One error inside Apple's own `ComponentDispatchHelper.c`**, at line
173, while compiling `kPascalStackBased` "in H264Codec.c" — i.e. this is
Apple's generated dispatcher choking on something in how `H264Codec.c`
declared its component routines (likely the `IMAGECODEC_BASENAME()` /
`IMAGECODEC_GLOBALS()` setup, or a missing/misnamed selector routine).
Exact message wasn't fully captured — needs a re-look once the header
extraction above makes it possible to compare against a real, working
Base Codec skeleton or the header's own selector list.

**4. Two Rez errors in `H264Codec.r`**:
   - `H264Codec.r` line 31, `kH264CodecVersion,` inside the `'thng'`
     resource literal — `Expected '}', but got number (65536)`. This
     means our hand-written `'thng'` field list doesn't match the actual
     field count/order Apple's `Components.r` template defines — need to
     diff against the real template rather than the from-memory version
     written in session 1.
   - Lines 50 and 54, the two `resource 'STR ' (...)` blocks — `Can't
     find the declaration for the resource type 'STR '`. This one has an
     obvious fix: `H264Codec.r` only `#include`s `Components.r`, but the
     built-in `'STR '` resource template lives in Apple's `Types.r`.
     **Fix**: add `#include "Types.r"` to `H264Codec.r`.

### Current blocker (as of end of session 2)

Screenshot capture from the host Mac started failing partway through
cataloguing the remaining errors (`SCContentFilter`/screen-recording
permission error from the OS, not from CodeWarrior or the VM). The device
bridge itself (file transfer, shell commands) is still alive — only the
*visual* computer-use channel dropped. This is a host-side macOS
permission issue outside the VM's control. **Likely fix on the maintainer's
end**: check System Settings → Privacy & Security → Screen Recording for
the Claude desktop app, toggle it, and/or relaunch the app.

### Next session's concrete to-do list (supersedes the session-1 list)

1. Once screen capture is back: pull `ImageCodec.h`, `ImageCodec.k.h`,
   and Apple's real `'thng'` Rez template (from `Components.r`) out of
   the VM as individual files across the VirtFS bridge (single files are
   fine — only directories break), into the cloud sandbox, so struct
   fields and the resource template can be fixed precisely by reading
   the real declarations instead of guessing through the small errors
   panel one row at a time.
2. Apply, in one batch: the `uintptr_t` typedef in `basetype.h`; every
   corrected field name in `H264Codec.c`; `#include "Types.r"` and a
   corrected `'thng'` field list in `H264Codec.r`.
3. Get the corrected files back into the VM (same single-file-at-a-time
   approach across the share volume) replacing the copies in
   `H264 Classic Project Folder`, or just edit them directly in
   CodeWarrior's own editor inside the VM if that's faster at that point.
4. Re-run `Project → Make`. Expect a much shorter error list, possibly
   zero — the four categories above look like the whole story for this
   first build, not the tip of an iceberg.
5. Once it links: test registration only (does QuickTime's "Get Info" on
   a movie, or a component-listing utility, show "H.264 (Software)" as
   an installed codec?) before worrying about actual decode correctness.
6. Find or make a **small, Baseline-profile, low-resolution** H.264 test
   file (QCIF/CIF, a few seconds) for the first real decode attempt.

## Session 2, part 2 — all four error categories fixed against the real headers

Picked up right where the to-do list above left off. Screen capture came
back once the host Mac woke up (it really was just sleep — nothing to
fix on our end after all). Pulled `ImageCodec.h`, `ImageCodec.k.h`,
`ImageCompression.h`, `Components.r`, and `ComponentDispatchHelper.c`
out of the VM as individual files (single-file drag-and-drop across the
VirtFS "share" volume, confirmed working symmetrically for pulling files
out just like pushing them in) into
`quicktime-headers/` in the cloud sandbox, both as-fetched (classic
CR-only line endings) and as `.unix` copies (`tr '\r' '\n'`) for easy
reading/grepping. Then fixed every diagnosed error against the *actual*
struct/template definitions instead of documentation-memory:

**basetype.h** — no change needed this round; the `uintptr_t` typedef
added earlier this session is unaffected by anything found in the real
QuickTime headers.

**H264Codec.c** — four separate fixes, all confirmed line-by-line against
the real headers:
1. `H264CodecGetCodecInfo`: removed the nonexistent `info->flags` and
   `info->pixelCode`, replaced with the real three-way split confirmed in
   `ImageCompression.h`'s `struct CodecInfo` — `compressFlags`,
   `decompressFlags`, `formatFlags`. (`decompressFlags` now carries
   `codecInfoDoes32 | codecInfoDoesTemporal`.)
2. `H264CodecInitialize`: removed
   `cap->baseCodecShouldCallDecodeBandForAllFrames = true;` — confirmed
   this field simply does not exist anywhere in the real
   `struct ImageSubCodecDecompressCapabilities` (`ImageCodec.h` line
   468). Changed `cap->decompressRecordSize` from
   `sizeof(ImageSubCodecDecompressRecord)` to `0`, since we're using the
   classic single-entry-point `BandDecompress` call (see next item), not
   the newer `BeginBand`/`DrawBand`/`EndBand` sub-codec split that
   `decompressRecordSize` is for.
3. `H264CodecBandDecompress`: this was the root cause of most of the
   remaining ~18 untranscribed errors. Session 1 had written this
   function to take an `ImageSubCodecDecompressRecord *drp` (the
   *sub-codec* calling convention) but declared it under the selector
   name `BandDecompress`, which `ImageCodec.k.h` line 54 confirms actually
   dispatches with the signature
   `(IMAGECODEC_GLOBALS(), CodecDecompressParams *params)` — a
   *completely different* struct (the classic, older calling convention).
   Changed the signature to `CodecDecompressParams *params` and rewrote
   the body: `drp->codecData` → `params->data`, `drp->bufferSize` →
   `params->bufferSize`, and `drp->baseAddr`/`drp->rowBytes` → the
   destination PixMap embedded in `params`:
   `params->dstPixMap.baseAddr`/`params->dstPixMap.rowBytes` (masked with
   `0x3FFF`, since a PixMap's `rowBytes` packs flag bits into its top two
   bits per Inside Macintosh — easy to miss if you've never hit it
   before, and exactly the kind of thing that would have decoded garbage
   or crashed at runtime even if it had somehow compiled as written).
4. Added an explicit `#include <MixedMode.h>` near the top of the file.
   This is the actual fix for error category 3 below.

**ComponentDispatchHelper.c error, solved**: the `kPascalStackBased`
error at line 171-173 is Apple's own file building a classic Mixed-Mode
routine descriptor for our PowerPC component entry point (the
`BUILD_ROUTINE_DESCRIPTOR(kPascalStackBased | RESULT_SIZE(...) | ...)`
call) — completely standard, and not a bug in Apple's file at all.
`kPascalStackBased`, `RESULT_SIZE`, `STACK_ROUTINE_PARAMETER`, and
`kFourByteCode` are all defined in `MixedMode.h`, which apparently isn't
pulled in transitively by `Components.h` in this particular Universal
Interfaces snapshot (some are, some aren't — this is exactly the kind of
SDK-revision quirk that's impossible to predict without hitting it).
Fix: explicit `#include <MixedMode.h>` in `H264Codec.c` (added alongside
fix #4 above). Safe even if something else already includes it — every
Universal Interfaces header guards against double inclusion.

**H264Codec.r** — three fixes (one more than originally diagnosed):
1. Added `#include "Types.r"` before `#include "Components.r"` — fixes
   the two `'STR '` "can't find the declaration" errors, exactly as
   diagnosed.
2. Added `#define thng_RezTemplateVersion 1` before `#include
   "Components.r"` — this is the real fix for the "`Expected '}', but
   got number (65536)`" error. Confirmed by reading `Components.r`
   directly: the default template (version 0) is Apple's original
   13-field `'thng'` with no room for a version number, registration
   flags, icon-family id, or the multi-platform code array — exactly the
   four things our resource literal was trying to supply. Version 1 (the
   "extended" template, QuickTime 3.0+) adds all four.
3. **A second, previously-undiagnosed bug found while reading the real
   template**: the `ComponentPlatformInfo` array entry inside the
   `'thng'` resource had *five* comma-separated values
   (`kAnyComponentFlagsMask, componentHasMultiplePlatforms, 'dlle', 256,
   platformPowerPCNativeEntryPoint`) crammed into what the real template
   defines as exactly *four* fields per platform entry (flags, Code
   Type, Code ID, platform). `componentHasMultiplePlatforms` doesn't
   belong inside the array at all — it's a registration-flags value,
   already correctly set at the top-level `'thng'` field a few lines up.
   This would not have shown up in the original 22-error list (Rez
   never got that far before choking on the version-0 template first),
   so it's a good example of why "fix one error, see what the compiler
   says next" can hide a second bug behind a first one — worth
   remembering for the rest of this port. Removed the stray value; the
   array entry is now the correct four fields.

### Getting from 22 errors to zero

The fixes above were transferred into the VM and the error count came
down in stages: 22 → 18 → 13 → 8 → 2 → 1 → **0**. Each stage exposed a
genuinely different problem, and the last few were the interesting ones.

**A trap worth knowing about: CodeWarrior's Errors & Warnings window
does not always reflect the build you just ran.** After the first batch
of fixes went in, the panel still listed the *old* `info->flags` /
`info->pixelCode` errors, with the old source text quoted underneath.
That is what a stale error list looks like, and it cost real time chasing
a phantom. Two things settled it: opening the file in CodeWarrior's own
editor and searching it (`pixelCode` appeared only inside a comment,
proving the fixed file really was on disk), and running
`Project → Remove Object Code` for a guaranteed-from-scratch rebuild.
**Lesson: if the errors look impossible, verify the file in the editor
and force a clean build before believing the panel.**
`Project → Re-search for Files` and
`Project → Synchronize Modification Dates` are the other two tools for
this, and both are worth running after any out-of-IDE file swap.

**Error category 5 — the dispatch macros.** ComponentDispatchHelper.c
emitted three `#error` directives demanding `COMPONENT_BASENAME`,
`COMPONENT_GLOBALS` and `COMPONENT_DISPATCH_FILE`. The draft had defined
only `IMAGECODEC_BASENAME`/`IMAGECODEC_GLOBALS`, plus two macros
(`IMAGECODEC_DECOMPRESS`, `IMAGECODEC_QUALITY_CHECK`) that nothing in
Apple's headers actually reads. The two headers do different jobs and
want different macros, and nothing bridges them automatically. Fixed by
adding the `CALLCOMPONENT_*` spellings (which ComponentDispatchHelper.c
falls back to) plus `COMPONENT_UPP_SELECT_ROOT() ImageCodec`. Also:
**GLOBALS must expand to a complete parameter declaration including the
name `storage`** — `H264GlobalsPtr storage`, not bare `H264GlobalsPtr` —
because the generated dispatcher body refers to `storage` by name.

**Error category 6 — there is no `ImageCodecDispatch.h`.** The natural
assumption is that Apple ships a ready-made dispatch table for image
codecs. It does not. This CodeWarrior's ComponentIncludes folder holds
exactly 16 files, all `.k.h` prototype headers plus
ComponentDispatchHelper.c itself. Every component of this era wrote its
own selector table. So `H264CodecDispatch.h` was written from scratch —
see the long comment at the top of that file, which documents the
indexing rules in detail because most of them are not compiler-checkable.

**The architecture bug this uncovered (the most important finding).**
Writing the dispatch table forced a decision the draft had been
straddling. There are two ways to write a QuickTime decompressor:

  - **(A) self-contained** — implement `PreDecompress` (0x0005) and
    `BandDecompress` (0x0006), both of which take `CodecDecompressParams`
    (carrying `data`, `bufferSize`, and `dstPixMap`).
  - **(B) base-codec client** — delegate 0x0005/0x0006 to Apple's base
    decompressor (`'base'`), which calls back into `Preflight` (0x0200),
    `Initialize` (0x0201), `BeginBand`, `DrawBand`, `EndBand`. Those take
    `ImageSubCodecDecompressRecord`, a *different* struct with
    `baseAddr`/`rowBytes`/`codecData` and deliberately **no** `bufferSize`.

That missing `bufferSize` is precisely the original session-1 compile
error. The draft had a function named `BandDecompress` (an (A) selector)
written against (B)'s struct, and carried `Preflight`+`Initialize` from
(B) alongside it. **We settled on (A)**: the code was already mostly
shaped for it, it needs no delegate component, and with nothing between
us and the destination PixMap it is far easier to debug a first decode.
The migration path to (B), if we ever want the base codec's
buffer/scaling help, is recorded in H264CodecDispatch.h.

**Also fixed along the way, all found by reading the real headers:**

- `Base_ImageCodecOpen` / `Base_ImageCodecClose` **do not exist** — they
  were invented in the from-memory draft and would have been undefined
  symbols at link time. Replaced with the genuine idiom: `NewPtrClear` +
  `SetComponentInstanceStorage` in Open, `DisposePtr` in Close. Added a
  `Target` handler too.
- **Don't write your own `CanDo`.** ComponentDispatchHelper.c generates
  one (near its line 400) that answers by running the selector through
  the real dispatch table, so it can never drift out of sync with it. A
  hand-written one produces "object 'H264CodecCanDo' redefined".
- `kAnyComponentFlagsMask` is **not** a general symbolic constant. In
  Components.r it is a *field default*
  (`unsigned hex longint kAnyComponentFlagsMask = 0;`), in scope only for
  that one field position. Using it for the platform array's flags field
  gives Rez "Badly formed expression". Use plain `0`.

### The last two errors were project settings, not code

Worth calling out separately, because no amount of staring at C would
have found them:

1. **`Link Error: undefined 'main' (code), referenced from '__start' in
   MSL_All_PPC.Lib`.** The PPC Linker panel's **Entry Points → Main**
   field still said `__start`, the MSL *application* startup stub, which
   in turn requires a `main()`. A component has no `main`; its entry
   point is the dispatcher. Fixed by setting Main to
   `H264CodecComponentDispatch` — which is also exactly the symbol named
   in the `'dlle'` resource, so the two now agree.

2. **`Link Error: Can't write application 'H264Codec'.`** The PPC Target
   panel's output **File Name** was `H264Codec` — identical to the
   CodeWarrior project file sitting in the same folder. The linker was
   being asked to overwrite the project with its own output. Fixed by
   renaming the output to **`H264 Decoder`**. (Project type was already
   correctly "Shared Library", and Type already `'thng'`.)

### What is actually proven, and what is not

Proven: it compiles, it links, Rez builds the resources, and the output
is a well-formed `'thng'` file. The h264bsd decoder core builds
unmodified on CodeWarrior 8 apart from the single `uintptr_t` typedef —
which was the big portability bet of this whole project, and it paid off.

**Not** proven, and deliberately not claimed:

- that QuickTime will register the component at all
- that the `'thng'` fields are right — a wrong dispatch-table ordering or
  a wrong selector offset links perfectly and then misbehaves at runtime
  (see the warnings at the top of H264CodecDispatch.h)
- that any frame decodes
- that anything appears on screen

### Known correctness gap to fix before any decode test

`CopyPlanarYUVToPixMap` currently writes **planar YUV 4:2:0** (it was
written for `k420YpCbCr8PlanarCodecType`), but `PreDecompress` now asks
the ICM for a **32-bit RGB** destination (`wantedPixelSize = 32`), which
is the universally-supported path on Mac OS 9 and sidesteps the
`wantedDestinationPixelTypes` format-negotiation dance entirely.

Those two do not agree yet. This was left deliberately inconsistent
rather than papered over, because it does not block the build and the fix
is a known, self-contained piece of work: write a YUV420 → 32-bit RGB
converter (BT.601 limited range, integer coefficients, clamp table —
plenty fast for QCIF/CIF on a G3). **This must be done before the first
decode test, or the picture will be garbage even if everything else is
perfect.** The alternative — keeping planar YUV output and negotiating it
through `wantedDestinationPixelTypes` — is faster at runtime but much
fiddlier, and is the wrong thing to attempt before we know the component
even loads.

### Next session's to-do list

1. **Install it and see if it registers.** Copy `H264 Decoder` from
   `H264 Classic Project Folder` into the System Folder's Extensions
   folder, reboot, and check whether QuickTime lists it. This is the
   highest-information next step and it is cheap: a component that
   registers proves the `'thng'`, the `'dlle'`, the entry point and the
   dispatch table are all at least structurally sound.
2. Write the YUV→RGB conversion (see above).
3. Find or make a **small, Baseline-profile, low-resolution** H.264 test
   file (QCIF/CIF, a few seconds) for the first real decode attempt.
4. Only then worry about decode correctness, colour, and speed.

### Workflow notes for whoever picks this up

- **The VirtFS bridge works fine for individual files in both
  directions** — push and pull are both proven now. Only *directories*
  are broken; keep everything flat.
- **Replacing a file CodeWarrior has open in an editor fails** with "You
  cannot replace the document ... because it is in use." Close the editor
  window first; the Window menu lists open editors.
- **The stuck-key bug is real and recurs.** Typing a whole string into a
  guest text field can leave a key auto-repeating (a field became
  `__aaaaaaaa...`). Recovery: press that key once on its own to clear it,
  then type character-by-character with individual key presses, using
  `shift+<letter>` for capitals.
- **Do not send Cmd+Q or Cmd+W to the guest.** UTM intercepts them at the
  *host* level; Cmd+Q raises "Quitting UTM will kill all running VMs".
  Cancel that dialog if it appears. Ordinary combos like Cmd+K pass
  through to the guest fine.
- If guest mouse clicks start landing in the wrong place entirely,
  closing and reopening the VM display window (UTM's Window menu → the
  VM) re-syncs the coordinate mapping.

## Session 3 — installed, booted, and tested against a real H.264 file

Short version: **the component builds, installs, and loads without
crashing Mac OS 9 — but QuickTime does not register it.** We now have a
clean, unambiguous failure to chase instead of a vague unknown.

### What was done

1. Installed `H264 Decoder` by dragging it onto the System Folder icon.
   Mac OS 9 responded with *"This item needs to be stored in the
   Extensions folder in order to be available to the computer"* and filed
   it itself. Worth noting: the OS only offers that for files it
   recognizes as system extensions, so the file **type `'thng'` is
   correct** and was set properly by the CodeWarrior target.

2. Rebooted. **Mac OS 9 came up completely normally with the component
   installed.** This matters more than it sounds: a malformed component
   can hang the Component Manager during startup, and it doesn't. The
   `'thng'` is at least well-enough formed not to poison the boot.

3. Built a proper test file with ffmpeg (in the cloud sandbox) and moved
   it into the VM:
   `176x144 (QCIF), Constrained Baseline (profile_idc 66), Level 1.1,
   yuv420p, CAVLC (cabac=0), no B-frames, 15 frames`
   verified to contain an `avc1` sample description with an `avcC`
   extension holding 1 SPS (23 bytes) + 1 PPS (5 bytes), and
   `lengthSizeMinusOne = 3` (so 4-byte NAL length prefixes). CABAC is
   explicitly off because h264bsd is Baseline-only and cannot do CABAC.
   This is exactly the shape of input the codec was written for; keep
   using it.

4. Opened it in QuickTime Player 5 in the VM.

### The result

QuickTime **opened the movie** — parsed the container, read the video
track, opened a correctly-sized player window — and then said:

> "You may experience problems playing a video track in "h264test.mov"
> because **the required compressor could not be found**."

(and earlier, the generic *"QuickTime is missing software required to
perform this operation"*).

That is QuickTime's "I have no decompressor for this sample description"
message. So: the container is fine, the file is fine, QuickTime is
looking for an `'avc1'` decompressor — and **it is not finding ours.**

### Where the problem is NOT

Ruled out by the above:

- Not a build problem — it compiles and links clean.
- Not a file-type problem — the OS recognizes it as a system extension.
- Not an install-location problem — the OS filed it into Extensions itself.
- Not a boot-time crash — the machine boots normally with it installed.
- Not a bad test file — QuickTime parses it and identifies the track.

### Prime suspect: the CFM entry point is probably not *exported*

For a PowerPC QuickTime component, registration works like this:

1. Component Manager reads the `'thng'` resource at startup.
2. The platform entry says code type `'dlle'`, id 256.
3. It reads `'dlle'` 256, which contains a **symbol name** —
   for us, `"H264CodecComponentDispatch"`.
4. It loads the file's PEF code fragment and calls **FindSymbol** for
   that name.
5. If the symbol isn't found, registration silently fails — no error,
   no crash, the component simply never appears. Which is exactly the
   behaviour we're seeing.

**CodeWarrior does not export symbols from a shared library by default.**
The PPC PEF settings panel has an Export setting (typically
`None` / `All Globals` / `Use .exp file` / `Use #pragma export`). If it
is on `None`, the fragment exports nothing, FindSymbol fails, and the
component never registers — silently. This fits the evidence exactly and
is the **first thing to check next session**:

> Edit → H264Codec Settings → Linker → **PPC PEF** → Export: set to
> **All Globals** (simplest), or add `#pragma export on/off` around the
> dispatch entry, then rebuild and reinstall.

Note we already set PPC Linker → Entry Points → Main to
`H264CodecComponentDispatch`. That satisfies the *linker*, but being the
fragment's main entry point is not the same thing as being an *exported
symbol* that FindSymbol can look up by name — which is what `'dlle'`
requires. Both need to be true.

### Other candidates, in rough order of likelihood

2. **`'thng'` platform entry mismatch.** We declare
   `platformPowerPCNativeEntryPoint` (5), which means "the named symbol
   is a plain native function". The alternative, `platformPowerPC` (2),
   means "the named symbol is a **RoutineDescriptor**". Our `'dlle'`
   names the plain function `H264CodecComponentDispatch`, so 5 is
   self-consistent — but ComponentDispatchHelper.c *also* generates a
   routine descriptor called `H264CodecComponentDispatchRD`, so if 5
   turns out to be wrong, the matching fix is to switch the `'thng'` to
   platform 2 **and** point `'dlle'` at the `...RD` symbol. Change both
   together or neither.

3. **Missing `'cfrg'` resource.** CodeWarrior's Shared Library target
   normally emits one; worth confirming with a resource editor
   (ResEdit/Resorcerer) that the built file actually contains `'thng'`,
   `'dlle'`, `'STR '` **and** `'cfrg'`, and that the `'thng'` fields
   read back as `'imdc'` / `'avc1'` / manufacturer as intended. Opening
   the built component in ResEdit is a cheap, high-information check and
   is probably worth doing *before* another build/install/reboot cycle,
   since it verifies the resources independently of the Component
   Manager.

4. **Component flags.** We set
   `componentHasMultiplePlatforms | componentDoAutoVersion` as
   registration flags. If the platform array is being misread, dropping
   to a single-platform (`thng_RezTemplateVersion 0`-style) resource with
   the code type/id at the top level is a simpler configuration to get
   working first, and can be elaborated later.

### Suggested order next session (cheapest/highest-information first)

1. Open the built `H264 Decoder` in ResEdit and confirm which resources
   are actually present and what the `'thng'` fields say. No rebuild
   needed.
2. Check (and almost certainly fix) the PPC PEF **Export** setting.
3. Rebuild, reinstall into Extensions, reboot, re-open `h264test.mov`.
4. If it still doesn't register, try the platform-2 + `...RD` variant
   from candidate 2 above.

Once it *does* register, expect the next failure to be the **YUV vs RGB
mismatch** already documented above — `CopyPlanarYUVToPixMap` writes
planar YUV while `PreDecompress` asks for 32-bit RGB. Fix that before
judging any picture that appears.

### Environment notes added this session

- **UTM's "Restart" / reset is unreliable on this VM** — it left the
  guest at a black screen twice, with the VM still "running". A full
  **Power → Force shut down**, then start from UTM's play button, boots
  reliably every time. Prefer guest `Special → Shut Down` where possible,
  and never trust reset.
- Force shutdown from a hung boot did **not** corrupt the disk image.
- The guest-click coordinate offset (~+46 x, +6 y) reappears whenever the
  VM window is resized or the display is re-created. Verify it by
  clicking once and seeing where the cursor lands, then compensate.

## Session 4 — the registration hunt, and what it definitively ruled out

The component builds, installs, and boots cleanly. It still does not
register with the Component Manager: QuickTime opens the test movie,
identifies the video track, and reports *"the required compressor could
not be found."* This session was spent narrowing that down, and the
narrowing was productive even though the bug is not yet closed.

### The single most useful technique found so far

**You can inspect the built component from the host, without any Mac OS 9
tooling at all.** Drag `H264 Decoder` onto the "share" volume, and the
VirtFS bridge writes three files on the macOS side:

    H264 Decoder        the data fork (the PEF code fragment)
    H264 Decoder.idump  the type/creator ('thng' / '????')
    H264 Decoder.rdump  the ENTIRE resource fork, as readable Rez text

The `.rdump` is a complete, human-readable dump of every resource with
hex + ASCII. That turns "is the 'thng' right?" from a guessing game into
a two-minute read. And the data fork can be parsed as a PEF container
with a short Python script to list the export table (there is one in the
transcript; PEF is well documented and the loader section header is a
fixed layout). **Do this first, before any build/install/reboot cycle.**
A cycle costs several minutes and a reboot; a dump costs one command.

### Verified correct, by direct inspection of the built binary

Every one of these was *checked*, not assumed:

- File type is `'thng'`. (Mac OS 9 itself confirms this a second way: it
  offers to file the component into Extensions, which it only does for
  recognized system extensions.)
- The component is in the Extensions folder.
- `'thng'` decodes exactly as intended: `'imdc'` / `'avc1'` /
  manufacturer, name `'STR '` 256, info `'STR '` 257, version 0x00010000.
- `'dlle'` contains the exact symbol name, NUL-terminated.
- `'STR '` 256 and 257 contain the right Pascal strings.
- `'cfrg'` 0 is present, architecture `'pwpc'`, `where` = data fork,
  offset/length 0/0 (meaning "the whole fork"), fragment name
  "H264 Decoder".
- The data fork is a valid PEF (`Joy!peffpwpc`), 3 sections, with a
  loader section whose `mainSection`/`mainOffset` point into the data
  section -- i.e. the linker's Main entry point setting took effect.
- **`H264CodecComponentDispatch` is genuinely in the PEF export table**,
  as a TVector, alongside `H264CodecComponentDispatchRD` (data) and
  `H264CodecCanDo`.
- Mac OS 9 boots normally with the component installed -- so the `'thng'`
  is at least well-formed enough not to poison Component Manager startup.

### The export bug that WAS found and fixed

CodeWarrior's **PPC PEF -> Export Symbols defaults to `None`**. A shared
library that exports nothing still builds and links perfectly, installs
fine, and then fails registration silently -- FindSymbol simply doesn't
find the entry point, and nothing anywhere reports an error. Confirmed by
dumping the export table: before the fix the only exports were two MSL
internals (`__som_check_ev`, `__som_check_new`).

Fix: set Export Symbols to **"Use #pragma"** and bracket the
`ComponentDispatchHelper.c` include in H264Codec.c with
`#pragma export on` / `#pragma export off`. ("All globals" also works but
buries the build in "Symbol may be too long for export" warnings, because
it tries to export MSL's C++ template instantiations whose mangled names
overflow the Str255 that CFM uses for symbol names.)

**And the trap underneath the trap:** changing that setting and rebuilding
was not enough, because the file had been replaced from outside the IDE
and CodeWarrior did not recompile it. The export table was still empty
after a normal Make. **`Project -> Remove Object Code` then Make** is what
actually produced the exported symbol. This is the second time in this
project that a stale CodeWarrior build produced a confusing wrong answer
(the first was the stale Errors & Warnings panel in session 2). Treat
"I changed something and the result is identical" as a stale-build
symptom until proven otherwise.

### Ruled out: the platform-code ambiguity

`ComponentPlatformInfo`'s platform field had two plausible readings, and
ComponentDispatchHelper.c generates a callable symbol for each:

    platformPowerPC (2)                 -> 'dlle' names the RoutineDescriptor
    platformPowerPCNativeEntryPoint (5) -> 'dlle' names the bare function

Rather than test one and then the other, both were declared at once in a
two-entry platform array, each pointing at its own `'dlle'` naming the
appropriate symbol. Verified in the dump: array count 2, entry one
`'dlle'` 256 / platform 2, entry two `'dlle'` 257 / platform 5, with both
symbols exported. **Neither registered.** So the platform code is not the
problem, and the two-at-once trick saved a whole cycle proving it.

### Where that leaves it: two changes now staged, untested

With every structural ingredient verified and the platform reading
eliminated, two things remain suspect, and both are now fixed in the
source (transferred to the VM, not yet built/tested):

**1. `GetCodecInfo` was filling in three fields out of seventeen.**
This is a real bug independent of registration. `CodecInfo` is not
cleared by the caller, and the ICM reads fields we were leaving as
whatever was on the stack -- including `minimumWidth` / `minimumHeight`,
which it uses to decide whether the codec can handle an image at all.
Now `memset`s the struct and fills in every field honestly: typeName,
version/revisionLevel from kH264CodecVersion, vendor, accuracies (255
for decompress, 0 = "unknown" for compress), speeds 0 ("unknown"), and
minimumWidth/Height = 16 (one H.264 macroblock).

**2. The `'thng'` has been reverted to the classic 13-field template**
(`thng_RezTemplateVersion 0`), with code type `'dlle'` and code ID 256 in
the *top-level* fields and no platform array at all. This is the oldest
and most widely-shipped form. Given that the extended template's platform
array was verified byte-correct and still didn't register, the array
machinery itself is the leading suspect, and this removes it entirely.

If the simple form registers, that is the answer and this file should
stay simple. If it still doesn't, the next things to try, in order:

- Open the built component in ResEdit *inside* the VM and confirm the
  resource fork is readable there too (the .rdump proves the bytes are
  right, but not that Mac OS 9's Resource Manager is reading that fork --
  a long shot, but cheap).
- Write a tiny test app (or use a component-lister utility) that calls
  `CountComponents` / `FindNextComponent` for type `'imdc'` and prints
  what it finds. That distinguishes "not registered at all" from
  "registered but the ICM won't use it" -- a distinction the QuickTime
  Player error message cannot make, and which every hypothesis so far has
  had to guess at.
- Try `componentDoAutoVersion` / flags variations, and a non-zero
  component flags value.

That second bullet is probably the highest-value next step overall: every
cycle so far has been "change something, reboot, watch QuickTime say the
same sentence." A component enumerator would replace that with an actual
readout, and would have short-circuited most of this session.

### Environment notes added

- The `.rdump` / `.idump` sidecars are the single best debugging affordance
  discovered in this project. Use them.
- The remote bridge dropped repeatedly this session; each drop loses the
  computer-use grant and needs re-approval. Long unattended runs of VM
  work are unreliable -- prefer short, verifiable steps that leave
  durable artifacts (like a dumped component) behind.

## Session 5 — THE ANSWER. It was registering all along.

Built a diagnostic instead of guessing, and it overturned four sessions of
assumptions in about thirty seconds of runtime.

### The tool

`ComponentProbe.c` — a Std C Console app built in its own CodeWarrior
project ("Probe", in H264 Classic Project Folder). It enumerates
components, tries a manual `RegisterComponentResourceFile`, and calls
`OpenComponent` on ours. **Crucially it writes its report to
`share:h264probe.txt`**, i.e. onto the VirtFS volume, so the results can
be read from the host with one shell command instead of being squinted
out of screenshots. That single design choice is what made the VM
finally legible.

### What it found

**The component was registered the whole time.** From the probe's dump of
all 88 image decompressors on the system:

    [8] type='imdc' sub='avc1' manu='wY9H' flags=0x00000000 mask=0x00010222
         name: H.264 (Software)
         info: H.264/AVC Baseline decoder for QuickTime, powered by
               h264bsd. Homebrew port for Mac OS 9.2.2 / PowerPC.

Sitting there between "Apple BMP" and "Apple AVR JPEG", with our name and
our info string, read straight out of our `'STR '` resources. `'imdc'` /
`'avc1'` search returns exactly 1 match: ours.

So the `'thng'`, the `'STR '`s, the `'cfrg'`, the file type, the install
location, the boot-time scan -- all of it has been correct since the
first install. `RegisterComponentResourceFile` returning 0 was not a
rejection; it was "nothing new to register, it is already in."

**The actual failure is one line further on:**

    -- OpenComponent() on our component --
       OpenComponent returned NULL.

The Component Manager finds the component and cannot get to its code.

### Why OpenComponent fails

Two facts from parsing the built PEF settle it.

First, the imports are all trivially resolvable -- nothing exotic, nothing
that could fail to bind:

    InterfaceLib (11): CallComponentFunctionWithStorageProcInfo,
      DisposeHandle, SetComponentInstanceStorage, MemError, NewPtr,
      BlockMove, HLock, HUnlock, DisposePtr, GetHandleSize, NewPtrClear
    QuickTimeLib (1): GetImageDescriptionExtension

So it is not an unresolved import.

Second, and this is the bug:

    mainSection = 1   mainOffset = 0x154
    initSection = -1
    termSection = -1

The fragment has a **main** symbol and no init routine. And our `'cfrg'`
declares `usage = kImportLibraryCFrag (0)` -- a shared library.

**For a CFM shared library, the `main` symbol IS the fragment's
initialization routine.** CFM calls it while preparing the fragment,
passing a `CFragInitBlockPtr` and expecting an `OSErr` back. We had set
PPC Linker -> Entry Points -> Main to `H264CodecComponentDispatch`. So
CFM calls our *component dispatcher* as an initialization routine: it
receives a `CFragInitBlockPtr` where it expects `ComponentParameters *`,
runs a selector lookup on whatever that memory happens to contain, and
returns something non-zero. CFM reads that as "initialization failed",
refuses to prepare the fragment, and every `OpenComponent` on the
component returns NULL forever after -- silently, because a component
that will not open is indistinguishable from a missing codec as far as
QuickTime's error message is concerned.

The irony: setting Main to the dispatcher is what cleared the original
`undefined 'main'` link error back in session 2. It fixed the link and
created this. The `'dlle'` mechanism never needed it -- the Component
Manager finds the dispatcher **by name** with FindSymbol, which is why
the symbol only ever needed to be *exported*, not to be *main*.

### The fix

Clear PPC Linker -> Entry Points -> **Main** (leave it empty). With no
main symbol, `__start` is never referenced, so the MSL "undefined main"
error does not come back, and CFM prepares the fragment without trying to
run our dispatcher as an initializer.

### The lesson worth keeping

Four sessions were spent on "why won't it register", and it was
registering the entire time. Every hypothesis was plausible, several were
checked carefully against the real headers and the real binary, and all
of them were answers to the wrong question -- because the only
instrument available (QuickTime Player's one error message) could not
distinguish "not registered" from "registered but won't open".

**Build the instrument first.** The probe took one session to write and
answered the question immediately, and it is reusable: `Probe` stays in
the project folder, and re-running it after any change prints the live
component list and the OpenComponent result to a file the host can read.

---

# Session 6 -- Root cause found: the fragment is declared as an application

**Date:** 2026-09-09

## Summary

`OpenComponent()` has been returning NULL on a component that registers
perfectly. The reason is a single byte in the `'cfrg'` resource, and the
reason for *that* byte is that the CodeWarrior target's **Project Type is
still "Application"**.

No screen access was available this session (the computer-control
permission was declined), so the whole diagnosis was done over the file
bridge from the host side, by decoding the `.rdump` sidecar of the built
component. That turned out to be enough.

## The evidence

Dragging the built component onto the shared volume produces
`H264 Decoder.rdump`, the entire resource fork rendered as Rez text.
Decoding the `'cfrg'` byte by byte:

```
'cfrg' (0, "Generated cfrg"), 88 bytes

  CFragResource header
    [10..11] version      = 0x0001
    [30..31] memberCount  = 1

  member[0], base = 32
    [32..35] architecture = 'pwpc'
    [54]     usage        = 0x00     <-- kApplicationCFrag
    [55]     where        = 0x01     <-- kDataForkCFragLocator  (correct)
    [56..59] offset       = 0        (correct)
    [60..63] length       = 0        (kCFragGoesToEOF, correct)
    [72..73] memberSize   = 0x0038 = 56  (= 88 - 32, self-consistent)
    [74..]   name         = "H264 Decoder"
```

The field offsets are pinned down by two independent anchors that agree:
`where` is the only byte in the member equal to 1, and it must be 1
(`kDataForkCFragLocator`) for a PEF in the data fork; and `memberSize`
must equal 56, which is exactly `88 - 32`. Both readings put `usage` at
member+22, i.e. byte 54, i.e. **0 = kApplicationCFrag**.

## Why that single byte produces exactly the symptom we see

`OpenComponent()` on a `'dlle'` component is, underneath:

```
GetDiskFragment(component file)   ->  connection
FindSymbol(connection, name from the 'dlle' resource)
call it
```

`GetDiskFragment` loads **import library** members only. An application
member is invisible to it. So:

- **Registration succeeds** -- it reads the `'thng'` resource and never
  touches the fragment. This is why ComponentProbe found us as entry [8]
  of 88 `'imdc'` decompressors with correct name and info strings.
- **Opening always fails** -- and `OpenComponent` reports failure as a
  bare NULL, with no error code anywhere, which is why four sessions of
  looking at the `'thng'`, the `'dlle'`, the export table and the import
  list found nothing wrong. Nothing *was* wrong with any of them.

It also retroactively explains the `Link Error: undefined 'main'` from
session 4. Only an Application target requires a `main`. Supplying
`H264CodecComponentDispatch` as `main` made the link succeed and papered
over the real signal, which was "this target is the wrong type."

## The fix (CodeWarrior settings, not source)

**PPC Target -> Project Type: Application -> Shared Library.**

Consequences to handle in the same pass:

- Switching project type resets **Type** to `shlb`; set it back to `thng`.
  Creator stays `????`. File Name stays `H264 Decoder`.
- **PPC Linker -> Entry Points -> Main** must be **cleared**. A shared
  library needs no `main`, and leaving the dispatcher named there is at
  best pointless.
- Leave Initialization / Termination blank unless the linker objects; the
  component is pure C with no static constructors that need the runtime.
- **PPC PEF -> Export Symbols must stay "Use #pragma"** (session 5's fix).
  Do not let a settings pass reset it.
- **Project -> Remove Object Code** before Make. A plain Make silently
  kept a stale object last time this project's settings changed.

## Second bug fixed while in here: the negative dispatch block

Reading the real `ComponentDispatchHelper.c` (pulled off the share)
rather than trusting memory turned up this, in `COMPONENTSELECTORLOOKUP`:

```c
theRange = selector_num >> RangeShift;
if (theRange < 0) {
    selector_num += SelOffset;
    if (selector_num >= 0) {
        infoP = &(cdhRangeDispatch[0].cdhDispatchInfoP)[selector_num];
    }
}
```

The negative branch **bounds-checks the low end only**. The positive
branch compares against `rangeMax`; this one does not. So if `SelOffset`
exceeds the number of entries listed, the lookup reads off the end of the
array and either calls whatever two words follow it in static data or
returns them as a result. Silently.

Our table used `ComponentSelectorOffset(-kComponentGetPublicResourceSelect)`
with nine entries -- correct if and only if that constant is exactly -9.
Which it is depends on the SDK:

| SDK | -8 | -9 | -10 |
|---|---|---|---|
| UI 3.4.1 / QuickTime 5 | GetMPWorkFunction | GetPublicResource | -- |
| later | GetMPWorkFunction | ExecuteWiredAction | GetPublicResource |

`Components.k.h` in this install is UI 3.4.1 and declares nine standard
calls with no ExecuteWiredAction, which points at -9. But if it were -10,
`Open` (selector -1) would index at 9 into a 9-entry table -- one past the
end, landing exactly on the call we cannot make.

Rather than gamble, the table now pins `ComponentSelectorOffset(12)` with
twelve entries ending at `Open`. The helper only computes
`index = selector + SelOffset`, so a table is correct iff its last entry
is selector -1 and it has `SelOffset` entries counting back from there --
a relationship entirely internal to the file. Twelve entries / offset 12
is correct for any SDK value from -1 to -12. `ComponentError(X)` expands
to `kCOMPONENT_ERROR, 0,` and never references a symbol named `X`, so the
three leading placeholders cannot fail to link.

## Third change: `H264Codec.r` back to the extended template

Session 4 reverted `H264Codec.r` from `thng_RezTemplateVersion 1` to the
13-field template, on the theory that the per-platform code array was
blocking registration. The `.rdump` plus ComponentProbe together show
that theory was wrong: the extended-template build produced exactly the
intended `'thng'` and **did register**. Registration was never the bug.

So the extended template is restored, including both platform entries:

```
0, 'dlle', 256, 2   /* platformPowerPC -> RoutineDescriptor symbol */
0, 'dlle', 257, 5   /* platformPowerPCNativeEntryPoint -> bare TVector */
```

`ComponentDispatchHelper.c` generates both symbols for us, and both are in
the PEF export table, so declaring both costs one resource and removes a
coin flip about which reading of "PowerPC" the Component Manager uses.

Registration flags and platform codes are written as numeric literals
(`9`, `2`, `5`) rather than symbolic names: Rez in this install rejects
some Components.r constants outside the exact field they were declared
for (`kAnyComponentFlagsMask` did this in session 3), and a literal
cannot fail that way.

## ComponentProbe version 2

Rewritten to go one layer below the Component Manager, which reports
nothing, to the Code Fragment Manager, which reports real error codes.
It now:

1. Prints every standard selector constant, settling the -9 / -10
   question empirically.
2. Dumps the installed component's `'cfrg'` as hex and decodes `usage`
   and `where`, naming `usage = 0` as the bug in the report itself. This
   is how we watch the byte flip after the settings change.
3. Calls `GetDiskFragment` on the component file directly and prints the
   OSErr plus CFM's `errName` string, then `FindSymbol` for both exported
   symbols with their addresses and symbol classes. Expected failure
   codes are listed inline in the output (-2804 `cfragNoLibraryErr` is
   the one an application member produces).
4. On a successful `OpenComponent`, calls `CallComponentVersion` and
   `CallComponentCanDo` on 0x00 / 0x05 / 0x06 (expect 1) and 0x03 / 0x200
   (expect 0), which walks the same table real dispatch walks -- so a
   mis-indexed block shows up as a wrong answer here instead of a crash
   three layers into QuickTime.

The version-1 habit of describing all 88 decompressors is dropped; it
made the report unreadable and it has served its purpose.

## Technique worth keeping

Dragging any built Mac OS 9 file onto the shared volume yields a `.rdump`
(the whole resource fork as readable Rez text) and the raw data fork,
both readable from the host with one shell command. Binary verification
of `'thng'` / `'dlle'` / `'cfrg'` stops being a guessing game. This is now
the first thing to do after any build, before installing or rebooting --
it caught a bug this session that four rounds of reasoning had missed.

## State at end of session 6

Written to the share, ready to be dragged into the VM's project folders:

- `H264CodecDispatch.h`  -- offset 12 / 12 entries, header-independent
- `H264Codec.r`          -- extended template restored, both platforms
- `ComponentProbe.c`     -- version 2
- `H264Codec.c`          -- unchanged from session 5 (YUV -> RGB blit),
                            still needs to be dragged in

Previous versions kept alongside as `*.bak-v1`.

**Next action:** the two CodeWarrior settings above, Remove Object Code,
Make, then drag the built `H264 Decoder` to the share so the `'cfrg'`
`usage` byte can be checked from the host *before* installing and
rebooting.

---

# Session 7 -- IT WORKS. H.264 decoding correctly on Mac OS 9.2.2

**Date:** 2026-09-10 / 09-11

QuickTime Player 6 on Mac OS 9.2.2 opens an H.264 `.mov` and plays it, and
the picture is pixel-for-pixel what ffmpeg renders from the same file:
timestamp, frame counter, all six colour bars in the right order, the
diagonal gradient, the grey cross, both noise blocks.

Three distinct bugs stood between session 6 and that frame. Each one is
written up below with the evidence that identified it, because every one
of them presented as something other than what it was.

## Bug 1 -- 'thng'/'dlle' cannot reach a code fragment on this system

### What was tried

Every combination, built and measured:

| `'thng'` template | `'dlle'` names | Result |
|---|---|---|
| extended (platform array) | `...ComponentDispatchRD` | `OpenComponent` NULL, our code never entered |
| extended (platform array) | `...ComponentDispatch` | same |
| simple 13-field | `...ComponentDispatchRD` | **caller dies, type 3** |
| simple 13-field | `...ComponentDispatch` | **caller dies, type 3** |
| simple 13-field | a four-line stub that only logs | **caller dies, type 3, stub never logged** |

### Why that last row is conclusive

The stub did nothing but append one line to a file and return
`badComponentSelector`. It could not crash. It never ran. **A stub that
cannot survive being called proves the address being called is not the
stub** -- so no choice of symbol name can fix this, and the `'dlle'`
route is a dead end here.

### Two useful side-findings

- **The `'thng'` template controls whether the code path is reached at
  all.** With the extended template (`componentHasMultiplePlatforms` +
  a `ComponentPlatformInfo` array) the Component Manager registers the
  component and then never follows through to the code -- silent NULL.
  With the simple 13-field template it *does* follow through, and
  crashes. Session 4's instinct about the platform array was right; it
  simply never got built (the sources were edited eleven minutes after
  that era's last link, so every "13-field template fails" data point
  was actually measuring the extended-template binary still installed).
- **A `'thng'` resource in Extensions is actively harmful once this is
  known.** The Component Manager finds it during startup scanning,
  follows the broken path, and takes QuickTime Player down with a type 3
  before its window appears. The shipped file must have no `'thng'`.

### The fix

Ship the codec as an inert CFM shared library and register it at run
time:

```c
GetDiskFragment(codec file)            -> connection (held open)
FindSymbol("H264CodecComponentDispatchRD")
RegisterComponent(&cd, (ComponentRoutineUPP)rdAddr, /*global*/ 1, ...)
```

Verified end to end:

```
Before: 'imdc'/'avc1' registered = 0
RegisterComponent (global) -> 0x000F0265
After:  'imdc'/'avc1' registered = 1
OpenComponent -> 0x00930016
CallComponentVersion       = 0x00010000
CanDo(0x00 GetCodecInfo)   = 1
CanDo(0x05 PreDecompress)  = 1
CanDo(0x06 BandDecompress) = 1
```

**Critical detail: `RegisterComponent` wants the RoutineDescriptor, not
the TVector.** It is declared to take a `ComponentRoutineUPP`, and on
classic PowerPC a UPP is a pointer to a RoutineDescriptor. Handing it the
bare TVector hangs the machine mid-call -- discovered the hard way.
`ComponentDispatchHelper.c` generates both symbols; use `...DispatchRD`.

The registrar must stay resident: a component registered by an
application is unregistered when that application quits, and the code
fragment unloads with it. Parking in `getchar()` works because SIOUX
pumps events while blocked.

## Bug 2 -- the blit ignored dstRect and bounds

One log line from a real `BandDecompress` call:

```
dstPixMap.pixelSize   32
dstPixMap.rowBytes    5120                 <- 1280 px * 4: a whole screen
dstPixMap.bounds      (-24,-50)-(1256,718)
dstRect               (88,24)-(264,168)    <- 176x144, our frame
glob->frameWidth      0                    <- !!
```

Two things wrong:

1. **The destination is not the top-left of the PixMap.** QuickTime hands
   a codec a PixMap that may be the entire screen backing store, plus a
   rectangle saying where the frame goes. Both the rectangle and the
   pixel data are in the pixmap's own coordinate system, whose origin is
   `bounds.topLeft` -- routinely negative. The first pixel is at

   ```
   baseAddr + (dstRect.top  - bounds.top ) * rowBytes
            + (dstRect.left - bounds.left) * 4
   ```

   not `baseAddr`.

2. **Geometry cached in per-instance globals is not reliable.**
   QuickTime opens several instances while negotiating, and the instance
   that receives `BandDecompress` is not always the one `PreDecompress`
   ran on. `frameWidth` read back as 0 on the call that was meant to
   draw -- and with width zero the blit loops run zero times.

   The blit now asks the decoder every frame (`h264bsdPicWidth` /
   `h264bsdPicHeight`, plus `h264bsdCroppingParams`). Given a valid
   decoder and the params block it cannot be wrong about geometry,
   whichever instance it lands on.

## Bug 3 -- h264bsdDecode consumes ONE NAL unit per call

This was the one that produced the actual bad picture, and it spent a
long time disguised as a colour-conversion bug.

```c
/* WRONG */
decodeStatus = h264bsdDecode(glob->h264, annexB, annexBSize, 0, &readBytes);
```

A QuickTime sample is not one NAL unit. For ffmpeg-produced clips each
sample is an SEI followed by the slice. That single call consumed the SEI
and stopped; the picture was never decoded. The API returns `readBytes`
precisely so the caller can advance and call again.

Correct form:

```c
u32 offset = 0;
while (offset < annexBSize) {
    readBytes = 0;
    status = h264bsdDecode(glob->h264, annexB + offset, annexBSize - offset,
                           0, &readBytes);
    if (status == H264BSD_ERROR || status == H264BSD_MEMALLOC_ERROR) return err;
    if (readBytes == 0) break;          /* no progress -- never spin */
    offset += readBytes;
    if (status == H264BSD_PIC_RDY) DrainOutputPictures(...);
}
```

The `readBytes == 0` guard is a hard requirement: an infinite loop inside
a QuickTime callback on Mac OS 9 is a hung machine and a power cycle.
Drain inside the loop, because a multi-slice sample can complete a
picture partway through the buffer.

### Why it looked like a colour bug

- Frame 0 came out flat grey (Y=Cb=Cr=128, h264bsd's initialised buffer)
  because the IDR never decoded.
- Every later frame predicts from earlier ones, so the damage accumulated.
- The frame counter and moving diagonal still rendered legibly -- those
  are intra-coded macroblocks redrawn from scratch each frame, sitting on
  a background that had never been decoded at all.
- Net effect: correct-looking foreground on a grey field with blocky
  colour noise. Reads as "chroma is wrong". It was not.

## The technique that cracked it

**Dump the decoder's raw output to the shared volume and render it on the
host with ffmpeg.**

The component writes the planar YUV it receives to `share:h264pic0.yuv`
and `share:h264piclast.yuv` using File Manager calls only (no stdio, no
malloc -- a logger that depends on the runtime it is debugging is
useless). On the host:

```
ffmpeg -f rawvideo -pix_fmt yuv420p -s 176x144 -i h264piclast.yuv out.png
ffmpeg -i h264test.mov -vf "select=eq(n\,14)" -frames:v 1 ref.png
```

Two facts fell out immediately:

1. The dump rendered **identically to the Mac screen** -> the blit was
   faithfully drawing what it was given; the decoder's output was already
   wrong. That eliminated the entire colour-conversion theory in one step.
2. **Frame 0 was uniform grey** -> the IDR never decoded, which pointed
   straight at the decode loop.

Generalisable: when a picture is wrong, get the intermediate buffer out to
where known-good tooling can look at it, before theorising about the
stage you can see.

## Other hard-won details from this session

- `kImportLibraryCFrag = 0`, `kApplicationCFrag = 1`. Zero is the GOOD
  value in a `'cfrg'` for a component. Having these backwards cost a
  session chasing a non-bug.
- `kComponentGetPublicResourceSelect = -10` in this SDK, not -9. The
  negative-selector block in `ComponentDispatchHelper.c` bounds-checks
  only the LOW end -- if `SelOffset` exceeds the number of entries you
  list, `Open` (selector -1) indexes off the end of the array silently.
  `H264CodecDispatch.h` now pins offset 12 with 12 entries, which is
  correct regardless of what the headers say.
- `CFragSymbolClass`: 0 = code, 1 = data, 2 = TVector, 3 = TOC, 4 = glue.
  The dispatcher resolves as class 2, its RoutineDescriptor as class 1.
- Allocating the h264bsd decoder in `Open` turns every allocation failure
  into an invisible NULL from `OpenComponent`. Build it lazily in
  `PreDecompress`, where a real `OSErr` can be returned.
- UTM's guest cursor drifts relative to the host pointer and must be
  recalibrated by moving to a known point and locating the drawn arrow.
  Classic Mac menus need press-hold-move-release, not a click.

## State at end of session 7

Working: `H264 Decoder` (CFM shared library, no `'thng'`) in Extensions,
plus the registrar built from `ComponentProbe.c`. Run the registrar,
leave it running, open an H.264 `.mov` in QuickTime Player.

Next, none of it blocking:

- strip the per-frame YUV dumps and logging (they write to the shared
  volume every frame and dominate the time)
- make the registrar a faceless background app in Startup Items so it
  runs at boot with no console window
- measure what real footage this hardware can actually keep up with

---

# Session 8 -- diagnostics off, and a performance ladder

## 1. The quiet build is confirmed good

`H264Codec.c` now carries a master switch at the top:

```c
#define kH264Diagnostics 0
```

With it at 0, `LogAppend()` and `DumpBytesToShare()` early-return, so the
per-frame 38 KB YUV dump and the `FSpCreate`/`FSWrite`/`FlushVol` log
traffic to the VirtFS share compile away to nothing. That traffic was not
a rounding error -- it was the dominant cost of playback.

Verified rather than assumed:

- rebuilt in CodeWarrior (315K/58K, no errors), installed into
  `System Folder:Extensions`, registrar re-run:
  `OpenComponent -> 0x00920016`, `Version = 0x00010000`,
  `CanDo(0x00/0x05/0x06) = 1/1/1`
- `h264log.txt` on the share kept its old timestamp through a full
  playback, i.e. nothing was written -- the switch really is off
- frame 0 renders correctly (`00:00:00.000`, counter `0`), and frame 14
  still matches ffmpeg's reference render pixel for pixel

Frame 0 mattering is worth restating: in the broken build frame 0 was
uniform grey, because `h264bsdDecode` was being called once per sample
instead of once per NAL unit and the IDR never completed. Seeing a
correct frame 0 is the single cheapest confirmation that the NAL loop in
`BandDecompress` is right.

## 2. How to measure playback speed without instrumenting the codec

Generate clips with the timecode and frame number burned into the image:

```sh
ffmpeg -f lavfi -i "testsrc2=size=${W}x${H}:rate=${FPS}:duration=${D}" \
  -vf "drawtext=fontfile=DejaVuSansMono-Bold.ttf:\
text='%{pts\:hms} f%{n}':x=4:y=4:fontsize=${FS}:\
fontcolor=white:box=1:boxcolor=black@0.8" \
  -c:v libx264 -profile:v baseline -level 3.0 -refs 1 -bf 0 -g 30 \
  -crf 26 -pix_fmt yuv420p -f mov out.mov
```

`-profile:v baseline -refs 1 -bf 0` is not decoration: h264bsd is
Baseline only, no B-frames. The reference clip `h264test.mov` probes as
Constrained Baseline / `avc1` / `nal_length_size=4`, and the generated
clips match it.

Then play, and sample the burned-in timestamp at known wall-clock
intervals. Movie-seconds advanced per wall-second is the speed ratio,
directly, with no code change and no logging.

This works because QuickTime 6 on this path runs SLOW rather than
dropping frames -- established by the diagnostics-on build, where the
3-second QCIF clip took roughly 25 seconds instead of finishing on time
with most frames skipped.

Copying clips in: write them to the host share folder, then force the
guest Finder to re-read the volume by closing the window and reopening it
from the desktop icon. Mac OS 9 does not notice files that appear on a
VirtFS share behind its back.

## 3. The ladder

All clips Constrained Baseline, 1 reference frame, no B-frames.
"MB/s" is macroblocks per second = (W/16)x(H/16)xfps.

| clip          | size      | fps | MB/s    | result            |
|---------------|-----------|-----|---------|-------------------|
| h264test.mov  | 176x144   |  5  |     495 | real time         |
| p_qcif15.mov  | 176x144   | 15  |   1,485 | real time         |
| p_qvga10.mov  | 320x240   | 10  |   3,000 | real time         |
| p_qvga15.mov  | 320x240   | 15  |   4,500 | real time         |
| p_vga10.mov   | 640x480   | 10  |  12,000 | real time         |
| p_vga30.mov   | 640x480   | 30  |  36,000 | real time         |
| p_540_30.mov  | 960x540   | 30  |  61,200 | real time         |
| p_540_60.mov  | 960x540   | 60  | 122,400 | real time         |
| p_1080_30.mov | 1920x1080 | 30  | 244,800 | window stays white|

Sample measurements, movie-seconds advanced per 5 s of wall clock:

- 640x480@30: 5.900 -> 11.033 -> 14.967 (end). +5.13, +3.93(end)
- 960x540@30: 5.933 -> 11.033 -> 14.967 (end). +5.10, +3.93(end)
- 960x540@60: 6.000 -> 11.050 -> 11.983 (end). +5.05

Every one of them tracks the wall clock to within measurement noise.

## 4. What the ladder does and does not prove

It is a measurement of THIS machine: a QEMU-emulated Mac99 PowerMac,
512 MB, running on an Apple Silicon MacBook Air. It is not a measurement
of a real G3 or G4. The emulated CPU is evidently fast -- 960x540@60 is
roughly 1080p30-class decoding work -- so what the ladder really says is:

- the decoder, the ICM plumbing and the YUV->RGB blit have no pathological
  cost that shows up as the frame size grows
- nothing in the path falls over between QCIF and 960x540
- the ceiling on this host is above 122,000 macroblocks/sec, i.e. we never
  found it

On period hardware the number will be far lower, and the interesting
question there is different: how much can be won back with AltiVec in the
IDCT, the deblocking filter and the colour conversion. None of that is
written yet -- the blit is a scalar table-driven BT.601 conversion.

## 5. The one real failure: 1920x1080

`p_1080_30.mov` opens, the window is created at full size (much larger
than the 1024x768 guest screen), and nothing is ever drawn -- the content
area stays white through the whole movie. With diagnostics compiled out
there is no evidence on disk saying why.

Candidates, in the order worth testing:

1. an allocation failure inside `EnsureDecoder` or the h264bsd DPB. A
   1920x1080 4:2:0 frame is ~3.1 MB; with the DPB and our own scratch
   buffer that is several megabytes out of QuickTime Player's application
   heap, which is not generous by default. Raising the app's memory
   partition in Get Info is a one-minute test.
2. `annexBScratch` sizing -- it is grown per sample, and a 1080p IDR is a
   much bigger sample than anything tested so far.
3. an h264bsd size or level limit rejecting the SPS.
4. QuickTime declining to build a GWorld larger than the screen, in which
   case our code is never called at all and the codec is blameless.

Test 4 first, because it is free: shrink the window (or set the movie to
half size) before pressing play. If the picture appears, it was never a
decoder problem.

## 6. Practical notes on driving the VM

- The UTM guest cursor sits about +47 px in x from where the host pointer
  is commanded, in this window geometry. Calibrate every session: move to
  a known point, screenshot, find the drawn arrow, subtract. Clicking
  "the button you can see" without that correction lands one button to
  the right -- which is how a "go to start" click turned into a
  step-backward, and a scrubber drag landed in the Finder window behind.
- The menu bar row is effectively unreachable while UTM is full screen;
  the host menu bar intercepts the pointer. Work through windows,
  type-select and the keyboard instead.
- Finder type-select cannot type `_` (it needs shift, and modifiers do
  not pass through). Sort the window by Date Modified instead and take
  the newest rows.
- The spacebar plays and pauses in QuickTime Player, which is the only way
  to start a movie whose controller is below the bottom of the screen.
- If a stray press lands on the host menu bar, guest input stops
  responding entirely. Escape clears it.

## State at end of session 8

Working and quiet: `H264 Decoder` in Extensions, registrar run once,
QuickTime plays H.264 up to at least 960x540@60 in real time on the
emulated machine.

Next:

- find out why 1920x1080 draws nothing (start with the window-size test)
- make the registrar a faceless background app in Startup Items
- delete the now-unused `H264CodecTinyEntry` / `H264CodecTinyEntryRD`
  diagnostic stub from `H264Codec.c`
- AltiVec, when there is a real machine to measure it on

---

# Session 8b -- streaming, and what late-gen G3 actually needs

## 1. Streaming: the chain works, the source did not

Test: S60Tube (s60tube.io.vn), a plain-HTTP YouTube frontend, in Classilla
on the guest. The site loads and searches fine. Each video detail page
offers a streaming URL of the form

    http://s60tube.io.vn/videoplayback?v=<id>

and a 360p MP4 download. YouTube's 360p MP4 is itag 18 -- H.264
Constrained Baseline plus AAC-LC -- which is exactly our decoder's format,
so this is the right vehicle.

QuickTime Player 6 accepted it through File -> Open URL (Cmd+U): it
resolved DNS, connected, titled its window `videoplayback`, and showed
"Loading...". Then: "Couldn't open the file because the file was not
found."

The cause is not ours. Pasting the same URL into Classilla returns a
plain-text body:

    error code: 1033

for two independent video IDs (`YE7VzlLtp-4` and `aqz-KE-bpKQ`). The
site's extraction backend is failing server-side -- the chronic condition
of YouTube proxy frontends. QuickTime was reporting "not found" because it
received a 20-byte error string where a movie should have been.

So the QuickTime end of the streaming path is proven functional. Nothing
in the codec was exercised, because no video data ever arrived.

## 2. QuickTime 6 has a real HTTP proxy setting

QuickTime Settings control panel -> pane popup -> **Streaming Proxy**:

  - SOCKS Server + Port ID
  - **HTTP Proxy Server + Port ID**
  - HTTP Proxy Options... button
  - RTSP Proxy Server + Port ID
  - "Bypass proxy servers for these hosts" list
  - "Use System Settings" checkbox (ticked by default -- this is why the
    fields are dimmed; untick to enter values manually)

This matters because QuickTime 6 has no TLS whatsoever and never will.
Any modern source is HTTPS, so a TLS-terminating proxy is mandatory, and
this pane is the supported way to put one in front of QuickTime without
touching our code.

Also set this session: Connection Speed was **56 Kbps Modem/ISDN**, which
governs both alternate-stream selection and pre-roll buffering. Changed to
**Intranet/LAN**.

## 3. The proxy has to be native -- and what it must do

Project constraint: everything ships on one Mac OS 9 machine. No VM, no
second computer. So the proxy is an OS 9 binary.

Two routes:

  - Crypto Ancienne (`carl`). 2.2 added classic Mac OS/MPW support. The
    open question is whether the classic port includes the listening half
    -- the Unix build leans on `micro_inetd`, and OS 9 has no inetd, so
    the Open Transport listener may be ours to write regardless.
  - Write one with CodeWarrior 8 linking **macTLS**, the TLS stack from
    MacSurf: hand-written TLS 1.3 (RFC 8446, X25519 and multi-curve
    ECDHE, ChaCha20-Poly1305 and AES-128-GCM), TLS 1.2 fallback, full
    Mozilla CA bundle, own entropy source, native on the classic Mac, no
    helper. MacSurf is a Carbon CFM app built with CodeWarrior 8 -- our
    exact toolchain. Licence is GPLv2 + OpenSSL exception, which is fine
    for a separate proxy binary and does not touch our BSD-licensed codec
    as long as they stay separate executables.

MacSurf itself cannot serve as the proxy: its own architecture docs state
the Go proxy was retired around 2026-05-25 when native macTLS replaced it,
and it exposes no proxy for other applications.

Whatever we build, it must do four QuickTime-specific things:

  1. silently upgrade `http://` requests to `https://` so QuickTime never
     meets TLS
  2. rewrite `Location:` headers from https back to http, or QuickTime
     follows a redirect into a protocol it cannot speak (Invidious
     redirects to googlevideo, so this is mandatory)
  3. de-chunk -- QuickTime 6's HTTP client is HTTP/1.0-era
  4. supply `Content-Length`, without which progressive download misbehaves

Note what a proxy does NOT solve: extracting a playable YouTube URL needs
YouTube's obfuscated signature and `n`-parameter JavaScript, which is not
happening on OS 9. Something server-side must always do that -- S60Tube,
an Invidious instance, or similar. The proxy widens the field from
"plain-HTTP sites, of which there is roughly one" to "any HTTPS frontend".

## 4. Late-gen G3 feasibility (Pismo 500, iBook 500-900)

Target hardware has **no AltiVec**, so there is no vector rescue. Work is
macroblocks per second, (W/16) x (H/16) x fps:

| stream           | size      | fps | MB/s   |
|------------------|-----------|-----|--------|
| itag 36          | 320x180   | 30  |  7,200 |
| (our test clip)  | 320x240   | 15  |  4,500 |
| itag 18          | 640x360   | 30  | 27,600 |

Honest read: **320x180-class (itag 36) is the realistic streaming target
for a late-gen G3. 640x360@30 is roughly 4x too much.** A 900 MHz iBook
might manage 640x360 at halved frame rate; a 500 MHz Pismo will not.

The session-8 ladder (real time to 960x540@60) was measured on a
QEMU-emulated PowerMac on Apple Silicon and must not be read as a
prediction for period hardware.

## 5. The highest-leverage change for G3: stop converting colour

Confirmed present in our SDK (Universal Interfaces 3.4.1), `ImageCodec.h`:

```c
struct CodecCapabilities {
  long    flags;
  short   wantedPixelSize;
  ...
  /* The following fields only exist for QuickTime 2.5 and greater */
  OSType  **wantedDestinationPixelTypes;  /* Handle to 0-terminated list of OSTypes */
  ...
};
```

So a decompressor can tell the ICM which destination pixel formats it
would prefer, by filling this in during `PreDecompress`. And
`ImageCompression.h` defines the YUV types:

```c
k422YpCbCr8CodecType     = FOUR_CHAR_CODE('2vuy')   /* packed 4:2:2 */
kYUV420CodecType         = FOUR_CHAR_CODE('y420')   /* planar 4:2:0 */
kComponentVideoCodecType = FOUR_CHAR_CODE('yuv2')
```

Today `CopyPlanarYUVToPixMap` does a scalar table-driven BT.601
conversion to 32-bit xRGB. At 640x360x30 that is 6.9 Mpixel/s, each
needing lookups, adds, clamps and a 4-byte store -- plausibly 20-35% of a
500 MHz G3 spent on colour alone, before any decoding.

Advertising `'2vuy'` would:

  - replace the colour matrix with a much cheaper chroma upsample and
    interleave (no per-pixel multiply)
  - on the Rage 128 / Radeon in these machines, potentially land the
    destination in a **hardware YUV overlay**, which also makes scaling to
    full screen free

`'y420'` would be better still -- h264bsd's output is already planar
4:2:0, so the blit could become close to a plane copy -- but whether
QuickTime 6 accepts it as a GWorld destination format is unverified.
`'2vuy'` is the pragmatic target; keep the existing 32-bit RGB path as the
fallback for when the ICM declines.

## 6. Driving the VM: further notes

  - `cmd+U`, `cmd+C`, `cmd+V`, `cmd+A` all reach the guest. This is a
    correction to session 7's note -- modifiers DO work, which makes
    menus reachable by shortcut even though the menu bar itself is not
    clickable under UTM full screen.
  - **`cmd+O` does NOT reach the guest.** UTM swallows it on the host and
    opens its Shared Directory picker. Use a double-click instead.
  - `computer_type` remains broken (types a run of `a`). Individual key
    presses work. Key names: `period`, `space`, `shift+semicolon` (`:`)
    work; `colon` and `slash` do not. Clipboard copy/paste between guest
    applications is by far the most reliable way to move text.
  - The UTM display window closes on its own periodically. Always
    screenshot to confirm it is open before sending guest input, or the
    clicks land on UTM's own window.
  - Classic popup menus: release any held button first, then
    mouse_move -> mouse_move -> mouse_down, or the pointer does not
    actually travel and the menu never opens.

---

# Session 9 -- QuickTime 6, and the container wall

## 1. The machine was on QuickTime 5, not 6

Discovered by accident: QuickTime Player's upsell banner read "QuickTime 5
Pro". Every result in sessions 1-8 was therefore measured against
QuickTime 5's Component Manager and ICM, not 6.

The maintainer installed QuickTime 6.0.3 (Macintosh Garden, abandonware). The
component survived intact and re-registers identically:

    RegisterComponent (global) -> 0x0002028B
    After:  'imdc'/'avc1' registered = 1
    OpenComponent              -> 0x0060014
    CallComponentVersion       = 0x00010000
    CanDo(0x00 GetCodecInfo)   = 1
    CanDo(0x05 PreDecompress)  = 1
    CanDo(0x06 BandDecompress) = 1

So the programmatic-registration approach is version-independent across
QuickTime 5 and 6. Good to know, and one less thing to worry about.

The upgrade also matters on its own terms: **QuickTime 5 cannot parse MP4
at all.** ISO/MPEG-4 container support arrived in QuickTime 6. Our test
clips always worked because ffmpeg wrote them as `.mov` with the `qt `
brand. A YouTube stream is an MP4, so on QuickTime 5 it could never have
worked regardless of the decoder.

## 2. S60Tube is still broken where it counts

A week later: the site loads, search works, video detail pages render and
show a streaming URL and a 360p MP4. But the stream endpoint still
returns, as a plain-text body,

    error code: 1033

for every video tried. QuickTime reports "Couldn't open the file because
the file was not found", which is simply what it says when handed a
20-byte error string instead of a movie.

Note for future sessions: the browsable front end being healthy is not
evidence that extraction is working. Test `/videoplayback?v=<id>` in
Classilla directly before concluding anything.

## 3. The real finding: MP4 fails, the same bitstream in MOV works

Built two files from one encode, Constrained Baseline, 1 ref, no
B-frames, faststart, both carrying an `avcC`:

| file            | brand  | result                                   |
|-----------------|--------|------------------------------------------|
| itag36.mp4      | `isom` | **fails at open, error -8971**           |
| itag36mov.mov   | `qt  ` | opens and plays perfectly                |

Identical video bitstream. Verified byte-wise that both contain `avc1`
and `avcC` atoms and that both are faststart. The only difference is the
container.

**-8971 is `codecExtensionNotFoundErr`** (the same numeric value as
`scTypeNotFoundErr`). That is precisely what
`GetImageDescriptionExtension()` returns when the requested extension is
not present on the image description.

The mechanism, as best it can be reasoned without diagnostics on:

  - QuickTime's `.mov` parser copies **all** sample-description
    extensions into the ImageDescription verbatim, including ones it does
    not understand. That is why `avcC` reaches us and why every test clip
    to date has worked.
  - QuickTime 6's **MPEG-4 importer** knows `mp4v` and `mp4a` sample
    entries. H.264 postdates it entirely -- `avc1` arrived with QuickTime
    7. Faced with an `avc1` sample entry it evidently builds an
    ImageDescription but does not carry the `avcC` across.
  - Our `PreDecompress` then asks for `avcC`, does not find it, and
    returns -8971 straight up the stack.

That last step is a hypothesis, not yet confirmed: the error could in
principle come from QuickTime's own importer rather than from our code.
Confirming it is cheap -- rebuild with `kH264Diagnostics 1` and see
whether the log records `PreDecompress enter` before the failure.

## 4. Why this matters more than the S60Tube outage

S60Tube serves itag 18 -- a 360p **MP4**. So even with the extraction
backend healthy, the stream would have hit -8971 at open, exactly as the
local `itag36.mp4` does. The streaming goal was never one working click
away; there is a container problem sitting behind the source problem, and
it would have surfaced the moment the source came back.

Better to have found it with a local file we control than over a flaky
network path.

## 5. Ways out, cheapest first

1. **Make `PreDecompress` tolerant of a missing `avcC`.** If the
   extension is absent, assume `nalLengthSize = 4` (effectively
   universal) and recover SPS/PPS from the bitstream by scanning the
   first sample for them. Works only if the stream carries SPS/PPS
   in-band; ffmpeg-muxed MP4s normally do not, but many live/streaming
   encoders do repeat them at every IDR. Cheap to try, and harmless as a
   fallback path even if it does not always fire.
2. **Defer decoder setup to `BandDecompress`.** Rather than failing in
   `PreDecompress`, succeed and build the h264bsd instance lazily on the
   first sample, feeding whatever SPS/PPS turn up. Fits the existing
   `EnsureDecoder` shape.
3. **Write a movie importer component** (`'eat '`) for MP4 that builds
   the ImageDescription properly, `avcC` and all. The correct fix, fully
   self-contained on the Mac, and a substantial piece of work.
4. **Fold an MP4->MOV remux into the local proxy.** When the native
   TLS-terminating proxy gets built (session 8b), have it rewrite the
   container on the way through. One local binary would then solve both
   the TLS problem and the container problem. Elegant, and still
   satisfies "runs on one OS 9 machine".

Options 1 and 2 are code we already own and should be tried first.

## 6. Performance, itag 36 shape

`itag36mov.mov` -- 320x180 @ 30fps, the realistic late-G3 target -- played
the full 20 seconds in real time: frame 178 at 6 s of wall clock, reaching
frame 599 of 600 on schedule. 7,200 macroblocks/sec. Emulated machine, so
the same caveat as session 8 applies.

## 7. VM-driving notes

  - UTM opened the VM **windowed** this time rather than full screen, and
    option-clicking the green zoom button maximises without going
    full screen. In that state the guest menu bar is directly clickable
    and **the pointer offset is zero** -- both of the session-7/8
    annoyances disappear. Always prefer windowed-maximised.
  - `.mp4` now maps to QuickTime in the Finder; the QuickTime 6 installer
    added the extension mapping.
  - If the pointer stops tracking, consider that the human may simply have
    taken the mouse. Check before assuming the bridge is stuck.

---

# Session 9b -- MP4 SOLVED: a four-byte patch

## The result

A standard MP4 carrying H.264 now plays on Mac OS 9 through our codec.
The fix is two things, both trivial:

  1. the file must be **routed to QuickTime's MOV parser**, which on disk
     means a `.mov` extension (HFS type `MooV`); and
  2. the `ftyp` **major brand must read `qt  `** -- four bytes at file
     offset 8.

Neither alone is enough. Both together work, at every size tested, with
audio, in real time.

## The ladder that got there

Each row is the same H.264 bitstream; only the container wrapper differs.

| probe         | change                                          | result                                   |
|---------------|-------------------------------------------------|------------------------------------------|
| `itag36.mp4`  | none -- plain MP4, `.mp4`                        | fails at open, **-8971**                 |
| `t1.mov`      | byte-identical MP4, renamed `.mov`               | "not a file that QuickTime understands"  |
| `t2.mov`      | major + first compatible brand -> `qt  `, `.mov` | **plays**                                |
| `t4.mov`      | **major brand only** -> `qt  `, `.mov`, 640x360  | **plays, real time**                     |
| `t5.mov`      | as t4 plus an AAC audio track                    | **plays, real time, audio intact**       |
| `t6.mp4`      | major brand -> `qt  ` but kept `.mp4`            | fails at open, **-8971**                 |

Reading the rows against each other is what pins the mechanism down:

  - `t1` vs `itag36.mp4`: changing only the extension changes the error
    message. So the **extension chooses the importer**. `.mp4` goes to the
    MPEG-4 importer; `.mov` goes to the MOV parser.
  - `t1` vs `t2`: with `.mov` in both cases, the MOV parser rejects a
    leading `ftyp` whose brand is `isom` and accepts one whose brand is
    `qt  `. So the **brand is what the MOV parser gates on**.
  - `t6` vs `t4`: patching the brand while leaving `.mp4` in place changes
    nothing -- still -8971. Confirms the extension, not the brand, does
    the routing.

And the reason the MOV path works at all: QuickTime's MOV parser copies
**every** sample-description extension into the ImageDescription verbatim,
including ones it does not understand. So `avcC` survives, our
`PreDecompress` finds it, and everything downstream is the code we already
had. The MPEG-4 importer, which predates H.264 entirely, drops it.

Worth noting `t4` is the **minimal** patch: four bytes. The compatible
brands can stay `isom iso2 avc1 mp41` untouched.

## What this means for streaming

S60Tube serves itag 18 -- 640x360 Constrained Baseline MP4 with AAC. That
is exactly `t5`, which plays in real time. So the decoder side of the
streaming goal is now proven end to end; only the transport remains.

For a stream there is no file extension, so the routing question becomes:
what does QuickTime key off when opening a URL? Almost certainly the
server's `Content-Type`. The untested hypothesis is that a proxy serving
**`Content-Type: video/quicktime`** while patching bytes 8-11 to `qt  `
would be enough. That is a cheap thing for a proxy to do -- the `ftyp` is
the first 32 bytes of the response, so it is a fixed-offset patch on the
very first packet, no buffering required.

This folds neatly into the native TLS proxy planned in session 8b: one
local binary that terminates TLS, patches four bytes, and sets one header.

## Caveats

  - The Content-Type hypothesis is **not yet tested** -- we had no working
    stream to test against. It is the first thing to try when S60Tube's
    extraction backend recovers.
  - All timings remain emulated-machine numbers.
  - A real YouTube MP4 may carry a `dash`/`mp42` major brand rather than
    `isom`; the patch overwrites whatever is there, so that should not
    matter, but it is unverified against a genuine YouTube file.

## Performance at the real target sizes

| clip                 | size     | fps | MB/s   | result    |
|----------------------|----------|-----|--------|-----------|
| `t2.mov` (itag 36)   | 320x180  | 30  |  7,200 | real time |
| `t4.mov` (itag 18)   | 640x360  | 30  | 27,600 | real time |
| `t5.mov` (+ AAC)     | 640x360  | 30  | 27,600 | real time |

## Practical note

QuickTime Player's **File -> Open Movie in New Player...** is far more
reliable for driving tests than fighting the Finder: it remembers the last
folder, supports type-select, and avoids the desktop-icon clicking that
kept missing. Note `cmd+O` cannot be used for it -- UTM swallows that on
the host -- so drive the File menu directly, which works in
windowed-maximised mode.

---

# Session 9c -- tooling, and reading the macTLS source

Written while the machine was asleep, so none of this is hardware-tested.
Everything below is staged on the share.

## 1. `MP4Fix.c` -- the four-byte patcher

Console app, same build recipe as the registrar. Pick a file; it verifies
the file really starts with an `ftyp` box, writes `qt  ` over the major
brand at offset 8, sets the Finder type to `MooV`/`TVOD`, and renames
`.mp4` to `.mov`.

Type *and* rename are both done on purpose, because which one matters
depends on where the file lives: on a real HFS volume the Finder type is
what counts, while on the VirtFS share the type is synthesised from the
extension on every read, so there the rename is what counts.

It refuses to touch anything not starting with `ftyp`, so it cannot
scribble on a normal `.mov` (which begins `moov` or `mdat` and needs no
patching anyway).

## 2. `OTProbe.c` -- the experiment that decides the proxy plan

macTLS's `docs/carbon-ot-passive-bind-finding.md` is a 14-round
investigation concluding that a **Carbon** CFM app cannot `OTBind` to a
caller-chosen address -- `-3150` for every combination of port, host,
qlen, protocol stack and sync/async. Their original architecture was
*exactly ours* (a local proxy on `127.0.0.1:8765` that browsers point
at) and they abandoned it and shipped as a library instead.

But their own research contains the escape hatch. Apple's pre-Carbon
HTTP Server sample binds successfully, and they explain why: it uses
plain `OTAsyncOpenEndpoint` and "doesn't go through CarbonLib's OT
wrapper". They also record that plain `OTOpenEndpoint` is not exported by
CarbonLib at all.

**We are not Carbon.** The decompressor is a classic CFM shared library
and the registrar is a plain classic PPC console app. MacSurf is
committed to Carbon and so could never run this test; for us the question
is open. `OTProbe.c` runs their matrix on the classic path:

| case | bind | expected on Carbon |
|------|------|--------------------|
| 1 | `OTBind(NULL, NULL)` | OK |
| 2 | `addr = NULL, qlen = 1` | OK, OT-chosen port |
| 3 | `127.0.0.1:8080, qlen = 1` | -3150 |
| 4 | `INADDR_ANY:8080, qlen = 1` | -3150 |
| 5 | `INADDR_ANY:0, qlen = 1` | sanity |

Cases 3 and 4 decide it. The program prints whether it was compiled
Carbon or classic, so the output itself proves the test was valid --
building it against CarbonLib would make the result meaningless.

## 3. Corrections to session 8b

Two things I had wrong, both from the website rather than the source:

- **macTLS is MIT, not GPLv2.** The repo-root `LICENSE` is NetSurf's
  GPLv2 -- that is the browser engine. macTLS's own README states MIT,
  with BearSSL vendored under its own MIT licence and nothing else
  statically linked. There is no licensing constraint on using it.
- **It is not purely hand-written.** It is ~20,600 lines of OS 9 code
  over vendored BearSSL (~77,400 lines) for the primitives. The TLS 1.3
  handshake, key schedule and record layer *are* hand-written (adapted
  from the Certainly project, MIT) because BearSSL has no 1.3; BearSSL
  drives the 1.2 fallback.

Neither changes the plan; both make it easier.

## 4. The Carbon surface in macTLS is nineteen call sites

Across the whole `os9/` tree, exactly two Carbon-only functions are used:

    OTOpenEndpointInContext        14 call sites
    OTAsyncOpenEndpointInContext    5 call sites

and there are no `TARGET_API_MAC_CARBON` conditionals anywhere. The
classic equivalents take identical arguments minus a trailing context
pointer.

So `ostls_classic_shim.h` defines the Carbon names as macros that discard
that argument and call the classic functions. **macTLS then compiles
unmodified into a classic non-Carbon application** -- add the shim as the
project's prefix file. That keeps macTLS a clean vendored drop we can
re-sync when upstream moves, rather than a fork.

The one thing the caller must supply is a global `g_ostls_ot_context`.
Under classic OT it is meaningless, but the symbol has to exist to link;
define it as NULL and use `InitOpenTransport()` / `CloseOpenTransport()`.

## 5. What macTLS gives us, concretely

- `OSTLS_Fetch()` -- blocking one-shot. 60-second deadline, writes into a
  fixed caller buffer. Fine for an API call, wrong for a 30 MB video.
- `OSTLS_New / Start / Pump / Write / Read / Close / Dispose` -- async,
  socket-like, caller drives with `Pump(conn, 4..8, &ev)`. This is the
  one to use for anything large; it streams, so it can go straight to
  disk.
- HTTP helpers already written: `OSTLS_HTTP_FormatGet`,
  `OSTLS_HTTP_FormatPost`, and a chunked-transfer decoder
  (`OSTLS_HTTP_ChunkDecoderInit` / `Process`). Redirects are explicitly
  the caller's job.
- Verified on a real Power Macintosh G3 running OS 9.1.

**Root anchors are worth checking before relying on this.** The v0.1 docs
list only 10 embedded roots (Amazon, DigiCert G2/G3, Google GTS R1-R4,
Let's Encrypt ISRG X1/X2, Starfield G2) while the website claims 121 from
the Mozilla bundle -- presumably the newer 1.3 build. For us the short
list happens to be exactly right: googlevideo is Google Trust Services
and Invidious instances are almost always Let's Encrypt. But confirm
which set ships in whatever commit we vendor.

## 6. The fallback is better than the ephemeral-port hack

If `OTProbe` says no, do not reach for "let OT pick a port and retype it
into the control panel each launch". There is a cleaner design that
sidesteps binding entirely:

**Fetch-to-disk.** An app that takes a URL, fetches it over HTTPS with
the async API, patches the four bytes on the way past, writes a `.mov`,
and hands it to QuickTime. No listener, no bind, no Carbon limitation --
**outbound only, which is the path macTLS has already verified on a G3.**

Not live streaming, but "paste a link, get a playable movie, no second
computer" is arguably the actual goal, and it is reachable entirely with
code already known to work. It is also strictly more certain than the
proxy, because it does not depend on the `OTProbe` result at all.

## 7. Morning test kit

Four files on the share, all deliberately **unpatched**, all 640x360
Constrained Baseline + AAC (the itag-18 shape S60Tube serves):

| file | tests | note |
|------|-------|------|
| `y_isom.mp4` | the brand already proven | baseline |
| `y_mp42.mp4` | `mp42` brand | common on real YouTube files |
| `y_dash.mp4` | `dash` brand | also seen in the wild |
| `y_noFS.mp4` | **`moov` at the end**, not faststart | the interesting one |

The first three check that the patch generalises beyond `isom` -- it
should, since it overwrites whatever is there, but that is unverified.

`y_noFS.mp4` matters more than it looks. Some real YouTube progressive
files put `moov` after `mdat`. Locally QuickTime can scan backwards for
it, so it may well play -- but if it does *not*, a streaming proxy would
have to buffer the entire file before playback could start, which would
change the proxy design substantially. Cheap to find out.

---

# Session 9d -- three programs written, logic host-verified

Still no hardware access, so nothing here is compiled. What follows is
what was written and what was actually proven about it.

## Programs now staged

| file | what it is | depends on |
|------|-----------|------------|
| `MP4Fix.c` | patches a downloaded MP4 in place | nothing |
| `OTProbe.c` | settles the passive-bind question | nothing |
| `QTProxy.c` | local HTTP proxy, patches streams in flight | nothing |
| `GetMovie.c` | HTTPS fetch-to-disk, patches on the way in | macTLS |
| `GetMovie_prefix.h` | the project prefix, in the right order | -- |
| `ostls_classic_shim.h` | lets macTLS build non-Carbon | -- |

Only `GetMovie.c` needs macTLS. The other three link nothing but the
system libraries, which is deliberate: the listener half and the patch
logic can both be proven before any cryptography is introduced.

## `QTProxy` does not depend on the probe result

Worth recording because it changes the plan's shape. The proxy tries to
bind the port we want; if Open Transport refuses, it binds an
OT-assigned one and prints it for the user to type into the QuickTime
Streaming Proxy pane. So:

  - classic path can bind  -> configure QuickTime once and forget it
  - classic path cannot    -> retype a port number each launch

Clunky in the second case, working in both. And because it reports which
happened, running it is a second independent reading of the same question
`OTProbe` asks.

## What was actually verified, and how

The parsers in these programs are ordinary C with no Mac dependencies, so
they were extracted into host harnesses and compiled and run with real
test vectors. That does not prove they compile under CW8 -- nothing here
does -- but it exercises the logic most likely to be quietly wrong.

`GetMovie.c`:
  - URL parsing: https/http, default and explicit ports, query strings
    preserved, missing path becomes `/`, `ftp://` and junk rejected
  - redirect resolution: absolute, scheme-relative `//host/x`, and
    root-relative `/path`, with non-standard ports carried through
  - header lookup: case-insensitive, and specifically that
    `Content-Length` is not confused with `Content-Type` -- a
    prefix-match bug that would have been miserable to find on hardware
  - status parsing: 200, 302, non-HTTP garbage
  - the ftyp patch: `isom` and `mp42` patched, already-`qt  ` left alone,
    `moov` untouched, short buffers refused

`MP4Fix.c`:
  - Pascal-string suffix matching, case-insensitive
  - the `.mp4` -> `.mov` rename including the degenerate case where the
    name *is* `.mp4`, long names, `.m4v`, and confirmation that the
    result never grows past the 31-character HFS limit

`QTProxy.c`:
  - absolute-form proxy request parsing; origin-form, `https` and `POST`
    all correctly refused
  - header rewriting: `Content-Type` forced to `video/quicktime` and
    added when absent, `Content-Length` preserved, unknown headers passed
    through, `Transfer-Encoding` dropped, `https` Location downgraded to
    `http` while a plain `http` Location is left alone

All passed.

## Two real bugs found by reading macTLS's own integration notes

Both were in `GetMovie.c` as first written, and both would have bitten
only on hardware:

1. **Entropy was never pumped.** `OSTLS_CollectEntropy()` has to be
   called from the host's idle loop -- it gathers mouse deltas and timer
   jitter. A tight fetch loop *is* the idle loop when nothing else is
   running. Omitting it does not fail loudly; the handshake still
   completes, just with less entropy behind the key material than
   anyone intended. Now called every time round the pump loop, with 256
   samples collected before the first handshake and
   `OSTLS_LoadSeed`/`OSTLS_SaveSeed` so a second launch starts warm.
2. **No clock check.** Certificate validity is checked against the Mac's
   clock, and a vintage machine with a flat PRAM battery is the single
   most likely way this fails. The error it produces otherwise --
   `kOSTLSAsync_ClockBefore2000` (2002) -- explains nothing. Now checked
   at startup with a plain-English message.

## macTLS build facts worth not rediscovering

- The minimal file list is **eight files**, named in their
  `docs/integration-handoff.md` and all verified present: `ostls_async.c`,
  `ostls_entropy.c`, `ostls_http.c`, `ostls_log.c`, `ostls_time.c`,
  `ssl_engine_cw8.c`, `ostls_b3_anchors.c`, and `ostls_fetch.c` if the
  blocking one-shot is wanted. Plus the BearSSL tree (294 `.c` files --
  add the folder, not the files).
- Access paths: `macTLS/os9/` and `macTLS/bearssl/inc/`.
- Prefix ordering matters. `ostls_cw8_prefix.h` must come first (it
  configures the BearSSL build and defines `inline` away, which CW8's
  C89 mode requires), then `ostls_classic_shim.h`. `GetMovie_prefix.h`
  does both in that order.
- Handshakes cost **1-3 seconds on a G3**, every fetch, because session
  resumption is not wired up. Fine for fetching one movie; painful for a
  proxy doing many small requests.
- ~32 KB per connection for BearSSL buffers, ~50 KB resident per fetch.

## A CodeWarrior 8 defect to file away

From `ostls_cw8_prefix.h`, explaining why they pin `BR_CT_MUL31` to 0:

> **CW8 PPC miscompiles 64-bit shift-by-constant** -- `(int64_t)a * const`
> writes `a >> log2(const)` into the high word.

It hit them in BearSSL's constant-time multiply, and the symptom was
thoroughly misleading: RSA chains up to 2048 bits verified correctly
while 3072-bit and larger failed as NOT_TRUSTED, because the corruption
only accumulates over enough Montgomery iterations.

That is a defect in the exact compiler building our codec. h264bsd is
pure 32-bit integer code so it should be untouched -- but if anything in
this project ever produces arithmetic that is wrong only sometimes, look
here first, and do not add 64-bit multiply-by-constant arithmetic to the
codec without checking it on hardware.

## The question none of this answers

On disk we proved the file extension chooses QuickTime's importer and the
ftyp brand decides whether that importer accepts the file. Over the wire
there is no extension, so something else must route -- almost certainly
`Content-Type`, which is why `QTProxy` forces `video/quicktime`.

That remains a hypothesis. If a stream still fails with -8971 while
carrying `video/quicktime` and a patched brand, QuickTime is sniffing
content rather than trusting the header, and the next thing to try is
stripping the `ftyp` box entirely so the body begins at `moov`, the way a
real `.mov` is laid out. The box is first and its length is in its first
four bytes, so that is a small change to the same code.

---

# Session 9e -- a second lever, and a correction

## I was wrong about stripping the ftyp box

In session 9d I wrote that if Content-Type routing fails over the wire,
the next thing to try is "stripping the ftyp box entirely so the body
begins at 'moov'", and called it "only slightly harder" than the brand
patch. That is wrong, and it would have corrupted every file it touched.

**The sample tables inside `moov` hold ABSOLUTE file offsets.** `stco`
(or `co64` on large files) records where each chunk of media begins in
the file. Remove a 32-byte box from the front and `mdat` slides 32 bytes
earlier while every `stco` entry still points at where the data used to
be.

Measured on a real file:

| file | `stco[0]` | `mdat` at | data really starts | correct? |
|------|-----------|-----------|--------------------|----------|
| intact `itag36.mp4` | 3327 | 3319 | 3327 | yes |
| ftyp removed (`t3.mov`) | **3327** | **3287** | **3295** | no, 32 short |

ffmpeg on the stripped file: `Invalid NAL unit size (1668248165 > 4275)`,
`Error splitting the input into NAL units`. The intact file decodes
clean. The corruption is exactly the 32 bytes removed.

Doing it properly would mean parsing down through
`moov/trak/mdia/minf/stbl` to `stco` and subtracting the removed length
from every entry. That is actually feasible in a stream for a faststart
file -- `moov` arrives before `mdat`, so the offsets can be rewritten
before the media is relayed -- but it is real work, not a byte skip.

`t3.mov` was left on the share as a probe before this was understood. It
is corrupt. Worse, it is corrupt in a way that would have produced a
*misleading* result: opening it and watching it fail would have suggested
QuickTime rejects moov-first files, which is not what the experiment
showed at all.

## The fix for that idea is better than the idea was

Do not remove the box. **Rename it.** Overwrite the atom TYPE at offset
4, turning `ftyp` into `free` -- a padding box every parser skips. The
length field is untouched, so not a single byte moves and every offset in
the file stays valid. The result reads as `free` / `moov` / `free` /
`mdat`: an entirely ordinary QuickTime layout with no ftyp box in it at
all.

Verified here: decodes clean through ffmpeg, atom offsets identical to
the original.

So there are now two independent levers, both four bytes, both
offset-safe:

| lever | edit | status |
|-------|------|--------|
| **A** | brand at offset 8 -> `qt  ` | **proven on hardware**, plays 640x360 + AAC in real time |
| **B** | atom type at offset 4 -> `free` | structurally verified, untried on the Mac |

Lever B may be the more robust of the two, because it does not depend on
the MOV parser accepting any particular brand -- it removes the box from
consideration rather than arguing with it. That matters most for the
streaming case, where how QuickTime routes a response is still unknown.

Both `MP4Fix.c` and `QTProxy.c` now carry `kPatchMode` at the top: 1 for
lever A (default), 2 for lever B. `t7.mov` on the share is a ready-made
lever-B file (640x360 + AAC) for testing whether QuickTime takes it.

## Two Open Transport bugs found by reading macTLS's client code

Both were in `QTProxy.c` as first written:

1. **`OTInitDNSAddress` returns the address length** to put in
   `TCall.addr.len`. I had computed it by hand from `sizeof(DNSAddress)`
   minus the name field plus the string length, which is the sort of
   arithmetic that is either subtly wrong or becomes wrong later. macTLS
   notes in its own B1 stage that a zero return is suspect even though
   the call is documented not to fail. Now uses the return value and
   checks it.
2. **A negative `OTRcv` is not simply end-of-stream.** `kOTLookErr` means
   an event is pending and must be acknowledged via `OTLook` --
   `T_ORDREL` needs `OTRcvOrderlyDisconnect`, `T_DISCONNECT` needs
   `OTRcvDisconnect`. This matters here specifically: with
   `Connection: close` and no `Content-Length`, the origin closing its
   half IS the end-of-body signal for an HTTP/1.0 response, so
   mishandling it risks truncating the tail of the movie.

Neither would have shown up in a host harness. Both came from reading how
macTLS drives the same APIs.

---

# Session 9f -- the audio half of the container problem, solved

## The result first

`dv512_snd.mov` -- the Internet Archive clip, 56 bytes larger than the
original -- opens with no error dialog on Mac OS 9.2.2 under QuickTime
6.0.3 and plays **H.264 video through our component and AAC audio
through QuickTime's**, from one file. Confirmed audible by ear.

That closes the container problem completely. Both tracks, one file,
four bytes plus fifty-six.

## The symmetry that explains everything

The previous sessions established that QuickTime routes a file to one of
two parsers and that they disagree. This session showed they disagree in
*opposite directions*, and that fixing one breaks the other:

| | MPEG-4 importer | MOV parser |
|---|---|---|
| `avcC` (video config) | **dropped** -- predates H.264 | copied verbatim |
| `mp4a` audio entry | understood natively | **expects the QuickTime layout** |
| net effect | `-8971`, no picture | picture, but silence |

So the four-byte brand patch cured video by moving us to a parser that
then broke audio. Same file, same four bytes, two opposite failures. The
symmetry is exact and it is worth remembering, because it predicts how
any other ISO-native box will behave: verbatim copying is what saves us,
and anything QuickTime wants in its own dialect is what bites us.

## What the MOV parser actually choked on

Dumping the audio sample entry from the Archive file:

```
[entry] mp4a  size=87
  SOUND DESCRIPTION VERSION = 0
  channels=2 sampleSize=16 compressionID=0x0000 packetSize=0
  sampleRate=48000.000 (0xbb800000, 16.16 fixed)
    esds  size=51            <- bare, loose in the entry
```

Two defects, from QuickTime's point of view:

**1. Version 0.** In a QuickTime SoundDescription, version 0 means "one
sample per packet, fixed frame size". True for raw PCM. Nonsense for
AAC, where one entry in the `stsz` table is a ~230-byte compressed frame
carrying 1024 audio samples. QuickTime could not work out the packet
framing, so the decompressor produced nothing.

Crucially it still **read** the `esds`: Get Movie Properties reported
*"MPEG-4 Audio, 48 kHz, Stereo, 16 bits"*, correctly. It knew exactly
what the codec was. It could not work out how the bytes were cut up.
That distinction is what made this diagnosable -- a missing or
unrecognised track would have looked completely different.

**2. The `esds` sat loose in the entry.** QuickTime's Sound Manager
looks for codec configuration inside a `wave` atom
(`siDecompressionParam`), not at the top level of the sample entry.

## The fix

```
[entry] mp4a  size=143
  SOUND DESCRIPTION VERSION = 1
  channels=2 sampleSize=16 compressionID=0xFFFE packetSize=0
  sampleRate=48000.000
  samplesPerPacket=1024 bytesPerPacket=0 bytesPerFrame=0 bytesPerSample=2
    wave  size=91
      frma  12      -> 'mp4a'
      mp4a  12      -> 00 00 00 00
      esds  51      -> the original bytes, untouched
      (terminator)  8   -> size 8, type 0
```

`compressionID` 0xFFFE is -2, `kVariableCompression`: "the frame size
varies, consult samplesPerPacket".

**This layout was not guessed.** The reference was produced with
`ffmpeg -c copy -f mov`, whose MOV muxer has interoperated with real
QuickTime for twenty years, and the entry we build is identical to
ffmpeg's in every field that matters -- including its **8-byte
terminator atom** (size 8, type 0) rather than the bare four zero bytes
some documentation describes. ffmpeg additionally writes an optional
`chan` box, which we omit; that is the only difference, and it is a
QuickTime 7-era channel-layout hint that QT6 ignores.

## The offset trap, nearly sprung a second time

The new entry is 56 bytes longer. That grows `stsd`, `stbl`, `minf`,
`mdia`, `trak` and `moov`, and in a faststart file it pushes `mdat`
56 bytes down -- while `stco` still holds **absolute file offsets** to
every chunk.

This is the exact trap that corrupted `t3.mov` when we tried deleting
the ftyp box outright (stco stayed 3327 while mdat moved 3319 -> 3287).
So the tool walks every `stco`/`co64` and adds the delta to every offset
at or past the splice point. For this clip: **3,149 chunk offsets across
two tracks, all shifted +56.** A chunk offset never lands inside `moov`,
so `>= splice` is a safe and sufficient test, and it handles both
faststart and moov-last layouts without special-casing either.

## Ruling out the boring explanations first

Worth recording because it is the correct order and it cost almost
nothing:

1. **Get Movie Properties** -- a Sound Track existed and was correctly
   identified. Not a missing track.
2. **Sound control panel** -- output device "Built-in", main volume up,
   Mute off, alert volume at max. Not a muted guest.
3. **Played a system alert sound** -- audible. Not a dead VM audio path,
   which had been a real worry given UTM's own warning about Mac OS 9
   audio sampling rates.

Only then was it worth believing the file was at fault.

## Tooling

**`tools/qtify.py`** -- the Python proof. Parses the tree, rebuilds the
entry, repairs ancestor sizes, shifts chunk offsets, patches the brand.
Useful for preparing files host-side and as the reference implementation.

**`codec-src/MP4Fix.c`** -- rewritten to do the same thing **on the Mac
itself**, which is the version the project actually needs, since the goal
is a standalone OS 9 machine and not a Python script on a modern laptop.

Design notes that matter:

- It **writes a new file** rather than editing in place. The four-byte
  patch could be in-place because nothing moved; a splice cannot, and a
  backwards block copy over the user's only copy of a movie with no way
  back is not a risk worth taking.
- Only `moov` is held in memory (26K for this clip, capped at 16 MB);
  media data streams through a 64K buffer.
- `co64` is handled with **manual 32-bit carry, not a 64-bit type**, on
  purpose. macTLS found that CodeWarrior 8 PPC miscompiles 64-bit
  shift-by-constant -- `(int64_t)a * const` writes `a >> log2(const)`
  into the high word. That is the exact compiler this project uses.
- The walker only treats `mp4a` as a sample entry **when its parent is
  `stsd`**. Without that check, running the tool twice would find the
  12-byte `mp4a` placeholder we write *inside* the `wave` atom and
  corrupt the file. It also refuses a description already at version 1,
  so re-running is a no-op rather than a disaster.

## Verification method worth reusing

`tools/mp4fix_hosttest.c` stubs the Classic Mac OS APIs over stdio and
then `#include`s **MP4Fix.c unmodified**, so what gets compiled and run
on the host is the exact source CodeWarrior will see. Its output is
**byte-for-byte identical** to the Python file that actually played on
the Mac.

That is as close to proof as is available without a PPC toolchain here,
and it catches the class of bug -- an off-by-one in the chunk-offset
shift -- that would otherwise show up as corruption minutes into
playback, long after it could be traced back.

## Still open

- Whether a G3 can carry both decoders at once (the Pismo answers this).
- Whether `Content-Type: video/quicktime` routes a *stream* the way an
  extension routes a *file*.
- Whether the classic non-Carbon path can `OTBind` a chosen port.
- The YUV output path (`wantedDestinationPixelTypes` -> `'2vuy'`).
- Everything so far is QEMU on Apple Silicon. None of it is hardware.

---

# Session 9g -- the YUV output path, a startup registrar, and a hardened MP4Fix

Worked unattended overnight against an agreed priority list. Everything
below is verified on the host; **none of it has been compiled on the
Mac** -- see "Build state" at the end, which is the one thing that did
not go to plan.

## A correction, first

Early in the session I flagged the per-frame `DumpBytesToShare` call in
`DrainOutputPictures` as a major performance find -- 130K to the shared
volume per frame, four megabytes a second of synchronous network I/O
inside the decode loop. That was wrong. `DumpBytesToShare` already
begins with `if (!kH264Diagnostics) return;` and the flag is already 0,
so the call has been inert all along. I had duplicated a mechanism that
existed thirty lines further down the same file.

The call is now additionally bounded to the first three pictures, so
that turning diagnostics on to chase an unrelated bug does not quietly
drop playback to a crawl. But the headline claim was mine and it was
not true.

## The YUV output path

### Grounded in the header, not in memory

The session summary carried into this session said
`wantedDestinationPixelTypes` was a field of `CodecCapabilities`. It is
not. Checking `quicktime-headers/ImageCodec.h` -- Universal Interfaces
3.4.1, QuickTime 5.0.1 -- settles it:

- `wantedDestinationPixelTypes` is `OSType **` in
  **`CodecDecompressParams`**, in the "QuickTime 2.5 and greater"
  block. A handle to a zero-terminated list.
- `CodecCapabilities` has no such field; it ends at `flags2`.
- `kYUV420CodecType` is `'y420'`, `k422YpCbCr8CodecType` is `'2vuy'`.
- `PlanarPixmapInfoYUV420` **is** declared, as three
  `PlanarComponentInfo` records of `{SInt32 offset; UInt32 rowBytes;}`
  -- so the planar path is available and did not have to be guessed.

A reminder that the recorded learning "ground code against the actual
headers rather than assumptions" keeps paying. The headers are in the
repo; there was no excuse for working from memory.

### What changed

`PreDecompress` now offers, in order of preference:

| format | why |
|---|---|
| `'y420'` | planar 4:2:0 -- **exactly** what h264bsd already outputs. The blit becomes three runs of `BlockMoveData` and the colour conversion disappears. |
| `'2vuy'` | packed 4:2:2. No per-pixel arithmetic, and two bytes per pixel instead of four. |
| 32-bit RGB | today's path, kept last as the guaranteed fallback. |

Rough per-pixel cost, which is the whole point:

```
RGB    3 table lookups, 3 adds, 3 shifts, 3 clamp lookups, 4 stores
2vuy   2 byte moves
y420   one memcpy, amortised
```

### Why this is safe to turn on

The blit dispatches on `dstPixMap.pixelFormat` **as actually handed to
us at BandDecompress time**, not on what we asked for. Asking is not
getting -- that lesson is already written into this file's own log
lines. If the ICM ignores the list entirely we take the RGB path exactly
as before and the picture is still right. An unrecognised format draws
nothing and logs once, rather than guessing: a blank window is
diagnosable, a wrong-format blit into a screen backing store is
convincing garbage at best.

`kOfferYUVOutput 0` restores the old behaviour outright. The RGB blit
itself was **not touched** -- the two new functions repeat its geometry
work rather than sharing a helper, deliberately, because refactoring the
one path known to work on hardware while nobody is watching is a bad
trade.

### Verified byte-exact

`tools/blit_hosttest.c` extracts the two new blit functions verbatim
from `H264Codec.c` and runs them against a synthetic 4:2:0 picture whose
values vary along both axes, so a transposition, a one-row shift or a
Cb/Cr swap all change the output. Compared against an independent Python
reference:

```
PASS  2vuy A  origin 0,0
PASS  2vuy B  inset, neg bounds
PASS  y420 A  origin 0,0
PASS  y420 B  inset, neg bounds
```

Case B reproduces the awkward geometry the real BandDecompress log
showed: a 1280x768 pixmap with bounds `(-24,-50)`, frame parked at
`(88,24)`.

**A trap in the harness worth recording.** The first y420 run failed
everywhere. The cause was not the code under test: the stub typedef'd
`SInt32` as `long`, which is 8 bytes on x86-64 and 4 on PowerPC, so
`PlanarPixmapInfoYUV420` came out 48 bytes instead of 24 and every plane
offset was wrong. A harness that models the target with wider types than
the target also hides 32-bit overflow the Mac would actually exhibit.
The stubs now use `int`/`unsigned int`.

## The startup registrar -- and why it must never quit

The obvious design is a small app in Startup Items that registers the
codec and quits. **That design is broken**, and it would not show up
until something tried to play a movie much later.

`RegisterComponent` does not copy anything. It stores the address of the
RoutineDescriptor inside the codec's CFM fragment and the Component
Manager calls through it forever after. A CFM connection belongs to the
process that opened it; when an application quits, CFM closes its
connections and the fragment's memory goes away. A startup app that
quits therefore leaves the Component Manager holding a pointer into
freed memory -- still listed, still found by `FindNextComponent`, and
jumping into whatever got reused for that memory the moment anything
opens it.

Two ways to keep something alive holding the connection:

1. **A system extension (INIT).** Loads into the System heap and stays
   for the session -- what real codecs do. But real codecs get there via
   `'thng'`/`'dlle'`, which is exactly what does not work here, and a
   PowerPC INIT means a 68K stub bridging into CFM: real work and a new
   class of bug.
2. **A faceless background application.** Registers, then never quits,
   holding the connection for the whole session. With the background-only
   bit in its `'SIZE'` resource it has no menu bar, no windows and no
   Application menu entry.

`H264Startup.c` is option 2, because it is a few hundred lines of
ordinary code rather than a new toolchain problem, and the cost is one
process slot on a machine with hundreds of megabytes. It idles in
`WaitNextEvent` with the maximum timeout, so it costs no measurable CPU.
Option 1 stays the tidier long-term answer and the reasoning is written
down so whoever attempts it knows what it must guarantee.

Two other things it does that the bench registrar does not:

- **Finds Extensions via `FindFolder(kOnSystemDisk,
  kExtensionFolderType)`** instead of the hardcoded
  `"Macintosh HD:System Folder:Extensions:..."`. The Pismo and the
  TiBook are not going to share a volume name, and this is the file
  whose entire purpose is to work unattended on machines I cannot see.
- **Reports failure without a face** -- a log beside the codec in
  Extensions, always; plus a `StandardAlert` on failure only, which
  needs no ALRT/DITL resources and so keeps this a one-file project.
  On success it says nothing.

## MP4Fix hardened against a real corpus

Six derived files, run through the **C implementation** (via the stdio
harness, so the exact source CodeWarrior will compile), each checked for
clean decode and for idempotency:

| case | result |
|---|---|
| faststart, moov before mdat | **2455 of 2455** chunk offsets shifted |
| **moov after mdat** | **0 of 2455** shifted -- correct, nothing moved |
| no audio track | detected, brand patch only, +0 bytes |
| mono | 1 channel, handled |
| 44.1 kHz | rate `0xAC440000` copied verbatim |
| already version 1 | declined both the audio rewrite and the brand patch |

All six decode clean. **Every second run correctly declined** -- the
"only treat `mp4a` as a sample entry when its parent is `stsd`" guard
does its job and the tool does not find the 12-byte `mp4a` placeholder
it wrote inside the `wave` atom.

The moov-last case is the one that matters. A tool that blindly added
the delta to every chunk offset would pass the faststart test, pass a
casual eyeball, and silently corrupt every non-faststart file it
touched.

One documented behaviour the corpus exposed: **extensions other than
`esds` are dropped.** ffmpeg files carry a `btrt` alongside it, which is
why their entries are 107 or 110 bytes rather than the Archive file's
87. Dropping it is deliberate -- we are translating into QuickTime's
dialect, QT6 does not read `btrt`, and nothing load-bearing lives
outside the esds -- but it is now written down rather than silent.

Regression check after the edit: output still byte-identical to the file
that actually played on OS 9.

## Build state -- the thing that did not work

CodeWarrior was opened and both projects restored. `Project ->
Synchronize Modification Dates` then `Make` on `H264Codec` **returns
instantly, with no progress window and no new messages** -- i.e. the IDE
believes nothing needs recompiling, which cannot be true of a file that
changed minutes earlier.

Also visible at launch, and possibly related: Project Messages carries
two warnings that an access path named `share:` cannot be found, for
project "Probe". If `H264Codec` resolves its sources through the same
access path, the project is not looking at the copy on the share at all.

I could not scroll the project's file list to check which
`H264Codec.c` it references -- neither the scroll wheel nor dragging the
scrollbar moved it -- and chose to stop rather than keep poking at a
working IDE unattended. **Nothing was changed in the project.** Only
Synchronize Modification Dates and Make were run, both harmless.

First thing to check in the morning: select `H264Codec.c` in the project
and confirm its path points at the share copy dated today. If it points
somewhere else, that is where the edits need to go.

### Addendum: the build blocker, diagnosed without the IDE

Rather than keep fighting the project window, check it from the host.
If CodeWarrior had compiled the share's `H264Codec.c`, something would
have **read** the file after it was written:

```
H264Codec.c
  modified: 2026-09-22 07:25:37.778
  accessed: 2026-09-22 07:25:37.782
```

Access time is four milliseconds after write time -- that is the write
itself. Nothing has opened the file since. The `Make` at 07:31 never
touched it.

So the conclusion is that **the `H264Codec` project does not reference
the copy on the share.** Its `H264Codec.c` is somewhere else, almost
certainly a local copy on `Macintosh HD` inside the VM, and every edit
delivered to the share has been landing beside the build rather than in
it.

Confidence: high but not total. Some mounts are `noatime`, and this is a
bridged volume rather than a local disk, so an unchanged access time is
strong evidence rather than proof. It is consistent with the other two
observations though -- an instant `Make` with no progress window, and
the launch-time warning that an access path named `share:` cannot be
found for the sibling "Probe" project.

**Morning check, in order:**

1. In the `H264Codec` project window, select `H264Codec.c` and Get Info
   on it. Where does it actually point?
2. If it points at a local copy, either repoint the project at the share
   or copy `share:H264Codec.c` over the local one -- the share copy is
   the current source of truth for everything in session 9f and 9g.
3. Settings -> Target -> Access Paths: the `share:` path is reported
   missing for "Probe" and may be missing here too. Worth fixing for
   both, since it is what makes the share usable as the source tree.

Nothing was changed in either project. Only Synchronize Modification
Dates and Make were run.

---

# Session 10 -- the YUV build runs on the Mac, and '2vuy' is what QuickTime picks

## Where the real source lives (and why last night's "build" never happened)

The `H264Codec` CodeWarrior project does **not** use the share. Its
source is

    Macintosh HD:Desktop Folder:H264 Classic Project Folder:H264Codec.c

read straight off the editor window's Path field. Every edit delivered to
`share:H264Codec.c` since the project was set up landed beside the build,
not in it, which is why last night's `Make` returned instantly.

Workflow that works: commit to the share from the host, then in the
guest Finder drag `share:H264Codec.c` onto the project folder and accept
"replace the older item". Close any CodeWarrior editor window on that
file first -- an open editor makes the Finder refuse with "in use". The
guest's pre-YUV source is preserved on the share as `Copy of H264Codec.c`
(diffed against the new version: only the intended hunks differ).

## Last night's freeze, reassessed

The relink-while-live hypothesis is refuted: `Extensions:H264 Decoder`
was still dated Thu Sep 10 9:24 PM, so `Make` never wrote it. The simpler
explanation fits every observation: the guest clock stopped at the exact
minute the VM was **paused** for the save-state attempt, and never moved
again after "resume". Pause + a failed QEMU snapshot left the vCPUs
stopped while UTM showed the VM as running. This is the maintainer's rule in
practice: the VM is shut down cleanly, never paused, slept or restarted.

## Three defects caught before or by the first Mac build

1. **Missing forward declarations.** `BlitPicture` calls `LogNum`, which
   is defined 300 lines later. C89 invents `extern int LogNum()` and the
   later `static void` definition collides. The blit-only host test
   never compiled `BlitPicture`, so it could not see this; found by
   reading definition order. `tools/ctx/ctx_test.c` now compiles every
   touched region **in the real file's order**, and extract.py asserts
   that order still holds.

2. **Handle ownership inverted.** The first draft resized
   `p->wantedDestinationPixelTypes` as though the ICM supplied the handle.
   The codec owns it: allocate once per instance in the globals, point the
   field at it, dispose it in `Close`.

3. **The YUV path compiled out of existence -- silently.** Classic builds
   see `PixMap`'s fourth-from-last field as `planeBytes`
   (`OLDPIXMAPSTRUCT`), and the first version responded by forcing
   `kOfferYUVOutput` to 0. The build was clean. The only symptom: object
   code came out **smaller** after adding 300 lines -- 5852 bytes before,
   5580 after, with data up by exactly what the YUV-off case predicts.
   Fixed by reading the same slot under whichever name the build uses.
   Build sizes since: 7132 → 7856 (diagnostics) → 8616 (fallback log +
   counters).

## The result

`dv512_snd.mov` plays with a correct picture. The one-shot report in
`BlitPicture`:

```
BlitPicture: dst pixelFormat 846624121     '2vuy'
BlitPicture: dst pixelSize 16
BlitPicture: dst rowBytes (masked) 728
```

- QuickTime accepted the offer list and, for playback, allocated an
  **offscreen '2vuy' buffer**; our 2vuy blit filled it; QuickTime's own
  transfer converted it to the 32-bit screen. **The 2vuy blit is now
  verified on real QuickTime 6.0.3**, not just on the host.
- It **skipped 'y420'**, first in the list. Most likely QuickTime 6 on
  this machine has no y420 transfer path. The y420 blit remains
  host-verified only. It stays in the list: on hardware with a YUV
  overlay (the TiBook's Radeon, possibly) the answer may differ, and the
  log will say so.
- One early call (`BandDecompress #0`) received the **32-bit screen**
  directly, but produced no picture -- h264bsd hands out the first
  picture only once the next one begins -- so the RGB blit did not run.

A YUV destination does not remove the colour conversion; it moves it
into QuickTime. Whether QuickTime's 2vuy→RGB is cheaper than our table
converter on a G3 is for the Pismo to measure, which is what the next
section is for.

## Frame-rate counters

Diagnostics-only counters, reported when a codec instance closes:

```
=== playback statistics ===
STATS pictures shown 1221
STATS BandDecompress calls 1224
STATS avg us per call (after first 8) 3761
STATS wall ms first->last picture 40396
STATS achieved fps x100 3020
```

In QEMU on Apple Silicon: 1221 of 1229 frames shown, 3.8 ms per frame
(decode + blit) against a 33.4 ms budget -- about 9x headroom. **This
says nothing about a 500 MHz G3.** The counters exist so the Pismo can
answer that with numbers. "avg us per call" skips the first 8 calls,
which carry the slow diagnostic file I/O; all arithmetic is 32-bit
unsigned on `Microseconds().lo`, wrap-safe, no 64-bit types (CW8 bug).

## Logging on real hardware

The log used to go only to `share:h264log.txt`, and fell silent on any
machine without the share -- i.e. exactly the real Macs. It now falls
back to `<boot volume>:Desktop Folder:H264 Decoder Log` via
`FindFolder`, so after playing a movie on the Pismo there is a
SimpleText file on the desktop with the pixel format QuickTime chose and
the statistics. The 130K YUV dumps stay share-only.

## Measured: quitting the registrar unregisters the codec

The bench registrar calls `RegisterComponent(..., global=1)` and never
`UnregisterComponent`. After quitting it and running it again:

```
Before: 'imdc'/'avc1' registered = 0
RegisterComponent (global) -> 0x0010027E      (previous run: 0x000F027E)
```

The Component Manager removes an application's components when it
quits, `global` or not. This **contradicts** the rationale I wrote into
`H264Startup.c` last night (that quitting would leave a dangling
pointer). The design conclusion stands -- something must stay resident
or the codec vanishes -- but the stated failure mode was wrong, and the
comments now say what was measured.

Also fixed in `H264Startup.c`: failures were reported with
`StandardAlert`, which a background-only process cannot reliably show.
It now posts a Notification Manager request allocated in the system heap
with `nmResp = -1` (auto-remove), so it can quit safely after a failure.

## State left in the VM

- `Extensions:H264 Decoder` -- this session's build, **diagnostics ON**
  (the fallback log and counters are the point of it). Flip
  `kH264Diagnostics` to 0 for a quiet build.
- The known-good Sep 10 build is in `H264 Classic Project Folder:Probe:`
  (moved, not deleted).
- Registration is still manual: run `Std C Console Debug`, leave it open.

---

# Session 11 -- a blank poster frame explained, a shutdown hang fixed, and a kit for real hardware

Everything below was measured in the UTM VM (Mac OS 9.2.2, QuickTime
6.0.3) or on the host against the real sources. Nothing here has run on
the Pismo or the TiBook yet; the last section is what makes that easy.

## MP4Fix now builds and runs on the Mac

The `mp4fix` CodeWarrior project (C Console Classic stationery, prefix
file cleared, ANSI Strict off) builds an application called
**`mp4fixer`**. Run on `share:dv_quicktime_h264_512kb.mp4`, it wrote
`macfix1.mov`, and on the host:

```
md5  f3ec3ec5e72f2f99679c927167eea460  macfix1.mov     (built by CodeWarrior, run on OS 9)
md5  f3ec3ec5e72f2f99679c927167eea460  dv512_snd.mov   (host-verified reference)
```

Byte-identical. The whole MP4 path -- brand patch, v1 sound description,
`wave` atom, chunk-offset shift -- is now proven on the target compiler,
not just under gcc.

Two build lessons:

- **"Link Error: Can't write application 'mp4fix'."** The output was
  named after the project file, `mp4fix`, in the same folder. HFS is
  case-insensitive, so the linker was being asked to overwrite the
  project. Same trap as session 5's `H264Codec`. Output renamed
  `mp4fixer`.
- **Make did nothing after a source was replaced in the Finder.**
  CodeWarrior caches modification dates. `Project > Synchronize
  Modification Dates` before `Make` whenever a file is swapped in from
  the share.

Also polished: the SIOUX console no longer asks to save a transcript
when you quit (`SIOUXSettings.asktosaveonclose = false`,
`autocloseonquit = true`, set before the first `printf` and guarded by
`__MWERKS__` so the host test still compiles). The gcc regression still
produces byte-identical output, and the six-file edge-case corpus is
unchanged.

## The blank poster frame: the decode loop was abandoning the first IDR

Session 10 recorded that `BandDecompress #0` received the 32-bit screen
"but produced no picture -- h264bsd hands out the first picture only once
the next one begins". **That explanation was wrong.**

`h264bsdDecode` returns `readBytes == 0` on purpose in two cases, and in
both the NAL at that offset has **not** been decoded yet:

- `H264BSD_HDRS_RDY` -- the first slice of a picture activated a new SPS.
  That is the first sample every fresh decoder instance sees.
- `H264BSD_PIC_RDY` after concealment -- a new slice arrived while the
  previous picture was incomplete.

It sets `prevBufNotFinished` and expects to be called again with the
**same pointer** (`h264bsd_decoder.c`, the
`prevBufNotFinished && byteStrm == prevBufPointer` branch). Our loop
treated zero bytes as "no progress" and broke out. So on every fresh
instance the IDR was activated and then dropped. Playback only looked
right because QuickTime happened to send the key frame a second time to
the same instance, and the second call slipped down the "same pointer"
path.

The stream itself rules out the reordering theory: its SPS has
`pic_order_cnt_type 2` and `num_reorder_frames 0`, so h264bsd outputs
every picture immediately.

### Measured, not argued: `tools/decode_run`

A new host harness runs the codec's real code. `extract_run.py` cuts the
decode loop, `DrainOutputPictures`, `BlitPicture`, `FeedParameterSets`
and `EnsureDecoder` out of `H264Codec.c` **verbatim**. `decode_run.c`
links them against the real h264bsd and feeds the movie's real samples,
one QuickTime sample per call, into a `'2vuy'` destination with rowBytes
728, exactly as QuickTime 6 allocated it. `make_old_loop.py` rebuilds the
previous loop into an otherwise identical file, so old and new run side
by side. Every picture is compared with ffmpeg's decode, bit for bit.

| mode | old loop | new loop |
|---|---|---|
| `poster` (first sample only, fresh decoder) | 0 pictures, destination unpainted | 1 picture, bit-exact |
| `seq` (every sample once) | **0 of 1229 pictures**, `-2070` on every later call | 1229 of 1229, bit-exact |
| `twice` (first sample twice, then the rest -- what QuickTime does) | 1229, but call 0 unpainted | 1229, all 1230 calls painted bit-exact |

The `seq` row is the one that would have bitten on a machine where
QuickTime did not happen to repeat the key frame.

### The fix

The loop now retries the same offset when zero bytes are consumed, up to
`kMaxDecodeStalls` (2) times in a row. That still guarantees it can never
spin inside a QuickTime callback. It also drains on `PIC_RDY` whether or
not bytes were consumed, and forgets the last picture on `HDRS_RDY`
(the DPB may be reallocated).

A second, smaller fix: **every `BandDecompress` call now paints the
destination.** When a call produces no new picture -- the re-sent key
frame is recognised by h264bsd as a redundant copy -- the last picture
is blitted again instead of leaving QuickTime's buffer as it was.

Confirmed on the Mac: the poster frame appears the moment a movie opens,
and the log shows the retry doing its job:

```
--- BandDecompress enter # 0
  decode: 0 bytes consumed, retrying same NAL, status 2
  output picture # 0
```

## The registrar hung Shut Down -- and now answers the Finder

The first `H264 Auto-Register` had no quit path at all, on the theory
that nobody can quit a process they cannot see. But the Finder quits
every process at Shut Down and Restart by sending it a Quit Application
Apple event, and our `'SIZE'` resource says we understand high-level
events. We received it and ignored it. Special > Shut Down stalled, and
a second attempt got "you have already chosen Shut Down or Restart".
The only way out was a forced power-off. On a real PowerBook that would
have been every shutdown.

The idle loop now passes high-level events to `AEProcessAppleEvent`, and
an `'aevt'/'quit'` handler ends it. Quitting is orderly:

1. Wait up to `kQuitGraceTicks` (10 s) for
   `CountComponentInstances()` to reach zero, yielding every 15 ticks so
   applications can close their movies. Our fragment must not unload
   while another application still has an instance open.
2. Then `UnregisterComponent` and `CloseConnection`. If the wait times
   out it leaves anyway, because stalling shutdown again would be the
   worse failure.

`System Folder:Extensions:H264 Register Log.txt` after a clean cycle:

```
=== H264 Auto-Register, startup ===
  registered, component = 983678
  'imdc'/'avc1' now count = 1
Holding the fragment connection for the rest of this session.
Quit requested (Shut Down or Restart).
  codec instances still open = 0
  UnregisterComponent = 0
  Unregistered and closed. Goodbye.
=== H264 Auto-Register, startup ===          <- next boot, fine
```

**Installation changed.** The registrar is now built as file type
`'appe'` and lives in **Extensions**, next to the decoder. Dragging it
onto the System Folder makes the Finder offer to put it there. The
Process Manager launches `'appe'` files from Extensions by itself, and
booting with Shift disables both halves together.

## Frame-rate statistics that mean something

After the poster-frame fix the first picture is drawn when the movie
*opens*, so "wall ms first->last picture" started including however long
the movie sat before Play: 61 s for a 41 s clip, "20 fps". It is kept
for comparison with older logs, now labelled `incl. pauses`. New lines:

- **`STATS playback run ...`** -- the longest stretch of pictures with no
  gap over 0.5 s, and its pictures per second. This is the real frame
  rate.
- **`STATS max us per call`** -- the slowest call. A G3 that averages
  under budget but spikes over it will stutter.
- **`STATS blits to y420 / 2vuy / 32-bit RGB`** -- which destination
  every picture actually went to. The poster frame and playback can
  differ, so the first-blit report alone was not enough. The
  `BlitPicture: dst ...` report now also repeats whenever the
  destination changes, up to six times.

`kH264Diagnostics` moved to the configuration block at the top of the
file. `BlitPicture` now uses it, 400 lines above where it used to be
defined. `tools/ctx` caught that as a compile error before CodeWarrior
saw it.

### Measured in the VM (QEMU on an M3 -- says nothing about a G3)

| build / depth | pictures | where they went | avg us | max us | run fps |
|---|---|---|---|---|---|
| v12, Millions | 1229/1229 | (not yet counted) | 4028 | -- | 30.00 |
| v13, Millions | **917**/1229 | 917 RGB32 direct, then 2vuy | 7379 | 243635 | 29.55 over 844 |
| v13, **Thousands** | 1229/1229 | **1229 via 2vuy** | **3360** | 21281 | 29.99 |

Three things to carry to real hardware:

1. **In Thousands, QuickTime gave us `'2vuy'` for everything**, poster
   frame included. It never handed us a 16-bit RGB pixmap, which we
   could not draw. The offer list is respected.
2. **In Millions, QuickTime sometimes draws straight to the 32-bit
   screen.** That uses our own table-driven YUV->RGB, which measured
   about twice the per-call time of the `'2vuy'` path. That is not a
   fair comparison, because QuickTime's own 2vuy->RGB happens outside
   our timer. The playback-run fps is the honest number.
3. **One Millions run showed only 917 of 1229 pictures**, and its
   slowest call took 244 ms (the other runs peaked near 21 ms). Whether
   the stall caused the drop is not proven. What is certain: when
   QuickTime falls behind on a temporally compressed codec it skips
   ahead to the next key frame, so the count of pictures shown is the
   first thing to read on a slow machine. The next runs were clean.
   Treat any single run with suspicion and run each test twice.

## A hardware test kit, and getting resource forks out of the VM

The ClassicVirtio shared folder carries resource forks across. For each
guest file it writes `NAME` (data fork), `NAME.rdump` (the resource fork
as DeRez text) and `NAME.idump` (type, creator, then Finder flags by
name). So the Finder-copied binaries arrive complete, and
`tools/kit/macpack.py` turns them back into real Mac files:

- **BinHex 4.0** (`.hqx`). Python 3.11 removed `binhex` and
  `binascii.b2a_hqx`, so the encoder is written out: RLE90 (runs over
  3, literal 0x90 as `90 00`), 6-bit encoding, CRC-16/XMODEM via the
  surviving `binascii.crc_hqx`, and 64-column lines.
- **MacBinary II**, and an **8 MB HFS image** (via `machfs`) with
  everything already decoded, for emulators or for writing to a card.

The encoder was checked three independent ways:

1. The Unarchiver (`unar`) decodes all three bit-exact (data fork,
   resource fork, type/creator).
2. macutils `hexbin`, a period Unix decoder with a strict line-length
   check, decodes all three bit-exact.
3. **StuffIt Expander in Mac OS 9 decodes all three into files
   byte-identical to CodeWarrior's output.** The data fork, the
   `.rdump` resource-fork text and the `.idump` type/creator/flags all
   match. Evidence is on the share in `kitsrc/` (originals) and
   `kitverify/` (`sit <name>` = StuffIt's output).

The only difference from macutils' own BinHex writer is policy, not
format: it zeroes the Finder flags and does not run-length the `????`
creator.

`tools/kit/make_kit.py` builds the kit: the three `.hqx` files, the
converted test movie (`MooV/TVOD`), the HFS image, a `READ ME FIRST`
(Mac Roman, CR line endings, for SimpleText) and `SHA256SUMS`. It is in
the host's Public folder as `H264 OS9 Kit/` and `H264 OS9 Kit.zip`.

## Two VM hazards found on the way

**StuffIt Expander froze the guest when told to expand onto the share.**
Its destination preference is "Same as original". Dropping an `.hqx`
that lived on the share made it write there, and the machine stopped:
the menu-bar clock froze and Cmd-Opt-Esc did nothing. (UTM's Virtual
Machine > Send Key > Left Meta+Alt+Esc sends the real key combination
without macOS catching it.) It was forced off per the standing rule,
and Disk First Aid came back clean. The same file, copied to Macintosh
HD first, decoded in seconds. So always **copy to Macintosh HD first, then
expand.** Also switched off StuffIt's "Allow Version Checking", to take
the network out of the picture.

**Dragging a file from a volume's window to the desktop moves it into
that volume's own `Desktop Folder`.** It does not copy it to the boot
disk. The share now has one. To get a file onto Macintosh HD, drop it in
a Macintosh HD folder window.

And a host-side oddity: after a tool reconnect, UTM re-created its
windows and the VM window came back un-zoomed (Window > Zoom fixes it;
guest coordinates shift by about +9 px). For a while, pointer events in
the top-right corner of the screen did not reach the guest, probably an
invisible overlay from an app outside the allowlist. Type-select plus
menus reach anything there.

## State left in the VM

- `Extensions:H264 Decoder` -- the v13 build (loop fix, repeat-last-
  picture, run statistics, per-format counters), **diagnostics ON**.
- `Extensions:H264 Auto-Register` -- the `'appe'` registrar with the
  Quit handler. Nothing needs to be run by hand any more. (Should an old
  `h264autoreg` ever sit in Startup Items as well, it is harmless: it
  finds the codec already registered and exits at once.)
- `untitled folder` on the desktop -- the StuffIt decode test (three
  `.hqx` and their decoded files).
- Trash (not emptied): the Sep 23 decoder and the v12 decoder.
- Monitors back on Millions. StuffIt Expander's version check is off.
- VM shut down cleanly with Special > Shut Down at the end of the
  session.

## Next

- **The Pismo and the TiBook.** Copy `H264 OS9 Kit` over, follow `READ
  ME FIRST`, and bring back `H264 Decoder Log` from the desktop.
- Depending on those numbers: a faster path for whichever destination
  real QuickTime picks (the RGB32 converter is the obvious suspect), or
  dropping `k32ARGBPixelFormat` from the offer list so QuickTime always
  converts.
- A quiet build (`kH264Diagnostics 0`) once the hardware numbers are in.
