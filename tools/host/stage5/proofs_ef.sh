#!/bin/sh
# proofs_ef.sh WORK -- stage 5 (v17) proof e (refusals: black + noErr, one log line, 0 leaks)
# and proof f (damaged B-frame streams under AddressSanitizer), plus the discontinuity trace.
# Needs the suite data dirs made by bprep.py: $DATA/<movie>/data (default WORK/../data).
set -u
W=$1; mkdir -p "$W"; W=$(cd "$W" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd); T=$HERE/../../..
LIB=${AVCLIB_X:-$T/../avcx/lib_x.o}; ASANLIB=${AVCLIB_ASAN:-$T/../avcx/asan/lib_x.o}; DATA=${DATA:-$W/../data}; M4=${M4:-$T/../movies4}
cd "$W"
sh "$HERE/../build.sh" ./dr gcc -DH264_BACKEND_AVCDEC=1 "$LIB" || exit 1
sh "$HERE/../build.sh" ./dr_asan gcc -fsanitize=address,undefined -fno-omit-frame-pointer -g -DH264_BACKEND_AVCDEC=1 "$ASANLIB" || exit 1
sh "$HERE/../build.sh" ./dr_tr gcc -DH264_BACKEND_AVCDEC=1 -DkH264TimingTrace=1 "$LIB" || exit 1
FAIL=0
black_all() { # dst file, W, H: every call black in y420
python3 - "$@" <<'PY'
import sys
d=open(sys.argv[1],'rb').read(); W=int(sys.argv[2]); H=int(sys.argv[3]); fs=W*H*3//2
blk=bytes([16])*(W*H)+bytes([128])*(fs-W*H); n=len(d)//fs
b=sum(1 for i in range(n) if d[i*fs:(i+1)*fs]==blk); print("calls %d black %d" % (n,b)); sys.exit(0 if n and b==n else 1)
PY
}
refuse() { # label samples W H extra-args...
    L=$1; S=$2; WW=$3; HH=$4; shift 4
    ./dr "$S" o.yuv --size $WW $HH --mode twice --dst y420 --dst-out d.bin --log "$@" > r.out 2> r.err; rc=$?
    nl=$(grep -c "UNSUPPORTED" r.err); bl=$(black_all d.bin $WW $HH); last=$(tail -1 r.out)
    if [ $rc -eq 0 ] && [ "$nl" = 1 ] && echo "$last" | grep -q "rc-errors 0 .*live after Close 0 " && ! grep -q predecompress-failed r.out && black_all d.bin $WW $HH > /dev/null
    then echo "PASS  e: $L: $bl, log lines 1: $(grep UNSUPPORTED r.err | sed 's/^.*UNSUPPORTED/UNSUPPORTED/') | $(echo "$last" | sed 's/.*\(rc-errors [0-9]*\).*\(live after Close [0-9]*\).*/\1, \2/')"
    else echo "FAIL  e: $L: rc $rc log-lines $nl $bl | $last"; FAIL=1; fi
}
X=$T/tools/decode_run/extract_samples.py
python3 $X "$M4/hi10_320x240.mov" hi10.samples > /dev/null
python3 $X "$M4/hi422_320x240.mov" hi422.samples > /dev/null
python3 $X "$M4/base_320x240.mov" base.samples > /dev/null
R2=$DATA/r2_320.mp4/data/movie.samples
refuse "High 10 with B-frames (10-bit)" hi10.samples 320 240
refuse "High 4:2:2 with B-frames" hi422.samples 320 240
refuse "library missing, r2_320 (B)" $R2 320 240 --no-lib
refuse "library 0x00080F01, r2_320 (B)" $R2 320 240 --fake-version 00080F01
refuse "library 0x00080F01, r2_320 with rdly 2" $R2 320 240 --fake-version 00080F01 --rdly 2
refuse "library 0x00070000 (too old), base_320" base.samples 320 240 --fake-version 00070000
# an old library still plays a stream without reordering
ffmpeg -v error -y -i "$M4/base_320x240.mov" -f rawvideo -pix_fmt yuv420p base.yuv
./dr base.samples o.yuv --size 320 240 --mode seq --fake-version 00080F01 > r.out 2>&1
if python3 $T/tools/host/check.py pics o.yuv base.yuv 320 240 320 240 > c.out && grep -q "live after Close 0 " r.out
then echo "PASS  e: library 0x00080F01 still plays Baseline without B: $(cat c.out)"; else echo "FAIL  e: old library, Baseline: $(cat c.out)"; FAIL=1; fi
# padding: harness + library under ASan, every sample in an exactly-sized block; still bit-exact
export ASAN_OPTIONS=detect_leaks=0
for m in r2_320.mp4 A_r2_320.mp4 opengop_320.mp4; do
  if python3 "$HERE/bcheck.py" $DATA/$m/data asan_$m --dr "$W/dr_asan" --rdly auto --scen linear,seek,keyseek --extra=--exact-buffers > asan_$m.txt 2>&1
  then echo "PASS  pad: ASan, exact buffers, $m: $(grep -c '^ok' asan_$m.txt) scenarios bit-exact, 0 ASan reports"
  else echo "FAIL  pad: $m $(grep -m2 'BAD\|ERROR' asan_$m.txt asan_$m/*.err | cut -c1-200)"; FAIL=1; fi
  rm -f asan_$m/d.bin asan_$m/o.yuv
done
./dr_asan base.samples o.yuv --size 320 240 --mode twice --exact-buffers > r.out 2> r.err; rc=$?
if [ $rc -eq 0 ] && ! grep -q "ERROR\|runtime error" r.err && python3 $T/tools/host/check.py pics o.yuv base.yuv 320 240 320 240 > c.out
then echo "PASS  pad: ASan, exact buffers, base_320 twice: $(cat c.out)"; else echo "FAIL  pad: base_320 rc $rc $(grep -m1 ERROR r.err)"; FAIL=1; fi
# stale pictures (v17b): every picture the component may no longer read is ASan-poisoned by the
# harness after each library call; decode errors injected (--fail-every), destination buffers
# swapped (--swap-dst: a held call into the other buffer must paint black or a still-valid picture)
for m in r2_320.mp4 opengop_320.mp4 A_r2_320.mp4 A_opengop_320.mp4 C_bf2pyr_320x240.mp4; do
  D=$DATA/$m/data; [ -d "$D" ] || { echo "NOTE  no data for $m"; continue; }
  python3 -c "
import json,random; i=json.load(open('$D/info.json')); r=random.Random(5); n=i['n']; out=[]
s=i['sync']
for q in range(15):
    t=r.randrange(n); k=max([x for x in s if x<=t] or [0]); out+=list(range(k,min(n,t+12)))
out+=[0]+list(range(n))+list(range(30))
open('stale.qt','w').write(''.join('%d 0\n'%x for x in out))"
  WH=$(python3 -c "import json;i=json.load(open('$D/info.json'));print(i['width'],i['height'])")
  RD=$(python3 -c "import json;i=json.load(open('$D/info.json'));print('--rdly %d' % (i['rdly'] if i['rdly'] is not None else i['Nmap']) if i['tails'] else '')")
  for a in "--fail-every 3" "--fail-every 7" "--swap-dst 3" "--swap-dst 2 --fail-every 5" "--fail-every 4 --swap-dst 5"; do
    ./dr_asan $D/movie.samples o.yuv --size $WH --mode qt:stale.qt --times $D/times.txt --dst y420 --dst-out d.bin --exact-buffers $RD $a > r.out 2> r.err; rc=$?
    if [ $rc -eq 0 ] && ! grep -q "ERROR\|runtime error" r.err && grep -q "rc-errors 0 .*live after Close 0 " r.out
    then echo "PASS  stale: $m [$RD $a]: 0 ASan reports | $(grep -o 'kept [0-9]*\|black-repaints [0-9]*\|decode-errors [0-9]*\|rdly-takes [0-9]*' r.out | tr '\n' ' ')"
    else echo "FAIL  stale: $m [$RD $a] rc $rc $(grep -m1 'ERROR\|runtime error' r.err)"; FAIL=1; fi
  done
done
# destination-changed detection (trace flag 512): a destination wiped before every call
D=$DATA/r2_320.mp4/data
./dr $D/movie.samples o.yuv --size 320 240 --mode twice --times $D/times.txt --dst 2vuy --dst-out d.bin --fresh-dst > r.out 2>&1
K=$(grep -o 'kept [0-9]*' r.out | awk '{print $2}'); C=$(grep -o 'dst-changed [0-9]*' r.out | awk '{print $2}')
if [ -n "$K" ] && [ "$K" -gt 0 ] && [ "$K" = "$C" ]; then echo "PASS  dstsig: --fresh-dst: kept $K, all $C seen as changed (flag 512)"; else echo "FAIL  dstsig: kept $K changed $C"; FAIL=1; fi
# v17c: QuickTime skips samples mid-GOP (no reset): drop 1, 2, 5 samples
for m in r2_320.mp4 C_r2_320.mp4 opengop_320.mp4; do
  [ -d $DATA/$m/data ] || { echo "NOTE  no data for $m"; continue; }
  python3 "$HERE/bcheck.py" $DATA/$m/data drop_$m --dr "$W/dr_asan" --scen drop --extra=--exact-buffers > drop_$m.txt 2>&1
  L=PASS; [ $m = opengop_320.mp4 ] && L=INFO
  grep -q "RESULT PASS" drop_$m.txt || { [ $L = PASS ] && { L=FAIL; FAIL=1; }; }
  grep "drop[0-9]" drop_$m.txt | sed "s/^\(...\) *\(drop[0-9]*\) *\(.*\) | v17.*\(nokey-jumps [0-9]*\).*\(stale-hidden [0-9]*\).*/$L  drop: $m \2 \3 | \4 \5/"
  rm -f drop_$m/d.bin
done
# v17c: reviewer 2 reproducers
RV=${RV:-/home/claude/s5/review2}
for d in d_base d_r2_320; do
  ./dr_asan $RV/$d/movie.samples o.yuv --size 320 240 --mode twice --swap-dst 1 --times $RV/$d/times.txt --dst y420 --dst-out d.bin --exact-buffers > r.out 2> r.err; rc=$?
  rp=$(grep -o ' repeats [0-9]*' r.out | awk '{print $2}'); bk=$(grep -o 'black-repaints [0-9]*' r.out | awk '{print $2}')
  exp=0; [ $d = d_r2_320 ] && exp=1   # r2: the held call 2 into a new buffer has no valid picture (decode ran): black by rule 3
  if [ $rc -eq 0 ] && ! grep -q ERROR r.err && [ "$rp" -ge 1 ] && [ "$bk" = $exp ]; then echo "PASS  review#1 $d twice --swap-dst 1: repeats $rp black-repaints $bk, 0 ASan"; else echo "FAIL  review#1 $d: rc $rc repeats $rp black $bk"; FAIL=1; fi
done
mkdir -p misc; cp $T/codec-src/* misc/
python3 - <<'PY'
f='misc/H264Codec.c'; s=open(f).read(); old='    return (unsigned long)__builtin_frame_address(0);'
assert s.count(old)==1
s=s.replace(old,'    { static int k=0; const char*e=getenv("MIS"); k++; if(e&&atoi(e)==k) return 8; }\n    return (unsigned long)__builtin_frame_address(0) & ~15UL;'); open(f,'w').write(s)
PY
gcc -std=gnu89 -O1 -fno-strict-aliasing -w -DH264BSD_EXTERNAL_ALLOC -DH264_BACKEND_AVCDEC=1 -I$T/tools/host -I$T/decoder-src -Imisc -o drm $T/tools/host/decode_run.c $T/decoder-src/h264bsd_*.c misc/avcC_parse.c "$LIB" -lm
D=$RV/d_C_r2_320; ffmpeg -v error -y -f h264 -i $D/movie.264 -f rawvideo -pix_fmt yuv420p mref.yuv
for mis in 0 3 2 4; do
  MIS=$mis H264_HOST_ALTIVEC=1 ./drm $D/movie.samples o$mis.yuv --size 320 240 --mode twice --rdly 2 --times $D/times.txt --log > r.out 2> r.err
  c=$(python3 $T/tools/host/check.py pics o$mis.yuv mref.yuv 320 240 320 240); ro=$(grep -c "AltiVec OFF" r.err)
  if [ $mis = 4 ]; then echo "INFO  review#2 MIS=4: reopen on a NON-key sample (sample 1, mid-GOP): garbage until the next key, as in 16t (pre-existing): $c"
  elif echo "$c" | grep -q "UNEXPECTED" ; then echo "FAIL  review#2 MIS=$mis reopen $ro: $c"; FAIL=1; else echo "PASS  review#2 MIS=$mis (reopen lines $ro): $c"; fi
done
# f: damage
for m in r2_320.mp4 opengop_320.mp4 A_r2_320.mp4 A_opengop_320.mp4; do
  D=$DATA/$m/data; [ -d "$D" ] || { echo "NOTE  no data for $m"; continue; }
  for seed in 1 2 3; do for pm in 20 400; do for mode in seq qt; do
    MO="--mode seq"; [ $mode = qt ] && { python3 -c "
import json,random; i=json.load(open('$D/info.json')); r=random.Random($seed); n=i['n']; out=[]
s=[k for k in i['sync']]
for q in range(15):
    t=r.randrange(n); k=max([x for x in s if x<=t] or [0]); out+=list(range(k,min(n,t+12)))
out+=list(range(n))+list(range(30))
open('dmg.qt','w').write(''.join('%d 0\n'%x for x in out))"; MO="--mode qt:dmg.qt"; }
    ASAN_OPTIONS=detect_leaks=0 ./dr_asan $D/movie.samples o.yuv --size $(python3 -c "import json;i=json.load(open('$D/info.json'));print(i['width'],i['height'])") \
        $MO --times $D/times.txt --corrupt $seed $pm --dst rgb32 --dst-out d.bin --exact-buffers > r.out 2> r.err; rc=$?
    if [ $rc -eq 0 ] && ! grep -q "ERROR\|runtime error" r.err && grep -q "rc-errors 0 .*live after Close 0 " r.out
    then echo "PASS  f: $m seed $seed $pm/1000 $mode: $(tail -1 r.out | sed 's/.*\(calls [0-9]*\).*\(decode-errors [0-9]*\).*\(live after Close [0-9]*\).*/\1 \2 \3/') | $(grep v17 r.out | cut -c1-60)"
    else echo "FAIL  f: $m seed $seed $pm $mode rc $rc $(grep -m2 'ERROR\|runtime error' r.err)"; FAIL=1; fi
  done; done; done
done
# discontinuity trace (kH264TimingTrace): raw values per call
D=$DATA/r2_320.mp4/data
python3 -c "open('tr.qt','w').write(''.join('%d 0\n'%x for x in [0,0,1,2,3,4,5,6,7,8,9,10,60,61,62,63,64,65]+list(range(290,300))+[0,1,2]))"
./dr_tr $D/movie.samples o.yuv --size 320 240 --mode qt:tr.qt --times $D/times.txt --log > r.out 2> trace.txt
tr "\r" "\n" < trace.txt > trace_lf.txt; mv trace_lf.txt trace.txt
echo "NOTE  trace: $(grep -c '^T ' trace.txt) T lines in $W/trace.txt; fields 13-18 = syncHi syncLo syncScale syncDur size avFlags"
grep "=== timing trace\|^T " trace.txt | awk '{print $1,$2,$3,$(NF-5),$(NF-4),$(NF-3),$(NF-2),$(NF-1),$NF}' | sed -n '1,4p;13,16p;29,32p'
[ $FAIL -eq 0 ] && echo "ALL PASSED" || echo "SOMETHING FAILED"
