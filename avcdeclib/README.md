# AVCDecLib: FFmpeg 0.8.15's H.264 decoder as a Mac OS 9 shared library

A CFM/PEF shared library ('shlb', fragment name `AVCDecLib`) with 14 C entry points
(`src/avcdec.h`), version 0x00080F02. It contains only FFmpeg's H.264 decoder and parser,
8-bit, no threads, with AltiVec chosen at run time. It imports nothing: memory and logging
come from callbacks.

**Licence.** The built library contains FFmpeg code and is therefore under the
**LGPL 2.1 or later**, like FFmpeg 0.8.15 as configured here (no GPL parts). The files in
this directory that we wrote (`src/`, the scripts) are under this repository's MIT licence.
Anyone who distributes the built library must also offer its complete source: FFmpeg
0.8.15 with the patches in `patches/` applied, plus this directory. (Not legal advice.)

## Rebuilding
1. Get FFmpeg 0.8.15 (tag `n0.8.15`) and apply `patches/ffmpeg-0.8.15-avcdeclib.diff`
   and `patches/ffmpeg-0.8.15-sps-leak.diff`. `patches/avcdeclib-r2-api.diff` is the
   history of our own API change and is already in `src/`.
2. Toolchain: Retro68 (PowerPC, GCC 16.1) with the MakePEF fixes in `../retro68-makepef/`.
   `altivec.h` comes from the GCC sources (`gcc/config/rs6000`).
3. `build.sh mac && linkm.sh` gives `AVCDecLib.pef` and a MacBinary file; `build.sh linux
   && linkl.sh && runl.sh` builds the same sources for ppc-linux and runs them under
   qemu-ppc (C on `-cpu 750`, AltiVec on `7450`) against reference checksums.
   The scripts carry the absolute paths of the workspace they were written in: edit the
   variables at the top.
4. `cfg/` is the configuration used; `NOTES.txt` lists replaced FFmpeg files and the libgcc
   members that are linked in.
5. `test/AVCDecTest.c` is the Mac OS 9 console test (CodeWarrior) that decodes clips with
   the library and checks checksums and the delay/take/peek calls.
