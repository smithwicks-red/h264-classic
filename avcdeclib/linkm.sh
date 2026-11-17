#!/bin/bash
# mac: XCOFF shared object (gc on) -> PEF (patched MakePEF) -> MacBinary 'shlb' with cfrg
set -e; cd /home/claude/stage3/s3; T=/home/claude/stage3/r68build/toolchain/bin
$T/powerpc-apple-macos-gcc -shared -nostdlib -Wl,-bE:src/avcdec.exp om/*.o lg/*.o -o AVCDecLib.so
$T/MakePEF AVCDecLib.so -o AVCDecLib.pef
$T/Rez -I /home/claude/stage3/r68build/toolchain/RIncludes src/avcdeclib.r -t shlb -c AVCd --data AVCDecLib.pef -o AVCDecLib.bin
ls -l AVCDecLib.pef AVCDecLib.bin
