#!/bin/sh
# build.sh OUT [CC] [extra -D flags...] -- build the host harness.
#   CC defaults to gcc. With powerpc-linux-gnu-gcc the result is a 32-bit
#   BIG-ENDIAN PowerPC program (run it with qemu-ppc): the same byte
#   order and the same 32-bit long as the Mac.
set -e
OUT=$1; CC=${2:-gcc}; [ $# -ge 2 ] && shift 2 || shift 1
HERE=$(cd "$(dirname "$0")" && pwd)
R=$HERE/../..
STATIC=""; case "$CC" in powerpc*) STATIC="-static -mcpu=750";; esac
$CC -std=gnu89 -O1 -fno-strict-aliasing -w $STATIC -DH264BSD_EXTERNAL_ALLOC "$@" \
    -I"$HERE" -I"$R/decoder-src" -I"$R/codec-src" \
    -o "$OUT" "$HERE/decode_run.c" "$R"/decoder-src/h264bsd_*.c "$R/codec-src/avcC_parse.c" -lm
