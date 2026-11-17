#!/bin/sh
# run_suite.sh DIR [WORK] -- stage 5 (v17) proofs a (linear play) and c (seek, key-frame seek,
# loop, re-sent poster frame) on every .mp4/.mov in DIR, x86 host, AVCDecLib backend.
# Prints one "PASS|FAIL file" line per movie; details in WORK/<file>/result.txt.
# Env: AVCLIB_X (AVCDecLib x86 relocatable, default comp/avcx/lib_x.o)
#      RDLY  atom (default: the movie's own 'rdly' atom, else none) | auto (the file's
#            decode-to-display distance, as our rewriter would write) | none | N
#      FMTS  destination formats for linear play (default y420; also 2vuy,rgb32,rgb16)
#      DR    use this harness command instead of building one (e.g. "qemu-ppc -cpu 750 ./dr_ppc")
#      SCEN  scenarios (default linear,twice,loop,seek,keyseek,posterseek)
#      TIMES media,movie (default both): syncFrameTime in the media time scale, and/or as QuickTime 6
#            passes it (movie time scale 1000, negative first start as hi=0/lo<0)
#      REFDIR directory with reference NAME.md5 (ffmpeg framemd5) to cross-check, optional
set -u
DIR=$(cd "$1" && pwd); WORK=${2:-./suite_work}; mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
LIB=${AVCLIB_X:-$HERE/../../../../avcx/lib_x.o}
DRC=${DR:-}
if [ -z "$DRC" ]; then
    sh "$HERE/../build.sh" "$WORK/dr" gcc -DH264_BACKEND_AVCDEC=1 "$LIB" || { echo "FAIL build"; exit 1; }
    DRC="$WORK/dr"
fi
FAILS=0
for f in "$DIR"/*.mp4 "$DIR"/*.mov; do
    [ -f "$f" ] || continue
    b=$(basename "$f"); D="$WORK/$b"; mkdir -p "$D"
    o=${b#A_}; o=${o#B_}; o=${o#C_}; REF=""
    [ -n "${REFDIR:-}" ] && [ -f "$REFDIR/$o.md5" ] && REF="$REFDIR/$o.md5"
    if ! python3 "$HERE/bprep.py" "$f" "$D/data" $REF > "$D/prep.txt" 2>&1; then
        echo "FAIL $b (prep: $(tail -1 "$D/prep.txt"))"; FAILS=$((FAILS+1)); continue
    fi
    : > "$D/result.txt"; RC=0
    for TB in $(echo "${TIMES:-media,movie}" | tr ',' ' '); do
        RUN="$D/run"; [ $TB = movie ] && RUN="$D/run_movie"
        echo "== timebase $TB" >> "$D/result.txt"
        python3 "$HERE/bcheck.py" "$D/data" "$RUN" --dr "$DRC" --rdly "${RDLY:-atom}" --fmts "${FMTS:-y420}" --timebase $TB \
            --scen "${SCEN:-linear,twice,loop,seek,keyseek,posterseek}" >> "$D/result.txt" 2>&1 || RC=1
        rm -f "$RUN/d.bin" "$RUN/o.yuv" "$RUN/ref.yuv"
    done
    if [ $RC -eq 0 ]
    then echo "PASS $b | $(head -1 "$D/prep.txt" | sed 's/^[^:]*: //') | $(grep -c '^ok' "$D/result.txt") scenario runs ok (timebases ${TIMES:-media,movie})"
    else echo "FAIL $b | $(grep '^BAD\|Traceback\|Error' "$D/result.txt" | head -3 | cut -c1-160 | tr '\n' ' ')"; FAILS=$((FAILS+1)); fi
done
[ $FAILS -eq 0 ] && echo "SUITE PASSED" || echo "SUITE: $FAILS FAILED"
exit $FAILS
