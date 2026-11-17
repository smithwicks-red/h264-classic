#!/bin/bash
# build.sh linux|mac : compile FFmpeg 0.8.15 h264 subset + AVCDecLib shims/API
set -e
T=$1; S=/home/claude/stage3/s3; FF=${FF:-/home/claude/stage3/s3/ff}
R68=/home/claude/stage3/r68build/toolchain/bin; ALTIH=/home/claude/stage3/Retro68/gcc/gcc/config/rs6000
AVC="avpacket bitstream cabac dsputil error_resilience golomb h264 h264_cabac h264_cavlc h264_direct
 h264_loopfilter h264_parser h264_ps h264_refs h264_sei h264dsp h264idct h264pred imgconvert mpegvideo
 options parser simple_idct jrevdct faanidct utils raw"
PPC="dsputil_ppc dsputil_altivec fdct_altivec float_altivec gmc_altivec h264_altivec idct_altivec int_altivec mpegvideo_altivec"
UTIL="mem cpu avstring dict imgutils mathematics rational pixdesc opt samplefmt audioconvert utils inverse intfloat_readwrite"
COMMON="-U_FORTIFY_SOURCE -fno-stack-protector -std=gnu99 ${OPT:--O2} -fno-tree-vectorize -fno-tree-slp-vectorize -fomit-frame-pointer -fno-strict-aliasing -fcommon -fno-math-errno -fno-signed-zeros -w -DHAVE_AV_CONFIG_H -DAVD_8BIT_ONLY=1 -I$S/cfg -I$FF -I$S/src"
if [ $T = linux ]; then
  CC=powerpc-linux-gnu-gcc; O=$S/ol
  PLAIN="-mcpu=750 -mno-altivec -ffunction-sections -fdata-sections"
  AV="-mcpu=7400 -maltivec -mabi=altivec -ffunction-sections -fdata-sections"
else
  CC=$R68/powerpc-apple-macos-gcc; O=$S/om
  MACX="-include $S/src/mac_compat.h -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration"
  PLAIN="-mcpu=750 -ffunction-sections -fdata-sections $MACX"
  AV="$MACX -mcpu=7400 -maltivec -Wa,-maltivec -mvrsave -mabi=altivec -I$ALTIH -flax-vector-conversions -ffunction-sections -fdata-sections"
fi
mkdir -p $O; cd $O; rm -f *.o
c(){ $CC $COMMON $2 -c "$1" -o "$3" 2> "$3.err" || { echo "FAIL $1"; head -5 "$3.err"; exit 1; }; }
for f in $AVC; do c $FF/libavcodec/$f.c "$PLAIN" avc_$f.o; done
for f in $PPC; do c $FF/libavcodec/ppc/$f.c "$AV" ppc_$f.o; done
for f in $UTIL; do x=""; [ $f = mem ] && x="-DMALLOC_PREFIX=avd_"; c $FF/libavutil/$f.c "$PLAIN $x" util_$f.o; done
for f in avcdec avd_shim; do c $S/src/$f.c "$PLAIN" s_$f.o; done
c $S/src/avd_libc.c "$PLAIN -fno-builtin -fno-tree-loop-distribute-patterns" s_avd_libc.o
ls *.o | wc -l
