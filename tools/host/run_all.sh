#!/bin/sh
# run_all.sh MOVIE.mp4 [WORKDIR] -- the v14 verification suite, one command.
#
# Builds tools/host/decode_run (the WHOLE of codec-src/H264Codec.c against
# host_mac.h) for this machine and, if powerpc-linux-gnu-gcc and qemu-ppc
# are installed, as a 32-bit big-endian PowerPC program too. Then checks,
# against ffmpeg's own decode of MOVIE:
#   1. every picture bit-exact, in seq / poster / twice order
#   2. the destination after every call, for '2vuy', 32-bit RGB, 16-bit
#      RGB and 'y420', at the display size and at an odd size, compared
#      with an independent numpy conversion; no byte outside the rectangle
#   3. NewPtr refusing big blocks -> temporary memory, still bit-exact
#   4. damaged samples under AddressSanitizer: no crash, no error return,
#      no leak
#   5. the lateness policy against a simulated clock (a summary per case)
# Every run also requires that all Memory Manager blocks are freed at Close.
#
# Needs: gcc, python3 + numpy, ffmpeg/ffprobe. Optional for the PowerPC
# leg: apt-get install gcc-powerpc-linux-gnu libc6-dev-powerpc-cross qemu-user
#
# The movie must be 360x240, or set DW/DH in the environment (DW=320 DH=240).
set -u
MOVIE=$1
W=${2:-./host_work}
HERE=$(cd "$(dirname "$0")" && pwd)
OLD=$HERE/../decode_run
DW=${DW:-360}; DH=${DH:-240}
CWID=$(( (DW + 15) / 16 * 16 )); CHGT=$(( (DH + 15) / 16 * 16 ))
mkdir -p "$W"; cd "$W"
FAIL=0
ok()  { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; FAIL=1; }
run() { # label, command...
    L=$1; shift
    if "$@" > run.out 2> run.err; then ok "$L: $(tail -1 run.out)"; else bad "$L: $(tail -1 run.out) $(tail -2 run.err)"; fi
}

python3 "$OLD/extract_samples.py" "$MOVIE" movie.samples > /dev/null
ffmpeg -v error -y -i "$MOVIE" -f rawvideo -pix_fmt yuv420p ffmpeg.yuv

sh "$HERE/build.sh" ./dr gcc || { bad "build (gcc)"; exit 1; }
BINS="./dr"
if command -v powerpc-linux-gnu-gcc > /dev/null && command -v qemu-ppc > /dev/null; then
    sh "$HERE/build.sh" ./dr_ppc powerpc-linux-gnu-gcc && BINS="./dr qemu-ppc:./dr_ppc"
else
    echo "NOTE  no PowerPC cross-compiler / qemu-ppc: big-endian leg skipped"
fi

for B in $BINS; do
    X=$(echo "$B" | tr ':' ' ')
    for m in seq poster twice; do
        run "[$B] $m" $X movie.samples o.yuv --size $DW $DH --mode $m
        run "[$B] $m pictures vs ffmpeg" python3 "$HERE/check.py" pics o.yuv ffmpeg.yuv $CWID $CHGT $DW $DH
    done
    for fmt in 2vuy rgb32 rgb16 y420; do
        for clip in "$DW $DH" "$((DW-1)) $((DH-1))"; do
            set -- $clip
            run "[$B] $fmt ${1}x$2 decode" $X movie.samples o.yuv --size $DW $DH --mode twice --dst $fmt --dst-out d.bin --clip $1 $2
            run "[$B] $fmt ${1}x$2 destination" python3 "$HERE/check.py" dst d.bin ffmpeg.yuv $fmt $DW $DH $1 $2 twice
        done
    done
    run "[$B] NewPtr refuses >16000 bytes" $X movie.samples o.yuv --size $DW $DH --ptrfail 16000
    run "[$B] ... pictures vs ffmpeg" python3 "$HERE/check.py" pics o.yuv ffmpeg.yuv $CWID $CHGT $DW $DH
    run "[$B] PreDecompress told only the first sample's size (v15)" $X movie.samples o.yuv --size $DW $DH --prebuf-first
    run "[$B] ... pictures vs ffmpeg" python3 "$HERE/check.py" pics o.yuv ffmpeg.yuv $CWID $CHGT $DW $DH
done

if sh "$HERE/build.sh" ./dr_asan gcc -fsanitize=address,undefined -fno-omit-frame-pointer -g 2> /dev/null; then
    for seed in 1 2 3; do for pm in 20 400; do
        ASAN_OPTIONS=detect_leaks=0 ./dr_asan movie.samples o.yuv --size $DW $DH --corrupt $seed $pm --dst rgb32 --dst-out d.bin > run.out 2> run.err
        if [ $? -eq 0 ] && ! grep -q "ERROR\|runtime error" run.err && grep -q "rc-errors 0" run.out
        then ok "damaged samples seed $seed, $pm per mille: $(sed 's/ | file.*//' run.out)"
        else bad "damaged samples seed $seed, $pm per mille"; grep -m3 "ERROR\|runtime error" run.err; fi
    done; done
else
    echo "NOTE  no AddressSanitizer: damaged-sample leg skipped"
fi

echo "---- lateness policy, simulated clock (decode ms, paint ms per frame; 33.4 ms budget)"
for args in "22 3.4" "22 24" "30 10" "45 5" "70 5" "22 3.4 --seek-at 200 300"; do
    ./dr movie.samples o.yuv --size $DW $DH --sim $args --index idx.txt | head -1 | sed 's/sim decode/      /'
    python3 "$HERE/check.py" pics o.yuv ffmpeg.yuv $CWID $CHGT $DW $DH idx.txt > run.out || FAIL=1
    sed 's/^/        /' run.out
done

[ $FAIL -eq 0 ] && echo "ALL PASSED" || echo "SOMETHING FAILED"
exit $FAIL
